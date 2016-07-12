/*
 * ntpq - query an NTP server using mode 6 commands
 */
#include <config.h>
#include <stdio.h>
#include <ctype.h>
#include <signal.h>
#include <setjmp.h>
#include <sys/types.h>
#include <sys/time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/select.h>
#ifdef SYS_WINNT
# include <mswsock.h>
#endif
#include <isc/net.h>
#include <isc/result.h>

#include "ntpq.h"
#include "ntp_assert.h"
#include "ntp_stdlib.h"
#include "ntp_unixtime.h"
#include "ntp_calendar.h"
#include "ntp_assert.h"
#include "lib_strbuf.h"
#include "ntp_lineedit.h"
#include "ntp_debug.h"
#ifdef HAVE_OPENSSL
#include "openssl/evp.h"
#include "openssl/objects.h"
#include "openssl/err.h"
#endif
#include <ssl_applink.c>

/*
 * Because we potentially understand a lot of commands we will run
 * interactive if connected to a terminal.
 */
int interactive = 0;		/* set to 1 when we should prompt */
const char *prompt = "ntpq> ";	/* prompt to ask him about */

/*
 * use old readvars behavior?  --old-rv processing in ntpq resets
 * this value based on the presence or absence of --old-rv.  It is
 * initialized to 1 here to maintain backward compatibility with
 * libntpq clients such as ntpsnmpd, which are free to reset it as
 * desired.
 */
bool	old_rv = true;


/*
 * for get_systime()
 */
int8_t	sys_precision;		/* local clock precision (log2 s) */

/*
 * Keyid used for authenticated requests.  Obtained on the fly.
 */
u_long info_auth_keyid = 0;

static	int	info_auth_keytype = NID_md5;	/* MD5 */
static	size_t	info_auth_hashlen = 16;		/* MD5 */
u_long	current_time;		/* needed by authkeys; not used */

/*
 * Flag which indicates we should always send authenticated requests
 */
bool always_auth = false;

/*
 * Flag which indicates raw mode output.
 */
bool rawmode = false;

/*
 * Packet version number we use
 */
uint8_t pktversion = NTP_OLDVERSION + 1;

/*
 * Don't jump if no set jmp.
 */
volatile int jump = 0;

/*
 * Format values
 */
#define	PADDING	0
#define	HA	1	/* host address */
#define	NA	2	/* network address */
#define	LP	3	/* leap (print in binary) */
#define	RF	4	/* refid (sometimes string, sometimes not) */
#define	AR	5	/* array of times */
#define FX	6	/* test flags */
#define TS	7	/* l_fp timestamp in hex */
#define	OC	8	/* integer, print in octal */
#define	EOV	255	/* end of table */

/*
 * For the most part ntpq simply displays what ntpd provides in the
 * mostly plain-text mode 6 responses.  A few variable names are by
 * default "cooked" to provide more human-friendly output.
 */
const var_format cookedvars[] = {
	{ "leap",		LP },
	{ "reach",		OC },
	{ "refid",		RF },
	{ "reftime",		TS },
	{ "clock",		TS },
	{ "org",		TS },
	{ "rec",		TS },
	{ "xmt",		TS },
	{ "flash",		FX },
	{ "srcadr",		HA },
	{ "peeradr",		HA },	/* compat with others */
	{ "dstadr",		NA },
	{ "filtdelay",		AR },
	{ "filtoffset",		AR },
	{ "filtdisp",		AR },
	{ "filterror",		AR },	/* compat with others */
};



/*
 * flasher bits
 */
static const char *tstflagnames[] = {
	"pkt_dup",		/* BOGON1 */
	"pkt_bogus",		/* BOGON2 */
	"pkt_unsync",		/* BOGON3 */
	"pkt_denied",		/* BOGON4 */
	"pkt_auth",		/* BOGON5 */
	"pkt_stratum",		/* BOGON6 */
	"pkt_header",		/* BOGON7 */
	"pkt_autokey",		/* BOGON8 */
	"pkt_crypto",		/* BOGON9 */
	"peer_stratum",		/* BOGON10 */
	"peer_dist",		/* BOGON11 */
	"peer_loop",		/* BOGON12 */
	"peer_unreach"		/* BOGON13 */
};


int		ntpqmain	(int,	char **);
/*
 * Built in command handler declarations
 */
static	bool	openhost	(const char *, int);
static	void	dump_hex_printable(const void *, size_t);
static	int	sendpkt		(void *, size_t);
static	int	getresponse	(int, int, u_short *, int *, const char **, int);
static	int	sendrequest	(int, associd_t, int, int, const char *);
static	char *	tstflags	(u_long);
#ifndef BUILD_AS_LIB
static	void	getcmds		(void);
#ifndef SYS_WINNT
static	void abortcmd	(int);
#endif	/* SYS_WINNT */
static	void	docmd		(const char *);
static	void	tokenize	(const char *, char **, int *);
static	bool	getarg		(const char *, int, arg_v *);
#endif	/* BUILD_AS_LIB */
static	int	findcmd		(const char *, struct xcmd *,
				 struct xcmd *, struct xcmd **);
static	bool	decodearr	(char *, int *, l_fp *);
static	void	help		(struct parse *, FILE *);
static	int	helpsort	(const void *, const void *);
static	void	printusage	(struct xcmd *, FILE *);
static	void	timeout		(struct parse *, FILE *);
static	void	auth_delay	(struct parse *, FILE *);
static	void	host		(struct parse *, FILE *);
static	void	ntp_poll	(struct parse *, FILE *);
static	void	keyid		(struct parse *, FILE *);
static	void	keytype		(struct parse *, FILE *);
static	void	passwd		(struct parse *, FILE *);
static	void	hostnames	(struct parse *, FILE *);
static	void	setdebug	(struct parse *, FILE *);
static	void	quit		(struct parse *, FILE *);
static	void	version		(struct parse *, FILE *);
static	void	raw		(struct parse *, FILE *);
static	void	cooked		(struct parse *, FILE *);
static	void	authenticate	(struct parse *, FILE *);
static	void	ntpversion	(struct parse *, FILE *);
static	void	warning		(const char *, ...)
    __attribute__((__format__(__printf__, 1, 2)));
static	void	error		(const char *, ...)
    __attribute__((__format__(__printf__, 1, 2)));
static	u_long	getkeyid	(const char *);
static	void	atoascii	(const char *, size_t, char *, size_t);
static	void	cookedprint	(int, int, const char *, int, int, FILE *);
static	void	rawprint	(int, int, const char *, int, int, FILE *);
static	void	startoutput	(void);
static	void	output		(FILE *, const char *, const char *);
static	void	endoutput	(FILE *);
static	void	outputarr	(FILE *, char *, int, l_fp *);
static	int	assoccmp	(const void *, const void *);
	u_short	varfmt		(const char *);

#ifdef HAVE_OPENSSL
# ifdef HAVE_EVP_MD_DO_ALL_SORTED
static void list_md_fn(const EVP_MD *m, const char *from,
		       const char *to, void *arg );
# endif
#endif
static char *list_digest_names(void);

/*
 * Built-in commands we understand
 */
struct xcmd builtins[] = {
	{ "?",		help,		{  OPT|NTP_STR, NO, NO, NO },
	  { "command", "", "", "" },
	  "tell the use and syntax of commands" },
	{ "help",	help,		{  OPT|NTP_STR, NO, NO, NO },
	  { "command", "", "", "" },
	  "tell the use and syntax of commands" },
	{ "timeout",	timeout,	{ OPT|NTP_UINT, NO, NO, NO },
	  { "msec", "", "", "" },
	  "set the primary receive time out" },
	{ "delay",	auth_delay,	{ OPT|NTP_INT, NO, NO, NO },
	  { "msec", "", "", "" },
	  "set the delay added to encryption time stamps" },
	{ "host",	host,		{ OPT|NTP_STR, OPT|NTP_STR, NO, NO },
	  { "-4|-6", "hostname", "", "" },
	  "specify the host whose NTP server we talk to" },
	{ "poll",	ntp_poll,	{ OPT|NTP_UINT, OPT|NTP_STR, NO, NO },
	  { "n", "verbose", "", "" },
	  "poll an NTP server in client mode `n' times" },
	{ "passwd",	passwd,		{ OPT|NTP_STR, NO, NO, NO },
	  { "", "", "", "" },
	  "specify a password to use for authenticated requests"},
	{ "hostnames",	hostnames,	{ OPT|NTP_STR, NO, NO, NO },
	  { "yes|no", "", "", "" },
	  "specify whether hostnames or net numbers are printed"},
	{ "debug",	setdebug,	{ OPT|NTP_STR, NO, NO, NO },
	  { "no|more|less", "", "", "" },
	  "set/change debugging level" },
	{ "quit",	quit,		{ NO, NO, NO, NO },
	  { "", "", "", "" },
	  "exit ntpq" },
	{ "exit",	quit,		{ NO, NO, NO, NO },
	  { "", "", "", "" },
	  "exit ntpq" },
	{ "keyid",	keyid,		{ OPT|NTP_UINT, NO, NO, NO },
	  { "key#", "", "", "" },
	  "set keyid to use for authenticated requests" },
	{ "version",	version,	{ NO, NO, NO, NO },
	  { "", "", "", "" },
	  "print version number" },
	{ "raw",	raw,		{ NO, NO, NO, NO },
	  { "", "", "", "" },
	  "do raw mode variable output" },
	{ "cooked",	cooked,		{ NO, NO, NO, NO },
	  { "", "", "", "" },
	  "do cooked mode variable output" },
	{ "authenticate", authenticate,	{ OPT|NTP_STR, NO, NO, NO },
	  { "yes|no", "", "", "" },
	  "always authenticate requests to this server" },
	{ "ntpversion",	ntpversion,	{ OPT|NTP_UINT, NO, NO, NO },
	  { "version number", "", "", "" },
	  "set the NTP version number to use for requests" },
	{ "keytype",	keytype,	{ OPT|NTP_STR, NO, NO, NO },
	  { "key type %s", "", "", "" },
	  NULL },
	{ 0,		0,		{ NO, NO, NO, NO },
	  { "", "", "", "" }, "" }
};


/*
 * Default values we use.
 */
#define	DEFHOST		"localhost"	/* default host name */
#define	DEFTIMEOUT	5		/* wait 5 seconds for 1st pkt */
#define	DEFSTIMEOUT	3		/* and 3 more for each additional */
/*
 * Requests are automatically retried once, so total timeout with no
 * response is a bit over 2 * DEFTIMEOUT, or 10 seconds.  At the other
 * extreme, a request eliciting 32 packets of responses each for some
 * reason nearly DEFSTIMEOUT seconds after the prior in that series,
 * with a single packet dropped, would take around 32 * DEFSTIMEOUT, or
 * 93 seconds to fail each of two times, or 186 seconds.
 * Some commands involve a series of requests, such as "peers" and
 * "mrulist", so the cumulative timeouts are even longer for those.
 */
#define	DEFDELAY	0x51EB852	/* 20 milliseconds, l_fp fraction */
#define	MAXCMDS		100		/* maximum commands on cmd line */
#define	MAXHOSTS	200		/* maximum hosts on cmd line */
#define	MAXLINE		512		/* maximum line length */
#define	MAXTOKENS	(1+MAXARGS+2)	/* maximum number of usable tokens */
#define	MAXVARLEN	256		/* maximum length of a variable name */
#define	MAXVALLEN	2048		/* maximum length of a variable value */
#define	MAXOUTLINE	72		/* maximum length of an output line */
#define SCREENWIDTH	76		/* nominal screen width in columns */

