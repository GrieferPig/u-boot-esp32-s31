// SPDX-License-Identifier: GPL-2.0+
/*
 * ESP32-P4 board support (M-mode, noMMU, no OpenSBI).
 *
 * Two phases share this file:
 *  - SPL: runs from internal SRAM, brings PSRAM up and loads U-Boot proper.
 *  - U-Boot proper: runs from internal SRAM and boots the kernel
 *    directly from mapped SPI NOR.
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
#include <env.h>
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

/* The device tree may be edited before entering Linux. The kernel stays in NOR. */
#define P4_DTB_KERN		0x48f00000u
#define P4_DTB_KERN_BYTES	P4_FIT_DTB_LIMIT

static const struct p4_fit_handoff *p4_get_fit(void)
{
	const struct p4_fit_handoff *fit = (void *)P4_FIT_HANDOFF;
	if (!p4_ext_size() || READ_ONCE(fit->magic) != P4_FIT_MAGIC_XIP)
		return NULL;
	asm volatile("fence r,r" ::: "memory");
	if (fit->image_len < 64 || fit->image_len > P4_FIT_IMAGE_FLASH_LIMIT ||
	    fit->dtb_len < sizeof(struct fdt_header) ||
	    fit->dtb_len > P4_FIT_DTB_LIMIT)
		return NULL;
	return fit;
}

static void p4_sha256(const void *data, u32 size, u8 *digest)
{
	sha256_context ctx;
	const u8 *p = data;

	sha256_starts(&ctx);
	while (size) {
		u32 n = min_t(u32, size, 0x4000u);

		sha256_update(&ctx, p, n);
		size -= n;
		p += n;
	}
	sha256_finish(&ctx, digest);
}

static bool p4_fit_sha_ok(const void *data, u32 size, const u8 *expected)
{
	u8 actual[SHA256_SUM_LEN];

	p4_sha256(data, size, actual);
	return !memcmp(expected, actual, sizeof(actual));
}

/*
 * TRM ch.9 System and Memory — the FDT uses the cached external RAM window.
 * Keep the hashed FIT input intact and open the tree in the separate 64 KiB
 * Linux reservation at the runtime stack's upper bound. Verify the final
 * bytes after writeback and invalidation before entering Linux.
 */
static int p4_prep_dtb(const struct p4_fit_handoff *fit)
{
	const void *src = (void *)P4_FIT_DTB;
	void *fdt = (void *)P4_DTB_KERN;
	u8 digest[SHA256_SUM_LEN];
	u32 size = p4_ext_size(), final;
	int chosen, mem, ret;
	const char *bootargs = env_get("bootargs");
	fdt32_t reg[2];

	if (!p4_fit_sha_ok(src, fit->dtb_len, fit->dtb_hash) ||
	    fdt_check_full(src, fit->dtb_len)) {
		puts("p4: FIT DTB in NOR invalid\n");
		return -EINVAL;
	}

	ret = fdt_open_into(src, fdt, P4_DTB_KERN_BYTES);
	if (ret) {
		printf("p4: cannot open DTB in %u-byte handoff: %s\n",
		       P4_DTB_KERN_BYTES, fdt_strerror(ret));
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
	if (chosen < 0 ||
	    (bootargs && fdt_setprop_string(fdt, chosen, "bootargs", bootargs)) ||
	    fdt_pack(fdt)) {
		puts("p4: DTB fixup/pack failed\n");
		return -EINVAL;
	}
	final = fdt_totalsize(fdt);
	if (final < sizeof(struct fdt_header) || final > P4_DTB_KERN_BYTES ||
	    fdt_check_full(fdt, final))
		return -EINVAL;
	p4_sha256(fdt, final, digest);
	if (p4_cache_republish(P4_DTB_KERN, final)) {
		puts("p4: final cached DTB sync failed\n");
		return -EIO;
	}
	if (fdt_check_full(fdt, final) || !p4_fit_sha_ok(fdt, final, digest)) {
		puts("p4: final cached DTB reload mismatch\n");
		return -EIO;
	}
	printf("P4 DTB-CACHED: %u byte reload OK, mem %08x/%08x\n",
	       final, P4_EXT_RAM_BASE, size);
	return 0;
}

static int do_p4boot(struct cmd_tbl *cmdtp, int flag, int argc,
		     char *const argv[])
{
	void (*kernel)(u32 hart, void *dtb) = (void *)P4_FIT_IMAGE;
	const struct p4_fit_handoff *fit = p4_get_fit();

	if (argc != 1)
		return CMD_RET_USAGE;
	if (!fit || !p4_fit_sha_ok((const void *)P4_FIT_IMAGE, fit->image_len,
				  fit->image_hash)) {
		puts("p4boot: missing or invalid NOR XIP kernel\n");
		return CMD_RET_FAILURE;
	}
	if (p4_prep_dtb(fit))
		return CMD_RET_FAILURE;

	printf("p4boot: NOR XIP kernel %08x fdt %08x, jumping\n",
	       P4_FIT_IMAGE, P4_DTB_KERN);
	asm volatile("csrci mstatus,8; fence rw,rw; fence.i" ::: "memory");
	kernel(0, (void *)P4_DTB_KERN);
	return CMD_RET_FAILURE;
}

U_BOOT_CMD(p4boot, 1, 0, do_p4boot, "boot Linux directly from mapped SPI NOR", "");

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
