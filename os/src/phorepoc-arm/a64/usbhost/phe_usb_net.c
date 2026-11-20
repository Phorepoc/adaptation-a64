/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: EPL-1.0 */

/* phe_usb_net.c -- the mbuf / ifnet / ifmedia / uether replacement under FreeBSD's if_cdce.c (see phe_usb_net.h), and the
 * port seam for the Ethernet PDD (phe_usb_net_*). Everything here runs on the USB stack's own thread except the
 * documented seam calls, which only touch the pending-transmit ring and flags. */
#include "phe_usb_net.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/mutex.h>
#include <dev/usb/usb.h>
#include <dev/usb/usbdi.h>

#define NET_TXQ 16			/* frames waiting for the host -> USB hand-over */
#define NET_FRAME_MAX 1536

struct net_if {
	struct usb_ether *ue;
	volatile int events;		/* PHE_USB_NET_* */
	volatile int want_up, want_down, want_start;
	char name[8];
	/* host -> USB: single producer (the PDD), single consumer (the USB thread) */
	unsigned short txlen[NET_TXQ];
	unsigned char txbuf[NET_TXQ][NET_FRAME_MAX];
	volatile unsigned int txhead, txtail;
};
static struct net_if nifs[PHE_USB_NET_MAX_IFS];
/* bounded [usbh] net markers: the first frames each way, then one line per 256 */
static unsigned int n_tx_ok, n_tx_drop, n_rx;
#define NET_EVERY(n) ((n) <= 8 || ((n) & 255) == 0)

void phe_usb_kick(void) __attribute__((weak));
void phe_usb_net_notify(int idx, int events) __attribute__((weak));
void phe_usb_net_rx_cb(int idx, const void *frame, int len) __attribute__((weak));

/*------------------------------------------------------------------------*
 * mbuf: one flat buffer
 *------------------------------------------------------------------------*/
static struct mbuf *
m_alloc(int cap)
{
	struct mbuf *m = malloc(sizeof(*m) + cap, M_USBDEV, M_NOWAIT | M_ZERO);

	if (m != NULL)
		memset(m, 0, sizeof(*m));

	if (m == NULL)
		return (NULL);
	m->m_buf = (unsigned char *)(m + 1);
	m->m_data = m->m_buf;
	m->m_cap = cap;
	return (m);
}

struct mbuf *
m_gethdr(int how, int type)
{
	(void)how; (void)type;
	return (m_alloc(MHLEN));
}

struct mbuf *
m_getcl(int how, int type, int flags)
{
	(void)how; (void)type; (void)flags;
	return (m_alloc(MCLBYTES));
}

void
m_freem(struct mbuf *m)
{
	while (m != NULL) {
		struct mbuf *n = m->m_next;

		free(m, M_USBDEV);
		m = n;
	}
}

void
m_adj(struct mbuf *m, int len)
{
	if (len > 0 && len <= m->m_len) {	/* the head only: cdce trims ETHER_ALIGN */
		m->m_data += len;
		m->m_len -= len;
		m->m_pkthdr.len -= len;
	}
}

int
m_append(struct mbuf *m, int len, const void *cp)
{
	int room = m->m_cap - (int)(m->m_data - m->m_buf) - m->m_len;

	if (len > room)
		return (0);
	memcpy(m->m_data + m->m_len, cp, len);
	m->m_len += len;
	m->m_pkthdr.len += len;
	return (1);
}

struct mbuf *
m_defrag(struct mbuf *m, int how)
{
	(void)how;
	return (m);			/* always one buffer */
}

int
m_apply(struct mbuf *m, int off, int len, int (*f)(void *, void *, unsigned int), void *arg)
{
	if (off < 0 || len < 0 || off + len > m->m_len)
		return (EMSGSIZE);
	return (f(arg, m->m_data + off, (unsigned int)len));
}

void
usbd_m_copy_in(struct usb_page_cache *cache, unsigned int dst_offset, struct mbuf *m, unsigned int src_offset,
    unsigned int src_len)
{
	if (src_offset + src_len > (unsigned int)m->m_len)
		src_len = (unsigned int)m->m_len > src_offset ? (unsigned int)m->m_len - src_offset : 0;
	usbd_copy_in(cache, dst_offset, m->m_data + src_offset, src_len);
}

unsigned int
crc32_raw(const void *buf, unsigned long size, unsigned int crc)
{
	const unsigned char *p = buf;
	unsigned int c;
	int k;

	/* reflected IEEE 802.3 CRC-32 (polynomial 0xedb88320), bitwise: frames are small and this is NCM-only */
	while (size--) {
		c = (crc ^ *p++) & 0xff;
		for (k = 0; k < 8; k++)
			c = (c & 1) ? (c >> 1) ^ 0xedb88320U : (c >> 1);
		crc = c ^ (crc >> 8);
	}
	return (crc);
}

