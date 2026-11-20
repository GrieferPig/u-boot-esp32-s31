# SPDX-License-Identifier: GPL-2.0-or-later
idf_component_register(SRCS "main.c" PRIV_REQUIRES esp_psram freertos spi_flash)
