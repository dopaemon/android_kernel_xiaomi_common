#include "spadfs.h"

#define MAX_CACHED_APAGE_BUFFERS	32

#define DECREASE_FREESPACE	0
#define INCREASE_FREESPACE	1

/* Helper function for group_action */

static int group_action_1(SPADFS *fs, unsigned group, unsigned len, int action)
{
	if (likely(action == INCREASE_FREESPACE)) {
		if (unlikely(fs->group_info[group].freespace + len < len) ||
		    unlikely(fs->group_info[group].freespace + len > fs->group_mask + 1)) {
			spadfs_error(fs, TXFLAGS_FS_ERROR,
				"allocation count for group %u overflow: %Lx, %Lx, %x",
				group,
				(unsigned long long)fs->group_info[group].freespace,
				(unsigned long long)fs->group_info[group].zone->freespace,
				len);
			return -EFSERROR;
		}
		fs->group_info[group].freespace += len;
		fs->group_info[group].zone->freespace += len;
	} else {
		if (unlikely(fs->group_info[group].freespace < len) ||
		    unlikely(fs->group_info[group].zone->freespace < len)) {
			spadfs_error(fs, TXFLAGS_FS_ERROR,
				"allocation count for group %u underflow: %Lx, %Lx, %x",
				group,
				(unsigned long long)fs->group_info[group].freespace,
				(unsigned long long)fs->group_info[group].zone->freespace,
				len);
			return -EFSERROR;
		}
		fs->group_info[group].freespace -= len;
		fs->group_info[group].zone->freespace -= len;
	}
	return 0;
}

/*
 * Action is either DECREASE_FREESPACE or INCREASE_FREESPACE.
 * This function will update in-memory group statistics (and handle correctly
 * situations like alloc or free crossing multiple groups).
 */

static int group_action(SPADFS *fs, sector_t start, unsigned len, int action)
{
	unsigned start_group, end_group;
	start_group = start >> fs->sectors_per_group_bits;
	end_group = (start + (len - 1)) >> fs->sectors_per_group_bits;
	if (unlikely(end_group >= fs->n_groups)) {
range_error:
		spadfs_error(fs, TXFLAGS_FS_ERROR,
			"invalid range in group action (%Lx,%x)",
			(unsigned long long)start, len);
		return -EFSERROR;
	}
	if (unlikely(start_group > end_group)) {
		goto range_error;
	}
	if (likely(start_group == end_group)) {
		return group_action_1(fs, start_group, len, action);
	} else {
		int r = group_action_1(fs, start_group,
				fs->group_mask + 1 - (start & fs->group_mask),
				action);
		if (unlikely(r))
			return r;
		for (start_group++; start_group < end_group; start_group++) {
			r = group_action_1(fs, start_group,
				fs->group_mask + 1, action);
			if (unlikely(r))
				return r;
		}
		r = group_action_1(fs, start_group,
			((start + len - 1) & fs->group_mask) + 1, action);
		return r;
	}
}

static void freespace_decrease(SPADFS *fs, sector_t sector, unsigned n_sectors)
{
	fs->freespace -= n_sectors;
	if (unlikely(fs->max_allocation > fs->freespace))
		fs->max_allocation = fs->freespace;
	group_action(fs, sector, n_sectors, DECREASE_FREESPACE);
}

static void freespace_increase(SPADFS *fs, sector_t sector, unsigned n_sectors)
{
	if (unlikely(!fs->max_allocation))
		fs->max_allocation = 1U << fs->sectors_per_disk_block_bits;
	fs->freespace += n_sectors;
	group_action(fs, sector, n_sectors, INCREASE_FREESPACE);
}

static void spadfs_discard_reservation_unlocked(SPADFS *fs, struct alloc_reservation *res)
{
	res->len = 0;
	rb_erase(&res->rb_node, &fs->alloc_reservations);
}

void spadfs_discard_reservation(SPADFS *fs, struct alloc_reservation *res)
{
	if (res->len) {
		mutex_lock(&fs->alloc_lock);
		if (likely(res->len != 0))
			spadfs_discard_reservation_unlocked(fs, res);
		mutex_unlock(&fs->alloc_lock);
	}
}

static int spadfs_discard_all_reservations(SPADFS *fs)
{
	struct rb_node *p;
	if (!(p = fs->alloc_reservations.rb_node))
		return 0;
	do {
#define res	rb_entry(p, struct alloc_reservation, rb_node)
		res->len = 0;
		rb_erase(&res->rb_node, &fs->alloc_reservations);
#undef res
	} while ((p = fs->alloc_reservations.rb_node));
	return 1;
}

static int spadfs_check_reservations(SPADFS *fs, sector_t start, unsigned n_sec, sector_t *new_free)
{
	struct rb_node *p = fs->alloc_reservations.rb_node;
	while (p) {
#define res	rb_entry(p, struct alloc_reservation, rb_node)
		if (start + n_sec <= res->start) {
			p = res->rb_node.rb_left;
		} else if (likely(start >= res->start + res->len)) {
			p = res->rb_node.rb_right;
		} else {
			*new_free = res->start + res->len;
			return 1;
		}
#undef res
	}
	return 0;
}

static void spadfs_add_reservation(SPADFS *fs, struct alloc_reservation *new)
{
	struct rb_node **p = &fs->alloc_reservations.rb_node;
	struct rb_node *parent = NULL;
	while (*p) {
		parent = *p;
#define res	rb_entry(parent, struct alloc_reservation, rb_node)
		if (new->start + new->len <= res->start) {
			p = &res->rb_node.rb_left;
		} else if (likely(new->start >= res->start + res->len)) {
			p = &res->rb_node.rb_right;
		} else {
			printk(KERN_EMERG "spadfs: new reservation %Lx,%Lx overlaps with %Lx,%Lx\n",
				(unsigned long long)new->start,
				(unsigned long long)(new->start + new->len),
				(unsigned long long)res->start,
				(unsigned long long)(res->start + res->len));
			BUG();
		}
#undef res
	}
	rb_link_node(&new->rb_node, parent, p);
	rb_insert_color(&new->rb_node, &fs->alloc_reservations);
}

/*
 * Convert pointer to mapped apage to index of an apage on filesystem
 * --- for error prints
 */

static int map_2_apage_n(SPADFS *fs, APAGE_MAP *map)
{
	unsigned i, j;
	if (map == fs->tmp_map)
		return -1;
	for (i = 0; i < fs->n_apages; i++)
		for (j = 0; j < 2; j++)
			if (fs->apage_info[i].mapping[j].map == map)
				return i;
	panic("spadfs: map_2_apage_n: invalid pointer %p", map);
}

/* Get 16-byte entry in mapped apage */

static struct aentry *get_aentry(SPADFS *fs, APAGE_MAP *map, unsigned off)
{
	if (unlikely(off & (sizeof(struct aentry) - 1))) {
		spadfs_error(fs, TXFLAGS_FS_ERROR,
			"unaligned apage entry %u, apage %d",
			off, map_2_apage_n(fs, map));
		return ERR_PTR(-EFSERROR);
	}
	if (unlikely(off >= fs->apage_size)) {
		spadfs_error(fs, TXFLAGS_FS_ERROR,
			"apage entry %u too big, apage %d",
			off, map_2_apage_n(fs, map));
		return ERR_PTR(-EFSERROR);
	}
	return (struct aentry *)((char *)map[off >> (9 + fs->sectors_per_buffer_bits)].entry + (off & ((512U << fs->sectors_per_buffer_bits) - 1)));
}

/* Get 16-byte entry in mapped apage assuming that the index is valid */

static struct aentry *get_aentry_valid(SPADFS *fs, APAGE_MAP *map, unsigned off)
{
#ifdef CONFIG_DEBUG_LIST
	if (unlikely(off & (sizeof(struct aentry) - 1))
	 || unlikely(off >= fs->apage_size)) {
		panic("spadfs: get_aentry_valid: invalid offset %u", off);
	}
#endif
	return (struct aentry *)((char *)map[off >> (9 + fs->sectors_per_buffer_bits)].entry + (off & ((512U << fs->sectors_per_buffer_bits) - 1)));
}

/* Get first 16-byte entry of an apage --- its head */

static struct apage_head *get_head(APAGE_MAP *map)
{
	return (struct apage_head *)map[0].entry;
}

static void internal_free_apage_buffers(SPADFS *fs, struct apage_mapping *mapping)
{
	unsigned n;
	APAGE_MAP *map;

	list_del(&mapping->list);
	BUG_ON(!fs->cached_apage_buffers);
	fs->cached_apage_buffers--;

	n = fs->n_apage_mappings;
	map = mapping->map;
	while (n--) {
		map->entry = NULL;
		if (likely(map->bh != NULL))
			spadfs_brelse(fs, map->bh);
		map++;
	}
}

static void spadfs_prune_cached_apage_buffer(SPADFS *fs)
{
	struct apage_mapping *mapping;

	BUG_ON(list_empty(&fs->apage_lru));

	mapping = list_entry(fs->apage_lru.next, struct apage_mapping, list);

	internal_free_apage_buffers(fs, mapping);
}

void spadfs_prune_cached_apage_buffers(SPADFS *fs)
{
	BUG_ON(fs->mapped_apage_buffers);
	while (!list_empty(&fs->apage_lru)) {
		spadfs_prune_cached_apage_buffer(fs);
	}
	BUG_ON(fs->cached_apage_buffers);
	BUG_ON(!list_empty(&fs->apage_lru));
}

static void free_apage_buffers(SPADFS *fs, APAGE_MAP *map)
{
	BUG_ON(!fs->mapped_apage_buffers);
	fs->mapped_apage_buffers--;
	if (!fs->mapped_apage_buffers) {
		while (unlikely(fs->cached_apage_buffers > MAX_CACHED_APAGE_BUFFERS))
			spadfs_prune_cached_apage_buffer(fs);
	}
}

static noinline int internal_read_apage_buffers(SPADFS *fs, struct apage_mapping *mapping, sector_t sec)
{
	unsigned i;
	fs->cached_apage_buffers++;
	list_add_tail(&mapping->list, &fs->apage_lru);
	mapping->map[0].entry = spadfs_read_sector(fs, sec, &mapping->map[0].bh, fs->n_apage_mappings << fs->sectors_per_buffer_bits, "internal_read_apage_buffers 1");
	if (unlikely(IS_ERR(mapping->map[0].entry))) {
		i = 0;
		goto error;
	}
	for (i = 1; i < fs->n_apage_mappings; i++) {
		sec += 1U << fs->sectors_per_buffer_bits;
		mapping->map[i].entry = spadfs_read_sector(fs, sec, &mapping->map[i].bh, 0, "internal_read_apage_buffers 2");
		if (unlikely(IS_ERR(mapping->map[i].entry))) {
			int r;
error:
			r = PTR_ERR(mapping->map[i].entry);
			internal_free_apage_buffers(fs, mapping);
			fs->mapped_apage_buffers--;
			return r;
		}
	}
	return 0;
}

static int read_apage_buffers(SPADFS *fs, struct apage_mapping *mapping, sector_t sec)
{
	fs->mapped_apage_buffers++;
	if (likely(mapping->map[0].entry != NULL)) {
		list_del(&mapping->list);
		list_add_tail(&mapping->list, &fs->apage_lru);
		return 0;
	}
	return internal_read_apage_buffers(fs, mapping, sec);
}

/* Copy one mapped apage to another */

