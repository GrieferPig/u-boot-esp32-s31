// SPDX-License-Identifier: GPL-2.0+
/*
 * ESP32-P4 board support (M-mode, noMMU, no OpenSBI).
 *
 * Two phases share this file:
 *  - SPL: runs from internal SRAM, brings PSRAM up and loads U-Boot proper.
 *  - U-Boot proper: runs from internal SRAM and boots the kernel
 *    directly from mapped SPI NOR.
 */

#include <asm/io.h>
#include <asm/sections.h>
#include <init.h>
#include <errno.h>
#include <spl.h>
#include <stdio.h>
#include <linux/libfdt.h>
#include <linux/sizes.h>
#include <asm/global_data.h>
#include <asm/arch-esp32p4/boot-layout.h>
#include <asm/arch-esp32p4/rom-watchdog.h>
#include "psram-handoff.h"
#include "spl_hw.h"

DECLARE_GLOBAL_DATA_PTR;

#define P4_SRAM_TOP		P4_LOW_SRAM_TOP
#define P4_SRAM_MID_BASE	0x4ff00000
#define P4_SRAM_MID_TOP	P4_LOW_SRAM_TOP

#if defined(CONFIG_SPL_BUILD)
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
	if (IS_ENABLED(CONFIG_ESP32P4_SRAM_ONLY) &&
	    !IS_ENABLED(CONFIG_SPL_BUILD)) {
		gd->ram_base = P4_SRAM_MID_BASE;
		gd->ram_size = P4_SRAM_MID_TOP - P4_SRAM_MID_BASE;
	} else {
		gd->ram_base = P4_EXT_RAM_BASE;
		gd->ram_size = SZ_32M;
	}
	return 0;
}

int dram_init_banksize(void)
{
	gd->bd->bi_dram[0].start = gd->ram_base;
	gd->bd->bi_dram[0].size = gd->ram_size;

#if defined(CONFIG_ESP32P4_SRAM_ONLY) && !defined(CONFIG_SPL_BUILD)
	/*
	 * U-Boot itself lives in SRAM, but the kernel's memory - and therefore
	 * the only place a bootm FDT relocation or kernel payload can be placed -
	 * is PSRAM. Report that as a second bank so boot_relocate_fdt() has real
	 * memory to target instead of failing with "device tree - allocation
	 * error" (the SRAM bank is fully occupied by the running image).
	 */
	gd->bd->bi_dram[1].start = 0x48000000;
	gd->bd->bi_dram[1].size = p4_ext_size();
	if (!gd->bd->bi_dram[1].size)
		return -EINVAL;
#endif
	return 0;
}

#if defined(CONFIG_ESP32P4_SRAM_ONLY) && !defined(CONFIG_SPL_BUILD)
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
 * Keep SPL allocations in internal SRAM. Proper's runtime stack and
 * control FDT use PSRAM below the separate Linux DTB reservation.
 */
phys_addr_t board_get_usable_ram_top(phys_size_t total_size)
{
	P4DBG("P4SPL:ram-top\n");

	if (spl_phase() == PHASE_SPL)
		return P4_SRAM_TOP;

	/* The proper image remains in SRAM in either stage. */
	return P4_RUNTIME_TOP;
}

void p4_systimer_enable(void);

/*
 * Watchdogs the ROM leaves armed after a flash boot. RWDT and the super
 * watchdog are cleared through their key registers; TIMG MWDT0/MWDT1 have a
 * flashboot mode that keeps resetting the SoC even with wdt_en clear, and the
 * ROM enables it on every flash boot (TRM ch.18 "Timer Group (TIMG)" -
 * TIMG_WDT_CONFIG0_REG.wdt_flashboot_mod_en; the RWDT/SWD key registers
 * used below are TRM ch.19 "Watchdog Timers (WDT)").
 * A multi-megabyte PSRAM copy takes long
 * enough to trip it, so disable it before any PSRAM work.
 */
#define P4_WDT_WKEY	0x50d83aa1u
#define P4_TIMG0_BASE	0x500c2000u
#define P4_TIMG1_BASE	0x500c3000u

#if !defined(CONFIG_ESP32P4_SPL_WDT_HANDOFF) && !defined(CONFIG_SPL_BUILD)
static void p4_mwdt_off(u32 base)
{
	u32 v;

	writel(P4_WDT_WKEY, (void *)(base + 0x64));	/* WDT_WPROTECT */
	v = readl((void *)(base + 0x48));
	v &= ~(BIT(31) |			/* wdt_en */
	       (3u << 29) | (3u << 27) | (3u << 25) | (3u << 23) |
	       BIT(14) | BIT(13) | BIT(12));	/* flashboot, cpu resets */
	writel(v, (void *)(base + 0x48));
	writel(v | BIT(22), (void *)(base + 0x48));	/* conf_update */
	writel(0, (void *)(base + 0x64));
}

#endif

void p4_watchdogs_off(void)
{
#if defined(CONFIG_SPL_BUILD)
	p4_spl_wdt_feed();
#elif defined(CONFIG_ESP32P4_SPL_WDT_HANDOFF)
	/* SPL left the super watchdog on auto-feed. */
	return;
#else
	void *wdt = (void *)0x50116000;

	writel(P4_WDT_WKEY, wdt + 0x18);		/* RWDT WPROTECT */
	clrbits_le32(wdt + 0x00, BIT(31) | BIT(14));
	writel(0, wdt + 0x18);

	writel(P4_WDT_WKEY, wdt + 0x20);		/* SWD_WPROTECT */
	setbits_le32(wdt + 0x1c, BIT(30));		/* swd_disable */
	writel(0, wdt + 0x20);

	p4_mwdt_off(P4_TIMG0_BASE);
	p4_mwdt_off(P4_TIMG1_BASE);
#endif
}

int board_init(void)
{
	p4_watchdogs_off();
	p4_systimer_enable();
	P4DBG("P4SPL:board-init\n");
	return 0;
}

#if defined(CONFIG_ESP32P4_SRAM_ONLY) && !defined(CONFIG_SPL_BUILD)
/*
 * The ROM leaves several watchdogs armed (RWDT, super watchdog and the TIMG
 * MWDTs). U-Boot feeds them through WATCHDOG_RESET(), which lands here.
 */
void hw_watchdog_reset(void)
{
	void *wdt = (void *)0x50116000;

	writel(P4_WDT_WKEY, wdt + 0x18);
	writel(1, wdt + 0x14);			/* RWDT feed */
	writel(0, wdt + 0x18);

	/* TIMG WDT feed register accepts any value. */
	writel(1, (void *)(P4_TIMG0_BASE + 0x60));
	writel(1, (void *)(P4_TIMG1_BASE + 0x60));
}

void hw_watchdog_init(void)
{
	p4_watchdogs_off();
}
#endif

int board_late_init(void)
{
	puts("P4_UBOOT: native M-mode bootloader active\n");
	if (IS_ENABLED(CONFIG_ESP32P4_SRAM_ONLY))
		printf("P4_UBOOT: PSRAM %u MiB from SPL\n", p4_ext_size() >> 20);
	return 0;
}