/*
 * Some variables used and manipulated locally
 */
struct sock_timeval tvout = { DEFTIMEOUT, 0 };	/* time out for reads */
struct sock_timeval tvsout = { DEFSTIMEOUT, 0 };/* secondary time out */
l_fp delay_time;				/* delay time */
char currenthost[NI_MAXHOST];			/* current host name */
bool currenthostisnum;				/* is prior text from IP? */
struct sockaddr_in hostaddr;			/* host address */
bool showhostnames = true;			/* show host names by default */
bool wideremote = false;			/* show wide remote names? */

int ai_fam_templ;				/* address family */
int ai_fam_default;				/* default address family */
SOCKET sockfd;					/* fd socket is opened on */
bool havehost = false;				/* set to true when host open */
int s_port = 0;
struct servent *server_entry = NULL;		/* server entry for ntp */


/*
 * Sequence number used for requests.  It is incremented before
 * it is used.
 */
u_short sequence;

/*
 * Holds data returned from queries.  Declare buffer long to be sure of
 * alignment.
 */
#define	DATASIZE	(MAXFRAGS*480)	/* maximum amount of data */
long pktdata[DATASIZE/sizeof(long)];

/*
 * assoc_cache[] is a dynamic array which allows references to
 * associations using &1 ... &N for n associations, avoiding manual
 * lookup of the current association IDs for a given ntpd.  It also
 * caches the status word for each association, retrieved incidentally.
 */
struct association *	assoc_cache;
u_int assoc_cache_slots;/* count of allocated array entries */
u_int numassoc;		/* number of cached associations */

/*
 * For commands typed on the command line (with the -c option)
 */
int numcmds = 0;
const char *ccmds[MAXCMDS];
#define	ADDCMD(cp)	if (numcmds < MAXCMDS) ccmds[numcmds++] = (cp)

/*
 * When multiple hosts are specified.
 */

u_int numhosts;

chost chosts[MAXHOSTS];
#define	ADDHOST(cp)						\
	do {							\
		if (numhosts < MAXHOSTS) {			\
			chosts[numhosts].name = (cp);		\
			chosts[numhosts].fam = ai_fam_templ;	\
			numhosts++;				\
		}						\
	} while (0)

/*
 * Macro definitions we use
 */
#define	ISSPACE(c)	((c) == ' ' || (c) == '\t')
#define	ISEOL(c)	((c) == '\n' || (c) == '\r' || (c) == '\0')
#define	STREQ(a, b)	(*(a) == *(b) && strcmp((a), (b)) == 0)

/*
 * Jump buffer for longjumping back to the command level
 */
jmp_buf interrupt_buf;

/*
 * Points at file being currently printed into
 */
FILE *current_output;

/*
 * Command table
 */
extern struct xcmd opcmds[];

char *progname;

#ifdef NO_MAIN_ALLOWED
#ifndef BUILD_AS_LIB
CALL(ntpq,"ntpq",ntpqmain);

void clear_globals(void)
{
	extern int ntp_optind;
	showhostnames = 0;	/* don'tshow host names by default */
	ntp_optind = 0;
	server_entry = NULL;	/* server entry for ntp */
	havehost = 0;		/* set to 1 when host open */
	numassoc = 0;		/* number of cached associations */
	numcmds = 0;
	numhosts = 0;
}
#endif /* !BUILD_AS_LIB */
#endif /* NO_MAIN_ALLOWED */

#define ALL_OPTIONS "46c:dhD:inOpVw"
static const struct option longoptions[] = {
    { "ipv4",		    0, 0, '4' },
    { "ipv6",		    0, 0, '6' },
    { "command",	    1, 0, 'c' },
    { "debug",		    0, 0, 'd' },
    { "set-debug-level",    1, 0, 'D' },
    { "help",               0, 0, 'h' },
    { "interactive",        0, 0, 'i' },
    { "numeric",            0, 0, 'n' },
    { "old-rv",             0, 0, 'O' },
    { "peers",              0, 0, 'p' },
    { "version",	    0, 0, 'V' },
    { "wide",		    0, 0, 'w' },
    { NULL,                 0, 0, '\0'},
};

static bool opt_ipv4 = false, opt_ipv6 = false;
static char *opt_command = NULL;
static bool opt_interactive = false;
static bool opt_numeric = false;
static bool opt_old_rv = false;
static bool opt_peers = false;
static bool opt_wide = false;

/*
 * main - parse arguments and handle options
 */
#ifndef NO_MAIN_ALLOWED
int
main(
	int argc,
	char *argv[]
	)
{
	return ntpqmain(argc, argv);
}
#endif

#ifndef BUILD_AS_LIB
static void ntpq_usage(void)
{
#define P(x)	fputs(x, stderr)
    P("USAGE: ntpq [-46dphinOV] [-c str] [-D lvl] [ host ...]\n");
    P("  Flg Arg Option-Name    Description\n");
    P("   -4 no  ipv4           Force IPv4 DNS name resolution\n");
    P("				- prohibits the option 'ipv6'\n");
    P("   -6 no  ipv6           Force IPv6 DNS name resolution\n");
    P("				- prohibits the option 'ipv4'\n");
    P("   -c Str command        run a command and exit\n");
    P("				- may appear multiple times\n");
    P("   -d no  debug-level    Increase output debug message level\n");
    P("				- may appear multiple times\n");
    P("   -D Str set-debug-level Set the output debug message level\n");
    P("				- may appear multiple times\n");
    P("   -h no  help           Print a usage message.\n");
    P("   -p no  peers          Print a list of the peers\n");
    P("				- prohibits the option 'interactive'\n");
    P("   -i no  interactive    Force ntpq to operate in interactive mode\n");
    P("				- prohibits these options:\n");
    P("				command\n");
    P("				peers\n");
    P("   -n no  numeric        numeric host addresses\n");
    P("   -O no  old-rv         Always output status line with readvar\n");
    P("   -V opt version        Output version information and exit\n");
    P("   -w no  wide           enable wide display of addresses\n");
#undef P
}


int
ntpqmain(
	int argc,
	char *argv[]
	)
{
	u_int ihost;
	size_t icmd;
	int msglen;

	delay_time.l_ui = 0;
	delay_time.l_uf = DEFDELAY;

	init_lib();	/* sets up ipv4_works, ipv6_works */
	ssl_applink();
	init_auth();

	/* Check to see if we have IPv6. Otherwise default to IPv4 */
	if (!ipv6_works)
		ai_fam_default = AF_INET;

	/* Fixup keytype's help based on available digest names */

	{
	    char *list;
	    char *msg, *fmt;

	    list = list_digest_names();
	    for (icmd = 0; icmd < sizeof(builtins)/sizeof(builtins[0]); icmd++) {
		if (strcmp("keytype", builtins[icmd].keyword) == 0)
		    break;
	    }

	    /* CID: 1295478 */
	    /* This should only "trip" if "keytype" is removed from builtins */
	    INSIST(icmd < sizeof(builtins)/sizeof(builtins[0]));

#ifdef HAVE_OPENSSL
	    builtins[icmd].desc[0] = "digest-name";
	    fmt = "set key type to use for authenticated requests, one of:%s";
#else
	    builtins[icmd].desc[0] = "md5";
	    fmt = "set key type to use for authenticated requests (%s)";
#endif
	    msglen = strlen(fmt) + strlen(list) - strlen("%s") + 1;
	    msg = malloc(msglen);
	    snprintf(msg, msglen, fmt, list);
	    builtins[icmd].comment = msg;
	    free(list);
	}

	progname = argv[0];

	{
		int op;

		while ((op = ntp_getopt_long(argc, argv,
					     ALL_OPTIONS, longoptions, NULL)) != -1) {

		    switch (op) {
		    case '4':
			opt_ipv4 = true;
			break;
		    case '6':
			opt_ipv6 = true;
			break;
		    case 'c':
			opt_command = ntp_optarg;
			ADDCMD(opt_command);
			break;
		    case 'd':
#ifdef DEBUG
			++debug;
#endif
			break;
		    case 'D':
#ifdef DEBUG
			debug = atoi(ntp_optarg);
#endif
			break;
		    case 'h':
			ntpq_usage();
			exit(0);
			break;
		    case 'i':
			opt_interactive = true;
			break;
		    case 'n':
			opt_numeric = true;
			break;
		    case 'O':
			opt_old_rv = true;
			break;
		    case 'p':
			opt_peers = true;
			ADDCMD("peers");
			break;
		    case 'V':
			printf("ntpq %s\n", Version);
			exit(0);
		    case 'w':
			opt_wide = true;
			break;
		    default:
			/* chars not in table get converted to ? */
			fputs("Unknown command line switch or missing argument.\n", stderr);
			ntpq_usage();
			exit(1);
		    } /*switch*/
		}

		argc -= ntp_optind;
		argv += ntp_optind;
	}

	if (opt_interactive && (opt_command || opt_peers)) {
	    fprintf(stderr, "%s: invalid option combination.\n", progname);
	    exit(EXIT_FAILURE);
	}

	if (opt_ipv4)
		ai_fam_templ = AF_INET;
	else if (opt_ipv6)
		ai_fam_templ = AF_INET6;
	else
		ai_fam_templ = ai_fam_default;

	if (opt_interactive)
		interactive = true;

	if (opt_numeric)
		showhostnames = false;

	if (opt_wide)
		wideremote = true;

	old_rv = opt_old_rv;

	if (0 == argc) {
		ADDHOST(DEFHOST);
	} else {
		for (ihost = 0; ihost < (u_int)argc; ihost++) {
			if ('-' == *argv[ihost]) {
				//
				// If I really cared I'd also check:
				// 0 == argv[ihost][2]
				//
				// and there are other cases as well...
				//
				if ('4' == argv[ihost][1]) {
					ai_fam_templ = AF_INET;
					continue;
				} else if ('6' == argv[ihost][1]) {
					ai_fam_templ = AF_INET6;
					continue;
				} else {
					// XXX Throw a usage error
				}
			}
			ADDHOST(argv[ihost]);
		}
	}

	if (numcmds == 0 && !interactive
	    && isatty(fileno(stdin)) && isatty(fileno(stderr))) {
		interactive = true;
	}

#ifndef SYS_WINNT /* Under NT cannot handle SIGINT, WIN32 spawns a handler */
	if (interactive)
	    (void) signal_no_reset(SIGINT, abortcmd);
#endif /* SYS_WINNT */

	if (numcmds == 0) {
		(void) openhost(chosts[0].name, chosts[0].fam);
		getcmds();
	} else {
		for (ihost = 0; ihost < numhosts; ihost++) {
			int i;
			if (openhost(chosts[ihost].name, chosts[ihost].fam))
				for (i = 0; i < numcmds; i++)
					docmd(ccmds[i]);
		}
	}
#ifdef SYS_WINNT
	WSACleanup();
#endif /* SYS_WINNT */
	return 0;
}
#endif /* !BUILD_AS_LIB */

/*
 * openhost - open a socket to a host
 */
