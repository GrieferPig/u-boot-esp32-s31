// SPDX-License-Identifier: GPL-2.0+
/*
 * ESP32-P4 timebase: the P4 has no CLINT, so both SPL and U-Boot proper read
 * the system timer instead. The counter must be latched: write
 * TRM ch.17 System Timer: set TIMER_UNIT0_UPDATE in UNIT0_OP, wait for
 * TIMER_UNIT0_VALUE_VALID, then read the latched UNIT0_VALUE_HI/LO.
 *
 * The timer's APB clock gate lives in HP_SYS_CLKRST at SOC_CLK_CTRL2 (offset
 * 0x1c): bit 23 gates the APB clock, bit 29 selects the 16 MHz RC_FAST source
 * and bit 30 enables it.
 */

#include <dm.h>
#include <hang.h>
#include <timer.h>
#include <asm/io.h>
#include <linux/bitops.h>
#include <linux/errno.h>

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

#define P4_TIMER_RATE		16000000

void p4_systimer_enable(void)
{
	void __iomem *clkrst = (void __iomem *)P4_HP_SYS_CLKRST;
	u32 val = readl(clkrst + P4_SOC_CLK_CTRL2);

	writel(val | P4_APB_ENABLE | P4_CLK_SOURCE_RC_FAST | P4_CLK_ENABLE,
	       clkrst + P4_SOC_CLK_CTRL2);
}

static int p4_timer_latch(void __iomem *timer)
{
	int count;

	setbits_le32(timer + P4_UNIT0_OP, P4_UPDATE);
	for (count = 0; count < 1000; count++) {
		if (readl(timer + P4_UNIT0_OP) & P4_VALUE_VALID)
			return 0;
	}

	return -ETIMEDOUT;
}

static int p4_timer_count(void __iomem *timer, u64 *ticks)
{
	int ret = p4_timer_latch(timer);

	if (ret)
		return ret;

	*ticks = (u64)readl(timer + P4_VALUE_HI) << 32;
	*ticks |= readl(timer + P4_VALUE_LO);
	return 0;
}

#if CONFIG_IS_ENABLED(TIMER)
static u64 notrace p4_dm_timer_count(struct udevice *dev)
{
	void __iomem **base = dev_get_priv(dev);
	u64 ticks;

	if (p4_timer_count(*base, &ticks))
		hang();

	return ticks;
}

static int p4_dm_timer_probe(struct udevice *dev)
{
	void __iomem **base = dev_get_priv(dev);
	struct timer_dev_priv *priv = dev_get_uclass_priv(dev);

	*base = dev_read_addr_ptr(dev);
	if (!*base)
		return -EINVAL;

	priv->clock_rate = dev_read_u32_default(dev, "clock-frequency", 0);
	if (priv->clock_rate != P4_TIMER_RATE)
		return -EINVAL;

	p4_systimer_enable();
	return 0;
}

static const struct timer_ops p4_timer_ops = {
	.get_count = p4_dm_timer_count,
};

static const struct udevice_id p4_timer_ids[] = {
	{ .compatible = "espressif,esp32p4-systimer" },
	{ }
};

U_BOOT_DRIVER(esp32p4_systimer) = {
	.name = "esp32p4_systimer",
	.id = UCLASS_TIMER,
	.of_match = p4_timer_ids,
	.probe = p4_dm_timer_probe,
	.ops = &p4_timer_ops,
	.priv_auto = sizeof(void __iomem *),
};

ulong notrace timer_early_get_rate(void)
{
	return P4_TIMER_RATE;
}

u64 notrace timer_early_get_count(void)
{
	u64 ticks;

	if (p4_timer_count((void __iomem *)P4_SYSTIMER_BASE, &ticks))
		hang();

	return ticks;
}
#else
ulong notrace get_tbclk(void)
{
	return P4_TIMER_RATE;
}

unsigned long notrace timer_read_counter(void)
{
	u64 ticks;

	if (p4_timer_count((void __iomem *)P4_SYSTIMER_BASE, &ticks))
		hang();

	return (u32)ticks;
}
#endif
