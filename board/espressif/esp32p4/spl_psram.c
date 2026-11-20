// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Native ESP32-P4 rev1.3 in-package AP Hex PSRAM. Use MSPI3 for mode and
 * training transactions through the mask ROM, MSPI2 for cached memory.
 * AP mode/density fields, P4 rev1.x registers and ROM ABI were checked
 * against the device documentation and ESP-IDF a602e67b. No IDF code or
 * object is linked. This executes with hart1 held in reset.
 */
#include <common.h>
#include <asm/io.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include "psram-handoff.h"
#include "cache-range.h"
#include "spl_hw.h"
#include "spl_psram.h"
#include "../common/ap-psram.h"
#include "../common/mspi-mmu.h"

#define REG(a) ((void *)(uintptr_t)(a))
#define HP_CLK		0x500e6000u
#define LP_ANA		0x50124000u
#define LP_IO		0x50120000u
#define LP_CLK		0x50111000u
#define PMU		0x50115000u
#define EFUSE		0x5012d000u
#define MSPI2		0x5008e000u
#define MSPI3		0x5008f000u
#define IOMUX		0x500e1200u
#define DQS0		0x3cu
#define DQS1		0x68u
#define PLL_CAL		(HP_CLK + 0xbcu)
#define CAL_STOP	BIT(9)
#define CAL_DONE	BIT(8)
#define MPLL_POWER	BIT(24)
#define I2C_RESETB	BIT(26)
#define I2C_POWER	BIT(27)
#define I2C_BUSY	BIT(25)
#define PSRAM_RST	(BIT(23) | BIT(25))
#define ROM_CFG		0x4fc00108u
#define ROM_XFER	0x4fc0010cu
#define ROM_MODE	0x4fc00110u
#define OPI_DTR		7
#define PSRAM_PORT	3
#define PSRAM_CS	BIT(1)

#define P4_SPEED_MHZ (IS_ENABLED(CONFIG_ESP32P4_SPL_PSRAM_200MHZ) ? 200u : 80u)
#define P4_MPLL_MHZ (P4_SPEED_MHZ == 200u ? 400u : 320u)
#define READ_LAT (P4_SPEED_MHZ == 200u ? 4u : 2u)
#define WRITE_LAT (P4_SPEED_MHZ == 200u ? 1u : 2u)
#define DUMMY_MR (P4_SPEED_MHZ == 200u ? 12u : 8u)
#define DUMMY_READ (P4_SPEED_MHZ == 200u ? 26u : 18u)
#define DUMMY_WRITE DUMMY_MR

void p4_spl_early_puts(const char *s);
void p4_spl_early_hex(u32 v);

static int spin_mask(unsigned int addr, unsigned int mask, bool on)
{
	unsigned int i;

	for (i = 0; i < 500000; i++)
		if (!!(readl(REG(addr)) & mask) == on)
			return 0;
	return -1;
}

static int mpll_rw(u8 addr, u8 *value, bool write)
{
	u32 word = 0x63u | ((u32)addr << 8);

	if (spin_mask(LP_ANA, I2C_BUSY, false))
		return -1;
	if (write)
		word |= BIT(24) | ((u32)*value << 16);
	writel(word, REG(LP_ANA));
	if (spin_mask(LP_ANA, I2C_BUSY, false))
		return -1;
	if (!write)
		*value = (u8)(readl(REG(LP_ANA)) >> 16);
	return 0;
}

static void power_vo2(void)
{
	/* In-package 1.8 V rail, PMU VO2 (P1 0P1A), not the SPI NOR VDDO.
	 * Prefer the part's calibrated 1.8 V pair; use the nominal pair if
	 * this eFuse block does not carry one. DREF=8:0.9 V, MUL=4:2x. */
	u32 a = readl(REG(EFUSE + 0x4c)), b = readl(REG(EFUSE + 0x50));
	u32 dref = a >> 28, mul = (b >> 3) & 7;

	if (!(a & (0x3fu << 8)) || !dref || !mul) {
		dref = 8;
		mul = 4;
	}
	clrsetbits_le32(REG(PMU + 0x1d0), (3u << 9) | BIT(14), BIT(7));
	clrsetbits_le32(REG(PMU + 0x1d4), (0xfu << 28) | (7u << 23),
			(dref << 28) | (mul << 23));
	setbits_le32(REG(PMU + 0x1d0), BIT(8));
	udelay(20);
}

