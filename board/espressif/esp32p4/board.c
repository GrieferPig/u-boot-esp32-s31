// SPDX-License-Identifier: GPL-2.0+
/*
 * ESP32-P4 board support (M-mode, noMMU, no OpenSBI).
 *
 * Two phases share this file:
 *  - SPL: runs from internal SRAM, brings PSRAM up and loads U-Boot proper.
 *  - U-Boot proper: runs from internal SRAM and boots the kernel
 *    that the SPL placed in cached PSRAM.
 */

#if defined(CONFIG_SPL_BUILD)
#include "spl_hw.h"
#endif
#include <asm/io.h>
#include <init.h>
#include <errno.h>
#include <spl.h>
#include <stdio.h>
#include <linux/libfdt.h>
#include <asm/global_data.h>
#include <asm/arch-esp32p4/boot-layout.h>
#include "psram-handoff.h"
#include "cache-range.h"
#if defined(CONFIG_ESP32P4_SRAM_ONLY) && !defined(CONFIG_SPL)
#include <u-boot/sha256.h>
#endif

DECLARE_GLOBAL_DATA_PTR;

#define P4_SRAM_TOP		P4_LOW_SRAM_TOP
#define P4_SRAM_MID_BASE	0x4ff00000
#define P4_SRAM_MID_TOP	P4_LOW_SRAM_TOP
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

#if defined(CONFIG_ESP32P4_SRAM_ONLY)
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
	/* Retained code in SRAM; only the runtime stack and GD use PSRAM. */
	return P4_RUNTIME_TOP;
#endif
#ifdef CONFIG_ESP32P4_LOW_RAM_TOP
	/* Only the ROM-mapped part of PSRAM is usable in this configuration. */
	return P4_PSRAM_LOW_TOP;
#endif
	return P4_UBOOT_RAM_TOP;
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



#if defined(CONFIG_ESP32P4_SRAM_ONLY)
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


#if !defined(CONFIG_SPL)
#include <command.h>

/*
 * Only the SPL's validated FIT places the kernel and its DTB in PSRAM.
 * The final FDT lives at a separate fixed address required by Linux's
 * AP mailbox and reserved-memory node. Use the cached address at
 * every CPU access; bounded cache operations republish changed data.
 */
#define P4_KERN_RUN		P4_FIT_IMAGE
#define P4_DTB_KERN		0x48f00000u
#define P4_DTB_STAGE_BYTES	0x4000u

/*
 * Internal SRAM BSS, checked against proper's live early stack/GD/heap
 * by the linker. Staging avoids in-place FDT mutation in PSRAM.
 */
static u8 p4_dtb_stage[P4_DTB_STAGE_BYTES] __aligned(P4_L2_LINE_BYTES);

/*
 * Firmware secondary-hart mailbox, published to the kernel as
 * /chosen espressif,cpu1-boot-mailbox. Layout and values must match both
 * the retained IDF loader (main/handoff_contract.h: magic 0x534D5034,
 * 28 bytes) and the kernel's p4_prepare_secondary check (magic, ready==1,
 * release==0, status==0, entry==0x48000000, dtb==0x48f00000, revision==103;
 * address inside [0x4FF00000, 0x4FF40000), 64-byte aligned). 0x4FF3C000 is
 * clear of U-Boot proper (~0x4FF00000+0x27c20) and the SPL (0x4FF40000+).
 */
#define P4_MBOX_ADDR		0x4ff3c000u
#define P4_MBOX_SIZE		28u
#define P4_MBOX_MAGIC		0x534d5034u
/* Hart1 waiter home: the HP mask ROM jumps here via its appcpu hook. */
#define P4_LPRAM_WAIT		0x50108000u
#define P4_LPCLKRST_CTRL0	0x50111014u
#define P4_RST_EN_CORE1		0x500e60c0u
#define P4_CORE1_CLK		0x500e6014u
#define P4_LP_RST_CAUSE		0x50111010u
#define P4_LPMEM_FORCE		0x5011102cu
/*
 * IDF start_other_core path (esp_system cpu_start.c, hal cpu_utility_ll.h):
 * hart1 boots the HP ROM at POR and parks there; IDF unstalls it, enables
 * its clock, releases its reset, then publishes the APP entry through the
 * ROM's appcpu boot hook (ets_set_appcpu_boot_addr, 0x4fc000a8), which the
 * parked hart1 spins on. PMU_CPU_SW_STALL_REG field [23:16]: 0x86 stalls,
 * 0xFF resumes; HP_SYSTEM CORESTALLED_ST bit 1 reports the stall.
 */
