/*-
 * Copyright (c) 1985, 1988, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 * -
 * Portions Copyright (c) 1993 by Digital Equipment Corporation.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies, and that
 * the name of Digital Equipment Corporation not be used in advertising or
 * publicity pertaining to distribution of the document or software without
 * specific, written prior permission.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND DIGITAL EQUIPMENT CORP. DISCLAIMS ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS.   IN NO EVENT SHALL DIGITAL EQUIPMENT
 * CORPORATION BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
 * PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS
 * ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS
 * SOFTWARE.
 * -
 * --Copyright--
 */
/* Portions Copyright (c) 1993 Carlos Leandro and Rui Salgueiro
 *	Dep. Matematica Universidade de Coimbra, Portugal, Europe
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 */

#if defined(LIBC_SCCS) && !defined(lint)
static char sccsid[] = "@(#)gethostnamadr.c	8.1 (Berkeley) 6/4/93";
#endif /* LIBC_SCCS and not lint */
#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/param.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <arpa/nameser.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <netdb.h>
#include <resolv.h>
#include <ctype.h>
#include <string.h>
#include <unistd.h>
#include <syslog.h>
#include <stdarg.h>
#include <nsswitch.h>

#include "netdb_private.h"
#include "res_config.h"

extern int h_errno;

#define BYADDR 0
#define BYNAME 1

#define MAXPACKET	(64*1024)

typedef union {
	HEADER	hdr;
	u_char	buf[MAXPACKET];
} querybuf;

typedef union {
	long	al;
	char	ac;
} align;

/*
 * Reverse the order of first four dotted entries of in.
 * Out must contain space for at least strlen(in) characters.
 * The result does not include any leading 0s of in.
 */
static void
ipreverse(char *in, char *out)
{
	char *pos[4];
	int len[4];
	char *p, *start;
	int i = 0;
	int leading = 1;

	/* Fill-in element positions and lengths: pos[], len[]. */
	start = p = in;
	for (;;) {
		if (*p == '.' || *p == '\0') {
			/* Leading 0? */
			if (leading && p - start == 1 && *start == '0')
				len[i] = 0;
			else {
				len[i] = p - start;
				leading = 0;
			}
			pos[i] = start;
			start = p + 1;
			i++;
		}
		if (i == 4)
			break;
		if (*p == 0) {
			for (; i < 4; i++) {
				pos[i] = p;
				len[i] = 0;
			}
			break;
		}
		p++;
	}

	/* Copy the entries in reverse order */
	p = out;
	leading = 1;
	for (i = 3; i >= 0; i--) {
		memcpy(p, pos[i], len[i]);
		if (len[i])
			leading = 0;
		p += len[i];
		/* Need a . separator? */
		if (!leading && i > 0 && len[i - 1])
			*p++ = '.';
	}
	*p = '\0';
}

static int
getnetanswer(querybuf *answer, int anslen, int net_i, struct netent *ne,
    struct netent_data *ned)
{

	HEADER *hp;
	u_char *cp;
	int n;
	u_char *eom;
	int type, class, ancount, qdcount, haveanswer;
	char aux[MAXHOSTNAMELEN];
	char ans[MAXHOSTNAMELEN];
	char *in, *bp, *ep, **ap;

	/*
	 * find first satisfactory answer
	 *
	 *      answer --> +------------+  ( MESSAGE )
	 *		   |   Header   |
	 *		   +------------+
	 *		   |  Question  | the question for the name server
	 *		   +------------+
	 *		   |   Answer   | RRs answering the question
	 *		   +------------+
	 *		   | Authority  | RRs pointing toward an authority
	 *		   | Additional | RRs holding additional information
	 *		   +------------+
	 */
	eom = answer->buf + anslen;
	hp = &answer->hdr;
	ancount = ntohs(hp->ancount); /* #/records in the answer section */
	qdcount = ntohs(hp->qdcount); /* #/entries in the question section */
	bp = ned->netbuf;
	ep = ned->netbuf + sizeof(ned->netbuf);
	cp = answer->buf + HFIXEDSZ;
	if (!qdcount) {
		if (hp->aa)
			h_errno = HOST_NOT_FOUND;
		else
			h_errno = TRY_AGAIN;
		return -1;
	}
	while (qdcount-- > 0)
		cp += __dn_skipname(cp, eom) + QFIXEDSZ;
	ap = ned->net_aliases;
	*ap = NULL;
	ne->n_aliases = ned->net_aliases;
	haveanswer = 0;
	while (--ancount >= 0 && cp < eom) {
		n = dn_expand(answer->buf, eom, cp, bp, ep - bp);
		if ((n < 0) || !res_dnok(bp))
			break;
		cp += n;
		ans[0] = '\0';
		(void)strncpy(&ans[0], bp, sizeof(ans) - 1);
		ans[sizeof(ans) - 1] = '\0';
		GETSHORT(type, cp);
		GETSHORT(class, cp);
		cp += INT32SZ;		/* TTL */
		GETSHORT(n, cp);
		if (class == C_IN && type == T_PTR) {
			n = dn_expand(answer->buf, eom, cp, bp, ep - bp);
			if ((n < 0) || !res_hnok(bp)) {
				cp += n;
				return -1;
			}
			cp += n; 
			*ap++ = bp;
			n = strlen(bp) + 1;
			bp += n;
			ne->n_addrtype = (class == C_IN) ? AF_INET : AF_UNSPEC;
			haveanswer++;
		}
	}
	if (haveanswer) {
		*ap = NULL;
		switch (net_i) {
		case BYADDR:
			ne->n_name = *ne->n_aliases;
			ne->n_net = 0L;
			break;
		case BYNAME:
			in = *ne->n_aliases;
			n = strlen(ans) + 1;
			if (ep - bp < n) {
				h_errno = NETDB_INTERNAL;
				errno = ENOBUFS;
				return -1;
			}
			strlcpy(bp, ans, ep - bp);
			ne->n_name = bp;
			if (strlen(in) + 1 > sizeof(aux)) {
				h_errno = NETDB_INTERNAL;
				errno = ENOBUFS;
				return -1;
			}
			ipreverse(in, aux);
			ne->n_net = inet_network(aux);
			break;
		}
		ne->n_aliases++;
		return 0;
	}
	h_errno = TRY_AGAIN;
	return -1;
}

