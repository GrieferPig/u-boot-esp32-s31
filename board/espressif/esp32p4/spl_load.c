// SPDX-License-Identifier: GPL-2.0+
/*
 * ESP32-P4 SPL: load U-Boot proper from SPI NOR into PSRAM and hand over.
 */

#include <common.h>
#include <asm/io.h>
#include <linux/bitops.h>
#include <linux/delay.h>

#include "spl_load.h"
#include "spl_nor.h"
#include "ubload.h"

void p4_spl_puts(const char *s);
void p4_spl_early_hex(u32 v);

#define P4_UBOOT_LOAD_ADDR	0x48c00000ul
#define P4_UBOOT_MAX_SIZE	0x00200000ul	/* 2 MB partition */

int p4_spl_load_uboot(void)
{
	u32 size = P4_UBOOT_SIZE;
	u32 crc = 0;
	u32 i;

	if (!size || size > P4_UBOOT_MAX_SIZE)
		return -1;

	p4_spl_puts("load: uboot ");
	p4_spl_early_hex(P4_UBOOT_OFFSET);
	p4_spl_puts(" -> ");
	p4_spl_early_hex(P4_UBOOT_LOAD_ADDR);
	p4_spl_puts(" len ");
	p4_spl_early_hex(size);
	p4_spl_puts("\n");

	if (p4_spl_nor_read(P4_UBOOT_OFFSET, (void *)P4_UBOOT_LOAD_ADDR, size)) {
		p4_spl_puts("load: nor read failed\n");
		return -1;
	}

	/* Simple integrity check over the words we just copied. */
	for (i = 0; i < size / 4; i++)
		crc += readl((void *)(P4_UBOOT_LOAD_ADDR + i * 4));

	p4_spl_puts("load: first words ");
	p4_spl_early_hex(readl((void *)P4_UBOOT_LOAD_ADDR));
	p4_spl_early_hex(readl((void *)(P4_UBOOT_LOAD_ADDR + 4)));
	p4_spl_puts(" sum ");
	p4_spl_early_hex(crc);
	p4_spl_puts("\n");

	return 0;
}

void p4_spl_enter_uboot(u32 dtb)
{
	typedef void (*uboot_entry_fn)(u32 hartid, u32 fdt);

	uboot_entry_fn entry = (uboot_entry_fn)P4_UBOOT_LOAD_ADDR;

	p4_spl_puts("jump: entry ");
	p4_spl_early_hex(P4_UBOOT_LOAD_ADDR);
	p4_spl_puts(" dtb ");
	p4_spl_early_hex(dtb);
	p4_spl_puts("\n");

	udelay(10000);

	entry(0, dtb);
}