#define P4_PMU_SW_STALL		0x50115200u
#define P4_CORE_STALLED_ST	0x500e5064u
#define P4_ROM_APPBOOT		0x4fc000a8u

/*
 * Linux retains this SRAM callback for executable data. It serializes
 * it across harts and does its own instruction fence. Complete D-cache
 * writeback before L2 writeback, never evict dirty data here.
 */
int p4_cache_sync_stub(unsigned int start, unsigned int bytes)
{
	u32 size = p4_ext_size();

	if (!size || start < P4_EXT_RAM_BASE ||
	    start - P4_EXT_RAM_BASE >= size ||
	    bytes > size - (start - P4_EXT_RAM_BASE))
		return -EINVAL;
	return p4_cache_writeback(start, bytes) ? -EIO : 0;
}

/*
 * Hart1 firmware waiter, copied to LP RAM by p4boot and entered via
 * ets_set_appcpu_boot_addr with no secondary software reset. Mirrors the IDF
 * loader's secondary_wait (main/p4_hardware_loader.c): M-mode, interrupts
 * off out of reset; open user-PSRAM PMP entry 7 over exactly the mapped
 * aperture (PMP_L|NAPOT|R|W|X, byte 7 = 0x9f). Do not inherit the IDF
 * app's different FreeRTOS policy; report 0/1 in mailbox status; then
 * poll release and jump to the kernel image entry, whose head selects the
 * secondary path itself (arch/riscv/kernel/head.S esp32p4_secondary_start).
 * Both HP harts share L1D, so the SRAM mailbox needs no cache management.
 * Written with the assembler (never hand-encoded) and position-independent.
 */
/* No stack, function calls or absolute code references: this entire waiter
 * moves into LP RAM. Limit polling so a full UART cannot stop CPU bringup.
 * t2-t6 are disposable at each marker; t0, t1, a0 and a1 are preserved.
 * Numeric labels are local to each expansion. TXFIFO_CNT = STATUS[23:16]. */
#define P4_WAITER_MARK(value) \
 "	lui t4, 0x500ca\n" \
 "	li t5, 100\n" \
 "	li t2, 128\n" \
 "	li t3, " #value "\n" \
 "90:	lw t6, 0x1c(t4)\n" \
 "	srli t6, t6, 16\n" \
 "	andi t6, t6, 0xff\n" \
 "	bltu t6, t5, 91f\n" \
 "	addi t2, t2, -1\n" \
 "	bnez t2, 90b\n" \
 "	j 92f\n" \
 "91:	sw t3, 0(t4)\n" \
 "92:\n"

__asm__(
".pushsection .text.hart1waiter,\"ax\"\n"
".global p4_waiter_start\n"
"p4_waiter_start:\n"
"	lui t0, 0x4ff3c\n"		/* mailbox 0x4ff3c000 */
"	li t1, 1\n"
"	sw t1, 16(t0)\n"		/* marker: entry = hartid (alive) */
"	fence\n"
P4_WAITER_MARK(0x57)			/* W: entered */
"1:	lw t1, 0(t0)\n"		/* magic published? */
"	li t2, 0x534d5034\n"
"	bne t1, t2, 1b\n"
"	lw t1, 28(t0)\n"		/* private waiter field, not Linux ABI */
"	li t2, 0x121fffff\n"		/* 16 MiB NAPOT */
"	beq t1, t2, 6f\n"
"	li t2, 0x123fffff\n"		/* 32 MiB NAPOT */
"	beq t1, t2, 6f\n"
"	li t2, 0x127fffff\n"		/* 64 MiB NAPOT */
"	bne t1, t2, 2f\n"
"6:\n"
"	csrr t2, 0x3a1\n"
"	srli t2, t2, 24\n"
"	bnez t2, 2f\n"		/* never modify a pre-existing policy */
"	csrw 0x3b7, t1\n"
"	lui t1, 0x9f000\n"		/* entry7 = L|NAPOT|R|W|X */
"	csrrs zero, 0x3a1, t1\n"
"	csrr t1, 0x3b7\n"		/* verify PMP took */
"	lw t2, 28(t0)\n"
"	bne t1, t2, 2f\n"
"	csrr t1, 0x3a1\n"
"	srli t1, t1, 24\n"
"	li t2, 0x9f\n"
"	beq t1, t2, 3f\n"
"2:	li t1, 1\n"
"	sw t1, 12(t0)\n"		/* status = 1 (PMP failure) */
"4:	wfi\n"
"	j 4b\n"
"3:	sw zero, 12(t0)\n"		/* status = 0 (PMP ok) */
"	li t1, 1\n"
"	sw t1, 4(t0)\n"			/* ready = 1 (IRQ-off and parked) */
P4_WAITER_MARK(0x50)			/* P: ready/parked */
"5:	lw t1, 8(t0)\n"		/* release? */
"	beqz t1, 5b\n"
"	lw t1, 12(t0)\n"		/* paranoia: jump only on success */
"	bnez t1, 4b\n"
"	fence\n"
"	lw t1, 16(t0)\n"		/* entry */
"	lw a1, 20(t0)\n"		/* dtb */
"	li a0, 1\n"
P4_WAITER_MARK(0x4a)			/* J: release seen, about to jump */
"	jr t1\n"
".global p4_waiter_end\n"
"p4_waiter_end:\n"
".popsection\n"
);
#undef P4_WAITER_MARK
extern char p4_waiter_start[];
extern char p4_waiter_end[];

