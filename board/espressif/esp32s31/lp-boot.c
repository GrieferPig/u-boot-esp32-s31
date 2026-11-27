// SPDX-License-Identifier: GPL-2.0+
/* ESP32-S31 boot-time LP CPU loader. */

#include <asm/io.h>
#include <blk.h>
#include <div64.h>
#include <dm.h>
#include <elf.h>
#include <fs.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/sizes.h>
#include <linux/string.h>
#include <malloc.h>
#include <part.h>
#include <squashfs.h>
#include <stdio.h>
#include <time.h>

#define EM_RISCV	243

#define LP_FIRMWARE	"/lib/firmware/esp32s31/s31-lp-core.elf"

/* TRM ch.2 Low-Power CPU and ch.5 System and Memory: LP SRAM. */
#define LP_SRAM_BASE	0x2e000000UL
#define LP_SRAM_LIMIT	0x2e007c00UL	/* Shared sleep control occupies the last KiB. */
#define LP_SRAM_END	0x2e008000UL

#define LP_SYS_BOOT_ADDR	((void __iomem *)0x20700018UL)
#define LP_SYS_STORE1		((void __iomem *)0x20700030UL)
#define LP_SYS_STORE0		((void __iomem *)0x2070002cUL)

/* ESP-IDF lp_core_ll_reset_register() and _lp_core_ll_enable_bus_clock(). */
#define LP_CPU_CTRL		((void __iomem *)0x20710004UL)
#define LP_RAM_CTRL		((void __iomem *)0x2071000cUL)
#define LP_INTR_CTRL		((void __iomem *)0x20710028UL)
#define LP_MAILBOX_CTRL		((void __iomem *)0x20710038UL)
#define LP_CPU_DEBUG_CTRL	((void __iomem *)0x20710044UL)
#define LP_CLKGATE		BIT(30)
#define LP_RESET		BIT(31)
#define LP_DEBUG_UNAVAILABLE	BIT(31)

#define LP_PMS_BASE		0x20706000UL
#define LP_PMS_REE_RW		(GENMASK(7, 5) | GENMASK(3, 1))
#define LP_PMS_LOCK		BIT(8)

#define LP_CPU_PWR0		((void __iomem *)0x207041acUL)
#define LP_CPU_PWR2		((void __iomem *)0x207041b4UL)
#define LP_CPU_COMM		((void __iomem *)0x207041b8UL)
#define LP_SLEEP_STALL		BIT(29)
#define LP_SLEEP_RESET		BIT(30)
#define LP_PMU_SW_WAKEUP_HP	BIT(1)
#define LP_PMU_HP_TRIGGER_LP	BIT(31)

/* TRM ch.9 RTC Timer: MAIN_TIMER_UPDATE latches the 48-bit count. */
#define RTC_UPDATE		((void __iomem *)0x20800010UL)
#define RTC_COUNT_LO		((void __iomem *)0x20800014UL)
#define RTC_COUNT_HI		((void __iomem *)0x20800018UL)
#define RTC_DATE		((void __iomem *)0x208003fcUL)
#define RTC_LATCH		BIT(27)
#define RTC_CLOCK_ENABLE	BIT(31)
#define RTC_SAMPLE_US		100000
#define RTC_CAL_FRAC_BITS	19

/* TRM ch.11 LP Mailbox: MB_MESSAGE, MB_HP_INT_RAW, MB_HP_INT_CLR. */
#define MB_MESSAGE(n)		((void __iomem *)(0x20817000UL + (n) * 4))
#define MB_HP_RAW		((void __iomem *)0x20817050UL)
#define MB_HP_ENABLE		((void __iomem *)0x20817058UL)
#define MB_HP_CLEAR		((void __iomem *)0x2081705cUL)
#define MB_CLOCK		((void __iomem *)0x20817060UL)
#define MB_CLOCK_ENABLE		BIT(0)
#define MB_READY		0x53310001U
#define MB_ACK			0xe5U

#define LP_STAGE_MAILBOX_READY	0x4c500002U
#define LP_STAGE_RUNNING	0x4c500004U
#define LP_BOOT_TIMEOUT_MS	500

