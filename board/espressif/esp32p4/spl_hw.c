// SPDX-License-Identifier: GPL-2.0-or-later
/* Minimal early policy for the ESP32-P4 rev1.3 mask-ROM boot contract.
 * Register fields: ESP32-P4 rev1.3 TRM, eFuse, LP_WDT and TIMG chapters.
 */
#include <asm/io.h>
#include <asm/global_data.h>
#include <linux/bitops.h>
#include <linux/types.h>
#include "spl_hw.h"
#include <asm/arch-esp32p4/boot-layout.h>

/* start.S puts the early malloc/GD below the fixed stack reservation. */
_Static_assert(CONFIG_SPL_TEXT_BASE == P4_SPL_BASE, "P4 SPL link/ROM mismatch");
_Static_assert(P4_SPL_BASE + CONFIG_SPL_MAX_SIZE <= CONFIG_SPL_BSS_START_ADDR,
	       "P4 SPL image would overlap BSS");
_Static_assert(CONFIG_SPL_BSS_START_ADDR + CONFIG_SPL_BSS_MAX_SIZE <=
	       CONFIG_CUSTOM_SYS_INIT_SP_ADDR - (1u << CONFIG_STACK_SIZE_SHIFT) -
	       CONFIG_SPL_SYS_MALLOC_F_LEN - sizeof(gd_t) - 16,
	       "P4 SPL BSS would overwrite early malloc/GD");
_Static_assert(CONFIG_CUSTOM_SYS_INIT_SP_ADDR < P4_LOW_SRAM_TOP,
	       "P4 SPL stack would enter reserved ROM/AP SRAM");

#define REG(a) ((void *)(uintptr_t)(a))
#define EFUSE_BASE	0x5012d000u
#define LP_WDT		0x50116000u
#define TIMG0		0x500c2000u
#define WDT_KEY		0x50d83aa1u
#define RWDT_CFG	0x00
#define RWDT_STAGE0	0x04
#define RWDT_FEED	0x14
#define RWDT_LOCK	0x18
#define SWD_CFG		0x1c
#define SWD_LOCK	0x20
#define RWDT_EN		BIT(31)
#define RWDT_STAGE_MASK (7u << 28)
#define RWDT_CHIP_RESET (4u << 28)
#define RWDT_FLASHBOOT	BIT(12)
#define SWD_OFF		BIT(30)
#define SWD_AUTO_FEED	BIT(18)

void p4_spl_wdt_feed(void)
{
	writel(WDT_KEY, REG(LP_WDT + RWDT_LOCK));
	writel(BIT(31), REG(LP_WDT + RWDT_FEED));
	writel(0, REG(LP_WDT + RWDT_LOCK));
}

void p4_spl_wdt_finish(void)
{
	writel(WDT_KEY, REG(LP_WDT + RWDT_LOCK));
	clrbits_le32(REG(LP_WDT + RWDT_CFG), RWDT_EN);
	writel(0, REG(LP_WDT + RWDT_LOCK));
}

static void wdt_start(void)
{
	/* Keep an active reset watchdog while taking ownership from ROM.
	 * 612,000 slow-clock ticks preserve the rev1.3 reference window. */
	writel(WDT_KEY, REG(LP_WDT + RWDT_LOCK));
	writel(612000, REG(LP_WDT + RWDT_STAGE0));
	clrsetbits_le32(REG(LP_WDT + RWDT_CFG), RWDT_STAGE_MASK | RWDT_FLASHBOOT,
			RWDT_EN | RWDT_CHIP_RESET);
	writel(BIT(31), REG(LP_WDT + RWDT_FEED));
	writel(0, REG(LP_WDT + RWDT_LOCK));

	writel(WDT_KEY, REG(TIMG0 + 0x64));
	clrbits_le32(REG(TIMG0 + 0x48), BIT(14));
	setbits_le32(REG(TIMG0 + 0x48), BIT(22));
	writel(0, REG(TIMG0 + 0x64));

	/* Retain the independent super watchdog, with the ROM's auto-feed. */
	writel(WDT_KEY, REG(LP_WDT + SWD_LOCK));
	clrsetbits_le32(REG(LP_WDT + SWD_CFG), SWD_OFF, SWD_AUTO_FEED | BIT(19));
	writel(0, REG(LP_WDT + SWD_LOCK));
}

/* Same mandatory low-address protection as the previous boot stage; only
 * entries whose policy is unambiguous in the rev1.3 memory map are used.
 * Never unlock a ROM-locked entry or widen the PSRAM window here.
 */
