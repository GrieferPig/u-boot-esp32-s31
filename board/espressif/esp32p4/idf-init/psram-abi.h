/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ESP32P4_PSRAM_ABI_H
#define ESP32P4_PSRAM_ABI_H

#define P4_PSRAM_HAL_ADDR  0x4ff74000u
#define P4_PSRAM_HAL_MAGIC 0x50535231u

struct p4_psram_info {
	unsigned int size;
	unsigned int mr01;
	unsigned int mr23;
	unsigned int mr4;
	unsigned int mr8;
};

struct p4_psram_ops {
	unsigned int magic;
	unsigned int clock_mhz;
	int (*init)(struct p4_psram_info *info);
};

#endif
