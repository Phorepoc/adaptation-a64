/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: EPL-1.0 */

/* phe_usb_net.h -- the network layer under FreeBSD's USB Ethernet class driver (if_cdce.c: CDC ECM / NCM, imported
 * unmodified into modem-eg25) and the port seam the kernel's Ethernet PDD uses (phe_usb_net_* below).
 *
 * if_cdce.c is written for FreeBSD's usb_ethernet.c (uether) + ifnet + mbuf + ifmedia. None of those exist here: the
 * USB stack runs on FreeBSD's own kernel shim, and the packets go to the Symbian Ethernet LDD, not to a BSD protocol
 * stack. This file is the small replacement for exactly the calls if_cdce.c makes (struct mbuf as one flat buffer,
 * struct ifnet as a flag word + a send queue + the driver hooks, uether_* as the attach/start/rx plumbing).
 * It is included, through the kinc/ wrappers net/if.h, net/if_var.h, sys/mbuf.h, sys/socket.h, sys/gsb_crc32.h and
 * dev/usb/net/usb_ethernet.h, by if_cdce.c; and, with PHE_USB_NET_PORT_ONLY, by the host side.
 *
 * Threading: everything that calls into if_cdce.c runs on the USB stack's own thread (phe_usb_net_service, called from
 * phe_usb_service). phe_usb_net_tx / _up / _down only queue and ask for a service run (phe_usb_kick); phe_usb_net_rx
 * is called on the USB thread (the host copies the frame out and returns at once). */
#ifndef PHE_USB_NET_H
#define PHE_USB_NET_H

#define PHE_USB_NET_EADDR_LEN 6
#define PHE_USB_NET_MAX_IFS 2
#define PHE_USB_NET_MTU 1500

/* ----- the port seam (plain C, usable from the Symbian kernel side) ----- */
#define PHE_USB_NET_ATTACHED 1	/* an interface exists (phe_usb_net_eaddr is valid) */
#define PHE_USB_NET_GONE 2
#define PHE_USB_NET_LINK 4	/* the class driver reports the link up (CDC SPEED/NETWORK_CONNECTION notification) */
#define PHE_USB_NET_RXFRAME 8	/* a frame arrived (see phe_usb_net_rx_cb) */

const char *phe_usb_net_name(int idx);			/* NULL = no such interface; "cdce0"... */
int phe_usb_net_eaddr(int idx, unsigned char eaddr[PHE_USB_NET_EADDR_LEN]);	/* 0 = ok */
int phe_usb_net_up(int idx);				/* IFF_UP + the driver's init: starts the bulk transfers */
void phe_usb_net_down(int idx);
int phe_usb_net_link(int idx);				/* 1 = link up */
int phe_usb_net_tx(int idx, const void *frame, int len);	/* a copy is queued; < 0 = no room / not up */
/* Host: one received Ethernet frame (no FCS), on the USB thread, valid until the call returns. */
void phe_usb_net_rx_cb(int idx, const void *frame, int len) __attribute__((weak));
/* Host: attach / link change, on the USB thread (weak default: nothing). */
void phe_usb_net_notify(int idx, int events) __attribute__((weak));
void phe_usb_net_service(void);				/* one pass, from phe_usb_service */
void phe_usb_kick(void);

#ifndef PHE_USB_NET_PORT_ONLY
/* ----- the shim proper (kshim side only) ----- */
struct usb_page_cache;
struct mtx;
struct usb_device;

#ifndef M_NOWAIT
#define M_NOWAIT 1
#endif
#ifndef M_ZERO
#define M_ZERO 0x100
#endif
#define BPF_MTAP(ifp, m) do { } while (0)
#define MHLEN 160
#define MCLBYTES 2048
#define MT_DATA 1
#define M_PKTHDR 2
#define ETHER_ADDR_LEN 6
#define ETHER_HDR_LEN 14
#define ETHER_CRC_LEN 4
#define ETHER_VLAN_ENCAP_LEN 4
#define ETHER_ALIGN 2
#define ETHER_MAX_LEN 1518
struct ether_header {
	unsigned char ether_dhost[ETHER_ADDR_LEN];
	unsigned char ether_shost[ETHER_ADDR_LEN];
	unsigned short ether_type;
} __attribute__((packed));

/* one contiguous buffer per mbuf: m_data points into it (m_adj moves it); chains are never built here */
struct mbuf {
	struct mbuf *m_next;
	struct mbuf *m_nextpkt;
	unsigned char *m_data;
	int m_len;
	int m_cap;		/* bytes of m_buf */
	struct { int len; } m_pkthdr;
	unsigned char *m_buf;
};
struct mbuf *m_gethdr(int how, int type);
struct mbuf *m_getcl(int how, int type, int flags);
void m_freem(struct mbuf *m);
void m_adj(struct mbuf *m, int len);
int m_append(struct mbuf *m, int len, const void *cp);
struct mbuf *m_defrag(struct mbuf *m, int how);
int m_apply(struct mbuf *m, int off, int len, int (*f)(void *, void *, unsigned int), void *arg);
void usbd_m_copy_in(struct usb_page_cache *cache, unsigned int dst_offset, struct mbuf *m, unsigned int src_offset,
    unsigned int src_len);

