/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: EPL-1.0 */

/* phe_usb_tty.h -- the tty layer FreeBSD's ucom (usb_serial.c, unmodified) attaches its ports to, for this port:
 * no tty devices, line discipline or console. Each port is a pair of byte rings that the Comm unit
 * (phe_usb_tty_* in phe_usb_port.h) reads and writes. Provided here: the termios and tty interface ucom, uftdi and
 * u3g use (FreeBSD's names and argument shapes; the values are ours where nothing outside this build sees them),
 * plus the small kernel facilities those files need beyond the kshim (unit numbers, sysctl adders, PPS,
 * console and event-handler hooks as no-ops). The implementation is phe_usb_tty.c. kinc/ maps the FreeBSD header
 * names onto this file. */
#ifndef PHE_USB_TTY_H
#define PHE_USB_TTY_H

#include <bsd_global.h>

typedef unsigned long u_long;
struct thread;
struct ucred;

/* termios */
typedef unsigned int tcflag_t;
typedef unsigned char cc_t;
typedef unsigned int speed_t;
#define NCCS 20
struct termios {
	tcflag_t c_iflag;
	tcflag_t c_oflag;
	tcflag_t c_cflag;
	tcflag_t c_lflag;
	cc_t c_cc[NCCS];
	speed_t c_ispeed;
	speed_t c_ospeed;
};
#define VEOF 0
#define VEOL 1
#define VEOL2 2
#define VERASE 3
#define VINTR 8
#define VQUIT 9
#define VSUSP 10
#define VSTART 12
#define VSTOP 13
#define VMIN 16
#define VTIME 17
/* c_iflag */
#define IGNBRK 0x00000001
#define BRKINT 0x00000002
#define IGNPAR 0x00000004
#define PARMRK 0x00000008
#define INPCK 0x00000010
#define ISTRIP 0x00000020
#define IXON 0x00000200
#define IXOFF 0x00000400
#define IXANY 0x00000800
/* c_cflag */
#define CIGNORE 0x00000001
#define CSIZE 0x00000300
#define CS5 0x00000000
#define CS6 0x00000100
#define CS7 0x00000200
#define CS8 0x00000300
#define CSTOPB 0x00000400
#define CREAD 0x00000800
#define PARENB 0x00001000
#define PARODD 0x00002000
#define HUPCL 0x00004000
#define CLOCAL 0x00008000
#define CCTS_OFLOW 0x00010000
#define CRTS_IFLOW 0x00020000
#define CRTSCTS (CCTS_OFLOW | CRTS_IFLOW)
#define CDTR_IFLOW 0x00040000
#define CDSR_OFLOW 0x00080000
#define CCAR_OFLOW 0x00100000
#define CNO_RTSDTR 0x00200000

/* modem lines (tsw_modem, ttydisc_modem) */
#define SER_DTR 0x0001
#define SER_RTS 0x0002
#define SER_STX 0x0004
#define SER_SRX 0x0008
#define SER_CTS 0x0010
#define SER_DCD 0x0020
#define SER_RI 0x0040
#define SER_DSR 0x0080
#define SER_DELTA(x) ((x) << 8)

/* receive errors (ttydisc_rint flags) */
#define TRE_FRAMING 0x01
#define TRE_PARITY 0x02
#define TRE_OVERRUN 0x04
#define TRE_BREAK 0x08

/* ioctls ucom handles (it passes the rest to the driver) */
#define TIOCSBRK 0x2000747b
#define TIOCCBRK 0x2000747a
#define TIOCSRING 0x20007450
#define TIOCCRING 0x20007451

/* the tty */
struct tty;
typedef int tsw_open_t(struct tty *);
typedef void tsw_close_t(struct tty *);
typedef void tsw_outwakeup_t(struct tty *);
typedef void tsw_inwakeup_t(struct tty *);
typedef int tsw_ioctl_t(struct tty *, u_long, caddr_t, struct thread *);
typedef int tsw_param_t(struct tty *, struct termios *);
typedef int tsw_modem_t(struct tty *, int, int);
typedef void tsw_free_t(void *);
typedef bool tsw_busy_t(struct tty *);
#define TF_INITLOCK 0x0001
#define TF_CALLOUT 0x0002
struct ttydevsw {
	unsigned int tsw_flags;
	tsw_open_t *tsw_open;
	tsw_close_t *tsw_close;
	tsw_outwakeup_t *tsw_outwakeup;
	tsw_inwakeup_t *tsw_inwakeup;
	tsw_ioctl_t *tsw_ioctl;
	tsw_param_t *tsw_param;
	tsw_modem_t *tsw_modem;
	tsw_free_t *tsw_free;
	tsw_busy_t *tsw_busy;
};

