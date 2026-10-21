/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: Apache-2.0
 *
 * ekern.lds -- linker script for the from-source EKA2 kernel.
 *
 * Lays out ekern.exe as an XIP kernel image:
 *  - the entry section (__this_must_go_at_the_beginning_of_the_kernel_image / _E32Startup
 *    from nk_entry.cia) MUST be first in .text so the kernel entry is at the code base;
 *  - __CTOR_LIST__ is synthesized from .init_array/.ctors in the format the __GCC32__
 *    entry loop expects: a header word (skipped by the loop's pre-increment `ldr,#4!`),
 *    the ctor pointers, then a 0 terminator;
 *  - standard .text/.rodata/.data/.bss.
 *
 * The ELF this produces is converted to a 120-byte-TRomImageHeader XIP image (code_address
 * = the segment base) and dropped in as the ROM primary via tools/rombuild.py --primary.
 * Base address is set at link time with -Ttext (the XIP wrap records it).
 */
OUTPUT_ARCH(arm)
ENTRY(_E32Startup)

/* __code_base__ is supplied at link time with `ld --defsym __code_base__=<addr>`
 * (see tools/kernel/link_ekern.sh). It is the kernel's XIP code base — the address
 * `__this_must_go_at_the_beginning_of_the_kernel_image` runs at, = the ROM offset of the
 * kernel's TRomImageHeader + 120 (the header precedes the code). */
SECTIONS
{
  . = __code_base__;

  .text : ALIGN(4)
  {
    /* The kernel entry vector table must be the very first thing in the image. */
    KEEP(*nk_entry.cia.o(.text))
    KEEP(*nk_entry*(.text .text.*))
    *(.text .text.* .glue_7 .glue_7t .v4_bx)
  }

  .rodata : ALIGN(4) { *(.rodata .rodata.* .gnu.linkonce.r.*) }

  /* Synthesize the GCC-style constructor list the entry loop walks. */
  .ctor_list : ALIGN(4)
  {
    __CTOR_LIST__ = .;
    LONG(-1)                                   /* header (skipped by pre-increment) */
    KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
    KEEP(*(.init_array))
    KEEP(*(SORT(.ctors.*)))
    KEEP(*(.ctors))
    LONG(0)                                    /* terminator */
    __CTOR_END__ = .;
  }
  .dtor_list : ALIGN(4)
  {
    __DTOR_LIST__ = .;
    LONG(-1)
    KEEP(*(.fini_array))
    KEEP(*(.dtors))
    LONG(0)
    __DTOR_END__ = .;
  }

  /* End of the execute-in-place (ROM) part. .data is stored here (LMA) but LINKED to run
   * at the kernel data RAM base (VMA); the bootstrap copies LMA->VMA and zeroes .bss.
   * __rom_end__ = iCodeSize base for iDataAddress; __kern_data_base__ = iKernDataAddress. */
  __rom_end__ = .;
  __code_size__ = __rom_end__ - __code_base__;

  .data __kern_data_base__ : AT(__rom_end__) ALIGN(4)
  {
    __data_start__ = .;
    *(.data .data.* .gnu.linkonce.d.*)
    __data_end__ = .;
  }
  __data_size__ = __data_end__ - __data_start__;
  __data_lma__ = __rom_end__;

  .bss (NOLOAD) : ALIGN(4)
  {
    __bss_start__ = .;
    *(.bss .bss.* .gnu.linkonce.b.* COMMON)
    __bss_end__ = .;
  }
  __bss_size__ = __bss_end__ - __bss_start__;

  /DISCARD/ : { *(.ARM.exidx* .ARM.extab* .comment .note.* .init .fini) }
}