static const struct p4_fit_handoff *p4_get_fit(void)
{
	const struct p4_fit_handoff *fit = (void *)P4_FIT_HANDOFF;

	if (!p4_ext_size() || READ_ONCE(fit->magic) != P4_FIT_MAGIC)
		return NULL;
	asm volatile("fence r,r" ::: "memory");
	if (fit->image_len < 64 || fit->image_len > P4_FIT_IMAGE_LIMIT ||
	    fit->dtb_len < sizeof(struct fdt_header) ||
	    fit->dtb_len > P4_FIT_DTB_LIMIT)
		return NULL;
	return fit;
}

static bool p4_fit_sha_ok(const void *data, u32 size, const u8 *expected)
{
	sha256_context ctx;
	u8 actual[SHA256_SUM_LEN];
	const u8 *p = data;

	sha256_starts(&ctx);
	while (size) {
		u32 n = min_t(u32, size, 0x4000u);

		sha256_update(&ctx, p, n);
		size -= n;
		p += n;
	}
	sha256_finish(&ctx, actual);
	return !memcmp(expected, actual, sizeof(actual));
}

/* Obtain the range from the verified RISC-V Image header; check that
 * its in-memory extent contains the exact original FIT file bytes.
 */
static u32 p4_image_span(const struct p4_fit_handoff *fit)
{
	const u8 *img = (void *)P4_KERN_RUN;
	u32 span;

	if (p4_cache_discard_data(P4_KERN_RUN, P4_L2_LINE_BYTES) ||
	    readl(img + 0x38) != 0x05435352u || readl(img + 0x14))
		return 0;
	span = readl(img + 0x10);
	return span >= fit->image_len && span <= P4_FIT_IMAGE_LIMIT ? span : 0;
}

/* Copy the checked FIT FDT into SRAM before mutating it. Pack the
 * declared structure/strings, then copy it once into cached PSRAM.
 * Only the data range we own is cleaned/invalidated. A subsequent
 * full byte comparison forces a refill, not a read of dirty stores.
 */
