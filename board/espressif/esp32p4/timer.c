// SPDX-License-Identifier: GPL-2.0+
/*
 * ESP32-P4 SYSTIMER timer driver.
 *
 * The P4 has no CLINT, so U-Boot's timebase comes from the system timer. The
 * counter is latched: write UPDATE|VALUE_VALID to UNIT0_OP, wait for VALID,
 * then read the latched high/low halves. The timer's APB clock gate lives in
 * HP_SYS_CLKRST at SOC_CLK_CTRL2 (offset 0x1c), bit 23 (APB enable), with the
 * 16 MHz RC_FAST source selected by bit 29 and enabled by bit 30.
 */

#include <common.h>
#include <dm.h>
#include <timer.h>
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
#define P4_VALUE_HI		0x40
#define P4_VALUE_LO		0x44
#define P4_COUNTER_HI_MASK	GENMASK(19, 0)

struct p4_timer_priv {
	void __iomem *base;
};

static void p4_timer_clock_enable(void)
{
	void __iomem *clkrst = (void __iomem *)P4_HP_SYS_CLKRST;
	u32 val;

	val = readl(clkrst + P4_SOC_CLK_CTRL2);
	val |= P4_APB_ENABLE | P4_CLK_SOURCE_RC_FAST | P4_CLK_ENABLE;
	writel(val, clkrst + P4_SOC_CLK_CTRL2);
}

static u64 p4_timer_get_count(struct udevice *dev)
{
	struct p4_timer_priv *priv = dev_get_priv(dev);
	u32 hi, lo;
	int tries;

	writel(P4_UPDATE | P4_VALUE_VALID, priv->base + P4_UNIT0_OP);
	for (tries = 0; tries < 1000; tries++) {
		if (readl(priv->base + P4_UNIT0_OP) & P4_VALUE_VALID)
			break;
	}

	hi = readl(priv->base + P4_VALUE_HI) & P4_COUNTER_HI_MASK;
	lo = readl(priv->base + P4_VALUE_LO);

	return ((u64)hi << 32) | lo;
}

static int p4_timer_probe(struct udevice *dev)
{
	struct p4_timer_priv *priv = dev_get_priv(dev);

	priv->base = dev_read_addr_ptr(dev);
	if (!priv->base)
		priv->base = (void __iomem *)P4_SYSTIMER_BASE;

	p4_timer_clock_enable();

	return timer_timebase_fallback(dev);
}

static const struct timer_ops p4_timer_ops = {
	.get_count = p4_timer_get_count,
};

static const struct udevice_id p4_timer_ids[] = {
	{ .compatible = "espressif,esp32p4-systimer" },
	{ }
};

U_BOOT_DRIVER(esp32p4_systimer) = {
	.name		= "esp32p4_systimer",
	.id		= UCLASS_TIMER,
	.of_match	= p4_timer_ids,
	.priv_auto	= sizeof(struct p4_timer_priv),
	.probe		= p4_timer_probe,
	.ops		= &p4_timer_ops,
};
