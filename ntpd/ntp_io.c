/*
 * ntp_io.c - input/output routines for ntpd.	The socket-opening code
 *		   was shamelessly stolen from ntpd.
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "ntp_machine.h"
#include "ntpd.h"
#include "ntp_io.h"
#include "iosignal.h"
#include "ntp_refclock.h"
#include "ntp_if.h"
#include "ntp_stdlib.h"
#include "ntp.h"

#ifdef SIM
#include "ntpsim.h"
#endif

#include <stdio.h>
#include <signal.h>
#ifdef HAVE_SYS_PARAM_H
# include <sys/param.h>
#endif /* HAVE_SYS_PARAM_H */
#ifdef HAVE_NETINET_IN_H
# include <netinet/in.h>
#endif
#ifdef HAVE_NETINET_IN_SYSTM_H
# include <netinet/in_systm.h>
#else /* Some old linux systems at least have in_system.h instead. */
# ifdef HAVE_NETINET_IN_SYSTEM_H
#  include <netinet/in_system.h>
# endif
#endif /* HAVE_NETINET_IN_SYSTM_H */
#ifdef HAVE_NETINET_IP_H
# include <netinet/ip.h>
#endif
#ifdef HAVE_SYS_IOCTL_H
# include <sys/ioctl.h>
#endif
#ifdef HAVE_SYS_SOCKIO_H	/* UXPV: SIOC* #defines (Frank Vance <fvance@waii.com>) */
# include <sys/sockio.h>
#endif
#include <arpa/inet.h>

extern int listen_to_virtual_ips;

#if _BSDI_VERSION >= 199510
# include <ifaddrs.h>
#endif

#if defined(VMS)		/* most likely UCX-specific */

#include <UCX$INETDEF.H>

/* "un*x"-compatible names for some items in UCX$INETDEF.H */
#define ifreq		IFREQDEF
#define ifr_name	IFR$T_NAME
#define ifr_addr		IFR$R_DUMMY.IFR$T_ADDR
#define ifr_broadaddr	IFR$R_DUMMY.IFR$T_BROADADDR
#define ifr_flags		IFR$R_DUMMY.IFR$R_DUMMY_1_OVRL.IFR$W_FLAGS
#define IFF_UP		IFR$M_IFF_UP
#define IFF_BROADCAST	IFR$M_IFF_BROADCAST
#define IFF_LOOPBACK	IFR$M_IFF_LOOPBACK

#endif /* VMS */

#if defined(SYS_WINNT)
#include <transmitbuff.h>
#endif

#if defined(VMS) || defined(SYS_WINNT)
/* structure used in SIOCGIFCONF request (after [KSR] OSF/1) */
struct ifconf {
	unsigned long ifc_len;			/* size of buffer */
	union {
		caddr_t ifcu_buf;
		struct ifreq *ifcu_req;
	} ifc_ifcu;
};
#define ifc_buf ifc_ifcu.ifcu_buf	/* buffer address */
#define ifc_req ifc_ifcu.ifcu_req	/* array of structures returned */

#endif /* VMS */

#if defined(USE_TTY_SIGPOLL) || defined(USE_UDP_SIGPOLL)
# if defined(SYS_AIX) && defined(_IO) /* XXX Identify AIX some other way */
#  undef _IO
# endif
# include <stropts.h>
#endif

#ifndef SIOCGLIFCONF
#define SIOCGLIFCONF SIOCGIFCONF
#define lifc_len ifc_len
#define lifc_buf ifc_buf
#define lifc_req ifc_req
#define lifconf ifconf
#else
#define HAVE_LIFC_FAMILY 1
#define HAVE_LIFC_FLAGS 1
#endif
#ifndef SIOCGLIFFLAGS
#define SIOCGLIFFLAGS SIOCGIFFLAGS
#define SIOCGLIFDSTADDR SIOCGIFDSTADDR
#define SIOCGLIFBRDADDR SIOCGIFBRDADDR
#define SIOCGLIFNETMASK SIOCGIFNETMASK
#define lifr_addr ifr_addr
#define lifr_name ifr_name
#define lifr_dstaddr ifr_dstaddr
#define lifr_broadaddr ifr_broadaddr
#define lifr_flags ifr_flags
#ifndef zz_family
#define zz_family sa_family
#endif
#define lifreq ifreq
#else
#define zz_family ss_family
#endif

/*
 * We do asynchronous input using the SIGIO facility.  A number of
 * recvbuf buffers are preallocated for input.	In the signal
 * handler we poll to see which sockets are ready and read the
 * packets from them into the recvbuf's along with a time stamp and
 * an indication of the source host and the interface it was received
 * through.  This allows us to get as accurate receive time stamps
 * as possible independent of other processing going on.
 *
 * We watch the number of recvbufs available to the signal handler
 * and allocate more when this number drops below the low water
 * mark.  If the signal handler should run out of buffers in the
 * interim it will drop incoming frames, the idea being that it is
 * better to drop a packet than to be inaccurate.
 */


/*
 * Other statistics of possible interest
 */
volatile u_long packets_dropped;	/* total number of packets dropped on reception */
volatile u_long packets_ignored;	/* packets received on wild card interface */
volatile u_long packets_received;	/* total number of packets received */
u_long packets_sent;	/* total number of packets sent */
u_long packets_notsent; /* total number of packets which couldn't be sent */

volatile u_long handler_calls;	/* number of calls to interrupt handler */
volatile u_long handler_pkts;	/* number of pkts received by handler */
u_long io_timereset;		/* time counters were reset */

/*
 * Interface stuff
 */
struct interface *any_interface;	/* default ipv4 interface */
struct interface *any6_interface;	/* default ipv6 interface */
struct interface *loopback_interface;	/* loopback interface */
struct interface inter_list[MAXINTERFACES];
int ninterfaces;

#ifdef REFCLOCK
/*
 * Refclock stuff.	We keep a chain of structures with data concerning
 * the guys we are doing I/O for.
 */
static	struct refclockio *refio;
#endif /* REFCLOCK */

/*
 * File descriptor masks etc. for call to select
 */
fd_set activefds;
int maxactivefd;

static	int create_sockets	P((u_int));
static	SOCKET	open_socket	P((struct sockaddr_storage *, int, int));
static	void	close_socket	P((SOCKET));
static	void	close_file	P((int));
static	char *	fdbits		P((int, fd_set *));
static	void	set_reuseaddr	P((int));

/*
 * init_io - initialize I/O data structures and call socket creation routine
 */
void
init_io(void)
{
#ifdef SYS_WINNT
	WORD wVersionRequested;
	WSADATA wsaData;
	init_transmitbuff();
#endif /* SYS_WINNT */

	/*
	 * Init buffer free list and stat counters
	 */
	init_recvbuff(RECV_INIT);

	packets_dropped = packets_received = 0;
	packets_ignored = 0;
	packets_sent = packets_notsent = 0;
	handler_calls = handler_pkts = 0;
	io_timereset = 0;
	loopback_interface = 0;

#ifdef REFCLOCK
	refio = 0;
#endif

#if defined(HAVE_SIGNALED_IO)
	(void) set_signal();
#endif

#ifdef SYS_WINNT
	wVersionRequested = MAKEWORD(1,1);
	if (WSAStartup(wVersionRequested, &wsaData))
	{
		msyslog(LOG_ERR, "No useable winsock.dll: %m");
		exit(1);
	}
#endif /* SYS_WINNT */

	/*
	 * Create the sockets
	 */
	BLOCKIO();
	(void) create_sockets(htons(NTP_PORT));
	UNBLOCKIO();

#ifdef DEBUG
	if (debug)
	    printf("init_io: maxactivefd %d\n", maxactivefd);
#endif
}

/*
 * create_sockets - create a socket for each interface plus a default
 *			socket for when we don't know where to send
 */
