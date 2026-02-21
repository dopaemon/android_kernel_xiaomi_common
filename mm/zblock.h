/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Author: Vitaly Wool <vitaly.wool@konsulko.com>
 * Copyright (C) 2025, Konsulko AB.
 */
#ifndef __ZBLOCK_H__
#define __ZBLOCK_H__

#include <linux/mm.h>
#include <linux/rbtree.h>
#include <linux/types.h>

#define SLOT_FREE 0
#define BIT_SLOT_OCCUPIED 0
#define BIT_SLOT_MAPPED 1

#if PAGE_SIZE == 0x1000
/* max 128 slots per block, max table size 32 */
#define SLOT_BITS 7
#elif PAGE_SIZE == 0x4000
/* max 256 slots per block, max table size 64 */
#define SLOT_BITS 8
#else
#error Unsupported PAGE_SIZE
#endif

#define MAX_SLOTS (1 << SLOT_BITS)
#define SLOT_MASK ((0x1UL << SLOT_BITS) - 1)

#define ZBLOCK_HEADER_SIZE	round_up(sizeof(struct zblock_block), sizeof(long))
#define BLOCK_DATA_SIZE(order) ((PAGE_SIZE << order) - ZBLOCK_HEADER_SIZE)
#define SLOT_SIZE(nslots, order) (round_down((BLOCK_DATA_SIZE(order) / nslots), sizeof(long)))

/**
 * struct zblock_block - block metadata
 * Block consists of several (1/2/4/8) pages and contains fixed
 * integer number of slots for allocating compressed pages.
 *
 * free_slots:	number of free slots in the block
 * slot_info:	contains data about free/occupied slots
 */
struct zblock_block {
	struct list_head link;
	DECLARE_BITMAP(slot_info, 1 << SLOT_BITS);
	u32 free_slots;
};

/**
 * struct block_desc - general metadata for block lists
 * Each block list stores only blocks of corresponding type which means
 * that all blocks in it have the same number and size of slots.
 * All slots are aligned to size of long.
 *
 * slot_size:		size of slot for this list
 * slots_per_block:	number of slots per block for this list
 * order:		order for __get_free_pages
 */
struct block_desc {
	unsigned int slot_size;
	unsigned short slots_per_block;
	unsigned short order;
};

struct block_desc_node {
	struct rb_node node;
	unsigned int this_slot_size;
	unsigned int next_slot_size;
	unsigned int block_idx;
};