static bool
openhost(
	const char *hname,
	int	    fam
	)
{
	const char svc[] = "ntp";
	char temphost[NI_MAXHOST];
	int a_info, i;
	struct addrinfo hints, *ai;
	sockaddr_u addr;
	size_t octets;
	register const char *cp;
	char name[NI_MAXHOST];

	/*
	 * We need to get by the [] if they were entered
	 */

	cp = hname;

	if (*cp == '[') {
		cp++;
		for (i = 0; *cp && *cp != ']'; cp++, i++)
			name[i] = *cp;
		if (*cp == ']') {
			name[i] = '\0';
			hname = name;
		} else {
			return false;
		}
	}

	/*
	 * First try to resolve it as an ip address and if that fails,
	 * do a fullblown (dns) lookup. That way we only use the dns
	 * when it is needed and work around some implementations that
	 * will return an "IPv4-mapped IPv6 address" address if you
	 * give it an IPv4 address to lookup.
	 */
	ZERO(hints);
	hints.ai_family = fam;
	hints.ai_protocol = IPPROTO_UDP;
	hints.ai_socktype = SOCK_DGRAM;
	hints.ai_flags = Z_AI_NUMERICHOST;
	ai = NULL;

	a_info = getaddrinfo(hname, svc, &hints, &ai);
	if (a_info == EAI_NONAME
#ifdef EAI_NODATA
	    || a_info == EAI_NODATA
#endif
	   ) {
		hints.ai_flags = AI_CANONNAME;
#ifdef AI_ADDRCONFIG
		hints.ai_flags |= AI_ADDRCONFIG;
#endif
		a_info = getaddrinfo(hname, svc, &hints, &ai);
	}
#ifdef AI_ADDRCONFIG
	/* Some older implementations don't like AI_ADDRCONFIG. */
	if (a_info == EAI_BADFLAGS) {
		hints.ai_flags &= ~AI_ADDRCONFIG;
		a_info = getaddrinfo(hname, svc, &hints, &ai);
	}
#endif
	if (a_info != 0) {
		fprintf(stderr, "%s\n", gai_strerror(a_info));
		return false;
	}

	INSIST(ai != NULL);
	ZERO(addr);
	octets = min(sizeof(addr), ai->ai_addrlen);
	memcpy(&addr, ai->ai_addr, octets);

	if (ai->ai_canonname == NULL) {
		strlcpy(temphost, stoa(&addr), sizeof(temphost));
		currenthostisnum = true;
	} else {
		strlcpy(temphost, ai->ai_canonname, sizeof(temphost));
		currenthostisnum = false;
	}

	if (debug > 2)
		printf("Opening host %s (%s)\n",
			temphost,
			(ai->ai_family == AF_INET)
			? "AF_INET"
			: (ai->ai_family == AF_INET6)
			  ? "AF_INET6"
			  : "AF-???"
			);

	if (havehost == 1) {
		if (debug > 2)
			printf("Closing old host %s\n", currenthost);
		closesocket(sockfd);
		havehost = false;
	}
	strlcpy(currenthost, temphost, sizeof(currenthost));

	/* port maps to the same location in both families */
	s_port = NSRCPORT(&addr);
#ifdef SYS_WINNT
	{
		int optionValue = SO_SYNCHRONOUS_NONALERT;
		int err;

		err = setsockopt(INVALID_SOCKET, SOL_SOCKET, SO_OPENTYPE,
				 (char *)&optionValue, sizeof(optionValue));
		if (err) {
			mfprintf(stderr,
				 "setsockopt(SO_SYNCHRONOUS_NONALERT)"
				 " error: %m\n");
			freeaddrinfo(ai);
			exit(1);
		}
	}
#endif /* SYS_WINNT */

	sockfd = socket(ai->ai_family, ai->ai_socktype,
			ai->ai_protocol);
	if (sockfd == INVALID_SOCKET) {
		error("socket");
		freeaddrinfo(ai);
		return false;
	}

	if (connect(sockfd, (struct sockaddr *)ai->ai_addr, ai->ai_addrlen)==-1)
	{
		error("connect");
		freeaddrinfo(ai);
		return false;
	}
	freeaddrinfo(ai);
	havehost = true;
	numassoc = 0;

	return true;
}


static void
dump_hex_printable(
	const void *	data,
	size_t		len
	)
{
	const char *	cdata;
	const char *	rowstart;
	size_t		idx;
	size_t		rowlen;
	uint8_t		uch;

	cdata = data;
	while (len > 0) {
		rowstart = cdata;
		rowlen = min(16, len);
		for (idx = 0; idx < rowlen; idx++) {
			uch = *(cdata++);
			printf("%02x ", uch);
		}
		for ( ; idx < 16 ; idx++)
			printf("   ");
		cdata = rowstart;
		for (idx = 0; idx < rowlen; idx++) {
			uch = *(cdata++);
			printf("%c", (isprint(uch))
					 ? uch
					 : '.');
		}
		printf("\n");
		len -= rowlen;
	}
}


/* XXX ELIMINATE sendpkt similar in ntpq.c, ntp_io.c, ntptrace.c */
/*
 * sendpkt - send a packet to the remote host
 */
static int
sendpkt(
	void *	xdata,
	size_t	xdatalen
	)
{
	if (debug >= 3)
		printf("Sending %zu octets\n", xdatalen);

	if (send(sockfd, xdata, (size_t)xdatalen, 0) == -1) {
		warning("write to %s failed", currenthost);
		return -1;
	}

	if (debug >= 4) {
		printf("Request packet:\n");
		dump_hex_printable(xdata, xdatalen);
	}
	return 0;
}

/*
 * getresponse - get a (series of) response packet(s) and return the data
 */
static int
getresponse(
	int opcode,
	int associd,
	u_short *rstatus,
	int *rsize,
	const char **rdata,
	int timeo
	)
{
	struct ntp_control rpkt;
	struct sock_timeval tvo;
	u_short offsets[MAXFRAGS+1];
	u_short counts[MAXFRAGS+1];
	u_short offset;
	u_short count;
	size_t numfrags;
	size_t f;
	size_t ff;
	bool seenlastfrag;
	int shouldbesize;
	fd_set fds;
	int n;
	int errcode;
	int bail = 0;

	/*
	 * This is pretty tricky.  We may get between 1 and MAXFRAG packets
	 * back in response to the request.  We peel the data out of
	 * each packet and collect it in one long block.  When the last
	 * packet in the sequence is received we'll know how much data we
	 * should have had.  Note we use one long time out, should reconsider.
	 */
	*rsize = 0;
	if (rstatus)
		*rstatus = 0;
	*rdata = (char *)pktdata;

	numfrags = 0;
	seenlastfrag = false;

	FD_ZERO(&fds);

	/*
	 * Loop until we have an error or a complete response.  Nearly all
	 * code paths to loop again use continue.
	 */
	for (;;) {

                /* Discarding various invalid packets can cause us to
                   loop more than MAXFRAGS times, but enforce a sane bound
                   on how long we're willing to spend here. */
		if(bail++ >= (2*MAXFRAGS)) {
                        warning("too many packets in response; bailing out");
			return ERR_TOOMUCH;
                }

		if (numfrags == 0)
			tvo = tvout;
		else
			tvo = tvsout;

		FD_SET(sockfd, &fds);
		n = select(sockfd + 1, &fds, NULL, NULL, &tvo);

		if (n == -1) {
			warning("select fails");
			return -1;
		}
		if (n == 0) {
			/*
			 * Timed out.  Return what we have
			 */
			if (numfrags == 0) {
				if (timeo)
					fprintf(stderr,
						"%s: timed out, nothing received\n",
						currenthost);
				return ERR_TIMEOUT;
			}
			if (timeo)
				fprintf(stderr,
					"%s: timed out with incomplete data\n",
					currenthost);
			if (debug) {
				fprintf(stderr,
					"ERR_INCOMPLETE: Received fragments:\n");
				for (f = 0; f < numfrags; f++)
					fprintf(stderr,
						"%2u: %5d %5d\t%3d octets\n",
						(u_int)f, offsets[f],
						offsets[f] +
						counts[f],
						counts[f]);
				fprintf(stderr,
					"last fragment %sreceived\n",
					(seenlastfrag)
					    ? ""
					    : "not ");
			}
			return ERR_INCOMPLETE;
		}

		n = recv(sockfd, (char *)&rpkt, sizeof(rpkt), 0);
		if (n == -1) {
			warning("read");
			return -1;
		}

		if (debug >= 4) {
			printf("Response packet:\n");
			dump_hex_printable(&rpkt, n);
		}

		/*
		 * Check for format errors.  Bug proofing.
		 */
		if (n < (int)CTL_HEADER_LEN) {
			if (debug)
				printf("Short (%d byte) packet received\n", n);
			continue;
		}
		if (PKT_VERSION(rpkt.li_vn_mode) > NTP_VERSION
		    || PKT_VERSION(rpkt.li_vn_mode) < NTP_OLDVERSION) {
			if (debug)
				printf("Packet received with version %d\n",
				       PKT_VERSION(rpkt.li_vn_mode));
			continue;
		}
		if (PKT_MODE(rpkt.li_vn_mode) != MODE_CONTROL) {
			if (debug)
				printf("Packet received with mode %d\n",
				       PKT_MODE(rpkt.li_vn_mode));
			continue;
		}
		if (!CTL_ISRESPONSE(rpkt.r_m_e_op)) {
			if (debug)
				printf("Received request packet, wanted response\n");
			continue;
		}

		/*
		 * Check opcode and sequence number for a match.
		 * Could be old data getting to us.
		 */
		if (ntohs(rpkt.sequence) != sequence) {
			if (debug)
				printf("Received sequnce number %d, wanted %d\n",
				       ntohs(rpkt.sequence), sequence);
			continue;
		}
		if (CTL_OP(rpkt.r_m_e_op) != opcode) {
			if (debug)
			    printf(
				    "Received opcode %d, wanted %d (sequence number okay)\n",
				    CTL_OP(rpkt.r_m_e_op), opcode);
			continue;
		}

		/*
		 * Check the error code.  If non-zero, return it.
		 */
		if (CTL_ISERROR(rpkt.r_m_e_op)) {
			errcode = (ntohs(rpkt.status) >> 8) & 0xff;
			if (CTL_ISMORE(rpkt.r_m_e_op))
				TRACE(1, ("Error code %d received on not-final packet\n",
					  errcode));
			if (errcode == CERR_UNSPEC)
				return ERR_UNSPEC;
			return errcode;
		}

		/*
		 * Check the association ID to make sure it matches what
		 * we sent.
		 */
		if (ntohs(rpkt.associd) != associd) {
			TRACE(1, ("Association ID %d doesn't match expected %d\n",
				  ntohs(rpkt.associd), associd));
		}

		/*
		 * Collect offset and count.  Make sure they make sense.
		 */
		offset = ntohs(rpkt.offset);
		count = ntohs(rpkt.count);

		/*
		 * validate received payload size is padded to next 32-bit
		 * boundary and no smaller than claimed by rpkt.count
		 */
		if (n & 0x3) {
			TRACE(1, ("Response packet not padded, size = %d\n",
				  n));
			continue;
		}

		shouldbesize = (CTL_HEADER_LEN + count + 3) & ~3;

		if (n < shouldbesize) {
			printf("Response packet claims %u octets payload, above %ld received\n",
			       count, (long)n - CTL_HEADER_LEN);
			return ERR_INCOMPLETE;
		}

		if (debug >= 3 && shouldbesize > n) {
			uint32_t key;
			uint32_t *lpkt;
			int maclen;

			/*
			 * Usually we ignore authentication, but for debugging purposes
			 * we watch it here.
			 */
			/* round to 8 octet boundary */
			shouldbesize = (shouldbesize + 7) & ~7;

			maclen = n - shouldbesize;
			if (maclen >= (int)MIN_MAC_LEN) {
				printf(
					"Packet shows signs of authentication (total %d, data %d, mac %d)\n",
					n, shouldbesize, maclen);
				lpkt = (uint32_t *)&rpkt;
				printf("%08lx %08lx %08lx %08lx %08lx %08lx\n",
				       (u_long)ntohl(lpkt[(n - maclen)/sizeof(uint32_t) - 3]),
				       (u_long)ntohl(lpkt[(n - maclen)/sizeof(uint32_t) - 2]),
				       (u_long)ntohl(lpkt[(n - maclen)/sizeof(uint32_t) - 1]),
				       (u_long)ntohl(lpkt[(n - maclen)/sizeof(uint32_t)]),
				       (u_long)ntohl(lpkt[(n - maclen)/sizeof(uint32_t) + 1]),
				       (u_long)ntohl(lpkt[(n - maclen)/sizeof(uint32_t) + 2]));
				key = ntohl(lpkt[(n - maclen) / sizeof(uint32_t)]);
				printf("Authenticated with keyid %lu\n", (u_long)key);
				if (key != 0 && key != info_auth_keyid) {
					printf("We don't know that key\n");
				} else {
					if (authdecrypt(key, (uint32_t *)&rpkt,
					    n - maclen, maclen)) {
						printf("Auth okay!\n");
					} else {
						printf("Auth failed!\n");
					}
				}
			}
		}

		TRACE(2, ("Got packet, size = %d\n", n));
		if (count > (n - CTL_HEADER_LEN)) {
			TRACE(1, ("Received count of %u octets, data in packet is %ld\n",
				  count, (long)n - CTL_HEADER_LEN));
			continue;
		}
		if (count == 0 && CTL_ISMORE(rpkt.r_m_e_op)) {
			TRACE(1, ("Received count of 0 in non-final fragment\n"));
			continue;
		}
		if (offset + count > sizeof(pktdata)) {
			TRACE(1, ("Offset %u, count %u, too big for buffer\n",
				  offset, count));
			return ERR_TOOMUCH;
		}
		if (seenlastfrag && !CTL_ISMORE(rpkt.r_m_e_op)) {
			TRACE(1, ("Received second last fragment packet\n"));
			continue;
		}

		/*
		 * So far, so good.  Record this fragment, making sure it doesn't
		 * overlap anything.
		 */
		TRACE(2, ("Packet okay\n"));

		if (numfrags > (MAXFRAGS - 1)) {
			TRACE(2, ("Number of fragments exceeds maximum %d\n",
				  MAXFRAGS - 1));
			return ERR_TOOMUCH;
		}

		/*
		 * Find the position for the fragment relative to any
		 * previously received.
		 */
		for (f = 0;
		     f < numfrags && offsets[f] < offset;
		     f++) {
			/* empty body */ ;
		}

		if (f < numfrags && offset == offsets[f]) {
			TRACE(1, ("duplicate %u octets at %u ignored, prior %u at %u\n",
				  count, offset, counts[f], offsets[f]));
			continue;
		}

		if (f > 0 && (offsets[f-1] + counts[f-1]) > offset) {
			TRACE(1, ("received frag at %u overlaps with %u octet frag at %u\n",
				  offset, counts[f-1], offsets[f-1]));
			continue;
		}

		if (f < numfrags && (offset + count) > offsets[f]) {
			TRACE(1, ("received %u octet frag at %u overlaps with frag at %u\n",
				  count, offset, offsets[f]));
			continue;
		}

		for (ff = numfrags; ff > f; ff--) {
			offsets[ff] = offsets[ff-1];
			counts[ff] = counts[ff-1];
		}
		offsets[f] = offset;
		counts[f] = count;
		numfrags++;

		/*
		 * Got that stuffed in right.  Figure out if this was the last.
		 * Record status info out of the last packet.
		 */
		if (!CTL_ISMORE(rpkt.r_m_e_op)) {
			seenlastfrag = true;
			if (rstatus != 0)
				*rstatus = ntohs(rpkt.status);
		}

		/*
		 * Copy the data into the data buffer.
		 */
		memcpy((char *)pktdata + offset, &rpkt.u, count);

		/*
		 * If we've seen the last fragment, look for holes in the sequence.
		 * If there aren't any, we're done.
		 */
		if (seenlastfrag && offsets[0] == 0) {
			for (f = 1; f < numfrags; f++)
				if (offsets[f-1] + counts[f-1] !=
				    offsets[f])
					break;
			if (f == numfrags) {
				*rsize = offsets[f-1] + counts[f-1];
				TRACE(1, ("%lu packets reassembled into response\n",
					  (u_long)numfrags));
				return 0;
			}
		}
	}  /* giant for (;;) collecting response packets */
}  /* getresponse() */