static int
create_sockets(
	u_int port
	)
{
	int ifstidx;				/* interface start index */
#if _BSDI_VERSION >= 199510
	int i, j;
	struct ifaddrs *ifaddrs, *ifap;
	struct sockaddr_storage resmask;
#if 	_BSDI_VERSION < 199701
	struct ifaddrs *lp;
	int num_if;
#endif
#else	/* _BSDI_VERSION >= 199510 */
# ifdef STREAMS_TLI
	struct strioctl ioc;
# endif /* STREAMS_TLI */
	char	buf[MAXINTERFACES*sizeof(struct lifreq)];
	struct	lifconf	lifc;
	struct	lifreq	lifreq, *lifr;
	SOCKET	vs;
	int af, n, i, j, len, size = 0;
	struct	sockaddr_storage resmask;
#endif	/* _BSDI_VERSION >= 199510 */

#ifdef DEBUG
	if (debug)
	    printf("create_sockets(%d)\n", ntohs( (u_short) port));
#endif

	ifstidx = 0;
	/*
	 * create pseudo-interface with wildcard IPv4 address
	 */
	((struct sockaddr_in*)&inter_list[0].sin)->sin_addr.s_addr = htonl(INADDR_ANY);
	((struct sockaddr_in*)&inter_list[0].sin)->sin_port = (u_short) port;
	inter_list[0].sin.ss_family = AF_INET;
	(void) strncpy(inter_list[0].name, "wildcard", sizeof(inter_list[0].name));
	inter_list[0].mask.ss_family = AF_INET;
	((struct sockaddr_in*)&inter_list[0].mask)->sin_addr.s_addr = htonl(~(u_int32)0);
	inter_list[0].received = 0;
	inter_list[0].sent = 0;
	inter_list[0].notsent = 0;
	inter_list[0].flags = INT_BROADCAST;
	any_interface = &inter_list[0];
	ifstidx++;

#ifdef HAVE_IPV6
	/*
	 * create pseudo-interface with wildcard IPv6 address
	 */
	inter_list[1].sin.ss_family = AF_INET6;
	((struct sockaddr_in6*)&inter_list[1].sin)->sin6_addr = in6addr_any;
	((struct sockaddr_in6*)&inter_list[1].sin)->sin6_port = (u_short) port;
	(void) strncpy(inter_list[1].name, "wildcard", sizeof(inter_list[1].name));
	memset(&((struct sockaddr_in6*)&inter_list[1].mask)->sin6_addr.s6_addr, 0xff, sizeof(struct in6_addr));
	inter_list[1].mask.ss_family = AF_INET6;
	inter_list[1].received = 0;
	inter_list[1].sent = 0;
	inter_list[1].notsent = 0;
	inter_list[1].flags = 0;
	any6_interface = &inter_list[1];
	ifstidx++;
#endif

#if _BSDI_VERSION >= 199510
	i = ifstidx;
#if 	_BSDI_VERSION >= 199701
	if (getifaddrs(&ifaddrs) < 0)
	{
		msyslog(LOG_ERR, "getifaddrs: %m");
		exit(1);
	}

	for (ifap = ifaddrs; ifap != NULL; ifap = ifap->ifa_next)
#else
	    if (getifaddrs(&ifaddrs, &num_if) < 0)
	    {
		    msyslog(LOG_ERR, "create_sockets: getifaddrs() failed: %m");
		    exit(1);
	    }

	for (ifap = ifaddrs, lp = ifap + num_if; ifap < lp; ifap++)
#endif
	{
		if (ifap->ifa_addr == NULL)
		    continue;

		if ((ifap->ifa_flags & IFF_UP) == 0)
		    continue;

		if (debug)
			printf("after getifaddrs(), considering %s (%s)\n",
			       ifap->ifa_name,
			       stoa((struct sockaddr_storage *)ifap->ifa_addr));

		if ((ifap->ifa_addr->sa_family == AF_INET) {
			if (ifap->ifa_flags & IFF_LOOPBACK) {
				if (ntohl(((struct sockaddr_in*)ifap->ifa_addr)->sin_addr.s_addr) != 0x7f000001 &&
				    !listen_to_virtual_ips)
					continue;
			}
			inter_list[i].flags = 0;
			if (ifap->ifa_flags & IFF_BROADCAST)
				inter_list[i].flags |= INT_BROADCAST;
			strcpy(inter_list[i].name, ifap->ifa_name);
			memcpy(&inter_list[i].sin, ifap->ifa_addr, SOCKLEN(ifap->ifa_addr));
			((struct sockaddr_in*)&inter_list[i].sin)->sin_port = port;
			if (ifap->ifa_flags & IFF_LOOPBACK) {
				inter_list[i].flags = INT_LOOPBACK;
				if (loopback_interface == NULL
				|| ntohl(sin->sin_addr.s_addr) != 0x7f000001)
				loopback_interface = &inter_list[i];
			}
			if (inter_list[i].flags & INT_BROADCAST) {
				sin = (struct sockaddr_in *)ifap->ifa_broadaddr;
				inter_list[i].bcast = *sin;
				inter_list[i].bcast.sin_port = port;
			}
			if (ifap->ifa_flags & (IFF_LOOPBACK|IFF_POINTOPOINT)) {
				inter_list[i].mask.ss_family = AF_INET;
				((struct sockaddr_in*)&inter_list[i].mask)->sin_addr.s_addr = 0xffffffff;
			} else {
				inter_list[i].mask = *(struct sockaddr_storage*)ifap->ifa_netmask;
			}

			/*
			* look for an already existing source interface
			* address.  If the machine has multiple point to
			* point interfaces, then the local address may
			* appear more than once.
			*
			* A second problem exists if we have two addresses on
			* the same network (via "ifconfig alias ...").  Don't
			* make two xntp interfaces for the two aliases on the
			* one physical interface. -wsr
			*/
			for (j=0; j < i; j++)
			if (inter_list[j].sin.ss_family == inter_list[i].sin.ss_family)
				if ((((struct sockaddr_in*)&inter_list[j].sin)->sin_addr.s_addr &
					((struct sockaddr_in*)&inter_list[j].mask)->sin_addr.s_addr) ==
					(((struct sockaddr_in*)&inter_list[i].sin)->sin_addr.s_addr &
					((struct sockaddr_in*)&inter_list[i].mask)->sin_addr.s_addr))
			{
				if (inter_list[j].flags & INT_LOOPBACK)
					inter_list[j] = inter_list[i];
				break;
			}
			if (j == i)
			i++;
			if (i > MAXINTERFACES)
			break;
		}
		else if(ifap->ifa_addr->sa_family == AF_INET6) {
			if (ifap->ifa_flags & IFF_LOOPBACK) {
				if (IN6_IS_ADDR_LOOPBACK(&((struct sockaddr_in6*)ifap->ifa_addr)->sin6_addr))
						continue;
			}
			/* IPv6 hasn't broadcast sockets */
			inter_list[i].flags = 0;
			strcpy(inter_list[i].name, ifap->ifa_name);
			memcpy(&(inter_list[i].sin), ifap->ifa_addr, SOCKLEN(ifap->ifa_addr));
			((struct sockaddr_in6*)&inter_list[i].sin)->sin6_port = port;

			if (ifap->ifa_flags & IFF_LOOPBACK) {
				inter_list[i].flags = INT_LOOPBACK;
				if (loopback_interface == NULL
				|| IN6_IS_ADDR_LOOPBACK(&((struct sockaddr_in6*)sin)->sin6_addr))
				loopback_interface = &inter_list[i];
			}

			if (ifap->ifa_flags & (IFF_LOOPBACK|IFF_POINTOPOINT)) {
				memset(&((struct sockaddr_in6*)&inter_list[i].mask)->sin6_addr, 0xff, sizeof(struct in6_addr));
				inter_list[i].mask.ss_family = AF_INET6;
			} else {
				inter_list[i].mask = *(struct sockaddr_storage*)ifap->ifa_netmask;
				inter_list[i].mask.ss_family = AF_INET6;
			}

			/*
			* look for an already existing source interface
			* address.  If the machine has multiple point to
			* point interfaces, then the local address may
			* appear more than once.
			*
			* A second problem exists if we have two addresses on
			* the same network (via "ifconfig alias ...").  Don't
			* make two xntp interfaces for the two aliases on the
			* one physical interface. -wsr
			*/
			for (j=0; j < i; j++)
			if (SOCKCMP(netof(&inter_list[j].sin), netof(&inter_list[i].sin)))
			{
				if (inter_list[j].flags & INT_LOOPBACK)
					inter_list[j] = inter_list[i];
				break;
			}
			if (j == i)
			i++;
			if (i > MAXINTERFACES)
			break;
		}  else continue;
	}
	free(ifaddrs);
#else	/* _BSDI_VERSION >= 199510 */
	if (
		(vs = socket(AF_INET, SOCK_DGRAM, 0))
#  ifndef SYS_WINNT
		< 0
#  else /* SYS_WINNT */
		== INVALID_SOCKET
#  endif /* SYS_WINNT */
		) {
		msyslog(LOG_ERR, "create_sockets: socket(AF_INET, SOCK_DGRAM) failed: %m");
		exit(1);
	}

	i = ifstidx;
# if !defined(SYS_WINNT)
	lifc.lifc_len = sizeof(buf);
# endif
# ifdef STREAMS_TLI
	ioc.ic_cmd = SIOCGLIFCONF;
	ioc.ic_timout = 0;
	ioc.ic_dp = (caddr_t)buf;
	ioc.ic_len = sizeof(buf);
	if(ioctl(vs, I_STR, &ioc) < 0 ||
	   ioc.ic_len < sizeof(struct lifreq))
	{
		msyslog(LOG_ERR, "create_sockets: ioctl(I_STR:SIOCGLIFCONF) failed: %m - exiting");
		exit(1);
	}
#  ifdef SIZE_RETURNED_IN_BUFFER
	lifc.lifc_len = ioc.ic_len - sizeof(int);
	lifc.lifc_buf = buf + sizeof(int);
#  else /* not SIZE_RETURNED_IN_BUFFER */
	lifc.lifc_len = ioc.ic_len;
	lifc.lifc_buf = buf;
#  endif /* not SIZE_RETURNED_IN_BUFFER */

# else /* not STREAMS_TLI */
	/* The real struct lifconf has more elements than what we fill in. */
	memset(&lifc, 0, sizeof(lifc));
	lifc.lifc_len = sizeof(buf);
	lifc.lifc_buf = buf;
#  ifndef SYS_WINNT
	if (ioctl(vs, SIOCGLIFCONF, (char *)&lifc) < 0)
#  else
 	if (WSAIoctl(vs, SIO_GET_INTERFACE_LIST, 0, 0, lifc.lifc_buf, lifc.lifc_len, &lifc.lifc_len, 0, 0) == SOCKET_ERROR) 
#  endif /* SYS_WINNT */
{
		msyslog(LOG_ERR, "create_sockets: ioctl(SIOCGLIFCONF) failed: %m - exiting");
		exit(1);
}

# endif /* not STREAMS_TLI */

	for(n = lifc.lifc_len, lifr = lifc.lifc_req; n > 0;
	    lifr = (struct lifreq *)((char *)lifr + size))
	{
# ifdef HAVE_SA_LEN_IN_STRUCT_SOCKADDR
		len = max(sizeof(struct sockaddr), lifr->lifr_addr.sa_len);
# else
		if (lifr->lifr_addr.zz_family != AF_INET6)
			len = sizeof(struct sockaddr);
		else
			len = SOCKLEN(&lifr->lifr_addr);
# endif
		size = sizeof(*lifr);
#ifndef SYS_WINNT
		if (size < sizeof(lifr->lifr_name) + len)
			size = sizeof(lifr->lifr_name) + len;
#else
		if(size  < len)
			size = len;
#endif
		n -= size;

# if !defined(SYS_WINNT)
		/* Exclude logical interfaces (indicated by ':' in the interface name)	*/
		if (debug)
			printf("interface <%s> ", lifr->lifr_name);
		if ((listen_to_virtual_ips == 0)
		    && (strchr(lifr->lifr_name, (int)':') != NULL)) {
			if (debug)
			    printf("ignored\n");
			continue;
		}
		if (debug)
		    printf("OK\n");

		if (
#  ifdef VMS /* VMS+UCX */
			(((struct sockaddr *)&(lifr->lifr_addr))->sa_family != AF_INET &&
			 ((struct sockaddr *)&(lifr->lifr_addr))->sa_family != AF_INET6 )
#  else
			(lifr->lifr_addr.zz_family != AF_INET &&
			 lifr->lifr_addr.zz_family != AF_INET6 )
#  endif /* VMS+UCX */
			) {
			if (debug)
			    printf("ignoring %s - not AF_INET or AF_INET6\n",
				   lifr->lifr_name);
			continue;
		}
# endif /* SYS_WINNT */
		lifreq = *lifr;
		inter_list[i].flags = 0;

		af = lifr->lifr_addr.zz_family;
		closesocket(vs);

		vs = socket(af, SOCK_DGRAM, 0);
#  ifndef SYS_WINNT
		if (vs < 0)
#  else /* SYS_WINNT */
		if (vs == INVALID_SOCKET)
#  endif /* SYS_WINNT */
			{
			msyslog(LOG_ERR,
			    "create_sockets: socket(%s, SOCK_DGRAM) failed: %m",
			    af == AF_INET ? "AF_INET" : "AF_INET6");
			exit(1);
		}

		/* is it broadcast capable? */
# ifndef SYS_WINNT
#  ifdef STREAMS_TLI
		ioc.ic_cmd = SIOCGLIFFLAGS;
		ioc.ic_timout = 0;
		ioc.ic_dp = (caddr_t)&lifreq;
		ioc.ic_len = sizeof(struct lifreq);
		if(ioctl(vs, I_STR, &ioc)) {
			msyslog(LOG_ERR, "create_sockets: ioctl(I_STR:SIOCGLIFFLAGS) failed: %m");
			continue;
		}
#  else /* not STREAMS_TLI */

		if (ioctl(vs, SIOCGLIFFLAGS, (char *)&lifreq) < 0) {
			if (errno != ENXIO)
			    msyslog(LOG_ERR, "create_sockets: ioctl(SIOCGLIFFLAGS) failed: %m");
			continue;
		}
#  endif /* not STREAMS_TLI */
		if ((lifreq.lifr_flags & IFF_UP) == 0) {
			if (debug)
			    printf("ignoring %s - interface not UP\n",
				   lifr->lifr_name);
			continue;
		}
		inter_list[i].flags = 0;
		if ((lifreq.lifr_flags & IFF_BROADCAST) && lifr->lifr_addr.zz_family == AF_INET)
		    inter_list[i].flags |= INT_BROADCAST;
# endif /* not SYS_WINNT */
		switch (lifr->lifr_addr.zz_family) {
		case AF_INET :
			if (
#  if defined(IFF_LOCAL_LOOPBACK) /* defined(SYS_HPUX) && (SYS_HPUX < 8) */
				(lifreq.lifr_flags & IFF_LOCAL_LOOPBACK)
#  elif defined(IFF_LOOPBACK)
				(lifreq.lifr_flags & IFF_LOOPBACK)
#  else /* not IFF_LOCAL_LOOPBACK and not IFF_LOOPBACK */
			/* test against 127.0.0.1 (yuck!!) */
			((*(struct sockaddr_in *)&lifr->lifr_addr).sin_addr.s_addr == inet_addr("127.0.0.1"))
#  endif /* not IFF_LOCAL_LOOPBACK and not IFF_LOOPBACK */
				)
			{
#  ifndef SYS_WINNT
				inter_list[i].flags |= INT_LOOPBACK;
#  endif /* not SYS_WINNT */
				/*
				 * Prefer an IPv4 loopback address to make
				 * the billboard look nice.
				 */
				if (loopback_interface == 0 ||
				    loopback_interface->sin.ss_family ==
				    AF_INET6)
				{
					loopback_interface = &inter_list[i];
				}
			}
			break;

		case AF_INET6 :
	  		if (
#  if defined(IFF_LOCAL_LOOPBACK) /* defined(SYS_HPUX) && (SYS_HPUX < 8) */
        	                (lifreq.lifr_flags & IFF_LOCAL_LOOPBACK)
#  elif defined(IFF_LOOPBACK)
                	        (lifreq.lifr_flags & IFF_LOOPBACK)
#  else /* not IFF_LOCAL_LOOPBACK and not IFF_LOOPBACK */
	                        /* test against ::1 */
        	                (IN6_IS_ADDR_LOOPBACK(&((struct sockaddr_in6*)(&inter_list[i].sin)->sin6_addr)))
#  endif /* not IFF_LOCAL_LOOPBACK and not IFF_LOOPBACK */
	                        )
        	        {
#  ifndef SYS_WINNT
                	        inter_list[i].flags |= INT_LOOPBACK;
#  endif /* not SYS_WINNT */
	                        if (loopback_interface == 0)
        	                {
                	                loopback_interface = &inter_list[i];
                        	}
                	}
                	break;
               	default :
			msyslog(LOG_ERR, "create_sockets: loopback test failed on inter_list[i]");
		}

# if defined(SYS_WINNT)
		{int TODO_FillInTheNameWithSomeThingReasonble;}
# else
  		(void)strncpy(inter_list[i].name, lifreq.lifr_name,
  			      sizeof(inter_list[i].name));
# endif
		memcpy(&(inter_list[i].sin), &lifr->lifr_addr, SOCKLEN(&lifr->lifr_addr));
		((struct sockaddr_in*)&inter_list[i].sin)->sin_port = (u_short) port;
# if !defined SYS_WINNT && !defined SYS_CYGWIN32 /* no interface flags on NT */
		if (inter_list[i].flags & INT_BROADCAST) {
#  ifdef STREAMS_TLI
			ioc.ic_cmd = SIOCGLIFBRDADDR;
			ioc.ic_timout = 0;
			ioc.ic_dp = (caddr_t)&lifreq;
			ioc.ic_len = sizeof(struct lifreq);
			if(ioctl(vs, I_STR, &ioc)) {
				msyslog(LOG_ERR, "create_sockets: ioctl(I_STR:SIOCGLIFBRDADDR) failed: %m");
				exit(1);
			}
#  else /* not STREAMS_TLI */
			if (ioctl(vs, SIOCGLIFBRDADDR, (char *)&lifreq) < 0) {
				msyslog(LOG_ERR, "create_sockets: ioctl(SIOCGLIFBRDADDR) failed: %m");
				exit(1);
			}
#  endif /* not STREAMS_TLI */

#  ifndef lifr_broadaddr
			inter_list[i].bcast =
			    *(struct sockaddr_storage *)&lifreq.lifr_addr;
#  else
			inter_list[i].bcast =
			    *(struct sockaddr_storage *)&lifreq.lifr_broadaddr;
#  endif /* lifr_broadaddr */
			((struct sockaddr_in*)&inter_list[i].bcast)->sin_port = (u_short) port;
		}

		if (inter_list[i].sin.ss_family == AF_INET) {
#  ifdef STREAMS_TLI
			ioc.ic_cmd = SIOCGLIFNETMASK;
			ioc.ic_timout = 0;
			ioc.ic_dp = (caddr_t)&lifreq;
			ioc.ic_len = sizeof(struct lifreq);
			if(ioctl(vs, I_STR, &ioc)) {
				msyslog(LOG_ERR, "create_sockets: ioctl(I_STR:SIOCGLIFNETMASK) failed: %m");
				exit(1);
			}
#  else /* not STREAMS_TLI */
			if (ioctl(vs, SIOCGLIFNETMASK, (char *)&lifreq) < 0) {
				msyslog(LOG_ERR, "create_sockets: ioctl(SIOCGLIFNETMASK) failed: %m");
				exit(1);
			}
#  endif /* not STREAMS_TLI */
			inter_list[i].mask = *(struct sockaddr_storage *)&lifreq.lifr_addr;
		}
		inter_list[i].mask.ss_family = inter_list[i].sin.ss_family;
# else
		/* winnt here */
		inter_list[i].bcast	= *(struct sockaddr_storage*)&lifreq.lifr_broadaddr;
		inter_list[i].bcast.ss_family = inter_list[i].sin.ss_family;
		((struct sockaddr_in*)&inter_list[i].bcast)->sin_port = (u_short) port;
		inter_list[i].mask	= *(struct sockaddr_storage*)&ifreq.ifr_mask;
		inter_list[i].mask.ss_family = inter_list[i].sin.ss_family;
# endif /* not SYS_WINNT */

		/* correct the mask for ipv6 addresses */
		if (inter_list[i].mask.ss_family == AF_INET6) {
			memset(&((struct sockaddr_in6*)&inter_list[i].mask)->sin6_addr, 0xff, 8*sizeof(u_char));
			memset(&((struct sockaddr_in6*)&inter_list[i].mask)->sin6_addr.s6_addr[8], 0, 8*sizeof(u_char));
		}

		/*
		 * look for an already existing source interface address.  If
		 * the machine has multiple point to point interfaces, then
		 * the local address may appear more than once.
		 */
		for (j=0; j < i; j++)
		    if (SOCKCMP(&inter_list[j].sin, &inter_list[i].sin)) {
			    break;
		    }
		if (j == i)
		    i++;
		if (i > MAXINTERFACES)
		    break;
	}
	closesocket(vs);
#endif	/* _BSDI_VERSION >= 199510 */

	ninterfaces = i;
	maxactivefd = 0;
	FD_ZERO(&activefds);
	for (i = 0; i < ninterfaces; i++) {
		inter_list[i].fd = open_socket(&inter_list[i].sin,
		    inter_list[i].flags & INT_BROADCAST, 0);
	}

	/*
	 * Now that we have opened all the sockets, turn off the reuse
	 * flag for security.
	 */
	set_reuseaddr(0);

#if defined(MCAST)
	/*
	 * enable possible multicast reception on the broadcast socket
	 */
	inter_list[0].bcast.ss_family = AF_INET;
	((struct sockaddr_in*)&inter_list[0].bcast)->sin_port = (u_short) port;
	((struct sockaddr_in*)&inter_list[0].bcast)->sin_addr.s_addr = htonl(INADDR_ANY);
#endif /* MCAST */

	/*
	 * Blacklist all bound interface addresses
	 */
	for (i = ifstidx; i < ninterfaces; i++) {
		SET_HOSTMASK(&resmask, inter_list[i].sin.ss_family);
		hack_restrict(RESTRICT_FLAGS, &inter_list[i].sin, &resmask,
		    RESM_NTPONLY|RESM_INTERFACE, RES_IGNORE);
	}

	/*
	 * Calculate the address hash for each interface address.
	 */
	for (i = 0; i < ninterfaces; i++)
		inter_list[i].addr_refid = addr2refid(&inter_list[i].sin);

#ifdef DEBUG
	if (debug > 1) {
		printf("create_sockets: ninterfaces=%d\n", ninterfaces);
		for (i = 0; i < ninterfaces; i++) {
			printf("interface %d:  fd=%d,  bfd=%d,  name=%.8s,  flags=0x%x\n",
			       i,
			       inter_list[i].fd,
			       inter_list[i].bfd,
			       inter_list[i].name,
			       inter_list[i].flags);
			/* Leave these as three printf calls. */
			printf("              sin=%s",
			       stoa((&inter_list[i].sin)));
			if (inter_list[i].flags & INT_BROADCAST)
			    printf("  bcast=%s,",
				   stoa((&inter_list[i].bcast)));
			printf("  mask=%s\n",
			       stoa((&inter_list[i].mask)));
		}
	}
#endif
#if defined (HAVE_IO_COMPLETION_PORT)
	for (i = 0; i < ninterfaces; i++) {
		io_completion_port_add_socket(&inter_list[i]);
	}
#endif
	return ninterfaces;
}

/*
 * io_setbclient - open the broadcast client sockets
 */
void
io_setbclient(void)
{
	int i;

#ifdef OPEN_BCAST_SOCKET
	set_reuseaddr(1);
#endif

	for (i = 1; i < ninterfaces; i++) {
		if (!(inter_list[i].flags & INT_BROADCAST))
			continue;

		if (inter_list[i].flags & INT_BCASTOPEN)
			continue;

#ifdef	SYS_SOLARIS
		inter_list[i].bcast.sin_addr.s_addr = htonl(INADDR_ANY);
#endif
#ifdef OPEN_BCAST_SOCKET /* Was: !SYS_DOMAINOS && !SYS_LINUX */
		inter_list[i].bfd = open_socket(&inter_list[i].bcast,
		    INT_BROADCAST, 1);
		inter_list[i].flags |= INT_BCASTOPEN;
#endif
	}

#ifdef OPEN_BCAST_SOCKET
	set_reuseaddr(0);
#endif
}

/*
 * set_reuseaddr() - set/clear REUSEADDR on all sockets
 *		NB possible hole - should we be doing this on broadcast
 *			fd's also?
 */
static void
set_reuseaddr(int flag) {
	int i;

	for (i = 0; i < ninterfaces; i++) {
		/*
		 * if inter_list[ n ].fd  is -1, we might have a adapter
		 * configured but not present
		 */
		if ( inter_list[ i ].fd != -1 ) {
			if (setsockopt(inter_list[i].fd, SOL_SOCKET,
				       SO_REUSEADDR, (char *)&flag,
				       sizeof(flag))) {
				msyslog(LOG_ERR, "create_sockets: setsockopt(SO_REUSEADDR,%s) failed: %m", flag ? "on" : "off");
			}
		}
	}
}


/*
 * io_multicast_add() - add multicast group address
 */
void
io_multicast_add(
	struct sockaddr_storage addr
	)
{
#ifdef MCAST
	struct ip_mreq mreq;
	int i = ninterfaces;	/* Use the next interface */
	u_int32 haddr = ntohl(((struct sockaddr_in*)&addr)->sin_addr.s_addr);
	struct in_addr iaddr;
	int s;
	struct sockaddr_in *sinp;

#ifdef HAVE_IPV6
	struct ipv6_mreq mreq6;
	struct in6_addr iaddr6;
	struct sockaddr_in6 *sin6p;
#endif /* HAVE_IPV6 */

	switch (addr.ss_family)
	{
	case AF_INET :
		iaddr = (((struct sockaddr_in*)&addr)->sin_addr);
		if (!IN_CLASSD(haddr)) {
			msyslog(LOG_ERR,
			"multicast address %s not class D",
				inet_ntoa(iaddr));
			return;
		}
		for (i = 0; i < ninterfaces; i++) {
			 /* Be sure it's the correct family */
                        if (inter_list[i].sin.ss_family != AF_INET)
                                continue;
			/* Already have this address */
			if (SOCKCMP(&inter_list[i].sin, &addr))
				return;
			/* found a free slot */
			if (SOCKNUL(&inter_list[i].sin) &&
			inter_list[i].fd <= 0 && inter_list[i].bfd <= 0 &&
			inter_list[i].flags == 0)
			break;
		}
		sinp = (struct sockaddr_in*)&(inter_list[i].sin);
		memset((char *)&mreq, 0, sizeof(mreq));
		memset((char *)&inter_list[i], 0, sizeof inter_list[0]);
		sinp->sin_family = AF_INET;
		sinp->sin_addr = iaddr;
		sinp->sin_port = htons(123);

		/*
		* Try opening a socket for the specified class D address. This
		* works under SunOS 4.x, but not OSF1 .. :-(
		*/
		set_reuseaddr(1);
		s = open_socket((struct sockaddr_storage*)sinp, 0, 1);
		set_reuseaddr(0);
		if (s < 0) {
			memset((char *)&inter_list[i], 0, sizeof inter_list[0]);
			i = 0;
			/* HACK ! -- stuff in an address */
			inter_list[i].bcast = addr;
			msyslog(LOG_ERR,
			"...multicast address %s using wildcard socket",
			inet_ntoa(iaddr));
		} else {
			inter_list[i].fd = s;
			inter_list[i].bfd = INVALID_SOCKET;
			(void) strncpy(inter_list[i].name, "multicast",
			sizeof(inter_list[i].name));
			((struct sockaddr_in*)&inter_list[i].mask)->sin_addr.s_addr = htonl(~(u_int32)0);
		}

		/*
		* enable reception of multicast packets
		*/
		mreq.imr_multiaddr = iaddr;
		mreq.imr_interface.s_addr = htonl(INADDR_ANY);
		if (setsockopt(inter_list[i].fd, IPPROTO_IP, IP_ADD_MEMBERSHIP,
		(char *)&mreq, sizeof(mreq)) == -1)
			msyslog(LOG_ERR,
			"setsockopt IP_ADD_MEMBERSHIP fails: %m for %x / %x (%s)",
			mreq.imr_multiaddr.s_addr,
			mreq.imr_interface.s_addr, inet_ntoa(iaddr));
		inter_list[i].flags |= INT_MULTICAST;
		if (i >= ninterfaces)
			ninterfaces = i+1;

		break;

#ifdef HAVE_IPV6
	case AF_INET6 :

		iaddr6 = ((struct sockaddr_in6*)&addr)->sin6_addr;
		if (!IN6_IS_ADDR_MULTICAST(&iaddr6)) {
			msyslog(LOG_ERR,
			    "address %s not IPv6 multicast address",
				stoa(&addr));
			return;
		}
		for (i = 0; i < ninterfaces; i++) {
			/* Be sure it's the correct family */
			if(inter_list[i].sin.ss_family != AF_INET6)
				continue;
			/* Already have this address */
			if (SOCKCMP(&inter_list[i].sin, &addr))
				return;
			/* found a free slot */
			if (SOCKNUL(&inter_list[i].sin) &&
			    inter_list[i].fd <= 0 && inter_list[i].bfd <= 0 &&
			    inter_list[i].flags == 0)
			break;
		}
		sin6p = (struct sockaddr_in6*)&inter_list[i].sin;
		memset((char *)&mreq6, 0, sizeof(mreq6));
		memset((char *)&inter_list[i], 0, sizeof inter_list[0]);
		sin6p->sin6_family = AF_INET6;
		sin6p->sin6_addr = iaddr6;
		sin6p->sin6_port = htons(123);

		/*
		 * Try opening a socket for the specified class D address. This
		 * works under SunOS 4.x, but not OSF1 .. :-(
		 */
		set_reuseaddr(1);
		s = open_socket((struct sockaddr_storage*)sin6p, 0, 1);
		set_reuseaddr(0);
		if(s < 0){
			memset((char *)&inter_list[i], 0, sizeof inter_list[0]);
			i = 0;
			/* HACK ! -- stuff in an address */
			inter_list[i].bcast = addr;
			msyslog(LOG_ERR,
			 "...multicast address %s using wildcard socket",
			 stoa(&addr));
		} else {
			inter_list[i].fd = s;
			inter_list[i].bfd = INVALID_SOCKET;
			(void)strncpy(inter_list[i].name, "multicast",
			   sizeof(inter_list[i].name));
			memset(&(((struct sockaddr_in6*)&inter_list[i].mask)->sin6_addr), 1, sizeof(struct in6_addr));
		}

		/*
		 * enable reception of multicast packets
		 */
		mreq6.ipv6mr_multiaddr = iaddr6;
		mreq6.ipv6mr_interface = 0;
		if(setsockopt(inter_list[i].fd, IPPROTO_IPV6, IPV6_JOIN_GROUP,
		   (char *)&mreq6, sizeof(mreq6)) == -1)
			msyslog(LOG_ERR,
			 "setsockopt IPV6_JOIN_GROUP fails: %m on interface %d(%s)",
			 mreq6.ipv6mr_interface, stoa(&addr));
		inter_list[i].flags |= INT_MULTICAST;
		if(i >= ninterfaces)
			ninterfaces = i+1;

		break;
#endif /* HAVE_IPV6 */
	}

#ifdef DEBUG
	if (debug)
		printf("io_multicast_add %s\n", stoa(&addr));
#endif
#else /* MCAST */
	msyslog(LOG_ERR,
	    "cannot add multicast address %s as no MCAST support",
	    stoa(&addr));
#endif /* MCAST */
}

/*
 * io_unsetbclient - close the broadcast client sockets
 */
void
io_unsetbclient(void)
{
	int i;

	for (i = 1; i < ninterfaces; i++)
	{
		if (!(inter_list[i].flags & INT_BCASTOPEN))
		    continue;
		close_socket(inter_list[i].bfd);
		inter_list[i].bfd = INVALID_SOCKET;
		inter_list[i].flags &= ~INT_BCASTOPEN;
	}
}


/*
 * io_multicast_del() - delete multicast group address
 */
void
io_multicast_del(
	struct sockaddr_storage addr
	)
{
#ifdef MCAST
	int i;
	struct ip_mreq mreq;
	u_int32 haddr;
	struct sockaddr_in sinaddr;

#ifdef HAVE_IPV6
	struct ipv6_mreq mreq6;
	struct in6_addr haddr6;
	struct sockaddr_in6 sin6addr;
#endif /* HAVE_IPV6 */

	switch (addr.ss_family)
	{
	case AF_INET :

		haddr = ntohl(((struct sockaddr_in*)&addr)->sin_addr.s_addr);

		if (!IN_CLASSD(haddr))
		{
			sinaddr.sin_addr.s_addr = ((struct sockaddr_in*)&addr)->sin_addr.s_addr;
			msyslog(LOG_ERR,
				 "invalid multicast address %s", stoa(&addr));
			return;
		}

		/*
		* Disable reception of multicast packets
		*/
		mreq.imr_multiaddr = ((struct sockaddr_in*)&addr)->sin_addr;
		mreq.imr_interface.s_addr = htonl(INADDR_ANY);
		for (i = 0; i < ninterfaces; i++)
		{
			/* Be sure it's the correct family */
			if (inter_list[i].sin.ss_family != AF_INET)
				continue;
			if (!(inter_list[i].flags & INT_MULTICAST))
				continue;
			if (!(inter_list[i].fd < 0))
				continue;
			if (!SOCKCMP(&addr, &inter_list[i].sin))
				continue;
			if (i != 0)
			{
				/* we have an explicit fd, so we can close it */
				close_socket(inter_list[i].fd);
				memset((char *)&inter_list[i], 0, sizeof inter_list[0]);
				inter_list[i].fd = INVALID_SOCKET;
				inter_list[i].bfd = INVALID_SOCKET;
			}
			else
			{
				/* We are sharing "any address" port :-(  Don't close it! */
				if (setsockopt(inter_list[i].fd, IPPROTO_IP, IP_DROP_MEMBERSHIP,
					(char *)&mreq, sizeof(mreq)) == -1)
				msyslog(LOG_ERR, "setsockopt IP_DROP_MEMBERSHIP fails: %m");
				/* This is **WRONG** -- there may be others ! */
				/* There should be a count of users ... */
				inter_list[i].flags &= ~INT_MULTICAST;
			}
		}
		break;

#ifdef HAVE_IPV6
	case AF_INET6 :
		haddr6 = ((struct sockaddr_in6*)&addr)->sin6_addr;

		if (!IN6_IS_ADDR_MULTICAST(&haddr6))
		{
			sin6addr.sin6_addr = ((struct sockaddr_in6*)&addr)->sin6_addr;
			msyslog(LOG_ERR,
				"invalid multicast address %s", stoa(&addr));
			return;
		}

		/*
		* Disable reception of multicast packets
		*/
		mreq6.ipv6mr_multiaddr = ((struct sockaddr_in6*)&addr)->sin6_addr;
		mreq6.ipv6mr_interface = 0;
		for (i = 0; i < ninterfaces; i++)
		{
			/* Be sure it's the correct family */
			if (inter_list[i].sin.ss_family != AF_INET6)
				continue;
			if (!(inter_list[i].flags & INT_MULTICAST))
				continue;
			if (!(inter_list[i].fd < 0))
				continue;
			if (!SOCKCMP(&addr, &inter_list[i].sin))
				continue;
			if (i != 0)
			{
				/* we have an explicit fd, so we can close it */
				close_socket(inter_list[i].fd);
				memset((char *)&inter_list[i], 0, sizeof inter_list[0]);
				inter_list[i].fd = INVALID_SOCKET;
				inter_list[i].bfd = INVALID_SOCKET;
			}
			else
			{
				/* We are sharing "any address" port :-(  Don't close it! */
				if (setsockopt(inter_list[i].fd, IPPROTO_IPV6, IPV6_LEAVE_GROUP,
					(char *)&mreq6, sizeof(mreq6)) == -1)
				msyslog(LOG_ERR, "setsockopt IP_DROP_MEMBERSHIP fails: %m");
				/* This is **WRONG** -- there may be others ! */
				/* There should be a count of users ... */
				inter_list[i].flags &= ~INT_MULTICAST;
			}
		}
		break;
#endif /* HAVE_IPV6 */
	}/* switch */
#else /* not MCAST */
	msyslog(LOG_ERR, "this function requires multicast kernel");
#endif /* not MCAST */
}


/*
 * open_socket - open a socket, returning the file descriptor
 */

static SOCKET
open_socket(
	struct sockaddr_storage *addr,
	int flags,
	int turn_off_reuse
	)
{
#ifdef SYS_WINNT
	int errval;
#endif
	SOCKET fd;
	int on = 1, off = 0;
#if defined(IPTOS_LOWDELAY) && defined(IPPROTO_IP) && defined(IP_TOS)
	int tos;
#endif /* IPTOS_LOWDELAY && IPPROTO_IP && IP_TOS */

#ifndef HAVE_IPV6
	if (addr->ss_family == AF_INET6)
		return (INVALID_SOCKET);
#endif /* HAVE_IPV6 */
	/* create a datagram (UDP) socket */
#ifndef SYS_WINNT
	if (  (fd = socket(addr->ss_family, SOCK_DGRAM, 0)) < 0) {
		if(addr->ss_family == AF_INET)
			msyslog(LOG_ERR, "socket(AF_INET, SOCK_DGRAM, 0) failed: %m");
		else if(addr->ss_family == AF_INET6)
			msyslog(LOG_ERR, "socket(AF_INET6, SOCK_DGRAM, 0) failed: %m");
		if (errno == EPROTONOSUPPORT || errno == EAFNOSUPPORT ||
		    errno == EPFNOSUPPORT)
			return (INVALID_SOCKET);
		exit(1);
		/*NOTREACHED*/
	}
#else
	if (  (fd = socket(addr->ss_family, SOCK_DGRAM, 0)) == INVALID_SOCKET) {
		errval = WSAGetLastError();
		if(addr->ss_family == AF_INET)
			msyslog(LOG_ERR, "socket(AF_INET, SOCK_DGRAM, 0) failed: %m");
		else if(addr->ss_family == AF_INET6)
			msyslog(LOG_ERR, "socket(AF_INET6, SOCK_DGRAM, 0) failed: %m");
		if (errno == WSAEPROTONOSUPPORT || errno == WSAEAFNOSUPPORT ||
		    errno == WSAEPFNOSUPPORT)
			return (INVALID_SOCKET);
		exit(1);
		/*NOTREACHED*/
	}
#endif /* SYS_WINNT */

	/* set SO_REUSEADDR since we will be binding the same port
	   number on each interface */
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
		       (char *)&on, sizeof(on)))
	{
		msyslog(LOG_ERR, "setsockopt SO_REUSEADDR on fails: %m");
	}

