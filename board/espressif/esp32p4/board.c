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
/*
 * Must be the same address p4boot hands to the kernel, and the same one the SPL
 * preloads the blob to (through the alias). It used to be 0x48f00000, i.e.
 * U-Boot's own RAM top, so a p4load followed by p4boot pointed the kernel at an
 * address that never held a device tree.
 */
#define P4_DTB_DST		0x48600000u
/*
 * Copy through the uncached alias, not the cached window.
 *
 * 0x4800_0000 is served by the cache, and a store there lands in a cache line
 * and is never written back to the part - measured in FLASH-NOTES round 123:
 * with the part cleared to 0xFFFFFFFF through the alias, a window store shows up
 * in the window and leaves the part at 0xFFFFFFFF. A p4load through the window
 * therefore reports success while the PSRAM never receives the image. The alias
 * is the path the SPL's preload uses and is verified byte-faithful, so use it
 * here too.
 */
#define P4_NC_OFFSET		0x40000000u
#define P4_LOAD_CHUNK		0x1000u

/*
 * "p4boot" - jump straight to the preloaded kernel.
 *
 * The kernel image and the device tree are placed in PSRAM by the SPL and the
 * kernel is entered at its link address, 0x4800_0000.
 *
 * This used to carry an "alias" mode that entered at the uncached
 * 0x8800_0000 alias instead. It is gone: the kernel relocates itself to its link
 * address whatever it is entered at (measured - mepc came back as a link address),
 * so entering through the alias only moved the same failure, and the device-tree
 * /memory fixup it needed was a workaround for an address the kernel never keeps.
 * See FLASH-NOTES rounds 114 and 121-125.
 */
#define P4_KERN_RUN		0x48000000u
#define P4_DTB_RUN		0x48600000u
#define P4_KERN_ALIAS		0x88000000u
#define P4_DTB_ALIAS		0x88600000u
/*
 * The kernel's SMP start (arch/riscv/kernel/cpu_ops_spinwait.c
 * p4_prepare_secondary) requires the DTB at 0x48f00000: it checks the
 * firmware mailbox's dtb field and dtb_early_pa against it, and the DTB's
 * own reserved-mem names boot-dtb@48f00000. p4boot copies the SPL blob
 * there fresh through the uncached alias at boot time, so no lifetime
 * hazard with U-Boot's own allocations exists.
 */
#define P4_DTB_KERN		0x48f00000u
#define P4_DTB_KERN_ALIAS	0x88f00000u
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
/* Hart1 waiter home: LP TCM RAM, selected by HPCORE1_STAT_VECTOR_SEL
 * (LP_CLKRST+0x14 bit 31 = 0; IDF lp_clkrst_reg.h). HPCORE1_SW_RESET
 * (same register, bit 29, WT) reboots hart1 into it. */
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
 * "p4wb [map]" - ROM Cache_WriteBack_All (0x4fc00414), map bits from
 * esp32p4/rom/cache.h: 0x01/0x02 L1 I0/I1, 0x10 L1 D, 0x20 L2.
 *
 * This exists to settle one question: a store to the cached PSRAM window is
 * absorbed by the cache and the part keeps the old contents, so either the
 * writeback path works and nothing flushes, or it does not work at all.
 */
static int do_p4wb(struct cmd_tbl *cmdtp, int flag, int argc,
		   char *const argv[])
{
	u32 map = (1u << 4) | (1u << 5);

	if (argc > 1)
		map = simple_strtoul(argv[1], NULL, 16);

	((void (*)(u32))0x4fc00414u)(map);
	printf("p4wb: writeback all, map 0x%02x\n", map);
	return CMD_RET_SUCCESS;
}

U_BOOT_CMD(p4wb, 2, 0, do_p4wb, "write back caches (map arg)", "[map]");

/*
 * "p4wba <addr> <size>" - ROM Cache_WriteBack_Addr (0x4FC003F4) for one line or
 * range, map 0x30 (L1 D + L2, the ROM's own SPI_FLASH_MMAP_INVALIDATE_CACHE_ADDR
 * pair). A whole-cache pass returns but does not push a dirty PSRAM line, so this
 * checks whether a targeted writeback reaches the part.
 */
