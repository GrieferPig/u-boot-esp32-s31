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

#if defined(CONFIG_ESP32P4_SRAM_ONLY)
	/*
	 * U-Boot itself lives in SRAM, but the kernel's memory - and therefore
	 * the only place a bootm FDT relocation or kernel payload can be placed -
	 * is PSRAM. Report that as a second bank so boot_relocate_fdt() has real
	 * memory to target instead of failing with "device tree - allocation
	 * error" (the SRAM bank is fully occupied by the running image).
	 */
	gd->bd->bi_dram[1].start = 0x48000000;
	gd->bd->bi_dram[1].size = 0x01000000;	/* 16 MiB PSRAM window */
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

void p4_watchdogs_off(void)
{
	void *wdt = (void *)0x50116000;

	writel(P4_WDT_WKEY, wdt + 0x18);		/* RWDT WPROTECT */
	clrbits_le32(wdt + 0x00, BIT(31) | BIT(14));
	writel(0, wdt + 0x18);

	writel(P4_WDT_WKEY, wdt + 0x20);		/* SWD_WPROTECT */
	setbits_le32(wdt + 0x1c, BIT(30));		/* swd_disable */
	writel(0, wdt + 0x20);

	p4_mwdt_off(P4_TIMG0_BASE);
	p4_mwdt_off(P4_TIMG1_BASE);
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
 * "p4load" - copy the kernel and device tree from the flash window into PSRAM.
 *
 * The PSRAM window is mapped twice: 0x48000000 is the cacheable alias and
 * 0x88000000 the non-cacheable one (SOC_NON_CACHEABLE_OFFSET_PSRAM). Writes
 * through 0x48000000 land in the L1/L2 cache and stay there - the ROM cache
 * writeback helper does not push them to the part from this context - so the
 * payload is copied through the non-cacheable alias and the kernel/DTB are
 * booted from the cacheable alias afterwards.
 */
#define P4_KERN_SRC		0x40200000u
#define P4_DTB_SRC		0x40900000u
#define P4_KERN_DST		0x48000000u
#define P4_DTB_DST		0x48f00000u
/*
 * Copy through the plain cacheable window. The non-cacheable alias
 * (0x8800_0000) is what earlier rounds used, but the window that is verified
 * working end to end on this chain is 0x4800_0000 (the SPL's window check
 * writes and reads it back), and no cache is enabled here anyway, so the
 * alias buys nothing.
 */
#define P4_NC_OFFSET		0x00000000u
#define P4_LOAD_CHUNK		0x1000u

/*
 * "p4boot" - jump straight to the preloaded kernel.
 *
 * booti does accept the image and relocates/fixes up the FDT, but its
 * announce_and_cleanup() then faults: the faulting instruction is a stack store
 * (`c.swsp`) with TVAL 0x94, i.e. the stack pointer is corrupted by the time the
 * handoff runs. Until that is understood, start the kernel the same way
 * boot_jump_linux() would - RISC-V kernels take (hartid, dtb) - without going
 * through that path.
 *
 * Layout: kernel image at 0x8800_0000 (direct alias) and the FDT at 0x8860_0000,
 * both placed there by the SPL preload.
 */
#define P4_KERN_ALIAS		0x88000000u
#define P4_DTB_ALIAS		0x88600000u
/*
 * The kernel must execute at its physical load address: the device tree
 * describes memory as 0x4800_0000 upward, so running from the 0x8800_0000 alias
 * would put the kernel outside its own memory map.
 */
#define P4_KERN_RUN		0x48000000u
#define P4_DTB_RUN		0x48600000u

/*
 * "p4bench <addr> <words>" - time a run of reads.
 *
 * Executing the kernel from the PSRAM window runs at roughly 50 instructions
 * per second while this same U-Boot retires ~100M instructions/s from SRAM, so
 * the fetch path into PSRAM is what needs measuring. Reading through the window,
 * through the uncached alias and from SRAM in the same units (mcycle) shows
 * whether the slowness is the window mapping itself or the memory behind it.
 */
static u64 p4_cycles(void)
{
	u32 lo, hi, hi2;

	do {
		asm volatile("csrr %0, mcycleh" : "=r"(hi));
		asm volatile("csrr %0, mcycle" : "=r"(lo));
		asm volatile("csrr %0, mcycleh" : "=r"(hi2));
	} while (hi != hi2);

	return ((u64)hi << 32) | lo;
}

static int do_p4bench(struct cmd_tbl *cmdtp, int flag, int argc,
		      char *const argv[])
{
	ulong addr, cnt, i;
	volatile u32 sum = 0;
	u64 t0, t1;
	u32 delta, per;

	if (argc < 3)
		return CMD_RET_USAGE;

	addr = simple_strtoul(argv[1], NULL, 16);
	cnt = simple_strtoul(argv[2], NULL, 16);

	t0 = p4_cycles();
	for (i = 0; i < cnt; i++)
		sum += readl((const void *)(addr + i * 4));
	t1 = p4_cycles();

	/* 32-bit arithmetic only: a u64 division here links libgcc's __udivdi3,
	 * which does not relocate in this configuration. */
	delta = (u32)(t1 - t0);
	per = cnt ? delta / (u32)cnt : 0;

	printf("p4bench: %lu words @ %08lx: %u cycles, %u cycles/word (sum %08x)\n",
	       cnt, addr, delta, per, sum);

	return CMD_RET_SUCCESS;
}

U_BOOT_CMD(p4bench, 3, 0, do_p4bench, "time reads from an address",
	   "<addr> <words>");

static int do_p4boot(struct cmd_tbl *cmdtp, int flag, int argc,
		     char *const argv[])
{
	void (*kernel)(u32 hart, void *dtb) = (void *)P4_KERN_RUN;

	if (readl((const void *)(P4_KERN_ALIAS + 0x38)) != 0x05435352u) {
		printf("p4boot: bad RISCV Image magic at %08x\n", P4_KERN_ALIAS);
		return CMD_RET_FAILURE;
	}
	if (readl((const void *)P4_DTB_ALIAS) != 0xedfe0dd0u) {
		printf("p4boot: no fdt at %08x\n", P4_DTB_ALIAS);
		return CMD_RET_FAILURE;
	}

	printf("p4boot: kernel %08x fdt %08x, jumping\n", P4_KERN_RUN,
	       P4_DTB_RUN);

	/*
	 * The same handoff the generic RISC-V cleanup_before_linux() performs:
	 * interrupts off and the pipeline/caches synchronised. booti reaches this
	 * point before its own fault, so replicate it here instead of relying on
	 * the bootm path.
	 */
	asm volatile("csrci mstatus, 8" ::: "memory");	/* clear MIE */
	asm volatile("fence.i" ::: "memory");
	asm volatile("fence" ::: "memory");

	kernel(0, (void *)P4_DTB_RUN);
	return CMD_RET_SUCCESS;
}

U_BOOT_CMD(p4boot, 1, 0, do_p4boot, "jump to the preloaded kernel", "");

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

	printf("p4load: start k=%x d=%x\n", ksize, dsize);

	for (off = 0; off < dsize; off += P4_LOAD_CHUNK) {
		u32 n = dsize - off > P4_LOAD_CHUNK ? P4_LOAD_CHUNK
						    : dsize - off;

		printf("p4load: dtb off %x src %08x\n", off,
		       readl((const void *)(P4_DTB_SRC + off)));
		memcpy((void *)(P4_DTB_DST + P4_NC_OFFSET + off),
		       (const void *)(P4_DTB_SRC + off), n);
		printf("p4load: dtb off %x copied\n", off);
	}
	printf("p4load: dtb done\n");

	for (off = 0; off < ksize; off += P4_LOAD_CHUNK) {
		u32 n = ksize - off > P4_LOAD_CHUNK ? P4_LOAD_CHUNK
						    : ksize - off;

		printf("p4load: k off %x\n", off);
		memcpy((void *)(P4_KERN_DST + P4_NC_OFFSET + off),
		       (const void *)(P4_KERN_SRC + off), n);
		printf("p4load: k off %x done\n", off);
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
	p4_watchdogs_off();
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
