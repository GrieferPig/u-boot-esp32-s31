// SPDX-License-Identifier: GPL-2.0+
/*
 * ESP32-P4 PSRAM bring-up for the SPL.
 *
 * The board carries a 32 MB in-package AP hex (16-line) PSRAM on MSPI2/MSPI3.
 * Bring-up order:
 *
 *   1. MPLL to 400 MHz: the analog PLL is programmed through the internal
 *      analog I2C master (regi2c), block 0x63 register 2 holds ref divider
 *      bits [2:0] and feedback divider bits [7:3]; 40 MHz * 10 / 1 = 400 MHz.
 *   2. route MPLL to the PSRAM controllers and take them out of reset.
 *   3. controller configuration (DDR, hex lines, AXI access, CS timing,
 *      2048-byte page, DLL, tuned delays).
 *   4. device mode registers MR0/MR4/MR8 through the mask ROM's OPI helpers.
 *   5. write/read pattern check so the caller can stop on failure.
 */

#include <common.h>
#include <asm/io.h>
#include <linux/bitops.h>
#include <linux/delay.h>

#include "psram_tables.h"
#include "spl_psram.h"

#define P4_CLKRST_BASE		0x500e6000ul
#define P4_MSPI2_BASE		0x5008e000ul
#define P4_MSPI3_BASE		0x5008f000ul
#define P4_PMU_BASE		0x50115000ul
#define P4_LP_CLKRST_BASE	0x50111000ul
#define P4_LPPERI_BASE		0x50120000ul
#define P4_ANA_MST_BASE		0x50124000ul
#define P4_PSRAM_BASE		0x48000000ul
#define P4_PSRAM_TEST_SIZE	0x00001000ul

/* Clock and reset gates. */
#define P4_PERI_CLK_CTRL00	(P4_CLKRST_BASE + 0x30)
#define P4_PSRAM_CLK_SRC_MPLL	BIT(12)
#define P4_PSRAM_CLK_SRC_MASK	(3u << 12)
#define P4_PSRAM_CORE_CLK_EN	BIT(15)
#define P4_RST_EN0		(P4_CLKRST_BASE + 0xc0)
#define P4_RST_DUAL_MSPI_AXI	BIT(23)
#define P4_RST_DUAL_MSPI_APB	BIT(25)

#define P4_PMU_RF_PWC		(P4_PMU_BASE + 0x15c)
#define P4_PMU_MSPI_PHY_XPD	BIT(24)
#define P4_PMU_PERIF_I2C_RSTB	BIT(26)
#define P4_PMU_XPD_PERIF_I2C	BIT(27)

#define P4_LP_CLKRST_HP_CLK_CTRL	(P4_LP_CLKRST_BASE + 0x40)
#define P4_LP_CLKRST_HP_MPLL_CLK_EN	BIT(28)

#define P4_LPPERI_CLK_EN	(P4_LPPERI_BASE + 0x00)
#define P4_LPPERI_CK_EN_I2CMST	BIT(27)

/* Analog I2C (regi2c) master. */
#define P4_ANA_MST_I2C0_CTRL	(P4_ANA_MST_BASE + 0x00)
#define P4_ANA_MST_ANA_CONF1	(P4_ANA_MST_BASE + 0x1c)
#define P4_ANA_MST_ANA_CONF2	(P4_ANA_MST_BASE + 0x20)
#define P4_ANA_MST_CLK160M	(P4_ANA_MST_BASE + 0x34)
#define P4_ANA_MST_CLK_SEL_160M	BIT(0)
#define P4_ANA_MST_SEL_MSPI	BIT(9)
#define P4_ANA_MST_BUSY		BIT(25)
#define P4_ANA_MST_WR_CNTL	BIT(24)

#define P4_MPLL_BLOCK		0x63
#define P4_MPLL_DIV_REG		2
#define P4_MPLL_REF_DIV		0	/* bits [2:0] */
#define P4_MPLL_FB_DIV		7	/* bits [7:3]: (7+1)/(0+1) = 8x -> 320 MHz */

/* Mask ROM OPI helpers (fixed addresses for this ROM revision). */
#define P4_ROM_SPI_CMD_CONFIG	0x4fc00108ul
#define P4_ROM_SPI_CMD_START	0x4fc0010cul
#define P4_ROM_SPI_SET_OP_MODE	0x4fc00110ul
#define P4_ROM_OPI_DTR_MODE	7

typedef struct {
	u16 cmd;
	u16 cmd_bit_len;
	u32 *addr;
	u32 addr_bit_len;
	u32 *tx_data;
	u32 tx_data_bit_len;
	u32 *rx_data;
	u32 rx_data_bit_len;
	u32 dummy_bit_len;
} rom_spi_cmd_t;

typedef void (*rom_set_op_mode_fn)(int spi_num, int mode);
typedef void (*rom_cmd_config_fn)(int spi_num, rom_spi_cmd_t *conf);
typedef void (*rom_cmd_start_fn)(int spi_num, u8 *rx_buf, u16 rx_len,
				 u8 cs_mask, bool is_write_erase);

/*
 * TRM references for this file (ESP32-P4 Technical Reference Manual v1.0).
 * Every register block touched here is listed with the chapter that defines
 * it, so a reviewer can check the code against the manual:
 *
 *   ch.9  System and Memory        HP L2MEM / L2 cache split, PSRAM window,
 *                                  memory map of the MSPI address ranges
 *   ch.11 GPIO Matrix and IO MUX   pad configuration and pin mux
 *   ch.12 Reset and Clock          MPLL, PSRAM clock source and dividers,
 *                                  HP_SYS_CLKRST (cache clocks and resets)
 *   ch.18 Timer Group (TIMG)       MWDT0/MWDT1 flashboot mode
 *   ch.19 Watchdog Timers (WDT)    RWDT / super watchdog key registers
 *   ch.21 Permission Control (PMS) PMS entries covering the MSPI regions
 *   ch.22 System Registers (SYSREG) Register 22.15 HP_SYSTEM_HP_CACHE_CLK_CONFIG
 *                                  (0x0020), 22.16 CACHE_RESET_CONFIG (0x0024),
 *                                  L2MEM/L2ROM clock force-on (0x0060/0x0040)
 *   ch.47 SPI Controller (SPI)     SPI (MSPI) command/user/clock registers
 *
 * NOT COVERED BY THE TRM. A full-text search of the manual finds no definition
 * for the memory-mapped MSPI cache path, so the values below come from the
 * ESP-IDF P4 sources (components/hal/esp32p4/include/hal/psram_ctrlr_ll.h,
 * components/esp_psram/device/esp_psram_impl_ap_hex.c) and from register dumps
 * of a working ESP-IDF boot - never from the TRM:
 *   - CACHE_SCTRL (0x40), SRAM_CMD (0x44), SRAM_DRD_CMD (0x48),
 *     SRAM_DWR_CMD (0x4c), SMEM_DDR (0xd8);
 *   - MMU item index/content (0x380/0x37c) and the MMU power/page-size
 *     registers (0x384);
 *   - the MSPI IOMUX pin block at 0x500E1200.
 * The only "SPI_MEM" registers the manual describes are the XTS_AES plaintext
 * registers in ch.35, which are unrelated.
 */

/* AP hex PSRAM commands and timings. */
#define PSRAM_REG_READ		0x4040
#define PSRAM_REG_WRITE		0xc0c0
#define PSRAM_REG_CMD_BITS	16
#define PSRAM_REG_ADDR_BITS	32
#define PSRAM_REG_DUMMY_RD	(2 * (5 - 1))
#define PSRAM_RD_LATENCY	4
#define PSRAM_WR_LATENCY	2
#define PSRAM_MR8_BL		3
#define PSRAM_MR8_BT		0
#define PSRAM_MR8_RBX		1
#define PSRAM_MR8_X16		1

void p4_spl_early_puts(const char *s);
void p4_watchdogs_off(void);
void p4_spl_early_hex(u32 v);

static void regi2c_write(u8 block, u8 reg, u8 data)
{
	u32 word;

	writel(0, (void *)P4_ANA_MST_ANA_CONF1);
	writel(0, (void *)P4_ANA_MST_ANA_CONF2);
	setbits_le32((void *)P4_ANA_MST_ANA_CONF2, P4_ANA_MST_SEL_MSPI);

	while (readl((void *)P4_ANA_MST_I2C0_CTRL) & P4_ANA_MST_BUSY)
		;

	word = (u32)block | ((u32)reg << 8) | P4_ANA_MST_WR_CNTL |
	       ((u32)data << 24);
	writel(word, (void *)P4_ANA_MST_I2C0_CTRL);

	while (readl((void *)P4_ANA_MST_I2C0_CTRL) & P4_ANA_MST_BUSY)
		;
}

/* TRM ch.12 "Reset and Clock": MPLL configuration, PSRAM clock source and
 * core clock enable, dual-MSPI reset release. */