/*
 * sendrequest - format and send a request packet
 */
static int
sendrequest(
	int opcode,
	associd_t associd,
	int auth,
	int qsize,
	const char *qdata
	)
{
	struct ntp_control qpkt;
	int	pktsize;
	u_long	key_id;
	char *	pass;
	int	maclen;

	/*
	 * Check to make sure the data will fit in one packet
	 */
	if (qsize > CTL_MAX_DATA_LEN) {
		fprintf(stderr,
			"***Internal error!  qsize (%d) too large\n",
			qsize);
		return 1;
	}

	/*
	 * Fill in the packet
	 */
	qpkt.li_vn_mode = PKT_LI_VN_MODE(0, pktversion, MODE_CONTROL);
	qpkt.r_m_e_op = (uint8_t)(opcode & CTL_OP_MASK);
	qpkt.sequence = htons(sequence);
	qpkt.status = 0;
	qpkt.associd = htons((u_short)associd);
	qpkt.offset = 0;
	qpkt.count = htons((u_short)qsize);

	pktsize = CTL_HEADER_LEN;

	/*
	 * If we have data, copy and pad it out to a 32-bit boundary.
	 */
	if (qsize > 0) {
		memcpy(&qpkt.u, qdata, (size_t)qsize);
		pktsize += qsize;
		while (pktsize & (sizeof(uint32_t) - 1)) {
			qpkt.u.data[qsize++] = 0;
			pktsize++;
		}
	}

	/*
	 * If it isn't authenticated we can just send it.  Otherwise
	 * we're going to have to think about it a little.
	 */
	if (!auth && !always_auth) {
		return sendpkt(&qpkt, pktsize);
	}

	/*
	 * Pad out packet to a multiple of 8 octets to be sure
	 * receiver can handle it.
	 */
	while (pktsize & 7) {
		qpkt.u.data[qsize++] = 0;
		pktsize++;
	}

	/*
	 * Get the keyid and the password if we don't have one.
	 */
	if (info_auth_keyid == 0) {
		key_id = getkeyid("Keyid: ");
		if (key_id == 0 || key_id > NTP_MAXKEY) {
			fprintf(stderr,
				"Invalid key identifier\n");
			return 1;
		}
		info_auth_keyid = key_id;
	}
	if (!authistrusted(info_auth_keyid)) {
		pass = getpass_keytype(info_auth_keytype);
		if ('\0' == pass[0]) {
			fprintf(stderr, "Invalid password\n");
			return 1;
		}
		authusekey(info_auth_keyid, info_auth_keytype,
			   (uint8_t *)pass);
		authtrust(info_auth_keyid, 1);
	}

	/*
	 * Do the encryption.
	 */
	maclen = authencrypt(info_auth_keyid, (void *)&qpkt, pktsize);
	if (!maclen) {
		fprintf(stderr, "Key not found\n");
		return 1;
	} else if ((size_t)maclen != (info_auth_hashlen + sizeof(keyid_t))) {
		fprintf(stderr,
			"%d octet MAC, %zu expected with %zu octet digest\n",
			maclen, (info_auth_hashlen + sizeof(keyid_t)),
			info_auth_hashlen);
		return 1;
	}

	return sendpkt((char *)&qpkt, pktsize + maclen);
}


/*
 * show_error_msg - display the error text for a mode 6 error response.
 */
void
show_error_msg(
	int		m6resp,
	associd_t	associd
	)
{
	if (numhosts > 1)
		fprintf(stderr, "server=%s ", currenthost);

	switch(m6resp) {

	case CERR_BADFMT:
		fprintf(stderr,
		    "***Server reports a bad format request packet\n");
		break;

	case CERR_PERMISSION:
		fprintf(stderr,
		    "***Server disallowed request (authentication?)\n");
		break;

	case CERR_BADOP:
		fprintf(stderr,
		    "***Server reports a bad opcode in request\n");
		break;

	case CERR_BADASSOC:
		fprintf(stderr,
		    "***Association ID %d unknown to server\n",
		    associd);
		break;

	case CERR_UNKNOWNVAR:
		fprintf(stderr,
		    "***A request variable unknown to the server\n");
		break;

	case CERR_BADVALUE:
		fprintf(stderr,
		    "***Server indicates a request variable was bad\n");
		break;

	case ERR_UNSPEC:
		fprintf(stderr,
		    "***Server returned an unspecified error\n");
		break;

	case ERR_TIMEOUT:
		fprintf(stderr, "***Request timed out\n");
		break;

	case ERR_INCOMPLETE:
		fprintf(stderr,
		    "***Response from server was incomplete\n");
		break;

	case ERR_TOOMUCH:
		fprintf(stderr,
		    "***Buffer size exceeded for returned data\n");
		break;

	default:
		fprintf(stderr,
		    "***Server returns unknown error code %d\n",
		    m6resp);
	}
}

/*
 * doquery - send a request and process the response, displaying
 *	     error messages for any error responses.
 */
int
doquery(
	int opcode,
	associd_t associd,
	int auth,
	int qsize,
	const char *qdata,
	u_short *rstatus,
	int *rsize,
	const char **rdata
	)
{
	return doqueryex(opcode, associd, auth, qsize, qdata, rstatus,
			 rsize, rdata, false);
}


/*
 * doqueryex - send a request and process the response, optionally
 *	       displaying error messages for any error responses.
 */
int
doqueryex(
	int opcode,
	associd_t associd,
	int auth,
	int qsize,
	const char *qdata,
	u_short *rstatus,
	int *rsize,
	const char **rdata,
	bool quiet
	)
{
	int res;
	bool done;

	/*
	 * Check to make sure host is open
	 */
	if (!havehost) {
		fprintf(stderr, "***No host open, use `host' command\n");
		return -1;
	}

	done = false;
	sequence++;

    again:
	/*
	 * send a request
	 */
	res = sendrequest(opcode, associd, auth, qsize, qdata);
	if (res != 0)
		return res;

	/*
	 * Get the response.  If we got a standard error, print a message
	 */
	res = getresponse(opcode, associd, rstatus, rsize, rdata, done);

	if (res > 0) {
		if (!done && (res == ERR_TIMEOUT || res == ERR_INCOMPLETE)) {
			if (res == ERR_INCOMPLETE) {
				/*
				 * better bump the sequence so we don't
				 * get confused about differing fragments.
				 */
				sequence++;
			}
			done = true;
			goto again;
		}
		if (!quiet)
			show_error_msg(res, associd);

	}
	return res;
}


#ifndef BUILD_AS_LIB
/*
 * getcmds - read commands from the standard input and execute them
 */