static int mpll_init(void)
{
	u8 v;

	power_vo2();
	setbits_le32(REG(PMU + 0x15c), MPLL_POWER | I2C_POWER | I2C_RESETB);
	setbits_le32(REG(LP_CLK + 0x40), BIT(28));
	setbits_le32(REG(LP_IO), BIT(27));
	setbits_le32(REG(LP_ANA + 0x34), BIT(0));
	writel(0, REG(LP_ANA + 0x1c));
	writel(BIT(9), REG(LP_ANA + 0x20)); /* select MPLL analog bus */
	clrbits_le32(REG(PLL_CAL), CAL_STOP);
	if (mpll_rw(3, &v, false))
		return -1;
	v |= 3u << 4;
	if (mpll_rw(3, &v, true) || mpll_rw(1, &v, false))
		return -1;
	v &= ~BIT(5);
	if (mpll_rw(1, &v, true))
		return -1;
	v |= BIT(5);
	if (mpll_rw(1, &v, true))
		return -1;
	/* 40 MHz XTAL, reference divisor 2. */
	v = ((P4_MPLL_MHZ / 20 - 1) << 3) | 1;
	if (mpll_rw(2, &v, true) || spin_mask(PLL_CAL, CAL_DONE, true))
		return -1;
	setbits_le32(REG(PLL_CAL), CAL_STOP);

	setbits_le32(REG(HP_CLK + 0x14), BIT(31)); /* PSRAM sys clock */
	clrsetbits_le32(REG(HP_CLK + 0x30), (3u << 12) | (0xffu << 16),
			BIT(12) | BIT(14) | BIT(15)); /* MPLL, core /1 */
	setbits_le32(REG(HP_CLK + 0xc0), PSRAM_RST);
	clrbits_le32(REG(HP_CLK + 0xc0), PSRAM_RST);
	return 0;
}

static void bus_clock(unsigned int mhz)
{
	u32 n = P4_MPLL_MHZ / mhz;
	u32 val = ((n - 1) << 16) | ((n / 2 - 1) << 8) | (n - 1);

	/* Distinct functional clock registers for the XIP and command ports. */
	writel(val, REG(MSPI2 + 0x50));
	writel(val, REG(MSPI3 + 0x14));
}

struct mspi_rom_cmd {
	u16 cmd, ncmd;
	u32 *addr;
	u32 naddr;
	u32 *tx;
	u32 ntx;
	u32 *rx;
	u32 nrx, dummy;
};

static void psram_cmd(u16 cmd, u32 addr, u32 *tx, u32 tx_bits,
		      u32 *rx, u32 rx_bits, u32 dummy)
{
	struct mspi_rom_cmd c = {
		.cmd = cmd, .ncmd = 16, .addr = &addr, .naddr = 32,
		.tx = tx, .ntx = tx_bits, .rx = rx, .nrx = rx_bits, .dummy = dummy,
	};

	((void (*)(int, int))ROM_MODE)(PSRAM_PORT, OPI_DTR);
	((void (*)(int, const struct mspi_rom_cmd *))ROM_CFG)(PSRAM_PORT, &c);
	((void (*)(int, void *, u16, u8, bool))ROM_XFER)
	       (PSRAM_PORT, rx, rx_bits / 8, PSRAM_CS, false);
}

static u32 mr_read(unsigned int index, unsigned int nbits)
{
	u32 v = 0;

	psram_cmd(0x4040, index, NULL, 0, &v, nbits, DUMMY_MR);
	return v;
}

static void mr_write(unsigned int index, u32 v)
{
	psram_cmd(0xc0c0, index, &v, 16, NULL, 0, 0);
}

static void pad_timing(unsigned int phase, unsigned int dqs, unsigned int data)
{
	unsigned int addr;

	for (addr = 0x1c; addr <= DQS1; addr += 4) {
		if (addr == DQS0 || addr == DQS1) {
			clrsetbits_le32(REG(IOMUX + addr),
				(3u << 1) | (0xfu << 7) | (0xfu << 17) | (3u << 15),
				BIT(0) | (phase << 1) | (dqs << 7) |
				(dqs << 17) | (2u << 15));
		} else {
			clrsetbits_le32(REG(IOMUX + addr), (0xfu << 4) | (3u << 12),
					(data << 4) | (2u << 12));
		}
	}
}

