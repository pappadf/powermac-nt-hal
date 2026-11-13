/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 powermac-nt-hal contributors
 *
 * stage0: the big-endian front end, linked at 0x600000 -- in a factory-default firmware's free
 * RAM (Open Firmware keeps 4-5 MB), clear of the ARC firmware's 0x50000 and below everything
 * the ARC firmware keeps at 0xE00000 and up.  NT's loader later loads at 0x600000 too; by then
 * stage0 is gone.  tools/mkxcoff.py wraps the flat .text and puts its entry descriptor, then
 * one page of .data, after it; .bss follows that page, as the gap below leaves room for. */
ENTRY(_start)
SECTIONS
{
    . = 0x00600000;
    .text : {
        KEEP(*(.text.start))
        *(.text .text.*)
        *(.rodata .rodata.* .sdata2 .sdata2.*)
        *(.data .data.* .sdata .sdata.*)
        . = ALIGN(8);
    }
    . = ALIGN(8) + 8;            /* the entry descriptor mkxcoff.py appends */
    . = ALIGN(0x1000) + 0x1000;  /* and its page of .data */
    .bss (NOLOAD) : {
        __bss_start = .;
        *(.sbss .sbss.* .bss .bss.* COMMON)
        . = ALIGN(16);
        __bss_end = .;
    }
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
