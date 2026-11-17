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

#ifndef _STANDALONE
#include <stdint.h>	/* FreeBSD-side files get the types from the kshim instead */
#endif

struct phe_usb_hc_cfg {
	uintptr_t base;		/* controller registers (physical = virtual in the kshim model) */
	uint32_t size;		/* register window */
	int irq;		/* GIC interrupt id, -1 = polled only */
};

/* Board layer: the n-th controller of a kind ("ehci" / "ohci") to attach (0, or -1 when n is past the last one). */
int phe_usb_board_hc(const char *kind, int n, struct phe_usb_hc_cfg *cfg);

/* Host: free-running microsecond clock (wraps). */
uint32_t phe_usb_time_us(void);

/* One service pass: run the kshim's USB processes and the 1 ms callout tick when due.
 * The host calls it from its USB thread (bare metal: the main loop). */
void phe_usb_service(void);

/* Debug: print the device and configuration descriptors of every device attached to every bus. */
void phe_usb_dump_devices(void);

#endif
