// SPDX-License-Identifier: GPL-2.0+
/*
 * ESP32-P4 PSRAM bring-up for the SPL.
 *
 * Status: not ported yet. The S31 sequence (LDO -> MPLL -> MSPI controller ->
 * mode regs -> ID/density readout -> timing tuning -> PSRAM MMU) is the model,
 * but the P4 uses MSPI1 with a different register map, a 32 MiB in-package
 * device at 200 MHz, and its own timing-tuning procedure. Until that lands,
 * report failure so the SPL stops instead of jumping into uninitialised PSRAM.
 */

#include <common.h>
#include <asm/io.h>

#include "spl_psram.h"

int p4_spl_psram_init(void)
{
	return -1;
}
