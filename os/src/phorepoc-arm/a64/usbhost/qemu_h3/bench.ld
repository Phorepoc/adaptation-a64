/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: EPL-1.0 */
/* bench.ld -- the bench runs from DRAM at 0x40000000 (QEMU -kernel loads the ELF there). */
ENTRY(_start)
SECTIONS
{
	. = 0x40000000;
	.text : { *(.text.start) *(.text*) *(.rodata*) }
	.ARM.exidx : { *(.ARM.exidx*) }
	.data : { *(.data*) }
	. = ALIGN(8);
	__bss_start = .;
	.bss : { *(.bss*) *(COMMON) }
	. = ALIGN(8);
	__bss_end = .;
	. = ALIGN(4096);
	__heap_start = .;
	. += 0x400000;
	__heap_end = .;
	. += 0x10000;
	__stack_top = .;
	/DISCARD/ : { *(.debug.sysinit) }
}