static int train(void)
{
	/* No memory allocated yet. Write at low speed before testing at the
	 * selected rate, then restore those words before publishing RAM. */
	static const u32 pattern[4] = {
		0x0123feca, 0x55aaff00, 0xfe039176, 0x042d7bbb
	};
	u32 saved[4], tx[4], rx[4];
	unsigned int phase, i;
	int good = -1;
	const unsigned int training_offset = 0x1000;

	bus_clock(20);
	pad_timing(0, 0, 0);
	psram_cmd(0, training_offset, NULL, 0, saved, 128, DUMMY_READ);
	memcpy(tx, pattern, sizeof(tx));
	psram_cmd(0x8080, training_offset, tx, 128, NULL, 0, DUMMY_WRITE);
	psram_cmd(0, training_offset, NULL, 0, rx, 128, DUMMY_READ);
	if (memcmp(rx, pattern, sizeof(rx)))
		goto restore;
	bus_clock(P4_SPEED_MHZ);

	/* Prefer the zero-delay board point; scan DQS phase and opposing
	 * DQS/data delays only if the nominal point fails. */
	for (phase = 0; phase < 4 && good < 0; phase++) {
		for (i = 0; i <= 30; i++) {
			int margin = i & 1 ? (int)((i + 1) / 2) : -(int)(i / 2);

			pad_timing(phase, margin > 0 ? margin : 0,
				   margin < 0 ? -margin : 0);
			psram_cmd(0, training_offset, NULL, 0, rx, 128, DUMMY_READ);
			if (memcmp(rx, pattern, sizeof(rx)))
				continue;
			psram_cmd(0, training_offset, NULL, 0, rx, 128, DUMMY_READ);
			if (!memcmp(rx, pattern, sizeof(rx))) {
				good = i;
				break;
			}
		}
		p4_spl_wdt_feed();
	}
restore:
	if (good < 0) {
		bus_clock(20);
		pad_timing(0, 0, 0);
	}
	psram_cmd(0x8080, training_offset, saved, 128, NULL, 0, DUMMY_WRITE);
	return good < 0 ? -1 : 0;
}

static void configure_cache_port(void)
{
	/* OPI command and data read/write, 32-bit address, x16 DDR data.
	 * Dummy field encoding is controller clocks minus one. */
	u32 fmt = (P4_SPEED_MHZ == 200) ? 0x02f7c679u : 0x01f7c479u;

	writel(fmt, REG(MSPI2 + 0x40));
	writel(0x0cbc0000, REG(MSPI2 + 0x44));
	writel(0xf0000000, REG(MSPI2 + 0x48));
	writel(0xf0008080, REG(MSPI2 + 0x4c));
	/* CS setup/hold 4 cycles, hold delay 3, split on 2 KiB boundaries. */
	clrsetbits_le32(REG(MSPI2 + 0x1a0), 0x7e000fffu,
			BIT(31) | (2u << 25) | (3u << 7) |
			(3u << 2) | 3u);
	clrsetbits_le32(REG(MSPI2 + 0x174), 3u << 18, 3u << 18);
	clrsetbits_le32(REG(MSPI2 + 0xd8), 0xfu, BIT(0) | BIT(1));
	setbits_le32(REG(MSPI3 + 0xd4), BIT(1));
	setbits_le32(REG(MSPI2 + 0x0c), BIT(25) | BIT(26));
}

static int psram_pmp_open(u32 size)
{
	/* The IDF bootloader has set CPU/peripheral/ROM protection. Do not
	 * replace its first PMP config word with the legacy catch-all.
	 * Use the same PSRAM-only window as the existing AP contract. */
	u32 addr = p4_ext_pmpaddr(size), cfg = 0x1f000000, prior, actual;

	asm volatile("csrr %0, 0x3a1; csrr %1, 0x3b7"
		     : "=r"(prior), "=r"(actual));
	if (prior & 0xff000000)
		return (prior >> 24) == 0x1f && actual == addr ? 0 : -1;

	asm volatile("csrw 0x3b7, %0; csrs 0x3a1, %1; fence" ::
		     "r"(addr), "r"(cfg) : "memory");
	asm volatile("csrr %0, 0x3a1; csrr %1, 0x3b7"
		     : "=r"(prior), "=r"(actual));
	if ((prior >> 24) != 0x1f || actual != addr)
		return -1;
	return 0;
}