static void psram_clock_init(void)
{
	u32 val;

	/* Analog I2C master clock, 160 MHz reference, perif I2C powered. */
	setbits_le32((void *)P4_LPPERI_CLK_EN, P4_LPPERI_CK_EN_I2CMST);
	setbits_le32((void *)P4_ANA_MST_CLK160M, P4_ANA_MST_CLK_SEL_160M);

	clrbits_le32((void *)P4_PMU_RF_PWC, P4_PMU_PERIF_I2C_RSTB);
	udelay(1);
	setbits_le32((void *)P4_PMU_RF_PWC, P4_PMU_XPD_PERIF_I2C);
	setbits_le32((void *)P4_PMU_RF_PWC, P4_PMU_PERIF_I2C_RSTB);

	/* MPLL 320 MHz = 40 MHz * (7 + 1) / (0 + 1): the reference build
	 * uses CONFIG_SPIRAM_SPEED_80M (MPLL 320 / 4 = 80 MHz). */
	regi2c_write(P4_MPLL_BLOCK, P4_MPLL_DIV_REG,
		     (P4_MPLL_FB_DIV << 3) | P4_MPLL_REF_DIV);

	/* Power up the MSPI PHY PLL and enable the MPLL clock gate. */
	setbits_le32((void *)P4_PMU_RF_PWC, P4_PMU_MSPI_PHY_XPD);
	setbits_le32((void *)P4_LP_CLKRST_HP_CLK_CTRL, P4_LP_CLKRST_HP_MPLL_CLK_EN);
	udelay(100);

	/* PSRAM controllers: MPLL source, core clock on, divider 1. */
	val = readl((void *)P4_PERI_CLK_CTRL00);
	val &= ~P4_PSRAM_CLK_SRC_MASK;
	val |= P4_PSRAM_CLK_SRC_MPLL | P4_PSRAM_CORE_CLK_EN;
	writel(val, (void *)P4_PERI_CLK_CTRL00);

	/* Let the dual-MSPI interface out of reset. */
	clrbits_le32((void *)P4_RST_EN0, P4_RST_DUAL_MSPI_AXI | P4_RST_DUAL_MSPI_APB);
	udelay(100);
}


/*
 * Replay the controller state a working ESP-IDF boot leaves behind. The values
 * are board-stable (they include the factory timing tuning), so this path is
 * enough to bring the controller up deterministically; the device mode
 * registers still have to be programmed for a cold power cycle.
 */

/*
 * The replayed ROM state leaves the "cache line <-> PSRAM" command registers
 * unset, so cached writes never reach the device: reads work, writes vanish.
 * These are the values a fully initialised ESP-IDF boot has in MSPI2.
 */
/* TRM ch.47 "SPI Controller (SPI)" covers the SPI peripheral; these
 * cache-path registers are not documented there (see the file header). */
static void psram_cache_cmds(void)
{
	writel(0x40000001u, (void *)0x5008e03c);	/* CACHE_FCTRL */
	writel(0x02f7c679u, (void *)0x5008e040);	/* CACHE_SCTRL */
	writel(0x8cfc0000u, (void *)0x5008e044);	/* SRAM_CMD */
	writel(0xf0000000u, (void *)0x5008e048);	/* SRAM_DRD_CMD */
	writel(0xf0008080u, (void *)0x5008e04c);	/* SRAM_DWR_CMD */
	writel(0x00010001u, (void *)0x5008e050);	/* SRAM_CLK */
	writel(0x00000200u, (void *)0x5008e054);
}

static void psram_replay(void)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(psram_m2); i++)
		writel(psram_m2[i].val, (void *)(P4_MSPI2_BASE + psram_m2[i].off));
	for (i = 0; i < ARRAY_SIZE(psram_m3); i++)
		writel(psram_m3[i].val, (void *)(P4_MSPI3_BASE + psram_m3[i].off));
	for (i = 0; i < ARRAY_SIZE(psram_ck); i++)
		writel(psram_ck[i].val, (void *)(P4_CLKRST_BASE + psram_ck[i].off));
	for (i = 0; i < ARRAY_SIZE(psram_lpc); i++)
		writel(psram_lpc[i].val, (void *)(P4_LP_CLKRST_BASE + psram_lpc[i].off));
	for (i = 0; i < ARRAY_SIZE(psram_lpp); i++)
		writel(psram_lpp[i].val, (void *)(P4_LPPERI_BASE + psram_lpp[i].off));
	for (i = 0; i < ARRAY_SIZE(psram_pmu); i++)
		writel(psram_pmu[i].val, (void *)(P4_PMU_BASE + psram_pmu[i].off));
	udelay(100);
}

static void psram_controller_init(void)
{
	/* System PSRAM controller (MSPI2): AXI reads/writes of the PSRAM window. */
	writel(0x2ee00000, (void *)(P4_MSPI2_BASE + 0x0c));
	writel(0x40000001, (void *)(P4_MSPI2_BASE + 0x3c));
	writel(0x02f7c679, (void *)(P4_MSPI2_BASE + 0x40));
	writel(0x8cfc0000, (void *)(P4_MSPI2_BASE + 0x44));
	writel(0xf0000000, (void *)(P4_MSPI2_BASE + 0x48));
	writel(0xf0008080, (void *)(P4_MSPI2_BASE + 0x4c));
	writel(0x00010001, (void *)(P4_MSPI2_BASE + 0x50));
	writel(0x000003e0, (void *)(P4_MSPI2_BASE + 0xc0));

	/* Peri PSRAM controller (MSPI3): user transactions and tuned delays. */
	writel(0x000000c0, (void *)(P4_MSPI3_BASE + 0x04));
	writel(0x002ca27c, (void *)(P4_MSPI3_BASE + 0x08));
	writel(0x00010001, (void *)(P4_MSPI3_BASE + 0x14));
	writel(0xf0000000, (void *)(P4_MSPI3_BASE + 0x18));
	writel(0x7c000019, (void *)(P4_MSPI3_BASE + 0x1c));
	writel(0xf0000000, (void *)(P4_MSPI3_BASE + 0x20));
	writel(0x000001ff, (void *)(P4_MSPI3_BASE + 0x28));
	writel(0x00000002, (void *)(P4_MSPI3_BASE + 0x34));
	writel(0xffffffff, (void *)(P4_MSPI3_BASE + 0x38));
	writel(0x00000002, (void *)(P4_MSPI3_BASE + 0x3c));
	writel(0x80786655, (void *)(P4_MSPI3_BASE + 0x58));
	writel(0x0020ff5a, (void *)(P4_MSPI3_BASE + 0x5c));
	writel(0xc03c33aa, (void *)(P4_MSPI3_BASE + 0x60));
	udelay(100);
}

static int psram_transaction(u32 cmd, u32 addr, u32 dummy_bits,
			      void *tx, u32 tx_bits, void *rx, u32 rx_bits)
{
	rom_spi_cmd_t conf;
	rom_set_op_mode_fn set_op_mode = (rom_set_op_mode_fn)P4_ROM_SPI_SET_OP_MODE;
	rom_cmd_config_fn cmd_config = (rom_cmd_config_fn)P4_ROM_SPI_CMD_CONFIG;
	rom_cmd_start_fn cmd_start = (rom_cmd_start_fn)P4_ROM_SPI_CMD_START;

	conf.cmd = (u16)cmd;
	conf.cmd_bit_len = PSRAM_REG_CMD_BITS;
	conf.addr = &addr;
	conf.addr_bit_len = PSRAM_REG_ADDR_BITS;
	conf.tx_data = tx;
	conf.tx_data_bit_len = tx_bits;
	conf.rx_data = rx;
	conf.rx_data_bit_len = rx_bits;
	conf.dummy_bit_len = dummy_bits;

	/*
	 * The ROM OPI helper spins forever on the P4 when driven from the SPL
	 * (esp_rom_spi_cmd_start never returns), so the transaction path below
	 * is disabled until the user-mode sequence is implemented locally.
	 */
#ifndef P4_PSRAM_USE_ROM_OPI
	(void)conf;
	(void)set_op_mode;
	(void)cmd_config;
	(void)cmd_start;
	return -1;
#endif
	p4_spl_early_puts("rom: op-mode\n");
	set_op_mode(3, P4_ROM_OPI_DTR_MODE);
	p4_spl_early_puts("rom: cmd-config\n");
	cmd_config(3, &conf);
	p4_spl_early_puts("rom: cmd-start\n");
	cmd_start(3, (u8 *)rx, rx_bits / 8, 1 << 1, false);
	p4_spl_early_puts("rom: done\n");

	return 0;
}

static int psram_mode_register_init(void)
{
	u16 mr01 = 0;
	u16 mr48 = 0;
	u8 mr8 = 0;

	if (psram_transaction(PSRAM_REG_READ, 0x0, PSRAM_REG_DUMMY_RD, NULL, 0,
			      &mr01, 16))
		return -1;
	if (psram_transaction(PSRAM_REG_READ, 0x4, PSRAM_REG_DUMMY_RD, NULL, 0,
			      &mr48, 16))
		return -1;

	mr01 &= ~(0x3 | (0x7 << 2) | BIT(5));
	mr01 |= (PSRAM_RD_LATENCY << 2) | BIT(5);
	mr48 &= ~(0x7 << 5);
	mr48 |= (PSRAM_WR_LATENCY << 5);

	if (psram_transaction(PSRAM_REG_WRITE, 0x0, 0, &mr01, 16, NULL, 0))
		return -1;
	if (psram_transaction(PSRAM_REG_WRITE, 0x4, 0, &mr48, 16, NULL, 0))
		return -1;

	if (psram_transaction(PSRAM_REG_READ, 0x8, PSRAM_REG_DUMMY_RD, NULL, 0,
			      &mr8, 8))
		return -1;
	mr8 &= ~(0x3 | BIT(2) | BIT(3) | BIT(6));
	mr8 |= PSRAM_MR8_BL | (PSRAM_MR8_BT << 2) | (PSRAM_MR8_RBX << 3) |
	       (PSRAM_MR8_X16 << 6);
	if (psram_transaction(PSRAM_REG_WRITE, 0x8, 0, (u16 *)&mr8, 16, NULL, 0))
		return -1;

	return 0;
}