#if defined(IPTOS_LOWDELAY) && defined(IPPROTO_IP) && defined(IP_TOS)
	/* set IP_TOS to minimize packet delay */
	tos = IPTOS_LOWDELAY;
	if (addr->ss_family == AF_INET)
		if (setsockopt(fd, IPPROTO_IP, IP_TOS, (char *) &tos, sizeof(tos)) < 0)
		{
			msyslog(LOG_ERR, "setsockopt IPTOS_LOWDELAY on fails: %m");
		}

#if defined(IPV6_V6ONLY)
        if (addr->ss_family == AF_INET6)
                if (setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY,
                	(char*)&on, sizeof(on)))
                {
                	msyslog(LOG_ERR, "setsockopt IPV6_V6ONLY on fails: %m");
		}
#else /* IPV6_V6ONLY */
#if defined(IPV6_BINDV6ONLY)
        if (addr->ss_family == AF_INET6)
                if (setsockopt(fd, IPPROTO_IPV6, IPV6_BINDV6ONLY,
                	(char*)&on, sizeof(on)))
                {
                	msyslog(LOG_ERR,
			    "setsockopt IPV6_BINDV6ONLY on fails: %m");
		}
#endif /* IPV6_BINDV6ONLY */
#endif /* IPV6_V6ONLY */