static int do_p4wba(struct cmd_tbl *cmdtp, int flag, int argc,
		    char *const argv[])
{
	ulong addr, size;

	if (argc < 3)
		return CMD_RET_USAGE;

	addr = simple_strtoul(argv[1], NULL, 16);
	size = simple_strtoul(argv[2], NULL, 16);

	((void (*)(u32, u32, u32))0x4fc003f4u)(0x30u, (u32)addr, (u32)size);
	printf("p4wba: writeback 0x30 @ %08lx size %lx\n", addr, size);
	return CMD_RET_SUCCESS;
}

U_BOOT_CMD(p4wba, 3, 0, do_p4wba, "write back one range", "<addr> <size>");

/*
 * Cache sync engine (real base 0x3FF10000 = IDF DR_REG_CACHE_BASE, not the
 * 0x500E5000 SYSREG block). TRM ch.9 has no register summary for these
 * (TRM 未覆盖); offsets/fields from IDF cache_reg.h v5.4.1: SYNC_CTRL +0x98
 * (WRITEBACK_ENA bit 2, SYNC_DONE bit 4, self-clearing), SYNC_MAP +0x9C
 * (bit 4 L1-D, bit 5 L2), SYNC_ADDR +0xA0 (virtual address), SYNC_SIZE +0xA4.
 */
#define P4_CACHE_SYNC_CTRL	0x3ff10098u
#define P4_CACHE_SYNC_MAP	0x3ff1009cu
#define P4_CACHE_SYNC_ADDR	0x3ff100a0u
#define P4_CACHE_SYNC_SIZE	0x3ff100a4u
#define P4_CACHE_SYNC_MAP_D_L2	0x30u
#define P4_CACHE_SYNC_WB	0x4u
#define P4_CACHE_SYNC_INV	0x1u
#define P4_CACHE_SYNC_DONE	0x10u

/* One sync-engine op (writeback or invalidate) over a range; 0 = done. */
static int p4_cache_sync_op(unsigned int addr, unsigned int bytes, unsigned int op)
{
	unsigned int off;

	for (off = 0; off < bytes; off += 0x10000) {
		unsigned int n = bytes - off > 0x10000 ? 0x10000
						       : bytes - off;
		unsigned int spin = 0;

		writel(P4_CACHE_SYNC_MAP_D_L2, (void *)P4_CACHE_SYNC_MAP);
		writel(addr + off, (void *)P4_CACHE_SYNC_ADDR);
		writel(n, (void *)P4_CACHE_SYNC_SIZE);
		writel(op, (void *)P4_CACHE_SYNC_CTRL);
		while (!(readl((void *)P4_CACHE_SYNC_CTRL) &
			 P4_CACHE_SYNC_DONE)) {
			if (++spin > 1000000)
				return -1;
		}
	}
	asm volatile("fence" ::: "memory");
	return 0;
}

/* Invalidate a cached PSRAM range (drop possibly stale lines). */
static int p4_cache_sync_inv(unsigned int addr, unsigned int bytes)
{
	return p4_cache_sync_op(addr, bytes, P4_CACHE_SYNC_INV);
}

/*
 * Retained SRAM cache-sync stub for the kernel handoff. The kernel looks for
 * /chosen espressif,chip-revision (= 103, this silicon) and
 * espressif,cache-sync-entry (address of int sync(start, bytes) in the
 * 0x4FF00000-0x4FFC0000 retained SRAM window) and panics without them
 * ("P4 retained executable cache callback missing"). This stub lives in
 * U-Boot's own SRAM text, so it is present whenever p4boot runs, and drives
 * exactly the writeback sequence proven from the prompt: L1-D + L2, chunked,
 * DONE-polled with a timeout.
 */