/*------------------------------------------------------------------------*
 * ifnet
 *------------------------------------------------------------------------*/
void if_setflags(if_t i, int f) { i->flags = (unsigned int)f; }
int if_getflags(if_t i) { return ((int)i->flags); }
void if_setdrvflagbits(if_t i, int set, int clear) { i->drvflags = (i->drvflags | (unsigned int)set) & ~(unsigned int)clear; }
void if_setcapabilitiesbit(if_t i, int set, int clear) { i->caps = (i->caps | (unsigned int)set) & ~(unsigned int)clear; }
int if_getcapabilities(if_t i) { return ((int)i->caps); }
void if_setcapenable(if_t i, int c) { i->capenable = (unsigned int)c; }
void *if_getsoftc(if_t i) { return (i->if_softc); }
void if_setstartfn(if_t i, void (*f)(if_t)) { i->startfn = f; }
void if_setioctlfn(if_t i, int (*f)(if_t, unsigned long, unsigned char *)) { i->ioctlfn = f; }
void if_setinitfn(if_t i, void (*f)(void *)) { i->initfn = f; }
void if_setsendqlen(if_t i, int n) { i->sq_max = n; }
void if_setsendqready(if_t i) { (void)i; }
int if_getlinkstate(if_t i) { return (i->link); }
void if_setbaudrate(if_t i, unsigned long long b) { i->baudrate = b; }
void if_inc_counter(if_t i, int c, unsigned long long n) { if (c >= 0 && c < IFCOUNTER_LAST) i->counters[c] += n; }

struct mbuf *
if_dequeue(if_t i)
{
	struct mbuf *m = i->sq_head;

	if (m != NULL) {
		i->sq_head = m->m_nextpkt;
		if (i->sq_head == NULL)
			i->sq_tail = NULL;
		m->m_nextpkt = NULL;
		i->sq_len--;
	}
	return (m);
}

void
if_sendq_prepend(if_t i, struct mbuf *m)
{
	m->m_nextpkt = i->sq_head;
	i->sq_head = m;
	if (i->sq_tail == NULL)
		i->sq_tail = m;
	i->sq_len++;
}

static void
sq_enqueue(if_t i, struct mbuf *m)
{
	m->m_nextpkt = NULL;
	if (i->sq_tail != NULL)
		i->sq_tail->m_nextpkt = m;
	else
		i->sq_head = m;
	i->sq_tail = m;
	i->sq_len++;
}

static struct net_if *
net_of(if_t i)
{
	struct usb_ether *ue = i->if_softc;

	return (&nifs[ue->ue_idx]);
}

void
if_link_state_change(if_t i, int state)
{
	struct net_if *n = net_of(i);

	i->link = state;
	printf("[usbh] net: link %s\n", state == LINK_STATE_UP ? "UP" : "DOWN");
	if (state == LINK_STATE_UP)
		n->events |= PHE_USB_NET_LINK;
	else
		n->events &= ~PHE_USB_NET_LINK;
	if (phe_usb_net_notify != NULL)
		phe_usb_net_notify((int)(n - nifs), n->events);
}

/*------------------------------------------------------------------------*
 * ifmedia (one entry; cdce answers status requests itself)
 *------------------------------------------------------------------------*/
void
ifmedia_init(struct ifmedia *m, int mask, int (*change)(if_t), void (*status)(if_t, struct ifmediareq *))
{
	(void)mask; (void)change; (void)status;
	memset(m, 0, sizeof(*m));
}

void
ifmedia_add(struct ifmedia *m, int media, int data, void *aux)
{
	(void)data; (void)aux;
	m->ifm_ent.ifm_media = media;
}

void
ifmedia_set(struct ifmedia *m, int media)
{
	m->ifm_ent.ifm_media = media;
	m->ifm_cur = &m->ifm_ent;
}

void ifmedia_removeall(struct ifmedia *m) { m->ifm_cur = NULL; }
int ifmedia_ioctl(if_t i, struct ifreq *r, struct ifmedia *m, unsigned long c) { (void)i; (void)r; (void)m; (void)c; return (0); }

/*------------------------------------------------------------------------*
 * uether
 *------------------------------------------------------------------------*/
