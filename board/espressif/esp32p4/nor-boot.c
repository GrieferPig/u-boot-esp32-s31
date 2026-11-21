// SPDX-License-Identifier: GPL-2.0+
/*
 * Standard Boot method for the ESP32-P4 fixed-slot NOR XIP image.
 */

#include <linux/bitops.h>
#include <bootdev.h>
#include <bootflow.h>
#include <bootmeth.h>
#include <dm.h>
#include <env.h>
#include <stdio.h>
#include <linux/errno.h>
#include <linux/libfdt.h>
#include <linux/string.h>
#include <u-boot/sha256.h>

#include "cache-range.h"
#include "psram-handoff.h"

/* The device tree may be edited before entering Linux. The kernel stays in NOR. */
#define P4_DTB_KERN		0x48f00000u
#define P4_DTB_KERN_BYTES	P4_FIT_DTB_LIMIT

static const struct p4_fit_handoff *p4_get_fit(void)
{
	const struct p4_fit_handoff *fit = (void *)P4_FIT_HANDOFF;

	if (!p4_ext_size() || READ_ONCE(fit->magic) != P4_FIT_MAGIC_XIP)
		return NULL;
	asm volatile("fence r,r" ::: "memory");
	if (fit->image_len < 64 || fit->image_len > P4_FIT_IMAGE_FLASH_LIMIT ||
	    fit->dtb_len < sizeof(struct fdt_header) ||
	    fit->dtb_len > P4_FIT_DTB_LIMIT)
		return NULL;
	return fit;
}

static void p4_sha256(const void *data, u32 size, u8 *digest)
{
	sha256_context ctx;
	const u8 *p = data;

	sha256_starts(&ctx);
	while (size) {
		u32 n = min_t(u32, size, 0x4000u);

		sha256_update(&ctx, p, n);
		size -= n;
		p += n;
	}
	sha256_finish(&ctx, digest);
}

static bool p4_fit_sha_ok(const void *data, u32 size, const u8 *expected)
{
	u8 actual[SHA256_SUM_LEN];

	p4_sha256(data, size, actual);
	return !memcmp(expected, actual, sizeof(actual));
}

/*
 * TRM ch.9 System and Memory — the FDT uses the cached external RAM window.
 * Keep the hashed FIT input intact and open the tree in the separate 64 KiB
 * Linux reservation at the runtime stack's upper bound. Verify the final
 * bytes after writeback and invalidation before entering Linux.
 */
static int p4_prep_dtb(const struct p4_fit_handoff *fit)
{
	const void *src = (void *)P4_FIT_DTB;
	void *fdt = (void *)P4_DTB_KERN;
	u8 digest[SHA256_SUM_LEN];
	u32 size = p4_ext_size(), final;
	int chosen, mem, ret;
	const char *bootargs = env_get("bootargs");
	fdt32_t reg[2];

	if (!p4_fit_sha_ok(src, fit->dtb_len, fit->dtb_hash) ||
	    fdt_check_full(src, fit->dtb_len)) {
		puts("p4: FIT DTB in NOR invalid\n");
		return -EINVAL;
	}

	ret = fdt_open_into(src, fdt, P4_DTB_KERN_BYTES);
	if (ret) {
		printf("p4: cannot open DTB in %u-byte handoff: %s\n",
		       P4_DTB_KERN_BYTES, fdt_strerror(ret));
		return -EINVAL;
	}
	mem = fdt_path_offset(fdt, "/memory@48000000");
	reg[0] = cpu_to_fdt32(P4_EXT_RAM_BASE);
	reg[1] = cpu_to_fdt32(size);
	if (mem < 0 || fdt_address_cells(fdt, 0) != 1 ||
	    fdt_size_cells(fdt, 0) != 1 ||
	    fdt_setprop(fdt, mem, "reg", reg, sizeof(reg))) {
		puts("p4: invalid DTB memory node\n");
		return -EINVAL;
	}

	chosen = fdt_path_offset(fdt, "/chosen");
	if (chosen < 0 ||
	    (bootargs && fdt_setprop_string(fdt, chosen, "bootargs", bootargs)) ||
	    fdt_pack(fdt)) {
		puts("p4: DTB fixup/pack failed\n");
		return -EINVAL;
	}
	final = fdt_totalsize(fdt);
	if (final < sizeof(struct fdt_header) || final > P4_DTB_KERN_BYTES ||
	    fdt_check_full(fdt, final))
		return -EINVAL;
	p4_sha256(fdt, final, digest);
	if (p4_cache_republish(P4_DTB_KERN, final)) {
		puts("p4: final cached DTB sync failed\n");
		return -EIO;
	}
	if (fdt_check_full(fdt, final) || !p4_fit_sha_ok(fdt, final, digest)) {
		puts("p4: final cached DTB reload mismatch\n");
		return -EIO;
	}
	printf("P4 DTB-CACHED: %u byte reload OK, mem %08x/%08x\n",
	       final, P4_EXT_RAM_BASE, size);
	return 0;
}

static int p4_boot_nor(struct udevice *dev, struct bootflow *bflow)
{
	void (*kernel)(u32 hart, void *dtb) = (void *)P4_FIT_IMAGE;
	const struct p4_fit_handoff *fit = p4_get_fit();

	if (!fit || !p4_fit_sha_ok((const void *)P4_FIT_IMAGE, fit->image_len,
				   fit->image_hash)) {
		puts("ESP32-P4 NOR: missing or invalid NOR XIP kernel\n");
		return -EIO;
	}
	if (p4_prep_dtb(fit))
		return -EIO;

	printf("ESP32-P4 NOR: NOR XIP kernel %08x fdt %08x, jumping\n",
	       P4_FIT_IMAGE, P4_DTB_KERN);
	asm volatile("csrci mstatus,8; fence rw,rw; fence.i" ::: "memory");
	kernel(0, (void *)P4_DTB_KERN);
	return -EIO;
}

static int p4_nor_check(struct udevice *dev, struct bootflow_iter *iter)
{
	return bootflow_iter_check_system(iter);
}

static int p4_nor_read_bootflow(struct udevice *dev, struct bootflow *bflow)
{
	if (!p4_get_fit())
		return -ENOENT;

	bflow->state = BOOTFLOWST_READY;
	return 0;
}

static int p4_nor_bind(struct udevice *dev)
{
	struct bootmeth_uc_plat *plat = dev_get_uclass_plat(dev);

	plat->desc = "ESP32-P4 mapped SPI NOR FIT";
	plat->flags = BOOTMETHF_GLOBAL;
	return 0;
}

static struct bootmeth_ops p4_nor_ops = {
	.check = p4_nor_check,
	.read_bootflow = p4_nor_read_bootflow,
	.boot = p4_boot_nor,
};

static const struct udevice_id p4_nor_ids[] = {
	{ .compatible = "espressif,esp32p4-nor-bootmeth" },
	{ }
};

U_BOOT_DRIVER(bootmeth_esp32p4_nor) = {
	.name = "bootmeth_esp32p4_nor",
	.id = UCLASS_BOOTMETH,
	.of_match = p4_nor_ids,
	.ops = &p4_nor_ops,
	.bind = p4_nor_bind,
};
