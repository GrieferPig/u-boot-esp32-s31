// SPDX-License-Identifier: GPL-2.0+
/*
 * ESP32-P4 board support (M-mode, noMMU, no OpenSBI).
 *
 * Two phases share this file:
 *  - SPL: runs from internal SRAM, brings PSRAM up and loads U-Boot proper.
 *  - U-Boot proper: runs from PSRAM at CONFIG_TEXT_BASE and boots the kernel
 *    that the SPL placed in PSRAM.
 */

#include <init.h>
#include <errno.h>
#include <spl.h>
#include <stdio.h>
#include <linux/libfdt.h>
#include <asm/global_data.h>

DECLARE_GLOBAL_DATA_PTR;

#define P4_SRAM_TOP		0x4ff80000
#define P4_SRAM_MID_BASE	0x4ff00000
#define P4_SRAM_MID_TOP	0x4ffc0000
#define P4_PSRAM_LOW_TOP	0x480e0000
#define P4_UBOOT_RAM_TOP	0x48f00000

#if defined(CONFIG_SPL_BUILD)
void p4_spl_early_puts(const char *s);
#define P4DBG(s)	p4_spl_early_puts(s)
#else
#define P4DBG(s)	do { } while (0)
#endif


/*
 * The usable RAM depends on how U-Boot was started: the ROM only leaves part
 * of PSRAM mapped, and the SRAM build lives below the SPL.
 */
int dram_init(void)
{
#ifdef CONFIG_ESP32P4_SRAM_ONLY
	gd->ram_base = P4_SRAM_MID_BASE;
	gd->ram_size = P4_SRAM_MID_TOP - P4_SRAM_MID_BASE;
#elif defined(CONFIG_ESP32P4_LOW_RAM_TOP)
	gd->ram_base = 0x48000000;
	gd->ram_size = P4_PSRAM_LOW_TOP - 0x48000000;
#else
	gd->ram_base = 0x48000000;
	gd->ram_size = 0x02000000;	/* 32 MiB of PSRAM */
#endif
	return 0;
}

int dram_init_banksize(void)
{
	gd->bd->bi_dram[0].start = gd->ram_base;
	gd->bd->bi_dram[0].size = gd->ram_size;
	return 0;
}


#if defined(CONFIG_ESP32P4_SRAM_ONLY)
/*
 * The relocation copies U-Boot through the write-back data cache, so the copy
 * has to be pushed to SRAM before the jump and the instruction cache dropped
 * so it re-fetches the relocated code.
 */
void flush_dcache_all(void)
{
	((int (*)(u32))0x4fc00414)(0x10);	/* L1 dcache writeback */
	((int (*)(u32))0x4fc00414)(0x20);	/* L2 writeback */
}
#endif

#if !defined(CONFIG_SPL_BUILD)
void *board_fdt_blob_setup(int *err)
{
	void *fdt = (void *)(uintptr_t)gd->arch.firmware_fdt_addr;

	extern char _end[];

	*err = 0;

	/*
	 * The SPL may hand over without a device tree (a1 = 0); in that case use
	 * the DT appended to this binary (CONFIG_OF_SEPARATE).
	 */
	if (!fdt || fdt_check_header(fdt) != 0)
		return (void *)_end;

	return fdt;
}
#endif

/*
 * U-Boot proper relocates towards the top of RAM, and the SPL places the kernel
 * device tree at 0x48f00000, so keep U-Boot's own allocations in the 3 MiB
 * window between its link address (0x48c00000) and that device tree. The SPL
 * itself must stay inside internal SRAM.
 */
phys_addr_t board_get_usable_ram_top(phys_size_t total_size)
{
	P4DBG("P4SPL:ram-top\n");

	if (spl_phase() == PHASE_SPL)
		return P4_SRAM_TOP;

#ifdef CONFIG_ESP32P4_SRAM_ONLY
	/* U-Boot proper runs from internal SRAM in this configuration. */
	return P4_SRAM_MID_TOP;
#endif
#ifdef CONFIG_ESP32P4_LOW_RAM_TOP
	/* Only the ROM-mapped part of PSRAM is usable in this configuration. */
	return P4_PSRAM_LOW_TOP;
#endif
	return P4_UBOOT_RAM_TOP;
}

void p4_systimer_enable(void);

int board_init(void)
{
	p4_systimer_enable();
	P4DBG("P4SPL:board-init\n");
	return 0;
}


#if defined(CONFIG_ESP32P4_SRAM_ONLY)
/*
 * The ROM arms watchdogs before the SPL runs. The SPL disarms them, but the
 * HP-system watchdog keeps resetting the board while U-Boot idles at the
 * prompt, so disarm and feed every watchdog we know about again here.
 */


static void p4_wdt_off(void)
{
	void *wdt = (void *)0x50116000;		/* LP_WDT / RWDT */
	u32 t;

	writel(0x50d83aa1, wdt + 0x08);		/* WPROTECT */
	writel(0, wdt + 0x04);			/* CONFIG1 */
	writel(0, wdt + 0x00);			/* CONFIG0 */
	writel(0x50d83aa1, wdt + 0x0c);		/* FEED */
	writel(0, wdt + 0x08);

	/*
	 * Super watchdog (SWDT): the ROM arms it and only the ROM can disable it,
	 * but IDF's bootloader switches on its auto-feed, which is what keeps the
	 * board alive while U-Boot idles at the prompt (LP_WDT_SWD_AUTO_FEED_EN,
	 * SWD_CONFIG bit 18, behind the SWD write-protect key).
	 */
	writel(0x50d83aa1, wdt + 0x20);		/* SWD_WPROTECT */
	writel(readl(wdt + 0x1c) | (1u << 18), wdt + 0x1c);
	writel(0, wdt + 0x20);

	for (t = 0; t < 2; t++) {		/* TIMG0/1 MWDT */
		u32 base = 0x500c2000 + t * 0x1000;

		writel(0x50d83aa1, (void *)(base + 0x64));
		writel(readl((void *)(base + 0x48)) & ~(1u << 31),
		       (void *)(base + 0x48));
		writel(0, (void *)(base + 0x40));	/* alarm */
		writel(0x50d83aa1, (void *)(base + 0x60));
		writel(0, (void *)(base + 0x64));
	}
}
#endif

int board_late_init(void)
{
#if defined(CONFIG_ESP32P4_SRAM_ONLY)
	p4_wdt_off();
#endif
#ifdef CONFIG_ESP32P4_HW_IDF_HANDOFF
	puts("P4_UBOOT: IDF hardware handoff (experimental)\n");
#else
	puts("P4_UBOOT: M-mode bootloader active (emulator only)\n");
#endif
	return 0;
}
