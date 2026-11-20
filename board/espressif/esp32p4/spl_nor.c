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
#include "spl_hw.h"
#include <asm/arch-esp32p4/boot-layout.h>
#include "../common/mspi-mmu.h"

void p4_spl_puts(const char *s);
void p4_spl_early_hex(u32 v);

#define P4_MSPI0_BASE		0x5008c000ul
#define P4_CLKRST_BASE		0x500e6000ul

#define MMU_ITEM_CONTENT	(P4_MSPI0_BASE + 0x37c)
#define MMU_ITEM_INDEX		(P4_MSPI0_BASE + 0x380)
#define MMU_POWER_CTRL		(P4_MSPI0_BASE + 0x384)

#define MMU_PAGE_SIZE_MASK	(3u << 2)
/* The reference boot leaves 0x04 here (bit 2), not 0x10 (bit 4). */
#define MMU_PAGE_SIZE_64K	(1u << 2)

/* Cache clock force-on bits, as the S31 port uses. */
#define CACHE_CTRL0_REG		(P4_CLKRST_BASE + 0x38)
#define CACHE_CTRL0_FORCE_ON	((1u << 1) | (1u << 4) | (1u << 7) | (1u << 10))

#define P4_MMU_PAGE_SIZE	0x10000u
#define P4_FLASH_VADDR		0x40000000u
#define P4_MMU_ENTRY_COUNT	1024u
#define P4_MMU_FLASH_VALID	BIT(12)

/* PSRAM side of the MMU (MSPI2). */
#define P4_PSRAM_MSPI_BASE	0x5008e000ul
#define P4_MMU_S_ITEM_CONTENT	(P4_PSRAM_MSPI_BASE + 0x37c)
#define P4_MMU_S_ITEM_INDEX	(P4_PSRAM_MSPI_BASE + 0x380)
#define P4_MMU_S_POWER_CTRL	(P4_PSRAM_MSPI_BASE + 0x384)
#define P4_MMU_PSRAM_VALID	BIT(11)
#define P4_MMU_ACCESS_PSRAM	BIT(10)

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
/* rev1.3 mask-ROM Cache_Set_L2_Cache_Mode(size_idx, ways, line_idx). */
#define ROM_CACHE_SET_L2_MODE		0x4fc003d4u
#define P4_L2_SIZE_512K			11u
#define P4_L2_WAYS			8u
#define P4_L2_LINE_128B			4u
#define P4_CACHE_MAP_L2			(1u << 5)

typedef void (*rom_void_fn)(void);
typedef void (*rom_flags_fn)(u32 flags);

static void mmu_write_entry(u32 entry, u32 content)
{
	esp_mspi_mmu_write((void *)MMU_ITEM_INDEX, (void *)MMU_ITEM_CONTENT,
			   entry, content);
}


void p4_spl_cache_init(void)
{
	u32 i;

	/* Cache geometry and flash table come from the rev1.3 mask ROM API;
	 * use this before any cached PSRAM access. */
	((rom_void_fn)ROM_CACHE_INIT)();
	/* The stack is live. Do not globally invalidate the shared L1 D-cache. */
	clrsetbits_le32((void *)MMU_POWER_CTRL, MMU_PAGE_SIZE_MASK,
			MMU_PAGE_SIZE_64K);
	((void (*)(u32, u32, u32))ROM_CACHE_SET_L2_MODE)(P4_L2_SIZE_512K,
				P4_L2_WAYS, P4_L2_LINE_128B);
	((rom_flags_fn)ROM_CACHE_INVALIDATE_ALL)(P4_CACHE_MAP_L2);
	for (i = 0; i < P4_MMU_ENTRY_COUNT; i++)
		mmu_write_entry(i, 0);
}

int p4_spl_flash_init(u32 size)
{
	u32 pages = DIV_ROUND_UP(size, P4_MMU_PAGE_SIZE);
	u32 i;

	if (pages > P4_MMU_ENTRY_COUNT)
		return -1;
	for (i = 0; i < pages; i++)
		mmu_write_entry(i, i | P4_MMU_FLASH_VALID);
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