static void copy_apage(SPADFS *fs, APAGE_MAP *dst, APAGE_MAP *src)
{
	unsigned i;
	unsigned n = fs->n_apage_mappings;
	unsigned size = 512U << fs->sectors_per_buffer_bits;
	const unsigned offset = sizeof(struct apage_head) - sizeof(struct apage_subhead);
	if (unlikely(fs->apage_size < size))
		size = fs->apage_size;
	memcpy((char *)dst[0].entry + offset, (char *)src[0].entry + offset, size - offset);
	for (i = 1; i < n; i++)
		memcpy(dst[i].entry, src[i].entry, size);
}

static void mark_apage_dirty(SPADFS *fs, APAGE_MAP *map)
{
	unsigned i;
	for (i = 0; i < fs->n_apage_mappings; i++)
		mark_buffer_dirty(map[i].bh);
}

#define invalid_flags(fs, flags)					\
	unlikely(((flags) & (APAGE_SIZE_BITS | APAGE_BLOCKSIZE_BITS)) !=\
	((((fs)->sectors_per_page_bits - 1) << APAGE_SIZE_BITS_SHIFT) |	\
	((fs)->sectors_per_disk_block_bits << APAGE_BLOCKSIZE_BITS_SHIFT)))

/*
 * Map existing apage apage
 * ap is index of an apage
 *  flags is one of
 *	MAP_READ --- map apage for read
 *	MAP_NEW --- map yet unused apage
 *	MAP_WRITE --- map apage for write (i.e. fixup cc/txc and copy its
 *		content if its the first map since last sync)
 *	MAP_ALLOC --- like MAP_WRITE but skip completely full apages
 *		(an optimization)
 */

#define MAP_READ	0
#define MAP_NEW		1
#define MAP_WRITE	2
#define MAP_ALLOC	3

static APAGE_MAP *map_apage(SPADFS *fs, unsigned ap, int flags,
			    APAGE_MAP **other)
{
	struct apage_info *info;
	unsigned idx;
	int r;
	sector_t sec;
	struct apage_head *a;
	struct apage_head *aa;
	sec = SPAD2CPU64_LV(&fs->apage_index[ap].apage);
	if (unlikely((unsigned)sec & ((1U << fs->sectors_per_page_bits) - 1))) {
		spadfs_error(fs, TXFLAGS_FS_ERROR,
			"misaligned apage %u at block %Lx",
			ap, (unsigned long long)sec);
		return ERR_PTR(-EFSERROR);
	}
	info = &fs->apage_info[ap];
	if (unlikely(r = read_apage_buffers(fs, &info->mapping[0], sec))) {
		return ERR_PTR(r);
	}
	if (unlikely(r = read_apage_buffers(fs, &info->mapping[1],
			       sec + (1U << (fs->sectors_per_page_bits - 1))))) {
		free_apage_buffers(fs, info->mapping[0].map);
		return ERR_PTR(r);
	}

	a = get_head(info->mapping[0].map);
	if (unlikely(a->magic != CPU2SPAD16_CONST(APAGE_MAGIC))) {
		spadfs_error(fs, TXFLAGS_FS_ERROR, "bad magic on apage %Lx",
			(unsigned long long)sec);
		r = -EFSERROR;
		goto ret_err;
	}

	if (unlikely(flags == MAP_NEW)) {
		start_atomic_buffer_modify(fs, info->mapping[0].map[0].bh);
		CC_SET_CURRENT_INVALID(fs, &a->cc, &a->txc);
		end_atomic_buffer_modify(fs, info->mapping[0].map[0].bh);
	}

	idx = !CC_VALID(fs, &a->cc, &a->txc);

	if (unlikely(flags == MAP_NEW)) {
		goto read_ok;
	}

	if (invalid_flags(fs, get_head(info->mapping[idx].map)->s.u.l.flags)) {
		spadfs_error(fs, TXFLAGS_FS_ERROR,
			"apage %Lx has invalid flags %02x",
			(unsigned long long)sec,
			get_head(info->mapping[idx].map)->s.u.l.flags);
		r = -EFSERROR;
		goto ret_err;
	}

	if (unlikely(flags == MAP_READ)) {
read_ok:
		if (likely(other != NULL))
			*other = info->mapping[idx ^ 1].map;
		else
			free_apage_buffers(fs, info->mapping[idx ^ 1].map);

		return info->mapping[idx].map;
	}
	if (likely(flags == MAP_ALLOC)) {
		aa = get_head(info->mapping[idx].map);
		if (unlikely(!(aa->s.u.l.flags & APAGE_BITMAP)) &&
		    unlikely(aa->s.u.l.last == CPU2SPAD16_CONST(0))) {
			r = -ENOSPC;
			goto ret_err;
		}
	}
	if (likely(CC_CURRENT(fs, &a->cc, &a->txc))) {
		if (invalid_flags(fs, get_head(info->mapping[idx ^ 1].map)->s.u.l.flags)) {
			spadfs_error(fs, TXFLAGS_FS_ERROR,
				"apage %Lx has invalid flags %02x in inactive part",
				(unsigned long long)sec,
				get_head(info->mapping[idx ^ 1].map)->s.u.l.flags);
			r = -EFSERROR;
			goto ret_err;
		}
		goto read_ok;
	}
	start_atomic_buffer_modify(fs, info->mapping[0].map[0].bh);
	CC_SET_CURRENT(fs, &a->cc, &a->txc);
	end_atomic_buffer_modify(fs, info->mapping[0].map[0].bh);

	copy_apage(fs, info->mapping[idx ^ 1].map, info->mapping[idx].map);
	mark_apage_dirty(fs, info->mapping[idx ^ 1].map);
	idx ^= 1;
	goto read_ok;

ret_err:
	free_apage_buffers(fs, info->mapping[0].map);
	free_apage_buffers(fs, info->mapping[1].map);
	return ERR_PTR(r);
}

/*
 * Unmap existing apage --- a companion to map_apage
 * If the apage has been modified, "modified" argument must be nonzero
 */

static void unmap_apage(SPADFS *fs, APAGE_MAP *map, int modified)
{
	BUG_ON(!atomic_read(&map[0].bh->b_count));
	if (modified) {
		get_head(map)->s.u.l.flags &= ~APAGE_CHECKSUM_VALID;
		mark_apage_dirty(fs, map);
	}
	free_apage_buffers(fs, map);
}

/*
 * Create apage that has all entries free (i.e. it doesn't specify any free
 * space yet)
 */

__cold static void make_apage(SPADFS *fs, APAGE_MAP *map)
{
	unsigned i;
	struct apage_head *head;
	struct aentry *ae;
	head = get_head(map);
	head->s.u.l.flags =
		((fs->sectors_per_page_bits - 1) << APAGE_SIZE_BITS_SHIFT) |
		(fs->sectors_per_disk_block_bits << APAGE_BLOCKSIZE_BITS_SHIFT);
	head->s.u.l.checksum = 0;
	CPU2SPAD16_LV(&head->s.u.l.freelist, sizeof(struct aentry));
	CPU2SPAD16_LV(&head->s.u.l.last, 0);
	CPU2SPAD16_LV(&head->s.u.l.first, 0);
	i = sizeof(struct aentry);
	do {
		ae = get_aentry_valid(fs, map, i);
		CPU2SPAD64_LV(&ae->start, 0);
		CPU2SPAD32_LV(&ae->len, 0);
		CPU2SPAD16_LV(&ae->prev, 0);
		i += sizeof(struct aentry);
		CPU2SPAD16_LV(&ae->next, i);
	} while (i < fs->apage_size);
	CPU2SPAD16_LV(&ae->next, 0);
}

/* Create bitmap apage that has all bits marked as "allocated" */

__cold static void make_apage_bitmap(SPADFS *fs, APAGE_MAP *map, sector_t start)
{
	unsigned i;
	struct apage_head *head;
	struct aentry *ae;
	head = get_head(map);
	head->s.u.b.flags =
		((fs->sectors_per_page_bits - 1) << APAGE_SIZE_BITS_SHIFT) |
		(fs->sectors_per_disk_block_bits << APAGE_BLOCKSIZE_BITS_SHIFT) |
		APAGE_BITMAP;
	head->s.u.b.checksum = 0;
	head->s.u.b.start1 = MAKE_PART_1(start);
	head->s.u.b.start0 = MAKE_PART_0(start);
	i = sizeof(struct aentry);
	do {
		ae = get_aentry_valid(fs, map, i);
		memset(ae, 0xff, sizeof(struct aentry));
		i += sizeof(struct aentry);
	} while (i < fs->apage_size);
}

/*
 * Helper for spadfs_count_free_space
 * Count free space in one apage
 */

static int get_map_stats(SPADFS *fs, APAGE_MAP *map, sector_t *freespace)
{
	struct apage_head *head = get_head(map);
	if (likely(!(head->s.u.l.flags & APAGE_BITMAP))) {
		unsigned p = SPAD2CPU16_LV(&head->s.u.l.first);
		unsigned max_loop = (512 / sizeof(struct aentry) / 2) <<
					fs->sectors_per_page_bits;
		while (p) {
			struct aentry *aentry = get_aentry(fs, map, p);
			if (unlikely(IS_ERR(aentry)))
				return PTR_ERR(aentry);
			if (unlikely(group_action(fs,
						SPAD2CPU64_LV(&aentry->start),
						SPAD2CPU32_LV(&aentry->len),
						INCREASE_FREESPACE)))
				return -EFSERROR;
			*freespace += SPAD2CPU32_LV(&aentry->len);
			p = SPAD2CPU16_LV(&aentry->next);
			if (unlikely(!--max_loop)) {
				spadfs_error(fs, TXFLAGS_FS_ERROR,
					"infinite loop in apage %d in get_map_stats",
					map_2_apage_n(fs, map));
				return -EFSERROR;
			}
		}
		return 0;
	} else {
		unsigned p;
		sector_t bst = MAKE_D_OFF(head->s.u.b.start0, head->s.u.b.start1);
		for (p = sizeof(struct aentry); p < fs->apage_size;
		     p += sizeof(struct aentry)) {
			unsigned i;
			u8 *bm = (u8 *)get_aentry(fs, map, p);
			if (unlikely(IS_ERR(bm)))
				return PTR_ERR(bm);
			for (i = 0; i < sizeof(struct aentry) * 8; i++) {
				if (BITMAP_TEST_32_FULL(bm - sizeof(struct apage_head), i)) {
					i += 31;
					continue;
				}
				if (!BITMAP_TEST(bm - sizeof(struct apage_head), i)) {
					*freespace += 1U << fs->sectors_per_disk_block_bits;
					if (unlikely(group_action(fs,
							bst + ((i + (p - sizeof(struct aentry)) * 8) << fs->sectors_per_disk_block_bits),
							1U << fs->sectors_per_disk_block_bits,
							INCREASE_FREESPACE)))
						return -EFSERROR;
				}
			}
		}
		return 0;
	}
}

/* Counts free space during mount */

int spadfs_count_free_space(SPADFS *fs)
{
	unsigned i;
	sector_t freespace = 0;
	for (i = 0; i < fs->n_active_apages; i++) {
		int r;
		APAGE_MAP *map;
		if (unlikely(IS_ERR(map = map_apage(fs, i, MAP_READ, NULL))))
			return PTR_ERR(map);
		r = get_map_stats(fs, map, &freespace);
		unmap_apage(fs, map, 0);
		if (unlikely(r)) return r;
	}
	fs->freespace = freespace;
	fs->max_allocation = freespace;
	if (unlikely(freespace != fs->max_allocation))
		fs->max_allocation = -(1U << fs->sectors_per_disk_block_bits);

	freespace = 0;
	for (i = 0; i < fs->n_groups; i++) {
		freespace += fs->group_info[i].freespace;
	}
	if (unlikely(freespace != fs->freespace)) {
		spadfs_error(fs, TXFLAGS_FS_ERROR,
			"free space in groups miscounted, %Lx != %Lx",
			(unsigned long long)freespace,
			(unsigned long long)fs->freespace);
		return -EFSERROR;
	}

	freespace = 0;
	for (i = 0; i < 3; i++) {
		freespace += fs->zones[i].freespace;
	}
	if (unlikely(freespace != fs->freespace)) {
		spadfs_error(fs, TXFLAGS_FS_ERROR,
			"free space in zones miscounted, %Lx != %Lx",
			(unsigned long long)freespace,
			(unsigned long long)fs->freespace);
		return -EFSERROR;
	}

	return 0;
}