int p4_cache_sync_stub(unsigned int start, unsigned int bytes)
{
	unsigned int off;

	for (off = 0; off < bytes; off += 0x10000) {
		unsigned int n = bytes - off > 0x10000 ? 0x10000
						       : bytes - off;
		unsigned int spin = 0;

		writel(P4_CACHE_SYNC_MAP_D_L2, (void *)P4_CACHE_SYNC_MAP);
		writel(start + off, (void *)P4_CACHE_SYNC_ADDR);
		writel(n, (void *)P4_CACHE_SYNC_SIZE);
		writel(P4_CACHE_SYNC_WB, (void *)P4_CACHE_SYNC_CTRL);
		while (!(readl((void *)P4_CACHE_SYNC_CTRL) &
			 P4_CACHE_SYNC_DONE)) {
			if (++spin > 1000000)
				return -1;
		}
	}
	asm volatile("fence" ::: "memory");
	return 0;
}

/*
 * Hart1 firmware waiter, copied to LP RAM by p4boot and entered via
 * HPCORE1_SW_RESET with STAT_VECTOR_SEL = LP RAM. Mirrors the retained IDF
 * loader's secondary_wait (main/p4_hardware_loader.c): M-mode, interrupts
 * off out of reset; open user-PSRAM PMP entry 7 exactly as IDF does
 * (PMP_L|NAPOT|R|W|X over [0x48000000, 0x58000000): pmpaddr7 = 0x127FFFFF,
 * the entry-7 byte of pmpcfg1 = 0x9F); report 0/1 in mailbox status; then
 * poll release and jump to the kernel image entry, whose head selects the
 * secondary path itself (arch/riscv/kernel/head.S esp32p4_secondary_start).
 * Both HP harts share L1D, so the SRAM mailbox needs no cache management.
 * Written with the assembler (never hand-encoded) and position-independent.
 */
__asm__(
".pushsection .text.hart1waiter,\"ax\"\n"
".global p4_waiter_start\n"
"p4_waiter_start:\n"
"	lui t0, 0x4ff3c\n"		/* mailbox 0x4ff3c000 */
"	li t1, 1\n"
"	sw t1, 16(t0)\n"		/* marker: entry = hartid (alive) */
"	fence\n"
"1:	lw t1, 0(t0)\n"		/* magic published? */
"	li t2, 0x534d5034\n"
"	bne t1, t2, 1b\n"
"	lui t1, 0x12800\n"		/* pmpaddr7 = NAPOT(0x48000000, 0x4c000000) */
"	addi t1, t1, -1\n"		/* 0x127fffff */
"	csrw 0x3b7, t1\n"
"	lui t1, 0x9f000\n"		/* entry7 = L|NAPOT|R|W|X */
"	csrrs zero, 0x3a1, t1\n"
"	csrr t1, 0x3b7\n"		/* verify PMP took */
"	li t2, 0x127fffff\n"
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
"5:	lw t1, 8(t0)\n"		/* release? */
"	beqz t1, 5b\n"
"	lw t1, 12(t0)\n"		/* paranoia: jump only on success */
"	bnez t1, 4b\n"
"	fence\n"
"	lw t1, 16(t0)\n"		/* entry */
"	lw a1, 20(t0)\n"		/* dtb */
"	li a0, 1\n"
"	jr t1\n"
".global p4_waiter_end\n"
"p4_waiter_end:\n"
".popsection\n"
);
extern char p4_waiter_start[];
extern char p4_waiter_end[];

static int p4_check_payload(void)
{
	/*
	 * The checks read through the uncached alias on purpose: the cached window
	 * can serve a stale line, so a magic number read through it proves nothing
	 * about what the part actually holds.
	 */
	if (readl((const void *)(P4_KERN_ALIAS + 0x38)) != 0x05435352u) {
		printf("p4: bad RISCV Image magic at %08x\n", P4_KERN_ALIAS);
		return -1;
	}
	if (readl((const void *)P4_DTB_ALIAS) != 0xedfe0dd0u) {
		printf("p4: no fdt at %08x\n", P4_DTB_ALIAS);
		return -1;
	}
	return 0;
}