static int p4_prep_dtb(const struct p4_fit_handoff *fit)
{
	const void *src = (void *)P4_FIT_DTB;
	void *dst = (void *)P4_DTB_KERN;
	void *fdt = p4_dtb_stage;
	u32 size = p4_ext_size(), final;
	int chosen, mem;
	fdt32_t rev, entry, mbox[2], reg[2];

	if (p4_cache_discard_data(P4_FIT_DTB, fit->dtb_len) ||
	    !p4_fit_sha_ok(src, fit->dtb_len, fit->dtb_hash) ||
	    fdt_check_full(src, fit->dtb_len)) {
		puts("p4: FIT DTB cached reload invalid\n");
		return -EINVAL;
	}

	if (fdt_open_into(src, fdt, sizeof(p4_dtb_stage))) {
		puts("p4: DTB does not fit into SRAM staging\n");
		return -EINVAL;
	}
	mem = fdt_path_offset(fdt, "/memory@48000000");
	reg[0] = cpu_to_fdt32(P4_EXT_RAM_BASE);
	reg[1] = cpu_to_fdt32(size);
	if (mem < 0 || fdt_address_cells(fdt, 0) != 1 ||
	    fdt_size_cells(fdt, 0) != 1 ||
	    fdt_setprop(fdt, mem, "reg", reg, sizeof(reg))) {
		puts("p4: invalid DTB memory node\n");
		return -EINVAL;
	}

	chosen = fdt_path_offset(fdt, "/chosen");
	if (chosen < 0) {
		puts("p4: no /chosen in dtb\n");
		return -EINVAL;
	}
	rev = cpu_to_fdt32(103);
	entry = cpu_to_fdt32((u32)p4_cache_sync_stub);
	mbox[0] = cpu_to_fdt32(P4_MBOX_ADDR);
	mbox[1] = cpu_to_fdt32(P4_MBOX_SIZE);
	if (fdt_setprop(fdt, chosen, "espressif,chip-revision", &rev, 4) ||
	    fdt_setprop(fdt, chosen, "espressif,cache-sync-entry", &entry, 4) ||
	    fdt_setprop(fdt, chosen, "espressif,cpu1-boot-mailbox", mbox, 8) ||
	    fdt_pack(fdt)) {
		puts("p4: DTB fixup/pack failed\n");
		return -EINVAL;
	}
	final = fdt_totalsize(fdt);
	if (final < sizeof(struct fdt_header) || final > sizeof(p4_dtb_stage) ||
	    fdt_check_full(fdt, final))
		return -EINVAL;
	memcpy(dst, fdt, final);
	if (p4_cache_republish(P4_DTB_KERN, final)) {
		puts("p4: final cached DTB sync failed\n");
		return -EIO;
	}
	if (fdt_check_full(dst, final) || memcmp(dst, fdt, final)) {
		puts("p4: final cached DTB reload mismatch\n");
		return -EIO;
	}
	printf("p4: handoff stub @ %08x mbox @ %08x\n",
	       (u32)p4_cache_sync_stub, P4_MBOX_ADDR);
	printf("P4 DTB-CACHED: %u byte reload OK, mem %08x/%08x\n",
	       final, P4_EXT_RAM_BASE, size);
	return 0;
}

/*
 * Publish the firmware mailbox (status = 2 = pending until the waiter
 * proves itself), then park hart1 in the LP-RAM waiter: install,
 * read back, unstall it, enable its clock, publish the ROM boot hook,
 * then deassert reset. Then wait for it to report parked.
 * Returns 1 when hart1 is parked. On failure clear the magic and refuse
 * to enter this SMP kernel.
 */
