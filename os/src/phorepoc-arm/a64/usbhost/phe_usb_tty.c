/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: EPL-1.0 */

/* phe_usb_tty.c -- the tty layer under FreeBSD's ucom (see phe_usb_tty.h) and the port seam the Comm unit uses
 * (phe_usb_tty_* in phe_usb_port.h). Everything that calls into ucom runs on the USB stack's own thread
 * (phe_usb_tty_service, from phe_usb_service); the reader/writer side only touches the two rings and flags, and
 * asks for a service run with phe_usb_kick(). Each ring has one producer and one consumer. */
#include "phe_usb_tty.h"

#include "phe_usb_port.h"

#include <stdarg.h>

int phe_usb_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);

#define PHE_USB_MAX_TTYS 8
static struct tty *ttys[PHE_USB_MAX_TTYS];
/* breadcrumbs of the write path (first 60): tty ring put, service outwakeup, ucom_get_data -> ttydisc_getc */
static int trc_left = 60;
#ifdef PHE_USBHOST_TRACE
#define TRC(...) do { if (trc_left > 0) { --trc_left; printf(__VA_ARGS__); } } while (0)
#else
#define TRC(...) do { } while (0)
#endif
static volatile unsigned char want_open[PHE_USB_MAX_TTYS], want_close[PHE_USB_MAX_TTYS], want_out[PHE_USB_MAX_TTYS],
    want_in[PHE_USB_MAX_TTYS];

/* the host provides these (the kernel extension queues its service DFC); the bench polls, so no-ops there */
void phe_usb_kick(void) __attribute__((weak));
void phe_usb_kick(void) { }
void phe_usb_tty_notify(int idx, int events) __attribute__((weak));
void phe_usb_tty_notify(int idx, int events) { (void)idx; (void)events; }

static unsigned int ring_used(const struct phe_usb_ring *r) { return (r->head - r->tail); }
static unsigned int ring_free(const struct phe_usb_ring *r) { return (PHE_USB_TTY_RING - ring_used(r)); }

static unsigned int
ring_put(struct phe_usb_ring *r, const unsigned char *p, unsigned int n)
{
	unsigned int i, f = ring_free(r);

	if (n > f)
		n = f;
	for (i = 0; i < n; i++)
		r->buf[(r->head + i) % PHE_USB_TTY_RING] = p[i];
	__sync_synchronize();
	r->head += n;
	return (n);
}

static unsigned int
ring_get(struct phe_usb_ring *r, unsigned char *p, unsigned int n)
{
	unsigned int i, u = ring_used(r);

	if (n > u)
		n = u;
	for (i = 0; i < n; i++)
		p[i] = r->buf[(r->tail + i) % PHE_USB_TTY_RING];
	__sync_synchronize();
	r->tail += n;
	return (n);
}

/*------------------------------------------------------------------------*
 * The tty interface ucom uses
 *------------------------------------------------------------------------*/
struct tty *
tty_alloc_mutex(struct ttydevsw *tsw, void *softc, struct mtx *mtx)
{
	struct tty *tp = malloc(sizeof(*tp), M_DEVBUF, M_WAITOK | M_ZERO);
	struct termios *t;

	if (tp == NULL)
		return (NULL);
	tp->t_devsw = tsw;
	tp->t_devswsoftc = softc;
	tp->t_mtx = mtx;
	tp->t_index = -1;
	/* 115200 8N1, receiver on, no modem-control hang-up: what an AT port expects */
	t = &tp->t_termios;
	t->c_cflag = CS8 | CREAD | CLOCAL;
	t->c_ispeed = t->c_ospeed = 115200;
	t->c_cc[VSTART] = 0x11;
	t->c_cc[VSTOP] = 0x13;
	tp->t_termios_init_in = tp->t_termios_init_out = *t;
	return (tp);
}

int
tty_makedev(struct tty *tp, struct ucred *cred, const char *fmt, ...)
{
	va_list ap;
	int i;

	va_start(ap, fmt);
	phe_usb_vsnprintf(tp->t_name, sizeof(tp->t_name), fmt, ap);
	va_end(ap);
	for (i = 0; i < PHE_USB_MAX_TTYS; i++)
		if (ttys[i] == NULL) {
			ttys[i] = tp;
			tp->t_index = i;
			printf("[usbh] tty%d = %s\n", i, tp->t_name);
			phe_usb_tty_notify(i, PHE_USB_TTY_ATTACHED);
			return (0);
		}
	return (ENOSPC);
}

