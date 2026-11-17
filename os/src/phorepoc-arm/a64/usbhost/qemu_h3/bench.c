/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: EPL-1.0 */

/* bench.c -- bare-metal bench for the FreeBSD USB host stack on QEMU orangepi-pc (Allwinner H3, Cortex-A7):
 * MMU and caches off (physical = virtual, DMA coherent), single CPU, polled. Implements phe_usb_port.h and the
 * few libc/kshim hooks (printf -> UART0, DELAY, pause). The H3's EHCI1/OHCI1 sit at the A64's USB1 addresses
 * (0x01C1B000 / 0x01C1B400). Build with build.sh, run with run.sh. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

#include "../phe_usb_port.h"

#define UART0 0x01C28000u	/* 16550, 32-bit register stride */

extern void usb_init(void);
extern void usb_idle(void);
void phe_ucom_bench(void);	/* next step: an AT loopback through the u3g/ucom Comm-unit layer, when linked */

static void uart_putc(char c)
{
	volatile uint32_t *u = (volatile uint32_t *)UART0;
	while (!(u[5] & 0x20))
		;
	u[0] = (uint8_t)c;
}

/* newlib syscalls */
int _write(int fd, const char *p, int n)
{
	(void)fd;
	for (int i = 0; i < n; i++) {
		if (p[i] == '\n')
			uart_putc('\r');
		uart_putc(p[i]);
	}
	return n;
}
extern char __heap_start[], __heap_end[];
void *_sbrk(int inc)
{
	static char *brk = __heap_start;
	char *old = brk;
	if (brk + inc > __heap_end) {
		errno = ENOMEM;
		return (void *)-1;
	}
	brk += inc;
	return old;
}
int _close(int fd) { (void)fd; return -1; }
int _fstat(int fd, struct stat *st) { (void)fd; st->st_mode = S_IFCHR; return 0; }
int _isatty(int fd) { (void)fd; return 1; }
int _lseek(int fd, int o, int w) { (void)fd; (void)o; (void)w; return 0; }
int _read(int fd, char *p, int n) { (void)fd; (void)p; (void)n; return 0; }
void _exit(int c) { (void)c; for (;;) ; }
int _kill(int p, int s) { (void)p; (void)s; return -1; }
int _getpid(void) { return 1; }

/* clock: the ARMv7 generic timer */
static uint32_t cntfrq;
static uint64_t cntpct(void)
{
	uint32_t lo, hi;
	__asm__ volatile("isb; mrrc p15, 0, %0, %1, c14" : "=r"(lo), "=r"(hi));
	return ((uint64_t)hi << 32) | lo;
}
unsigned int phe_usb_time_us(void)
{
	return (unsigned int)(cntpct() / (cntfrq / 1000000u));
}

/* flat memory, MMU off: bus address = CPU address; the interrupts are polled */
unsigned int phe_usb_v2p(unsigned long va) { return (unsigned int)va; }
void phe_usb_irq_attach(int irq) { (void)irq; }
int phe_usb_board_init(void) { return 0; }
int phe_usb_board_phy(struct phe_usb_phy_cfg *cfg) { (void)cfg; return -1; }

void DELAY(unsigned int us)
{
	uint32_t t0 = phe_usb_time_us();
	while ((uint32_t)(phe_usb_time_us() - t0) < us)
		;
}
void delay(unsigned int us) { DELAY(us); }

/* kshim pause(): sleep `timeout` ticks (hz = 1000) while the stack keeps running */
int pause(const char *what, int timeout)
{
	uint32_t t0 = phe_usb_time_us();
	(void)what;
	if (timeout <= 0)
		timeout = 1;
	while ((uint32_t)(phe_usb_time_us() - t0) < (uint32_t)timeout * 1000u)
		phe_usb_service();
	return 0;
}

/* board: the H3 model's USB1 pair, EHCI1 + OHCI1 (QEMU needs no PHY / CCU programming; it has no companion
 * routing either, so HS devices go on usb-bus.1 = EHCI1, FS/LS ones on usb-bus.5 = OHCI1) */
int phe_usb_board_hc(const char *kind, int n, struct phe_usb_hc_cfg *cfg)
{
	if (n != 0)
		return -1;
	cfg->base = (kind[0] == 'e') ? 0x01C1B000u : 0x01C1B400u;
	cfg->size = 0x400;
	cfg->irq = -1;
	return 0;
}

__attribute__((weak)) void phe_ucom_bench(void) { }

int main(void)
{
	uint32_t t0, dumped = 0;

	__asm__ volatile("mrc p15, 0, %0, c14, c0, 0" : "=r"(cntfrq));
	if (cntfrq == 0)
		cntfrq = 62500000u;
	printf("[usbh] bench: FreeBSD USB (kshim) on QEMU orangepi-pc, cntfrq=%lu\n", (unsigned long)cntfrq);
	usb_init();
	printf("[usbh] usb_init done\n");
	t0 = phe_usb_time_us();
	for (;;) {
		phe_usb_service();
		if (!dumped && (uint32_t)(phe_usb_time_us() - t0) > 6000000u) {
			dumped = 1;
			phe_usb_dump_devices();
			{ volatile uint32_t *o = (volatile uint32_t *)0x01C1B400u; printf("[usbh] ohci ctl=%08lx ists=%08lx ien=%08lx rha=%08lx rhs=%08lx p1=%08lx\n", (unsigned long)o[1], (unsigned long)o[3], (unsigned long)o[4], (unsigned long)o[0x48/4], (unsigned long)o[0x50/4], (unsigned long)o[0x54/4]); }
			printf("[usbh] dump done\n");
			phe_ucom_bench();
			printf("[usbh] bench done\n");
		}
	}
}