#define PHE_USB_TTY_RING 2048
struct phe_usb_ring {
	volatile unsigned int head, tail;	/* head = next write, tail = next read; one producer, one consumer */
	unsigned char buf[PHE_USB_TTY_RING];
};
struct tty {
	struct termios t_termios;
	struct termios t_termios_init_in;
	struct termios t_termios_init_out;
	const struct ttydevsw *t_devsw;
	void *t_devswsoftc;
	struct mtx *t_mtx;
	int t_gone;
	int t_open;
	int t_index;
	char t_name[32];
	struct phe_usb_ring t_rx;		/* device -> reader */
	struct phe_usb_ring t_tx;		/* writer -> device */
};
struct tty *tty_alloc_mutex(struct ttydevsw *tsw, void *softc, struct mtx *mtx);
int tty_makedev(struct tty *tp, struct ucred *cred, const char *fmt, ...);
#define tty_softc(tp) ((tp)->t_devswsoftc)
#define tty_lock(tp) mtx_lock((tp)->t_mtx)
#define tty_unlock(tp) mtx_unlock((tp)->t_mtx)
#define tty_gone(tp) ((tp)->t_gone)
#define tty_opened(tp) ((tp)->t_open)
void tty_rel_gone(struct tty *tp);
void tty_init_console(struct tty *tp, speed_t speed);
int ttydisc_rint(struct tty *tp, char c, int flags);
size_t ttydisc_rint_bypass(struct tty *tp, const void *buf, size_t len);
void ttydisc_rint_done(struct tty *tp);
size_t ttydisc_rint_poll(struct tty *tp);
#define ttydisc_can_bypass(tp) 1
size_t ttydisc_getc(struct tty *tp, void *buf, size_t len);
void ttydisc_modem(struct tty *tp, int open);

/* PPS (ucom keeps the API; no PPS consumer here) */
#define PPS_CAPTUREASSERT 0x01
#define PPS_CAPTURECLEAR 0x02
#define PPS_CAPTUREBOTH 0x03
#define PPS_ABI_VERSION 1
struct pps_params {
	int mode;
};
struct pps_state {
	int ppscap;
	int driver_abi;
	struct mtx *driver_mtx;
	struct pps_params ppsparam;
};
#define pps_init_abi(p) do { } while (0)
#define pps_capture(p) do { } while (0)
#define pps_event(p, e) do { } while (0)
#define pps_ioctl(c, d, p) ENOIOCTL
#define UART_PPS_SIGNAL_MASK 0x0f
#define UART_PPS_DISABLED 0x00
#define UART_PPS_CTS 0x01
#define UART_PPS_DCD 0x02
#define UART_PPS_INVERT_PULSE 0x10
#ifndef ENOIOCTL
#define ENOIOCTL (-3)
#endif

/* the console hooks ucom registers (no USB console here) */
struct consdev;
typedef void cn_probe_t(struct consdev *);
typedef void cn_init_t(struct consdev *);
typedef void cn_term_t(struct consdev *);
typedef int cn_getc_t(struct consdev *);
typedef void cn_putc_t(struct consdev *, int);
typedef void cn_grab_t(struct consdev *);
typedef void cn_ungrab_t(struct consdev *);
struct consdev_ops {
	cn_probe_t *cn_probe;
	cn_init_t *cn_init;
	cn_term_t *cn_term;
	cn_getc_t *cn_getc;
	cn_putc_t *cn_putc;
	cn_grab_t *cn_grab;
	cn_ungrab_t *cn_ungrab;
};
struct consdev {
	const struct consdev_ops *cn_ops;
	char cn_name[16];
	int cn_pri;
	void *cn_arg;
};
size_t strlcat(char *dst, const char *src, size_t size);
#define CN_DEAD 0
#define CN_NORMAL 3
#define CONSOLE_DRIVER(name, ...) \
	static struct consdev name##_consdev __attribute__((unused))
#define cnadd(cp) 0
#define cnremove(cp) do { } while (0)

/* unit numbers */
struct unrhdr;
struct unrhdr *new_unrhdr(int low, int high, struct mtx *mutex);
void delete_unrhdr(struct unrhdr *uh);
int alloc_unr(struct unrhdr *uh);
void free_unr(struct unrhdr *uh, unsigned int item);

/* sysctl adders (no sysctl tree here: the values are not exported) */
struct sysctl_oid;
#define SYSCTL_CHILDREN(x) NULL
#define SYSCTL_ADD_STRING(...) ((struct sysctl_oid *)1)
#define SYSCTL_ADD_INT(...) ((struct sysctl_oid *)1)
#define sysctl_remove_oid(...) 0
#define device_get_sysctl_tree(dev) NULL
#define CTLFLAG_RWTUN 0
#define CTLFLAG_MPSAFE 0

/* event handlers (u3g's mode switch hook on new devices: called directly by our attach path, not by an event) */
#define EVENTHANDLER_REGISTER(...) NULL
#define EVENTHANDLER_DEREGISTER(...) do { } while (0)
#define EVENTHANDLER_PRI_ANY 10000
typedef void *eventhandler_tag;

/* small kernel helpers the drivers use beyond the kshim */
#define MODULE_PNP_INFO(...)
#ifndef bootverbose
#define bootverbose 0
#endif
#define BUS_PROBE_SPECIFIC 0
typedef int boolean_t;
#define min(a, b) ((unsigned int)(a) < (unsigned int)(b) ? (unsigned int)(a) : (unsigned int)(b))
char *strstr(const char *s, const char *find);
int strncmp(const char *a, const char *b, size_t n);
/* the driver keeps its softc past detach (uftdi): the kshim must not free it */
#define device_claim_softc(dev) ((dev)->dev_softc_alloc = 0)
#define device_free_softc(p) free((p), M_DEVBUF)

#define priv_check(td, p) 0
#define PRIV_DRIVER 14

#endif
