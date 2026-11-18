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
unsigned long notrace timer_read_counter(void);

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

	/* Compare the cache block with a working boot. */
	{
		u32 off;

		p4_spl_puts("P4C blk");
		for (off = 0; off <= 0x60; off += 4) {
			p4_spl_puts(" ");
			p4_spl_early_hex(off);
			p4_spl_puts("=");
			p4_spl_early_hex(readl((void *)(0x3ff10000 + off)));
		}
		p4_spl_puts("\nP4C mmu384 ");
		p4_spl_early_hex(readl((void *)0x5008e384));
		p4_spl_early_hex(readl((void *)0x5008f384));
		p4_spl_puts("\n");
	}

	/* Verify that the flash MMU entries really hold what we wrote. */
	{
		u32 i, bad = 0;

		for (i = 0; i < 128; i++) {
			writel(i, (void *)0x5008c380);
			if ((readl((void *)0x5008c37c) & 0x7ff) != i)
				bad++;
		}
		p4_spl_puts("mmuverify bad=");
		p4_spl_early_hex(bad);
		p4_spl_puts("\n");
	}

	/* Check the payload address itself before copying anything. */
	p4_spl_puts("payload ");
	writel(0x12345678, (void *)P4_UBOOT_LOAD_ADDR);
	p4_spl_early_hex(readl((void *)P4_UBOOT_LOAD_ADDR));
	p4_spl_puts("\n");

	/* Copy in chunks so progress is visible on the console. */
	{
		u32 done = 0;

		p4_spl_puts("copy ");
		while (done < size) {
			u32 chunk = size - done;
			u32 t0, t1;

			if (chunk > 0x4000)
				chunk = 0x4000;
			t0 = timer_read_counter();
			if (p4_spl_nor_read(P4_UBOOT_OFFSET + done,
					    (void *)(P4_UBOOT_LOAD_ADDR + done),
					    chunk)) {
				p4_spl_puts("nor read failed\n");
				return -1;
			}
			t1 = timer_read_counter();
			done += chunk;
			p4_spl_puts(" [");
			p4_spl_early_hex(t1 - t0);
			p4_spl_puts("]");
		}
		p4_spl_puts(" done\n");
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
