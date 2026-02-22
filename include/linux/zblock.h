/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_ZBLOCK_H
#define _LINUX_ZBLOCK_H

#include <linux/gfp.h>
#include <linux/types.h>

struct zblock_pool;

struct zblock_pool *zblock_create_pool(gfp_t gfp);
void zblock_destroy_pool(struct zblock_pool *pool);
int zblock_alloc(struct zblock_pool *pool, size_t size, gfp_t gfp,
		unsigned long *handle);
void zblock_free(struct zblock_pool *pool, unsigned long handle);
void *zblock_map(struct zblock_pool *pool, unsigned long handle);
void zblock_unmap(struct zblock_pool *pool, unsigned long handle);
u64 zblock_get_total_pages(struct zblock_pool *pool);
size_t zblock_get_max_alloc_size(void);

#endif /* _LINUX_ZBLOCK_H */
