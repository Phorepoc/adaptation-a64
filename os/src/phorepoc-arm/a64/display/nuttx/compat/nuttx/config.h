/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: EPL-1.0 */

/* OURS: the NuttX environment the vendored A64 display drivers (../../*.c, Apache-2.0, apache/nuttx master,
 * fetched 2026-10-06) compile against inside the Phorepoc kernel. Every NuttX header they include lands here or
 * in an empty stub next to it. Register access goes through the kernel's DPlatChunkHw mappings: the
 * A64_*_ADDR bases (hardware/a64_memorymap.h) are runtime linear addresses that a64_display.cpp fills in. */
#ifndef PH_NUTTX_COMPAT_H
#define PH_NUTTX_COMPAT_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>

#ifndef OK
#define OK 0
#endif
#ifndef ERROR
#define ERROR (-1)
#endif

#define getreg8(a)      (*(volatile uint8_t *)(uintptr_t)(a))
#define getreg32(a)     (*(volatile uint32_t *)(uintptr_t)(a))
/* putreg32: the CCU's bus clock gating (0x060/0x064) and bus soft reset (0x2C0/0x2C4) registers are SHARED by every
 * block -- NuttX's a64_tcon0.c writes TCON0's bit with a plain store, which also gated + reset the Mali GPU (bit 20)
 * that our GPU driver had just brought up (2026-10-09). In those registers a store only ever SETS bits here. */
extern uintptr_t ph_a64_io;
static inline void ph_putreg32(uint32_t v, uintptr_t a)
{
  const uintptr_t off = a - (ph_a64_io + 0x20000);
  if (off == 0x060 || off == 0x064 || off == 0x2c0 || off == 0x2c4)
    v |= *(volatile uint32_t *)a;
  *(volatile uint32_t *)a = v;
}
#define putreg32(v, a)  ph_putreg32((uint32_t)(v), (uintptr_t)(a))
#define modreg32(v, m, a) (*(volatile uint32_t *)(uintptr_t)(a) = \
  (getreg32(a) & ~(uint32_t)(m)) | ((uint32_t)(v) & (uint32_t)(m)))
#define ARM64_DSB()     __asm__ volatile("dsb" ::: "memory")
#define ARM64_ISB()     __asm__ volatile("isb" ::: "memory")
#define UP_DSB()        ARM64_DSB()

#ifdef __cplusplus
extern "C" {
#endif
void ph_mdelay(unsigned ms);
void ph_udelay(unsigned us);
void ph_log(const char* fmt, ...);
uint16_t crc16ccittpart(const uint8_t* src, size_t len, uint16_t crc16val);
#ifdef __cplusplus
}
#endif
#define up_mdelay(ms) ph_mdelay(ms)
#define up_udelay(us) ph_udelay(us)

/* NuttX logging: errors go to the kernel log, the chatty info level is compiled out */
#define gerr(...)    ph_log(__VA_ARGS__)
#define baterr(...)  ph_log(__VA_ARGS__)
#define gwarn(...)   ((void)0)
#define ginfo(...)   ((void)0)
#define batinfo(...) ((void)0)
#define gpioinfo(...) ((void)0)
#define gpioerr(...) ((void)0)
#define ginfodumpbuffer(...) ((void)0)
#define DEBUGASSERT(x) ((void)0)
#define DEBUGPANIC()  ((void)0)
#define UNUSED(x) ((void)(x))
#define FAR
#define enter_critical_section() 0
#define leave_critical_section(f) ((void)(f))
typedef int irqstate_t;

#endif