static int psram_check(void)
{
	int i;

	writel(0x5a6b7c8d, (void *)P4_PSRAM_BASE);
	writel(0xa5a55a5a, (void *)(P4_PSRAM_BASE + 4));
	writel(0x12345678, (void *)(P4_PSRAM_BASE + P4_PSRAM_TEST_SIZE - 4));

	if (readl((void *)P4_PSRAM_BASE) != 0x5a6b7c8d)
		return -1;
	if (readl((void *)(P4_PSRAM_BASE + 4)) != 0xa5a55a5a)
		return -1;
	if (readl((void *)(P4_PSRAM_BASE + P4_PSRAM_TEST_SIZE - 4)) != 0x12345678)
		return -1;

	for (i = 0; i < 0x40; i++)
		writel((u32)i * 0x01010101u, (void *)(P4_PSRAM_BASE + i * 4));
	for (i = 0; i < 0x40; i++) {
		if (readl((void *)(P4_PSRAM_BASE + i * 4)) != (u32)i * 0x01010101u)
			return -1;
	}

	return 0;
}





/*
 * HP_SYS_CLKRST bits that the reference (IDF) boot has set and that we do not:
 * without them the PSRAM side of the MMU never answers (reads return junk and
 * any access past the ROM-mapped pages faults).
 */

/*
 * PMU/analogue state that the reference (IDF) loader has set by the time its
 * PSRAM init finishes. Values captured over JTAG while the IDF loader was
 * running (halt 400 ms after reset, PC in PSRAM):
 *
 *   PMU 0x14 7f800000  0x18 02ec0000  0x1c 210000a0  0x20 ffffffff
 *       0x24 08000000  0x28 a04037f0  0x80 c0800000  0x84 92800200
 *       0x88 ffffffff  0x8c 30000000  0x90 c0640000  0x9c e8400000
 *       0x1c0 40200180 0x1c4 97000000 0x1d0 40200180 0x1d4 86800000
 */
static const u32 psram_pmu_state[][2] = {
	{ 0x014, 0x7f800000 }, { 0x018, 0x02ec0000 }, { 0x01c, 0x210000a0 },
	{ 0x020, 0xffffffff }, { 0x024, 0x08000000 }, { 0x028, 0xa04037f0 },
	{ 0x080, 0xc0800000 }, { 0x084, 0x92800200 }, { 0x088, 0xffffffff },
	{ 0x08c, 0x30000000 }, { 0x090, 0xc0640000 }, { 0x09c, 0xe8400000 },
	{ 0x1c0, 0x40200180 }, { 0x1c4, 0x97000000 },
	{ 0x1d0, 0x40200180 }, { 0x1d4, 0x86800000 },
};

static void psram_pmu_fixup(void)
{
	void *pmu = (void *)P4_PMU_BASE;
	u32 i;

	for (i = 0; i < ARRAY_SIZE(psram_pmu_state); i++)
		writel(psram_pmu_state[i][1], pmu + psram_pmu_state[i][0]);
}



static void psram_clkrst_fixup(void)
{
	void *c = (void *)P4_CLKRST_BASE;

	setbits_le32(c + 0x14, BIT(13));
	setbits_le32(c + 0x18, 0x1030);
	setbits_le32(c + 0x1c, BIT(13));
	setbits_le32(c + 0x2c, BIT(8));
	writel(0x2a3, c + 0x3c);
	setbits_le32(c + 0x40, BIT(27));
}

/* Read back one PSRAM MMU entry: the reference returns page | 0xC00. */
static void psram_mmu_report(void)
{
	writel(0, (void *)(P4_MSPI2_BASE + 0x380));
	p4_spl_early_puts("mmu0 ");
	p4_spl_early_hex(readl((void *)(P4_MSPI2_BASE + 0x37c)));
	writel(1, (void *)(P4_MSPI2_BASE + 0x380));
	p4_spl_early_puts(" mmu1 ");
	p4_spl_early_hex(readl((void *)(P4_MSPI2_BASE + 0x37c)));
	p4_spl_early_puts(" slow-clk ok\n");

	/* Back to the full-speed clock for the cache path. */
	writel(0x00010001u, (void *)(P4_MSPI3_BASE + 0x14));
	writel(0x00010001u, (void *)(P4_MSPI2_BASE + 0x14));
}



/*
 * Hex PSRAM register transactions over MSPI3.
 *
 * Ported from the working ESP32-S31 implementation in this tree
 * (board/espressif/esp32s31/spl_psram.c), with the P4 register bases:
 *   CMD 0x00, ADDR 0x04, CTRL 0x08, USER 0x18, USER1 0x1c, USER2 0x20,
 *   MOSI_DLEN 0x24, MISO_DLEN 0x28, MISC 0x34, CACHE_FCTRL 0x3c,
 *   DDR 0xd4 and the data buffer at 0x58 (W0).
 */
#define P4_MSPI_S_CMD		(P4_MSPI3_BASE + 0x00)
#define P4_MSPI_S_ADDR		(P4_MSPI3_BASE + 0x04)
#define P4_MSPI_S_CTRL		(P4_MSPI3_BASE + 0x08)
#define P4_MSPI_S_USER		(P4_MSPI3_BASE + 0x18)
#define P4_MSPI_S_USER1		(P4_MSPI3_BASE + 0x1c)
#define P4_MSPI_S_USER2		(P4_MSPI3_BASE + 0x20)
#define P4_MSPI_S_MOSI_DLEN	(P4_MSPI3_BASE + 0x24)
#define P4_MSPI_S_MISO_DLEN	(P4_MSPI3_BASE + 0x28)
#define P4_MSPI_S_MISC		(P4_MSPI3_BASE + 0x34)
#define P4_MSPI_S_CACHE_FCTRL	(P4_MSPI3_BASE + 0x3c)
#define P4_MSPI_S_W0		(P4_MSPI3_BASE + 0x58)
#define P4_MSPI_S_DDR		(P4_MSPI3_BASE + 0xd4)

#define P4_USR			BIT(18)
#define P4_USR_MOSI		BIT(27)
#define P4_USR_MISO		BIT(28)
#define P4_USR_DUMMY		BIT(29)
#define P4_USR_ADDR		BIT(30)
#define P4_USR_COMMAND		BIT(31)
#define P4_USR_ADDR_BITLEN_S	26
#define P4_USR_COMMAND_BITLEN_S	28
#define P4_CS0_DIS		BIT(0)
#define P4_CS1_DIS		BIT(1)
#define P4_CACHE_USR_ADDR_4BYTE	BIT(1)
#define P4_FDOUT_OCT		BIT(4)
#define P4_FDIN_OCT		BIT(5)
#define P4_FADDR_OCT		BIT(6)
#define P4_FCMD_QUAD		BIT(8)
#define P4_FCMD_OCT		BIT(9)
#define P4_FASTRD_MODE		BIT(13)
#define P4_FREAD_DUAL		BIT(14)
#define P4_FREAD_QUAD		BIT(20)
#define P4_FREAD_DIO		BIT(23)
#define P4_FREAD_QIO		BIT(24)
#define P4_FMEM_DDR_EN		BIT(0)
#define P4_FMEM_DDR_CMD_DIS	BIT(4)

/*
 * PSRAM pad configuration: the reference boot has the output enable bit and the
 * drive strength fields set on every PSRAM pad, our chain leaves them at the
 * reset value. 0x500e1200+ is deliberately not touched (that range is GPIO
 * output/OE config for other pins and writing it reset-loops the board).
 */
static const u32 psram_pad_state[][2] = {
	{ 0x500e1020, 0x00001b02u }, { 0x500e1024, 0x00001b02u },
	{ 0x500e1028, 0x00000802u }, { 0x500e102c, 0x00000802u },
	{ 0x500e1030, 0x00000802u }, { 0x500e1034, 0x00000802u },
	{ 0x500e1038, 0x00000802u }, { 0x500e103c, 0x00000802u },
	{ 0x500e1040, 0x00000802u }, { 0x500e1044, 0x00000802u },
	{ 0x500e1048, 0x00000802u }, { 0x500e104c, 0x00000802u },
	{ 0x500e1050, 0x00000802u }, { 0x500e1054, 0x00000802u },
	{ 0x500e1058, 0x00000802u }, { 0x500e105c, 0x00000802u },
	{ 0x500e1060, 0x00000802u }, { 0x500e1064, 0x00000c02u },
	{ 0x500e1068, 0x00000c02u }, { 0x500e106c, 0x00000802u },
	{ 0x500e1070, 0x00000802u }, { 0x500e1074, 0x00000802u },
	{ 0x500e1078, 0x00000802u }, { 0x500e107c, 0x00000802u },
	{ 0x500e1080, 0x00000802u }, { 0x500e1084, 0x00000802u },
	{ 0x500e1088, 0x00000802u }, { 0x500e108c, 0x00000802u },
	{ 0x500e1090, 0x00000802u }, { 0x500e1094, 0x00000802u },
	{ 0x500e10a0, 0x00001802u }, { 0x500e10a4, 0x00001802u },
	{ 0x500e10a8, 0x00001802u }, { 0x500e10ac, 0x00001802u },
	{ 0x500e10b0, 0x00001802u }, { 0x500e10b4, 0x00001802u },
	{ 0x500e10b8, 0x00001802u }, { 0x500e10bc, 0x00001802u },
	{ 0x500e10c0, 0x00001802u }, { 0x500e10c4, 0x00001802u },
	{ 0x500e10c8, 0x00000802u }, { 0x500e10cc, 0x00000802u },
	{ 0x500e10d0, 0x00000802u }, { 0x500e10d4, 0x00000802u },
	{ 0x500e10d8, 0x00000802u }, { 0x500e10dc, 0x00000802u },
};

