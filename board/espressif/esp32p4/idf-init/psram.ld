/* SPDX-License-Identifier: GPL-2.0-or-later */
ENTRY(p4_psram_api)
MEMORY { hw (rwx) : ORIGIN = 0x4ff74000, LENGTH = 48K }
SECTIONS {
 .text : { KEEP(*(.p4_api)) *(.text .text.* .iram* .literal.* .spm.text*) } > hw
 .rodata : { *(.rodata* .srodata* .dram1*) } > hw
 .data : { *(.data .data.* .sdata* .spm.data*) } > hw
 .bss (NOLOAD) : { . = ALIGN(4); _bss_start = .; *(.bss .bss.* .sbss* COMMON) . = ALIGN(4); _bss_end = .; } > hw
 ASSERT(_bss_end <= ORIGIN(hw) + LENGTH(hw), "PSRAM hardware closure exceeded SRAM")
}