/*
 * Relocate the DTB to where the kernel demands it (0x48F00000, see the
 * P4_DTB_KERN comment): fresh through the uncached alias, then drop any
 * lines U-Boot may hold for the range so the libfdt edits below read
 * the fresh copy from the part. Installs the kernel handoff (retained
 * sync stub address, chip revision, cpu1 boot mailbox) into the copy.
 */
static int p4_prep_dtb(void)
{
	const u32 *dtb_src = (const u32 *)P4_DTB_SRC;
	void *fdt = (void *)P4_DTB_KERN;
	u32 dsize, newsize;
	int chosen;
	fdt32_t rev, entry, mbox[2];

	dsize = fdt_totalsize(dtb_src);
	if (dsize < 40 || dsize > 0x10000) {
		printf("p4: implausible dtb size %x\n", dsize);
		return -1;
	}
	memcpy((void *)(P4_DTB_KERN_ALIAS), (const void *)P4_DTB_ALIAS, dsize);
	if (readl((const void *)P4_DTB_KERN_ALIAS) != 0xedfe0dd0u) {
		printf("p4: dtb relocate failed\n");
		return -1;
	}
	if (p4_cache_sync_inv(P4_DTB_KERN, 0x00010000)) {
		printf("p4: dtb invalidate failed\n");
		return -1;
	}

	newsize = fdt_totalsize(fdt) + 0x1000;
	if (fdt_open_into(fdt, fdt, newsize)) {
		printf("p4: fdt_open_into failed\n");
		return -1;
	}
	chosen = fdt_path_offset(fdt, "/chosen");
	if (chosen < 0) {
		printf("p4: no /chosen in dtb\n");
		return -1;
	}
	rev = cpu_to_fdt32(103);
	entry = cpu_to_fdt32((u32)p4_cache_sync_stub);
	mbox[0] = cpu_to_fdt32(P4_MBOX_ADDR);
	mbox[1] = cpu_to_fdt32(P4_MBOX_SIZE);
	if (fdt_setprop(fdt, chosen, "espressif,chip-revision",
			&rev, 4) ||
	    fdt_setprop(fdt, chosen, "espressif,cache-sync-entry",
			&entry, 4) ||
	    fdt_setprop(fdt, chosen, "espressif,cpu1-boot-mailbox",
			mbox, 8)) {
		printf("p4: fdt setprop failed\n");
		return -1;
	}
	printf("p4: handoff stub @ %08x mbox @ %08x\n",
	       (u32)p4_cache_sync_stub, P4_MBOX_ADDR);
	return 0;
}

/*
 * Publish the firmware mailbox (status = 2 = pending until the waiter
 * proves itself), then park hart1 in the LP-RAM waiter: install,
 * read back, select LP RAM as its reset vector, enable its CPU clock,
 * SW-reset it, release it, then wait for it to report parked.
 * Returns 1 when hart1 is parked (SMP on), 0 otherwise; on 0 the magic
 * is cleared so the kernel fails the SMP validate fast instead of
 * hanging a hart.
 */
