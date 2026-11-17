/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: EPL-1.0 */
/* start.S -- AArch32 entry for the QEMU orangepi-pc bench: SVC mode, IRQ/FIQ masked, stack, clear .bss, main(). */
	.syntax unified
	.arm
	.section .text.start, "ax"
	.global _start
_start:
	cpsid	if
	mrc	p15, 0, r0, c0, c0, 5	/* MPIDR: only CPU 0 runs the bench */
	ands	r0, r0, #3
	bne	1f
	ldr	sp, =__stack_top
	ldr	r0, =__bss_start
	ldr	r1, =__bss_end
	mov	r2, #0
2:	cmp	r0, r1
	strlo	r2, [r0], #4
	blo	2b
	bl	main
1:	wfi
	b	1b
