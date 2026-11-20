/* SPDX-License-Identifier: GPL-2.0-or-later */
ENTRY(p4_idf_table)
MEMORY { hw (rwx) : ORIGIN = 0x4ff70000, LENGTH = 64K }
SECTIONS {
 .text : { KEEP(*(.p4_api)) *(.text .text.* .iram* .literal.*) } > hw
 .rodata : { *(.rodata* .srodata* .dram1* .esp_bootloader_desc*) } > hw
 .data : { _data_start = .; *(.data .data.* .sdata*) _data_end = .; } > hw
 .bss (NOLOAD) : { . = ALIGN(4); _bss_start = .; *(.bss .bss.* .sbss* COMMON) . = ALIGN(4); _bss_end = .; } > hw
 ASSERT(_bss_end <= ORIGIN(hw) + LENGTH(hw), "IDF init exceeded reserved SRAM")
}