#endif /* IPTOS_LOWDELAY && IPPROTO_IP && IP_TOS */

	/*
	 * bind the local address.
	 */
	if (bind(fd, (struct sockaddr *)addr, SOCKLEN(addr)) < 0) {
		char buff[160];

		if(addr->ss_family == AF_INET)
			sprintf(buff,
				"bind() fd %d, family %d, port %d, addr %s, in_classd=%d flags=%d fails: %%m",
				fd, addr->ss_family, (int)ntohs(((struct sockaddr_in*)addr)->sin_port),
				stoa(addr),
				IN_CLASSD(ntohl(((struct sockaddr_in*)addr)->sin_addr.s_addr)), flags);
		else if(addr->ss_family == AF_INET6)
		                sprintf(buff,
                                "bind() fd %d, family %d, port %d, addr %s, in6_is_addr_multicast=%d flags=%d fails: %%m",
                                fd, addr->ss_family, (int)ntohs(((struct sockaddr_in6*)addr)->sin6_port),
                                stoa(addr),
                                IN6_IS_ADDR_MULTICAST(&((struct sockaddr_in6*)addr)->sin6_addr), flags);
		else return INVALID_SOCKET;

		msyslog(LOG_ERR, buff);
		closesocket(fd);

		/*
		 * soft fail if opening a multicast address
		 */
 		if(addr->ss_family == AF_INET){
			if(IN_CLASSD(ntohl(((struct sockaddr_in*)addr)->sin_addr.s_addr)))
				return (INVALID_SOCKET);
		}
		else {
			if(IN6_IS_ADDR_MULTICAST(&((struct sockaddr_in6*)addr)->sin6_addr))
				return (INVALID_SOCKET);
		}
#if 0
		exit(1);
#else
		return INVALID_SOCKET;
#endif
	}
