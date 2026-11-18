/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Board-specific U-Boot config header for ESP32-P4 (M-mode, no OpenSBI).
 */

#ifndef __ESP32P4_CONFIG_H
#define __ESP32P4_CONFIG_H

/*
 * SPL reads U-Boot proper from the flash XIP window that it maps before
 * loading (see board/espressif/esp32p4/spl_flash.c). U-Boot proper is flashed
 * at raw offset 0x10000, one megabyte window starting at raw flash zero.
 */
#define CFG_SYS_UBOOT_BASE	0x40010000

/* The SPL reads the 16 MHz SYSTIMER as its legacy timebase. */
#define CFG_SYS_TIMER_RATE	16000000

/*
 * booti allocates below board_get_usable_ram_top() by default, which is where
 * U-Boot itself lives. Keep the boot-time allocations inside PSRAM.
 */
#define CFG_EXTRA_ENV_SETTINGS \
	"bootm_low=0x48000000\0" \
	"bootm_size=0x08000000\0"

#endif /* __ESP32P4_CONFIG_H */
