// SPDX-License-Identifier: GPL-2.0+
/*
 * Standard Boot method for the ESP32-S31 mapped SPI NOR Linux image.
 */

#include <linux/bitops.h>
#include <bootdev.h>
#include <bootflow.h>
#include <bootmeth.h>
#include <command.h>
#include <dm.h>
#include <image.h>
#include <stdio.h>
#include <linux/errno.h>
#include <linux/libfdt.h>
#include <linux/sizes.h>
#include <linux/string.h>

/*
 * ESP32-S31 TRM ch.5 System and Memory, External Memory Address Mapping:
 * the cached flash window starts at 0x40000000. The Linux image and DTB
 * offsets below come from the S31 integration repository's flash layout.
 */
#define S31_LINUX_NOR_ADDR	0x40400000UL
#define S31_LINUX_DTB_ADDR	0x4005e000UL
#define S31_LINUX_DTB_SIZE	SZ_64K

static int s31_nor_check(struct udevice *dev, struct bootflow_iter *iter)
{
	return bootflow_iter_check_system(iter);
}

static int s31_nor_read_bootflow(struct udevice *dev, struct bootflow *bflow)
{
	const void *fdt = (const void *)S31_LINUX_DTB_ADDR;
	ulong entry, size;
	int ret;

	ret = booti_setup(S31_LINUX_NOR_ADDR, &entry, &size, false);
	if (ret)
		return ret;
	/*
	 * Image size includes writable state placed in PSRAM by the kernel,
	 * so it is not the size of the payload written to the NOR slot.
	 */
	if (entry != S31_LINUX_NOR_ADDR)
		return -EINVAL;
	if (fdt_check_full(fdt, S31_LINUX_DTB_SIZE))
		return -EINVAL;

	bflow->name = strdup("ESP32-S31 SPI NOR");
	if (!bflow->name)
		return -ENOMEM;

	bflow->state = BOOTFLOWST_READY;
	return 0;
}

static int s31_nor_boot(struct udevice *dev, struct bootflow *bflow)
{
	char image[2 * sizeof(ulong) + 1];
	char fdt[2 * sizeof(ulong) + 1];
	char *const argv[] = { "booti", image, "-", fdt, NULL };

	snprintf(image, sizeof(image), "%lx", S31_LINUX_NOR_ADDR);
	snprintf(fdt, sizeof(fdt), "%lx", S31_LINUX_DTB_ADDR);

	return do_booti(NULL, 0, ARRAY_SIZE(argv) - 1, argv) ? -EIO : 0;
}

static int s31_nor_bind(struct udevice *dev)
{
	struct bootmeth_uc_plat *plat = dev_get_uclass_plat(dev);

	plat->desc = "ESP32-S31 mapped SPI NOR XIP";
	plat->flags = BOOTMETHF_GLOBAL;
	return 0;
}

static const struct bootmeth_ops s31_nor_ops = {
	.check = s31_nor_check,
	.read_bootflow = s31_nor_read_bootflow,
	.boot = s31_nor_boot,
};

static const struct udevice_id s31_nor_ids[] = {
	{ .compatible = "espressif,esp32s31-nor-bootmeth" },
	{ }
};

U_BOOT_DRIVER(bootmeth_esp32s31_nor) = {
	.name = "bootmeth_esp32s31_nor",
	.id = UCLASS_BOOTMETH,
	.of_match = s31_nor_ids,
	.ops = &s31_nor_ops,
	.bind = s31_nor_bind,
};
