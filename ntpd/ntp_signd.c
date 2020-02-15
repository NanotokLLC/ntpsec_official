/* Copyright Red Hat, Inc.
   Copyright Andrew Tridgell.
   Copyright the NTPsec Contributors
   Licensed under the same terms as NTP itself.
 */
#include "config.h"

#ifdef ENABLE_MSSNTP

#include "ntpd.h"
#include "ntp_io.h"
#include "ntp_stdlib.h"

#include <string.h>
#include <stdio.h>
#include <stddef.h>

#include <sys/un.h>

/* socket routines by tridge - from junkcode.samba.org */
/*
 * Dependency on NTP packet structure removed by ESR.
 * This code now only knows about the length of an NTP packet header,
 * not its content. Note that the signing technique never handled anything
 * but unextended and MACless packet headers, so it can't be used with NTS.
 */


/*
  connect to a unix domain socket
*/
static int
ux_socket_connect(const char *name)
{
	int fd;
	struct sockaddr_un addr;
	if (!name) {
		return -1;
	}

	ZERO(addr);
	addr.sun_family = AF_UNIX;
	strlcpy(addr.sun_path, name, sizeof(addr.sun_path));

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd == -1) {
		return -1;
	}

	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
		close(fd);
		return -1;
	}

	return fd;
}


/*
  keep writing until its all sent
*/
static int
write_all(int fd, const void *buf, size_t len)
{
	size_t total = 0;
	while (len) {
		int n = write(fd, buf, len);
		if (n <= 0) return total;
		buf = n + (const char *)buf;
		len -= (unsigned int)n;
		total += (unsigned int)n;
	}
	return total;
}

/*
  keep reading until its all read
*/
static int
read_all(int fd, void *buf, size_t len)
{
	size_t total = 0;
	while (len) {
		int n = read(fd, buf, len);
		if (n <= 0) return total;
		buf = n + (char *)buf;
		len -= (unsigned int)n;
		total += (unsigned int)n;
	}
	return total;
}

/*
  send a packet in length prefix format
*/
static int
send_packet(int fd, const char *buf, uint32_t len)
{
	uint32_t net_len = htonl(len);
	if (write_all(fd, &net_len, sizeof(net_len)) != sizeof(net_len)) return -1;
	if (write_all(fd, buf, len) != (int)len) return -1;
	return 0;
}

/*
  receive a packet in length prefix format
*/
static int
recv_packet(int fd, char **buf, uint32_t *len)
{
	if (read_all(fd, len, sizeof(*len)) != sizeof(*len)) return -1;
	*len = ntohl(*len);
	(*buf) = emalloc(*len);
	if (read_all(fd, *buf, *len) != (int)*len) {
		free(*buf);
		return -1;
	}
	return 0;
}

void
send_via_ntp_signd(
	struct recvbuf *rbufp,	/* receive packet pointer */
	int	xmode,
	keyid_t	xkeyid,
	int flags,
	void *xpkt
	)
{
	UNUSED_ARG(flags);
#ifndef DEBUG
	UNUSED_ARG(xmode);
#endif

	/* We are here because it was detected that the client
	 * sent an all-zero signature, and we therefore know
	 * it's windows trying to talk to an AD server
	 *
	 * Because we don't want to dive into Samba's secrets
	 * database just to find the long-term kerberos key
	 * that is re-used as the NTP key, we instead hand the
	 * packet over to Samba to sign, and return to us.
	 *
	 * The signing method Samba will use is described by
	 * Microsoft in MS-SNTP, found here:
	 * http://msdn.microsoft.com/en-us/library/cc212930.aspx
	 */

	int fd, sendlen;
	struct samba_key_in {
		uint32_t version;
		uint32_t op;
		uint32_t packet_id;
		uint32_t key_id_le;
		char pkt[LEN_PKT_NOMAC];
	} samba_pkt;

	struct samba_key_out {
		uint32_t version;
		uint32_t op;
		uint32_t packet_id;
		char pkt[LEN_PKT_NOMAC];
	} samba_reply;

	char full_socket[256];

	char *reply = NULL;
	uint32_t reply_len;

	ZERO(samba_pkt);
	samba_pkt.op = 0; /* Sign message */
	/* This will be echoed into the reply - a different
	 * implementation might want multiple packets
	 * awaiting signing */

	samba_pkt.packet_id = 1;

	/* Swap the byte order back - it's actually little
	 * endian on the wire, but it was read above as
	 * network byte order */
	samba_pkt.key_id_le = htonl(xkeyid);
	memcpy(&samba_pkt.pkt, xpkt, sizeof(samba_pkt.pkt));

	snprintf(full_socket, sizeof(full_socket), "%s/socket", ntp_signd_socket);

	fd = ux_socket_connect(full_socket);
	/* Only continue with this if we can talk to Samba */
	if (fd != -1) {
		/* Send old packet to Samba, expect response */
		/* Packet to Samba is quite simple:
		   All values BIG endian except key ID as noted
		   [packet size as BE] - 4 bytes
		   [protocol version (0)] - 4 bytes
		   [packet ID] - 4 bytes
		   [operation (sign message=0)] - 4 bytes
		   [key id] - LITTLE endian (as on wire) - 4 bytes
		   [message to sign] - as marshalled, without signature
		*/

		if (send_packet(fd, (char *)&samba_pkt, offsetof(struct samba_key_in, pkt) + LEN_PKT_NOMAC) != 0) {
			/* Huh?  could not talk to Samba... */
			close(fd);
			return;
		}

		if (recv_packet(fd, &reply, &reply_len) != 0) {
			close(fd);
			return;
		}
		/* Return packet is also simple:
		   [packet size] - network byte order - 4 bytes
		   [protocol version (0)] network byte order - - 4 bytes
		   [operation (signed success=3, failure=4)] network byte order - - 4 byte
		   (optional) [signed message] - as provided before, with signature appended
		*/

		if (reply_len <= sizeof(samba_reply)) {
			memcpy(&samba_reply, reply, reply_len);
			if (ntohl(samba_reply.op) == 3 && reply_len >  offsetof(struct samba_key_out, pkt)) {
				sendlen = reply_len - offsetof(struct samba_key_out, pkt);
				xpkt = &samba_reply.pkt;
				sendpkt(&rbufp->recv_srcadr, rbufp->dstadr, xpkt, sendlen);
				DPRINT(1, ("transmit ntp_signd packet: at %u %s->%s mode %d keyid %08x len %d\n",
					   current_time, socktoa(&rbufp->dstadr->sin),
					   socktoa(&rbufp->recv_srcadr), xmode, xkeyid, sendlen));
			}
		}

		if (reply) {
			free(reply);
		}
		close(fd);

	}
}
#endif