/*
 * Find the last apage entry mapping area before the specified block
 * Uses algorithm with average O(sqrt(n)) complexity:
 *	Scan any sqrt(n) blocks (the blocks are selected deliberately in
 *		64-byte chunks (4 entries, each 16 bytes) to minimize cache
 *		pollution)
 *	From scanned blocks, select the one with highest start that is lower
 *		than "before"
 *	From the selected block, scan forward using double-linked list pointers
 *		(on average we scan sqrt(n) apage entries in this pass)
 */

static int find_block_before(SPADFS *fs, APAGE_MAP *map, sector_t before)
{
	unsigned i;
	unsigned rlimit;
	sector_t xst = 0;
	unsigned st = 0, nx;
	struct aentry *sta = (struct aentry *)get_head(map), *nxa;
	for (i = 4 * sizeof(struct aentry);
	     i <= fs->apage_size - 4 * sizeof(struct aentry);
	     i += 124 * sizeof(struct aentry)) {
		unsigned j;
		for (j = 0; j < 4; j++) {
			struct aentry *a = get_aentry_valid(fs, map, i);
			if (unlikely(IS_ERR(a)))
				return PTR_ERR(a);
			if (a->len != CPU2SPAD32_CONST(0) &&
			    SPAD2CPU64_LV(&a->start) <= before &&
			    unlikely(SPAD2CPU64_LV(&a->start) >= xst)) {
				xst = SPAD2CPU64_LV(&a->start);
				st = i;
				sta = a;
			}
			i += sizeof(struct aentry);
		}
	}
	rlimit = fs->apage_size;
	do {
		nx = SPAD2CPU16_LV(&sta->next);
		nxa = get_aentry(fs, map, nx);
		if (unlikely(IS_ERR(nxa)))
			return PTR_ERR(nxa);
		if (unlikely(!nx) ||
		    SPAD2CPU64_LV(&nxa->start) > before)
			return st;
		st = nx;
		sta = nxa;
	} while (likely(rlimit -= sizeof(struct aentry)));
	spadfs_error(fs, TXFLAGS_FS_ERROR,
		"infinite loop in apage %d in find_block_before",
		map_2_apage_n(fs, map));
	return -EFSERROR;
}

/* Simple operations with bits in bitmaps */

static int bmp_test(SPADFS *fs, APAGE_MAP *map, unsigned off)
{
	u32 *p = (u32 *)get_aentry_valid(fs, map,
				(1 + off / (sizeof(struct aentry) * 8)) *
				sizeof(struct aentry));
	p += (off % (sizeof(struct aentry) * 8)) >> 5;
	return (SPAD2CPU32_LV(p) >> (off & 31)) & 1;
}

static void bmp_set(SPADFS *fs, APAGE_MAP *map, unsigned off)
{
	u32 *p = (u32 *)get_aentry_valid(fs, map,
				(1 + off / (sizeof(struct aentry) * 8)) *
				sizeof(struct aentry));
	p += (off % (sizeof(struct aentry) * 8)) >> 5;
	CPU2SPAD32_LV(p, SPAD2CPU32_LV(p) | (1U << (off & 31)));
}

static void bmp_clear(SPADFS *fs, APAGE_MAP *map, unsigned off)
{
	u32 *p = (u32 *)get_aentry_valid(fs, map,
				(1 + off / (sizeof(struct aentry) * 8)) *
				sizeof(struct aentry));
	p += (off % (sizeof(struct aentry) * 8)) >> 5;
	CPU2SPAD32_LV(p, SPAD2CPU32_LV(p) & ~(1U << (off & 31)));
}

static int bmp_test_32_full(SPADFS *fs, APAGE_MAP *map, unsigned off)
{
	u32 *p = (u32 *)get_aentry_valid(fs, map,
				(1 + off / (sizeof(struct aentry) * 8)) *
				sizeof(struct aentry));
	p += (off % (sizeof(struct aentry) * 8)) >> 5;
	return *p == CPU2SPAD32(0xffffffffu);
}

/* Helper for check_other_map */

static int check_other_map_bmp(SPADFS *fs, APAGE_MAP *map, sector_t off,
			       unsigned n_sec, sector_t *next_free)
{
	struct apage_head *head = get_head(map);
	unsigned Xboff = BITMAP_OFFSET(head, off);
	unsigned Xn_sec = BITMAP_LEN(head, n_sec);
	if (unlikely(Xboff + Xn_sec <= Xboff) ||
	    unlikely(Xboff + Xn_sec > BITMAP_SIZE(fs->apage_size))) {
		spadfs_error(fs, TXFLAGS_FS_ERROR,
			"bitmap error, apage %d, Xboff %x, off %Lx, n_sec %x",
			map_2_apage_n(fs, map), Xboff, (unsigned long long)off,
			n_sec);
		return -EFSERROR;
	}
	do {
		if (unlikely(bmp_test(fs, map, Xboff))) {
			while (++Xboff < BITMAP_SIZE(fs->apage_size))
				if (!bmp_test(fs, map, Xboff)) {
					*next_free = MAKE_D_OFF(head->s.u.b.start0, head->s.u.b.start1) +
						    (Xboff << fs->sectors_per_disk_block_bits);
					return -EBUSY;
				}
			return -EBUSY;
		}
		Xboff++;
	} while (--Xn_sec);
	return 0;
}

/*
 * When we are allocating space, we must make sure that we don't allocate space
 * that is free but that would be valid in case of crash --- this functions
 * checks it
 */

static int check_other_map(SPADFS *fs, APAGE_MAP *map, sector_t off,
			   unsigned n_sec, sector_t *next_free)
{
	if (unlikely(!map))
		return 0;
	*next_free = 0;
	if (likely(!(get_head(map)->s.u.l.flags & APAGE_BITMAP))) {
		int preblk, postblk;
		struct aentry *e;
		preblk = find_block_before(fs, map, off);
		if (unlikely(preblk < 0))
			return preblk;
		e = get_aentry_valid(fs, map, preblk);
		if (likely(preblk != 0)) {
			if (off + n_sec <=
			    SPAD2CPU64_LV(&e->start) + SPAD2CPU32_LV(&e->len))
				return 0;
		}
		postblk = SPAD2CPU16_LV(&e->next);
		if (likely(postblk != 0)) {
			e = get_aentry(fs, map, postblk);
			if (unlikely(IS_ERR(e)))
				return PTR_ERR(e);
			*next_free = SPAD2CPU64_LV(&e->start);
		}
		return -EBUSY;
	} else
		return check_other_map_bmp(fs, map, off, n_sec, next_free);
}

static int check_conflict(SPADFS *fs, APAGE_MAP *other, sector_t start, unsigned n_sec, sector_t *new_free)
{
	if ((other && unlikely(check_other_map(fs, other, start, n_sec, new_free))) ||
	    unlikely(spadfs_allocmem_find(fs, start, n_sec, new_free)) ||
	    unlikely(spadfs_check_reservations(fs, start, n_sec, new_free)))
		return 1;
	return 0;
}

#ifdef SPADFS_RESURRECT
static int check_resurrect_map(SPADFS *fs, APAGE_MAP *map, sector_t off,
			       unsigned n_sec)
{
	sector_t next_free_sink;
	int r;

	r = check_other_map(fs, map, off, n_sec, &next_free_sink);
	if (likely(r == -EBUSY))
		return 0;

	if (!r) {
		spadfs_error(fs, TXFLAGS_FS_ERROR,
			"no shadow when resurrecting blocks (%Lx,%x)",
			(unsigned long long)off,
			n_sec);
		r = -EFSERROR;
	}

	return r;
}
#endif

/* Helper for alloc_blocks_in_apage --- do allocation in case it is bitmap */

static int alloc_blocks_in_apage_bmp(SPADFS *fs, APAGE_MAP *map,
				     APAGE_MAP *other, struct alloc *al)
{
	struct apage_head *head = get_head(map);
	unsigned shift = fs->sectors_per_disk_block_bits;
	unsigned Xbmax = BITMAP_SIZE(fs->apage_size);
	unsigned Xboff;
	sector_t bst = MAKE_D_OFF(head->s.u.b.start0, head->s.u.b.start1);
	sector_t bottom, top;
	bottom = al->sector;
	if (bottom < bst)
		bottom = bst;
	top = al->top;
	if (likely(top > bst + (Xbmax << shift)))
		top = bst + (Xbmax << shift);
	if (unlikely(bottom >= top))
		return -ENOSPC;
	Xboff = (bottom - bst) >> shift;
	if (top - bst < (Xbmax << shift))
		Xbmax = (top - bst) >> shift;
	while (Xboff < Xbmax) {
		if (likely(bmp_test_32_full(fs, map, Xboff))) {
			Xboff = (Xboff + 32) & ~31;
			continue;
		}
test_bit:
		if (unlikely(!bmp_test(fs, map, Xboff))) {
			sector_t start;
			sector_t new_bot;
			unsigned Xc;
			unsigned limit;
			if (unlikely(al->flags & ALLOC_METADATA)) {
				if ((((unsigned)bst + (Xboff << shift)) &
				    ALLOC_MASK(al->flags)) + al->n_sectors >
				    ALLOC_MASK(al->flags) + 1)
					goto align;
			} else if (((unsigned)bst + (Xboff << shift)) &
				   ALLOC_MASK(al->flags)) {
				unsigned Xplus;
align:
				Xplus = (ALLOC_MASK(al->flags) + 1 -
					(((unsigned)bst + (Xboff << shift)) &
					ALLOC_MASK(al->flags))) >> shift;
				if (unlikely(!Xplus))
					Xplus = 1;
				Xboff += Xplus;
				continue;
			}
			limit = (al->n_sectors + al->extra_sectors) >> shift;
			for (Xc = 1; Xc < limit; Xc++)
				if (unlikely(Xboff + Xc >= Xbmax) ||
				    bmp_test(fs, map, Xboff + Xc)) {
					Xboff += Xc + 1;
					goto cont;
				}
			start = bst + (Xboff << shift);
#ifdef SPADFS_RESURRECT
			if (unlikely(al->flags & ALLOC_RESURRECT)) {
				int r = check_resurrect_map(fs, other, start,
					al->n_sectors);
				if (unlikely(r))
					return r;
			} else
#endif
			if (unlikely(check_conflict(fs, other, start, al->n_sectors + al->extra_sectors, &new_bot))) {
				if (likely(new_bot > bst + (Xboff << shift)) &&
				    likely(new_bot < bst + (Xbmax << shift))) {
					Xboff = (new_bot - bst +
						((1U << shift) - 1)) >> shift;
					continue;
				}
				return -ENOSPC;
			}
			al->sector = start;
			limit = al->n_sectors >> shift;
			for (Xc = 0; Xc < limit; Xc++)
				bmp_set(fs, map, Xboff + Xc);
			return 0;
		}
		Xboff++;
		if (likely(Xboff < Xbmax) && likely(Xboff & 31))
			goto test_bit;
		cont:;
	}
	return -ENOSPC;
}