int
uether_ifattach(struct usb_ether *ue)
{
	static struct ifnet ifs[PHE_USB_NET_MAX_IFS];
	int idx;

	for (idx = 0; idx < PHE_USB_NET_MAX_IFS; idx++)
		if (nifs[idx].ue == NULL)
			break;
	if (idx == PHE_USB_NET_MAX_IFS)
		return (ENOMEM);
	memset(&ifs[idx], 0, sizeof(ifs[idx]));
	memset(&nifs[idx], 0, sizeof(nifs[idx]));
	ue->ue_idx = idx;
	ue->ue_ifp = &ifs[idx];
	ifs[idx].if_softc = ue;
	ifs[idx].link = LINK_STATE_UNKNOWN;
	nifs[idx].ue = ue;
	nifs[idx].name[0] = 'c'; nifs[idx].name[1] = 'd'; nifs[idx].name[2] = 'c'; nifs[idx].name[3] = 'e';
	nifs[idx].name[4] = (char)('0' + idx);
	if (ue->ue_methods->ue_attach_post != NULL)
		ue->ue_methods->ue_attach_post(ue);
	if (ue->ue_methods->ue_attach_post_sub != NULL)
		(void)ue->ue_methods->ue_attach_post_sub(ue);
	nifs[idx].events = PHE_USB_NET_ATTACHED;
	printf("[usbh] net: %s attached, MAC %02x:%02x:%02x:%02x:%02x:%02x, flags %x\n", nifs[idx].name,
	    ue->ue_eaddr[0], ue->ue_eaddr[1], ue->ue_eaddr[2], ue->ue_eaddr[3], ue->ue_eaddr[4], ue->ue_eaddr[5], ifs[idx].flags);
	if (phe_usb_net_notify != NULL)
		phe_usb_net_notify(idx, nifs[idx].events);
	return (0);
}

void
uether_ifdetach(struct usb_ether *ue)
{
	struct net_if *n = &nifs[ue->ue_idx];

	printf("[usbh] net: %s detached\n", n->name);
	n->events = PHE_USB_NET_GONE;
	if (phe_usb_net_notify != NULL)
		phe_usb_net_notify(ue->ue_idx, n->events);
	if (ue->ue_methods->ue_stop != NULL)
		ue->ue_methods->ue_stop(ue);
	while (ue->ue_ifp->sq_head != NULL)
		m_freem(if_dequeue(ue->ue_ifp));
	while (ue->ue_rxq != NULL) {
		struct mbuf *m = ue->ue_rxq;

		ue->ue_rxq = m->m_nextpkt;
		m_freem(m);
	}
	ue->ue_rxq_tail = NULL;
	n->ue = NULL;
}

void
uether_start(if_t ifp)
{
	net_of(ifp)->want_start = 1;	/* run on the next service pass (cdce_start starts the bulk-write transfer) */
}

void
uether_init(void *arg)
{
	struct usb_ether *ue = arg;

	ue->ue_ifp->flags |= IFF_UP;
	if (ue->ue_methods->ue_init != NULL)
		ue->ue_methods->ue_init(ue);
}

int
uether_ioctl(if_t ifp, unsigned long cmd, unsigned char *data)
{
	(void)ifp; (void)cmd; (void)data;
	return (ENOTTY);
}

struct mbuf *
uether_newbuf(void)
{
	struct mbuf *m = m_getcl(M_NOWAIT, MT_DATA, M_PKTHDR);

	if (m == NULL)
		return (NULL);
	m->m_len = m->m_pkthdr.len = MCLBYTES;
	m_adj(m, ETHER_ALIGN);		/* the IP header lands 4-aligned */
	return (m);
}

void
uether_rxmbuf(struct usb_ether *ue, struct mbuf *m, unsigned int len)
{
	m->m_pkthdr.len = m->m_len = (int)len;
	m->m_nextpkt = NULL;
	if (ue->ue_rxq_tail != NULL)
		ue->ue_rxq_tail->m_nextpkt = m;
	else
		ue->ue_rxq = m;
	ue->ue_rxq_tail = m;
	if_inc_counter(ue->ue_ifp, IFCOUNTER_IPACKETS, 1);
}

void
uether_rxflush(struct usb_ether *ue)
{
	struct mbuf *m, *next;

	m = ue->ue_rxq;
	ue->ue_rxq = ue->ue_rxq_tail = NULL;
	for (; m != NULL; m = next) {
		next = m->m_nextpkt;
		n_rx++;
		if (NET_EVERY(n_rx))
			printf("[usbh] net: rx #%u len %d type %02x%02x%s\n", n_rx, m->m_len, m->m_len > 13 ? m->m_data[12] : 0,
			    m->m_len > 13 ? m->m_data[13] : 0, (ue->ue_ifp->drvflags & IFF_DRV_RUNNING) ? "" : " (dropped: not running)");
		if (phe_usb_net_rx_cb != NULL && (ue->ue_ifp->drvflags & IFF_DRV_RUNNING))
			phe_usb_net_rx_cb(ue->ue_idx, m->m_data, m->m_len);
		m_freem(m);
	}
}

