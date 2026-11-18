/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef __SPL_NOR_H
#define __SPL_NOR_H

int p4_spl_flash_init(u32 size);
int p4_spl_nor_read(u32 offset, void *buf, u32 len);
int p4_spl_nor_probe(void);

#endif