/*
 * Delete one aentry from double linked list and add it to single linked
 * freelist
 */

static void delete_block(SPADFS *fs, APAGE_MAP *map, struct aentry *e)
{
	struct aentry *pre, *post;
	struct apage_head *head;
	u16 nblk;
	pre = get_aentry(fs, map, SPAD2CPU16_LV(&e->prev));
	if (unlikely(IS_ERR(pre)))
		return;
	pre->next = e->next;
	post = get_aentry(fs, map, SPAD2CPU16_LV(&e->next));
	if (unlikely(IS_ERR(post)))
		return;
	nblk = post->prev;
	post->prev = e->prev;
	CPU2SPAD64_LV(&e->start, 0);
	CPU2SPAD32_LV(&e->len, 0);
	CPU2SPAD16_LV(&e->prev, 0);
	head = get_head(map);
	e->next = head->s.u.l.freelist;
	head->s.u.l.freelist = nblk;
}

/* Alloc block in a given aentry */

static int alloc_block(SPADFS *fs, struct aentry *a, struct alloc *al,
		       APAGE_MAP *other)
{
	sector_t bottom, top, new_bot;
	bottom = al->sector;
	if (likely(bottom < SPAD2CPU64_LV(&a->start)))
		bottom = SPAD2CPU64_LV(&a->start);
	top = al->top;
	if (likely(top > SPAD2CPU64_LV(&a->start) + SPAD2CPU32_LV(&a->len)))
		top = SPAD2CPU64_LV(&a->start) + SPAD2CPU32_LV(&a->len);
new_bottom:
	if (unlikely(al->flags & ALLOC_METADATA))
		if (likely(((unsigned)bottom & ALLOC_MASK(al->flags)) +
				al->n_sectors <= ALLOC_MASK(al->flags) + 1))
			goto skip_pad;
	bottom = (bottom + ALLOC_MASK(al->flags)) &
		 ~(sector_t)ALLOC_MASK(al->flags);
skip_pad:
	if (unlikely(bottom + al->n_sectors + al->extra_sectors > top))
		return -ENOSPC;

#ifdef SPADFS_RESURRECT
	if (unlikely(al->flags & ALLOC_RESURRECT)) {
		int r = check_resurrect_map(fs, other, bottom, al->n_sectors);
		if (unlikely(r))
			return r;
	} else
#endif
	if (unlikely(check_conflict(fs, other, bottom, al->n_sectors + al->extra_sectors, &new_bot))) {
		if (likely(new_bot > bottom)) {
			bottom = new_bot;
			goto new_bottom;
		}
		return -ENOSPC;
	}
	if (unlikely(bottom > SPAD2CPU64_LV(&a->start))) {
		al->flags |= ALLOC_FREE_FROM;
		al->top = SPAD2CPU64_LV(&a->start);
	}
	CPU2SPAD32_LV(&a->len, SPAD2CPU32_LV(&a->len) -
	  (((u32)bottom - (u32)SPAD2CPU64_LV(&a->start)) + al->n_sectors));
	CPU2SPAD64_LV(&a->start, bottom + al->n_sectors);
	al->sector = bottom;
	return 0;
}

/* Alloc block run in a given apage */

static int alloc_blocks_in_apage(SPADFS *fs, APAGE_MAP *map, APAGE_MAP *other,
				 struct alloc *al)
{
	struct apage_head *head = get_head(map);
	int r;

	if (unlikely(IS_ERR(head)))
		return PTR_ERR(head);

	if (likely(!(head->s.u.l.flags & APAGE_BITMAP))) {
		struct aentry *e;
		int n_cycles;
		int blk = find_block_before(fs, map, al->sector);
		if (unlikely(blk < 0))
			return blk;
		e = get_aentry_valid(fs, map, blk);
		n_cycles = fs->apage_size;
next_try:
		if (!blk ||
		    al->sector >=
		    SPAD2CPU64_LV(&e->start) + SPAD2CPU32_LV(&e->len)) {
			blk = SPAD2CPU16_LV(&e->next);
			/* blk is valid from find_block_before */
			if (unlikely(!blk))
				return -ENOSPC;
			e = get_aentry_valid(fs, map, blk);
		}
		if (unlikely(SPAD2CPU64_LV(&e->start) >= al->top))
			return -ENOSPC;
		if (SPAD2CPU32_LV(&e->len) >= al->n_sectors + al->extra_sectors) {
			r = alloc_block(fs, e, al, other);
			if (likely(!r)) {
				if (unlikely(e->len == CPU2SPAD32(0)))
					delete_block(fs, map, e);
				return 0;
			}
			if (unlikely(r != -ENOSPC))
				return r;
		}

		blk = SPAD2CPU16_LV(&e->next);
		if (unlikely(!blk))
			return -ENOSPC;
		e = get_aentry(fs, map, blk);
		if (unlikely(blk < 0))
			return blk;
		if (likely(n_cycles -= sizeof(struct aentry)))
			goto next_try;

		spadfs_error(fs, TXFLAGS_FS_ERROR,
			"infinite loop in apage %d in alloc_blocks_in_apage",
			map_2_apage_n(fs, map));
		return -EFSERROR;
	} else
		return alloc_blocks_in_apage_bmp(fs, map, other, al);
}

/* Map apage and alloc block run in it */

static int try_apage(SPADFS *fs, unsigned ap, struct alloc *al)
{
	int r;
	APAGE_MAP *map, *other;
	if (unlikely(IS_ERR(map = map_apage(fs, ap, MAP_ALLOC, &other))))
		return PTR_ERR(map);
	r = alloc_blocks_in_apage(fs, map, other, al);
	if (likely(!r)) {
		unmap_apage(fs, map, 1);
		unmap_apage(fs, other, 0);
		return 0;
	}
	unmap_apage(fs, map, 0);
	unmap_apage(fs, other, 0);
	return r;
}

/* Convert block number to an index of apage --- use binary search */

static int addr_2_apage(SPADFS *fs, sector_t o, char *msg)
{
	int a1, a2, a;
	a2 = fs->n_active_apages - 1;
	a1 = 0;
again:
	a = (a1 + a2) >> 1;
	if (o < (sector_t)SPAD2CPU64_LV(&fs->apage_index[a - 1].end_sector))
		a2 = a - 1;
	else if (o >= (sector_t)SPAD2CPU64_LV(&fs->apage_index[a].end_sector))
		a1 = a + 1;
	else
		return a;
	if (likely(a1 <= a2))
		goto again;
	spadfs_error(fs, TXFLAGS_FS_ERROR,
		"can't find apage for block %Lx, stuck on %d/%d, called from %s",
		(unsigned long long)o, a1, a2, msg);
	return -EFSERROR;
}

/* Alloc block run in a range specified by al->sector and al->top */

static int alloc_blocks_in_range(SPADFS *fs, struct alloc *al)
{
	int ap, tap;
	int r;

	if (unlikely(al->top > fs->size))
		al->top = fs->size;
	if (unlikely(al->sector >= al->top))
		return -ENOSPC;

	ap = addr_2_apage(fs, al->sector, "alloc_blocks_in_range 1");
	if (unlikely(ap < 0))
		return ap;
	if (likely(al->top <= SPAD2CPU64_LV(&fs->apage_index[ap].end_sector)))
		tap = ap;
	else {
		tap = addr_2_apage(fs, al->top - 1, "alloc_blocks_in_range 2");
		if (unlikely(tap < 0))
			return tap;
	}

next_ap:
	r = try_apage(fs, ap, al);
	if (likely(!r))
		return 0;
	if (unlikely(r != -ENOSPC))
		return r;
	ap++;
	if (unlikely(ap <= tap))
		goto next_ap;
	return -ENOSPC;
}

/*
 * Get number of consecutive free blocks at given location
 * "max" is the limit, don't scan past this, even if there are more free blocks
 */

static unsigned get_blocklen_at(SPADFS *fs, sector_t block, unsigned max, sector_t *next)
{
	struct apage_head *head;
	APAGE_MAP *map, *other = NULL;
	unsigned ret;
	int ap;
	max &= -(1U << fs->sectors_per_disk_block_bits);
	if (next)
		*next = 0;
	if (block >= fs->size)
		return 0;
	ap = addr_2_apage(fs, block, "get_blocklen_at");
	if (unlikely(ap < 0))
		return 0;
	map = map_apage(fs, ap, MAP_READ, unlikely(next != NULL) ? &other : NULL);
	if (unlikely(IS_ERR(map)))
		return 0;
	head = get_head(map);
	if (unlikely(other != NULL) && !CC_CURRENT(fs, &head->cc, &head->txc)) {
		unmap_apage(fs, other, 0);
		other = NULL;
	}
	if (likely(!(head->s.u.l.flags & APAGE_BITMAP))) {
		struct aentry *e;
		sector_t rs;
		int blk = find_block_before(fs, map, block);
		if (unlikely(blk < 0))
			goto unmap_0;
		e = get_aentry_valid(fs, map, blk);
		if (!blk || block >= SPAD2CPU64_LV(&e->start) + SPAD2CPU32_LV(&e->len)) {
			if (unlikely(next != NULL)) {
				int nextblk = SPAD2CPU16_LV(&e->next);
				if (likely(nextblk != 0)) {
					e = get_aentry(fs, map, nextblk);
					if (unlikely(IS_ERR(e)))
						goto unmap_0;
					*next = SPAD2CPU64_LV(&e->start);
				} else {
					*next = SPAD2CPU64_LV(&fs->apage_index[ap].end_sector);
				}
			}
			goto unmap_0;
		}
		rs = SPAD2CPU64_LV(&e->start) + SPAD2CPU32_LV(&e->len) - block;
		if (likely(rs > max))
			ret = max;
		else
			ret = rs;
	} else {
		unsigned Xboff = BITMAP_OFFSET(head, block);
		unsigned add = 1U << fs->sectors_per_disk_block_bits;
		ret = 0;
		while (ret < max &&
		       likely(Xboff < BITMAP_SIZE(fs->apage_size)) &&
		       !bmp_test(fs, map, Xboff))
			ret += add, Xboff++;
		if (unlikely(next != NULL) && !ret) {
			while (likely(Xboff < BITMAP_SIZE(fs->apage_size)) &&
			       bmp_test(fs, map, Xboff))
				block += add, Xboff++;
			*next = block;
			goto unmap_0;
		}
	}
	if (unlikely(next != NULL)) {
		sector_t new_free;
		if (unlikely(check_conflict(fs, other, block, ret, &new_free))) {
			unsigned orig_size = ret;
			unsigned test_len;
			if (check_conflict(fs, other, block, 1U << fs->sectors_per_disk_block_bits, &new_free)) {
				if (new_free)
					*next = new_free;
				else
					*next = SPAD2CPU64_LV(&fs->apage_index[ap].end_sector);
				goto unmap_0;
			}
			ret = 1U << fs->sectors_per_disk_block_bits;
			for (test_len = 2U << fs->sectors_per_disk_block_bits; test_len && test_len < orig_size; test_len <<= 1) {
				if (check_conflict(fs, other, block, test_len, &new_free))
					break;
				ret = test_len;
			}
			for (test_len >>= 1; test_len >= 1U << fs->sectors_per_disk_block_bits; test_len >>= 1) {
				if (ret + test_len < orig_size && !check_conflict(fs, other, block, ret + test_len, &new_free))
					ret += test_len;
			}
			goto unmap_ret;
		}
	}
	goto unmap_ret;

unmap_0:
	ret = 0;
unmap_ret:
	unmap_apage(fs, map, 0);
	if (other)
		unmap_apage(fs, other, 0);
	return ret;
}