static int s31_lp_read_elf(void **image, size_t *len)
{
	struct udevice *nvm, *blk;
	struct blk_desc *desc;
	struct disk_partition partition = { 0 };
	loff_t size, actual;
	int ret;

	ret = uclass_get_device_by_ofnode(UCLASS_NVMXIP,
					ofnode_path("/rootfs@40a00000"), &nvm);
	if (ret)
		return ret;
	ret = blk_get_from_parent(nvm, &blk);
	if (ret)
		return ret;
	desc = dev_get_uclass_plat(blk);
	partition.blksz = desc->blksz;
	partition.size = desc->lba;

	ret = sqfs_probe(desc, &partition);
	if (ret)
		return ret;

	ret = sqfs_size(LP_FIRMWARE, &size);
	if (ret)
		goto close;
	if (size < sizeof(Elf32_Ehdr) || size > SZ_1M) {
		ret = -EFBIG;
		goto close;
	}
	*image = malloc(size);
	if (!*image) {
		ret = -ENOMEM;
		goto close;
	}

	ret = sqfs_read(LP_FIRMWARE, *image, 0, size, &actual);
	if (!ret && actual != size)
		ret = -EIO;
	if (ret)
		free(*image);
	else
		*len = size;
close:
	sqfs_close();
	return ret;
}

static int s31_lp_validate_elf(const void *image, size_t len)
{
	const Elf32_Ehdr *ehdr = image;
	const Elf32_Phdr *phdr;
	bool entry_loaded = false;
	unsigned int i;

	if (memcmp(ehdr->e_ident, ELFMAG, SELFMAG) ||
	    ehdr->e_ident[EI_CLASS] != ELFCLASS32 ||
	    ehdr->e_ident[EI_DATA] != ELFDATA2LSB ||
	    ehdr->e_ident[EI_VERSION] != EV_CURRENT ||
	    ehdr->e_type != ET_EXEC || ehdr->e_machine != EM_RISCV ||
	    ehdr->e_version != EV_CURRENT || ehdr->e_ehsize != sizeof(*ehdr) ||
	    ehdr->e_phentsize != sizeof(*phdr) || !ehdr->e_phnum ||
	    ehdr->e_phoff < sizeof(*ehdr) ||
	    !IS_ALIGNED(ehdr->e_phoff, __alignof__(Elf32_Phdr)) ||
	    ehdr->e_phoff > len ||
	    ehdr->e_phnum > (len - ehdr->e_phoff) / sizeof(*phdr) ||
	    ehdr->e_entry != LP_SRAM_BASE + 0x80)
		return -ENOEXEC;

	phdr = image + ehdr->e_phoff;
	for (i = 0; i < ehdr->e_phnum; i++, phdr++) {
		if (phdr->p_type != PT_LOAD)
			continue;
		if (phdr->p_vaddr != phdr->p_paddr ||
		    phdr->p_paddr < LP_SRAM_BASE || phdr->p_paddr >= LP_SRAM_LIMIT ||
		    phdr->p_offset > len || phdr->p_filesz > len - phdr->p_offset ||
		    phdr->p_filesz > phdr->p_memsz ||
		    phdr->p_memsz > LP_SRAM_LIMIT - phdr->p_paddr)
			return -ENOEXEC;
		if ((phdr->p_flags & PF_X) && phdr->p_paddr <= ehdr->e_entry &&
		    ehdr->e_entry - phdr->p_paddr < phdr->p_filesz)
			entry_loaded = true;
	}
	return entry_loaded ? 0 : -ENOEXEC;
}

static void s31_lp_copy_segments(const void *image)
{
	const Elf32_Ehdr *ehdr = image;
	const Elf32_Phdr *phdr = image + ehdr->e_phoff;
	unsigned int i;

	memset((void *)LP_SRAM_BASE, 0, LP_SRAM_END - LP_SRAM_BASE);
	for (i = 0; i < ehdr->e_phnum; i++, phdr++) {
		if (phdr->p_type == PT_LOAD)
			memcpy((void *)phdr->p_paddr, image + phdr->p_offset,
			       phdr->p_filesz);
	}
	/* Publish the uncached LP SRAM writes before waking the LP CPU. */
	mb();
}

static int s31_lp_grant_access(void)
{
	/* ESP-IDF lp_peri_pms_reg.h: SYSREG, TIMER, PERICLKRST, IOMUX, MAILBOX. */
	static const u8 offsets[] = { 0x00, 0x24, 0x38, 0x3c, 0x64 };
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(offsets); i++) {
		void __iomem *reg = (void __iomem *)(LP_PMS_BASE + offsets[i]);
		u32 value = readl(reg);

		if ((value & LP_PMS_REE_RW) == LP_PMS_REE_RW)
			continue;
		if (value & LP_PMS_LOCK)
			return -EACCES;
		writel(value | LP_PMS_REE_RW, reg);
		if ((readl(reg) & LP_PMS_REE_RW) != LP_PMS_REE_RW)
			return -EACCES;
	}
	return 0;
}

