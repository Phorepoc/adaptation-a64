/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: EPL-1.0 */

/* phe_usb_port.h -- what the FreeBSD USB stack (imported unmodified: FreeBSD's own stand/kshim kernel shim +
 * sys/dev/usb core + controller/ehci.c + generic_ehci.c, third_party/freebsd-usb) needs from the host OS.
 * The kshim already supplies mutexes, condvars, callouts, the device/devclass glue, bus_space and
 * bus_dma on a flat physical = virtual memory; this header is the remaining seam, implemented once per host:
 *   qemu_h3/  = a bare-metal bench on QEMU orangepi-pc (MMU off, polled)
 *   (next)    = the Phorepoc kernel extension (DFC thread + NTimer tick, Epoc::AllocPhysicalRam pool).
 * Board facts (controller base / IRQ) come from the board layer through phe_usb_board_hc(). */
#ifndef PHE_USB_PORT_H
#define PHE_USB_PORT_H

/* plain C types only: included by the kshim side (its own typedefs), newlib (the bench) and the Symbian kernel */

struct phe_usb_hc_cfg {
	unsigned long base;		/* controller registers (physical = virtual in the kshim model) */
	unsigned int size;		/* register window */
	int irq;		/* GIC interrupt id, -1 = polled only */
};

/* Board layer: the n-th controller of a kind ("ehci" / "ohci") to attach (0, or -1 when n is past the last one). */
int phe_usb_board_hc(const char *kind, int n, struct phe_usb_hc_cfg *cfg);

/* Board layer: the SoC's USB PHY provider for aw_usbphy.c (phe_usb_fdt.c), or -1 when there is none to set up.
 * reg_name/reg_base = the node's "reg-names" and their mapped windows; clk/rst = the gates and resets the node lists
 * (bit masks set in a CCU register); host_phy = the PHYs to enable in host mode (-1 ends the list). */
#define PHE_USB_MAX_REGS 4
#define PHE_USB_MAX_GATES 4
#define PHE_USB_MAX_PHYS 2
struct phe_usb_gate {
	unsigned long base;
	unsigned int reg;
	unsigned int mask;
};
struct phe_usb_phy_cfg {
	const char *compatible;
	const char *reg_name[PHE_USB_MAX_REGS];
	unsigned long reg_base[PHE_USB_MAX_REGS];
	struct phe_usb_gate clk[PHE_USB_MAX_GATES];
	struct phe_usb_gate rst[PHE_USB_MAX_GATES];
	int host_phy[PHE_USB_MAX_PHYS];
};
int phe_usb_board_phy(struct phe_usb_phy_cfg *cfg);

/* Board layer: bring the controllers up (map registers, clocks, resets, PHY) before usb_init(); 0 = OK. */
int phe_usb_board_init(void);

/* Host: a controller's interrupt (cfg.irq >= 0), bound and serviced by the host's USB thread. */
void phe_usb_irq_attach(int irq);

/* Host: the bus address the controller must see for a CPU address of the stack's memory (identity on the bench). */
unsigned int phe_usb_v2p(unsigned long va);

/* Host: free-running microsecond clock (wraps). */
unsigned int phe_usb_time_us(void);

/* One service pass: run the kshim's USB processes and the 1 ms callout tick when due.
 * The host calls it from its USB thread (bare metal: the main loop). */
void phe_usb_service(void);

/* Debug: print the device and configuration descriptors of every device attached to every bus. */
void phe_usb_dump_devices(void);

/* Serial ports (phe_usb_tty.c): the ttys FreeBSD's ucom drivers (uftdi, u3g) create, index 0.. in attach order.
 * Safe from any thread: they touch only the port's rings and ask for a service run (phe_usb_kick). The host
 * provides phe_usb_kick (run phe_usb_service soon) and may provide phe_usb_tty_notify, called on the USB thread. */
#define PHE_USB_TTY_ATTACHED 1
#define PHE_USB_TTY_GONE 2
#define PHE_USB_TTY_RX 4	/* data to read */
#define PHE_USB_TTY_TX 8	/* room to write */
const char *phe_usb_tty_name(int idx);	/* NULL = no such port */
int phe_usb_tty_open(int idx);
void phe_usb_tty_close(int idx);
int phe_usb_tty_write(int idx, const void *buf, int len);	/* bytes queued, < 0 = error */
int phe_usb_tty_read(int idx, void *buf, int len);
int phe_usb_tty_rx_avail(int idx);
int phe_usb_tty_tx_free(int idx);
void phe_usb_tty_service(void);
void phe_usb_kick(void);
void phe_usb_tty_notify(int idx, int events);

#endif
