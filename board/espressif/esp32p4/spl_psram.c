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
#define P4_MPLL_FB_DIV		9	/* bits [7:3]: (9+1)/(0+1) = 10x */

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

/* AP hex PSRAM commands and timings. */
#define PSRAM_REG_READ		0x4040
#define PSRAM_REG_WRITE		0xc0c0
#define PSRAM_REG_CMD_BITS	16
#define PSRAM_REG_ADDR_BITS	32
#define PSRAM_REG_DUMMY_RD	(2 * (5 - 1))
#define PSRAM_RD_LATENCY	2
#define PSRAM_WR_LATENCY	2
#define PSRAM_MR8_BL		3
#define PSRAM_MR8_BT		0
#define PSRAM_MR8_RBX		1
#define PSRAM_MR8_X16		1

void p4_spl_early_puts(const char *s);
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

	/* MPLL 400 MHz = 40 MHz * (9 + 1) / (0 + 1). */
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
	p4_spl_early_puts("\n");
}


/*
 * Local user-mode transaction on MSPI3, needed because the ROM's
 * esp_rom_spi_cmd_start never returns when driven from the SPL.
 *
 * Register map (0x5008f000, hw_ver3 spi_mem_s_reg.h):
 *   0x00 CMD    bit18 USR (start, clears when done)
 *   0x04 ADDR
 *   0x08 CTRL   bit4 DQ_OE, bit5 CK_OE, bit6 CS_OE, bit9 CK_IDLE_EDGE
 *   0x18 USER   bit27 MOSI, bit28 MISO, bit29 DUMMY, bit30 ADDR, bit31 CMD
 *   0x1c USER1  [5:0] DUMMY_CYCLELEN, [8:6] DBYTELEN, [31:26] ADDR_BITLEN
 *   0x20 USER2  [15:0] COMMAND_VALUE, [31:28] COMMAND_BITLEN
 *   0x24 MOSI_DLEN, 0x28 MISO_DLEN, 0x40.. W0..W15 (2 bytes per DDR cycle)
 */
static int psram_txn_local(u16 cmd, u16 cmd_bits, u32 addr, u32 addr_bits,
			   u32 dummy_bits, u32 *tx, u32 tx_bits,
			   u32 *rx, u32 rx_bits)
{
	u32 user = 0, user1, user2;
	int i;

	if (cmd_bits)
		user |= BIT(31);
	if (addr_bits)
		user |= BIT(30);
	if (dummy_bits) {
		user |= BIT(29);
		user |= BIT(26);			/* DUMMY_IDLE */
	}
	if (tx_bits) {
		user |= BIT(27);
		writel(tx_bits - 1, (void *)(P4_MSPI3_BASE + 0x24));
	}
	if (rx_bits) {
		user |= BIT(28);
		writel(rx_bits - 1, (void *)(P4_MSPI3_BASE + 0x28));
	}

	writel(addr, (void *)(P4_MSPI3_BASE + 0x04));

	user1 = ((addr_bits ? addr_bits - 1 : 0) << 26) | (1u << 6) |
		(dummy_bits / 2);
	writel(user1, (void *)(P4_MSPI3_BASE + 0x1c));

	user2 = ((cmd_bits ? cmd_bits - 1 : 0) << 28) | cmd;
	writel(user2, (void *)(P4_MSPI3_BASE + 0x20));

	if (tx && tx_bits)
		memcpy((void *)(P4_MSPI3_BASE + 0x40), tx, (tx_bits + 7) / 8);

	writel(user, (void *)(P4_MSPI3_BASE + 0x18));
	writel(BIT(18), (void *)(P4_MSPI3_BASE + 0x00));

	for (i = 0; i < 100000; i++) {
		if (!(readl((void *)(P4_MSPI3_BASE + 0x00)) & BIT(18)))
			break;
	}
	if (readl((void *)(P4_MSPI3_BASE + 0x00)) & BIT(18))
		return -1;

	if (rx && rx_bits)
		*rx = readl((void *)(P4_MSPI3_BASE + 0x40));

	return 0;
}

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

static void psram_pads_init(void)
{
	u32 i;

	for (i = 0; i < ARRAY_SIZE(psram_pad_state); i++)
		writel(psram_pad_state[i][1], (void *)psram_pad_state[i][0]);
}

/* Hex PSRAM register read: cmd 0x4040, 32 bit address, 16 bit result. */
static int psram_read_reg(u32 reg, u32 dummy, u32 *out)
{
	return psram_txn_local(0x4040, 16, reg, 32, dummy, NULL, 0, out, 16);
}

static void psram_probe(void)
{
	static const u32 dummies[] = { 4, 8, 14, 20, 26, 32, 40, 50, 60 };
	u32 i, val = 0;
	int ret;

	p4_spl_early_puts("psram probe:");
	for (i = 0; i < ARRAY_SIZE(dummies); i++) {
		val = 0;
		ret = psram_read_reg(0, dummies[i], &val);
		p4_spl_early_puts(" d");
		p4_spl_early_hex(dummies[i]);
		p4_spl_early_puts(ret ? "=ERR " : "=");
		p4_spl_early_hex(val);
	}
	p4_spl_early_puts("\n");
}

int p4_spl_psram_init(void)
{
	p4_spl_early_puts("psram: clock\n");
	psram_clkrst_fixup();
	psram_clock_init();

	p4_spl_early_puts("psram: replay\n");
	psram_replay();

	p4_spl_early_puts("psram: cmds\n");
	psram_pmu_fixup();
	psram_pads_init();
	psram_probe();
	psram_cache_cmds();
	psram_mmu_report();

	p4_spl_early_puts("psram: check\n");
	if (psram_check())
		return -1;

	return 0;
}
