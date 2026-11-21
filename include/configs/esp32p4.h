/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Board-specific U-Boot config header for ESP32-P4 (M-mode, no OpenSBI).
 */

#ifndef __ESP32P4_CONFIG_H
#define __ESP32P4_CONFIG_H

#ifdef CONFIG_BOARD_PRELOADED_RUNTIME
#include <asm/arch-esp32p4/boot-layout.h>
#define CFG_SYS_RUNTIME_MALLOC_BASE	P4_RUNTIME_MALLOC
#define CFG_SYS_RUNTIME_MALLOC_END	P4_RUNTIME_MALLOC_END
#endif

/*
 * SPL reads U-Boot proper from the flash XIP window that it maps before
 * loading. TRM ch.9 System and Memory describes the MSPI NOR mapping.
 * The P4 FIT places U-Boot proper at raw NOR offset 0x100000.
 */
#define CFG_SYS_UBOOT_BASE	0x40100000

/*
 * booti allocates below board_get_usable_ram_top() by default, which is where
 * U-Boot itself lives. Keep the boot-time allocations inside PSRAM.
 */
#define CFG_EXTRA_ENV_SETTINGS \
	"bootm_low=0x48000000\0" \
	"bootm_size=0x08000000\0"

#endif /* __ESP32P4_CONFIG_H */
