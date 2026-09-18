/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 powermac-nt-hal contributors
 *
 * adbport.ld — layout for elf2pe: one page of headers, then page-aligned sections.  Drivers
 * link at 0x10000 like the shipped ones and are relocated by the loader; elf2pe emits the
 * base relocations. */
ENTRY(desc_DriverEntry)
SECTIONS
{
  . = 0x00010000 + 0x1000;
  .text : { *(.text .text.*) }
  . = ALIGN(0x1000);
  .rdata : { *(.rodata .rodata.*) }
  . = ALIGN(0x1000);
  .data : { *(.data .data.* .sdata .sdata.* .got2 .got) }
  .bss  : { *(.bss .bss.* .sbss .sbss.* COMMON) }
  . = ALIGN(0x1000);
  .idata : { KEEP(*(.idata)) }
  /DISCARD/ : { *(.comment .note.* .eh_frame .rela.eh_frame .PPC.EMB.apuinfo) }
}
