// SPDX-License-Identifier: GPL-2.0+
/* Fixed-board FIT profile. The manifest is untrusted: addresses, lengths,
 * roles and algorithm are checked, never handed directly to memcpy/jump.
 * SHA256 provides corruption detection, not public-key authentication.
 */
#include <common.h>
#include <asm/io.h>
#include <linux/libfdt.h>
#include <u-boot/sha256.h>
#include "idf-init/abi.h"
#include "spl_load.h"

#if CONFIG_SPL_OF_LIBFDT_ASSUME_MASK != 0
#error "Untrusted FIT input requires full libfdt bounds checking"
#endif

/* The 0x34000 ceiling leaves the initial proper stack/GD/malloc area
 * clear. Linux handoff cache synchronization currently covers 4 MiB. */
#include "psram-handoff.h"

#define FLASH_WIN 0x40000000U
#define FIT_OFF   0x000f0000U
#define HDR_LIMIT 0x00010000U
#define CHUNK     0x4000U
struct fit_slot {
	const char *node, *type, *os;
	u32 off, dst, store, limit;
};
static const struct fit_slot slots[] = {
	{ "uboot", "standalone", "u-boot", 0x100000, 0x4ff00000, 0x4ff00000, 0x34000 },
	{ "linux", "kernel", "linux", 0x200000, 0x48000000, 0x88000000, 0x400000 },
	{ "dtb", "flat_dt", "linux", 0x900000, 0x48600000, 0x88600000, 0x10000 },
};
struct payload {
	u32 len;
	const u8 *hash;
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

	if (n < 0 || !STR(f, n, "type", s->type) || !STR(f, n, "os", s->os) ||
	    !STR(f, n, "arch", "riscv") || !STR(f, n, "compression", "none") ||
	    getnum(f, n, "data-position", &off) || off != s->off - FIT_OFF ||
	    getnum(f, n, "data-size", &p->len) || p->len < 64 || p->len > s->limit ||
	    getnum(f, n, "load", &load) || load != s->dst ||
	    (i < 2 && (getnum(f, n, "entry", &entry) || entry != s->dst)) ||
	    fdt_getprop(f, n, "data", NULL) || fdt_getprop(f, n, "data-offset", NULL))
		return -1;
	h = fdt_subnode_offset(f, n, "hash-1");
	if (h < 0 || !STR(f, h, "algo", "sha256"))
		return -1;
	p->hash = fdt_getprop(f, h, "value", &len);
	return p->hash && len == SHA256_SUM_LEN ? 0 : -1;
}

int p4_spl_load_fit(void)
{
	const void *f = (const void *)(FLASH_WIN + FIT_OFF);
	struct payload p[ARRAY_SIZE(slots)];
	int i, conf, images;
	u32 off, n, kspan, psram_size = p4_ext_size();

	if (!psram_size)
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
		if (describe(f, images, i, &p[i]) ||
		    (i && (slots[i].dst - P4_EXT_RAM_BASE > psram_size ||
		     slots[i].limit > psram_size - (slots[i].dst - P4_EXT_RAM_BASE))))
			goto invalid;
	/* All ranges checked before the first copy. Verify the bytes actually
	 * loaded, including PSRAM via its uncached alias. No raw fallback. */
	for (i = 0; i < ARRAY_SIZE(slots); i++) {
		const struct fit_slot *s = &slots[i];
		u8 *mem = (u8 *)(uintptr_t)s->store;
		const u8 *src = (const u8 *)(uintptr_t)(FLASH_WIN + s->off);
		sha256_context ctx;
		u8 sum[SHA256_SUM_LEN];

		sha256_starts(&ctx);
		for (off = 0; off < p[i].len; off += n) {
			n = min(CHUNK, p[i].len - off);
			memcpy(mem + off, src + off, n);
			sha256_update(&ctx, mem + off, n);
			p4_idf_api()->feed();
		}
		sha256_finish(&ctx, sum);
		if (memcmp(sum, p[i].hash, sizeof(sum))) {
			printf("P4 FIT: %s SHA256 mismatch; halted\n", s->node);
			return -1;
		}
		printf("P4 FIT: %s SHA256 OK (%u bytes)\n", s->node, p[i].len);
	}
	/* RISC-V Image.image_size includes its uninitialized tail; FIT stores
	 * only file bytes. Never read fill or the next object as kernel data. */
	if (readl((void *)(slots[1].store + 0x38)) != 0x05435352 ||
	    readl((void *)(slots[1].store + 0x14)) ||
	    fdt_check_header((void *)slots[2].store) ||
	    fdt_totalsize((void *)slots[2].store) != p[2].len)
		goto invalid;
	kspan = readl((void *)(slots[1].store + 0x10));
	if (kspan < p[1].len || kspan > slots[1].limit)
		goto invalid;
	memset((void *)(slots[1].store + p[1].len), 0, kspan - p[1].len);
	((int (*)(u32, u32, u32))0x4fc003f4)(0x10, slots[0].dst, p[0].len);
	((int (*)(u32, u32, u32))0x4fc003f4)(0x20, slots[0].dst, p[0].len);
	asm volatile("fence rw,rw; fence.i" ::: "memory");
	return 0;
invalid:
	puts("P4 FIT: invalid manifest/image; halted\n");
	return -1;
}