/* ifnet: the flags/capabilities cdce sets, a send queue, link state, and the three driver hooks */
#define IFF_UP 0x1
#define IFF_BROADCAST 0x2
#define IFF_PROMISC 0x100
#define IFF_ALLMULTI 0x200
#define IFF_SIMPLEX 0x800
#define IFF_MULTICAST 0x8000
#define IFF_DRV_RUNNING 0x40
#define IFCAP_VLAN_MTU 0x2
#define IFCAP_LINKSTATE 0x400000
#define IFCOUNTER_IPACKETS 0
#define IFCOUNTER_IERRORS 1
#define IFCOUNTER_OPACKETS 2
#define IFCOUNTER_OERRORS 3
#define IFCOUNTER_LAST 4
#define LINK_STATE_UNKNOWN 0
#define LINK_STATE_DOWN 1
#define LINK_STATE_UP 2
#define ifqmaxlen 50
#define SIOCSIFMEDIA 0xc0206937UL
#define SIOCGIFMEDIA 0xc0286938UL
struct ifnet;
typedef struct ifnet *if_t;
struct ifreq;
struct ifnet {
	void *if_softc;
	unsigned int flags, drvflags, caps, capenable;
	int link;
	unsigned long long baudrate;
	unsigned long long counters[IFCOUNTER_LAST];
	struct mbuf *sq_head, *sq_tail;
	int sq_len, sq_max;
	void (*startfn)(if_t);
	int (*ioctlfn)(if_t, unsigned long, unsigned char *);
	void (*initfn)(void *);
};
void if_setflags(if_t, int);
int if_getflags(if_t);
void if_setdrvflagbits(if_t, int set, int clear);
void if_setcapabilitiesbit(if_t, int set, int clear);
int if_getcapabilities(if_t);
void if_setcapenable(if_t, int);
void *if_getsoftc(if_t);
void if_setstartfn(if_t, void (*)(if_t));
void if_setioctlfn(if_t, int (*)(if_t, unsigned long, unsigned char *));
void if_setinitfn(if_t, void (*)(void *));
void if_setsendqlen(if_t, int);
void if_setsendqready(if_t);
struct mbuf *if_dequeue(if_t);
void if_sendq_prepend(if_t, struct mbuf *);
void if_inc_counter(if_t, int, unsigned long long);
int if_getlinkstate(if_t);
void if_link_state_change(if_t, int);
void if_setbaudrate(if_t, unsigned long long);

/* ifmedia: cdce keeps one "auto ethernet" entry; nothing else reads it */
#define IFM_ETHER 0x20
#define IFM_AUTO 0
#define IFM_IMASK 0xff000000U
#define IFM_AVALID 1
#define IFM_ACTIVE 2
struct ifmedia_entry { int ifm_media; };
struct ifmedia {
	int ifm_media;
	struct ifmedia_entry *ifm_cur;
	struct ifmedia_entry ifm_ent;
};
struct ifmediareq {
	int ifm_active;
	int ifm_status;
};
void ifmedia_init(struct ifmedia *, int, int (*)(if_t), void (*)(if_t, struct ifmediareq *));
void ifmedia_add(struct ifmedia *, int, int, void *);
void ifmedia_set(struct ifmedia *, int);
void ifmedia_removeall(struct ifmedia *);
int ifmedia_ioctl(if_t, struct ifreq *, struct ifmedia *, unsigned long);

/* uether: attach/start/init/rx plumbing (FreeBSD sys/dev/usb/net/usb_ethernet.[ch] reduced to what cdce uses) */
struct usb_ether;
typedef void (uether_fn_t)(struct usb_ether *);
struct usb_ether_methods {
	uether_fn_t *ue_attach_post;
	int (*ue_attach_post_sub)(struct usb_ether *);
	uether_fn_t *ue_start;
	uether_fn_t *ue_init;
	uether_fn_t *ue_stop;
	uether_fn_t *ue_setmulti;
	uether_fn_t *ue_setpromisc;
};
struct usb_ether {
	if_t ue_ifp;
	struct mtx *ue_mtx;
	void *ue_dev;
	struct usb_device *ue_udev;
	void *ue_sc;
	const struct usb_ether_methods *ue_methods;
	unsigned char ue_eaddr[ETHER_ADDR_LEN];
	struct mbuf *ue_rxq, *ue_rxq_tail;
	int ue_idx;
};
#define uether_getsc(ue) ((ue)->ue_sc)
#define uether_getifp(ue) ((ue)->ue_ifp)
int uether_ifattach(struct usb_ether *);
void uether_ifdetach(struct usb_ether *);
void uether_start(if_t);
void uether_init(void *);
int uether_ioctl(if_t, unsigned long, unsigned char *);
void uether_rxmbuf(struct usb_ether *, struct mbuf *, unsigned int);
void uether_rxflush(struct usb_ether *);
struct mbuf *uether_newbuf(void);

/* sys/gsb_crc32.h: the CRC-32 (IEEE 802.3, reflected) the NCM "crc" mode asks for */
unsigned int crc32_raw(const void *buf, unsigned long size, unsigned int crc);
#endif /* PHE_USB_NET_PORT_ONLY */

#endif