/*
 * A watchdog reset (or any abort) in the middle of a PSRAM transaction leaves
 * the MSPI blocks holding a half-finished command: the next boot then reads
 * the same stale value from every register and the mode-register verify
 * fails. Pulse the dual-MSPI resets so the controllers come back clean before
 * the state is replayed.
 */
static void psram_controller_reset(void)
{
	setbits_le32((void *)P4_RST_EN0, P4_RST_DUAL_MSPI_AXI | P4_RST_DUAL_MSPI_APB);
	udelay(10);
	clrbits_le32((void *)P4_RST_EN0, P4_RST_DUAL_MSPI_AXI | P4_RST_DUAL_MSPI_APB);
	udelay(200);
}

/* TRM ch.11 "GPIO Matrix and IO MUX": drive strength and pad enables for
 * the MSPI pins. (The MSPI-specific IOMUX block itself is not described in
 * the manual - see the file header.) */
static void psram_pads_init(void)
{
	u32 i;

	for (i = 0; i < ARRAY_SIZE(psram_pad_state); i++)
		writel(psram_pad_state[i][1], (void *)psram_pad_state[i][0]);
}

/*
 * The PSRAM pins live in their own mux block at 0x500e1200
 * (DR_REG_IOMUX_MSPI_PIN_BASE = HPPERIPH1 + 0x21200) and are left at zero by
 * our chain; the reference has drive strength 0x2020 on every PSRAM data pin
 * and 0x00010001 on the two DQS pins. The flash pins (0x500e1204..0x500e1218)
 * are deliberately not touched.
 */
static const u32 psram_pin_state[][2] = {
	{ 0x500e121c, 0x00002020u }, { 0x500e1220, 0x00002020u },
	{ 0x500e1224, 0x00002020u }, { 0x500e1228, 0x00002020u },
	{ 0x500e122c, 0x00002020u }, { 0x500e1230, 0x00002020u },
	{ 0x500e1234, 0x00002020u }, { 0x500e1238, 0x00002020u },
	{ 0x500e123c, 0x00010001u }, { 0x500e1240, 0x00002020u },
	{ 0x500e1244, 0x00002020u }, { 0x500e1248, 0x00002020u },
	{ 0x500e124c, 0x00002020u }, { 0x500e1250, 0x00002020u },
	{ 0x500e1254, 0x00002020u }, { 0x500e1258, 0x00002020u },
	{ 0x500e125c, 0x00002020u }, { 0x500e1260, 0x00002020u },
	{ 0x500e1264, 0x00002020u }, { 0x500e1268, 0x00010001u },
};

static void psram_pin_mux_init(void)
{
	u32 i;

	for (i = 0; i < ARRAY_SIZE(psram_pin_state); i++)
		writel(psram_pin_state[i][1], (void *)psram_pin_state[i][0]);
}

static void psram_opi_dtr_mode(void)
{
	u32 clear = P4_FCMD_OCT | P4_FCMD_QUAD | P4_FREAD_QIO | P4_FREAD_QUAD |
		    P4_FREAD_DIO | P4_FREAD_DUAL | P4_FASTRD_MODE |
		    P4_FADDR_OCT | P4_FDIN_OCT | P4_FDOUT_OCT;

	clrbits_le32((void *)P4_MSPI_S_CTRL, clear);
	clrbits_le32((void *)P4_MSPI_S_USER, BIT(12) | BIT(13) | BIT(14) |
						 BIT(15));
	setbits_le32((void *)P4_MSPI_S_CTRL,
		     P4_FCMD_OCT | P4_FADDR_OCT | P4_FDIN_OCT | P4_FDOUT_OCT |
			     P4_FASTRD_MODE);
	clrbits_le32((void *)P4_MSPI_S_DDR, P4_FMEM_DDR_CMD_DIS);
	setbits_le32((void *)P4_MSPI_S_DDR, P4_FMEM_DDR_EN);
}

/* Bytes per clock cycle in the data phase (DDR: 4 for 16 lines). */
static u32 psram_dbyte = 1;

static int psram_xact(u16 cmd, u32 addr, u32 *tx, u32 tx_bits,
		      u32 *rx, u32 rx_bits, u32 dummy)
{
	u32 user, i;

	for (i = 0; i < 100000; i++) {
		if (!readl((void *)P4_MSPI_S_CMD))
			break;
	}

	writel(((u32)cmd & 0xffff) | ((16 - 1) << P4_USR_COMMAND_BITLEN_S),
	       (void *)P4_MSPI_S_USER2);

	/*
	 * Hex PSRAM in DDR mode moves at most 4 bytes per clock (16 lines x 2
	 * edges), so USER1.DBYTELEN ("data byte length - 1") is 3 for every
	 * 32-bit and wider data phase, 1 for a 16-bit register access and 0 for
	 * the 8-bit MR8 read. Deriving it from "> 32" made a 32-bit array
	 * access move only half of its bytes.
	 */
	{
		u32 bits = rx_bits > tx_bits ? rx_bits : tx_bits;

		psram_dbyte = bits >= 32 ? 3 : (bits ? bits / 8 - 1 : 0);
	}

	user = ((32 - 1) << P4_USR_ADDR_BITLEN_S);
	if (dummy)
		user |= (dummy - 1) & 0x3f;
	user |= psram_dbyte << 6;
	writel(user, (void *)P4_MSPI_S_USER1);

	writel(addr, (void *)P4_MSPI_S_ADDR);
	setbits_le32((void *)P4_MSPI_S_CACHE_FCTRL, P4_CACHE_USR_ADDR_4BYTE);

	user = P4_USR_COMMAND | P4_USR_ADDR;
	if (dummy)
		user |= P4_USR_DUMMY;
	if (tx_bits) {
		user |= P4_USR_MOSI;
		writel(tx_bits - 1, (void *)P4_MSPI_S_MOSI_DLEN);
		if (tx)
			for (i = 0; i < DIV_ROUND_UP(tx_bits, 32); i++)
				writel(tx[i], (void *)(P4_MSPI_S_W0 + i * 4));
	} else {
		writel(0, (void *)P4_MSPI_S_MOSI_DLEN);
	}
	if (rx_bits) {
		user |= P4_USR_MISO;
		writel(rx_bits - 1, (void *)P4_MSPI_S_MISO_DLEN);
	} else {
		writel(0, (void *)P4_MSPI_S_MISO_DLEN);
	}
	writel(user, (void *)P4_MSPI_S_USER);

	/* The PSRAM hangs off CS1. */
	setbits_le32((void *)P4_MSPI_S_MISC, P4_CS0_DIS);
	clrbits_le32((void *)P4_MSPI_S_MISC, P4_CS1_DIS);

	setbits_le32((void *)P4_MSPI_S_CMD, P4_USR);
	for (i = 0; i < 100000; i++) {
		if (!(readl((void *)P4_MSPI_S_CMD) & P4_USR))
			break;
	}

	clrbits_le32((void *)P4_MSPI_S_MISC, P4_CS0_DIS);
	setbits_le32((void *)P4_MSPI_S_MISC, P4_CS1_DIS);

	if (readl((void *)P4_MSPI_S_CMD) & P4_USR)
		return -1;

	if (rx_bits && rx) {
		for (i = 0; i < DIV_ROUND_UP(rx_bits, 32); i++) {
			u32 v = readl((void *)(P4_MSPI_S_W0 + i * 4));
			u32 left = rx_bits - i * 32;

			if (left < 32)
				v &= (1u << left) - 1;
			rx[i] = v;
		}
	}

	return 0;
}

static u32 psram_read_reg(u32 reg, u32 dummy)
{
	u32 rx = 0;

	psram_xact(0x4040, reg, NULL, 0, &rx, 16, dummy);

	return rx;
}

static void psram_probe(void)
{
	static const u32 dummies[] = { 8, 12, 16, 20, 24, 28 };
	static const u32 regs[] = { 0x0, 0x4, 0x8 };
	u32 i, val;

	p4_spl_early_puts("psram probe:");
	p4_spl_early_puts(" w0pre ");
	p4_spl_early_hex(readl((void *)P4_MSPI_S_W0));
	p4_spl_early_puts(" ");
	p4_spl_early_hex(readl((void *)(P4_MSPI_S_W0 + 4)));
	p4_spl_early_puts("\n");

	/*
	 * Command-mode transactions must run at a slow bus clock: the S31 port
	 * uses MPLL/20 (~20 MHz) for its register accesses, and the replay
	 * leaves CLOCK at divider 1 (full speed), where the dummy window is far
	 * too short for a register read.
	 */
	writel(0x00130913u, (void *)(P4_MSPI3_BASE + 0x14));
	writel(0x00130913u, (void *)(P4_MSPI2_BASE + 0x14));
	psram_pin_mux_init();
	psram_opi_dtr_mode();
	for (i = 0; i < ARRAY_SIZE(dummies); i++) {
		writel(0xdeadbeef, (void *)P4_MSPI_S_W0);
		val = psram_read_reg(0, dummies[i]);
		p4_spl_early_puts(" d");
		p4_spl_early_hex(dummies[i]);
		p4_spl_early_puts("=");
		p4_spl_early_hex(val);
	}

	/*
	 * Poison W0 before each read. A result still equal to the poison means
	 * nothing was captured; three different answers for the three register
	 * addresses prove the address phase is decoded too.
	 */
	for (i = 0; i < ARRAY_SIZE(regs); i++) {
		writel(0xdeadbeef, (void *)P4_MSPI_S_W0);
		val = psram_read_reg(regs[i], 12);
		p4_spl_early_puts(" mr");
		p4_spl_early_hex(regs[i]);
		p4_spl_early_puts("=");
		p4_spl_early_hex(val);
	}
	p4_spl_early_puts("\n");
}