sector_t spadfs_get_freespace(SPADFS *fs)
{
	return fs->freespace + fs->small_prealloc.n_sectors
#ifdef SPADFS_META_PREALLOC
		+ fs->meta_prealloc.n_sectors
#endif
		;
}

static int trim_alloc(SPADFS *fs, struct alloc *al)
{
	sector_t available;
	if (unlikely(al->n_sectors + al->extra_sectors < al->n_sectors)) {
		al->extra_sectors = -(1U << fs->sectors_per_disk_block_bits) - al->n_sectors;
	}
	available = spadfs_get_freespace(fs);
	if (likely(!capable(CAP_SYS_RESOURCE))) {
		if (unlikely(available <= fs->reserve_sectors))
			return 0;
		available -= fs->reserve_sectors;
	}
	if (likely(!(al->flags & ALLOC_METADATA))) {
		if (likely(available > fs->group_mask + 1))
			available = fs->group_mask + 1;
		if (unlikely(available > fs->max_allocation))
			available = fs->max_allocation;
	}
	if (unlikely(available < al->n_sectors + al->extra_sectors)) {
		if (likely(available >= al->n_sectors)) {
			al->extra_sectors = available - al->n_sectors;
		} else {
			if (unlikely(!available))
				return 0;
			al->n_sectors = available;
			al->extra_sectors = 0;
			if (unlikely(al->flags & ALLOC_METADATA))
				return 0;
		}
	}
	return 1;
}

static void goal_out_of_fs(SPADFS *fs, struct alloc *al)
{
	unsigned char sectors_per_group_bits = fs->sectors_per_group_bits;
	al->sector = (sector_t)fs->zones[2].grp_start << sectors_per_group_bits;
	if (unlikely(al->sector >= fs->size)) {
		al->sector = (sector_t)fs->zones[1].grp_start << sectors_per_group_bits;
		if (unlikely(al->sector >= fs->size))
			al->sector = 0;
	}
}

/*
 * Average number of free blocks in group in zone. We couldn't divide 64-bit
 * values in kernel, so get approximate value.
 */

static sector_t zone_average_free(struct spadfszone *z)
{
	sector_t s = z->freespace;
	sector_div(s, z->grp_n);
	return s;
}

/* A "slow path" helper for spadfs_alloc_blocks_unlocked */

static noinline int alloc_blocks_in_different_group(SPADFS *fs,
						    struct alloc *al)
{
	sector_t orig_start = al->sector;
	struct spadfszone *z;
	int orig_zone_empty;
	unsigned n_sectors;
	int r;

retry_less:
	if (likely(al->flags & ALLOC_BIG_FILE))
		z = &fs->zones[2];
	else if (likely(al->flags & ALLOC_SMALL_FILE))
		z = &fs->zones[1];
	else
		z = &fs->zones[0];

	orig_zone_empty = !z->grp_n;
forward_quad_search:
	if (likely(z->grp_n)) {
		unsigned pass;
		sector_t avg_free = zone_average_free(z);
		if (unlikely(avg_free < al->n_sectors))
			avg_free = al->n_sectors;
retry:
		for (pass = 0; pass < 2; pass++) {
			unsigned i;
			unsigned grp_current = orig_start >>
					       fs->sectors_per_group_bits;
			if (unlikely(grp_current < z->grp_start) ||
			    unlikely(grp_current >= z->grp_start + z->grp_n))
				grp_current = z->grp_start + grp_current % z->grp_n;
			i = 1;
			do {
				grp_current += likely(!pass) ? i : 1;
				if (unlikely(grp_current >=
					     z->grp_start + z->grp_n))
					grp_current -= z->grp_n;
				if (fs->group_info[grp_current].freespace >=
				    avg_free) {
					al->sector = (sector_t)grp_current <<
						     fs->sectors_per_group_bits;
					al->top = (sector_t)(grp_current + 1) <<
						  fs->sectors_per_group_bits;
					if (unlikely(al->top < al->sector))
						al->top = fs->size;
					r = alloc_blocks_in_range(fs, al);
					if (likely(!r)) {
alloc_success_set_group:
						al->flags |=
							ALLOC_NEW_GROUP_HINT;
						return 0;
					}
					if (unlikely(r != -ENOSPC))
						return r;
				}
				if (likely(!pass))
					i *= 2;
				else
					i++;
			} while (i <= z->grp_n);
		}
		if (avg_free > al->n_sectors) {
			avg_free = al->n_sectors;
			goto retry;
		}
	}
try_another_zone:
	if (likely(al->flags & ALLOC_BIG_FILE)) {
		if (likely(z == &fs->zones[2]))
			z = &fs->zones[1];
		else if (likely(z == &fs->zones[1]))
			z = &fs->zones[0];
		else
			goto failed;
	} else if (likely(al->flags & ALLOC_SMALL_FILE)) {
		if (likely(z == &fs->zones[1]))
			z = &fs->zones[2];
		else if (likely(z == &fs->zones[2]))
			z = &fs->zones[0];
		else
			goto failed;
	} else {
		if (likely(z == &fs->zones[0]))
			z = &fs->zones[1];
		else if (likely(z == &fs->zones[1]))
			z = &fs->zones[2];
		else
			goto failed;
	}
	if (z == &fs->zones[2] || orig_zone_empty)
		goto forward_quad_search;
	if (likely(z->grp_n)) {
		unsigned grp_current;
		sector_t avg_free = zone_average_free(z) /
					SPADFS_AVG_FREE_DIVIDE_OTHERZONE;
		if (unlikely(avg_free < al->n_sectors))
			avg_free = al->n_sectors;
retry2:
		grp_current = z->grp_start + z->grp_n;
		do {
			grp_current--;
			if (fs->group_info[grp_current].freespace >= avg_free) {
				al->sector = (sector_t)grp_current <<
					     fs->sectors_per_group_bits;
				al->top = (sector_t)(grp_current + 1) <<
					  fs->sectors_per_group_bits;
				if (unlikely(al->top < al->sector))
					al->top = fs->size;
				r = alloc_blocks_in_range(fs, al);
				if (likely(!r))
					goto alloc_success_set_group;
				if (unlikely(r != -ENOSPC))
					return r;
			}
		} while (grp_current > z->grp_start);
		if (avg_free > al->n_sectors) {
			avg_free = al->n_sectors;
			goto retry2;
		}
	}
	goto try_another_zone;

failed:
	/*
	 * Try to allocate less sectors
	 */
	n_sectors = ((al->n_sectors + al->extra_sectors) >> 1) & ~((1U << fs->sectors_per_disk_block_bits) - 1);
	if (n_sectors >= al->n_sectors) {
		al->extra_sectors = n_sectors - al->n_sectors;
	} else {
		if (spadfs_discard_all_reservations(fs))
			n_sectors = al->n_sectors;
		al->n_sectors = n_sectors;
		al->extra_sectors = 0;
	}

	/*printk("set max_allocation %x -> %x\n", al->n_sectors, fs->max_allocation);*/
	if (likely(n_sectors < fs->max_allocation))
		fs->max_allocation = n_sectors;

	if (unlikely(!n_sectors) || unlikely(al->flags & ALLOC_METADATA))
		return -ENOSPC;

	if (unlikely(al->n_sectors <= ALLOC_MASK(al->flags)))
		al->flags = (al->flags &
			    ~(ALLOC_MASK_MASK | ALLOC_BIG_FILE)) |
			    ALLOC_SMALL_FILE;
	else if (!al->extra_sectors)
		al->n_sectors &= ~ALLOC_MASK(al->flags);

	al->sector = orig_start;
	al->top = (al->sector + fs->group_mask + 1) & ~fs->group_mask;
	if (unlikely(al->top < al->sector))
		al->top = fs->size;
	r = alloc_blocks_in_range(fs, al);
	if (!r)
		return 0;
	if (r != -ENOSPC)
		return r;
	goto retry_less;
}

static int spadfs_free_blocks_(SPADFS *fs, sector_t start, sector_t n_sectors,
			       int acct);

static int spadfs_alloc_blocks_unlocked(SPADFS *fs, struct alloc *al)
{
	int r;
	struct alloc_reservation *res;

	if (unlikely(al->n_sectors & ((1U << fs->sectors_per_disk_block_bits) - 1))) {
		spadfs_error(fs, TXFLAGS_FS_ERROR,
			"trying to allocate %u blocks", al->n_sectors);
		r = -EFSERROR;
		goto ret_r;
	}

	if ((res = al->reservation) && res->len) {
		unsigned orig_res_len = res->len;
		res->len = 0;
		al->sector = res->start;
		if (al->n_sectors >= orig_res_len)
			al->n_sectors = orig_res_len;
		al->extra_sectors = 0;
		do {
			al->top = al->sector + al->n_sectors;
			r = alloc_blocks_in_range(fs, al);
			if (likely(!r)) {
				if (al->n_sectors == orig_res_len) {
					spadfs_discard_reservation_unlocked(fs, res);
				} else {
					res->len = orig_res_len - al->n_sectors;
					res->start += al->n_sectors;
				}
				goto ok_allocated_nores;
			}
			if (unlikely(r != -ENOSPC))
				goto ret_r;
			al->n_sectors >>= 1;
			al->n_sectors &= ~((1U << fs->sectors_per_disk_block_bits) - 1);
		} while (al->n_sectors);
		spadfs_error(fs, TXFLAGS_FS_ERROR, "failed to allocate reserved sectors at (%Lx,%x), flags %x",
				(unsigned long long)res->start,
				orig_res_len,
				al->flags);
		spadfs_discard_reservation_unlocked(fs, res);
		r = -EFSERROR;
		goto ret_r;
	}

	al->sector &= ~(sector_t)((1U << fs->sectors_per_disk_block_bits) - 1);
	if (unlikely(al->sector >= fs->size))
		goal_out_of_fs(fs, al);

	if (unlikely(al->flags & ALLOC_METADATA)) {
		if (al->n_sectors <= (1U << fs->sectors_per_page_bits))
			al->flags |= ((1U << fs->sectors_per_page_bits) - 1) *
				     ALLOC_MASK_1;
	}

	if (al->flags & (ALLOC_PARTIAL_AT_GOAL
#ifdef SPADFS_RESURRECT
	    | ALLOC_RESURRECT
#endif
	    )) {
		unsigned bl = get_blocklen_at(fs, al->sector, al->n_sectors + al->extra_sectors, NULL);
		if (likely(bl != 0)) {
			unsigned orig_n_sectors = al->n_sectors;
			unsigned orig_extra_sectors = al->extra_sectors;
			while (1) {
				bl &= ~((1U << fs->sectors_per_disk_block_bits) - 1);
				if (bl >= al->n_sectors) {
					al->extra_sectors = bl - al->n_sectors;
				} else {
					if (unlikely(!bl))
						break;
					al->n_sectors = bl;
					al->extra_sectors = 0;
				}
				al->top = al->sector + al->n_sectors + al->extra_sectors;
				r = alloc_blocks_in_range(fs, al);
				if (likely(!r))
					goto ok_allocated;
				if (unlikely(r != -ENOSPC))
					goto ret_r;
				bl >>= 1;
			}
			al->n_sectors = orig_n_sectors;
			al->extra_sectors = orig_extra_sectors;
		}
#ifdef SPADFS_RESURRECT
		if (unlikely(al->flags & ALLOC_RESURRECT)) {
			spadfs_error(fs, TXFLAGS_FS_ERROR,
				"failed to resurrect blocks at (%Lx,%x)",
				(unsigned long long)al->sector,
				al->n_sectors);
			r = -EFSERROR;
			goto ret_r;
		}
#endif
	}

	if (al->flags & ALLOC_BIG_FILE) {
		if (al->n_sectors >= 1U << fs->sectors_per_cluster_bits) {
			al->flags |= ((1U << fs->sectors_per_cluster_bits) - 1) *
				     ALLOC_MASK_1;
			al->sector &= ~(sector_t)((1U << fs->sectors_per_cluster_bits) - 1);
		}
	}

	al->top = (al->sector + fs->group_mask + 1) & ~fs->group_mask;
	if (unlikely(al->top < al->sector))
		al->top = fs->size;

	r = alloc_blocks_in_range(fs, al);
	if (likely(!r))
		goto ok_allocated;

	if (unlikely(r != -ENOSPC))
		goto ret_r;

	r = alloc_blocks_in_different_group(fs, al);
	if (likely(!r))
		goto ok_allocated;
ret_r:
	return r;

ok_allocated:
	if ((res = al->reservation)) {
		BUG_ON(res->len);
		if (likely(al->extra_sectors != 0)) {
			res->start = al->sector + al->n_sectors;
			res->len = al->extra_sectors;
			spadfs_add_reservation(fs, res);
		}
	}
ok_allocated_nores:
	if (unlikely(al->flags & ALLOC_FREE_FROM)) {
		r = spadfs_free_blocks_(fs, al->top,
					al->sector - al->top, 0);
		if (unlikely(r))
			goto ret_r;
	}
	if (unlikely(al->n_sectors > fs->freespace)) {
		spadfs_error(fs, TXFLAGS_FS_ERROR,
			"free block count underrun: %x < %x",
			(unsigned)fs->freespace, al->n_sectors);
		r = -EFSERROR;
		goto ret_r;
	}
	freespace_decrease(fs, al->sector, al->n_sectors);
	return 0;
}

