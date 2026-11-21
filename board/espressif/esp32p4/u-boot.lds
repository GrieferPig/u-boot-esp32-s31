/* SPDX-License-Identifier: GPL-2.0+ */

#include "../../../arch/riscv/cpu/u-boot.lds"

/*
 * Keep proper BSS below the early GD and malloc. The 256-byte margin
 * covers generated GD and alignment, in addition to the 16 KiB stack.
 */
ASSERT(__bss_end + 0x100 <= CONFIG_CUSTOM_SYS_INIT_SP_ADDR
       - 0x4000 - CONFIG_SYS_MALLOC_F_LEN,
       "ESP32-P4 proper BSS overlaps early stack/malloc")