#ifdef DEBUG
	if (debug)
	    printf("bind() fd %d, family %d, port %d, addr %s, flags=%d\n",
		   fd,
		   addr->ss_family,
		   (int)ntohs(((struct sockaddr_in*)addr)->sin_port),
		   stoa(addr),
		   flags);
#endif
	if (fd > maxactivefd)
	    maxactivefd = fd;
	FD_SET(fd, &activefds);

	/*
	 * set non-blocking,
	 */

#ifdef USE_FIONBIO
	/* in vxWorks we use FIONBIO, but the others are defined for old systems, so
	 * all hell breaks loose if we leave them defined
	 */
#undef O_NONBLOCK
#undef FNDELAY
#undef O_NDELAY
#endif

#if defined(O_NONBLOCK) /* POSIX */
	if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0)
	{
		msyslog(LOG_ERR, "fcntl(O_NONBLOCK) fails: %m");
		exit(1);
		/*NOTREACHED*/
	}
#elif defined(FNDELAY)
	if (fcntl(fd, F_SETFL, FNDELAY) < 0)
	{
		msyslog(LOG_ERR, "fcntl(FNDELAY) fails: %m");
		exit(1);
		/*NOTREACHED*/
	}
#elif defined(O_NDELAY) /* generally the same as FNDELAY */
	if (fcntl(fd, F_SETFL, O_NDELAY) < 0)
	{
		msyslog(LOG_ERR, "fcntl(O_NDELAY) fails: %m");
		exit(1);
		/*NOTREACHED*/
	}
