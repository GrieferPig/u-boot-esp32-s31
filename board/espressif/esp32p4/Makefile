# SPDX-License-Identifier: GPL-2.0+
obj-y += board.o timer.o cache-range.o
obj-$(CONFIG_ESP32P4_NOR_BOOTMETH) += nor-boot.o
obj-$(CONFIG_SPL_BUILD) += spl.o spl_hw.o spl_psram.o spl_nor.o spl_load.o
ifdef CONFIG_SPL_BUILD
obj-$(CONFIG_ESP32P4_SPL_FIT_PAYLOAD) += spl_fit.o
endif