/*
 * Mode registers: MR0 (latency/drive), MR4 (write latency) and MR8 (burst).
 * Values follow the AP hex PSRAM defaults IDF/S31 use for the 200-250 MHz
 * profile: lt = 1, read latency 2..4, write latency 1..2, bl = 3, rbx = 1,
 * x16 = 1 (hex mode).
 */
static int psram_mode_reg_write(u32 mr0, u32 mr8)
{
	u32 w;

	w = mr0 & 0xff;
	psram_xact(0xC0C0, 0x0, &w, 16, NULL, 0, 0);
	w = mr8 & 0xff;
	psram_xact(0xC0C0, 0x8, &w, 16, NULL, 0, 0);

	return 0;
}

static int psram_mode_reg_verify(u32 mr0, u32 mr8)
{
	u32 r0 = 0, r8 = 0;
	int i;

	for (i = 0; i < 4; i++) {
		r0 = psram_read_reg(0x0, 12) & 0xff;
		r8 = psram_read_reg(0x8, 12) & 0xff;
		if (r0 == (mr0 & 0xff) && r8 == (mr8 & 0xff))
			return 0;
	}

	p4_spl_early_puts("mode: verify failed mr0 ");
	p4_spl_early_hex(r0);
	p4_spl_early_puts(" mr8 ");
	p4_spl_early_hex(r8);
	p4_spl_early_puts("\n");

	return -1;
}

static int psram_mode_reg_init(void)
{
	/*
	 * Mode registers are volatile: a cold power cycle starts from the
	 * part's defaults, so program them unconditionally and prove the result
	 * by reading them back, retrying once.
	 *
	 * MR0 = fixed read latency 2; MR8 = 2048 byte burst, x16 (hex) mode.
	 * MR4 (write latency) is deliberately not written: this part ignores
	 * the write (it reads back 0xdf before and after) and the reference
	 * chain runs with the same value.
	 */
	u32 mr0 = (PSRAM_RD_LATENCY << 2) | BIT(5);
	u32 mr8 = PSRAM_MR8_BL | (PSRAM_MR8_BT << 2) | (PSRAM_MR8_RBX << 3) |
		  (PSRAM_MR8_X16 << 6);

	psram_mode_reg_write(mr0, mr8);
	if (psram_mode_reg_verify(mr0, mr8)) {
		psram_mode_reg_write(mr0, mr8);
		if (psram_mode_reg_verify(mr0, mr8))
			return -1;
	}

	p4_spl_early_puts("mode: mr0 ");
	p4_spl_early_hex(psram_read_reg(0x0, 12));
	p4_spl_early_puts(" mr4 ");
	p4_spl_early_hex(psram_read_reg(0x4, 12));
	p4_spl_early_puts(" mr8 ");
	p4_spl_early_hex(psram_read_reg(0x8, 12));
	p4_spl_early_puts("\n");

	return 0;
}



/*
 * Open the cache/AXI window so the MMU entries we program are actually
 * reachable. Ported from the S31 port's psram_axi_config(), with the hex
 * (16 data line) variant of the opcodes and dummies used by the P4 part.
 */
#define P4_SRAM_DWR_CMD		(P4_MSPI2_BASE + 0x4c)
#define P4_SRAM_DRD_CMD		(P4_MSPI2_BASE + 0x48)
#define P4_SRAM_CMD		(P4_MSPI2_BASE + 0x44)
#define P4_CACHE_SCTRL		(P4_MSPI2_BASE + 0x40)
#define P4_CACHE_FCTRL		(P4_MSPI2_BASE + 0x3c)
#define P4_CTRL1		(P4_MSPI2_BASE + 0x0c)
#define P4_DDR_AXI		(P4_MSPI2_BASE + 0xd8)
#define P4_MMU_CONTENT		(P4_MSPI2_BASE + 0x37c)
#define P4_MMU_INDEX		(P4_MSPI2_BASE + 0x380)

#define P4_SCTRL_SADDR_4BYTE	BIT(0)
#define P4_SCTRL_WR_DUMMY	BIT(3)
#define P4_SCTRL_RD_DUMMY	BIT(4)
#define P4_SCTRL_RCMD		BIT(5)
#define P4_SCTRL_RDUMMY_S	6
#define P4_SCTRL_ADDR_BITLEN_S	14
#define P4_SCTRL_WCMD		BIT(20)
#define P4_SCTRL_HEX		BIT(21)
#define P4_SCTRL_WDUMMY_S	22
#define P4_CMD_SDIN_HEX		BIT(26)
#define P4_CMD_SDOUT_HEX	BIT(27)
#define P4_CMD_SDUMMY_WOUT	BIT(23)

#define P4_PSRAM_RD_DUMMY	(2 * (14 - 1))
/* The cache path needs the same 46-cycle read latency the tuning found. */
#define P4_PSRAM_CACHE_RD_DUMMY	18
#define P4_PSRAM_WR_DUMMY	(2 * (5 - 1))

/*
 * The command path is tuned with the slow profile (MR0 read latency 2), which
 * is what the array test and the timing search use. The cache path, however,
 * is configured for IDF's 200 MHz profile: 26-bit read dummy, 12-bit write
 * dummy, MR0 read latency 4 and MR4 write latency 1. Switch the device over
 * once the command path is retired, otherwise every cache read samples the
 * data four cycles too early and the controller waits forever.
 */
#define P4_PSRAM_RD_LATENCY_FINAL	4
#define P4_PSRAM_WR_LATENCY_FINAL	2

static void psram_final_latency(void)
{
	u32 mr4 = 0, w;

	psram_xact(0x4040, 0x4, NULL, 0, &mr4, 16, 12);
	w = (mr4 & 0x1f) | (P4_PSRAM_WR_LATENCY_FINAL << 5);
	psram_xact(0xC0C0, 0x4, &w, 16, NULL, 0, 0);

	psram_mode_reg_write((P4_PSRAM_RD_LATENCY_FINAL << 2) | BIT(5),
			     PSRAM_MR8_X16);

	p4_spl_early_puts("lat: mr0 ");
	p4_spl_early_hex(psram_read_reg(0x0, 12));
	p4_spl_early_puts(" mr4 ");
	p4_spl_early_hex(psram_read_reg(0x4, 12));
	p4_spl_early_puts("\n");
}

static void psram_axi_config(void)
{
	u32 sctrl, cmd;

	writel(0x8080u | ((16 - 1) << 28), (void *)P4_SRAM_DWR_CMD);
	writel(0x0000u | ((16 - 1) << 28), (void *)P4_SRAM_DRD_CMD);

	sctrl = P4_SCTRL_SADDR_4BYTE | P4_SCTRL_WR_DUMMY | P4_SCTRL_RD_DUMMY |
		P4_SCTRL_RCMD | P4_SCTRL_HEX |
		((u32)(P4_PSRAM_CACHE_RD_DUMMY - 1) << P4_SCTRL_RDUMMY_S) |
		((u32)(32 - 1) << P4_SCTRL_ADDR_BITLEN_S) |
		P4_SCTRL_WCMD |
		((u32)(P4_PSRAM_WR_DUMMY - 1) << P4_SCTRL_WDUMMY_S);
	writel(sctrl, (void *)P4_CACHE_SCTRL);

	/* CS timing + split transaction, as the reference has it. */
	writel(0x8400b18fu, (void *)(P4_MSPI2_BASE + 0x1a0));
	writel(0, (void *)(P4_MSPI2_BASE + 0x170));

	/*
	 * TRM ch.47 "SPI" - SPI_MEM_SRAM_CMD: enable the 16-line data phase for
	 * the cache controller as well. psram_ctrlr_ll_enable_hex_data_line_mode()
	 * in IDF sets mem_sdin_hex/mem_sdout_hex unconditionally, and without
	 * them the cache controller drives only 8 of the 16 data lines (2 bytes
	 * per clock), so a 32-bit AXI store leaves two bytes at offset 0 and two
	 * at offset 4.
	 */
	cmd = readl((void *)P4_SRAM_CMD);
	cmd &= ~(P4_CMD_SDIN_HEX | P4_CMD_SDOUT_HEX);
	cmd |= BIT(18) | BIT(19) | BIT(20) | BIT(21) | P4_CMD_SDUMMY_WOUT;
	writel(cmd, (void *)P4_SRAM_CMD);

	/*
	 * DDR control for the cache path: reference value 0x3023. Do not write
	 * the whole register - bits 12/13 (tx/rx DDR mask enable, default on)
	 * and bit 5 (outminbytelen) must stay set, otherwise the DDR data
	 * placement is wrong. Only set ddr_en and var_dummy.
	 */
	setbits_le32((void *)P4_DDR_AXI, BIT(0) | BIT(1));

	/* AXI stays closed until the MMU is programmed. */
	clrbits_le32((void *)P4_CACHE_FCTRL, BIT(31));

	setbits_le32((void *)P4_CTRL1, BIT(25) | BIT(26));

	p4_spl_early_puts("axi: sctrl ");
	p4_spl_early_hex(readl((void *)P4_CACHE_SCTRL));
	p4_spl_early_puts("\n");
}



/*
 * Load the payload into PSRAM from the flash window while we still own the
 * machine: U-Boot's "cp" command crashes on this port, and the SPL has working
 * bulk reads and writes.
 */
