#include "spadfs.h"

void spadfs_allocmem_init(SPADFS *fs)
{
	fs->alloc_mem = RB_ROOT;
	fs->alloc_mem_sectors = 0;
}

void spadfs_allocmem_done(SPADFS *fs)
{
	struct rb_node *root;
	while ((root = fs->alloc_mem.rb_node)) {
#define am	rb_entry(root, struct allocmem, rb_node)
		fs->alloc_mem_sectors -= am->len;
		rb_erase(root, &fs->alloc_mem);
		kmem_cache_free(spadfs_extent_cachep, am);
#undef am
	}
	BUG_ON(fs->alloc_mem_sectors != 0);
}

static struct allocmem *spadfs_allocmem_find_node(SPADFS *fs, sector_t off,
						  sector_t n_sec,
						  sector_t *next_allocated)
{
	struct rb_node *p = fs->alloc_mem.rb_node;
	while (p) {
#define am	rb_entry(p, struct allocmem, rb_node)
		if (off + n_sec <= am->start) {
			*next_allocated = am->start;
			p = am->rb_node.rb_left;
		} else if (likely(off >= am->start + am->len)) {
			p = am->rb_node.rb_right;
		} else {
			return am;
		}
#undef am
	}
	return NULL;
}

int spadfs_allocmem_find(SPADFS *fs, sector_t off, sector_t n_sec,
			 sector_t *next_change)
{
	struct allocmem *am;
	*next_change = 0;
	am = spadfs_allocmem_find_node(fs, off, n_sec, next_change);
	if (unlikely(am != NULL)) {
		*next_change = am->start + am->len;
		return 1;
	}
	return 0;
}

int spadfs_allocmem_add(SPADFS *fs, sector_t off, sector_t n_sec)
{
	struct rb_node **p = &fs->alloc_mem.rb_node;
	struct rb_node *parent = NULL;
	struct allocmem *new;
	while (*p) {
		parent = *p;
#define am	rb_entry(parent, struct allocmem, rb_node)
		if (off + n_sec < am->start) {
			p = &am->rb_node.rb_left;
		} else if (off > am->start + am->len) {
			p = &am->rb_node.rb_right;
		} else if (off + n_sec == am->start) {
			am->start -= n_sec;
			am->len += n_sec;
			goto ret_0;
		} else if (likely(off == am->start + am->len)) {
			am->len += n_sec;
			goto ret_0;
		} else {
			printk(KERN_EMERG "spadfs: new alloc mem entry %Lx,%Lx overlaps with %Lx,%Lx\n",
				(unsigned long long)off,
				(unsigned long long)(off + n_sec),
				(unsigned long long)am->start,
				(unsigned long long)(am->start + am->len));
			BUG();
		}
#undef am
	}
	new = kmem_cache_alloc(spadfs_extent_cachep, GFP_NOFS);
	if (unlikely(!new))
		return -ENOMEM;
	new->start = off;
	new->len = n_sec;
	rb_link_node(&new->rb_node, parent, p);
	rb_insert_color(&new->rb_node, &fs->alloc_mem);
ret_0:
	fs->alloc_mem_sectors += n_sec;
	return 0;
}

void spadfs_allocmem_delete(SPADFS *fs, sector_t off, sector_t n_sec)
{
	do {
		sector_t step_n_sec;
		sector_t d_off_sink;
		struct allocmem *am = spadfs_allocmem_find_node(fs, off, 1,
								&d_off_sink);
		if (unlikely(!am)) {
			printk(KERN_EMERG "spadfs: trying to free non-present block %Lx,%Lx\n",
				(unsigned long long)off,
				(unsigned long long)n_sec);
			BUG();
		}
		if (unlikely(off < am->start)) {
			printk(KERN_EMERG "spadfs: trying to free non-present block %Lx, %Lx, found %Lx,%Lx\n",
				(unsigned long long)off,
				(unsigned long long)n_sec,
				(unsigned long long)am->start,
				(unsigned long long)am->len);
			BUG();
		}

		step_n_sec = n_sec;
		if (unlikely(step_n_sec > (am->start + am->len) - off))
			step_n_sec = (am->start + am->len) - off;

		if (off == am->start) {
			am->start += step_n_sec;
			am->len -= step_n_sec;
			fs->alloc_mem_sectors -= step_n_sec;
			if (!am->len) {
				rb_erase(&am->rb_node, &fs->alloc_mem);
				kmem_cache_free(spadfs_extent_cachep, am);
			}
		} else if (off + step_n_sec == am->start + am->len) {
			am->len -= step_n_sec;
			fs->alloc_mem_sectors -= step_n_sec;
		} else {
			sector_t orig_len = am->len;
			am->len = off - am->start;
			fs->alloc_mem_sectors -= orig_len - am->len;
			if (unlikely(spadfs_allocmem_add(fs, off + step_n_sec,
				  am->start + orig_len - (off + step_n_sec)))) {
				fs->alloc_mem_sectors += orig_len - am->len;
				am->len = orig_len;
				printk(KERN_WARNING "spadfs: memory allocation failure, leaking allocmem\n");
			}
		}
		off += step_n_sec;
		n_sec -= step_n_sec;
	} while (unlikely(n_sec != 0));
}

#ifdef CHECK_ALLOCMEM

int spadfs_allocmem_unit_test(SPADFS *fs)
{
	u8 map[256];
	unsigned x;
	sector_t zzzz;
	printk("allocmem unit test\n");
	memset(map, 0, sizeof map);
	for (x = 0; x < 100000; x++) {
		unsigned y;
		unsigned z;
		unsigned zz;
		unsigned xp;
		get_random_bytes(&y, sizeof y);
		y %= sizeof(map);
		get_random_bytes(&z, sizeof z);
		z = z % (sizeof(map) - y) + 1;
		for (zz = 0; zz < z; zz++) {
			if (map[y + zz] != map[y]) break;
		}
		for (z = 0; z < zz; z++)
			map[y + z] ^= 1;
		if (map[y]) {
			if (spadfs_allocmem_add(fs, y, zz)) {
				printk("spadfs_allocmem_add failed\n");
				return -ENOMEM;
			}
		} else {
			spadfs_allocmem_delete(fs, y, zz);
		}
		get_random_bytes(&y, sizeof y);
		y %= sizeof(map);
		get_random_bytes(&z, sizeof z);
		z = z % (sizeof(map) - y) + 1;
		get_random_bytes(&xp, sizeof xp);
		if (z > 5 && xp & 1) z = (z % 5) + 1;
		xp = 0;
		for (zz = 0; zz < z; zz++) {
			if (map[y + zz]) xp = 1;
		}
		zzzz = 0;
		if (spadfs_allocmem_find(fs, y, z, &zzzz) != xp) {
			printk("unit test error for %x/%x != %u\n", y, z, xp);
			return -EINVAL;
		}
		if (xp && zzzz <= y) {
			printk("next free returned wrong: %x < %x\n",
							(unsigned)zzzz, y);
			return -EINVAL;
		}
	}
	for (x = 0; x < sizeof(map); x++) {
		if (spadfs_allocmem_find(fs, x, 1, &zzzz) != map[x]) {
			printk("final test failed at %x\n", x);
			return -EINVAL;
		}
		if (map[x]) spadfs_allocmem_delete(fs, x, 1);
	}
	if (fs->alloc_mem_sectors) {
		printk("allocmem block count leaked: %Lx\n",
				(unsigned long long)fs->alloc_mem_sectors);
		return -EINVAL;
	}
	printk("allocmem unit test passed\n");
	return 0;
}

#endif