#elif defined(FIONBIO)
	if (
# if defined(VMS)
		(ioctl(fd,FIONBIO,&1) < 0)
# elif defined(SYS_WINNT)
		(ioctlsocket(fd,FIONBIO,(u_long *) &on) == SOCKET_ERROR)
# else
		(ioctl(fd,FIONBIO,&on) < 0)
# endif
	   )
	{
		msyslog(LOG_ERR, "ioctl(FIONBIO) fails: %m");
		exit(1);
		/*NOTREACHED*/
	}
#elif defined(FIOSNBIO)
	if (ioctl(fd,FIOSNBIO,&on) < 0)
	{
		msyslog(LOG_ERR, "ioctl(FIOSNBIO) fails: %m");
		exit(1);
		/*NOTREACHED*/
	}
#else
# include "Bletch: Need non-blocking I/O!"
#endif

#ifdef HAVE_SIGNALED_IO
	init_socket_sig(fd);
#endif /* not HAVE_SIGNALED_IO */

	/*
	 *	Turn off the SO_REUSEADDR socket option.  It apparently
	 *	causes heartburn on systems with multicast IP installed.
	 *	On normal systems it only gets looked at when the address
	 *	is being bound anyway..
	 */
	if (turn_off_reuse)
	    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
			   (char *)&off, sizeof(off)))
	    {
		    msyslog(LOG_ERR, "setsockopt SO_REUSEADDR off fails: %m");
	    }

#ifdef SO_BROADCAST
	/* if this interface can support broadcast, set SO_BROADCAST */
	if (flags & INT_BROADCAST)
	{
		if (setsockopt(fd, SOL_SOCKET, SO_BROADCAST,
			       (char *)&on, sizeof(on)))
		{
			msyslog(LOG_ERR, "setsockopt(SO_BROADCAST): %m");
		}
	}
#endif /* SO_BROADCAST */

#if !defined(SYS_WINNT) && !defined(VMS)
# ifdef DEBUG
	if (debug > 1)
	    printf("flags for fd %d: 0%o\n", fd,
		   fcntl(fd, F_GETFL, 0));
# endif
#endif /* SYS_WINNT || VMS */

	return fd;
}


/*
 * close_socket - close a socket and remove from the activefd list
 */
static void
close_socket(
	     SOCKET fd
	)
{
	SOCKET i, newmax;

	(void) closesocket(fd);
	FD_CLR( (u_int) fd, &activefds);

	if (fd == maxactivefd) {
		newmax = 0;
		for (i = 0; i < maxactivefd; i++)
			if (FD_ISSET(i, &activefds))
				newmax = i;
		maxactivefd = newmax;
	}
}


/*
 * close_file - close a file and remove from the activefd list
 * added 1/31/1997 Greg Schueman for Windows NT portability
 */
static void
close_file(
	int fd
	)
{
	int i, newmax;

	(void) close(fd);
	FD_CLR( (u_int) fd, &activefds);

	if (fd == maxactivefd) {
		newmax = 0;
		for (i = 0; i < maxactivefd; i++)
			if (FD_ISSET(i, &activefds))
				newmax = i;
		maxactivefd = newmax;
	}
}


/* XXX ELIMINATE sendpkt similar in ntpq.c, ntpdc.c, ntp_io.c, ntptrace.c */
/*
 * sendpkt - send a packet to the specified destination. Maintain a
 * send error cache so that only the first consecutive error for a
 * destination is logged.
 */
