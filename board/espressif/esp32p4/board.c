// SPDX-License-Identifier: GPL-2.0+
/*
 * ESP32-P4 board support (M-mode, noMMU, no OpenSBI).
 *
 * Two phases share this file:
 *  - SPL: runs from internal SRAM, brings PSRAM up and loads U-Boot proper.
 *  - U-Boot proper: runs from PSRAM at CONFIG_TEXT_BASE and boots the kernel
 *    that the SPL placed in PSRAM.
 */

#include <asm/io.h>
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
 * The ROM leaves several watchdogs armed (RWDT, super watchdog and the TIMG
 * MWDTs). U-Boot feeds them through WATCHDOG_RESET(), which lands here.
 */
void hw_watchdog_reset(void)
{
	void *wdt = (void *)0x50116000;
	u32 t;

	writel(0x50d83aa1, wdt + 0x18);
	writel(1, wdt + 0x14);
	writel(0, wdt + 0x18);

	writel(0x50d83aa1, wdt + 0x20);
	writel(readl(wdt + 0x1c) | (1u << 31), wdt + 0x1c);
	writel(0, wdt + 0x20);

	for (t = 0; t < 4; t++) {
		u32 base = 0x500c2000 + t * 0x1000;

		writel(0x50d83aa1, (void *)(base + 0x64));
		writel(0x50d83aa1, (void *)(base + 0x60));
		writel(0, (void *)(base + 0x64));
	}
}


#if !defined(CONFIG_SPL)
#include <command.h>

/*
 * "p4load" - copy the kernel and device tree from the flash window into PSRAM.
 * U-Boot's own "cp" command faults on this port and a single multi-megabyte
 * burst leaves the PSRAM mode registers in a bad state, so copy in bounded
 * chunks and flush each one through the ROM helper with the full L1 mask.
 */
#define P4_ROM_CACHE_WRITEBACK	0x4fc003f4
#define P4_CACHE_MAP_ALL	0x13
#define P4_KERN_SRC		0x40200000u
#define P4_DTB_SRC		0x40900000u
#define P4_KERN_DST		0x48000000u
#define P4_DTB_DST		0x48f00000u
#define P4_LOAD_CHUNK		0x10000u

static int do_p4load(struct cmd_tbl *cmdtp, int flag, int argc,
		     char *const argv[])
{
	const u32 *kern = (const u32 *)P4_KERN_SRC;
	const u32 *dtb = (const u32 *)P4_DTB_SRC;
	u32 ksize, dsize, off;

	if (dtb[0] != 0xedfe0dd0u) {
		printf("p4load: no fdt at 0x%08x\n", P4_DTB_SRC);
		return CMD_RET_FAILURE;
	}

	ksize = kern[4];			/* arm64 image_size at 0x10 */
	dsize = __builtin_bswap32(dtb[1]);	/* fdt totalsize, big endian */
	if (ksize < 0x100000u || ksize > 0x01000000u || dsize > 0x10000u) {
		printf("p4load: implausible sizes k=%x d=%x\n", ksize, dsize);
		return CMD_RET_FAILURE;
	}

	for (off = 0; off < dsize; off += P4_LOAD_CHUNK) {
		u32 n = dsize - off > P4_LOAD_CHUNK ? P4_LOAD_CHUNK
						    : dsize - off;

		memcpy((void *)(P4_DTB_DST + off), (const void *)(P4_DTB_SRC + off),
		       n);
		((int (*)(u32, u32, u32))P4_ROM_CACHE_WRITEBACK)
			(P4_CACHE_MAP_ALL, P4_DTB_DST + off, n);
	}

	for (off = 0; off < ksize; off += P4_LOAD_CHUNK) {
		u32 n = ksize - off > P4_LOAD_CHUNK ? P4_LOAD_CHUNK
						    : ksize - off;

		memcpy((void *)(P4_KERN_DST + off), (const void *)(P4_KERN_SRC + off),
		       n);
		((int (*)(u32, u32, u32))P4_ROM_CACHE_WRITEBACK)
			(P4_CACHE_MAP_ALL, P4_KERN_DST + off, n);
	}

	printf("p4load: kernel %x bytes -> 0x%08x (%08x), dtb %x -> 0x%08x (%08x)\n",
	       ksize, P4_KERN_DST, readl((void *)P4_KERN_DST), dsize,
	       P4_DTB_DST, readl((void *)P4_DTB_DST));
	return CMD_RET_SUCCESS;
}

U_BOOT_CMD(p4load, 1, 1, do_p4load,
	   "copy kernel and fdt from flash into psram",
	   "");
#endif

void hw_watchdog_init(void)
{
	/*
	 * Long operations (copying a kernel into PSRAM) cannot feed the
	 * watchdogs from the serial poll path, so switch off the RWDT and the
	 * super watchdog outright instead of only feeding them.
	 */
	void *wdt = (void *)0x50116000;

	writel(0x50d83aa1, wdt + 0x18);			/* WPROTECT */
	clrbits_le32(wdt + 0x00, BIT(31));		/* rwdt_en */
	clrbits_le32(wdt + 0x00, BIT(14));		/* flashboot enable */
	writel(0, wdt + 0x18);

	writel(0x50d83aa1, wdt + 0x20);			/* SWD_WPROTECT */
	setbits_le32(wdt + 0x1c, BIT(30));		/* swd_disable */
	writel(0, wdt + 0x20);
}
#endif


#if defined(CONFIG_ESP32P4_SRAM_ONLY)
/* Map the PSRAM window through the MSPI2 "S" MMU. */
static void p4_psram_map(u32 size)
{
	u32 entries = size >> 16;
	u32 i;

	for (i = 0; i < entries; i++) {
		writel(i, (void *)0x5008e380);
		writel(i | BIT(11) | BIT(10), (void *)0x5008e37c);
	}
	puts("P4_UBOOT: psram mmu written\n");
}
#endif

int board_late_init(void)
{
#ifdef CONFIG_ESP32P4_HW_IDF_HANDOFF
	puts("P4_UBOOT: IDF hardware handoff (experimental)\n");
#else
	puts("P4_UBOOT: M-mode bootloader active (emulator only)\n");
#if defined(CONFIG_ESP32P4_SRAM_ONLY)
	p4_psram_map(0x01000000);
#endif
#endif
	return 0;
}
