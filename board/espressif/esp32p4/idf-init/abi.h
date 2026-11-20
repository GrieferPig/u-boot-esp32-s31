/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ESP32P4_IDF_INIT_ABI_H
#define ESP32P4_IDF_INIT_ABI_H
#define P4_IDF_MAGIC 0x50444931u
struct p4_idf_ops {
	unsigned int magic;
	int (*start)(void);
	void (*finish)(void);
	void (*feed)(void);
	void (*stop_entropy)(void);
};
static inline const struct p4_idf_ops *p4_idf_api(void)
{
	return (const struct p4_idf_ops *)0x4ff70000;
}
#endif
