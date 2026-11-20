/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ESPRESSIF_AP_PSRAM_H
#define ESPRESSIF_AP_PSRAM_H

/* AP Hex PSRAM MR2[2:0] density. The controller and board must still
 * validate address aperture, bus mode, supported part and package limits. */
static inline unsigned int esp_ap_psram_density(unsigned int mr2)
{
	static const unsigned int mib[] = { 0, 4, 0, 8, 0, 16, 64, 32 };

	return mib[mr2 & 7] << 20;
}
#endif