static pid_t prealloc_pgrp(void)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,24)
	return process_group(current);
#elif LINUX_VERSION_CODE < KERNEL_VERSION(2,6,30)
	struct pid *grp;
	pid_t val;
	rcu_read_lock();
	grp = task_pgrp(current);
	val = pid_vnr(grp);
	rcu_read_unlock();
	return val;
#else
	return task_pgrp_vnr(current);
#endif
}

/*
 * This is the main entry to block allocator. May be called by any other
 * subsystem.
 * In order to minimize CPU usage by passing all allocation parameters between
 * allocator functions, we pass pointer to struct alloc instead. struct alloc is
 * usually placed on caller's stack.
 * The following fields in "struct alloc" must be filled before calling this
 * function:
 *	sector: a hint where the allocation should start. Must be dividable by
 *		"sectors_per_block" value
 *	n_sectors: preferred number of sectors. Must be dividable by
 *		"sectors_per_block" value
 *	flags:
 *		ALLOC_METADATA: we are allocating metadata, this means:
 *			- prefer metadata zone
 *			- alloc exactly n_sectors or nothing (the caller must
 *			handle failure of allocating more than 1 block without
 *			loss of functionality --- i.e. create chains instead of
 *			hash tables in directories)
 *			- n_sectors must be less or equal than
 *			"sectors_per_page"
 *			- the resulting allocation won't cross
 *			"sectors_per_page" boundary
 *		ALLOC_SMALL_FILE: we are allocating small file (below threshold)
 *			- prefer small file zone
 *			- may alloc less blocks if the disk is too fragmented,
 *			the resulting number is returned in n_sectors. The
 *			result is always multiple of "sectors_per_block"
 *		ALLOC_BIG_FILE: we are allocating big file
 *			- prefer big file zone
 *			- prefer allocating at sectors_per_cluster boundary
 *			- may alloc less blocks if the disk is too fragmented,
 *			the resulting number is returned in n_sectors. The
 *			result is always multiple of "sectors_per_block"
 *		ALLOC_PARTIAL_AT_GOAL: may be ORed with other flags. It means
 *			that if the block at al->sector is free, we always
 *			allocate from al->sector, even if we allocate less than
 *			n_sectors sectors --- this is used when extending file
 *			with non-zero length
 * Returns:
 *	-ENOSPC --- the blocks couldn't be allocated
 *		If ALLOC_SMALL_FILE or ALLOC_BIG_FILE was specified, it means
 *		that the disk is totally full
 *		If ALLOC_METADATA was specified and n_sectors ==
 *		"sectors_per_page", it also means that the disk is totally full
 *		If ALLOC_METADATA was specified and n_sectors is larger, it
 *		means that the disk is too fragmented to allocate specified
 *		number of blocks
 *	-EIO, -EFSERROR, -E... --- some error occured
 *	0 --- succesful, then
 *		al->sector --- allocated sector
 *		al->n_sectors --- allocated number of sectors
 *		al->flags & ALLOC_NEW_GROUP_HINT --- the caller should set group
 *			hint for the file's directory to returned value
 */

int spadfs_alloc_blocks(SPADFS *fs, struct alloc *al)
{
	int r;
	struct prealloc_state *prealloc;
	unsigned max_prealloc;

	mutex_lock(&fs->alloc_lock);

	if (unlikely(!trim_alloc(fs, al))) {
		r = -ENOSPC;
		goto unlock_ret_r;
	}

	max_prealloc = (fs->max_prealloc & ~((512U << fs->sectors_per_disk_block_bits) - 1)) >> 9;
	if (unlikely(max_prealloc > fs->group_mask + 1))
		max_prealloc = fs->group_mask + 1;
	if (unlikely(max_prealloc > fs->max_allocation))
		max_prealloc = fs->max_allocation;

	if (al->reservation && al->reservation->len)
		goto no_prealloc;

	if ((al->flags & (ALLOC_SMALL_FILE | ALLOC_PARTIAL_AT_GOAL
#ifdef SPADFS_RESURRECT
		| ALLOC_RESURRECT
#endif
		)) == (ALLOC_SMALL_FILE | ALLOC_PARTIAL_AT_GOAL)) {
		if (al->sector == fs->small_prealloc.sector && fs->small_prealloc.n_sectors &&
		    prealloc_pgrp() == fs->small_prealloc.pgrp) {
			if (al->n_sectors > fs->small_prealloc.n_sectors)
				al->n_sectors = fs->small_prealloc.n_sectors;
			fs->small_prealloc.sector += al->n_sectors;
			fs->small_prealloc.n_sectors -= al->n_sectors;
			r = 0;
			goto unlock_ret_r;
		}
	}

	if (al->flags & (ALLOC_BIG_FILE | ALLOC_PARTIAL_AT_GOAL
#ifdef SPADFS_RESURRECT
		| ALLOC_RESURRECT
#endif
		) || unlikely(!max_prealloc))
		goto no_prealloc;
#ifdef SPADFS_META_PREALLOC
	else if (al->flags & ALLOC_METADATA)
		prealloc = &fs->meta_prealloc;
#endif
	else if (al->flags & ALLOC_SMALL_FILE)
		prealloc = &fs->small_prealloc;
	else
		goto no_prealloc;

	{
		struct alloc pre_al;
		pid_t pgrp = prealloc_pgrp();
		int retried = 0;
retry_prealloc:
		if (unlikely(!prealloc->n_sectors)) {
			unsigned long j = jiffies;
			if (j - prealloc->last_alloc > PREALLOC_TIMEOUT || pgrp != prealloc->pgrp) {
				prealloc->last_alloc = j;
				prealloc->n_allocations = 0;
				prealloc->allocations_size = 0;
			}
			prealloc->n_allocations++;
			prealloc->allocations_size += al->n_sectors;
			prealloc->pgrp = pgrp;
			/*printk("time: %lu, flags %x, n_allocations %u, allocations_size %u\n", j - prealloc->last_alloc, al->flags, prealloc->n_allocations, prealloc->allocations_size);*/
			if (prealloc->n_allocations >= PREALLOC_THRESHOLD) {
				pre_al.sector = al->sector;
				pre_al.flags = al->flags;
				pre_al.n_sectors = prealloc->allocations_size;
				if (pre_al.n_sectors > max_prealloc)
					pre_al.n_sectors = max_prealloc;
				if (unlikely(pre_al.n_sectors > fs->max_allocation))
					goto skip_prealloc;
				pre_al.extra_sectors = 0;
				pre_al.reservation = NULL;
				if (likely(pre_al.n_sectors >= al->n_sectors)) {
					r = spadfs_alloc_blocks_unlocked(fs, &pre_al);
					if (!r) {
						/*printk("prealloc done: %llx, %x\n", (unsigned long long)pre_al.sector, pre_al.n_sectors);*/
						prealloc->sector = pre_al.sector;
						prealloc->n_sectors = pre_al.n_sectors;
					}
				}
				skip_prealloc:;
			}
		} else {
			if (unlikely(jiffies - prealloc->last_alloc > PREALLOC_DISCARD_TIMEOUT))
				spadfs_prealloc_discard_unlocked(fs, prealloc);
		}
		if (unlikely(prealloc->n_sectors) && pgrp == prealloc->pgrp) {
retry_use_prealloc:
			if (likely(al->n_sectors <= prealloc->n_sectors)) {
#ifdef SPADFS_META_PREALLOC
				if (al->flags & ALLOC_METADATA) {
					unsigned sectors_per_page = 1U << fs->sectors_per_page_bits;
					unsigned to_page = sectors_per_page - ((unsigned)prealloc->sector & (sectors_per_page - 1));
					if (unlikely(to_page < al->n_sectors)) {
						if (unlikely(to_page > prealloc->n_sectors))
							to_page = prealloc->n_sectors;
						spadfs_free_blocks_unlocked(fs, prealloc->sector, to_page);
						prealloc->sector += to_page;
						prealloc->n_sectors -= to_page;
						goto retry_use_prealloc;
					}
				}
#endif
				al->sector = prealloc->sector;
				prealloc->sector += al->n_sectors;
				prealloc->n_sectors -= al->n_sectors;
				r = 0;
				prealloc->last_alloc = jiffies;
				prealloc->allocations_size += al->n_sectors;
				/*printk("taking from prealloc: flags %x, n_allocations %u, allocations_size %u took %u left %u\n", al->flags, prealloc->n_allocations, prealloc->allocations_size, al->n_sectors, prealloc->n_sectors);*/
				goto unlock_ret_r;
			} else {
				unsigned need, bl;
				need = al->n_sectors - prealloc->n_sectors;
				bl = get_blocklen_at(fs, prealloc->sector + prealloc->n_sectors, need, NULL);
				if (bl >= need) {
					pre_al.sector = prealloc->sector + prealloc->n_sectors;
					pre_al.n_sectors = need;
					if (pre_al.n_sectors < prealloc->allocations_size && pre_al.n_sectors < max_prealloc) {
						pre_al.n_sectors = prealloc->allocations_size;
						if (pre_al.n_sectors > max_prealloc)
							pre_al.n_sectors = max_prealloc;
					}
					pre_al.extra_sectors = 0;
					pre_al.flags = al->flags | ALLOC_PARTIAL_AT_GOAL;
					pre_al.reservation = NULL;
					r = spadfs_alloc_blocks_unlocked(fs, &pre_al);
					if (!r) {
						/*printk("prealloc extend: %llx, %x\n", (unsigned long long)pre_al.sector, pre_al.n_sectors);*/
						if (likely(pre_al.sector == prealloc->sector + prealloc->n_sectors)) {
							prealloc->n_sectors += pre_al.n_sectors;
						} else {
							spadfs_free_blocks_unlocked(fs, prealloc->sector, prealloc->n_sectors);
							prealloc->sector = pre_al.sector;
							prealloc->n_sectors = pre_al.n_sectors;
						}
						if (likely(al->n_sectors <= prealloc->n_sectors))
							goto retry_use_prealloc;
					}
				}
				spadfs_free_blocks_unlocked(fs, prealloc->sector, prealloc->n_sectors);
				prealloc->sector = 0;
				prealloc->n_sectors = 0;
				if (!retried) {
					retried = 1;
					goto retry_prealloc;
				}
			}
		}
	}

no_prealloc:

	r = spadfs_alloc_blocks_unlocked(fs, al);
	if (r == -ENOSPC) {
		if (
#ifdef SPADFS_META_PREALLOC
		    fs->meta_prealloc.n_sectors ||
#endif
		    fs->small_prealloc.n_sectors) {
#ifdef SPADFS_META_PREALLOC
			spadfs_prealloc_discard_unlocked(fs, &fs->meta_prealloc);
#endif
			spadfs_prealloc_discard_unlocked(fs, &fs->small_prealloc);

			r = spadfs_alloc_blocks_unlocked(fs, al);
		}
	}

unlock_ret_r:

	if (likely(!r) && unlikely(fs->trim_len != 0)) {
		while (al->sector < fs->trim_start + fs->trim_len &&
		       al->sector + al->n_sectors > fs->trim_start) {
			mutex_unlock(&fs->alloc_lock);
			msleep(1);
			mutex_lock(&fs->alloc_lock);
		}
	}

	mutex_unlock(&fs->alloc_lock);

	return r;
}

