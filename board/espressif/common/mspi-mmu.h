/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ESPRESSIF_MSPI_MMU_H
#define ESPRESSIF_MSPI_MMU_H

#include <asm/io.h>

static inline void esp_mspi_mmu_write(void *selector, void *content,
				     u32 index, u32 value)
{
	writel(index, selector);
	writel(value, content);
}
#endif
