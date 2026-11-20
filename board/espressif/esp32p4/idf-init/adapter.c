/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Integer-only calls across the ESP-IDF ilp32f / U-Boot ilp32 boundary. */
#include "sdkconfig.h"
#include "abi.h"
#include "hal/efuse_ll.h"
#include "hal/wdt_hal.h"
#include "esp_efuse.h"
#include "bootloader_init.h"
#include "bootloader_console.h"
#include "bootloader_random.h"
#include "hal/efuse_hal.h"

#if !CONFIG_ESP32P4_SELECTS_REV_LESS_V3 || CONFIG_SECURE_BOOT || CONFIG_SECURE_FLASH_ENC_ENABLED || CONFIG_EFUSE_VIRTUAL
#error This development raw-payload port is not a secure image verifier
#endif
static int start(void)
{
	/* This raw Linux image flow has no signed-payload verifier. Refuse
	 * protected chips before touching the flash, even with a dev config. */
	if (efuse_ll_get_secure_boot_v2_en() || esp_efuse_is_flash_encryption_enabled())
		return -1;
	if (efuse_hal_chip_revision() != 103)
		return -2;
	return bootloader_init();
}
static void finish(void)
{
	wdt_hal_context_t wdt = RWDT_HAL_CONTEXT_DEFAULT();
	bootloader_random_disable();
	bootloader_console_deinit();
	/* IDF stops the boot RWDT in its app's startup functions, not in
	 * bootloader_init. This is the equivalent ownership boundary. */
#if CONFIG_BOOTLOADER_WDT_ENABLE && !CONFIG_BOOTLOADER_WDT_DISABLE_IN_USER_CODE
	wdt_hal_write_protect_disable(&wdt);
	wdt_hal_disable(&wdt);
	wdt_hal_write_protect_enable(&wdt);
#endif
}
static void feed(void)
{
	wdt_hal_context_t wdt = RWDT_HAL_CONTEXT_DEFAULT();
	wdt_hal_write_protect_disable(&wdt);
	wdt_hal_feed(&wdt);
	wdt_hal_write_protect_enable(&wdt);
}
__attribute__((section(".p4_api"),used))
const struct p4_idf_ops p4_idf_table = { P4_IDF_MAGIC, start, finish, feed };
