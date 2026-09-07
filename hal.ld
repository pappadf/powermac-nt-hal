/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 powermac-nt-hal contributors
 *
 * hal.ld — layout for elf2pe: one page of headers, then page-aligned sections. */
ENTRY(desc_HalInitSystem)
SECTIONS
{
  . = 0x80010000 + 0x1000;
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