static int psram_mmu_map(u32 size)
{
	u32 entries = size >> 16;
	u32 i;

	/*
	 * Page size for the PSRAM side of the MMU (MSPI2 + 0x384). Never
	 * programmed before, which left the window correct only with the caches
	 * off before publishing the cached aperture and wrong as soon as
	 * the L2 was on. 0x13200004 is what the working reference holds, with
	 * bits [4:3] = 2 selecting 64 KiB pages - the same geometry the flash
	 * side is given in p4_spl_flash_init().
	 */
	writel(0x13200004u, (void *)(MSPI2 + 0x384));

	for (i = 0; i < entries; i++) {
		esp_mspi_mmu_write(REG(MSPI2 + 0x380), REG(MSPI2 + 0x37c), i,
				   i | BIT(11) | BIT(10));
	}
	for (i = 0; i < entries; i++) {
		writel(i, (void *)(MSPI2 + 0x380));
		if (readl((void *)(MSPI2 + 0x37c)) !=
		    (i | BIT(11) | BIT(10))) {
			p4_spl_early_puts("psram: MMU read-back mismatch at page ");
			p4_spl_early_hex(i);
			p4_spl_early_puts("\n");
			return -1;
		}
	}
	return 0;
}


/*
 * HIL diagnostic only. The linked selector is the sole payload byte
 * changed between reverse-order stock (0xf1) and spread (0xf3).
 * Volatile prevents optimizer specialization.
 */
const volatile u32 p4_wb_order_selector = 0x4f5244f1u;

static inline u32 probe_offset(u32 page, u32 spread_mask)
{
	/* Choose one of the last 16 lines within the same 2 KiB controller
	 * split. Page 481 remains at the original line in the 32 MiB arm.
	 * Supported periodic aliases have equal page number modulo 16. */
	return (page + 1) * P4_EXT_PAGE - 16 -
	       (((page - 1u) & spread_mask) * P4_L2_LINE_BYTES);
}

enum {
	PROBE_L1D_WB = 1,
	PROBE_L2_WB,
	PROBE_L1D_INV,
	PROBE_L2_INV,
};

static int probe_sync(u32 addr, bool dirty)
{
	if (dirty) {
		if (p4_cache_range(addr, 8, P4_SYNC_L1D, P4_SYNC_WBACK))
			return PROBE_L1D_WB;
		if (p4_cache_range(addr, 8, P4_SYNC_L2, P4_SYNC_WBACK))
			return PROBE_L2_WB;
	}
	if (p4_cache_range(addr, 8, P4_SYNC_L1D, P4_SYNC_INVALIDATE))
		return PROBE_L1D_INV;
	if (p4_cache_range(addr, 8, P4_SYNC_L2, P4_SYNC_INVALIDATE))
		return PROBE_L2_INV;
	return 0;
}