static void
getcmds(void)
{
	char *	line;
	int	count;

	ntp_readline_init(interactive ? prompt : NULL);

	for (;;) {
		line = ntp_readline(&count);
		if (NULL == line)
			break;
		docmd(line);
		free(line);
	}

	ntp_readline_uninit();
}
#endif /* !BUILD_AS_LIB */


#if !defined(SYS_WINNT) && !defined(BUILD_AS_LIB)
/*
 * abortcmd - catch interrupts and abort the current command
 */
static void
abortcmd(
	int sig
	)
{
	UNUSED_ARG(sig);

	if (current_output == stdout)
	    (void) fflush(stdout);
	putc('\n', stderr);
	(void) fflush(stderr);
	if (jump) longjmp(interrupt_buf, 1);
}
#endif	/* !SYS_WINNT && !BUILD_AS_LIB */


#ifndef	BUILD_AS_LIB
/*
 * docmd - decode the command line and execute a command
 */
static void
docmd(
	const char *cmdline
	)
{
	char *tokens[1+MAXARGS+2];
	struct parse pcmd;
	int ntok;
	static int i;
	struct xcmd *xcmd;

	/*
	 * Tokenize the command line.  If nothing on it, return.
	 */
	tokenize(cmdline, tokens, &ntok);
	if (ntok == 0)
	    return;

	/*
	 * Find the appropriate command description.
	 */
	i = findcmd(tokens[0], builtins, opcmds, &xcmd);
	if (i == 0) {
		(void) fprintf(stderr, "***Command `%s' unknown\n",
			       tokens[0]);
		return;
	} else if (i >= 2) {
		(void) fprintf(stderr, "***Command `%s' ambiguous\n",
			       tokens[0]);
		return;
	}

	/* Warn about ignored extra args */
	for (i = MAXARGS + 1; i < ntok ; ++i) {
		fprintf(stderr, "***Extra arg `%s' ignored\n", tokens[i]);
	}

	/*
	 * Save the keyword, then walk through the arguments, interpreting
	 * as we go.
	 */
	pcmd.keyword = tokens[0];
	pcmd.nargs = 0;
	for (i = 0; i < MAXARGS && xcmd->arg[i] != NO; i++) {
		if ((i+1) >= ntok) {
			if (!(xcmd->arg[i] & OPT)) {
				printusage(xcmd, stderr);
				return;
			}
			break;
		}
		if ((xcmd->arg[i] & OPT) && (*tokens[i+1] == '>'))
			break;
		if (!getarg(tokens[i+1], (int)xcmd->arg[i], &pcmd.argval[i]))
			return;
		pcmd.nargs++;
	}

	i++;
	if (i < ntok && *tokens[i] == '>') {
		char *fname;

		if (*(tokens[i]+1) != '\0')
			fname = tokens[i]+1;
		else if ((i+1) < ntok)
			fname = tokens[i+1];
		else {
			(void) fprintf(stderr, "***No file for redirect\n");
			return;
		}

		current_output = fopen(fname, "w");
		if (current_output == NULL) {
			(void) fprintf(stderr, "***Error opening %s: ", fname);
			perror("");
			return;
		}
		i = 1;		/* flag we need a close */
	} else {
		current_output = stdout;
		i = 0;		/* flag no close */
	}

	if (interactive && setjmp(interrupt_buf)) {
		jump = 0;
		return;
	} else {
		jump++;
		(xcmd->handler)(&pcmd, current_output);
		jump = 0;	/* HMS: 961106: was after fclose() */
		if (i) (void) fclose(current_output);
	}

	return;
}


/*
 * tokenize - turn a command line into tokens
 *
 * SK: Modified to allow a quoted string
 *
 * HMS: If the first character of the first token is a ':' then (after
 * eating inter-token whitespace) the 2nd token is the rest of the line.
 */

static void
tokenize(
	const char *line,
	char **tokens,
	int *ntok
	)
{
	register const char *cp;
	register char *sp;
	static char tspace[MAXLINE];

	sp = tspace;
	cp = line;
	for (*ntok = 0; *ntok < MAXTOKENS; (*ntok)++) {
		tokens[*ntok] = sp;

		/* Skip inter-token whitespace */
		while (ISSPACE(*cp))
		    cp++;

		/* If we're at EOL we're done */
		if (ISEOL(*cp))
		    break;

		/* If this is the 2nd token and the first token begins
		 * with a ':', then just grab to EOL.
		 */

		if (*ntok == 1 && tokens[0][0] == ':') {
			do {
				if (sp - tspace >= MAXLINE)
					goto toobig;
				*sp++ = *cp++;
			} while (!ISEOL(*cp));
		}

		/* Check if this token begins with a double quote.
		 * If yes, continue reading till the next double quote
		 */
		else if (*cp == '\"') {
			++cp;
			do {
				if (sp - tspace >= MAXLINE)
					goto toobig;
				*sp++ = *cp++;
			} while ((*cp != '\"') && !ISEOL(*cp));
			/* HMS: a missing closing " should be an error */
		}
		else {
			do {
				if (sp - tspace >= MAXLINE)
					goto toobig;
				*sp++ = *cp++;
			} while ((*cp != '\"') && !ISSPACE(*cp) && !ISEOL(*cp));
			/* HMS: Why check for a " in the previous line? */
		}

		if (sp - tspace >= MAXLINE)
			goto toobig;
		*sp++ = '\0';
	}
	return;

  toobig:
	*ntok = 0;
	fprintf(stderr,
		"***Line `%s' is too big\n",
		line);
	return;
}


/*
 * getarg - interpret an argument token
 */
static bool
getarg(
	const char *str,
	int code,
	arg_v *argp
	)
{
	u_long ul;

	switch (code & ~OPT) {
	case NTP_STR:
		argp->string = str;
		break;

	case NTP_ADD:
		if (!getnetnum(str, &argp->netnum, NULL, 0))
			return false;
		break;

	case NTP_UINT:
		if ('&' == str[0]) {
			errno = 0;
			ul = strtoul(&str[1], NULL, 10);
			if (errno == EINVAL || errno == ERANGE) {
				fprintf(stderr,
					"***Association index `%s' invalid/undecodable\n",
					str);
				return false;
			}
			if (0 == numassoc) {
				dogetassoc(stdout);
				if (0 == numassoc) {
					fprintf(stderr,
						"***No associations found, `%s' unknown\n",
						str);
					return false;
				}
			}
			ul = min(ul, numassoc);
			argp->uval = assoc_cache[ul - 1].assid;
			break;
		}
		errno = 0;
		argp->uval = strtoul(str, NULL, 10);
		if (errno == EINVAL || errno == ERANGE) {
			fprintf(stderr, "***Illegal unsigned value %s\n",
				str);
			return false;
		}
		break;

	case NTP_INT:
		errno = 0;
		argp->ival = strtol(str, NULL, 10);
		if (errno == EINVAL || errno == ERANGE) {
			fprintf(stderr, "***Illegal integer value %s\n",
				str);
			return false;
		}
		break;

	case IP_VERSION:
		if (!strcmp("-6", str)) {
			argp->ival = 6;
		} else if (!strcmp("-4", str)) {
			argp->ival = 4;
		} else {
			fprintf(stderr, "***Version must be either 4 or 6\n");
			return false;
		}
		break;
	}

	return true;
}
#endif	/* !BUILD_AS_LIB */


/*
 * findcmd - find a command in a command description table
 */
static int
findcmd(
	const char *	str,
	struct xcmd *	clist1,
	struct xcmd *	clist2,
	struct xcmd **	cmd
	)
{
	struct xcmd *cl;
	int clen;
	int nmatch;
	struct xcmd *nearmatch = NULL;
	struct xcmd *clist;

	clen = strlen(str);
	nmatch = 0;
	if (clist1 != 0)
	    clist = clist1;
	else if (clist2 != 0)
	    clist = clist2;
	else
	    return 0;

    again:
	for (cl = clist; cl->keyword != 0; cl++) {
		/* do a first character check, for efficiency */
		if (*str != *(cl->keyword))
		    continue;
		if (strncmp(str, cl->keyword, (unsigned)clen) == 0) {
			/*
			 * Could be extact match, could be approximate.
			 * Is exact if the length of the keyword is the
			 * same as the str.
			 */
			if (*((cl->keyword) + clen) == '\0') {
				*cmd = cl;
				return 1;
			}
			nmatch++;
			nearmatch = cl;
		}
	}

	/*
	 * See if there is more to do.  If so, go again.  Sorry about the
	 * goto, too much looking at BSD sources...
	 */
	if (clist == clist1 && clist2 != 0) {
		clist = clist2;
		goto again;
	}

	/*
	 * If we got extactly 1 near match, use it, else return number
	 * of matches.
	 */
	if (nmatch == 1) {
		*cmd = nearmatch;
		return 1;
	}
	return nmatch;
}


/*
 * getnetnum - given a host name, return its net number
 *	       and (optional) full name
 */
bool
getnetnum(
	const char *hname,
	sockaddr_u *num,
	char *fullhost,
	int af
	)
{
	struct addrinfo hints, *ai = NULL;

	UNUSED_ARG(af);

	ZERO(hints);
	hints.ai_flags = AI_CANONNAME;
#ifdef AI_ADDRCONFIG
	hints.ai_flags |= AI_ADDRCONFIG;
#endif

	/*
	 * decodenetnum only works with addresses, but handles syntax
	 * that getaddrinfo doesn't:  [2001::1]:1234
	 */
	if (decodenetnum(hname, num)) {
		if (fullhost != NULL)
			getnameinfo(&num->sa, SOCKLEN(num), fullhost,
				    NI_MAXHOST, NULL, 0, 0);
		return 1;
	} else if (getaddrinfo(hname, "ntp", &hints, &ai) == 0) {
		INSIST(sizeof(*num) >= ai->ai_addrlen);
		memcpy(num, ai->ai_addr, ai->ai_addrlen);
		if (fullhost != NULL) {
			if (ai->ai_canonname != NULL)
				strlcpy(fullhost, ai->ai_canonname,
					NI_MAXHOST);
			else
				getnameinfo(&num->sa, SOCKLEN(num),
					    fullhost, NI_MAXHOST, NULL,
					    0, 0);
		}
		freeaddrinfo(ai);
		return true;
	}
	fprintf(stderr, "***Can't find host %s\n", hname);

	return false;
}


/*
 * nntohost - convert network number to host name.  This routine enforces
 *	       the showhostnames setting.
 */
const char *
nntohost(
	sockaddr_u *netnum
	)
{
	return nntohost_col(netnum, LIB_BUFLENGTH - 1, false);
}


/*
 * nntohost_col - convert network number to host name in fixed width.
 *		  This routine enforces the showhostnames setting.
 *		  When displaying hostnames longer than the width,
 *		  the first part of the hostname is displayed.  When
 *		  displaying numeric addresses longer than the width,
 *		  Such as IPv6 addresses, the caller decides whether
 *		  the first or last of the numeric address is used.
 */
const char *
nntohost_col(
	sockaddr_u *	addr,
	size_t		width,
	int		preserve_lowaddrbits
	)
{
	const char *	out;

	if (!showhostnames || SOCK_UNSPEC(addr)) {
		if (preserve_lowaddrbits)
			out = trunc_left(stoa(addr), width);
		else
			out = trunc_right(stoa(addr), width);
	} else {
		out = trunc_right(socktohost(addr), width);
	}
	return out;
}


/*
 * nntohostp() is the same as nntohost() plus a :port suffix
 */