/*------------------------------------------------------------------------*
 * the port seam
 *------------------------------------------------------------------------*/
static struct net_if *
slot(int idx)
{
	if (idx < 0 || idx >= PHE_USB_NET_MAX_IFS || nifs[idx].ue == NULL)
		return (NULL);
	return (&nifs[idx]);
}

const char *phe_usb_net_name(int idx) { struct net_if *n = slot(idx); return (n ? n->name : NULL); }

int
phe_usb_net_eaddr(int idx, unsigned char eaddr[PHE_USB_NET_EADDR_LEN])
{
	struct net_if *n = slot(idx);

	if (n == NULL)
		return (-1);
	memcpy(eaddr, n->ue->ue_eaddr, PHE_USB_NET_EADDR_LEN);
	return (0);
}

int
phe_usb_net_up(int idx)
{
	struct net_if *n = slot(idx);

	if (n == NULL)
		return (-1);
	n->want_down = 0;
	n->want_up = 1;
	phe_usb_kick();
	return (0);
}

void
phe_usb_net_down(int idx)
{
	struct net_if *n = slot(idx);

	if (n != NULL) {
		n->want_up = 0;
		n->want_down = 1;
		phe_usb_kick();
	}
}

int phe_usb_net_link(int idx) { struct net_if *n = slot(idx); return (n != NULL && (n->events & PHE_USB_NET_LINK)); }

int
phe_usb_net_tx(int idx, const void *frame, int len)
{
	struct net_if *n = slot(idx);
	unsigned int h;

	if (n == NULL || !(n->ue->ue_ifp->drvflags & IFF_DRV_RUNNING) || len <= 0 || len > NET_FRAME_MAX) {
		n_tx_drop++;
		if (NET_EVERY(n_tx_drop))
			printf("[usbh] net: tx REFUSED #%u (len %d, interface %s, running %d)\n", n_tx_drop, len, n ? "ok" : "none",
			    n ? (n->ue->ue_ifp->drvflags & IFF_DRV_RUNNING) != 0 : 0);
		return (-1);
	}
	h = n->txhead;
	if (h - n->txtail >= NET_TXQ)
		return (-2);
	memcpy(n->txbuf[h % NET_TXQ], frame, (size_t)len);
	n->txlen[h % NET_TXQ] = (unsigned short)len;
	__sync_synchronize();
	n->txhead = h + 1;
	n_tx_ok++;
	if (NET_EVERY(n_tx_ok))
		printf("[usbh] net: tx #%u len %d type %02x%02x\n", n_tx_ok, len, ((const unsigned char *)frame)[12], ((const unsigned char *)frame)[13]);
	n->want_start = 1;
	phe_usb_kick();
	return (len);
}

void
phe_usb_net_service(void)
{
	int i;

	for (i = 0; i < PHE_USB_NET_MAX_IFS; i++) {
		struct net_if *n = &nifs[i];
		struct usb_ether *ue = n->ue;
		if_t ifp;

		if (ue == NULL)
			continue;
		ifp = ue->ue_ifp;
		if (n->want_up) {
			n->want_up = 0;
			if (!(ifp->drvflags & IFF_DRV_RUNNING))
				uether_init(ue);
			printf("[usbh] net: up, drv flags %x, link %d\n", ifp->drvflags, ifp->link);
		}
		if (n->want_down) {
			n->want_down = 0;
			ifp->flags &= ~IFF_UP;
			printf("[usbh] net: down\n");
			if (ue->ue_methods->ue_stop != NULL)
				ue->ue_methods->ue_stop(ue);
		}
		/* host -> send queue (mbufs), then let the class driver start the write transfer */
		while (n->txtail != n->txhead && ifp->sq_len < ifp->sq_max) {
			unsigned int t = n->txtail;
			struct mbuf *m = m_gethdr(M_NOWAIT, MT_DATA);

			if (m == NULL || n->txlen[t % NET_TXQ] > m->m_cap) {
				if (m != NULL)
					m_freem(m);
				m = m_getcl(M_NOWAIT, MT_DATA, M_PKTHDR);
			}
			if (m == NULL)
				break;
			memcpy(m->m_data, n->txbuf[t % NET_TXQ], n->txlen[t % NET_TXQ]);
			m->m_len = m->m_pkthdr.len = n->txlen[t % NET_TXQ];
			n->txtail = t + 1;
			sq_enqueue(ifp, m);
		}
		if (n->want_start || ifp->sq_len > 0) {
			n->want_start = 0;
			if ((ifp->drvflags & IFF_DRV_RUNNING) && ue->ue_methods->ue_start != NULL)
				ue->ue_methods->ue_start(ue);
		}
	}
}