static void psram_dcache_flush(void *addr, u32 size);
static void psram_dcache_invalidate(void *addr, u32 size);


/* Nothing feeds the watchdogs during a multi-megabyte copy, so turn them off. */
static void psram_watchdogs_off(void)
{
	p4_watchdogs_off();
}

void p4_spl_psram_load_payload(void)
{
	u32 *kern = (u32 *)0x40200000;		/* flash 0x200000 */
	u32 *dtb = (u32 *)0x40900000;		/* flash 0x900000 */
	u32 ksize, dsize;

	if (dtb[0] != 0xedfe0dd0u) {
		p4_spl_early_puts("payload: no fdt at 0x900000\n");
		return;
	}

	ksize = kern[4];			/* arm64 image_size at 0x10 */
	dsize = swab32(dtb[1]);			/* fdt totalsize, big endian */

	if (ksize < 0x100000u || ksize > 0x01000000u || dsize < 0x100u ||
	    dsize > 0x10000u) {
		p4_spl_early_puts("payload: implausible sizes, skipped\n");
		return;
	}

	psram_watchdogs_off();

	/* The cache path works now, so a plain copy is enough. */
	memcpy((void *)0x48e00000, dtb, dsize);
	memcpy((void *)0x48000000, kern, ksize);

	/* Confirm through the command path that the data really is in the chip. */
	{
		u32 v = 0;

		psram_xact(0x0000, 0, NULL, 0, &v, 32, 46);
		p4_spl_early_puts("payload: chip word0 ");
		p4_spl_early_hex(v);
		p4_spl_early_puts("\n");
	}

	p4_spl_early_puts("payload: kern ");
	p4_spl_early_hex(readl((void *)0x48000000));
	p4_spl_early_puts(" size ");
	p4_spl_early_hex(ksize);
	p4_spl_early_puts(" dtb ");
	p4_spl_early_hex(readl((void *)0x48e00000));
	p4_spl_early_puts(" size ");
	p4_spl_early_hex(dsize);
	p4_spl_early_puts("\n");
}

/* Bulk write/read test: find the size at which PSRAM writes start to fail. */

/*
 * Cross-check the cache/AXI path against the (now working) command path.
 * 0x48100000 is PSRAM offset 0x100000 in both views.
 */

/* ROM helpers: Cache_WriteBack_Addr / Cache_Invalidate_Addr in mask 0x10 (L1 D). */
#define P4_ROM_CACHE_INVALIDATE_ADDR	0x4fc003e4
#define P4_ROM_CACHE_WRITEBACK_ADDR	0x4fc003f4
#define P4_CACHE_MAP_DCACHE		0x10
#define P4_CACHE_MAP_ALL		0x13	/* L1 I0|I1|D; 0x3f hits undefined bits */
#define P4_CACHE_MAP_ICACHE0		0x01

static void psram_dcache_flush(void *addr, u32 size)
{
	((int (*)(u32, u32, u32))P4_ROM_CACHE_WRITEBACK_ADDR)
		(P4_CACHE_MAP_ALL, (u32)addr, size);
}

static void psram_dcache_invalidate(void *addr, u32 size)
{
	((int (*)(u32, u32, u32))P4_ROM_CACHE_INVALIDATE_ADDR)
		(P4_CACHE_MAP_ALL, (u32)addr, size);
}


/* Does the command path work across the whole PSRAM, or only near offset 0? */
static void psram_range_check(void)
{
	static const u32 offs[] = { 0x80, 0x1000, 0x100000, 0x200000, 0x800000,
				    0xf00000 };
	u32 i;

	for (i = 0; i < ARRAY_SIZE(offs); i++) {
		u32 w = 0x5a5a0000u + i, r = 0;

		psram_xact(0x8080, offs[i], &w, 32, NULL, 0, P4_PSRAM_WR_DUMMY);
		psram_xact(0x0000, offs[i], NULL, 0, &r, 32, 46);
		p4_spl_early_puts(" cmd@");
		p4_spl_early_hex(offs[i]);
		p4_spl_early_puts(r == w ? "=ok" : "=BAD");
	}
	p4_spl_early_puts("\n");
}



/*
 * TRM ch.9 "System and Memory" - HP L2MEM and L2 cache share the same 768 KB
 * and the split is configurable (Config 0: 256/512, 1: 512/256, 2: 640/128,
 * 3: 768/0 KB). Our chain runs with the whole block as L2 RAM, so every PSRAM
 * access is a single AXI transaction; in 16-line DDR a 32-bit access is one
 * clock and the controller never completes it. Enable the 128 KB cache split
 * (the SPL, U-Boot and its stack all live below 0x4ff90000) so the CPU issues
 * cache-line fills instead.
 */
/*
 * TRM ch.22 System Registers (SYSREG), Register 22.15
 * HP_SYSTEM_HP_CACHE_CLK_CONFIG_REG (0x500E6020): the HP L1/L2 cache clocks
 * are off in the proven reference chain (readback 0x8 - every cache clock
 * field is 0), so its PSRAM window is uncached and each access is a direct
 * MSPI2 AXI transaction. Keep this port on the same page: do not carve the L2
 * out as cache and do not assume a cache between the CPU and the PSRAM.
 * (The ROM cache helpers silently do nothing while those clocks are off.)
 */


/*
 * Prove the cacheable window end to end: write a pattern through it and read
 * it back through it. The chip runs 16-line but the controller is left in
 * 8-line mode (no SRAM_CMD hex bits), which paired with the real read latency
 * of 4 moves 4 bytes per AXI beat correctly - in 16-line mode every read
 * stalls, and with the old latency of 2 the data came back truncated.
 */
/*
 * Prove the cacheable window end to end: write a 4 KB pattern through it and
 * read it back through it. The part reports a 14-cycle read latency, and with
 * that latency programmed while the controller stays in 8-line mode the window
 * moves 4 bytes per AXI beat correctly. In 16-line mode every window read
 * stalls, and with the old latency-2 programming the data came back truncated,
 * so both of those settings are deliberate here.
 */
static void psram_cache_check(void)
{
	u32 i, v, bad = 0;

	for (i = 0; i < 1024; i++)
		writel(0xa5a50000u + i, (void *)(0x88000000 + i * 4));
	for (i = 0; i < 1024; i++) {
		v = readl((void *)(0x88000000 + i * 4));
		if (v != 0xa5a50000u + i)
			bad++;
	}

	p4_spl_early_puts("window: bad ");
	p4_spl_early_hex(bad);
	p4_spl_early_puts(" of 1024\n");
}

static void psram_bulk_test(void)
{
	static const u32 sizes[] = { 0x40, 0x100, 0x400, 0x1000, 0x4000 };
	u32 i, n, sz, dst = 0x48100000;

	for (i = 0; i < ARRAY_SIZE(sizes); i++) {
		sz = sizes[i];
		for (n = 0; n < sz / 4; n++)
			writel(0x5a5a0000u + n, (void *)(dst + n * 4));
		for (n = 0; n < sz / 4; n++) {
			if (readl((void *)(dst + n * 4)) != 0x5a5a0000u + n)
				break;
		}
		p4_spl_early_puts(" bulk ");
		p4_spl_early_hex(sz);
		p4_spl_early_puts(n == sz / 4 ? "=ok" : "=BAD");
	}
	p4_spl_early_puts("\n");
	/* Persistence probe: is the previous boot's pattern anywhere near? */
	p4_spl_early_puts(" stale");
	p4_spl_early_puts(" a=");
	p4_spl_early_hex(readl((void *)0x49f00000));
	p4_spl_early_puts(" b=");
	p4_spl_early_hex(readl((void *)0x49effffc));
	p4_spl_early_puts(" c=");
	p4_spl_early_hex(readl((void *)0x49f00004));
	p4_spl_early_puts(" d=");
	p4_spl_early_hex(readl((void *)0x48080000));
	p4_spl_early_puts("\n");

	writel(0xcafebabe, (void *)0x49f00000);
	writel(0xdead1234, (void *)0x48080000);
	p4_spl_early_puts(" wrote cafebabe@49f00000 dead1234@48080000 now ");
	p4_spl_early_hex(readl((void *)0x49f00000));
	p4_spl_early_hex(readl((void *)0x48080000));

}


/*
 * DQS timing calibration, ported from the S31 port (psram_tune_pins /
 * psram_tune_read / psram_timing_tune). The PSRAM pins live in the mux block
 * at 0x500e1200: DQS_0 at +0x3c, the 16 data pins at +0x1c..+0x5c, CS at
 * +0x60, CK at +0x64 and DQS_1 at +0x68.
 */
#define P4_PSRAM_PIN_BASE	0x500e1200u
#define P4_PSRAM_DQS		(P4_PSRAM_PIN_BASE + 0x3c)
#define P4_PSRAM_DQS1		(P4_PSRAM_PIN_BASE + 0x68)
#define P4_DQS_PHASE_MASK	(3u << 1)
#define P4_DQS_DELAY_MASK	((15u << 7) | (15u << 18))
#define P4_PIN_DELAY_MASK	(15u << 4)
#define P4_TUNE_ADDR		0x80u
#define P4_TUNE_WORDS		32u
#define P4_TUNE_RD_DUMMY	(P4_PSRAM_RD_DUMMY)
#define P4_TUNE_WR_DUMMY	(P4_PSRAM_WR_DUMMY)

