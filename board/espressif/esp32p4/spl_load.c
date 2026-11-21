// SPDX-License-Identifier: GPL-2.0+
/*
 * Enter the validated U-Boot proper image from the P4 SPL.
 */

#include <asm/arch-esp32p4/boot-layout.h>
#include <linux/delay.h>

#include "spl_hw.h"
#include "spl_load.h"

void p4_spl_enter_uboot(u32 dtb)
{
	typedef void (*uboot_entry_fn)(u32 hartid, u32 fdt);
	uboot_entry_fn entry = (uboot_entry_fn)P4_PROPER_BASE;

	p4_spl_puts("jump: entry ");
	p4_spl_early_hex(P4_PROPER_BASE);
	p4_spl_puts(" dtb ");
	p4_spl_early_hex(dtb);
	p4_spl_puts("\n");

	p4_spl_wdt_finish();
	entry(0, dtb);
}