void
tty_rel_gone(struct tty *tp)
{
	int i = tp->t_index;

	tp->t_gone = 1;
	tty_unlock(tp);
	if (i >= 0 && i < PHE_USB_MAX_TTYS && ttys[i] == tp) {
		printf("[usbh] tty%d gone\n", i);
		phe_usb_tty_notify(i, PHE_USB_TTY_GONE);
		ttys[i] = NULL;
	}
	if (tp->t_devsw->tsw_free != NULL)
		tp->t_devsw->tsw_free(tp->t_devswsoftc);
	free(tp, M_DEVBUF);
}

char *
strstr(const char *s, const char *find)
{
	size_t n = strlen(find);

	for (; *s != 0; s++)
		if (strncmp(s, find, n) == 0)
			return ((char *)s);
	return (n == 0 ? (char *)s : NULL);
}

size_t
strlcat(char *dst, const char *src, size_t size)
{
	size_t d = strlen(dst), i;

	for (i = 0; src[i] != 0 && d + i + 1 < size; i++)
		dst[d + i] = src[i];
	if (d + i < size)
		dst[d + i] = 0;
	return (d + strlen(src));
}

void tty_init_console(struct tty *tp, speed_t speed) { (void)tp; (void)speed; }

int
ttydisc_rint(struct tty *tp, char c, int flags)
{
	if (flags != 0)
		return (0);		/* break / framing / parity events: no character */
	return (ring_put(&tp->t_rx, (unsigned char *)&c, 1) == 1 ? 0 : -1);
}

size_t
ttydisc_rint_bypass(struct tty *tp, const void *buf, size_t len)
{
	return (ring_put(&tp->t_rx, buf, len));
}

size_t ttydisc_rint_poll(struct tty *tp) { return (ring_free(&tp->t_rx)); }

void
ttydisc_rint_done(struct tty *tp)
{
	if (tp->t_index >= 0)
		phe_usb_tty_notify(tp->t_index, PHE_USB_TTY_RX);
}

size_t
ttydisc_getc(struct tty *tp, void *buf, size_t len)
{
	size_t n = ring_get(&tp->t_tx, buf, len);
	TRC("[usbh] tty%d: ttydisc_getc (ucom_get_data) len %d -> %d\n", tp->t_index, (int)len, (int)n);

	if (n != 0 && tp->t_index >= 0)
		phe_usb_tty_notify(tp->t_index, PHE_USB_TTY_TX);
	return (n);
}

void ttydisc_modem(struct tty *tp, int open) { (void)tp; (void)open; }

/*------------------------------------------------------------------------*
 * Unit numbers (ucom's U<n> names)
 *------------------------------------------------------------------------*/
struct unrhdr {
	int low, high;
	unsigned char used[128];
};

struct unrhdr *
new_unrhdr(int low, int high, struct mtx *mutex)
{
	struct unrhdr *u = malloc(sizeof(*u), M_DEVBUF, M_WAITOK | M_ZERO);

	(void)mutex;
	if (u != NULL) {
		u->low = low;
		u->high = high < low + 127 ? high : low + 127;
	}
	return (u);
}

void delete_unrhdr(struct unrhdr *uh) { free(uh, M_DEVBUF); }

int
alloc_unr(struct unrhdr *uh)
{
	int i;

	for (i = uh->low; i <= uh->high; i++)
		if (!uh->used[i - uh->low]) {
			uh->used[i - uh->low] = 1;
			return (i);
		}
	return (-1);
}

void
free_unr(struct unrhdr *uh, unsigned int item)
{
	if ((int)item >= uh->low && (int)item <= uh->high)
		uh->used[item - uh->low] = 0;
}

/*------------------------------------------------------------------------*
 * The port seam (phe_usb_port.h)
 *------------------------------------------------------------------------*/
static struct tty *
tty_at(int idx)
{
	return ((idx >= 0 && idx < PHE_USB_MAX_TTYS) ? ttys[idx] : NULL);
}

const char *
phe_usb_tty_name(int idx)
{
	struct tty *tp = tty_at(idx);

	return (tp != NULL ? tp->t_name : NULL);
}

int
phe_usb_tty_open(int idx)
{
	if (tty_at(idx) == NULL)
		return (ENXIO);
	want_open[idx] = 1;
	phe_usb_kick();
	return (0);
}

void
phe_usb_tty_close(int idx)
{
	if (tty_at(idx) == NULL)
		return;
	want_close[idx] = 1;
	phe_usb_kick();
}

int
phe_usb_tty_write(int idx, const void *buf, int len)
{
	struct tty *tp = tty_at(idx);
	int n;

	if (tp == NULL || tp->t_gone)
		return (-ENXIO);
	n = ring_put(&tp->t_tx, buf, len);
	TRC("[usbh] tty%d: write ring put %d of %d\n", idx, (int)n, len);
	if (n != 0) {
		want_out[idx] = 1;
		phe_usb_kick();
	}
	return (n);
}