static int psram_probe_fail(const char *phase, u32 ordinal, u32 page, u32 step,
			    u32 choice)
{
	/* Read only, once, before UART; none of these registers is an ACK. */
	u32 ctrl = readl(REG(P4_SYNC_CTRL)), map = readl(REG(P4_SYNC_MAP));
	u32 addr = readl(REG(P4_SYNC_VADDR)), len = readl(REG(P4_SYNC_BYTES));
	u32 fifo = readl(REG(MSPI2 + 0x178)), raw = readl(REG(MSPI2 + 0xc8));
	u32 axie = readl(REG(MSPI2 + 0x170)), scmd = readl(REG(MSPI2 + 0x44));
	u32 rcmd = readl(REG(MSPI2 + 0x48)), wcmd = readl(REG(MSPI2 + 0x4c));
	u32 cs = readl(REG(MSPI2 + 0x1a0)), ecc = readl(REG(MSPI2 + 0x174));
	u32 dqs0 = readl(REG(IOMUX + DQS0)), dqs1 = readl(REG(IOMUX + DQS1));
	u32 fctrl = readl(REG(MSPI2 + 0x3c)), sctrl = readl(REG(MSPI2 + 0x40));
	u32 ddr = readl(REG(MSPI2 + 0xd8));

	p4_spl_early_puts("psram: cached page sync timeout phase=");
	p4_spl_early_puts(phase);
#define SHOW(label, v) do { p4_spl_early_puts(" " label "="); p4_spl_early_hex(v); } while (0)
	SHOW("reverse", choice & 1);
	SHOW("spread", (choice >> 1) & 1);
	SHOW("ord1", ordinal);
	SHOW("mmu_page", page);
	SHOW("set_idx", (addr >> 7) & 0x1ff); /* 512 KiB, 8-way, 128-byte */
	SHOW("step", step);
	SHOW("ctrl", ctrl);
	SHOW("map", map);
	SHOW("addr", addr);
	SHOW("len", len);
	SHOW("fifo", fifo);
	SHOW("raw", raw);
	SHOW("axie", axie);
	SHOW("scmd", scmd);
	SHOW("rcmd", rcmd);
	SHOW("wcmd", wcmd);
	SHOW("cs", cs);
	SHOW("ecc", ecc);
	SHOW("dqs0", dqs0);
	SHOW("dqs1", dqs1);
	SHOW("ddr", ddr);
	SHOW("sctrl", sctrl);
	SHOW("fctrl", fctrl);
#undef SHOW
	p4_spl_early_puts("\n");
	return -1;
}

/* Save and verify forward in both images. Only the traversal of dirty
 * writeback passes changes; page signatures, safety gates and all
 * controller accesses are identical. Restore original words before FIT. */
static int psram_check_window(u32 size)
{
	static u32 original[P4_EXT_MAX / P4_EXT_PAGE][2];
	u32 pages = size / P4_EXT_PAGE, order, i, k, off;
	u32 choice = p4_wb_order_selector;
	bool reverse = (choice & 1) != 0, spread = (choice & 2) != 0;
	u32 spread_mask = spread ? 15u : 0u;
	int err = 0, rc;

	for (i = 0; i < pages; i++) {
		off = probe_offset(i, spread_mask);
		rc = probe_sync(P4_EXT_RAM_BASE + off, false);
		if (rc)
			return psram_probe_fail("save", i + 1, i, rc, choice);
		for (k = 0; k < 2; k++)
			original[i][k] =
				readl((void *)(P4_EXT_RAM_BASE + off + 4 * k));
	}
	for (order = 0; order < pages; order++) {
		u32 sig;

		i = reverse ? pages - order - 1 : order;
		off = probe_offset(i, spread_mask);
		sig = 0x6b53a7c9u ^ (i * 0x9e3779b9u);
		writel(sig, (void *)(P4_EXT_RAM_BASE + off));
		writel(~sig, (void *)(P4_EXT_RAM_BASE + off + 4));
		rc = probe_sync(P4_EXT_RAM_BASE + off, true);
		if (rc)
			return psram_probe_fail("signature", order + 1, i, rc, choice);
		if (!(order & 31))
			p4_spl_wdt_feed();
	}
	asm volatile("fence iorw, iorw" ::: "memory");
	for (i = 0; i < pages; i++) {
		u32 sig = 0x6b53a7c9u ^ (i * 0x9e3779b9u);

		off = probe_offset(i, spread_mask);
		if (readl((void *)(P4_EXT_RAM_BASE + off)) != sig ||
		    readl((void *)(P4_EXT_RAM_BASE + off + 4)) != ~sig) {
			p4_spl_early_puts("psram: mirror/data error at ");
			p4_spl_early_hex(P4_EXT_RAM_BASE + off);
			p4_spl_early_puts("\n");
			err = -1;
			break;
		}
	}
	for (order = 0; order < pages; order++) {
		i = reverse ? pages - order - 1 : order;
		off = probe_offset(i, spread_mask);
		for (k = 0; k < 2; k++)
			writel(original[i][k],
			       (void *)(P4_EXT_RAM_BASE + off + 4 * k));
		rc = probe_sync(P4_EXT_RAM_BASE + off, true);
		if (rc)
			return psram_probe_fail("restore", order + 1, i, rc, choice);
		if (!(order & 31))
			p4_spl_wdt_feed();
	}
	asm volatile("fence iorw, iorw" ::: "memory");
	p4_spl_early_puts(spread ? "psram: WB probe reverse-spread done\n" :
				  "psram: WB probe reverse-stock done\n");
	return err;
}