static int protect_regions(void)
{
	u32 p0, p1, p2, p3;
	const u32 want0 = 0x009d9b9b, want1 = 0x001d000f;
	const u32 want2 = 0x1f000000, want3 = 0x9b000000;

	asm volatile("csrr %0, pmpcfg0; csrr %1, pmpcfg1; "
		     "csrr %2, pmpcfg2; csrr %3, pmpcfg3"
		     : "=r"(p0), "=r"(p1), "=r"(p2), "=r"(p3));
	/* ROM may leave these entries configured but never an unrelated PMP
	 * owner. Unexpected permission words must not be silently erased. */
	if ((p0 & ~want0) || (p1 & ~want1) || (p2 & ~want2) || (p3 & ~want3))
		return -1;

#define PMP_ADDR(csr, addr) asm volatile("csrw " #csr ", %0" :: "r"(addr))
	PMP_ADDR(pmpaddr0, 0x09ffffffu); /* locked RW 0x20000000..0x30000000 */
	PMP_ADDR(pmpaddr1, 0x0ffc3fffu); /* locked RW SRAM cache-controller aperture */
	PMP_ADDR(pmpaddr2, 0x13f03fffu); /* locked RX boot ROM */
	PMP_ADDR(pmpaddr3, 0x13fc0000u); /* lower limit of SRAM entry */
	PMP_ADDR(pmpaddr4, P4_PHYS_SRAM_END >> 2); /* TOR RWX retained HP SRAM */
	PMP_ADDR(pmpaddr6, 0x107fffffu); /* RX flash mapping */
	PMP_ADDR(pmpaddr11, 0x14042fffu); /* RWX LP SRAM */
	PMP_ADDR(pmpaddr15, 0x1401ffffu); /* locked RW HP peripheral range */
#undef PMP_ADDR
	asm volatile("csrw pmpcfg0, %0; csrw pmpcfg1, %1; "
		     "csrw pmpcfg2, %2; csrw pmpcfg3, %3; fence"
		     :: "r"(want0), "r"(want1), "r"(want2), "r"(want3)
		     : "memory");
	asm volatile("csrr %0, pmpcfg0; csrr %1, pmpcfg1; "
		     "csrr %2, pmpcfg2; csrr %3, pmpcfg3"
		     : "=r"(p0), "=r"(p1), "=r"(p2), "=r"(p3));
	return p0 != want0 || p1 != want1 || p2 != want2 || p3 != want3;
}

/* Rev1.3 PMA excludes the SoC address gaps while leaving the contiguous
 * instruction/data window cacheable. CSR fields and address boundaries:
 * ESP32-P4 TRM CPU chapter, Physical Memory Attributes. Apply on the boot
 * hart only; CPU1 keeps the normal mask-ROM attributes and waiter PMP. */
static void memory_attributes(void)
{
	const u32 guard = BIT(29) | BIT(0);
	const u32 off = guard;
	const u32 tor = 0x40000000u | guard;
	const u32 napot = 0xc0000000u | guard;
	const u32 rwx = BIT(4) | BIT(3) | BIT(2);

#define PMA(cfgreg, areg, address, cfg) \
	asm volatile("csrw " #areg ", %0; csrw " #cfgreg ", %1" \
		     :: "r"(address), "r"(cfg) : "memory")
	PMA(0xbc0, 0xbd0, 0x03ffffffu, napot);	/* address prefix gap */
	PMA(0xbc1, 0xbd1, 0x0c000000u, off);
	PMA(0xbc2, 0xbd2, 0x0c040000u, tor);
	PMA(0xbc3, 0xbd3, 0x0c040800u, off);
	PMA(0xbc4, 0xbd4, 0x0ffc0000u, tor);
	PMA(0xbc5, 0xbd5, 0x0ffc8000u, off);
	PMA(0xbc6, 0xbd6, 0x10000000u, tor);	/* SPI flash lower bound */
	PMA(0xbc7, 0xbd7, 0x117fffffu, napot);
	PMA(0xbc8, 0xbd8, 0x13000000u, off);	/* post-PSRAM gap */
	PMA(0xbc9, 0xbd9, 0x13f00000u, tor);
	PMA(0xbca, 0xbda, 0x13f08000u, off);	/* post-ROM gap */
	PMA(0xbcb, 0xbdb, 0x13fc0000u, tor);
	PMA(0xbcc, 0xbdc, 0x13ff7fffu, napot);
	PMA(0xbcd, 0xbdd, 0x11ffffffu, napot | rwx);
	PMA(0xbce, 0xbde, 0x23ff0000u, off);	/* end of uncached SRAM */
	PMA(0xbcf, 0xbdf, 0x3fffffffu, tor);
#undef PMA
	asm volatile("fence rw,rw; fence.i" ::: "memory");
}

int p4_spl_hw_init(void)
{
	u32 rev = readl(REG(EFUSE_BASE + 0x4c));
	u32 crypt = (readl(REG(EFUSE_BASE + 0x34)) >> 18) & 7;

	/* An unsigned development FIT must not replace a protected boot flow.
	 * The ROM already loaded the SPL; these are read-only checks. */
	if ((readl(REG(EFUSE_BASE + 0x38)) & BIT(20)) ||
	    ((crypt ^ (crypt >> 1) ^ (crypt >> 2)) & 1))
		return -1;
	if ((rev & (BIT(23) | 0x3f)) != 0x13)
		return -2;
	wdt_start();
	/* Route mode-1 brownout to the hardware reset, not a soft handler. */
	clrbits_le32(REG(0x5011301cu), BIT(1));
	setbits_le32(REG(0x50113004u), BIT(31));
	if (protect_regions())
		return -3;
	memory_attributes();
	return 0;
}