int
_dns_getnetbyaddr(void *rval, void *cb_data, va_list ap)
{
	unsigned long net;
	int net_type;
	struct netent *ne;
	struct netent_data *ned;
	unsigned int netbr[4];
	int nn, anslen, error;
	querybuf *buf;
	char qbuf[MAXDNAME];
	unsigned long net2;

	net = va_arg(ap, unsigned long);
	net_type = va_arg(ap, int);
	ne = va_arg(ap, struct netent *);
	ned = va_arg(ap, struct netent_data *);

	if (net_type != AF_INET)
		return NS_UNAVAIL;

	for (nn = 4, net2 = net; net2; net2 >>= 8)
		netbr[--nn] = net2 & 0xff;
	switch (nn) {
	case 3: 	/* Class A */
		sprintf(qbuf, "0.0.0.%u.in-addr.arpa", netbr[3]);
		break;
	case 2: 	/* Class B */
		sprintf(qbuf, "0.0.%u.%u.in-addr.arpa", netbr[3], netbr[2]);
		break;
	case 1: 	/* Class C */
		sprintf(qbuf, "0.%u.%u.%u.in-addr.arpa", netbr[3], netbr[2],
		    netbr[1]);
		break;
	case 0: 	/* Class D - E */
		sprintf(qbuf, "%u.%u.%u.%u.in-addr.arpa", netbr[3], netbr[2],
		    netbr[1], netbr[0]);
		break;
	}
	if ((buf = malloc(sizeof(*buf))) == NULL) {
		h_errno = NETDB_INTERNAL;
		return NS_NOTFOUND;
	}
	anslen = res_query(qbuf, C_IN, T_PTR, (u_char *)buf, sizeof(*buf));
	if (anslen < 0) {
		free(buf);
#ifdef DEBUG
		if (_res.options & RES_DEBUG)
			printf("res_search failed\n");
#endif
		return NS_UNAVAIL;
	} else if (anslen > sizeof(*buf)) {
		free(buf);
#ifdef DEBUG
		if (_res.options & RES_DEBUG)
			printf("res_search static buffer too small\n");
#endif
		return NS_UNAVAIL;
	}
	error = getnetanswer(buf, anslen, BYADDR, ne, ned);
	free(buf);
	if (error == 0) {
		unsigned u_net = net;	/* maybe net should be unsigned ? */

		/* Strip trailing zeros */
		while ((u_net & 0xff) == 0 && u_net != 0)
			u_net >>= 8;
		ne->n_net = u_net;
		return NS_SUCCESS;
	}
	return NS_NOTFOUND;
}

int
_dns_getnetbyname(void *rval, void *cb_data, va_list ap)
{
	const char *net;
	struct netent *ne;
	struct netent_data *ned;
	int anslen, error;
	querybuf *buf;
	char qbuf[MAXDNAME];

	net = va_arg(ap, const char *);
	ne = va_arg(ap, struct netent *);
	ned = va_arg(ap, struct netent_data *);

	if ((_res.options & RES_INIT) == 0 && res_init() == -1) {
		h_errno = NETDB_INTERNAL;
		return NS_UNAVAIL;
	}
	if ((buf = malloc(sizeof(*buf))) == NULL) {
		h_errno = NETDB_INTERNAL;
		return NS_NOTFOUND;
	}
	strncpy(qbuf, net, sizeof(qbuf) - 1);
	qbuf[sizeof(qbuf) - 1] = '\0';
	anslen = res_search(qbuf, C_IN, T_PTR, (u_char *)buf, sizeof(*buf));
	if (anslen < 0) {
		free(buf);
#ifdef DEBUG
		if (_res.options & RES_DEBUG)
			printf("res_search failed\n");
#endif
		return NS_UNAVAIL;
	} else if (anslen > sizeof(*buf)) {
		free(buf);
#ifdef DEBUG
		if (_res.options & RES_DEBUG)
			printf("res_search static buffer too small\n");
#endif
		return NS_UNAVAIL;
	}
	error = getnetanswer(buf, anslen, BYNAME, ne, ned);
	free(buf);
	return (error == 0) ? NS_SUCCESS : NS_NOTFOUND;
}

void
_setnetdnsent(stayopen)
	int stayopen;
{
	if (stayopen)
		_res.options |= RES_STAYOPEN | RES_USEVC;
}

void
_endnetdnsent()
{
	_res.options &= ~(RES_STAYOPEN | RES_USEVC);
	res_close();
}