int
phe_usb_tty_read(int idx, void *buf, int len)
{
	struct tty *tp = tty_at(idx);
	int n;

	if (tp == NULL)
		return (-ENXIO);
	n = ring_get(&tp->t_rx, buf, len);
	if (n != 0) {
		want_in[idx] = 1;
		phe_usb_kick();
	}
	return (n);
}

int
phe_usb_tty_rx_avail(int idx)
{
	struct tty *tp = tty_at(idx);

	return (tp != NULL ? (int)ring_used(&tp->t_rx) : 0);
}

int
phe_usb_tty_tx_free(int idx)
{
	struct tty *tp = tty_at(idx);

	return (tp != NULL ? (int)ring_free(&tp->t_tx) : 0);
}

/*------------------------------------------------------------------------*
 * Driver task queues (usb_proc_create: ucom's per-device command queue). The kshim runs only the stack's own fixed
 * processes; these run here, on the same thread: from phe_usb_tty_service, and inline when a driver waits for its
 * command (usb_proc_mwait_sig), which is the order a FreeBSD process thread gives them.
 *------------------------------------------------------------------------*/
#define PHE_USB_MAX_PROCS 8
static struct usb_process *procs[PHE_USB_MAX_PROCS];

static int
proc_run(struct usb_process *up)
{
	struct usb_proc_msg *pm;
	int worked = 0;

	mtx_lock(up->up_mtx);
	while ((pm = TAILQ_FIRST(&up->up_qhead)) != NULL) {
		worked = 1;
		(pm->pm_callback)(pm);
		if (pm == TAILQ_FIRST(&up->up_qhead)) {
			TAILQ_REMOVE(&up->up_qhead, pm, pm_qentry);
			pm->pm_qentry.tqe_prev = NULL;
		}
	}
	mtx_unlock(up->up_mtx);
	return (worked);
}

int
usb_proc_create(struct usb_process *up, struct mtx *p_mtx, const char *pmesg, uint8_t prio)
{
	int i;

	(void)pmesg;
	TAILQ_INIT(&up->up_qhead);
	up->up_mtx = p_mtx;
	up->up_prio = prio;
	up->up_gone = 0;
	up->up_msg_num = 0;
	for (i = 0; i < PHE_USB_MAX_PROCS; i++)
		if (procs[i] == NULL) {
			procs[i] = up;
			return (0);
		}
	return (ENOMEM);
}

int
usb_proc_mwait_sig(struct usb_process *up, void *pm0, void *pm1)
{
	struct usb_proc_msg *m0 = pm0, *m1 = pm1;

	while (m0->pm_qentry.tqe_prev != NULL || m1->pm_qentry.tqe_prev != NULL)
		if (!proc_run(up))
			break;
	return (0);
}

void
usb_proc_drain(struct usb_process *up)
{
	int i;

	proc_run(up);
	up->up_gone = 1;
	for (i = 0; i < PHE_USB_MAX_PROCS; i++)
		if (procs[i] == up)
			procs[i] = NULL;
}

void usb_proc_free(struct usb_process *up) { usb_proc_drain(up); }

/* on the USB thread: carry out what the reader/writer side asked for */
void
phe_usb_tty_service(void)
{
	struct tty *tp;
	int i, r;

	for (i = 0; i < PHE_USB_MAX_PROCS; i++)
		if (procs[i] != NULL)
			proc_run(procs[i]);

	for (i = 0; i < PHE_USB_MAX_TTYS; i++) {
		if ((tp = ttys[i]) == NULL || tp->t_gone)
			continue;
		if (want_open[i]) {
			want_open[i] = 0;
			if (!tp->t_open) {
				tty_lock(tp);
				r = tp->t_devsw->tsw_open(tp);
				if (r == 0) {
					tp->t_open = 1;
					tp->t_devsw->tsw_param(tp, &tp->t_termios);
					tp->t_devsw->tsw_modem(tp, SER_DTR | SER_RTS, 0);
				}
				tty_unlock(tp);
				printf("[usbh] tty%d open r=%d\n", i, r);
			}
		}
		if (want_close[i]) {
			want_close[i] = 0;
			if (tp->t_open) {
				tty_lock(tp);
				tp->t_devsw->tsw_close(tp);
				tp->t_open = 0;
				tty_unlock(tp);
			}
		}
		if (want_out[i] && tp->t_open) {
			want_out[i] = 0;
			tty_lock(tp);
			TRC("[usbh] tty%d: service -> tsw_outwakeup (ucom_outwakeup)\n", i);
			tp->t_devsw->tsw_outwakeup(tp);
			tty_unlock(tp);
		}
		if (want_in[i] && tp->t_open) {
			want_in[i] = 0;
			tty_lock(tp);
			tp->t_devsw->tsw_inwakeup(tp);
			tty_unlock(tp);
		}
	}
}