const char *
nntohostp(
	sockaddr_u *netnum
	)
{
	const char *	hostn;
	char *		buf;

	if (!showhostnames || SOCK_UNSPEC(netnum))
		return sptoa(netnum);

	hostn = socktohost(netnum);
	LIB_GETBUF(buf);
	snprintf(buf, LIB_BUFLENGTH, "%s:%u", hostn, SRCPORT(netnum));

	return buf;
}

/*
 * decodets - decode a hex or decimal timestamp into an l_fp format number
 */
bool
decodets(
	char *str,
	l_fp *lfp
	)
{
	/*
	 * If it starts with a 0x, decode as hex.
	 */
	if (*str == '0' && (*(str+1) == 'x' || *(str+1) == 'X'))
		return hextolfp(str+2, lfp);

	/*
	 * Might still be hex.  Check out the first character.  Talk
	 * about heuristics!
	 */
	if ((*str >= 'A' && *str <= 'F') || (*str >= 'a' && *str <= 'f'))
		return hextolfp(str, lfp);

	/*
	 * Try it as a decimal.  If this fails, try as an unquoted
	 * RT-11 date.  This code should go away eventually.
	 */
	if (atolfp(str, lfp))
		return true;

	return false;
}


/*
 * decodetime - decode a time value.  It should be in milliseconds
 */
bool
decodetime(
	char *str,
	l_fp *lfp
	)
{
	return mstolfp(str, lfp);
}


/*
 * decodeint - decode an integer
 */
bool
decodeint(
	char *str,
	long *val
	)
{
	errno = 0;
	/* magic 0 enables hex/octal recognition */
	*val = strtol(str, NULL, 0);
	return !(errno == EINVAL || errno == ERANGE);
}


/*
 * decodeuint - decode an unsigned integer
 */
bool
decodeuint(
	char *str,
	u_long *val
	)
{
	errno = 0;
	/* magic 0 enables hex/octal recognition */
	*val = strtoul(str, NULL, 0);
	return !(errno == EINVAL || errno == ERANGE);
}


/*
 * decodearr - decode an array of time values
 */
static bool
decodearr(
	char *str,
	int *narr,
	l_fp *lfparr
	)
{
	register char *cp, *bp;
	register l_fp *lfp;
	char buf[60];

	lfp = lfparr;
	cp = str;
	*narr = 0;

	while (*narr < 8) {
		while (isspace((int)*cp))
		    cp++;
		if (*cp == '\0')
		    break;

		bp = buf;
		while (!isspace((int)*cp) && *cp != '\0') {
		    *bp++ = *cp++;
		    if(bp >= buf + sizeof buf)
		        return false;
		}

		*bp++ = '\0';

		if (!decodetime(buf, lfp))
		    return false;
		(*narr)++;
		lfp++;
	}
	return true;
}


/*
 * Finally, the built in command handlers
 */

/*
 * help - tell about commands, or details of a particular command
 */
static void
help(
	struct parse *pcmd,
	FILE *fp
	)
{
	struct xcmd *xcp = NULL;	/* quiet warning */
	const char *cmd;
	const char *list[100];
	size_t word, words;
	size_t row, rows;
	size_t col, cols;
	size_t length;

	if (pcmd->nargs == 0) {
		words = 0;
		for (xcp = builtins; xcp->keyword != NULL; xcp++) {
			if (*(xcp->keyword) != '?' &&
			    words < COUNTOF(list))
				list[words++] = xcp->keyword;
		}
		for (xcp = opcmds; xcp->keyword != NULL; xcp++)
			if (words < COUNTOF(list))
				list[words++] = xcp->keyword;

		qsort((void *)list, words, sizeof(list[0]), helpsort);
		col = 0;
		for (word = 0; word < words; word++) {
			length = strlen(list[word]);
			col = max(col, length);
		}

		cols = SCREENWIDTH / ++col;
		rows = (words + cols - 1) / cols;

		fprintf(fp, "ntpq commands:\n");

		for (row = 0; row < rows; row++) {
			for (word = row; word < words; word += rows)
				fprintf(fp, "%-*.*s", (int)col,
					(int)col - 1, list[word]);
			fprintf(fp, "\n");
		}
	} else {
		cmd = pcmd->argval[0].string;
		words = findcmd(cmd, builtins, opcmds, &xcp);
		if (words == 0) {
			fprintf(stderr,
				"Command `%s' is unknown\n", cmd);
			return;
		} else if (words >= 2) {
			fprintf(stderr,
				"Command `%s' is ambiguous\n", cmd);
			return;
		}
		fprintf(fp, "function: %s\n", xcp->comment);
		printusage(xcp, fp);
	}
}


/*
 * helpsort - do hostname qsort comparisons
 */
static int
helpsort(
	const void *t1,
	const void *t2
	)
{
	const char * const *	name1 = t1;
	const char * const *	name2 = t2;

	return strcmp(*name1, *name2);
}


/*
 * printusage - print usage information for a command
 */
static void
printusage(
	struct xcmd *xcp,
	FILE *fp
	)
{
	register int i;

	/* XXX: Do we need to warn about extra args here too? */

	(void) fprintf(fp, "usage: %s", xcp->keyword);
	for (i = 0; i < MAXARGS && xcp->arg[i] != NO; i++) {
		if (xcp->arg[i] & OPT)
		    (void) fprintf(fp, " [ %s ]", xcp->desc[i]);
		else
		    (void) fprintf(fp, " %s", xcp->desc[i]);
	}
	(void) fprintf(fp, "\n");
}


/*
 * timeout - set time out time
 */
static void
timeout(
	struct parse *pcmd,
	FILE *fp
	)
{
	int val;

	if (pcmd->nargs == 0) {
		val = (int)tvout.tv_sec * 1000 + tvout.tv_usec / 1000;
		(void) fprintf(fp, "primary timeout %d ms\n", val);
	} else {
		tvout.tv_sec = pcmd->argval[0].uval / 1000;
		tvout.tv_usec = (pcmd->argval[0].uval - ((long)tvout.tv_sec * 1000))
			* 1000;
	}
}


/*
 * auth_delay - set delay for auth requests
 */
static void
auth_delay(
	struct parse *pcmd,
	FILE *fp
	)
{
	bool isneg;
	u_long val;

	if (pcmd->nargs == 0) {
		val = delay_time.l_ui * 1000 + delay_time.l_uf / 4294967;
		(void) fprintf(fp, "delay %lu ms\n", val);
	} else {
		if (pcmd->argval[0].ival < 0) {
			isneg = true;
			val = (u_long)(-pcmd->argval[0].ival);
		} else {
			isneg = false;
			val = (u_long)pcmd->argval[0].ival;
		}

		delay_time.l_ui = val / 1000;
		val %= 1000;
		delay_time.l_uf = val * 4294967;	/* 2**32/1000 */

		if (isneg)
		    L_NEG(&delay_time);
	}
}


/*
 * host - set the host we are dealing with.
 */
static void
host(
	struct parse *pcmd,
	FILE *fp
	)
{
	int i;

	if (pcmd->nargs == 0) {
		if (havehost)
			(void) fprintf(fp, "current host is %s\n",
					   currenthost);
		else
			(void) fprintf(fp, "no current host\n");
		return;
	}

	i = 0;
	ai_fam_templ = ai_fam_default;
	if (pcmd->nargs == 2) {
		if (!strcmp("-4", pcmd->argval[i].string))
			ai_fam_templ = AF_INET;
		else if (!strcmp("-6", pcmd->argval[i].string))
			ai_fam_templ = AF_INET6;
		else
			goto no_change;
		i = 1;
	}
	if (openhost(pcmd->argval[i].string, ai_fam_templ)) {
		fprintf(fp, "current host set to %s\n", currenthost);
	} else {
    no_change:
		if (havehost)
			fprintf(fp, "current host remains %s\n",
				currenthost);
		else
			fprintf(fp, "still no current host\n");
	}
}


/*
 * poll - do one (or more) polls of the host via NTP
 */
/*ARGSUSED*/
static void
ntp_poll(
	struct parse *pcmd,
	FILE *fp
	)
{
	UNUSED_ARG(pcmd);
	(void) fprintf(fp, "poll not implemented yet\n");
}


/*
 * keyid - get a keyid to use for authenticating requests
 */
static void
keyid(
	struct parse *pcmd,
	FILE *fp
	)
{
	if (pcmd->nargs == 0) {
		if (info_auth_keyid == 0)
		    (void) fprintf(fp, "no keyid defined\n");
		else
		    (void) fprintf(fp, "keyid is %lu\n", (u_long)info_auth_keyid);
	} else {
		/* allow zero so that keyid can be cleared. */
		if(pcmd->argval[0].uval > NTP_MAXKEY)
		    (void) fprintf(fp, "Invalid key identifier\n");
		info_auth_keyid = pcmd->argval[0].uval;
	}
}

/*
 * keytype - get type of key to use for authenticating requests
 */
static void
keytype(
	struct parse *pcmd,
	FILE *fp
	)
{
	const char *	digest_name;
	size_t		digest_len;
	int		key_type;

	if (!pcmd->nargs) {
		fprintf(fp, "keytype is %s with %lu octet digests\n",
			keytype_name(info_auth_keytype),
			(u_long)info_auth_hashlen);
		return;
	}

	digest_name = pcmd->argval[0].string;
	digest_len = 0;
	key_type = keytype_from_text(digest_name, &digest_len);

	if (!key_type) {
		fprintf(fp, "keytype is not valid. "
#ifdef HAVE_OPENSSL
			"Type \"help keytype\" for the available digest types.\n");
#else
			"Only \"md5\" is available.\n");
#endif
		return;
	}

	info_auth_keytype = key_type;
	info_auth_hashlen = digest_len;
}


/*
 * passwd - get an authentication key
 */
/*ARGSUSED*/
static void
passwd(
	struct parse *pcmd,
	FILE *fp
	)
{
	const char *pass;

	if (info_auth_keyid == 0) {
		info_auth_keyid = getkeyid("Keyid: ");
		if (info_auth_keyid == 0) {
			(void)fprintf(fp, "Keyid must be defined\n");
			return;
		}
	}
	if (pcmd->nargs >= 1)
		pass = pcmd->argval[0].string;
	else {
		pass = getpass_keytype(info_auth_keytype);
		if ('\0' == pass[0]) {
			fprintf(fp, "Password unchanged\n");
			return;
		}
	}
	authusekey(info_auth_keyid, info_auth_keytype,
		   (const uint8_t *)pass);
	authtrust(info_auth_keyid, 1);
}


/*
 * hostnames - set the showhostnames flag
 */
static void
hostnames(
	struct parse *pcmd,
	FILE *fp
	)
{
	if (pcmd->nargs == 0) {
		if (showhostnames)
		    (void) fprintf(fp, "hostnames being shown\n");
		else
		    (void) fprintf(fp, "hostnames not being shown\n");
	} else {
		if (STREQ(pcmd->argval[0].string, "yes"))
		    showhostnames = 1;
		else if (STREQ(pcmd->argval[0].string, "no"))
		    showhostnames = 0;
		else
		    (void)fprintf(stderr, "What?\n");
	}
}



/*
 * setdebug - set/change debugging level
 */
static void
setdebug(
	struct parse *pcmd,
	FILE *fp
	)
{
	if (pcmd->nargs == 0) {
		(void) fprintf(fp, "debug level is %d\n", debug);
		return;
	} else if (STREQ(pcmd->argval[0].string, "no")) {
		debug = 0;
	} else if (STREQ(pcmd->argval[0].string, "more")) {
		debug++;
	} else if (STREQ(pcmd->argval[0].string, "less")) {
		debug--;
	} else {
		(void) fprintf(fp, "What?\n");
		return;
	}
	(void) fprintf(fp, "debug level set to %d\n", debug);
}


