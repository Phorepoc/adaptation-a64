/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: Apache-2.0 */

// shim.S -- OURS (phone P3): enter the 32-bit Symbian kernel on a Cortex-A53.
//
// The the phone's U-Boot (and QEMU `virt,virtualization=on -cpu cortex-a53`) runs in AArch64 at EL2. Our kernel and
// bootstrap are AArch32 (ARMv7 code, which an A53 runs at EL1 in AArch32 state). An execution-state change needs an
// exception return from a higher EL, so this AArch64 stub, entered at EL2, configures EL2 to host an AArch32 EL1 and
// ERETs into the Symbian bootstrap in SVC mode with the MMU, caches and interrupts off -- exactly the state the
// bootstrap's reset entry expects. Every core runs it: the boot CPU at reset / from U-Boot, the secondaries when the
// bootstrap's PSCI CPU_ON starts them with entry = this shim.
//
//   x0..x3 are ignored (U-Boot's booti/go arguments), EXCEPT a core resume: x0 = a physical resume record, see "CORE RESUME" below. TARGET = the AArch32 bootstrap's physical entry.

#ifndef TARGET
#define TARGET 0x04000000		/* QEMU: our ROM in pflash1 */
#endif

// A64_LEDS (the board's image script only -- QEMU virt has flash at this address): progress on the phone's RGB
// LED (PIO port D at 0x01C2086C: CFG2 +0x08, DAT +0x10; PD18 green, PD19 red, PD20 blue), as the bootstrap's
// STAGE_LED does. White = the shim runs; magenta = entered at EL1 or EL3 (parked); cyan = about to ERET.
#define LED_G	0x040000
#define LED_R	0x080000
#define LED_B	0x100000
	.macro	LED bits