void
sendpkt(
	struct sockaddr_storage *dest,
	struct interface *inter,
	int ttl,
	struct pkt *pkt,
	int len
	)
{
	int cc, slot;
#ifdef SYS_WINNT
	DWORD err;
#endif /* SYS_WINNT */

	/*
	 * Send error caches. Empty slots have port == 0
	 * Set ERRORCACHESIZE to 0 to disable
	 */
	struct cache {
		u_short port;
		struct	in_addr addr;
	};

#ifdef HAVE_IPV6
	struct cache6 {
		u_short port;
		struct in6_addr addr;
	};
#endif /* HAVE_IPV6 */

#ifndef ERRORCACHESIZE
#define ERRORCACHESIZE 8
#endif
#if ERRORCACHESIZE > 0
	static struct cache badaddrs[ERRORCACHESIZE];
#ifdef HAVE_IPV6
	static struct cache6 badaddrs6[ERRORCACHESIZE];
#endif /* HAVE_IPV6 */
#else
#define badaddrs ((struct cache *)0)		/* Only used in empty loops! */
#ifdef HAVE_IPV6
#define badaddrs6 ((struct cache6 *)0)		/* Only used in empty loops! */
#endif /* HAVE_IPV6 */
#endif
#ifdef DEBUG
	if (debug > 1)
	    printf("%ssendpkt(fd=%d dst=%s, src=%s, ttl=%d, len=%d)\n",
		   (ttl >= 0) ? "\tMCAST\t*****" : "",
		   inter->fd, stoa(dest),
		   stoa(&inter->sin), ttl, len);
#endif

#ifdef MCAST

	switch (inter->sin.ss_family) {

	case AF_INET :

		/*
		* for the moment we use the bcast option to set multicast ttl
		*/
		if (ttl > 0 && ttl != inter->last_ttl) {
			char mttl = (char) ttl;

			/*
			* set the multicast ttl for outgoing packets
			*/
			if (setsockopt(inter->fd, IPPROTO_IP, IP_MULTICAST_TTL,
				&mttl, sizeof(mttl)) != 0) {
				msyslog(LOG_ERR, "setsockopt IP_MULTICAST_TTL fails: %m");
			}
			else
   				inter->last_ttl = ttl;
		}
		break;

#ifdef HAVE_IPV6
	case AF_INET6 :

	 	/*
		 * for the moment we use the bcast option to set
		 * multicast max hops
		 */
        	if (ttl > 0 && ttl != inter->last_ttl) {

                	/*
                 	* set the multicast ttl for outgoing packets
                 	*/
                	if (setsockopt(inter->fd, IPPROTO_IPV6, IPV6_MULTICAST_HOPS,
                    	&ttl, sizeof(ttl)) == -1)
	                        msyslog(LOG_ERR, "setsockopt IP_MULTICAST_TTL fails: %m");
                	else
	                        inter->last_ttl = ttl;
	        }
	        break;
#endif /* HAVE_IPV6 */

	default :
		exit(1);

	}


#endif /* MCAST */

	for (slot = ERRORCACHESIZE; --slot >= 0; )
		if(dest->ss_family == AF_INET) {
			if (badaddrs[slot].port == ((struct sockaddr_in*)dest)->sin_port &&
				badaddrs[slot].addr.s_addr == ((struct sockaddr_in*)dest)->sin_addr.s_addr)
			break;
		}
#ifdef HAVE_IPV6
		else if (dest->ss_family == AF_INET6) {
			if (badaddrs6[slot].port == ((struct sockaddr_in6*)dest)->sin6_port &&
				badaddrs6[slot].addr.s6_addr == ((struct sockaddr_in6*)dest)->sin6_addr.s6_addr)
			break;
		}
#endif /* HAVE_IPV6 */
		else exit(1);  /* address family not supported yet */

#if defined(HAVE_IO_COMPLETION_PORT)
        err = io_completion_port_sendto(inter, pkt, len, dest);
	if (err != ERROR_SUCCESS)
#else
#ifdef SIM
        cc = srvr_rply(&ntp_node,  dest, inter, pkt);
#else /* SIM */
	cc = sendto(inter->fd, (char *)pkt, (unsigned int)len, 0, (struct sockaddr *)dest,
		    SOCKLEN(dest));
#endif /* SIM */
	if (cc == -1)
#endif
	{
		inter->notsent++;
		packets_notsent++;
#if defined(HAVE_IO_COMPLETION_PORT)
		if (err != WSAEWOULDBLOCK && err != WSAENOBUFS && slot < 0)
#else
		if (errno != EWOULDBLOCK && errno != ENOBUFS && slot < 0)
#endif
		{
			/*
			 * Remember this, if there's an empty slot
			 */
			switch (dest->ss_family) {

			case AF_INET :

				for (slot = ERRORCACHESIZE; --slot >= 0; )
					if (badaddrs[slot].port == 0)
					{
						badaddrs[slot].port = SRCPORT(dest);
						badaddrs[slot].addr = ((struct sockaddr_in*)dest)->sin_addr;
						break;
					}
				break;

#ifdef HAVE_IPV6
			case AF_INET6 :

				for (slot = ERRORCACHESIZE; --slot >= 0; )
        				if (badaddrs6[slot].port == 0)
            				{
                                    		badaddrs6[slot].port = SRCPORT(dest);
                                    		badaddrs6[slot].addr = ((struct sockaddr_in6*)dest)->sin6_addr;
                                    		break;
                            		}
                		break;
#endif /* HAVE_IPV6 */

			default :
				exit(1);
			}

			msyslog(LOG_ERR, "sendto(%s): %m", stoa(dest));
		}
	}
	else
	{
		inter->sent++;
		packets_sent++;
		/*
		 * He's not bad any more
		 */
		if (slot >= 0)
		{
			msyslog(LOG_INFO, "Connection re-established to %s", stoa(dest));
			switch (dest->ss_family) {
			case AF_INET :
				badaddrs[slot].port = 0;
				break;
#ifdef HAVE_IPV6
			case AF_INET6 :
				badaddrs6[slot].port = 0;
				break;
#endif /* HAVE_IPV6 */
			}
		}
	}
}

#if !defined(HAVE_IO_COMPLETION_PORT)
/*
 * fdbits - generate ascii representation of fd_set (FAU debug support)
 * HFDF format - highest fd first.
 */
static char *
fdbits(
	int count,
	fd_set *set
	)
{
	static char buffer[256];
	char * buf = buffer;

	count = (count < 256) ? count : 255;

	while (count >= 0)
	{
		*buf++ = FD_ISSET(count, set) ? '#' : '-';
		count--;
	}
	*buf = '\0';

	return buffer;
}

/*
 * input_handler - receive packets asynchronously
 */
void
input_handler(
	l_fp *cts
	)
{
	register int i, n;
	register struct recvbuf *rb;
	register int doing;
	register SOCKET fd;
	struct timeval tvzero;
	int fromlen;
	l_fp ts;			/* Timestamp at BOselect() gob */
	l_fp ts_e;			/* Timestamp at EOselect() gob */
	fd_set fds;
	int select_count = 0;
	static int handler_count = 0;

	++handler_count;
	if (handler_count != 1)
	    msyslog(LOG_ERR, "input_handler: handler_count is %d!", handler_count);
	handler_calls++;
	ts = *cts;

	for (;;)
	{
		/*
		 * Do a poll to see who has data
		 */

		fds = activefds;
		tvzero.tv_sec = tvzero.tv_usec = 0;

		/*
		 * If we have something to do, freeze a timestamp.
		 * See below for the other cases (nothing (left) to do or error)
		 */
		while (0 < (n = select(maxactivefd+1, &fds, (fd_set *)0, (fd_set *)0, &tvzero)))
		{
			++select_count;
			++handler_pkts;

#ifdef REFCLOCK
			/*
			 * Check out the reference clocks first, if any
			 */
			if (refio != 0)
			{
				register struct refclockio *rp;

				for (rp = refio; rp != 0 && n > 0; rp = rp->next)
				{
					fd = rp->fd;
					if (FD_ISSET(fd, &fds))
					{
						n--;
						if (free_recvbuffs() == 0)
						{
							char buf[RX_BUFF_SIZE];

							(void) read(fd, buf, sizeof buf);
							packets_dropped++;
							goto select_again;
						}

						rb = get_free_recv_buffer();

						i = (rp->datalen == 0
						     || rp->datalen > sizeof(rb->recv_space))
						    ? sizeof(rb->recv_space) : rp->datalen;
						rb->recv_length =
						    read(fd, (char *)&rb->recv_space, (unsigned)i);

						if (rb->recv_length == -1)
						{
							msyslog(LOG_ERR, "clock read fd %d: %m", fd);
							freerecvbuf(rb);
							goto select_again;
						}

						/*
						 * Got one.  Mark how
						 * and when it got here,
						 * put it on the full
						 * list and do
						 * bookkeeping.
						 */
						rb->recv_srcclock = rp->srcclock;
						rb->dstadr = 0;
						rb->fd = fd;
						rb->recv_time = ts;
						rb->receiver = rp->clock_recv;

						if (rp->io_input)
						{
							/*
							 * have direct
							 * input routine
							 * for refclocks
							 */
							if (rp->io_input(rb) == 0)
							{
								/*
								 * data
								 * was
								 * consumed
								 * -
								 * nothing
								 * to
								 * pass
								 * up
								 * into
								 * block
								 * input
								 * machine
								 */
								freerecvbuf(rb);
#if 1
								goto select_again;
#else
								continue;
#endif
							}
						}

						add_full_recv_buffer(rb);

						rp->recvcount++;
						packets_received++;
					}
				}
			}
#endif /* REFCLOCK */

			/*
			 * Loop through the interfaces looking for data
			 * to read.
			 */
			for (i = ninterfaces - 1; (i >= 0) && (n > 0); i--)
			{
				for (doing = 0; (doing < 2) && (n > 0); doing++)
				{
					if (doing == 0)
					{
						fd = inter_list[i].fd;
					}
					else
					{
						if (!(inter_list[i].flags & INT_BCASTOPEN))
						    break;
						fd = inter_list[i].bfd;
					}
					if (fd < 0) continue;
					if (FD_ISSET(fd, &fds))
					{
						n--;

						/*
						 * Get a buffer and read
						 * the frame.  If we
						 * haven't got a buffer,
						 * or this is received
						 * on the wild card
						 * socket, just dump the
						 * packet.
						 */
						if (
#ifdef UDP_WILDCARD_DELIVERY
				/*
				 * these guys manage to put properly addressed
				 * packets into the wildcard queue
				 */
							(free_recvbuffs() == 0)
#else
							((i == 0) || (free_recvbuffs() == 0))
#endif
							)
	{
		char buf[RX_BUFF_SIZE];
		struct sockaddr_storage from;

		fromlen = sizeof from;
		(void) recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr*)&from, &fromlen);
#ifdef DEBUG
		if (debug)
		    printf("%s on %d(%lu) fd=%d from %s\n",
			   (i) ? "drop" : "ignore",
			   i, free_recvbuffs(), fd,
			   stoa(&from));
#endif
		if (i == 0)
		    packets_ignored++;
		else
		    packets_dropped++;
		goto select_again;
	}

	rb = get_free_recv_buffer();

	fromlen = sizeof(struct sockaddr_storage);
	rb->recv_length = recvfrom(fd,
				   (char *)&rb->recv_space,
				   sizeof(rb->recv_space), 0,
				   (struct sockaddr *)&rb->recv_srcadr,
				   &fromlen);
	if (rb->recv_length == 0
#ifdef EWOULDBLOCK
		 || errno==EWOULDBLOCK
#endif
#ifdef EAGAIN
		 || errno==EAGAIN
#endif
		 ) {
		freerecvbuf(rb);
	    continue;
	}
	else if (rb->recv_length < 0)
	{
		msyslog(LOG_ERR, "recvfrom(%s) fd=%d: %m",
 			stoa(&rb->recv_srcadr), fd);
#ifdef DEBUG
		if (debug)
		    printf("input_handler: fd=%d dropped (bad recvfrom)\n", fd);
#endif
		freerecvbuf(rb);
		continue;
	}
#ifdef DEBUG
	if (debug > 2) {
		if(rb->recv_srcadr.ss_family == AF_INET)
			printf("input_handler: if=%d fd=%d length %d from %08lx %s\n",
		   		i, fd, rb->recv_length,
				(u_long)ntohl(((struct sockaddr_in*)&rb->recv_srcadr)->sin_addr.s_addr) &
				0x00000000ffffffff,
			   	stoa(&rb->recv_srcadr));
		else
			printf("input_handler: if=%d fd=%d length %d from %s\n",
				i, fd, rb->recv_length,
				stoa(&rb->recv_srcadr));
        }
#endif

	/*
	 * Got one.  Mark how and when it got here,
	 * put it on the full list and do bookkeeping.
	 */
	rb->dstadr = &inter_list[i];
	rb->fd = fd;
	rb->recv_time = ts;
	rb->receiver = receive;

	add_full_recv_buffer(rb);

	inter_list[i].received++;
	packets_received++;
	goto select_again;
					}
					/* Check more interfaces */
				}
			}
		select_again:;
			/*
			 * Done everything from that select.  Poll again.
			 */
		}

		/*
		 * If nothing more to do, try again.
		 * If nothing to do, just return.
		 * If an error occurred, complain and return.
		 */
		if (n == 0)
		{
			if (select_count == 0) /* We really had nothing to do */
			{
				if (debug)
				    msyslog(LOG_DEBUG, "input_handler: select() returned 0");
				--handler_count;
				return;
			}
			/* We've done our work */
			get_systime(&ts_e);
			/*
			 * (ts_e - ts) is the amount of time we spent
			 * processing this gob of file descriptors.  Log
			 * it.
			 */
			L_SUB(&ts_e, &ts);
			if (debug > 3)
			    msyslog(LOG_INFO, "input_handler: Processed a gob of fd's in %s msec", lfptoms(&ts_e, 6));

			/* just bail. */
			--handler_count;
			return;
		}
		else if (n == -1)
		{
			int err = errno;

			/*
			 * extended FAU debugging output
			 */
			msyslog(LOG_ERR, "select(%d, %s, 0L, 0L, &0.000000) error: %m",
				maxactivefd+1, fdbits(maxactivefd, &activefds));
			if (err == EBADF) {
				int j, b;

				fds = activefds;
				for (j = 0; j <= maxactivefd; j++)
				    if (
					    (FD_ISSET(j, &fds) && (read(j, &b, 0) == -1))
					    )
					msyslog(LOG_ERR, "Bad file descriptor %d", j);
			}
			--handler_count;
			return;
		}
	}
	msyslog(LOG_ERR, "input_handler: fell out of infinite for(;;) loop!");
	--handler_count;
	return;
}

