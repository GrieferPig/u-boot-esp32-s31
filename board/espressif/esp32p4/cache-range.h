/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ESP32P4_CACHE_RANGE_H
#define ESP32P4_CACHE_RANGE_H

#include <asm/io.h>
#include <asm/arch-esp32p4/boot-layout.h>
#include "psram-handoff.h"

/*
 * P4 rev1.x cache sync register ABI (ESP-IDF cache_reg.h hw_ver1).
 * Follow cache_ll_writeback_addr()/cache_ll_invalidate_addr(): complete
 * the inner level before the outer one, rather than triggering both
 * with one map. Only touch bounded aligned PSRAM ranges; never sweep
 * the full cache while any stack/data/code is live.
 */
#define P4_SYNC_CTRL		0x3ff10098u
#define P4_SYNC_MAP		0x3ff1009cu
#define P4_SYNC_VADDR		0x3ff100a0u
#define P4_SYNC_BYTES		0x3ff100a4u
#define P4_SYNC_L1I		0x03u
#define P4_SYNC_L1D		0x10u
#define P4_SYNC_L2		0x20u
#define P4_SYNC_WBACK		0x04u
#define P4_SYNC_INVALIDATE	0x01u
#define P4_SYNC_BUSY		0x0fu
#define P4_SYNC_DONE		0x10u

int p4_cache_range(u32 start, u32 bytes, u32 map, u32 op);

static inline int p4_cache_writeback(u32 start, u32 bytes)
{
	return p4_cache_range(start, bytes, P4_SYNC_L1D, P4_SYNC_WBACK) ||
	       p4_cache_range(start, bytes, P4_SYNC_L2, P4_SYNC_WBACK);
}

static inline int p4_cache_discard_data(u32 start, u32 bytes)
{
	return p4_cache_range(start, bytes, P4_SYNC_L1D, P4_SYNC_INVALIDATE) ||
	       p4_cache_range(start, bytes, P4_SYNC_L2, P4_SYNC_INVALIDATE);
}

/* Dirty data must complete writeback before any data invalidation. */
static inline int p4_cache_republish(u32 start, u32 bytes)
{
	return p4_cache_writeback(start, bytes) ||
	       p4_cache_discard_data(start, bytes);
}

#endif
