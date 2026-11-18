// SPDX-License-Identifier: GPL-2.0+
/*
 * ESP32-P4 SPI NOR read path for the SPL.
 *
 * The flash is reached through the cache/MMU, exactly like the S31 port does:
 * the flash MMU entries map 64 KiB physical pages into the XIP window at
 * 0x40000000, the caches are switched on through the mask ROM helpers, and the
 * image is then read with ordinary loads.
 */

#include <common.h>
#include <asm/io.h>
#include <linux/bitops.h>
#include <linux/delay.h>

void p4_spl_puts(const char *s);
void p4_spl_early_hex(u32 v);

#define P4_MSPI0_BASE		0x5008c000ul

#define MMU_ITEM_CONTENT	(P4_MSPI0_BASE + 0x37c)
#define MMU_ITEM_INDEX		(P4_MSPI0_BASE + 0x380)

#define P4_MMU_PAGE_SIZE	0x10000u
#define P4_MMU_PAGE_MASK	(P4_MMU_PAGE_SIZE - 1u)
#define P4_FLASH_VADDR		0x40000000u
#define P4_MMU_ENTRY_COUNT	1024u
#define P4_MMU_FLASH_VALID	BIT(12)

/* Mask ROM cache helpers. */
#define ROM_CACHE_INIT		0x4fc003dcu
#define ROM_CACHE_EN_ICACHE0	0x4fc004d4u
#define ROM_CACHE_EN_DCACHE	0x4fc004f4u
#define ROM_CACHE_EN_L2		0x4fc00504u

typedef void (*rom_cache_fn)(u32 flags);

int p4_spl_flash_init(u32 size)
{
	u32 pages = DIV_ROUND_UP(size, P4_MMU_PAGE_SIZE);
	rom_cache_fn cache_init = (rom_cache_fn)ROM_CACHE_INIT;
	rom_cache_fn en_icache0 = (rom_cache_fn)ROM_CACHE_EN_ICACHE0;
	rom_cache_fn en_dcache = (rom_cache_fn)ROM_CACHE_EN_DCACHE;
	rom_cache_fn en_l2 = (rom_cache_fn)ROM_CACHE_EN_L2;
	u32 i;

	if (pages > P4_MMU_ENTRY_COUNT)
		return -1;

	cache_init(0);

	/* Bring the caches up first, then populate the flash MMU entries. */
	en_dcache(0);
	en_icache0(0);
	en_l2(0);
	udelay(10);

	for (i = 0; i < pages; i++) {
		writel(i, (void *)MMU_ITEM_INDEX);
		writel((i << 16) | P4_MMU_FLASH_VALID, (void *)MMU_ITEM_CONTENT);
	}
	udelay(100);

	return 0;
}

int p4_spl_nor_read(u32 offset, void *buf, u32 len)
{
	memcpy(buf, (void *)(P4_FLASH_VADDR + offset), len);

	return 0;
}


static void mmu_self_test(void)
{
	u32 i;
	u32 saved_index, saved_content;

	p4_spl_puts("P4SPL:mmu");
	for (i = 0x360; i <= 0x390; i += 4)
		p4_spl_early_hex(readl((void *)(P4_MSPI0_BASE + i)));

	/* Write a known entry and read it back. */
	writel(0x21, (void *)MMU_ITEM_INDEX);
	writel(0x00012345, (void *)MMU_ITEM_CONTENT);
	saved_index = readl((void *)MMU_ITEM_INDEX);
	saved_content = readl((void *)MMU_ITEM_CONTENT);
	p4_spl_puts(" rb ");
	p4_spl_early_hex(saved_index);
	p4_spl_early_hex(saved_content);
	p4_spl_puts("\n");
}

int p4_spl_nor_probe(void)
{
	u32 buf[2];

	mmu_self_test();

	p4_spl_puts("P4SPL:flash ");
	if (p4_spl_flash_init(0x800000)) {
		p4_spl_puts("map failed\n");
		return -1;
	}

	p4_spl_nor_read(0, buf, 8);
	p4_spl_early_hex(buf[0]);
	p4_spl_early_hex(buf[1]);

	p4_spl_puts(" nor10000 ");
	p4_spl_nor_read(0x10000, buf, 8);
	p4_spl_early_hex(buf[0]);
	p4_spl_early_hex(buf[1]);
	p4_spl_puts("\n");

	return 0;
}