#endif

/*
 * findinterface - find interface corresponding to address
 */
struct interface *
findinterface(
	struct sockaddr_storage *addr
	)
{
	int s, rtn, i;
	struct sockaddr_storage saddr;
	int saddrlen = SOCKLEN(addr);
	/*
	 * This is considerably hoke. We open a socket, connect to it
	 * and slap a getsockname() on it. If anything breaks, as it
	 * probably will in some j-random knockoff, we just return the
	 * wildcard interface.
	 */
	memset(&saddr, 0, sizeof(saddr));
	saddr.ss_family = addr->ss_family;
	if(addr->ss_family == AF_INET)
		memcpy(&((struct sockaddr_in*)&saddr)->sin_addr, &((struct sockaddr_in*)addr)->sin_addr, sizeof(struct in_addr));
	else if(addr->ss_family == AF_INET6)
		memcpy(&((struct sockaddr_in6*)&saddr)->sin6_addr, &((struct sockaddr_in6*)addr)->sin6_addr, sizeof(struct in6_addr));
	((struct sockaddr_in*)&saddr)->sin_port = htons(2000);
	s = socket(addr->ss_family, SOCK_DGRAM, 0);
#ifndef SYS_WINNT
	if (s < 0)
#else
	if (s == INVALID_SOCKET)
#endif
		return ANY_INTERFACE_CHOOSE(addr);

	rtn = connect(s, (struct sockaddr *)&saddr, SOCKLEN(&saddr));
#ifndef SYS_WINNT
	if (rtn < 0)
#else
	if (rtn != 0)
#endif
		return ANY_INTERFACE_CHOOSE(addr);

	rtn = getsockname(s, (struct sockaddr *)&saddr, &saddrlen);
#ifndef SYS_WINNT
	if (rtn < 0)
#else
	if (rtn != 0)
#endif
		return ANY_INTERFACE_CHOOSE(addr);

	close_socket(s);
	for (i = 1; i < ninterfaces; i++) {
		/*
		* First look if is the the correct family
		*/
		if(inter_list[i].sin.ss_family != saddr.ss_family)
	  		continue;
		/*
		 * We match the unicast address only.
		 */
		if (SOCKCMP(&inter_list[i].sin, &saddr))
			return (&inter_list[i]);
	}
	return ANY_INTERFACE_CHOOSE(addr);
}


/*
 * findbcastinter - find broadcast interface corresponding to address
 */
struct interface *
findbcastinter(
	struct sockaddr_storage *addr
	)
{
#if !defined(MPE) && (defined(SIOCGIFCONF) || defined(SYS_WINNT))
	register int i;

	for (i = 1; i < ninterfaces; i++) {
		/*
		* First look if is the the correct family
		*/
		if(inter_list[i].sin.ss_family != addr->ss_family)
	  		continue;
		/*
		 * We match only those interfaces marked as
		 * broadcastable and either the explicit broadcast
		 * address or the network portion of the IP address.
		 * Sloppy.
		 */
		if (!(inter_list[i].flags & INT_BROADCAST))
			continue;
		if(addr->ss_family == AF_INET) {
			if (SOCKCMP(&inter_list[i].bcast, addr))
				return (&inter_list[i]);
			if ((NSRCADR(&inter_list[i].sin) &
				NSRCADR(&inter_list[i].mask)) == (NSRCADR(addr) &
			    	NSRCADR(&inter_list[i].mask)))
				return (&inter_list[i]);
		}
		else if(addr->ss_family == AF_INET6) {
			if (SOCKCMP(&inter_list[i].bcast, addr))
				return (&inter_list[i]);
			if (SOCKCMP(netof(&inter_list[i].sin), netof(addr)))
				return (&inter_list[i]);
		     }
	}
#endif /* SIOCGIFCONF */
 	return ANY_INTERFACE_CHOOSE(addr);
}


/*
 * io_clr_stats - clear I/O module statistics
 */
void
io_clr_stats(void)
{
	packets_dropped = 0;
	packets_ignored = 0;
	packets_received = 0;
	packets_sent = 0;
	packets_notsent = 0;

	handler_calls = 0;
	handler_pkts = 0;
	io_timereset = current_time;
}


#ifdef REFCLOCK
/*
 * This is a hack so that I don't have to fool with these ioctls in the
 * pps driver ... we are already non-blocking and turn on SIGIO thru
 * another mechanisim
 */
int
io_addclock_simple(
	struct refclockio *rio
	)
{
	BLOCKIO();
	/*
	 * Stuff the I/O structure in the list and mark the descriptor
	 * in use.	There is a harmless (I hope) race condition here.
	 */
	rio->next = refio;
	refio = rio;

	if (rio->fd > maxactivefd)
	    maxactivefd = rio->fd;
	FD_SET(rio->fd, &activefds);
	UNBLOCKIO();
	return 1;
}

/*
 * io_addclock - add a reference clock to the list and arrange that we
 *				 get SIGIO interrupts from it.
 */
int
io_addclock(
	struct refclockio *rio
	)
{
	BLOCKIO();
	/*
	 * Stuff the I/O structure in the list and mark the descriptor
	 * in use.	There is a harmless (I hope) race condition here.
	 */
	rio->next = refio;
	refio = rio;

# ifdef HAVE_SIGNALED_IO
	if (init_clock_sig(rio))
	{
		refio = rio->next;
		UNBLOCKIO();
		return 0;
	}
# elif defined(HAVE_IO_COMPLETION_PORT)
	if (io_completion_port_add_clock_io(rio))
	{
		refio = rio->next;
		UNBLOCKIO();
		return 0;
	}
# endif

	if (rio->fd > maxactivefd)
	    maxactivefd = rio->fd;
	FD_SET(rio->fd, &activefds);

	UNBLOCKIO();
	return 1;
}

/*
 * io_closeclock - close the clock in the I/O structure given
 */
void
io_closeclock(
	struct refclockio *rio
	)
{
	/*
	 * Remove structure from the list
	 */
	if (refio == rio)
	{
		refio = rio->next;
	}
	else
	{
		register struct refclockio *rp;

		for (rp = refio; rp != 0; rp = rp->next)
		    if (rp->next == rio)
		    {
			    rp->next = rio->next;
			    break;
		    }

		if (rp == 0)
		{
			/*
			 * Internal error.	Report it.
			 */
			msyslog(LOG_ERR,
				"internal error: refclockio structure not found");
			return;
		}
	}

	/*
	 * Close the descriptor.
	 */
	close_file(rio->fd);
}
#endif	/* REFCLOCK */

void
kill_asyncio(void)
{
	SOCKET i;

	BLOCKIO();
	for (i = 0; i <= maxactivefd; i++)
	    (void)close_socket(i);
}