/*
 * quit - stop this nonsense
 */
/*ARGSUSED*/
static void
quit(
	struct parse *pcmd,
	FILE *fp
	)
{
	UNUSED_ARG(pcmd);
	UNUSED_ARG(fp);
	if (havehost)
	    closesocket(sockfd);	/* cleanliness next to godliness */
	exit(0);
}


/*
 * version - print the current version number
 */
/*ARGSUSED*/
static void
version(
	struct parse *pcmd,
	FILE *fp
	)
{

	UNUSED_ARG(pcmd);
	(void) fprintf(fp, "ntpq %s\n", Version);
	return;
}

/*
 * raw - set raw mode output
 */
/*ARGSUSED*/
static void
raw(
	struct parse *pcmd,
	FILE *fp
	)
{
	UNUSED_ARG(pcmd);
	rawmode = true;
	(void) fprintf(fp, "Output set to raw\n");
}


/*
 * cooked - set cooked mode output
 */
/*ARGSUSED*/
static void
cooked(
	struct parse *pcmd,
	FILE *fp
	)
{
	UNUSED_ARG(pcmd);
	rawmode = false;
	(void) fprintf(fp, "Output set to cooked\n");
	return;
}


/*
 * authenticate - always authenticate requests to this host
 */
static void
authenticate(
	struct parse *pcmd,
	FILE *fp
	)
{
	if (pcmd->nargs == 0) {
		if (always_auth) {
			(void) fprintf(fp,
				       "authenticated requests being sent\n");
		} else
		    (void) fprintf(fp,
				   "unauthenticated requests being sent\n");
	} else {
		if (STREQ(pcmd->argval[0].string, "yes")) {
			always_auth = true;
		} else if (STREQ(pcmd->argval[0].string, "no")) {
			always_auth = false;
		} else
		    (void)fprintf(stderr, "What?\n");
	}
}


/*
 * ntpversion - choose the NTP version to use
 */
static void
ntpversion(
	struct parse *pcmd,
	FILE *fp
	)
{
	if (pcmd->nargs == 0) {
		(void) fprintf(fp,
			       "NTP version being claimed is %d\n", pktversion);
	} else {
		if (pcmd->argval[0].uval < NTP_OLDVERSION
		    || pcmd->argval[0].uval > NTP_VERSION) {
			(void) fprintf(stderr, "versions %d to %d, please\n",
				       NTP_OLDVERSION, NTP_VERSION);
		} else {
			pktversion = (uint8_t) pcmd->argval[0].uval;
		}
	}
}


static void __attribute__((__format__(__printf__, 1, 0)))
vwarning(const char *fmt, va_list ap)
{
	int serrno = errno;
	(void) fprintf(stderr, "%s: ", progname);
	vfprintf(stderr, fmt, ap);
	(void) fprintf(stderr, ": %s\n", strerror(serrno));
}

/*
 * warning - print a warning message
 */
static void __attribute__((__format__(__printf__, 1, 2)))
warning(
	const char *fmt,
	...
	)
{
	va_list ap;
	va_start(ap, fmt);
	vwarning(fmt, ap);
	va_end(ap);
}


/*
 * error - print a message and exit
 */
static void __attribute__((__format__(__printf__, 1, 2)))
error(
	const char *fmt,
	...
	)
{
	va_list ap;
	va_start(ap, fmt);
	vwarning(fmt, ap);
	va_end(ap);
	exit(1);
}
/*
 * getkeyid - prompt the user for a keyid to use
 */
static u_long
getkeyid(
	const char *keyprompt
	)
{
	int c;
	FILE *fi;
	char pbuf[20];
	size_t i;
	size_t ilim;
	int fd;

#ifndef SYS_WINNT
	fd = open("/dev/tty", 2);
	if (fd < 0 || (fi = fdopen(fd, "r")) == NULL)
#else
	fd = open("CONIN$", _O_TEXT);
	if (fd < 0 || (fi = _fdopen(fd, "r")) == NULL)
#endif /* SYS_WINNT */
		fi = stdin;
	else
		setbuf(fi, (char *)NULL);
	fprintf(stderr, "%s", keyprompt); fflush(stderr);
	for (i = 0, ilim = COUNTOF(pbuf) - 1;
	     i < ilim && (c = getc(fi)) != '\n' && c != EOF;
	     )
		pbuf[i++] = (char)c;
	pbuf[i] = '\0';
	if (fi != stdin)
		fclose(fi);

	return (u_long) atoi(pbuf);
}


/*
 * atoascii - printable-ize possibly ascii data using the character
 *	      transformations cat -v uses.
 */
static void
atoascii(
	const char *in,
	size_t in_octets,
	char *out,
	size_t out_octets
	)
{
	const uint8_t *	pchIn;
	const uint8_t *	pchInLimit;
	uint8_t *	pchOut;
	uint8_t		c;

	pchIn = (const uint8_t *)in;
	pchInLimit = pchIn + in_octets;
	pchOut = (uint8_t *)out;

	if (NULL == pchIn) {
		if (0 < out_octets)
			*pchOut = '\0';
		return;
	}

#define	ONEOUT(c)					\
do {							\
	if (0 == --out_octets) {			\
		*pchOut = '\0';				\
		return;					\
	}						\
	*pchOut++ = (c);				\
} while (0)

	for (	; pchIn < pchInLimit; pchIn++) {
		c = *pchIn;
		if ('\0' == c)
			break;
		if (c & 0x80) {
			ONEOUT('M');
			ONEOUT('-');
			c &= 0x7f;
		}
		if (c < ' ') {
			ONEOUT('^');
			ONEOUT((uint8_t)(c + '@'));
		} else if (0x7f == c) {
			ONEOUT('^');
			ONEOUT('?');
		} else
			ONEOUT(c);
	}
	ONEOUT('\0');

#undef ONEOUT
}


/*
 * makeascii - print possibly ascii data using the character
 *	       transformations that cat -v uses.
 */
void
makeascii(
	int length,
	const char *data,
	FILE *fp
	)
{
	const uint8_t *data_uint8_t;
	const uint8_t *cp;
	int c;

	data_uint8_t = (const uint8_t *)data;

	for (cp = data_uint8_t; cp < data_uint8_t + length; cp++) {
		c = (int)*cp;
		if (c & 0x80) {
			putc('M', fp);
			putc('-', fp);
			c &= 0x7f;
		}

		if (c < ' ') {
			putc('^', fp);
			putc(c + '@', fp);
		} else if (0x7f == c) {
			putc('^', fp);
			putc('?', fp);
		} else
			putc(c, fp);
	}
}


/*
 * asciize - same thing as makeascii except add a newline
 */
void
asciize(
	int length,
	char *data,
	FILE *fp
	)
{
	makeascii(length, data, fp);
	putc('\n', fp);
}


/*
 * truncate string to fit clipping excess at end.
 *	"too long"	->	"too l"
 * Used for hostnames.
 */
const char *
trunc_right(
	const char *	src,
	size_t		width
	)
{
	size_t	sl;
	char *	out;


	sl = strlen(src);
	if (sl > width && LIB_BUFLENGTH - 1 > width && width > 0) {
		LIB_GETBUF(out);
		memcpy(out, src, width);
		out[width] = '\0';

		return out;
	}

	return src;
}


/*
 * truncate string to fit by preserving right side and using '_' to hint
 *	"too long"	->	"_long"
 * Used for local IPv6 addresses, where low bits differentiate.
 */
const char *
trunc_left(
	const char *	src,
	size_t		width
	)
{
	size_t	sl;
	char *	out;


	sl = strlen(src);
	if (sl > width && LIB_BUFLENGTH - 1 > width && width > 1) {
		LIB_GETBUF(out);
		out[0] = '_';
		memcpy(&out[1], &src[sl + 1 - width], width);

		return out;
	}

	return src;
}


/*
 * Some circular buffer space
 */
#define	CBLEN	80
#define	NUMCB	6

char circ_buf[NUMCB][CBLEN];
int nextcb = 0;

/*
 * nextvar - find the next variable in the buffer
 */
int
nextvar(
	int *datalen,
	const char **datap,
	char **vname,
	char **vvalue
	)
{
	const char *cp;
	const char *np;
	const char *cpend;
	size_t srclen;
	size_t len;
	static char name[MAXVARLEN];
	static char value[MAXVALLEN];

	cp = *datap;
	cpend = cp + *datalen;

	/*
	 * Space past commas and white space
	 */
	while (cp < cpend && (*cp == ',' || isspace((int)*cp)))
		cp++;
	if (cp >= cpend)
		return 0;

	/*
	 * Copy name until we hit a ',', an '=', a '\r' or a '\n'.  Backspace
	 * over any white space and terminate it.
	 */
	srclen = strcspn(cp, ",=\r\n");
	srclen = min(srclen, (size_t)(cpend - cp));
	len = srclen;
	while (len > 0 && isspace((unsigned char)cp[len - 1]))
		len--;
        if (len >= sizeof(name))
                return 0;
	if (len > 0)
		memcpy(name, cp, len);
	name[len] = '\0';
	*vname = name;
	cp += srclen;

	/*
	 * Check if we hit the end of the buffer or a ','.  If so we are done.
	 */
	if (cp >= cpend || *cp == ',' || *cp == '\r' || *cp == '\n') {
		if (cp < cpend)
			cp++;
		*datap = cp;
		*datalen = cpend - cp;
		*vvalue = NULL;
		return 1;
	}

	/*
	 * So far, so good.  Copy out the value
	 */
	cp++;	/* past '=' */
	while (cp < cpend && (isspace((unsigned char)*cp) && *cp != '\r' && *cp != '\n'))
		cp++;
	np = cp;
	if ('"' == *np) {
		do {
			np++;
		} while (np < cpend && '"' != *np);
		if (np < cpend && '"' == *np)
			np++;
	} else {
		while (np < cpend && ',' != *np && '\r' != *np)
			np++;
	}
	len = np - cp;
	if (np > cpend || len >= sizeof(value) ||
	    (np < cpend && ',' != *np && '\r' != *np))
		return 0;
	memcpy(value, cp, len);
	/*
	 * Trim off any trailing whitespace
	 */
	while (len > 0 && isspace((unsigned char)value[len - 1]))
		len--;
	value[len] = '\0';

	/*
	 * Return this.  All done.
	 */
	if (np < cpend && ',' == *np)
		np++;
	*datap = np;
	*datalen = cpend - np;
	*vvalue = value;
	return 1;
}


u_short
varfmt(const char * varname)
{
	u_int n;

	for (n = 0; n < COUNTOF(cookedvars); n++)
		if (!strcmp(varname, cookedvars[n].varname))
			return cookedvars[n].fmt;

	return PADDING;
}


/*
 * printvars - print variables returned in response packet
 */
void
printvars(
	int length,
	const char *data,
	int status,
	int sttype,
	int quiet,
	FILE *fp
	)
{
	if (rawmode)
	    rawprint(sttype, length, data, status, quiet, fp);
	else
	    cookedprint(sttype, length, data, status, quiet, fp);
}


/*
 * rawprint - do a printout of the data in raw mode
 */
