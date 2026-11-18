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
	 * Hex PSRAM in DDR mode moves 4 bytes per clock (16 lines x 2 edges);
	 * the 16-bit register transfers use a single line pair, which is
	 * DBYTELEN 1. Pick the width from the data phase length.
	 */
	psram_dbyte = (rx_bits > 32 || tx_bits > 32) ? 3 : 1;

	user = ((32 - 1) << P4_USR_ADDR_BITLEN_S);
	if (dummy)
		user |= (dummy - 1) & 0x3f;
	writel(user | (psram_dbyte << 6), (void *)P4_MSPI_S_USER1);

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

	if (rx_bits && rx)
		for (i = 0; i < DIV_ROUND_UP(rx_bits, 32); i++)
			rx[i] = readl((void *)(P4_MSPI_S_W0 + i * 4));

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
	u32 i, val;

	p4_spl_early_puts("psram probe:");
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
		val = psram_read_reg(0, dummies[i]);
		p4_spl_early_puts(" d");
		p4_spl_early_hex(dummies[i]);
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
static void psram_mode_reg_init(void)
{
	u32 mr01 = 0, mr48 = 0, mr8 = 0;
	u32 w;

	psram_xact(0x4040, 0x0, NULL, 0, &mr01, 16, 12);
	psram_xact(0x4040, 0x4, NULL, 0, &mr48, 16, 12);
	psram_xact(0x4040, 0x8, NULL, 0, &mr8, 8, 12);

	mr01 &= ~(0x3u | (0x7u << 2) | BIT(5));
	mr01 |= (PSRAM_RD_LATENCY << 2) | BIT(5);
	mr48 &= ~(0x7u << 5);
	mr48 |= (PSRAM_WR_LATENCY << 5);
	mr8 &= ~(0x3u | BIT(2) | BIT(3) | BIT(6));
	mr8 |= 0x3u | BIT(3) | BIT(6);		/* bl = 3, rbx = 1, x16 = 1 */

	w = mr01 & 0xffff;
	psram_xact(0xC0C0, 0x0, &w, 16, NULL, 0, 0);
	w = mr48 & 0xffff;
	psram_xact(0xC0C0, 0x4, &w, 16, NULL, 0, 0);
	w = mr8 & 0xff;
	psram_xact(0xC0C0, 0x8, &w, 16, NULL, 0, 0);

	p4_spl_early_puts("mode: mr0 ");
	p4_spl_early_hex(mr01);
	p4_spl_early_puts(" mr4 ");
	p4_spl_early_hex(mr48);
	p4_spl_early_puts(" mr8 ");
	p4_spl_early_hex(mr8);
	p4_spl_early_puts("\n");
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
#define P4_PSRAM_CACHE_RD_DUMMY	49
#define P4_PSRAM_WR_DUMMY	(2 * (7 - 1))

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

	cmd = readl((void *)P4_SRAM_CMD);
	cmd &= ~(P4_CMD_SDIN_HEX | P4_CMD_SDOUT_HEX);
	cmd |= BIT(18) | BIT(19) | BIT(20) | BIT(21) | P4_CMD_SDUMMY_WOUT;
	writel(cmd, (void *)P4_SRAM_CMD);

	writel(BIT(0) | BIT(1), (void *)P4_DDR_AXI);

	setbits_le32((void *)P4_CACHE_FCTRL, BIT(0));
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
		p4_spl_early_puts("ckA");
	{
		u32 i2;

		for (i2 = 0; i2 < 40; i2++) {
			p4_spl_early_puts(" ");
			p4_spl_early_hex(readl((void *)(P4_CLKRST_BASE + i2 * 4)));
		}
		p4_spl_early_puts("\n");
	}

	p4_spl_early_puts(" cmd@");
		p4_spl_early_hex(offs[i]);
		p4_spl_early_puts(r == w ? "=ok" : "=BAD");
	}
	p4_spl_early_puts("\n");
}



static void psram_cache_check(void)
{
	u32 r1 = 0, r2 = 0;

	writel(0x11223344, (void *)0x48100000);
	psram_dcache_flush((void *)0x48100000, 0x1000);
	psram_xact(0x0000, 0x100000, NULL, 0, &r1, 32, 46);

	/* command write -> invalidate -> is it visible through the cache? */
	r2 = 0x55667788;
	psram_xact(0x8080, 0x100004, &r2, 32, NULL, 0, P4_PSRAM_WR_DUMMY);
	psram_dcache_invalidate((void *)0x48100000, 0x1000);
	r2 = readl((void *)0x48100004);

	p4_spl_early_puts("cache: write->chip ");
	p4_spl_early_hex(r1);
	p4_spl_early_puts(" chip->read ");
	p4_spl_early_hex(r2);
	p4_spl_early_puts("\n");
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
static void psram_mmu_map(u32 size)
{
	u32 entries = size >> 16;
	u32 i;

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
	psram_mode_reg_init();
	psram_cache_cmds();
	psram_axi_config();
	psram_word_test();
	psram_timing_tune();
	psram_mmu_map(0x01000000);

	psram_rw_test();
	psram_range_check();
	psram_cache_check();
	psram_bulk_test();
	psram_mmu_report();

	p4_spl_early_puts("psram: check\n");
	if (psram_check())
		return -1;

	return 0;
}
