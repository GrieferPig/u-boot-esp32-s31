// SPDX-License-Identifier: GPL-2.0+
/*
 * ESP32-P4 SPL: the mask ROM's second stage.
 *
 * Goal (mirrors the S31 port): first-stage SoC bring-up (watchdogs, PSRAM,
 * clock tree), then load U-Boot proper from NOR into PSRAM and jump to it, so
 * no ESP-IDF application is needed in the boot chain.
 *
 * The ROM loads this image into internal SRAM (SPL_TEXT_BASE) because it is
 * flashed as an ESP bootable image (esptool elf2image).
 */

#include <spl.h>
#include <stdio.h>
#include <asm/io.h>
#include <asm/sections.h>
#include <linux/bitops.h>

#include "spl_load.h"
#include "spl_nor.h"
#include "spl_psram.h"

/* SYSTIMER, used as the SPL timebase (the P4 has no CLINT / rdtime). */
#define P4_SYSTIMER_BASE	0x500e2000ul
#define P4_HP_SYS_CLKRST	0x500e6000ul
#define P4_SOC_CLK_CTRL2	0x1c
#define P4_TIMER_APB_EN		BIT(23)
#define P4_TIMER_SRC_RC_FAST	BIT(29)
#define P4_TIMER_CLK_EN		BIT(30)
#define P4_UNIT0_OP		0x04
#define P4_UPDATE		BIT(30)
#define P4_VALUE_VALID		BIT(29)
#define P4_VALUE_HI		0x40
#define P4_VALUE_LO		0x44
#define P4_COUNTER_HI_MASK	0x000fffff

static void p4_systimer_enable(void)
{
	void *clkrst = (void *)P4_HP_SYS_CLKRST;
	u32 val = readl(clkrst + P4_SOC_CLK_CTRL2);

	writel(val | P4_TIMER_APB_EN | P4_TIMER_SRC_RC_FAST | P4_TIMER_CLK_EN,
	       clkrst + P4_SOC_CLK_CTRL2);
}


#if CONFIG_IS_ENABLED(TIMER_EARLY)
unsigned long notrace timer_early_get_rate(void)
{
	return 16000000;
}

u64 notrace timer_early_get_count(void)
{
	void *timer = (void *)P4_SYSTIMER_BASE;
	u32 hi, lo;
	int tries;

	writel(P4_UPDATE | P4_VALUE_VALID, timer + P4_UNIT0_OP);
	for (tries = 0; tries < 1000; tries++) {
		if (readl(timer + P4_UNIT0_OP) & P4_VALUE_VALID)
			break;
	}
	hi = readl(timer + P4_VALUE_HI) & P4_COUNTER_HI_MASK;
	lo = readl(timer + P4_VALUE_LO);

	return ((u64)hi << 32) | lo;
}
#endif

/* ROM-armed watchdogs live in the LP/RTC block on the P4. */
#define P4_LP_WDT_BASE		0x50116000
#define P4_WDT_WKEY		0x50D83AA1
#define P4_LP_WDT_WPROTECT	0x18
#define P4_LP_WDT_CONFIG1		0x04
#define P4_LP_WDT_FEED		0x14
#define P4_LP_WDT_CONFIG0	0x00

/* Timer-group watchdogs: WDT registers sit at 0x48/0x60/0x64 in each group. */
#define P4_TIMG_BASE		0x500c2000

/* HP UART0, used before the driver model exists. */
#define P4_UART0_BASE		0x500ca000
#define P4_UART_TXFIFO_CNT_M	0x00ff0000
#define P4_UART_TXFIFO_CNT_S	16

void p4_spl_putc(const char c)
{
	void *uart = (void *)P4_UART0_BASE;
	int guard = 100000;

	while (guard-- > 0) {
		u32 status = readl(uart + 0x1c);

		if (((status & P4_UART_TXFIFO_CNT_M) >> P4_UART_TXFIFO_CNT_S) < 100)
			break;
	}
	writel((u32)(unsigned char)c, uart);
}

void p4_spl_puts(const char *s)
{
	while (*s) {
		if (*s == '\n')
			p4_spl_putc('\r');
		p4_spl_putc(*s++);
	}
}

void p4_spl_early_puts(const char *s)
{
	p4_spl_puts(s);
}

void p4_spl_early_hex(u32 value)
{
	static const char digits[] = "0123456789abcdef";
	int shift;

	p4_spl_puts("0x");
	for (shift = 28; shift >= 0; shift -= 4)
		p4_spl_putc(digits[(value >> shift) & 0xf]);
}