static int p4_park_hart1(void)
{
	volatile u32 *mbox = (volatile u32 *)P4_MBOX_ADDR;
	unsigned long wlen = (unsigned long)(p4_waiter_end -
					     p4_waiter_start);
	unsigned int spin = 0;
	u32 size = p4_ext_size();
	int alive = 0;
	int smp = 0;

	if (!size)
		return 0;

	/*
	 * A waiter parked by an earlier p4park in this session is still
	 * valid: it only polls release, which only the kernel sets. Reuse
	 * it untouched (refilling ready/status under it would hide its
	 * parked report forever).
	 */
	if (mbox[0] == P4_MBOX_MAGIC && mbox[1] == 1 && mbox[2] == 0 &&
	    mbox[3] == 0 && mbox[4] == P4_KERN_RUN &&
	    mbox[5] == P4_DTB_KERN && mbox[6] == 103 &&
	    mbox[7] == p4_ext_pmpaddr(size)) {
		printf("p4: waiter already parked, reusing\n");
		return 1;
	}

	mbox[1] = 0;			/* ready */
	mbox[2] = 0;			/* release */
	mbox[3] = 2;			/* status = pending */
	mbox[4] = P4_KERN_RUN;		/* entry */
	mbox[5] = P4_DTB_KERN;		/* dtb */
	mbox[6] = 103;			/* revision */
	mbox[7] = p4_ext_pmpaddr(size);	/* private to the waiter */
	asm volatile("fence" ::: "memory");
	mbox[0] = P4_MBOX_MAGIC;
	asm volatile("fence" ::: "memory");

	memcpy((void *)P4_LPRAM_WAIT, p4_waiter_start, wlen);
	if (memcmp((const void *)P4_LPRAM_WAIT, p4_waiter_start,
		   wlen)) {
		printf("p4: LP RAM dead, single CPU\n");
		mbox[0] = 0;
		return 0;
	}

	/*
	 * Park hart1 in the waiter the IDF way: unstall it (it parks in the
	 * HP ROM when released), enable its clock, and publish the waiter
	 * through the ROM hook before releasing core1. The ROM owns the
	 * reset vector; publishing while core1 is held eliminates the
	 * window in which it can read an empty entry.
	 */
	{
		u32 v = readl((void *)P4_PMU_SW_STALL);
		unsigned int spin2 = 0;

		v = (v & ~(0xffu << 16)) | (0xffu << 16);
		writel(v, (void *)P4_PMU_SW_STALL);
		while ((readl((void *)P4_CORE_STALLED_ST) & BIT(1)) &&
		       spin2++ < 100000)
			;
		if (readl((void *)P4_CORE_STALLED_ST) & BIT(1)) {
			puts("p4: hart1 remains stalled\n");
			mbox[0] = 0;
			return 0;
		}
		printf("p4: stall %08x clk %08x rst %08x\n",
		       readl((void *)P4_CORE_STALLED_ST),
		       readl((void *)P4_CORE1_CLK),
		       readl((void *)P4_RST_EN_CORE1));
		/* Hart1's CPU clock gates its fetch: default off. */
		setbits_le32((void *)P4_CORE1_CLK, BIT(4));
		((void (*)(u32))P4_ROM_APPBOOT)(P4_LPRAM_WAIT);
		asm volatile("fence iorw, iorw" ::: "memory");
		clrbits_le32((void *)P4_RST_EN_CORE1, BIT(8));
		printf("p4: boot hook set, cause %08x\n",
		       readl((void *)P4_LP_RST_CAUSE));
	}
	/*
	 * Plain polling, like the reference loader and the kernel's
	 * ioremap reads: no cache maintenance on the mailbox itself (an
	 * invalidate here would drop the very lines being polled).
	 */
	while (mbox[1] != 1 && spin++ < 4000000) {
		if (mbox[4] != P4_KERN_RUN)
			alive = 1;
	}
	if (mbox[1] == 1 && mbox[3] == 0) {
		smp = 1;
		((void (*)(u32))P4_ROM_APPBOOT)(0);
		/* Restore entry: the kernel requires the Image address,
		 * the 1 was only the waiter-alive marker. */
		mbox[4] = P4_KERN_RUN;
		asm volatile("fence" ::: "memory");
		printf("p4: hart1 parked, SMP on\n");
	} else {
		((void (*)(u32))P4_ROM_APPBOOT)(0);
		printf("p4: hart1 not parked (alive %d status %u); reset required\n",
		       alive, mbox[3]);
		mbox[0] = 0;
	}
	return smp;
}

static int do_p4boot(struct cmd_tbl *cmdtp, int flag, int argc,
		     char *const argv[])
{
	void (*kernel)(u32 hart, void *dtb) = (void *)P4_KERN_RUN;
	const struct p4_fit_handoff *fit = p4_get_fit();
	bool verify = argc == 2 && !strcmp(argv[1], "verify");
	u32 span;

	if (argc > 1 && !verify)
		return CMD_RET_USAGE;
	if (!fit || !(span = p4_image_span(fit))) {
		puts("p4boot: missing or invalid SPL FIT handoff\n");
		return CMD_RET_FAILURE;
	}
	if (p4_prep_dtb(fit) || !p4_park_hart1())
		return CMD_RET_FAILURE;

	/*
	 * SPL has already written back and checked these bytes. There are no
	 * writers to the Image here. Drop stale data and instruction lines
	 * for the bounded Image only, then (opt-in) rehash exact FIT bytes.
	 */
	if (p4_cache_discard_data(P4_KERN_RUN, span) ||
	    p4_cache_range(P4_KERN_RUN, span, P4_SYNC_L1I,
			   P4_SYNC_INVALIDATE)) {
		puts("p4boot: Image cache invalidate failed\n");
		return CMD_RET_FAILURE;
	}
	if (verify) {
		if (!p4_fit_sha_ok((void *)P4_KERN_RUN, fit->image_len,
				   fit->image_hash)) {
			puts("p4boot: cached Image SHA256 mismatch; halted\n");
			return CMD_RET_FAILURE;
		}
		printf("p4boot: cached Image SHA256 recheck OK (%u bytes)\n",
		       fit->image_len);
	}
	printf("p4boot: kernel %08x fdt %08x, jumping\n", P4_KERN_RUN, P4_DTB_KERN);
	asm volatile("csrci mstatus,8; fence rw,rw; fence.i" ::: "memory");
	kernel(0, (void *)P4_DTB_KERN);
	return CMD_RET_SUCCESS;
}

