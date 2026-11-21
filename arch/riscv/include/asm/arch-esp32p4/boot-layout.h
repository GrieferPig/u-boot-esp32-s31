/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef __ESP32P4_BOOT_LAYOUT_H
#define __ESP32P4_BOOT_LAYOUT_H

/*
 * TRM ch.9 System and Memory: 512 KiB L2 occupies high SRAM.
 * The last 16 KiB of low SRAM is reserved for the ROM/AP handoff.
 */
#define P4_PROPER_BASE		0x4ff00000u
#define P4_SPL_BASE		0x4ff2b000u
#define P4_PROPER_SLOT		(P4_SPL_BASE - P4_PROPER_BASE)
#define P4_LOW_SRAM_TOP		0x4ff3c000u
#define P4_PHYS_SRAM_END	0x4ff40000u
#define P4_RUNTIME_FDT		0x48e10000u
#define P4_RUNTIME_MALLOC	0x48e20000u
#define P4_RUNTIME_MALLOC_END	0x48e60000u
#define P4_RUNTIME_TOP		0x48f00000u
#define P4_L2_LINE_BYTES	128u

#endif