static void
rawprint(
	int datatype,
	int length,
	const char *data,
	int status,
	int quiet,
	FILE *fp
	)
{
	const char *cp;
	const char *cpend;

	UNUSED_ARG(datatype);
	/*
	 * Essentially print the data as is.  We reformat unprintables, though.
	 */
	cp = data;
	cpend = data + length;

	if (!quiet)
		(void) fprintf(fp, "status=0x%04x,\n", status);

	while (cp < cpend) {
		if (*cp == '\r') {
			/*
			 * If this is a \r and the next character is a
			 * \n, supress this, else pretty print it.  Otherwise
			 * just output the character.
			 */
			if (cp == (cpend - 1) || *(cp + 1) != '\n')
			    makeascii(1, cp, fp);
		} else if (isspace((unsigned char)*cp) || isprint((unsigned char)*cp))
			putc(*cp, fp);
		else
			makeascii(1, cp, fp);
		cp++;
	}
}


/*
 * Global data used by the cooked output routines
 */
int out_chars;		/* number of characters output */
int out_linecount;	/* number of characters output on this line */


/*
 * startoutput - get ready to do cooked output
 */
static void
startoutput(void)
{
	out_chars = 0;
	out_linecount = 0;
}


/*
 * output - output a variable=value combination
 */
static void
output(
	FILE *fp,
	const char *name,
	const char *value
	)
{
	size_t len;

	/* strlen of "name=value" */
	len = strlen(name) + 1 + strlen(value);

	if (out_chars != 0) {
		out_chars += 2;
		if ((out_linecount + len + 2) > MAXOUTLINE) {
			fputs(",\n", fp);
			out_linecount = 0;
		} else {
			fputs(", ", fp);
			out_linecount += 2;
		}
	}

	fputs(name, fp);
	putc('=', fp);
	fputs(value, fp);
	out_chars += len;
	out_linecount += len;
}


/*
 * endoutput - terminate a block of cooked output
 */
static void
endoutput(
	FILE *fp
	)
{
	if (out_chars != 0)
		putc('\n', fp);
}


/*
 * outputarr - output an array of values
 */
static void
outputarr(
	FILE *fp,
	char *name,
	int narr,
	l_fp *lfp
	)
{
	register char *bp;
	register char *cp;
	register int i;
	register int len;
	char *buf;

	REQUIRE(narr >= 0 && narr <= MAXVALLEN);
	buf = malloc(16 + 8*narr);
	ENSURE(buf != NULL);

	bp = buf;
	/*
	 * Hack to align delay and offset values
	 */
	for (i = (int)strlen(name); i < 11; i++)
	    *bp++ = ' ';

	for (i = narr; i > 0; i--) {
		if (i != narr)
		    *bp++ = ' ';
		cp = lfptoms(lfp, 2);
		len = strlen(cp);
		if (len > 7) {
			cp[7] = '\0';
			len = 7;
		}
		while (len < 7) {
			*bp++ = ' ';
			len++;
		}
		while (*cp != '\0')
		    *bp++ = *cp++;
		lfp++;
	}
	*bp = '\0';
	output(fp, name, buf);
	free(buf);
}

static char *
tstflags(
	u_long val
	)
{
	register char *cp, *s;
	size_t cb;
	register int i;
	register const char *sep;

	sep = "";
	i = 0;
	s = cp = circ_buf[nextcb];
	if (++nextcb >= NUMCB)
		nextcb = 0;
	cb = sizeof(circ_buf[0]);

	snprintf(cp, cb, "%02lx", val);
	cp += strlen(cp);
	cb -= strlen(cp);
	if (!val) {
		strlcat(cp, " ok", cb);
		cp += strlen(cp);
		cb -= strlen(cp);
	} else {
		if (cb) {
			*cp++ = ' ';
			cb--;
		}
		for (i = 0; i < (int)COUNTOF(tstflagnames); i++) {
			if (val & 0x1) {
				snprintf(cp, cb, "%s%s", sep,
					 tstflagnames[i]);
				sep = ", ";
				cp += strlen(cp);
				cb -= strlen(cp);
			}
			val >>= 1;
		}
	}
	if (cb)
		*cp = '\0';

	return s;
}

/*
 * cookedprint - output variables in cooked mode
 */
static void
cookedprint(
	int datatype,
	int length,
	const char *data,
	int status,
	int quiet,
	FILE *fp
	)
{
	char *name;
	char *value;
	char output_raw;
	int fmt;
	l_fp lfp;
	sockaddr_u hval;
	u_long uval;
	int narr;
	size_t len;
	l_fp lfparr[8];
	char b[12];
	char bn[2 * MAXVARLEN];
	char bv[2 * MAXVALLEN];

	UNUSED_ARG(datatype);

	if (!quiet)
		fprintf(fp, "status=%04x %s,\n", status,
			statustoa(datatype, status));

	startoutput();
	while (nextvar(&length, &data, &name, &value)) {
		fmt = varfmt(name);
		output_raw = 0;
		switch (fmt) {

		case PADDING:
			output_raw = '*';
			break;

		case TS:
			if (!decodets(value, &lfp))
				output_raw = '?';
			else
				output(fp, name, prettydate(&lfp));
			break;

		case HA:	/* fallthru */
		case NA:
			if (!decodenetnum(value, &hval)) {
				output_raw = '?';
			} else if (fmt == HA){
				output(fp, name, nntohost(&hval));
			} else {
				output(fp, name, stoa(&hval));
			}
			break;

		case RF:
			if (decodenetnum(value, &hval)) {
			    output(fp, name, stoa(&hval));
			} else if (strlen(value) <= 4) {
				output(fp, name, value);
			} else {
				output_raw = '?';
			}
			break;

		case LP:
			if (!decodeuint(value, &uval) || uval > 3) {
				output_raw = '?';
			} else {
				b[0] = (0x2 & uval)
					   ? '1'
					   : '0';
				b[1] = (0x1 & uval)
					   ? '1'
					   : '0';
				b[2] = '\0';
				output(fp, name, b);
			}
			break;

		case OC:
			if (!decodeuint(value, &uval)) {
				output_raw = '?';
			} else {
				snprintf(b, sizeof(b), "%03lo", uval);
				output(fp, name, b);
			}
			break;

		case AR:
			if (!decodearr(value, &narr, lfparr))
				output_raw = '?';
			else
				outputarr(fp, name, narr, lfparr);
			break;

		case FX:
			if (!decodeuint(value, &uval))
				output_raw = '?';
			else
				output(fp, name, tstflags(uval));
			break;

		default:
			fprintf(stderr, "Internal error in cookedprint, %s=%s, fmt %d\n",
				name, value, fmt);
			output_raw = '?';
			break;
		}

		if (output_raw != 0) {
			atoascii(name, MAXVARLEN, bn, sizeof(bn));
			atoascii(value, MAXVALLEN, bv, sizeof(bv)-1);
			if (output_raw != '*') {
				len = strlen(bv);
				bv[len] = output_raw;
				bv[len+1] = '\0';
			}
			output(fp, bn, bv);
		}
	}
	endoutput(fp);
}


/*
 * sortassoc - sort associations in the cache into ascending order
 */
void
sortassoc(void)
{
	if (numassoc > 1)
		qsort(assoc_cache, (size_t)numassoc,
		      sizeof(assoc_cache[0]), &assoccmp);
}


/*
 * assoccmp - compare two associations
 */
static int
assoccmp(
	const void *t1,
	const void *t2
	)
{
	const struct association *ass1 = t1;
	const struct association *ass2 = t2;

	if (ass1->assid < ass2->assid)
		return COMPARE_LESSTHAN;
	if (ass1->assid > ass2->assid)
		return COMPARE_GREATERTHAN;
	return COMPARE_EQUAL;
}


/*
 * grow_assoc_cache() - enlarge dynamic assoc_cache array
 *
 * The strategy is to add an assumed 4k page size at a time, leaving
 * room for malloc() bookkeeping overhead equivalent to 4 pointers.
 */
void
grow_assoc_cache(void)
{
	static size_t	prior_sz;
	size_t		new_sz;

	new_sz = prior_sz + 4 * 1024;
	if (0 == prior_sz) {
		new_sz -= 4 * sizeof(void *);
	}
	assoc_cache = erealloc_zero(assoc_cache, new_sz, prior_sz); 
	prior_sz = new_sz;
	assoc_cache_slots = new_sz / sizeof(assoc_cache[0]);
}




/*
 * Obtain list of digest names
 */

#ifdef HAVE_OPENSSL
# ifdef HAVE_EVP_MD_DO_ALL_SORTED
struct hstate {
   char *list;
   const char **seen;
   int idx;
};
#define K_PER_LINE 8
#define K_NL_PFX_STR "\n    "
#define K_DELIM_STR ", "
static void list_md_fn(const EVP_MD *m, const char *from, const char *to, void *arg )
{
    size_t len, n;
    const char *name, *cp, **seen;
    struct hstate *hstate = arg;
    EVP_MD_CTX ctx;
    u_int digest_len;
    uint8_t digest[EVP_MAX_MD_SIZE];

    UNUSED_ARG(from);
    UNUSED_ARG(to);

    if (!m)
        return; /* Ignore aliases */

    name = EVP_MD_name(m);

    /* Lowercase names aren't accepted by keytype_from_text in ssl_init.c */

    for( cp = name; *cp; cp++ ) {
	if( islower((int)*cp) )
	    return;
    }
    len = (cp - name) + 1;

    /* There are duplicates.  Discard if name has been seen. */

    for (seen = hstate->seen; *seen; seen++)
        if (!strcmp(*seen, name))
	    return;
    n = (seen - hstate->seen) + 2;
    hstate->seen = realloc(hstate->seen, n * sizeof(*seen));
    hstate->seen[n-2] = name;
    hstate->seen[n-1] = NULL;

    /* Discard MACs that NTP won't accept.
     * Keep this consistent with keytype_from_text() in ssl_init.c.
     */

    EVP_DigestInit(&ctx, EVP_get_digestbyname(name));
    EVP_DigestFinal(&ctx, digest, &digest_len);
    if (digest_len > (MAX_MAC_LEN - sizeof(keyid_t)))
        return;

    if (hstate->list != NULL)
	len += strlen(hstate->list);
    len += (hstate->idx >= K_PER_LINE)? strlen(K_NL_PFX_STR): strlen(K_DELIM_STR);

    if (hstate->list == NULL) {
	hstate->list = (char *)malloc(len);
	hstate->list[0] = '\0';
    } else
	hstate->list = (char *)realloc(hstate->list, len);

    snprintf(hstate->list + strlen(hstate->list), len - strlen(hstate->list),
	     "%s%s",
	     ((hstate->idx >= K_PER_LINE)? K_NL_PFX_STR : K_DELIM_STR),
	     name);
    if (hstate->idx >= K_PER_LINE)
	hstate->idx = 1;
    else
	hstate->idx++;
}
# endif
#endif

static char *list_digest_names(void)
{
    char *list = NULL;

#ifdef HAVE_OPENSSL
# ifdef HAVE_EVP_MD_DO_ALL_SORTED
    struct hstate hstate = { NULL, NULL, K_PER_LINE+1 };

    hstate.seen = (const char **)calloc(1, sizeof( const char * ));

    INIT_SSL();
    EVP_MD_do_all_sorted(list_md_fn, &hstate);
    list = hstate.list;
    free(hstate.seen);
# else
    list = strdup("md5, others (upgrade to OpenSSL-1.0 for full list)");
# endif
#else
    list = strdup("md5");
#endif

    return list;
}
