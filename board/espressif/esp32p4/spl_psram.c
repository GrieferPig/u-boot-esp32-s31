// SPDX-License-Identifier: GPL-2.0+
/*
 * ESP32-P4 PSRAM bring-up for the SPL.
 *
 * The board carries a 32 MB in-package AP hex (16-line) PSRAM on MSPI2/MSPI3,
 * clocked from MPLL at 200 MHz. Bring-up order follows the vendor sequence:
 *
 *   1. clock tree: MPLL -> MSPI2/MSPI3 module clocks, 2:1 divider
 *   2. controller configuration (DDR, hex data lines, AXI access, splicing,
 *      CS timing, 2048-byte page, DLL, tuned input/output delays)
 *   3. device mode registers MR0/MR4/MR8 through the mask ROM's OPI helpers,
 *      which program the user-command path for us
 *   4. a write/read pattern check so the caller can stop on failure
 *
 * The tuned delay values in step 2 are the ones a working ESP-IDF boot leaves
 * behind on this board (captured with the SPL register dump).
 */

#include <common.h>
#include <asm/io.h>
#include <linux/bitops.h>
#include <linux/delay.h>

#include "spl_psram.h"

#define P4_CLKRST_BASE		0x500e6000ul
#define P4_MSPI2_BASE		0x5008e000ul
#define P4_MSPI3_BASE		0x5008f000ul
#define P4_PSRAM_BASE		0x48000000ul
#define P4_PSRAM_TEST_SIZE	0x00001000ul

/* HP_SYS_CLKRST reset gate for the dual MSPI (AXI + APB). */
#define P4_RST_EN0		0xc0
#define P4_RST_DUAL_MSPI_AXI	BIT(23)
#define P4_RST_DUAL_MSPI_APB	BIT(25)

/* Mask ROM OPI helpers (fixed addresses for this ROM revision). */
#define P4_ROM_SPI_CMD_CONFIG	0x4fc00108ul
#define P4_ROM_SPI_CMD_START	0x4fc0010cU
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

	set_op_mode(3, P4_ROM_OPI_DTR_MODE);
	cmd_config(3, &conf);
	cmd_start(3, (u8 *)rx, rx_bits / 8, 1 << 1, false);

	return 0;
}

static void psram_clock_init(void)
{
	/* MPLL -> 400 MHz, MSPI2/MSPI3 2:1 so both run at 200 MHz. */
	writel(0xe6df97ff, (void *)(P4_CLKRST_BASE + 0x14));
	writel(0x0e040004, (void *)(P4_CLKRST_BASE + 0x18));
	writel(0x20a000be, (void *)(P4_CLKRST_BASE + 0x1c));
	writel(0x40170503, (void *)(P4_CLKRST_BASE + 0x28));
	writel(0x00000000, (void *)(P4_CLKRST_BASE + 0x2c));
	writel(0x0000d03d, (void *)(P4_CLKRST_BASE + 0x30));

	/* Pulse the dual-MSPI reset so its APB/AXI interface comes out of reset. */
	setbits_le32((void *)(P4_CLKRST_BASE + P4_RST_EN0),
		     P4_RST_DUAL_MSPI_AXI | P4_RST_DUAL_MSPI_APB);
	udelay(20);
	clrbits_le32((void *)(P4_CLKRST_BASE + P4_RST_EN0),
		     P4_RST_DUAL_MSPI_AXI | P4_RST_DUAL_MSPI_APB);
	udelay(200);
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

static int psram_mode_register_init(void)
{
	u16 mr01 = 0;
	u16 mr48 = 0;
	u8 mr8 = 0;

	/* MR0/MR1 pair, then MR4/MR8 pair. */
	if (psram_transaction(PSRAM_REG_READ, 0x0, PSRAM_REG_DUMMY_RD, NULL, 0,
			      &mr01, 16))
		return -1;
	if (psram_transaction(PSRAM_REG_READ, 0x4, PSRAM_REG_DUMMY_RD, NULL, 0,
			      &mr48, 16))
		return -1;

	/* MR0: drive strength 0, read latency, latency type 1. */
	mr01 &= ~(0x3 | (0x7 << 2) | BIT(5));
	mr01 |= (PSRAM_RD_LATENCY << 2) | BIT(5);
	/* MR4: write latency in bits [7:5]. */
	mr48 &= ~(0x7 << 5);
	mr48 |= (PSRAM_WR_LATENCY << 5);

	if (psram_transaction(PSRAM_REG_WRITE, 0x0, 0, &mr01, 16, NULL, 0))
		return -1;
	if (psram_transaction(PSRAM_REG_WRITE, 0x4, 0, &mr48, 16, NULL, 0))
		return -1;

	/* MR8: burst length, type, x16. */
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
	volatile u32 *base = (volatile u32 *)P4_PSRAM_BASE;
	u32 value;
	int i;

	writel(0x5a6b7c8d, (void *)base);
	writel(0xa5a55a5a, (void *)(P4_PSRAM_BASE + 4));
	writel(0x12345678, (void *)(P4_PSRAM_BASE + P4_PSRAM_TEST_SIZE - 4));

	value = readl((void *)base);
	if (value != 0x5a6b7c8d)
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

void p4_spl_early_puts(const char *s);

int p4_spl_psram_init(void)
{
	int ret;

	/*
	 * TODO: program MPLL (400 MHz) and switch the PSRAM source to it. Until
	 * that sequence is ported, stay on the ROM clock state.
	 */
	p4_spl_early_puts("psram: clock (rom state)\n");
	psram_controller_init();

	/*
	 * The ROM OPI transaction path needs MPLL plus the MSPI2/MSPI3 CS and
	 * clock programming first; until that sequence is ported, fail cleanly
	 * instead of hanging inside the ROM helper.
	 */
#ifndef P4_PSRAM_BRINGUP_READY
	p4_spl_early_puts("psram: not ported yet\n");
	return -1;
#endif

	p4_spl_early_puts("psram: mode-regs\n");
	ret = psram_mode_register_init();
	if (ret)
		return ret;

	/* The ROM helpers reconfigure MSPI3 for the transactions above. */
	p4_spl_early_puts("psram: reconfig\n");
	psram_controller_init();

	p4_spl_early_puts("psram: check\n");
	return psram_check();
}