static u64 s31_lp_rtc_count(void)
{
	writel(RTC_LATCH, RTC_UPDATE);
	return ((u64)(readl(RTC_COUNT_HI) & 0xffff) << 32) |
	       readl(RTC_COUNT_LO);
}

static int s31_lp_calibrate_rtc(void)
{
	ulong start, elapsed;
	u64 first, delta, rate, period;

	setbits_le32(RTC_DATE, RTC_CLOCK_ENABLE);
	first = s31_lp_rtc_count();
	start = timer_get_us();
	udelay(RTC_SAMPLE_US);
	delta = (s31_lp_rtc_count() - first) & GENMASK_ULL(47, 0);
	elapsed = timer_get_us() - start;
	if (!delta || !elapsed)
		return -EIO;

	rate = lldiv(delta * 1000000U, elapsed);
	if (rate < 20000 || rate > 300000)
		return -ERANGE;

	/* ESP-IDF clk_ll_rtc_slow_store_cal(): STORE1 holds the Q13.19 period. */
	period = lldiv((u64)elapsed << RTC_CAL_FRAC_BITS, delta);
	writel(period, LP_SYS_STORE1);
	return 0;
}

static int s31_lp_wait_stage(u32 stage)
{
	ulong start = get_timer(0);

	do {
		u32 current = readl(LP_SYS_STORE0);

		if (current >= stage && current <= LP_STAGE_RUNNING)
			return 0;
	} while (get_timer(start) < LP_BOOT_TIMEOUT_MS);

	return -ETIMEDOUT;
}

static int s31_lp_handshake(void)
{
	ulong start;
	int ret;

	/* ESP-IDF lp_core_mailbox_impl_init() resets the mailbox, including
	 * the clock enable and HP interrupt mask. Wait for that reset to end
	 * before accessing the mailbox from HP.
	 */
	ret = s31_lp_wait_stage(LP_STAGE_MAILBOX_READY);
	if (ret)
		return ret;
	writel(MB_CLOCK_ENABLE, MB_CLOCK);
	writel(0, MB_HP_ENABLE);

	start = get_timer(0);
	do {
		if (readl(MB_HP_RAW) & BIT(0)) {
			if (readl(MB_MESSAGE(0)) != MB_READY)
				return -EPROTO;
			writel(MB_ACK, MB_MESSAGE(1));
			writel(BIT(0) | BIT(1), MB_HP_CLEAR);
			return s31_lp_wait_stage(LP_STAGE_RUNNING);
		}
	} while (get_timer(start) < LP_BOOT_TIMEOUT_MS);

	return -ETIMEDOUT;
}

int esp32s31_lp_boot(void)
{
	static bool booted;
	void *image;
	size_t len;
	u32 entry;
	int ret;

	/* A failed booti may return to the prompt with LP already running. */
	if (booted)
		return 0;

	ret = s31_lp_read_elf(&image, &len);
	if (ret)
		return ret;
	ret = s31_lp_validate_elf(image, len);
	if (ret)
		goto free_image;
	ret = s31_lp_grant_access();
	if (ret)
		goto free_image;
	ret = s31_lp_calibrate_rtc();
	if (ret)
		goto free_image;

	entry = ((const Elf32_Ehdr *)image)->e_entry;
	setbits_le32(LP_RAM_CTRL, LP_CLKGATE);
	writel(entry, LP_SYS_BOOT_ADDR);
	writel(0, LP_SYS_STORE0);
	setbits_le32(LP_CPU_CTRL, LP_RESET);
	setbits_le32(LP_INTR_CTRL, LP_RESET);
	clrsetbits_le32(LP_CPU_CTRL, LP_RESET, LP_CLKGATE);
	clrsetbits_le32(LP_INTR_CTRL, LP_RESET, LP_CLKGATE);
	clrbits_le32(LP_CPU_DEBUG_CTRL, LP_DEBUG_UNAVAILABLE);
	setbits_le32(LP_MAILBOX_CTRL, LP_CLKGATE);

	s31_lp_copy_segments(image);
	/* ESP-IDF ulp_lp_core_run(): configure HP software wake, then trigger. */
	setbits_le32(LP_CPU_PWR0, LP_SLEEP_STALL | LP_SLEEP_RESET);
	writel(LP_PMU_SW_WAKEUP_HP, LP_CPU_PWR2);
	writel(LP_PMU_HP_TRIGGER_LP, LP_CPU_COMM);
	ret = s31_lp_handshake();
	if (ret)
		printf("ESP32-S31 LP init stage 0x%08x\n", readl(LP_SYS_STORE0));
	else {
		booted = true;
		printf("ESP32-S31 LP core ready (SquashFS, entry 0x%08x)\n", entry);
	}

free_image:
	free(image);
	return ret;
}
