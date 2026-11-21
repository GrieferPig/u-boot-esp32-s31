/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ESP32P4_PSRAM_HANDOFF_H
#define ESP32P4_PSRAM_HANDOFF_H

#include <linux/compiler.h>

#define P4_EXT_RAM_BASE 0x48000000u
#define P4_EXT_PAGE 0x10000u
#define P4_EXT_MIN (16u << 20) /* retained DTB and Linux fixed image ABI */
#define P4_EXT_MAX (64u << 20) /* 1024 pages; aperture ends at 0x4c000000 */
#define P4_EXT_HANDOFF 0x4ff3c040u /* SPL to U-Boot proper */
#define P4_EXT_MAGIC 0x5034524du

/* SPL and U-Boot proper share the validation record until Linux starts. */
#define P4_FIT_HANDOFF 0x4ff3c080u
#define P4_FIT_MAGIC_XIP 0x50344658u
/* TRM ch.9 System and Memory - MSPI maps NOR into the CPU instruction window. */
#define P4_FLASH_WIN 0x40000000u
#define P4_FIT_IMAGE_OFF 0x200000u
#define P4_FIT_DTB_OFF 0xb00000u
#define P4_FIT_IMAGE (P4_FLASH_WIN + P4_FIT_IMAGE_OFF)
#define P4_FIT_DTB (P4_FLASH_WIN + P4_FIT_DTB_OFF)
#define P4_FIT_IMAGE_FLASH_LIMIT (P4_FIT_DTB_OFF - P4_FIT_IMAGE_OFF)
#define P4_FIT_DTB_LIMIT 0x00010000u
#define P4_FIT_HASH_SIZE 32u

struct p4_fit_handoff {
	unsigned int magic;
	unsigned int image_len;
	unsigned int dtb_len;
	unsigned char image_hash[P4_FIT_HASH_SIZE];
	unsigned char dtb_hash[P4_FIT_HASH_SIZE];
};

static __always_inline int p4_ext_valid_size(unsigned int size)
{
	return size >= P4_EXT_MIN && size <= P4_EXT_MAX &&
	       !(size & (size - 1));
}

static inline unsigned int p4_ext_pmpaddr(unsigned int size)
{
	return (P4_EXT_RAM_BASE >> 2) | ((size >> 3) - 1);
}

static __always_inline unsigned int p4_ext_size(void)
{
	const unsigned int *record = (const void *)P4_EXT_HANDOFF;
	unsigned int size;

	if (READ_ONCE(record[0]) != P4_EXT_MAGIC)
		return 0;
	asm volatile("fence r,r" ::: "memory");
	size = READ_ONCE(record[1]);
	return READ_ONCE(record[2]) == ~size && p4_ext_valid_size(size) ? size : 0;
}

#endif
