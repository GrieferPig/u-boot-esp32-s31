/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Board-specific U-Boot config header for ESP32-S31.
 *
 * Copyright (C) 2026, Espressif Systems (Shanghai) CO LTD
 */

#ifndef __ESP32S31_CONFIG_H
#define __ESP32S31_CONFIG_H

/*
 * SPL reads the FIT from the flash XIP window (mapped by the SPL
 * before load; see board/espressif/esp32s31/spl_flash.c). The linear
 * mapping starts at raw flash zero; FIT is at raw offset 0xe000.
 */
#define CFG_SYS_UBOOT_BASE     0x4000e000

#ifdef CONFIG_BOARD_PRELOADED_RUNTIME
/* The S31 FIT preloads the monitor at 0x50400000. SquashFS needs space
 * for the ELF, decompression buffers and filesystem tables in PSRAM.
 */
#define CFG_SYS_RUNTIME_MALLOC_BASE 0x50500000
#define CFG_SYS_RUNTIME_MALLOC_END  0x50710000
#endif

/* Start the Linux handoff allocations above U-Boot's PSRAM footprint. */
#define CFG_EXTRA_ENV_SETTINGS \
	"bootm_low=0x50a00000\0" \
	"bootm_size=0x600000\0"

#endif /* __ESP32S31_CONFIG_H */