static int p4_park_hart1(void)
{
	volatile u32 *mbox = (volatile u32 *)P4_MBOX_ADDR;
	unsigned long wlen = (unsigned long)(p4_waiter_end -
					     p4_waiter_start);
	unsigned int spin = 0;
	int alive = 0;
	int smp = 0;

	/*
	 * A waiter parked by an earlier p4park in this session is still
	 * valid: it only polls release, which only the kernel sets. Reuse
	 * it untouched (refilling ready/status under it would hide its
	 * parked report forever).
	 */
	if (mbox[0] == P4_MBOX_MAGIC && mbox[1] == 1 && mbox[2] == 0 &&
	    mbox[3] == 0 && mbox[4] == P4_KERN_RUN &&
	    mbox[5] == P4_DTB_KERN && mbox[6] == 103) {
		printf("p4: waiter already parked, reusing\n");
		return 1;
	}

	mbox[1] = 0;			/* ready */
	mbox[2] = 0;			/* release */
	mbox[3] = 2;			/* status = pending */
	mbox[4] = P4_KERN_RUN;		/* entry */
	mbox[5] = P4_DTB_KERN;		/* dtb */
	mbox[6] = 103;			/* revision */
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
	 * HP ROM at POR), make sure its clock is on and it is out of reset,
	 * then publish the waiter through the ROM's appcpu boot hook — the
	 * parked hart1 spins on that hook and jumps to it. No SW reset, no
	 * reset-vector change: the ROM owns hart1's boot.
	 */
	{
		u32 v = readl((void *)P4_PMU_SW_STALL);
		unsigned int spin2 = 0;

		v = (v & ~(0xffu << 16)) | (0xffu << 16);
		writel(v, (void *)P4_PMU_SW_STALL);
		while ((readl((void *)P4_CORE_STALLED_ST) & BIT(1)) &&
		       spin2++ < 100000)
			;
		printf("p4: stall %08x clk %08x rst %08x\n",
		       readl((void *)P4_CORE_STALLED_ST),
		       readl((void *)P4_CORE1_CLK),
		       readl((void *)P4_RST_EN_CORE1));
		/* Hart1's CPU clock gates its fetch: default off. */
		setbits_le32((void *)P4_CORE1_CLK, BIT(4));
		clrbits_le32((void *)P4_RST_EN_CORE1, BIT(8));
		((void (*)(u32))P4_ROM_APPBOOT)(P4_LPRAM_WAIT);
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
		/* Restore entry: the kernel requires the Image address,
		 * the 1 was only the waiter-alive marker. */
		mbox[4] = P4_KERN_RUN;
		asm volatile("fence" ::: "memory");
		printf("p4: hart1 parked, SMP on\n");
	} else {
		printf("p4: waiter quiet (alive %d status %u), single CPU\n",
		       alive, mbox[3]);
		mbox[0] = 0;
	}
	return smp;
}

static int do_p4boot(struct cmd_tbl *cmdtp, int flag, int argc,
		     char *const argv[])
{
	void (*kernel)(u32 hart, void *dtb) = (void *)P4_KERN_RUN;
	int smp = 0;

	if (p4_check_payload() || p4_prep_dtb())
		return CMD_RET_FAILURE;

	printf("p4boot: kernel %08x fdt %08x, jumping\n", P4_KERN_RUN, P4_DTB_KERN);
	smp = p4_park_hart1();

	/*
	 * Push the payload (and the freshly patched DTB) to the part before
	 * entering the kernel: instruction/data fetches must see device
	 * contents, not just dirty cache lines.
	 */
	if (p4_cache_sync_stub(P4_KERN_RUN, 0x00400000) ||
	    p4_cache_sync_stub(P4_DTB_KERN, 0x00010000)) {
		printf("p4boot: cache sync failed\n");
		return CMD_RET_FAILURE;
	}

	asm volatile("csrci mstatus, 8" ::: "memory");	/* clear MIE */
	asm volatile("fence.i" ::: "memory");
	asm volatile("fence" ::: "memory");

	if (!smp)
		printf("p4boot: single CPU\n");
	kernel(0, (void *)P4_DTB_KERN);
	return CMD_RET_SUCCESS;
}

U_BOOT_CMD(p4boot, 1, 0, do_p4boot, "jump to the preloaded kernel", "");

/*
 * "p4park" - run the full p4boot prep (DTB relocate + handoff + mailbox +
 * hart1 waiter) but return to the prompt instead of jumping. Lets the
 * mailbox be watched with md while hart1 sits parked waiting for release.
 */
static int do_p4park(struct cmd_tbl *cmdtp, int flag, int argc,
		     char *const argv[])
{
	int smp;

	if (p4_check_payload() || p4_prep_dtb())
		return CMD_RET_FAILURE;

	smp = p4_park_hart1();
	printf("p4park: parked=%d mbox live, release untouched\n", smp);
	return CMD_RET_SUCCESS;
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