void spadfs_prealloc_discard_unlocked(SPADFS *fs, struct prealloc_state *prealloc)
{
	if (prealloc->n_sectors)
		spadfs_free_blocks_unlocked(fs, prealloc->sector, prealloc->n_sectors);
	prealloc->sector = 0;
	prealloc->n_sectors = 0;
	prealloc->last_alloc = jiffies;
	prealloc->n_allocations = 0;
	prealloc->allocations_size = 0;
}

static void adjust_max_run(SPADFS *fs, unsigned n_sectors)
{
	if (unlikely(n_sectors > fs->max_freed_run))
		fs->max_freed_run = n_sectors;
}

/* Free a given sector run in mapped apage */

static int apage_free(SPADFS *fs, APAGE_MAP *map, sector_t off, u32 len)
{
	struct apage_head *head = get_head(map);
	if (likely(!(head->s.u.l.flags & APAGE_BITMAP))) {
		int preblk, postblk, newblk;
		struct aentry *pre, *post, *new;
		preblk = find_block_before(fs, map, off);
		if (unlikely(preblk < 0))
			return preblk;
		pre = get_aentry_valid(fs, map, preblk);
		postblk = SPAD2CPU16_LV(&pre->next);	/* already valid */
		post = get_aentry_valid(fs, map, postblk);
		if (likely(preblk != 0)) {
			if ((sector_t)SPAD2CPU64_LV(&pre->start) +
			    SPAD2CPU32_LV(&pre->len) == off) {
				u32 new_pre_len = SPAD2CPU32_LV(&pre->len) + len;
				if (unlikely(new_pre_len < (u32)len))
					goto nj1;
				if (likely(postblk != 0)) {
					if (off + len ==
					   (sector_t)SPAD2CPU64_LV(&post->start)
					   ) {
						u32 new_post_len = new_pre_len + SPAD2CPU32_LV(&post->len);
						if (unlikely(new_post_len < new_pre_len))
							goto nj2;
						CPU2SPAD32_LV(&pre->len, new_post_len);
						adjust_max_run(fs, new_post_len);
						delete_block(fs, map, post);
						goto done;
					} else if (unlikely(off + len >
						  (sector_t)SPAD2CPU64_LV(&post->start))) {
						goto post_over;
					}
				}
nj2:
				CPU2SPAD32_LV(&pre->len, new_pre_len);
				adjust_max_run(fs, new_pre_len);
				goto done;
			} else if (unlikely((sector_t)SPAD2CPU64_LV(&pre->start)
				   + SPAD2CPU32_LV(&pre->len) > off)) {
				postblk = preblk;
				post = pre;
				goto post_over;
			}
		}
nj1:
		if (likely(postblk != 0)) {
			if (off + len ==
			    (sector_t)SPAD2CPU64_LV(&post->start)) {
				u32 new_post_len = SPAD2CPU32_LV(&post->len) + len;
				if (unlikely(new_post_len < (u32)len))
					goto nj3;
				CPU2SPAD64_LV(&post->start, off);
				CPU2SPAD32_LV(&post->len, new_post_len);
				adjust_max_run(fs, new_post_len);
				goto done;
			} else if (unlikely(off + len >
				   (sector_t)SPAD2CPU64_LV(&post->start))) {
post_over:
				spadfs_error(fs, TXFLAGS_FS_ERROR,
					"free (%Lx,%x) does overlap with block (%Lx,%x)",
					(unsigned long long)off,
					(unsigned)len,
					(unsigned long long)SPAD2CPU64_LV(
						&post->start),
					(unsigned)SPAD2CPU32_LV(&post->len));
				return -EFSERROR;
			}
		}
nj3:
		if (unlikely(!(newblk = SPAD2CPU16_LV(&head->s.u.l.freelist))))
			return 1;
		new = get_aentry(fs, map, newblk);
		if (unlikely(IS_ERR(new)))
			return PTR_ERR(new);
		head->s.u.l.freelist = new->next;
		CPU2SPAD64_LV(&new->start, off);
		CPU2SPAD32_LV(&new->len, len);
		CPU2SPAD16_LV(&new->prev, preblk);
		CPU2SPAD16_LV(&new->next, postblk);
		CPU2SPAD16_LV(&pre->next, newblk);
		CPU2SPAD16_LV(&post->prev, newblk);
		adjust_max_run(fs, len);

done:
		return 0;
	} else {
		unsigned bmpoff;
		adjust_max_run(fs, len);
		bmpoff = BITMAP_OFFSET(head, off);
		len = BITMAP_LEN(head, len);
		if (unlikely(bmpoff + len <= bmpoff) ||
		    unlikely(bmpoff + len > BITMAP_SIZE(fs->apage_size))) {
			spadfs_error(fs, TXFLAGS_FS_ERROR,
				"bad bitmap offset: %Lx,%x free(%Lx,%x)",
				(unsigned long long)MAKE_D_OFF(
					head->s.u.b.start0, head->s.u.b.start1),
				bmpoff,
				(unsigned long long)off, (unsigned)len);
			return -EFSERROR;
		}
		do {
			bmp_clear(fs, map, bmpoff);
			bmpoff++;
		} while (--len);
		return 0;
	}
}

__cold static int split_apage(SPADFS *fs, APAGE_MAP *map0, APAGE_MAP *map0_other, int ap);
__cold static int convert_to_bitmap(SPADFS *fs, APAGE_MAP *map, sector_t start, sector_t end);

/*
 * General block-freeing routine.
 * spadfs_free_blocks should be called instead of this
 * --- it locks the semaphore and forces "acct" to 1.
 *
 * start and n_sectors is the extent. acct means that we should do free space
 * accounting. The only situation where acct could be 0 is the call from
 * spadfs_alloc_blocks (in rare case when spadfs_alloc_blocks needs to allocate
 * blocks in the middle of one free extent, the extent must be split to two ---
 * instead of complicating logic of spadfs_alloc_blocks, it allocates more space
 * from the beginning of the extent and then calls spadfs_free_blocks_ with
 * acct == 0)
 */

static int spadfs_free_blocks_(SPADFS *fs, sector_t start, sector_t n_sectors, int acct)
{
	int ap;
	int r;
	APAGE_MAP *map, *other;
	if (unlikely(!validate_range(fs->size, (1U << fs->sectors_per_disk_block_bits) - 1, start, n_sectors))) {
		spadfs_error(fs, TXFLAGS_FS_ERROR,
			"freeing blocks: %Lx, %Lx",
			(unsigned long long)start,
			(unsigned long long)n_sectors);
		return -EFSERROR;
	}
retry:
	if (unlikely((n_sectors & ~(sector_t)0xffffffffu) != 0)) {
		/*
		 * Max block size is 64k, that is 128 sectors.
		 * Keep n_sectors aligned.
		 */
		const u32 max_sectors = -(1U << MAX_SECTORS_PER_BLOCK_BITS);
		if (unlikely(r = spadfs_free_blocks_(fs, start, max_sectors,
						     acct)))
			return r;
		start += max_sectors;
		n_sectors -= max_sectors;
		goto retry;
	}
retry_32:
	ap = addr_2_apage(fs, start, "spadfs_free_blocks_");
	if (unlikely(ap < 0))
		return ap;
	if ((sector_t)SPAD2CPU64_LV(&fs->apage_index[ap].end_sector) <
	    start + n_sectors) {
		u32 ss = (sector_t)SPAD2CPU64_LV(
				&fs->apage_index[ap].end_sector) - start;
		if (unlikely(!ss) || unlikely(ss >= n_sectors)) {
			spadfs_error(fs, TXFLAGS_FS_ERROR,
				"corrupted apage index: freeing %Lx,%Lx - apage %d, apage_end %Lx",
				(unsigned long long)start,
				(unsigned long long)n_sectors,
				ap,
				(unsigned long long)SPAD2CPU64_LV(
					&fs->apage_index[ap].end_sector));
			return -EFSERROR;
		}
		if (unlikely(r = spadfs_free_blocks_(fs, start, ss, acct)))
			return r;
		start += ss;
		n_sectors -= ss;
		goto retry_32;
	}
	map = map_apage(fs, ap, MAP_WRITE, &other);
	if (unlikely(IS_ERR(map)))
		return PTR_ERR(map);
free_again:
	r = apage_free(fs, map, start, n_sectors);
	if (likely(r <= 0)) {
		unmap_apage(fs, map, 1);
		unmap_apage(fs, other, 0);
		if (likely(!r) && likely(acct)) {
			freespace_increase(fs, start, n_sectors);
		}
		return r;
	}
	if (likely((sector_t)SPAD2CPU64_LV(&fs->apage_index[ap].end_sector) -
	    (sector_t)SPAD2CPU64_LV(&fs->apage_index[ap - 1].end_sector) >
	    BITMAP_SIZE(fs->apage_size) << fs->sectors_per_disk_block_bits)) {
		if (unlikely(r = split_apage(fs, map, other, ap)))
			return r;
		goto retry_32;
	}
	if (unlikely(r = convert_to_bitmap(fs, map,
		  (sector_t)SPAD2CPU64_LV(&fs->apage_index[ap - 1].end_sector),
		  (sector_t)SPAD2CPU64_LV(&fs->apage_index[ap].end_sector)))) {
		unmap_apage(fs, map, 1);
		unmap_apage(fs, other, 0);
		return r;
	}
	goto free_again;
}

int spadfs_free_blocks_unlocked(SPADFS *fs, sector_t start,
				sector_t n_sectors)
{
	return spadfs_free_blocks_(fs, start, n_sectors, 1);
}

static int spadfs_free_blocks(SPADFS *fs, sector_t start, sector_t n_sectors)
{
	int r;
	mutex_lock(&fs->alloc_lock);
	r = spadfs_free_blocks_unlocked(fs, start, n_sectors);
	mutex_unlock(&fs->alloc_lock);
	return r;
}

int spadfs_free_blocks_metadata(SPADFS *fs, sector_t start,
				sector_t n_sectors)
{
	spadfs_discard_buffers(fs, start, n_sectors);
	return spadfs_free_blocks(fs, start, n_sectors);
}

static int write_apage_index(SPADFS *fs);

/* Split apage to two */

