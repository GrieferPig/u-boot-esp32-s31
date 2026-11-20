/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Only AP hex PSRAM HAL objects are reachable; no IDF system/app startup. */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "hal/psram_ctrlr_ll.h"
#include "psram-abi.h"

#if !CONFIG_SPIRAM || CONFIG_SPIRAM_USE_8LINE_MODE || CONFIG_SPIRAM_ECC_ENABLE
#error Expected rev1.x board profile with x16 non-ECC PSRAM
#endif
#if CONFIG_SPIRAM_SPEED_80M
#define MR_DUMMY 8
#elif CONFIG_SPIRAM_SPEED_200M
#define MR_DUMMY 12
#else
#error Unsupported PSRAM frequency
#endif

extern int esp_psram_impl_enable(void);
extern int esp_psram_impl_get_physical_size(uint32_t *size);
extern char _bss_start[], _bss_end[];

void abort(void)
{
	esp_rom_printf("SPL: PSRAM HAL assertion; boot stopped\n");
	for (;;)
		asm volatile("wfi");
}

void __assert_func(const char *file, int line, const char *func, const char *expr)
{
	esp_rom_printf("SPL: PSRAM HAL assert: %s:%d %s: %s\n", file, line, func, expr);
	abort();
}

/* The entire call is an IRQ-off, hart0-only transaction. RTOS locks have
 * no scheduler/state in SPL. Assert the isolation at each imported boundary. */
static void check_exclusive(void)
{
	unsigned long hart, flags;

	asm volatile("csrr %0, mhartid; csrr %1, mstatus" : "=r"(hart), "=r"(flags));
	if (hart || (flags & 8) || !(*(volatile uint32_t *)0x500e60c0u & (1u << 8)))
		abort();
}

BaseType_t xPortEnterCriticalTimeout(portMUX_TYPE *mux, BaseType_t timeout)
{
	(void)mux;
	(void)timeout;
	check_exclusive();
	return 1;
}

void vPortExitCriticalMultiCore(portMUX_TYPE *mux)
{
	(void)mux;
	check_exclusive();
}

BaseType_t xPortInIsrContext(void)
{
	check_exclusive();
	return 0;
}

/* Early vendor messages still use esp_rom_printf. No OS logging backend. */
esp_log_level_t esp_log_default_level = ESP_LOG_INFO;
uint32_t esp_log_timestamp(void) { return 0; }
void esp_log(esp_log_config_t config, const char *tag, const char *format, ...)
{
	(void)config;
	(void)tag;
	(void)format;
}

static uint32_t get_mr(uint32_t address, uint32_t bits)
{
	uint32_t value = 0;

	psram_ctrlr_ll_common_transaction(PSRAM_CTRLR_LL_MSPI_ID_3, 0x4040, 16,
					 address, 32, MR_DUMMY, NULL, 0,
					 (uint8_t *)&value, bits, false);
	return value;
}

static int init(struct p4_psram_info *info)
{
	uint32_t size = 0, flags;
	int rc;

	asm volatile("csrrci %0, mstatus, 8" : "=r"(flags) :: "memory");
	check_exclusive();
	memset(_bss_start, 0, _bss_end - _bss_start);
	rc = esp_psram_impl_enable();
	if (!rc) {
		/* A zero size is reported with the raw density for diagnosis. */
		esp_psram_impl_get_physical_size(&size);
		info->size = size;
		info->mr01 = get_mr(0, 16);
		info->mr23 = get_mr(2, 16);
		info->mr4 = get_mr(4, 8);
		info->mr8 = get_mr(8, 8);
	}
	if (flags & 8)
		asm volatile("csrsi mstatus, 8" ::: "memory");
	return rc;
}

__attribute__((section(".p4_api"),used))
const struct p4_psram_ops p4_psram_api = {
	P4_PSRAM_HAL_MAGIC, CONFIG_SPIRAM_SPEED, init
};
