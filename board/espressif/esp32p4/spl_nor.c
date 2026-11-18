// SPDX-License-Identifier: GPL-2.0+
/*
 * ESP32-P4 SPI NOR read path for the SPL.
 *
 * Mirrors the ESP32-S31 port: bring the caches up, program the flash MMU for
 * 64 KiB pages, map the first pages of flash into the XIP window at
 * 0x40000000 and read the image with ordinary loads.
 */

#include <common.h>
#include <asm/io.h>
#include <linux/bitops.h>
#include <linux/delay.h>

void p4_spl_puts(const char *s);
void p4_spl_early_hex(u32 v);

#define P4_MSPI0_BASE		0x5008c000ul
#define P4_CLKRST_BASE		0x500e6000ul

#define MMU_ITEM_CONTENT	(P4_MSPI0_BASE + 0x37c)
#define MMU_ITEM_INDEX		(P4_MSPI0_BASE + 0x380)
#define MMU_POWER_CTRL		(P4_MSPI0_BASE + 0x384)

#define MMU_PAGE_SIZE_MASK	(3u << 3)
#define MMU_PAGE_SIZE_64K	(2u << 3)

/* Cache clock force-on bits, as the S31 port uses. */
#define CACHE_CTRL0_REG		(P4_CLKRST_BASE + 0x38)
#define CACHE_CTRL0_FORCE_ON	((1u << 1) | (1u << 4) | (1u << 7) | (1u << 10))

#define P4_MMU_PAGE_SIZE	0x10000u
#define P4_FLASH_VADDR		0x40000000u
#define P4_MMU_ENTRY_COUNT	1024u
#define P4_MMU_FLASH_VALID	BIT(12)

/* Mask ROM helpers (P4 addresses). */
#define ROM_CACHE_INIT		0x4fc003dcu
#define ROM_CACHE_INVALIDATE_ALL	0x4fc00404u
#define ROM_CACHE_DIS_IC0		0x4fc004d0u
#define ROM_CACHE_DIS_IC1		0x4fc004e0u
#define ROM_CACHE_DIS_DC		0x4fc004f0u
#define ROM_CACHE_EN_IC0		0x4fc004d4u
#define ROM_CACHE_EN_IC1		0x4fc004e4u
#define ROM_CACHE_EN_DC			0x4fc004f4u
#define ROM_CACHE_EN_L2			0x4fc00504u

#define CACHE_MAP_L1_ALL	0x3u	/* L1 icache | L1 dcache */

typedef void (*rom_void_fn)(void);
typedef void (*rom_flags_fn)(u32 flags);
typedef void (*rom_invalidate_all_fn)(u32 map);

static void mmu_write_entry(u32 entry, u32 content)
{
	writel(entry, (void *)MMU_ITEM_INDEX);
	writel(content, (void *)MMU_ITEM_CONTENT);
}


static void cache_enable_all(void)
{
	((rom_flags_fn)ROM_CACHE_EN_DC)(0);
	((rom_flags_fn)ROM_CACHE_EN_IC0)(0);
	((rom_flags_fn)ROM_CACHE_EN_IC1)(0);
}

int p4_spl_flash_init(u32 size)
{
	u32 pages = DIV_ROUND_UP(size, P4_MMU_PAGE_SIZE);
	u32 i, val;

	if (pages > P4_MMU_ENTRY_COUNT)
		return -1;

	/* Make sure the cache block clock is up before touching the MMU. */
	setbits_le32((void *)CACHE_CTRL0_REG, CACHE_CTRL0_FORCE_ON);

	p4_spl_puts("init ");
	((rom_void_fn)ROM_CACHE_INIT)();

	p4_spl_puts("inv ");
	((rom_invalidate_all_fn)ROM_CACHE_INVALIDATE_ALL)(CACHE_MAP_L1_ALL);

	p4_spl_puts("pg ");

	/* 64 KiB MMU pages, then a clean table. */
	val = readl((void *)MMU_POWER_CTRL);
	val &= ~MMU_PAGE_SIZE_MASK;
	val |= MMU_PAGE_SIZE_64K;
	writel(val, (void *)MMU_POWER_CTRL);

	p4_spl_puts("tab ");
	for (i = 0; i < P4_MMU_ENTRY_COUNT; i++)
		mmu_write_entry(i, 0);

	/* Linear map: virtual page i -> physical page i. */
	for (i = 0; i < pages; i++)
		mmu_write_entry(i, i | P4_MMU_FLASH_VALID);

	p4_spl_puts("en ");
	cache_enable_all();
	udelay(100);
	p4_spl_puts("ok ");

	return 0;
}

int p4_spl_nor_read(u32 offset, void *buf, u32 len)
{
	memcpy(buf, (void *)(P4_FLASH_VADDR + offset), len);

	return 0;
}

int p4_spl_nor_probe(void)
{
	u32 buf[2];

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
