/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ESP32P4_SPL_HW_H
#define ESP32P4_SPL_HW_H

#include <linux/types.h>
int p4_spl_hw_init(void);
void p4_spl_wdt_feed(void);
void p4_spl_wdt_finish(void);
void p4_spl_cache_init(void);
void p4_spl_puts(const char *s);
void p4_spl_early_puts(const char *s);
void p4_spl_early_hex(u32 value);
#endif
