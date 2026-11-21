# SPDX-License-Identifier: GPL-2.0+
obj-y += board.o timer.o cache-range.o
ifdef CONFIG_SPL_BUILD
obj-y += spl.o spl_hw.o spl_psram.o spl_nor.o spl_load.o
obj-$(CONFIG_ESP32P4_SPL_FIT_PAYLOAD) += spl_fit.o
else
obj-$(CONFIG_ESP32P4_NOR_BOOTMETH) += nor-boot.o
endif