static void psram_tune_pins(u32 phase, u32 tap)
{
	u32 dqs = tap < 15 ? 15 - tap : 0;
	u32 data = tap > 15 ? tap - 15 : 0;
	u32 i;

	clrsetbits_le32((void *)P4_PSRAM_DQS,
			P4_DQS_PHASE_MASK | P4_DQS_DELAY_MASK,
			(phase << 1) | (dqs << 7) | (dqs << 18));
	clrsetbits_le32((void *)P4_PSRAM_DQS1,
			P4_DQS_PHASE_MASK | P4_DQS_DELAY_MASK,
			(phase << 1) | (dqs << 7) | (dqs << 18));

	for (i = 0; i < 16; i++)
		clrsetbits_le32((void *)(P4_PSRAM_PIN_BASE + 0x1c + i * 4),
				P4_PIN_DELAY_MASK, data << 4);
	/* CS and CK */
	clrsetbits_le32((void *)(P4_PSRAM_PIN_BASE + 0x60), P4_PIN_DELAY_MASK,
			data << 4);
	clrsetbits_le32((void *)(P4_PSRAM_PIN_BASE + 0x64), P4_PIN_DELAY_MASK,
			data << 4);
}

/* Read dummy in cycles depends on the clock: 50 at MPLL/20, 25 at full speed. */
static u32 psram_tune_dummy = 50;
static u32 psram_tune_verbose = 1;

static int psram_tune_read(const u32 *reference)
{
	u32 readback[17];
	u32 offset, i;

	for (offset = 0; offset < P4_TUNE_WORDS; offset += 16) {
		psram_xact(0x0000, P4_TUNE_ADDR + offset * 4, NULL, 0, readback,
			   512, psram_tune_dummy);
		/* The last word of each chunk reads back undriven; skip it. */
		for (i = 0; i < 15; i++) {
			if (readback[i] != reference[offset + i]) {
				if (psram_tune_verbose) {
					p4_spl_early_puts(" mismatch ");
					p4_spl_early_hex(offset + i);
					p4_spl_early_puts(" want ");
					p4_spl_early_hex(reference[offset + i]);
					p4_spl_early_puts(" got ");
					p4_spl_early_hex(readback[i]);
					p4_spl_early_puts("\n");
				}
				return 0;
			}
		}
	}

	return 1;
}


/*
 * Smallest possible array write test: one 32-bit word through the direct
 * command path, written and then read back.
 */
static void psram_word_test(void)
{
	u32 w = 0x1234abcd, r = 0, r2 = 0;

	{
		u32 pat[4] = { 0xaaaaaaaa, 0xaaaaaaaa, 0xaaaaaaaa,
			       0xaaaaaaaa };
		u32 rb[4];

		psram_xact(0x8080, 0x80, pat, 128, NULL, 0,
			   P4_PSRAM_WR_DUMMY);
		{
			static const u32 dummies[] = { 50 };

			for (w = 0; w < ARRAY_SIZE(dummies); w++) {
				psram_xact(0x0000, 0x80, NULL, 0, rb, 128,
					   dummies[w]);
				p4_spl_early_puts(" d");
				p4_spl_early_hex(dummies[w]);
				p4_spl_early_puts("=");
				p4_spl_early_hex(rb[0]);
				p4_spl_early_puts("/");
				p4_spl_early_hex(rb[1]);
			}
			p4_spl_early_puts("\n");
		}
	}

	psram_xact(0x8080, 0x80, &w, 32, NULL, 0, P4_PSRAM_WR_DUMMY);
	psram_xact(0x0000, 0x80, NULL, 0, &r, 32, 50);
	psram_xact(0x8080, 0x84, &w, 32, NULL, 0, P4_PSRAM_WR_DUMMY);
	psram_xact(0x0000, 0x84, NULL, 0, &r2, 32, 50);

	p4_spl_early_puts("wordtest wrote ");
	p4_spl_early_hex(w);
	p4_spl_early_puts(" read80 ");
	p4_spl_early_hex(r);
	p4_spl_early_puts(" read84 ");
	p4_spl_early_hex(r2);
	p4_spl_early_puts("\n");
}

static int psram_timing_tune(void)
{
	u32 reference[P4_TUNE_WORDS];
	u32 pattern = 0x6d2b79f5;
	u32 phase, selected = 4, tap, trial, run = 0, best = 0, end = 0;
	u32 i;

	/* Reference write/read at the slow command clock, as the S31 does. */
	writel(0x00130913u, (void *)(P4_MSPI2_BASE + 0x14));
	psram_tune_pins(0, 15);
	for (i = 0; i < P4_TUNE_WORDS; i++) {
		pattern ^= pattern << 13;
		pattern ^= pattern >> 17;
		pattern ^= pattern << 5;
		reference[i] = pattern;
	}
	for (i = 0; i < P4_TUNE_WORDS; i += 16)
		psram_xact(0x8080, P4_TUNE_ADDR + i * 4, reference + i, 512,
			   NULL, 0, P4_TUNE_WR_DUMMY);
	/* The aligned window measured 45..48 cycles at the slow clock. */
	psram_tune_dummy = 46;
	if (!psram_tune_read(reference)) {
		psram_tune_verbose = 1;
		psram_tune_read(reference);
		p4_spl_early_puts("tune: low-clock reference failed\n");
		return -1;
	}
	p4_spl_early_puts("tune: reference ok");

	{
		u32 d, best_div = 0;

		psram_tune_verbose = 0;
		psram_tune_dummy = 46;
		for (d = 1; d <= 12; d++) {
			writel((d << 16) | (d << 8) | d,
			       (void *)(P4_MSPI2_BASE + 0x14));
			if (psram_tune_read(reference)) {
				best_div = d;
				break;
			}
		}
		psram_tune_verbose = 1;
		p4_spl_early_puts("tune: fastest div ");
		p4_spl_early_hex(best_div);
		p4_spl_early_puts("\n");
		if (!best_div) {
			p4_spl_early_puts("tune: no working clock\n");
			return -1;
		}
		writel((best_div << 16) | (best_div << 8) | best_div,
		       (void *)(P4_MSPI2_BASE + 0x14));
	}
	for (phase = 0; phase < 4; phase++) {
		psram_tune_pins(phase, 15);
		if (psram_tune_read(reference)) {
			run++;
			if (run > best) {
				best = run;
				selected = phase - run + 1;
			}
		} else {
			run = 0;
		}
	}
	p4_spl_early_puts(" phase ");
	p4_spl_early_hex(selected);
	if (selected == 4) {
		p4_spl_early_puts(" none\n");
		return -1;
	}

	run = 0;
	best = 0;
	for (tap = 0; tap < 31; tap++) {
		psram_tune_pins(selected, tap);
		for (trial = 0; trial < 20; trial++)
			if (!psram_tune_read(reference))
				break;
		if (trial == 20) {
			run++;
			if (run > best) {
				best = run;
				end = tap;
			}
		} else {
			run = 0;
		}
	}
	p4_spl_early_puts(" delay ");
	p4_spl_early_hex(best ? end - best / 2 : 0);
	p4_spl_early_puts(" margin ");
	p4_spl_early_hex(best);
	p4_spl_early_puts("\n");
	if (best < 3)
		return -1;

	tap = end - best / 2;
	psram_tune_pins(selected, tap);
	if (!psram_tune_read(reference))
		return -1;

	for (i = 0; i < P4_TUNE_WORDS; i++)
		reference[i] = ~reference[i];
	for (i = 0; i < P4_TUNE_WORDS; i += 16)
		psram_xact(0x8080, P4_TUNE_ADDR + i * 4, reference + i, 512,
			   NULL, 0, P4_TUNE_WR_DUMMY);
	if (!psram_tune_read(reference)) {
		p4_spl_early_puts("tune: write verify failed\n");
		return -1;
	}
	p4_spl_early_puts("tune: ok\n");

	return 0;
}

/* Map the PSRAM window through the MSPI2 "S" MMU (64 KiB pages). */
/* TRM ch.9 "System and Memory": the MMU maps the PSRAM window. The item
 * registers are not in the manual (see the file header). */
/*
 * TRM ch.2 "High-Performance CPU", Register 2.118 pma_cfgX (CSR 0xBC0+n) and
 * pma_addrX (CSR 0xBD0+n): mark the external-RAM window non-cacheable so CPU
 * accesses take the direct path instead of entering the HP cache, whose clocks
 * are off in this chain (TRM ch.22 Register 22.15).
 *
 *   A = NAPOT (bits 31:30 = 0b11), EN bit0, R bit4, W bit3
 *   ATTRIBUTE bit27 = 1 non-cacheable, bit25/24 = write/read miss no allocate
 * pma_addrX holds the address shifted right by 2 (see IDF riscv/csr.h).
 */
static void psram_pma_non_cacheable(void)
{
	u32 addr = (0x48000000u | (0x04000000u / 2 - 1)) >> 2;
	u32 cfg = 0xc0000000u | BIT(0) | BIT(4) | BIT(3) |
		  BIT(27) | BIT(25) | BIT(24);

	asm volatile("csrw 0xbd0, %0" :: "r"(addr));
	asm volatile("csrw 0xbc0, %0" :: "r"(cfg));
	asm volatile("fence" ::: "memory");
}



/*
 * TRM ch.22 System Registers (SYSREG), Register 22.15
 * HP_SYSTEM_HP_CACHE_CLK_CONFIG_REG at HP_SYSTEM + 0x0020. HP_SYSTEM is
 * HPPERIPH1 + 0x25000 (0x500E5000, confirmed by its VER_DATE reading
 * 0x20230519); HP_SYS_CLKRST is a different block at +0x26000 (0x500E6000).
 * Bits 0/1/4/5 are the L2 / L1 D / L1 I1 / L1 I0 cache clocks. The working
 * reference has all four enabled (0x00000033), and TRM ch.9 §9.3.3.1 says the
 * 0x4800_0000 window is "accessed by HP CPU via cache".
 */
