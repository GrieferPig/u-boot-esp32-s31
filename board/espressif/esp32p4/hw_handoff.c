// SPDX-License-Identifier: GPL-2.0+
/*
 * Hardware-only contract supplied by the firmware (ESP-IDF loader today, SPL
 * later), not a guessed ROM ABI.
 */

#include <cpu_func.h>
#include <hang.h>
#include <stdio.h>
#include <linux/libfdt.h>
#include <asm/global_data.h>

DECLARE_GLOBAL_DATA_PTR;

void esp32p4_hardware_cache_sync(void)
{
	const void *fdt = (const void *)(uintptr_t)gd->arch.firmware_fdt_addr;
	const fdt32_t *entry, *revision;
	int chosen, len;
	unsigned long address;
	int (*sync)(unsigned int start, unsigned int bytes);

	if (!fdt || fdt_check_header(fdt))
		goto bad;
	chosen = fdt_path_offset(fdt, "/chosen");
	if (chosen < 0)
		goto bad;
	revision = fdt_getprop(fdt, chosen, "espressif,chip-revision", &len);
	if (!revision || len != 4 || fdt32_to_cpu(*revision) != 103)
		goto bad;
	entry = fdt_getprop(fdt, chosen, "espressif,cache-sync-entry", &len);
	if (!entry || len != 4)
		goto bad;
	address = fdt32_to_cpu(*entry);
	if (address < 0x4ff00000 || address >= 0x4ffc0000 || (address & 1))
		goto bad;
	sync = (void *)address;
	if (sync(0x48000000, 0x01000000))
		goto bad;
	return;
bad:
	puts("P4_UBOOT: invalid hardware cache handoff\n");
	hang();
}

void flush_dcache_all(void)
{
	esp32p4_hardware_cache_sync();
}

void flush_dcache_range(unsigned long start, unsigned long end)
{
	/* U-Boot owns only this PSRAM window; use the serialized full sync. */
	if (end > start && start < 0x49000000 && end > 0x48000000)
		esp32p4_hardware_cache_sync();
}