/*
 * The ROM does not zero our BSS (the SRAM segments contain only initialized data), so
 * do it before start.S reads the global-data lottery word from BSS.
 */
static void p4_spl_clear_bss(void)
{
	unsigned long *p = (unsigned long *)__bss_start;

	while (p < (unsigned long *)__bss_end)
		*p++ = 0;
}

#ifdef CONFIG_ESP32P4_SPL_IDF_INIT
#include "idf-init/abi.h"
#endif

void harts_early_init(void)
{
	p4_spl_clear_bss();
#ifdef CONFIG_ESP32P4_SPL_IDF_INIT
	/* No system-wide watchdog, clock or memory writes ahead of the
	 * matched bootloader hardware sequence. This is ROM-loaded SRAM. */
	if (p4_idf_api()->magic != P4_IDF_MAGIC || p4_idf_api()->start()) {
		p4_spl_puts("P4SPL: matched init failed; halted\n");
		for (;;)
			asm volatile("wfi");
	}
	p4_systimer_enable();
	p4_spl_puts("P4SPL: IDF hardware init returned\n");
#else
	void *wdt = (void *)P4_LP_WDT_BASE;

	p4_systimer_enable();
	p4_spl_puts("\nP4SPL:entry\n");

	/*
	 * Disarm the ROM-armed LP watchdog before doing slow work: unlock the
	 * protection, clear the enable bit in CONFIG1 and the config registers,
	 * feed the counter, then lock it again.
	 */
	writel(P4_WDT_WKEY, wdt + P4_LP_WDT_WPROTECT);
	writel(0, wdt + P4_LP_WDT_CONFIG1);
	writel(0, wdt + P4_LP_WDT_CONFIG0);
	writel(P4_WDT_WKEY, wdt + P4_LP_WDT_FEED);
	writel(0, wdt + P4_LP_WDT_WPROTECT);

	/* Same for both timer-group watchdogs (the HP-domain MWDTs). */
	{
		u32 t;

		for (t = 0; t < 2; t++) {
			u32 base = P4_TIMG_BASE + t * 0x1000;

			writel(P4_WDT_WKEY, (void *)(base + 0x64));
			writel(readl((void *)(base + 0x48)) & ~(1u << 31),
			       (void *)(base + 0x48));
			writel(P4_WDT_WKEY, (void *)(base + 0x60));
			writel(0, (void *)(base + 0x64));
		}
	}

	p4_spl_puts("P4SPL:wdt-off\n");
#endif
}

static void p4_dump_block(const char *name, u32 base, int first, int last)
{
	int off;
	char buf[16];

	p4_spl_puts(name);
	for (off = first; off <= last; off += 4) {
		u32 v = readl((void *)(uintptr_t)(base + off));
		int i;

		buf[0] = ' ';
		for (i = 0; i < 8; i++)
			buf[1 + i] = "0123456789abcdef"[(v >> (28 - 4 * i)) & 0xf];
		buf[9] = 0;
		p4_spl_puts(buf);
	}
	p4_spl_puts("\n");
}

static void p4_psram_dump(void)
{
	p4_spl_puts("SPLD clkrst");
	p4_dump_block("", 0x500e6000, 0x00, 0x40);
	p4_dump_block("SPLD pmu", 0x50115000, 0x1c0, 0x220);
	p4_dump_block("SPLD mspi2", 0x5008e000, 0x00, 0x60);
	p4_dump_block("SPLD mspi3", 0x5008f000, 0x00, 0x60);
	p4_dump_block("SPLD mspi3t", 0x5008f000, 0x50, 0x70);
}

void spl_board_init(void)
{
	p4_spl_puts("ESP32-P4 SPL active\n");
	p4_psram_dump();

	if (p4_spl_psram_init()) {
		p4_spl_puts("spl: psram_init not ported yet; boot stopped\n");
		for (;;)
			asm volatile("wfi");
	}

	p4_spl_puts("spl: psram ready\n");


	if (p4_spl_flash_init(0x01000000)) {
		p4_spl_puts("spl: flash map failed; boot stopped\n");
		for (;;)
			;
	}

	{
		void p4_spl_psram_preload(void);

		p4_spl_psram_preload();
	}


	/* payload loading happens from the U-Boot prompt for now */

	if (p4_spl_load_uboot()) {
		p4_spl_puts("spl: uboot load failed; boot stopped\n");
		for (;;)
			;
	}

	p4_spl_enter_uboot(0);
}

u32 spl_boot_device(void)
{
	return BOOT_DEVICE_NOR;
}

void *board_fdt_blob_setup(int *err)
{
	*err = 0;
	return (void *)_end;
}