#ifdef A64_LEDS
	cbnz	x15, 7f			// the boot CPU only: the secondaries' writes buried the bootstrap's colours
	ldr	x10, =0x01C2086C
	ldr	w11, [x10, #8]
	ldr	w12, =0x000FFF00
	bic	w11, w11, w12
	ldr	w12, =0x00011100
	orr	w11, w11, w12
	str	w11, [x10, #8]
	ldr	w11, [x10, #0x10]
	ldr	w12, =(LED_R | LED_G | LED_B)
	bic	w11, w11, w12
	ldr	w12, =\bits
	orr	w11, w11, w12
	str	w11, [x10, #0x10]
	dsb	sy
7:
#endif
	.endm

// HOLD: keep the LED's colour 1 s (24 MHz CNTPCT) so a person can read it -- boot CPU only (MPIDR Aff0 = 0): the
// secondaries also enter here, and the kernel gives each 0.5 s to check in. A frozen counter = stuck on this colour.
	.macro	HOLD
#ifdef A64_LEDS
	mrs	x13, mpidr_el1
	ands	x13, x13, #0xff
	b.ne	9f
	mrs	x13, cntpct_el0
	ldr	x14, =24000000
	add	x14, x13, x14
8:	mrs	x13, cntpct_el0
	cmp	x13, x14
	b.lo	8b
9:
#endif
	.endm

	.text
	.global	_start
_start:
	mrs	x15, mpidr_el1
	and	x15, x15, #0xff		// Aff0: 0 = the boot CPU (LED / HOLD only there)
	LED	(LED_R | LED_G | LED_B)
	HOLD
	mrs	x9, CurrentEL
	lsr	x9, x9, #2
	cmp	x9, #2
	b.eq	2f			// only EL2 entry is supported (QEMU virtualization=on; phone U-Boot)
	cmp	x9, #3
	b.eq	3f
	LED	(LED_R | LED_B)		// EL1: magenta (parked)
	b	1f
3:	LED	(LED_R | LED_B)		// EL3: magenta
	b	1f
2:

	// EL1 runs AArch32 (RW=0); no stage 2, no traps of EL1 system registers.
	mov	x9, #0
	msr	hcr_el2, x9
	// EL1/EL0 may use the physical timer and counter (our tick is CNTP; the timestamp reads CNTPCT)
	mov	x9, #3			// EL1PCTEN | EL1PCEN
	msr	cnthctl_el2, x9
	msr	cntvoff_el2, xzr
	// no EL2 traps of FP/SIMD or coprocessor access
	mov	x9, #0x33ff
	msr	cptr_el2, x9
	msr	hstr_el2, xzr
	// EL1's view of the CPU id = the real one (VMPIDR/VPIDR are what EL1 reads under EL2)
	mrs	x9, midr_el1
	msr	vpidr_el2, x9
	mrs	x9, mpidr_el1
	msr	vmpidr_el2, x9
	// PMU (the kernel's CPU clock probe, a64_glue.cpp A64CpuProbe): MDCR_EL2 resets UNKNOWN, so TPM/TPMCR may trap
	// every EL1 PMU access to EL2 -- where we have no vectors. HPMN = all counters (PMCR_EL0.N), no traps.
	mrs	x9, pmcr_el0
	ubfx	x9, x9, #11, #5
	msr	mdcr_el2, x9
	// A53 IMPLEMENTATION DEFINED registers: let EL1 write CPUECTLR (ACTLR_EL2 bit 1) and L2CTLR/L2ECTLR (bits 4, 5;
	// reads are always allowed). Only takes effect if ACTLR_EL3 grants the same to EL2 (TF-A's choice).
	mrs	x9, actlr_el2
	mov	x10, #0x32
	orr	x9, x9, x10
	msr	actlr_el2, x9
#ifndef A64_NO_SMPEN
	// CPUECTLR_EL1.SMPEN (bit 6): the A53 TRM requires it set BEFORE the caches and MMU are enabled; without it the
	// core is not coherent with the cluster. TF-A's cortex_a53 reset handler normally sets it on every core; if it is
	// clear here, set it (the write is skipped when already set, so a firmware that denies EL2 the write -- ACTLR_EL3
	// bit 1 clear -- is only hit in the broken case). A64_NO_SMPEN = leave it alone.
	mrs	x9, s3_1_c15_c2_1
	tbnz	x9, #6, 6f
	orr	x9, x9, #(1 << 6)
	msr	s3_1_c15_c2_1, x9
	isb
6:
#endif
	// SCTLR for AArch32 EL1: MMU/caches off, the RES1 bits set
	ldr	x9, =0x00c50838
	msr	sctlr_el1, x9
	// return to AArch32 SVC (M[4]=1 AArch32, mode 0x13), A/I/F masked
	mov	x9, #0x1d3
	msr	spsr_el2, x9
	ldr	x9, =TARGET
	// CORE RESUME (power plan P5, src/phorepoc-arm/a64/power/a64_idle.cpp): a core the kernel retired (PSCI CPU_OFF) is
	// restarted by PSCI CPU_ON(cpu, this shim, context_id); ARM TF-A hands context_id over in x0. A resume passes the
	// PHYSICAL address of a 16-byte-aligned record {'PHRS', entry, r0, -}: ERET straight to `entry` (the kernel's AP boot
	// code, APBootCodeEntry) with r0 = the register set it restores -- not to the bootstrap. Every other entry has x0 =
	// 0 (the bootstrap's CPU_ON), U-Boot's argc (`go`) or QEMU's DTB pointer: those fail the range/alignment/magic checks
	// (the magic read is only attempted inside DRAM, 1 GiB .. 3 GiB).
	ldr	x10, =0x40000000
	cmp	x0, x10
	b.lo	20f
	ldr	x10, =0xBFFFFFF0
	cmp	x0, x10
	b.hi	20f
	tst	x0, #0xf
	b.ne	20f
	ldr	w10, [x0]
	ldr	w11, =0x53524850	// 'PHRS'
	cmp	w10, w11
	b.ne	20f
	ldr	w9, [x0, #4]		// entry
	ldr	w0, [x0, #8]		// r0 for the AArch32 side
20:
#ifdef A64_LEDS
	// boot CPU: ERET into the A32 probe below first (green 1 s, then on to TARGET) -- proves the AArch32 switch
	mrs	x10, mpidr_el1
	ands	x10, x10, #0xff
	b.ne	5f
	adr	x9, a32probe
5:
#endif
	msr	elr_el2, x9
	LED	(LED_G | LED_B)		// cyan: the ERET into the AArch32 bootstrap is next
	HOLD
	isb
	eret

1:	wfe				// unsupported entry level: park
	b	1b

#ifdef A64_LEDS
// a32probe: hand-encoded A32 (this file is AArch64 assembly). Entered by the ERET in AArch32 SVC: LED green, hold
// ~1 s on CNTPCT (24 MHz), then magenta, then BX to the bootstrap at TARGET. Green after cyan = the execution-state
// switch works (seen on the phone 2026-10-05). Then a word checksum of the whole ROM as it sits in RAM, compared with
// the file's (the board's image script patches ROMLEN / ROMSUM, placeholders 0xC6C6C6C6 / 0xC5C5C5C5): LED off = the
// ROM in RAM is intact, blue = it is NOT (U-Boot's load / cache flush left it corrupt). Then BX to TARGET.
	.balign	4
a32probe:
	.word	0xE59F007C		// ldr   r0, [pc, #124]     ; =0x01C2087C (PIO port D DAT)
	.word	0xE5901000		// ldr   r1, [r0]
	.word	0xE3C11707		// bic   r1, r1, #0x1C0000  ; LEDs off
	.word	0xE3811701		// orr   r1, r1, #0x040000  ; green (PD18)
	.word	0xE5801000		// str   r1, [r0]
	.word	0xEC532F0E		// mrrc  p15, 0, r2, r3, c14 ; CNTPCT
	.word	0xE59F5068		// ldr   r5, [pc, #104]     ; =24000000
	.word	0xEC576F0E		// 1: mrrc p15, 0, r6, r7, c14
	.word	0xE0466002		// sub   r6, r6, r2
	.word	0xE1560005		// cmp   r6, r5
	.word	0x3AFFFFFB		// blo   1b
	.word	0xE59F2058		// ldr   r2, [pc, #88]      ; =TARGET
	.word	0xE59F9058		// ldr   r9, [pc, #88]      ; =ROMLEN
	.word	0xE0899002		// add   r9, r9, r2         ; end
	.word	0xE3A07000		// mov   r7, #0             ; sum
	.word	0xE1A08002		// mov   r8, r2
	.word	0xE4983004		// 2: ldr r3, [r8], #4
	.word	0xE0877003		// add   r7, r7, r3
	.word	0xE1580009		// cmp   r8, r9
	.word	0x3AFFFFFB		// blo   2b
	.word	0xE59F403C		// ldr   r4, [pc, #60]      ; =ROMSUM
	.word	0xE1570004		// cmp   r7, r4
	.word	0xE5901000		// ldr   r1, [r0]
	.word	0xE3C11707		// bic   r1, r1, #0x1C0000  ; equal: LED off
	.word	0x13811704		// orrne r1, r1, #0x100000  ; differs: blue
	.word	0xE5801000		// str   r1, [r0]
	.word	0xEC532F0E		// mrrc  p15, 0, r2, r3, c14 ; hold the verdict 1 s
	.word	0xEC576F0E		// 3: mrrc p15, 0, r6, r7, c14
	.word	0xE0466002		// sub   r6, r6, r2
	.word	0xE1560005		// cmp   r6, r5
	.word	0x3AFFFFFB		// blo   3b
	.word	0xE59F2008		// ldr   r2, [pc, #8]       ; =TARGET
	.word	0xE12FFF12		// bx    r2                 ; the bootstrap
	.word	0x01C2087C
	.word	24000000
	.word	TARGET
	.word	0xC6C6C6C6		// ROMLEN (patched)
	.word	0xC5C5C5C5		// ROMSUM (patched)
#endif
