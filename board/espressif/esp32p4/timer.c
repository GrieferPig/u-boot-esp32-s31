// SPDX-License-Identifier: GPL-2.0+
/*
 * ESP32-P4 timebase: the P4 has no CLINT, so both SPL and U-Boot proper read
 * the system timer instead. The counter must be latched: write
 * UPDATE|VALUE_VALID to UNIT0_OP, wait for VALID, then read the halves.
 *
 * The timer's APB clock gate lives in HP_SYS_CLKRST at SOC_CLK_CTRL2 (offset
 * 0x1c): bit 23 gates the APB clock, bit 29 selects the 16 MHz RC_FAST source
 * and bit 30 enables it.
 */

#include <common.h>
#include <asm/io.h>
#include <linux/bitops.h>

#define P4_SYSTIMER_BASE	0x500e2000ul
#define P4_HP_SYS_CLKRST	0x500e6000ul
#define P4_SOC_CLK_CTRL2	0x1c
#define P4_APB_ENABLE		BIT(23)
#define P4_CLK_SOURCE_RC_FAST	BIT(29)
#define P4_CLK_ENABLE		BIT(30)

#define P4_UNIT0_OP		0x04
#define P4_UPDATE		BIT(30)
#define P4_VALUE_VALID		BIT(29)
#define P4_VALUE_LO		0x44

#define P4_TIMER_RATE		16000000

void p4_systimer_enable(void)
{
	void __iomem *clkrst = (void __iomem *)P4_HP_SYS_CLKRST;
	u32 val = readl(clkrst + P4_SOC_CLK_CTRL2);

	writel(val | P4_APB_ENABLE | P4_CLK_SOURCE_RC_FAST | P4_CLK_ENABLE,
	       clkrst + P4_SOC_CLK_CTRL2);
}

ulong notrace get_tbclk(void)
{
	return P4_TIMER_RATE;
}

unsigned long notrace timer_read_counter(void)
{
	void __iomem *timer = (void __iomem *)P4_SYSTIMER_BASE;
	int tries;

	writel(P4_UPDATE | P4_VALUE_VALID, timer + P4_UNIT0_OP);
	for (tries = 0; tries < 1000; tries++) {
		if (readl(timer + P4_UNIT0_OP) & P4_VALUE_VALID)
			break;
	}

	return readl(timer + P4_VALUE_LO);
}