int p4_spl_psram_init(void)
{
	volatile u32 *record = REG(P4_EXT_HANDOFF);
	u32 mr01, mr23, mr4, mr8, id, size;

	record[0] = 0;
	if (mpll_init()) {
		p4_spl_early_puts("psram: MPLL failed to calibrate\n");
		return -1;
	}
	bus_clock(P4_SPEED_MHZ);
	/* Enable the MSPI2 SMEM DLL and the shared MSPI3 command DLL. */
	setbits_le32(REG(MSPI2 + 0x190), BIT(5));
	setbits_le32(REG(MSPI2 + 0x180), BIT(5));
	pad_timing(0, 0, 0);

	mr01 = mr_read(0, 16);
	mr23 = mr_read(2, 16);
	id = (mr01 >> 8) & 0x1f;
	size = esp_ap_psram_density(mr23);
	if ((id != 0x0d && id != 0x1a) || ((mr23 >> 5) & 7) != 6 || !size) {
		p4_spl_early_puts("psram: absent, unknown or defective AP Hex device\n");
		return -1;
	}
	if (!p4_ext_valid_size(size)) {
		p4_spl_early_puts("psram: density cannot support fixed FIT/DTB layout: ");
		p4_spl_early_hex(size);
		p4_spl_early_puts(" bytes\n");
		return -1;
	}

	mr_write(0, (mr01 & ~0x3fu) | BIT(5) | (READ_LAT << 2));
	mr_write(4, (mr_read(4, 16) & ~0xe0u) | (WRITE_LAT << 5));
	mr_write(8, (mr_read(8, 8) & ~0x4fu) | 0x4b);
	mr01 = mr_read(0, 16);
	mr4 = mr_read(4, 8);
	mr8 = mr_read(8, 8);
	p4_spl_early_puts("psram: MR01/MR23/MR4/MR8 ");
	p4_spl_early_hex(mr01);
	p4_spl_early_puts(" ");
	p4_spl_early_hex(mr23);
	p4_spl_early_puts(" ");
	p4_spl_early_hex(mr4);
	p4_spl_early_puts(" ");
	p4_spl_early_hex(mr8);
	p4_spl_early_puts("\n");
	if ((mr8 & 0x4f) != 0x4b ||
	    (mr01 & 0x3f) != (BIT(5) | (READ_LAT << 2)) ||
	    (mr4 >> 5) != WRITE_LAT || train()) {
		p4_spl_early_puts("psram: MR readback or DQS training failed\n");
		return -1;
	}
	configure_cache_port();
	if (psram_mmu_map(size))
		return -1;
	setbits_le32(REG(0x500e5020u), BIT(0) | BIT(1) | BIT(4) | BIT(5));
	((void (*)(u32))0x4fc004f4u)(0); /* Cache_Enable_L1_DCache */
	((void (*)(u32))0x4fc00504u)(0); /* Cache_Enable_L2_Cache */
	p4_spl_wdt_feed();
	/* Enable PSRAM AXI requests and clear CLOSE_AXI_INF_EN (SLV_ERR only). */
	clrsetbits_le32(REG(MSPI2 + 0x3c), BIT(31), BIT(0));
	if (psram_check_window(size) || psram_pmp_open(size)) {
		p4_spl_early_puts("psram: memory test/PMP failed; no handoff\n");
		return -1;
	}
	record[1] = size;
	record[2] = ~size;
	asm volatile("fence rw,w" ::: "memory");
	record[0] = P4_EXT_MAGIC;
	p4_spl_early_puts("psram: x16 bytes ");
	p4_spl_early_hex(size);
	p4_spl_early_puts(" MHz ");
	p4_spl_early_hex(P4_SPEED_MHZ);
	p4_spl_early_puts(" page/alias test OK (cached readback)\n");
	return 0;
}