__cold static noinline int split_apage(SPADFS *fs, APAGE_MAP *map0, APAGE_MAP *map0_other, int ap)
{
	int r;
	unsigned i, n;
	struct aentry *ae;
	sector_t split_block;
	APAGE_MAP *map1, *map1_other;
	struct apage_index_entry aie;
	unsigned b_s;
	if (unlikely(fs->n_active_apages >= fs->n_apages)) {
		spadfs_error(fs, TXFLAGS_FS_ERROR,
			"all %u apages used", fs->n_active_apages);
		r = -EFSERROR;
		goto ret0;
	}
	n = 0;
	ae = (struct aentry *)get_head(map0);
	for (i = fs->apage_size / (2 * sizeof(struct aentry)); i; i--) {
		n = SPAD2CPU16_LV(&ae->next);
		ae = get_aentry(fs, map0, n);
		if (unlikely(IS_ERR(ae))) {
			r = PTR_ERR(ae);
			goto ret0;
		}
	}
	if (unlikely(!n))
		split_block = ((sector_t)SPAD2CPU64_LV(&fs->apage_index[ap - 1].end_sector) +
		    (sector_t)SPAD2CPU64_LV(&fs->apage_index[ap].end_sector))
		    >> 1;
	else
		split_block = SPAD2CPU64_LV(&ae->start);
	split_block &= ~(sector_t)((1U << fs->sectors_per_disk_block_bits) - 1);

	/*printk("spadfs: splitting apage %d : (%llx %llx %llx)\n", ap, (unsigned long long)SPAD2CPU64_LV(&fs->apage_index[ap - 1].end_sector), (unsigned long long)split_block, (unsigned long long)SPAD2CPU64_LV(&fs->apage_index[ap].end_sector));*/

	b_s = (BITMAP_SIZE(fs->apage_size) >> 1 << fs->sectors_per_disk_block_bits);
	if (unlikely(split_block - (sector_t)SPAD2CPU64_LV(&fs->apage_index[ap - 1].end_sector) < b_s)) {
		split_block = (sector_t)SPAD2CPU64_LV(&fs->apage_index[ap - 1].end_sector) + b_s;
		split_block += (1U << fs->sectors_per_disk_block_bits) - 1;
		split_block &= ~(sector_t)((1U << fs->sectors_per_disk_block_bits) - 1);
	}
	if (unlikely((sector_t)SPAD2CPU64_LV(&fs->apage_index[ap].end_sector) -
	    split_block < b_s)) {
		split_block = (sector_t)SPAD2CPU64_LV(&fs->apage_index[ap].end_sector) - b_s;
		split_block &= ~(sector_t) ((1U << fs->sectors_per_disk_block_bits) - 1);
	}
	copy_apage(fs, fs->tmp_map, map0);
	map1 = map_apage(fs, fs->n_active_apages, MAP_NEW, &map1_other);
	if (unlikely(IS_ERR(map1))) {
		r = PTR_ERR(map1);
		goto ret0;
	}
	copy_apage(fs, map1_other, map0_other);
	unmap_apage(fs, map1_other, 1);
	make_apage(fs, map0);
	make_apage(fs, map1);
	for (i = sizeof(struct aentry); i < fs->apage_size;
	     i += sizeof(struct aentry)) {
		sector_t st;
		struct aentry *ae = get_aentry_valid(fs, fs->tmp_map, i);
		if (unlikely(ae->len == CPU2SPAD32_CONST(0)))
			continue;
		st = SPAD2CPU64_LV(&ae->start);
		if (st >= split_block)
			r = apage_free(fs, map1, st, SPAD2CPU32_LV(&ae->len));
		else if (st + SPAD2CPU32_LV(&ae->len) <= split_block)
			r = apage_free(fs, map0, st, SPAD2CPU32_LV(&ae->len));
		else {
			r = apage_free(fs, map0, st, split_block - st);
			if (likely(!r))
				r = apage_free(fs, map1, split_block,
					       st + SPAD2CPU32_LV(&ae->len) -
					       split_block);
		}
		if (unlikely(r)) {
			if (r > 0) {
				spadfs_error(fs, TXFLAGS_FS_ERROR,
					"out of space when splitting apage");
				r = -EFSERROR;
			}
			unmap_apage(fs, map1, 1);
			goto ret0;
		}
	}
	unmap_apage(fs, map0, 1);
	unmap_apage(fs, map1, 1);
	unmap_apage(fs, map0_other, 0);
	aie = fs->apage_index[fs->n_active_apages];
	for (i = fs->n_active_apages; i > ap + 1; i--) {
		fs->apage_index[i] = fs->apage_index[i - 1];
		spadfs_cond_resched();
	}
	fs->apage_index[ap + 1] = aie;
	fs->apage_index[ap + 1].end_sector =
				(sector_t)fs->apage_index[ap].end_sector;
	CPU2SPAD64_LV(&fs->apage_index[ap].end_sector, split_block);
	fs->n_active_apages++;

	spadfs_prune_cached_apage_buffers(fs);

	return write_apage_index(fs);

ret0:
	unmap_apage(fs, map0, 1);
	unmap_apage(fs, map0_other, 0);
	return r;
}

/* Write apage index after the split */

__cold static int write_apage_index(SPADFS *fs)
{
	unsigned i, n;
	sector_t sec;
	if (likely(!CC_CURRENT(fs, &fs->a_cc, &fs->a_txc))) {
		struct txblock *tx;
		struct buffer_head *bh;
		tx = spadfs_read_tx_block(fs, &bh, "write_apage_index 1");
		if (unlikely(IS_ERR(tx)))
			return PTR_ERR(tx);
		start_atomic_buffer_modify(fs, bh);
		CC_SET_CURRENT(fs, &fs->a_cc, &fs->a_txc);
		tx->a_cc = fs->a_cc;
		tx->a_txc = fs->a_txc;
		spadfs_tx_block_checksum(tx);
		end_atomic_buffer_modify(fs, bh);
		spadfs_brelse(fs, bh);
	}
	n = APAGE_INDEX_SECTORS(fs->n_apages, 512U << fs->sectors_per_disk_block_bits);
	sec = CC_VALID(fs, &fs->a_cc, &fs->a_txc) ? fs->apage_index0_sec : fs->apage_index1_sec;
	for (i = 0; i < n;
	     i += 1U << fs->sectors_per_buffer_bits,
	     sec += 1U << fs->sectors_per_buffer_bits) {
		struct buffer_head *bh;
		void *p;
		if (fs->split_happened &&
		    ((unsigned long)i << 9) >= (unsigned long)fs->n_active_apages * sizeof(struct apage_index_entry))
			break;
		p = spadfs_get_new_sector(fs, sec, &bh, "write_apage_index 2");
		if (unlikely(IS_ERR(p)))
			continue;
		memcpy(p, (u8 *)fs->apage_index + ((unsigned long)i << 9), 512U << fs->sectors_per_buffer_bits);
		mark_buffer_dirty(bh);
		spadfs_brelse(fs, bh);
	}
	fs->split_happened = 1;
	return 0;
}

/* Convert apage to bitmap */

__cold static noinline int convert_to_bitmap(SPADFS *fs, APAGE_MAP *map, sector_t start, sector_t end)
{
	unsigned i;
	copy_apage(fs, fs->tmp_map, map);
	make_apage_bitmap(fs, map, start);
	for (i = sizeof(struct aentry); i < fs->apage_size;
	     i += sizeof(struct aentry)) {
		struct aentry *ae = get_aentry_valid(fs, fs->tmp_map, i);
		sector_t st;
		unsigned len = SPAD2CPU32_LV(&ae->len);
		unsigned Xoff, Xlen;
		if (unlikely(!len))
			continue;
		st = SPAD2CPU64_LV(&ae->start);
		if (unlikely(st < start) ||
		    unlikely(st + len < st) ||
		    unlikely(st + len > end)) {
			spadfs_error(fs, TXFLAGS_FS_ERROR,
				"pointer (%Lx,%x) out of range (%Lx-%Lx)",
				(unsigned long long)st,
				len,
				(unsigned long long)start,
				(unsigned long long)end);
			return -EFSERROR;
		}
		if (unlikely(((unsigned)st | (unsigned)len) & ((1U << fs->sectors_per_disk_block_bits) - 1))) {
			spadfs_error(fs, TXFLAGS_FS_ERROR,
				"pointer (%Lx,%x) not aligned",
				(unsigned long long)st,
				len);
		}
		Xoff = (st - start) >> fs->sectors_per_disk_block_bits;
		Xlen = len >> fs->sectors_per_disk_block_bits;
		do {
			if (unlikely(!bmp_test(fs, map, Xoff))) {
				spadfs_error(fs, TXFLAGS_FS_ERROR,
					"block %Lx+%x freed twice",
					(unsigned long long)st,
					(Xoff << fs->sectors_per_disk_block_bits) - (unsigned)(st - start));
				return -EFSERROR;
			}
			bmp_clear(fs, map, Xoff);
			Xoff++;
		} while (--Xlen);
	}
	return 0;
}

void spadfs_reclaim_max_allocation(SPADFS *fs)
{
	unsigned max_allocation;
	mutex_lock(&fs->alloc_lock);
	max_allocation = fs->max_allocation * 2 + (4096U << fs->sectors_per_disk_block_bits);
	if (likely(max_allocation >= fs->max_allocation)) {
		if (max_allocation < fs->freespace)
			max_allocation = fs->freespace;
		fs->max_allocation = max_allocation;
	}
	mutex_unlock(&fs->alloc_lock);
}

#ifdef SPADFS_FSTRIM
int spadfs_trim_fs(SPADFS *fs, u64 start, u64 end, u64 minlen, sector_t *result)
{
	int err = 0;
	unsigned max;
	sector_t next;
	*result = 0;
	start &= -(u64)(1U << fs->sectors_per_disk_block_bits);
	end &= -(u64)(1U << fs->sectors_per_disk_block_bits);
	if (!end || end > fs->size)
		end = fs->size;
	if (!minlen)
		minlen = 1;

	if (READ_ONCE(fs->need_background_sync)) {
		spadfs_commit(fs);
	}

	while (start < end && !err) {
		sync_lock_decl

		mutex_lock(&fs->trim_lock);
		down_read_sync_lock(fs);
		mutex_lock(&fs->alloc_lock);

		if (unlikely(sb_rdonly(fs->s))) {
			err = -EROFS;
			mutex_unlock(&fs->alloc_lock);
			up_read_sync_lock(fs);
			mutex_unlock(&fs->trim_lock);
			break;
		}
		if (end - start == (unsigned)(end - start))
			max = end - start;
		else
			max = UINT_MAX;
		fs->trim_len = get_blocklen_at(fs, start, max, &next);
		if (fs->trim_len) {
			fs->trim_start = start;
			start += fs->trim_len;
		} else {
			if (next <= start)
				start = end;
			else
				start = next;
		}

		mutex_unlock(&fs->alloc_lock);
		up_read_sync_lock(fs);

		if (fs->trim_len >= minlen) {
#if LINUX_VERSION_CODE < KERNEL_VERSION(5,19,0)
			err = blkdev_issue_discard(fs->s->s_bdev, fs->trim_start, fs->trim_len, GFP_NOFS, 0);
#else
			err = blkdev_issue_discard(fs->s->s_bdev, fs->trim_start, fs->trim_len, GFP_NOFS);
#endif
			if (likely(!err))
				*result += fs->trim_len;
		}

		mutex_lock(&fs->alloc_lock);
		fs->trim_start = 0;
		fs->trim_len = 0;
		mutex_unlock(&fs->alloc_lock);
		mutex_unlock(&fs->trim_lock);

		if (!err && fatal_signal_pending(current))
			err = -EINTR;
	}
	return err;
}
#endif