static const struct block_desc block_desc[] = {
#if PAGE_SIZE == 0x1000
	{ SLOT_SIZE(63, 0), 63, 0 },
	{ SLOT_SIZE(32, 0), 32, 0 },
	{ SLOT_SIZE(21, 0), 21, 0 },
	{ SLOT_SIZE(15, 0), 15, 0 },
	{ SLOT_SIZE(12, 0), 12, 0 },
	{ SLOT_SIZE(10, 0), 10, 0 },
	{ SLOT_SIZE(9, 0), 9, 0 },
	{ SLOT_SIZE(8, 0), 8, 0 },
	{ SLOT_SIZE(29, 2), 29, 2 },
	{ SLOT_SIZE(13, 1), 13, 1 },
	{ SLOT_SIZE(6, 0), 6, 0 },
	{ SLOT_SIZE(11, 1), 11, 1 },
	{ SLOT_SIZE(5, 0), 5, 0 },
	{ SLOT_SIZE(9, 1), 9, 1 },
	{ SLOT_SIZE(8, 1), 8, 1 },
	{ SLOT_SIZE(29, 3), 29, 3 },
	{ SLOT_SIZE(13, 2), 13, 2 },
	{ SLOT_SIZE(12, 2), 12, 2 },
	{ SLOT_SIZE(11, 2), 11, 2 },
	{ SLOT_SIZE(10, 2), 10, 2 },
	{ SLOT_SIZE(9, 2), 9, 2 },
	{ SLOT_SIZE(17, 3), 17, 3 },
	{ SLOT_SIZE(8, 2), 8, 2 },
	{ SLOT_SIZE(15, 3), 15, 3 },
	{ SLOT_SIZE(14, 3), 14, 3 },
	{ SLOT_SIZE(13, 3), 13, 3 },
	{ SLOT_SIZE(6, 2), 6, 2 },
	{ SLOT_SIZE(11, 3), 11, 3 },
	{ SLOT_SIZE(10, 3), 10, 3 },
	{ SLOT_SIZE(9, 3), 9, 3 },
	{ SLOT_SIZE(4, 2), 4, 2 },
#elif PAGE_SIZE == 0x4000
	{ SLOT_SIZE(255, 0), 255, 0 },
	{ SLOT_SIZE(185, 0), 185, 0 },
	{ SLOT_SIZE(145, 0), 145, 0 },
	{ SLOT_SIZE(113, 0), 113, 0 },
	{ SLOT_SIZE(92, 0), 92, 0 },
	{ SLOT_SIZE(75, 0), 75, 0 },
	{ SLOT_SIZE(60, 0), 60, 0 },
	{ SLOT_SIZE(51, 0), 51, 0 },
	{ SLOT_SIZE(43, 0), 43, 0 },
	{ SLOT_SIZE(37, 0), 37, 0 },
	{ SLOT_SIZE(32, 0), 32, 0 },
	{ SLOT_SIZE(27, 0), 27, 0 },
	{ SLOT_SIZE(23, 0), 23, 0 },
	{ SLOT_SIZE(19, 0), 19, 0 },
	{ SLOT_SIZE(17, 0), 17, 0 },
	{ SLOT_SIZE(15, 0), 15, 0 },
	{ SLOT_SIZE(13, 0), 13, 0 },
	{ SLOT_SIZE(11, 0), 11, 0 },
	{ SLOT_SIZE(10, 0), 10, 0 },
	{ SLOT_SIZE(9, 0), 9, 0 },
	{ SLOT_SIZE(8, 0), 8, 0 },
	{ SLOT_SIZE(15, 1), 15, 1 },
	{ SLOT_SIZE(14, 1), 14, 1 },
	{ SLOT_SIZE(13, 1), 13, 1 },
	{ SLOT_SIZE(12, 1), 12, 1 },
	{ SLOT_SIZE(11, 1), 11, 1 },
	{ SLOT_SIZE(10, 1), 10, 1 },
	{ SLOT_SIZE(9, 1), 9, 1 },
	{ SLOT_SIZE(8, 1), 8, 1 },
	{ SLOT_SIZE(15, 2), 15, 2 },
	{ SLOT_SIZE(14, 2), 14, 2 },
	{ SLOT_SIZE(13, 2), 13, 2 },
	{ SLOT_SIZE(12, 2), 12, 2 },
	{ SLOT_SIZE(11, 2), 11, 2 },
	{ SLOT_SIZE(10, 2), 10, 2 },
	{ SLOT_SIZE(9, 2), 9, 2 },
	{ SLOT_SIZE(8, 2), 8, 2 },
	{ SLOT_SIZE(7, 2), 7, 2 },
	{ SLOT_SIZE(6, 2), 6, 2 },
	{ SLOT_SIZE(5, 2), 5, 2 },
#endif /* PAGE_SIZE */
};

/**
 * struct block_list - stores metadata of particular list
 * lock:		protects the list of blocks
 * active_list:		linked list of active (non-full) blocks
 * full_list:		linked list of full blocks
 * block_count:		total number of blocks in the list
 */
struct block_list {
	spinlock_t lock;
	struct list_head active_list;
	struct list_head full_list;
	unsigned long block_count;
};

/**
 * struct zblock_pool - stores metadata for each zblock pool
 * @block_lists:	array of block lists
 * @zpool:		zpool driver
 *
 * This structure is allocated at pool creation time and maintains metadata
 * for a particular zblock pool.
 */
struct zblock_pool {
	struct block_list block_lists[ARRAY_SIZE(block_desc)];
	struct zpool *zpool;
};


#endif
