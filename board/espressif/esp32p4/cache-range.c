// SPDX-License-Identifier: GPL-2.0-or-later
#include <common.h>
#include <linux/errno.h>
#include "cache-range.h"

#define P4_MSPI2_AXI_STATUS	0x5008e178u
#define P4_MSPI2_AXI_IDLE	0xfc000000u

static __always_inline int p4_cache_wait(u32 reg, u32 mask, u32 value)
{
	u32 i;

	for (i = 0; i < 1000000; i++)
		if ((readl((void *)reg) & mask) == value)
			return 0;
	return -1;
}

int p4_cache_range(u32 start, u32 bytes, u32 map, u32 op)
{
	u32 end, n;

	if (!bytes)
		return 0;
	if (start < P4_EXT_RAM_BASE ||
	    start - P4_EXT_RAM_BASE >= P4_EXT_MAX ||
	    bytes > P4_EXT_MAX - (start - P4_EXT_RAM_BASE))
		return -1;

	end = ALIGN(start + bytes, P4_L2_LINE_BYTES);
	start = ALIGN_DOWN(start, P4_L2_LINE_BYTES);
	asm volatile("fence iorw,iorw" ::: "memory");
	for (; start < end; start += n) {
		/* No operation crosses a 64 KiB PSRAM MMU page. */
		n = P4_EXT_PAGE - (start & (P4_EXT_PAGE - 1));
		if (n > end - start)
			n = end - start;

		if (p4_cache_wait(P4_SYNC_CTRL, P4_SYNC_BUSY, 0))
			return -1;
		writel(start, (void *)P4_SYNC_VADDR);
		writel(n, (void *)P4_SYNC_BYTES);
		writel(map, (void *)P4_SYNC_MAP);
		/* Clear RGID, use one self-clearing operation only. */
		writel(op, (void *)P4_SYNC_CTRL);
		if (p4_cache_wait(P4_SYNC_CTRL, P4_SYNC_BUSY | P4_SYNC_DONE,
				  P4_SYNC_DONE))
			return -1;
#ifdef CONFIG_SPL_BUILD
		/*
		 * CACHE_SYNC_DONE alone does not establish that the MSPI
		 * write data reached PSRAM. SPL has exclusive use of MSPI2
		 * while the second hart is in reset.
		 */
		if (map == P4_SYNC_L2 && op == P4_SYNC_WBACK) {
			asm volatile("fence iorw,iorw" ::: "memory");
			if (p4_cache_wait(P4_MSPI2_AXI_STATUS, P4_MSPI2_AXI_IDLE,
					  P4_MSPI2_AXI_IDLE))
				return -ETIMEDOUT;
		}
#endif
	}
	asm volatile("fence iorw,iorw" ::: "memory");
	return 0;
}