U_BOOT_CMD(p4boot, 2, 0, do_p4boot, "jump to the preloaded kernel",
	   "[verify] - also recheck exact FIT Image SHA256");

/*
 * "p4park" - run the full p4boot prep (DTB relocate + handoff + mailbox +
 * hart1 waiter) but return to the prompt instead of jumping. Lets the
 * mailbox be watched with md while hart1 sits parked waiting for release.
 */
static int do_p4park(struct cmd_tbl *cmdtp, int flag, int argc,
		     char *const argv[])
{
	const struct p4_fit_handoff *fit = p4_get_fit();
	int smp;

	if (!fit || !p4_image_span(fit) || p4_prep_dtb(fit))
		return CMD_RET_FAILURE;

	smp = p4_park_hart1();
	printf("p4park: parked=%d mbox live, release untouched\n", smp);
	return smp ? CMD_RET_SUCCESS : CMD_RET_FAILURE;
}

U_BOOT_CMD(p4park, 1, 0, do_p4park, "prep handoff and park hart1, no jump", "");

static int do_p4sync(struct cmd_tbl *cmdtp, int flag, int argc,
		     char *const argv[])
{
	ulong addr, size;

	if (argc < 3)
		return CMD_RET_USAGE;

	addr = simple_strtoul(argv[1], NULL, 16);
	size = simple_strtoul(argv[2], NULL, 16);

	if (p4_cache_sync_stub((unsigned int)addr, (unsigned int)size)) {
		printf("p4sync: writeback failed\n");
		return CMD_RET_FAILURE;
	}
	printf("p4sync: writeback %08lx size %lx ok\n", addr, size);
	return CMD_RET_SUCCESS;
}

U_BOOT_CMD(p4sync, 3, 0, do_p4sync, "write back one range (retained stub)",
	   "<addr> <size>");

#define P4_CSR(csr) case csr: asm volatile("csrr %0, " #csr : "=r"(v)); break

static int do_p4csr(struct cmd_tbl *cmdtp, int flag, int argc,
		    char *const argv[])
{
	ulong csr;
	unsigned long v = ~0ul;

	if (argc < 2)
		return CMD_RET_USAGE;

	csr = simple_strtoul(argv[1], NULL, 0);

	switch (csr) {
	P4_CSR(0x300);
	P4_CSR(0x301);
	P4_CSR(0x3a0);
	P4_CSR(0x3a2);
	P4_CSR(0x3b0);
	P4_CSR(0x3b1);
	P4_CSR(0x3b2);
	P4_CSR(0x3b3);
	P4_CSR(0xbc0);
	P4_CSR(0xbc1);
	P4_CSR(0xbc2);
	P4_CSR(0xbc3);
	P4_CSR(0xbd0);
	P4_CSR(0xbd1);
	P4_CSR(0xbd2);
	P4_CSR(0xbd3);
	default:
		printf("p4csr: unsupported csr 0x%lx\n", csr);
		return CMD_RET_FAILURE;
	}
	printf("p4csr: 0x%lx = 0x%lx\n", csr, v);
	return CMD_RET_SUCCESS;
}

U_BOOT_CMD(p4csr, 2, 0, do_p4csr, "read one CSR (PMA/PMP/mode)",
	   "<csr number>");

#endif

void hw_watchdog_init(void)
{
	p4_watchdogs_off();
}
#endif


int board_late_init(void)
{
#ifdef CONFIG_ESP32P4_HW_IDF_HANDOFF
	puts("P4_UBOOT: IDF hardware handoff (experimental)\n");
#else
	puts("P4_UBOOT: native M-mode bootloader active\n");
#if defined(CONFIG_ESP32P4_SRAM_ONLY)
	printf("P4_UBOOT: PSRAM %u MiB from SPL\n", p4_ext_size() >> 20);
#endif
#endif
	return 0;
}