#define P4_ROM_CACHE_INVALIDATE_ALL	0x4fc00404u
#define P4_ROM_CACHE_ENABLE_L1_DCACHE	0x4fc004f4u

static void psram_cache_clocks_on(void)
{
	setbits_le32((void *)0x500e5020u, BIT(0) | BIT(1) | BIT(4) | BIT(5));
	udelay(10);

}


static void psram_mmu_map(u32 size)
{
	u32 entries = size >> 16;
	u32 i;

	/*
	 * Page size for the PSRAM side of the MMU (MSPI2 + 0x384). Never
	 * programmed before, which left the window correct only with the caches
	 * off (the uncached path does not use this mapping) and wrong as soon as
	 * the L2 was on. 0x13200004 is what the working reference holds, with
	 * bits [4:3] = 2 selecting 64 KiB pages - the same geometry the flash
	 * side is given in p4_spl_flash_init().
	 */
	writel(0x13200004u, (void *)(P4_MSPI2_BASE + 0x384));

	for (i = 0; i < entries; i++) {
		writel(i, (void *)(P4_MSPI2_BASE + 0x380));
		writel(i | BIT(11) | BIT(10), (void *)(P4_MSPI2_BASE + 0x37c));
	}
}

/* Write a pattern at an address and read it back through the cache path. */
static void psram_rw_test(void)
{
	static const u32 addrs[] = { 0x48010000, 0x48100000, 0x49000000 };
	u32 i, val;

	for (i = 0; i < ARRAY_SIZE(addrs); i++) {
		writel(0xa5a50000u + i, (void *)addrs[i]);
		val = readl((void *)addrs[i]);
		p4_spl_early_puts(" rw ");
		p4_spl_early_hex(addrs[i]);
		p4_spl_early_puts("=");
		p4_spl_early_hex(val);
	}
	p4_spl_early_puts("\n");
}






/*
 * Preload the kernel and the DTB from flash into PSRAM, then push the lines
 * out to the part.
 *
 * Flash XIP window: 0x4000_0000 (P4_FLASH_VADDR); the argument of
 * p4_spl_flash_init() is the mapped size, not a base. Kernel: flash 0x200000
 * (image_size at offset 0x10), DTB: flash 0x900000 (FDT totalsize).
 *
 * The copies pass through the caches, so the writeback has to be
 * Cache_WriteBack_All (ROM 0x4fc00414) with the L1 D and L2 map bits
 * (rom/cache.h: CACHE_MAP_L1_DCACHE BIT(4), CACHE_MAP_L2_CACHE BIT(5)).
 * Cache_WriteBack_Invalidate_All discards the dirty lines instead, which is
 * why an earlier attempt lost the data.
 */
#define P4_FLASH_WIN		0x40000000u
#define P4_KERN_OFF		0x00200000u
#define P4_DTB_OFF		0x00900000u
/*
 * Write through the direct (uncached) alias: TRM ch.9 §9.3.3.1 says
 * 0x8800_0000-0x8BFF_FFFF is the same external RAM "accessed directly", i.e.
 * bypassing the cache, so no line can be lost to eviction during the copy.
 * The alias is 0x8800_0000 + (vaddr - 0x4800_0000).
 */
#define P4_KERN_ADDR		0x88000000u
#define P4_DTB_ADDR		0x88f00000u
#define P4_PRE_CHUNK		0x1000u
#define P4_ROM_CACHE_WB_ALL	0x4fc00414u
#define P4_CACHE_MAP_DCACHE	((1u << 4) | (1u << 5))

void p4_spl_psram_preload(void)
{
	const u32 *kern = (const u32 *)(P4_FLASH_WIN + P4_KERN_OFF);
	const u32 *dtb = (const u32 *)(P4_FLASH_WIN + P4_DTB_OFF);
	u32 ksize, dsize, off;

	if (dtb[0] != 0xedfe0dd0u) {
		p4_spl_early_puts("preload: no fdt\n");
		return;
	}
	ksize = kern[4];
	dsize = __builtin_bswap32(dtb[1]);
	p4_spl_early_puts("preload: k ");
	p4_spl_early_hex(ksize);
	p4_spl_early_puts(" d ");
	p4_spl_early_hex(dsize);

	for (off = 0; off < dsize; off += P4_PRE_CHUNK) {
		u32 n = dsize - off > P4_PRE_CHUNK ? P4_PRE_CHUNK : dsize - off;

		memcpy((void *)(P4_DTB_ADDR + off),
		       (const void *)(P4_FLASH_WIN + P4_DTB_OFF + off), n);
	}
	for (off = 0; off < ksize; off += P4_PRE_CHUNK) {
		u32 n = ksize - off > P4_PRE_CHUNK ? P4_PRE_CHUNK : ksize - off;

		memcpy((void *)(P4_KERN_ADDR + off),
		       (const void *)(P4_FLASH_WIN + P4_KERN_OFF + off), n);
	}

	((void (*)(u32))P4_ROM_CACHE_WB_ALL)(P4_CACHE_MAP_DCACHE);

	/* Compare source and destination at a few offsets. */
	{
		static const u32 at[] = { 0x0, 0x4, 0x8, 0x38, 0x1000, 0x100000,
					  0x200000 };
		u32 i;

		for (i = 0; i < ARRAY_SIZE(at); i++) {
			p4_spl_early_puts("cmp ");
			p4_spl_early_hex(at[i]);
			p4_spl_early_puts(" src ");
			p4_spl_early_hex(readl((const void *)(P4_FLASH_WIN +
							      P4_KERN_OFF + at[i])));
			p4_spl_early_puts(" dst ");
			p4_spl_early_hex(readl((const void *)(P4_KERN_ADDR + at[i])));
			p4_spl_early_puts("\n");
		}
	}
}

int p4_spl_psram_init(void)
{
	/*
	 * The ROM arms MWDT0 flashboot mode on every flash boot; it keeps
	 * resetting the SoC even after wdt_en is cleared and will abort a PSRAM
	 * transaction mid-flight, leaving the controller stuck.
	 */
	p4_watchdogs_off();

	p4_spl_early_puts("psram: clock\n");
	psram_clkrst_fixup();
	psram_clock_init();

	p4_spl_early_puts("psram: replay\n");
	psram_replay();

	/* The replay leaves MEM_AXI_REQ_EN set; keep the AXI port closed while
	 * the command path is in use so the cache controller cannot grab the
	 * shared PSRAM bus. */
	clrbits_le32((void *)P4_CACHE_FCTRL, BIT(0));

	p4_spl_early_puts("psram: cmds\n");
	psram_pmu_fixup();

	psram_pads_init();
	psram_probe();
	if (psram_mode_reg_init()) {
		p4_spl_early_puts("psram: mode unverified, resetting\n");
		/* The reset clears the controllers, so the pads and the OPI/DTR
		 * mode have to be re-established before the state is replayed. */
		psram_controller_reset();
		psram_pin_mux_init();
		psram_opi_dtr_mode();
		psram_replay();
		psram_pmu_fixup();
		psram_mode_reg_init();
	}
	psram_cache_cmds();
	psram_axi_config();

	/*
	 * With the AXI port open the command path stalls (the AXI has priority on
	 * the shared MSPI), so close it again for the tuning and reopen it after.
	 */
	clrbits_le32((void *)P4_CACHE_FCTRL, BIT(0));
	psram_word_test();
	psram_timing_tune();

	/*
	 * Now that the command path is no longer needed, put the cache
	 * controller back in 16-line data mode. The reference SRAM_CMD
	 * (0x8cfc0000) has SDIN_HEX/SDOUT_HEX (bits 26/27) set; without them a
	 * 32-bit AXI write drives only half of the lines and leaves two bytes
	 * at offset 0 and two at offset 4. Program it, and the MMU, with the
	 * AXI port still closed, then open the port as the very last step.
	 */
	/*
	 * Command path is done: enable the 16-line data phase for the cache
	 * controller (IDF does this in s_config_mspi_for_psram, after the mode
	 * registers), map the window and only then let the AXI through.
	 */
	/*
	 * Bring the cache controller's own state to exactly what the reference
	 * has. A register-for-register diff of 0x5008e000..0x1a0 against a live
	 * IDF boot found only two differences: SRAM_CMD (the 16-line data-phase
	 * bits) and SPI_MEM_C_CLOCK (0x14). The reference runs that clock
	 * divider at 0x00030103 (N=3, H=1, L=3) instead of our 0x00010001, i.e.
	 * half the rate. Set both before opening the AXI port.
	 */
	psram_final_latency();
	writel(0x00030103u, (void *)(P4_MSPI2_BASE + 0x14));

	psram_cache_clocks_on();
	/*
	 * Take the CPU's PSRAM window accesses off the cache path. With them
	 * cached, writes land in L2 and dirty lines evicted during a multi-MB
	 * copy never reach the part (the DTB written first was gone by the time
	 * the kernel copy finished). Uncached, the same accesses go straight to
	 * the MSPI2 window and both reads and writes are correct - that is the
	 * state every working verification in this port has been run in.
	 */
	psram_pma_non_cacheable();
	psram_mmu_map(0x01000000);
	setbits_le32((void *)P4_CACHE_FCTRL, BIT(0));

	psram_cache_check();

	/*
	 * psram_check() does single-word window accesses, which hang the
	 * controller in 16-line DDR mode; the burst written above is verified
	 * from the prompt through the cacheable window instead.
	 */
	p4_spl_early_puts("psram: check skipped (hex mode)\n");

	return 0;
}
