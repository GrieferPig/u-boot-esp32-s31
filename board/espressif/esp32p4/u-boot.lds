/* SPDX-License-Identifier: GPL-2.0+ */

#include "../../../arch/riscv/cpu/u-boot.lds"

/*
 * Keep proper BSS below the early GD and malloc. The 256-byte margin
 * covers generated GD and stack alignment.
 */
ASSERT(__bss_end + 0x100 <= CONFIG_CUSTOM_SYS_INIT_SP_ADDR
       - (1 << CONFIG_STACK_SIZE_SHIFT) - CONFIG_SYS_MALLOC_F_LEN,
       "ESP32-P4 proper BSS overlaps early stack/malloc")
