// SPDX-License-Identifier: GPL-2.0+
/* Fixed-board FIT profile. The manifest is untrusted: addresses, lengths,
 * roles and algorithm are checked, never handed directly to memcpy/jump.
 * SHA256 provides corruption detection, not public-key authentication.
 */
#include <common.h>
#include <asm/io.h>
#include <linux/libfdt.h>
#include <u-boot/sha256.h>
#include "spl_hw.h"
#include "spl_load.h"
#include <asm/arch-esp32p4/boot-layout.h>

#if CONFIG_SPL_OF_LIBFDT_ASSUME_MASK != 0
#error "Untrusted FIT input requires full libfdt bounds checking"
#endif

/* Limit position-independent proper payload below the live SPL. Its
 * BSS can reuse the SPL only after handoff. */
#include "psram-handoff.h"

#define FIT_OFF   0x000f0000U
#define HDR_LIMIT 0x00010000U
#define CHUNK     0x4000U
struct fit_slot {
	const char *node, *type, *os;
	u32 off, dst, limit;
};
static const struct fit_slot slots[] = {
	{ "uboot", "standalone", "u-boot", 0x100000, P4_PROPER_BASE, P4_PROPER_SLOT },
	{ "linux", "kernel", "linux", P4_FIT_IMAGE_OFF, P4_FIT_IMAGE, P4_FIT_IMAGE_FLASH_LIMIT },
	{ "dtb", "flat_dt", "linux", P4_FIT_DTB_OFF, P4_FIT_DTB, P4_FIT_DTB_LIMIT },
};
struct payload {
	u32 len;
	u8 hash[SHA256_SUM_LEN];
};

static int prop(const void *f, int n, const char *k, const void *val, int size)
{
	int len;
	const void *p = fdt_getprop(f, n, k, &len);

	return p && len == size && !memcmp(p, val, len);
}
#define STR(f, n, k, s) prop(f, n, k, s, strlen(s) + 1)

static int getnum(const void *f, int n, const char *k, u32 *v)
{
	int len;
	const fdt32_t *p = fdt_getprop(f, n, k, &len);

	if (!p || len != 4)
		return -1;
	*v = fdt32_to_cpu(*p);
	return 0;
}

static int describe(const void *f, int images, int i, struct payload *p)
{
	const struct fit_slot *s = slots + i;
	int len, n = fdt_subnode_offset(f, images, s->node), h;
	u32 off, load, entry;
	const u8 *hash;

	if (n < 0 || !STR(f, n, "type", s->type) || !STR(f, n, "os", s->os) ||
	    !STR(f, n, "arch", "riscv") ||
	    getnum(f, n, "data-position", &off) || off != s->off - FIT_OFF ||
	    getnum(f, n, "data-size", &p->len) || p->len < 64 || p->len > s->limit ||
	    getnum(f, n, "load", &load) || load != s->dst ||
	    (i < 2 && (getnum(f, n, "entry", &entry) || entry != s->dst)) ||
	    fdt_getprop(f, n, "data", NULL) || fdt_getprop(f, n, "data-offset", NULL))
		return -1;
	if (!STR(f, n, "compression", "none"))
		return -1;
	h = fdt_subnode_offset(f, n, "hash-1");
	if (h < 0 || !STR(f, h, "algo", "sha256"))
		return -1;
	hash = fdt_getprop(f, h, "value", &len);
	if (!hash || len != SHA256_SUM_LEN)
		return -1;
	memcpy(p->hash, hash, SHA256_SUM_LEN);
	return 0;
}

int p4_spl_load_fit(void)
{
	const void *f = (const void *)(P4_FLASH_WIN + FIT_OFF);
	struct payload p[ARRAY_SIZE(slots)];
	int i, conf, images;
	u32 off, n;
	struct p4_fit_handoff *rec = (void *)P4_FIT_HANDOFF;

	WRITE_ONCE(rec->magic, 0);
	if (!p4_ext_size())
		goto invalid;

	if (fdt_magic(f) != FDT_MAGIC || fdt_totalsize(f) < sizeof(struct fdt_header) ||
	    fdt_totalsize(f) > HDR_LIMIT || fdt_check_header(f))
		goto invalid;
	conf = fdt_path_offset(f, "/configurations");
	if (conf < 0 || !STR(f, conf, "default", "p4"))
		goto invalid;
	conf = fdt_subnode_offset(f, conf, "p4");
	images = fdt_path_offset(f, "/images");
	if (conf < 0 || images < 0 || !STR(f, conf, "firmware", "uboot") ||
	    !STR(f, conf, "kernel", "linux") || !STR(f, conf, "fdt", "dtb"))
		goto invalid;
	for (i = 0; i < ARRAY_SIZE(slots); i++)
		if (describe(f, images, i, &p[i]))
			goto invalid;
	/* Only U-Boot proper is copied. Linux and its original DTB stay in NOR. */
	for (i = 0; i < ARRAY_SIZE(slots); i++) {
		const struct fit_slot *s = &slots[i];
		const u8 *src = (const u8 *)(uintptr_t)(P4_FLASH_WIN + s->off);
		sha256_context ctx;
		u8 sum[SHA256_SUM_LEN];

		sha256_starts(&ctx);
		for (off = 0; off < p[i].len; off += n) {
			const u8 *data = src + off;

			n = min(CHUNK, p[i].len - off);
			if (!i) {
				u8 *dst = (void *)(uintptr_t)(s->dst + off);

				memcpy(dst, data, n);
				data = dst;
			}
			sha256_update(&ctx, data, n);
			p4_spl_wdt_feed();
		}
		sha256_finish(&ctx, sum);
		if (memcmp(sum, p[i].hash, sizeof(sum))) {
			printf("P4 FIT: %s SHA256 mismatch; halted\n", s->node);
			return -1;
		}
		printf("P4 FIT: %s SHA256 OK (%u bytes)\n", s->node, p[i].len);
	}
	/* ROM-mapped kernel length excludes BSS and must match the FIT input. */
	if (fdt_check_full((void *)P4_FIT_DTB, p[2].len) ||
	    fdt_totalsize((void *)P4_FIT_DTB) != p[2].len ||
	    readl((void *)(P4_FIT_IMAGE + 0x38)) != 0x05435352 ||
	    readl((void *)(P4_FIT_IMAGE + 0x14)) ||
	    readl((void *)(P4_FIT_IMAGE + 0x10)) != p[1].len)
		goto invalid;
	rec->image_len = p[1].len;
	rec->dtb_len = p[2].len;
	memcpy(rec->image_hash, p[1].hash, SHA256_SUM_LEN);
	memcpy(rec->dtb_hash, p[2].hash, SHA256_SUM_LEN);
	asm volatile("fence rw,w" ::: "memory");
	WRITE_ONCE(rec->magic, P4_FIT_MAGIC_XIP);
	((int (*)(u32, u32, u32))0x4fc003f4)(0x10, slots[0].dst, p[0].len);
	((int (*)(u32, u32, u32))0x4fc003f4)(0x20, slots[0].dst, p[0].len);
	asm volatile("fence rw,rw; fence.i" ::: "memory");
	return 0;
invalid:
	puts("P4 FIT: invalid manifest/image; halted\n");
	return -1;
}
