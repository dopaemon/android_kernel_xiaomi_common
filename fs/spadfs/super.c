#include "spadfs.h"

__cold static void spadfs_free_super(struct super_block *s);

static
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,21)
const
#endif
struct super_operations spadfs_sops;
static spadfs_cache_t *spadfs_inode_cachep = NULL;
static spadfs_cache_t *spadfs_ea_cachep = NULL;
spadfs_cache_t *spadfs_extent_cachep = NULL;

/* Slab contructor */

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,24)
static void fnode_ctor(void *fn_, spadfs_cache_t *cachep, unsigned long flags)
#elif LINUX_VERSION_CODE < KERNEL_VERSION(2,6,27)
static void fnode_ctor(spadfs_cache_t *cachep, void *fn_)
#else
static void fnode_ctor(void *fn_)
#endif
{
	SPADFNODE *fn = fn_;
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,22)
	if (likely((flags & (SLAB_CTOR_VERIFY|SLAB_CTOR_CONSTRUCTOR)) ==
	    SLAB_CTOR_CONSTRUCTOR))
#endif
	{
		inode_init_once(inode(fn));
		fn->res.len = 0;
		INIT_LIST_HEAD(&fn->clear_entry);
		fn->ea = fn->ea_inline;
		if (spadfs_unlocked_extent_cache)
			fn->extent_cache_seq = 0;
	}
}

/*
 * Allocate/free inode. The list is walked and searched for
 * parent_fnode_block/parent_fnode_pos, so we'd better initialize them.
 */

static struct inode *spadfs_alloc_inode(struct super_block *s)
{
	SPADFS *fs = spadfs(s);
	SPADFNODE *fn;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,18,0)
	fn = alloc_inode_sb(s, spadfs_inode_cachep, GFP_NOFS);
#else
	fn = kmem_cache_alloc(spadfs_inode_cachep, GFP_NOFS);
#endif
	if (unlikely(!fn))
		return NULL;

	fn->fs = fs;
	fn->parent_fnode_block = 0;
	fn->parent_fnode_pos = 0;
	fn->target_blocks = 0;
	fn->target_blocks_exact = 0;
	fn->dont_truncate_prealloc = 0;
	fn->ea_size = 0;

	mutex_init(&fn->file_lock);

#if defined(SPADFS_QUOTA) && SPADFS_QUOTA >= 2
	memset(&fn->i_dquot, 0, sizeof fn->i_dquot);
#endif

	return inode(fn);
}

static void spadfs_destroy_inode(struct inode *inode)
{
	SPADFNODE *fn = spadfnode(inode);

	if (fn->parent_fnode_block)
		spadfs_set_parent_fnode(fn, 0, 0);

	if (unlikely(fn->ea != fn->ea_inline)) {
		kmem_cache_free(spadfs_ea_cachep, fn->ea);
		fn->ea = fn->ea_inline;
	}

	mutex_destroy(&fn->file_lock);

	kmem_cache_free(spadfs_inode_cachep, fn);
}

int spadfs_ea_alloc(SPADFNODE *f, unsigned ea_size)
{
	u8 *ea;
	BUG_ON(ea_size > FNODE_MAX_EA_SIZE);
	if (likely(f->ea != f->ea_inline))
		return 0;
	ea = kmem_cache_alloc(spadfs_ea_cachep, GFP_NOFS);
	if (unlikely(!ea))
		return -ENOMEM;
	memcpy(ea, f->ea_inline, f->ea_size);
	f->ea = ea;
	spadfs_find_ea_unx(f);
	return 0;
}

static void set_prealloc_part(SPADFS *fs, unsigned prealloc_part)
{
	fs->prealloc_part = prealloc_part;
	if (!(fs->prealloc_part & (fs->prealloc_part - 1)))
		fs->prealloc_part_bits = ffs(fs->prealloc_part) - 1;
	else
		fs->prealloc_part_bits = -1;
}

enum {
	Opt_help, Opt_uid, Opt_gid, Opt_umask,
	Opt_prealloc_part, Opt_prealloc_min, Opt_prealloc_max,
	Opt_xfer_size, Opt_buffer_size, Opt_prefetch, Opt_sync_time,
	Opt_no_checksums, Opt_checksums,
	Opt_ino64,
	Opt_usrquota, Opt_grpquota,
	Opt_err,
};

static match_table_t tokens = {
	{Opt_help, "help"},
	{Opt_uid, "uid=%u"},
	{Opt_gid, "gid=%u"},
	{Opt_umask, "umask=%o"},
	{Opt_prealloc_part, "prealloc_part=%u"},
	{Opt_prealloc_min, "prealloc_min=%u"},
	{Opt_prealloc_max, "prealloc_max=%u"},
	{Opt_xfer_size, "xfer_size=%u"},
	{Opt_buffer_size, "buffer_size=%u"},
	{Opt_prefetch, "prefetch=%u"},
	{Opt_sync_time, "sync_time=%u"},
	{Opt_no_checksums, "no_checksums"},
	{Opt_checksums, "checksums"},
	{Opt_ino64, "ino64=%s"},
	{Opt_usrquota, "usrquota"},
	{Opt_grpquota, "grpquota"},
	{Opt_err, NULL},
};

__cold static int parse_opts(SPADFS *fs, char *opts, int remount)
{
	char *p;
	while ((p = strsep(&opts, ","))) {
		substring_t args[MAX_OPT_ARGS];
		int token, option;
		char str[7];

		if (!*p)
			continue;

		token = match_token(p, tokens, args);
		switch (token) {
			case Opt_help:
				return 2;
			case Opt_uid:
				if (match_int(args, &option))
					return 0;
				if (remount) {
					if (fs->uid != option)
						return 0;
					break;
				}
				fs->uid = option;
				break;
			case Opt_gid:
				if (match_int(args, &option))
					return 0;
				if (remount) {
					if (fs->gid != option)
						return 0;
					break;
				}
				fs->gid = option;
				break;
			case Opt_umask:
				if (match_octal(args, &option))
					return 0;
				if (option < 0 || option > 0777)
					return 0;
				if (remount) {
					if (fs->mode != (0777 & ~option))
						return 0;
					break;
				}
				fs->mode = 0777 & ~option;
				break;
			case Opt_prealloc_part:
				if (match_int(args, &option))
					return 0;
				if (option <= 0) return 0;
				set_prealloc_part(fs, option);
				break;
			case Opt_prealloc_min:
				if (match_int(args, &option))
					return 0;
				if (option < 0) return 0;
				fs->min_prealloc = option;
				break;
			case Opt_prealloc_max:
				if (match_int(args, &option))
					return 0;
				if (option < 0) return 0;
				fs->max_prealloc = option;
				break;
			case Opt_xfer_size:
				if (match_int(args, &option))
					return 0;
				if (option <= 0) return 0;
				fs->xfer_size = option;
				break;
			case Opt_buffer_size:
				if (match_int(args, &option))
					return 0;
				if (option < 512 || option > PAGE_SIZE || (option & (option - 1)))
					return 0;
				if (remount) {
					if (option != 512U << fs->sectors_per_buffer_bits)
						return 0;
					break;
				}
				fs->buffer_size = option;
				break;
			case Opt_prefetch:
				if (match_int(args, &option))
					return 0;
				if (option < 0) return 0;
				fs->metadata_prefetch = option >> 9;
				if (fs->metadata_prefetch & (fs->metadata_prefetch - 1))
					fs->metadata_prefetch = 1U << (fls(fs->metadata_prefetch) - 1);
				break;
			case Opt_sync_time:
				if (match_int(args, &option))
					return 0;
				if (option <= 0 ||
				    option >= (1UL << (BITS_PER_LONG - 1)) / HZ)
					return 0;
				fs->spadfs_sync_time = option * HZ;
				break;
			case Opt_checksums:
				fs->mount_flags |=
					MOUNT_FLAGS_CHECKSUMS_OVERRIDE;
				fs->mount_flags |=
					MOUNT_FLAGS_CHECKSUMS;
				break;
			case Opt_no_checksums:
				fs->mount_flags |=
					MOUNT_FLAGS_CHECKSUMS_OVERRIDE;
				fs->mount_flags &=
					~MOUNT_FLAGS_CHECKSUMS;
				break;
			case Opt_ino64:
				match_strlcpy(str, args, sizeof str);
				if (!strcmp(str, "no")) {
					fs->mount_flags &=
						~(MOUNT_FLAGS_64BIT_INO |
						MOUNT_FLAGS_64BIT_INO_FORCE);
				} else if (!strcmp(str, "yes")) {
					fs->mount_flags |=
						MOUNT_FLAGS_64BIT_INO;
					fs->mount_flags &=
						~MOUNT_FLAGS_64BIT_INO_FORCE;
				} else if (!strcmp(str, "force")) {
					fs->mount_flags |=
						MOUNT_FLAGS_64BIT_INO |
						MOUNT_FLAGS_64BIT_INO_FORCE;
				} else {
					return 0;
				}
				break;
#ifdef SPADFS_QUOTA
			case Opt_usrquota:
				fs->mount_flags |= MOUNT_FLAGS_USRQUOTA;
				break;
			case Opt_grpquota:
				fs->mount_flags |= MOUNT_FLAGS_GRPQUOTA;
				break;
#endif
			default:
				return 0;
		}
	}
	return 1;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(3,3,0)
static int spadfs_show_options(struct seq_file *seq, struct vfsmount *vfs)
{
	struct super_block *s = vfs->mnt_sb;
#else
static int spadfs_show_options(struct seq_file *seq, struct dentry *root)
{
	struct super_block *s = root->d_sb;
#endif
	SPADFS *fs = spadfs(s);

	if (fs->uid)
		seq_printf(seq, ",uid=%u", (unsigned)fs->uid);
	if (fs->gid)
		seq_printf(seq, ",gid=%u", (unsigned)fs->gid);
	seq_printf(seq, ",umask=%03o", (~fs->mode & 0777));
	seq_printf(seq, ",prealloc_part=%u", fs->prealloc_part);
	seq_printf(seq, ",prealloc_min=%u", fs->min_prealloc);
	seq_printf(seq, ",prealloc_max=%u", fs->max_prealloc);
	seq_printf(seq, ",xfer_size=%u", fs->xfer_size);
	seq_printf(seq, ",buffer_size=%u", 512U << fs->sectors_per_buffer_bits);
	seq_printf(seq, ",prefetch=%u", fs->metadata_prefetch << 9);
	seq_printf(seq, ",sync_time=%lu", fs->spadfs_sync_time / HZ);
	if (fs->mount_flags & MOUNT_FLAGS_CHECKSUMS)
		seq_printf(seq, ",checksums");
	else
		seq_printf(seq, ",no_checksums");
	if (!(fs->mount_flags & MOUNT_FLAGS_64BIT_INO))
		seq_printf(seq, ",ino64=no");
	else if (!(fs->mount_flags & MOUNT_FLAGS_64BIT_INO_FORCE))
		seq_printf(seq, ",ino64=yes");
	else
		seq_printf(seq, ",ino64=force");
#ifdef SPADFS_QUOTA
	if (fs->mount_flags & MOUNT_FLAGS_USRQUOTA)
		seq_printf(seq, ",usrquota");
	if (fs->mount_flags & MOUNT_FLAGS_GRPQUOTA)
		seq_printf(seq, ",grpquota");
#endif

	return 0;
}

static void spadfs_help(void)
{
	printk("\n\
SPADFS filesystem options:\n\
        help                    display this text\n\
        uid=xxx                 set default uid of files\n\
        gid=xxx                 set default gid of files\n\
        umask=xxx               set default mode of files\n\
        prealloc_part=xxx       prealloc this part of existing file size\n\
                                (i.e. 8 means prealloc 1/8 of file size)\n\
        prealloc_min=xxx        minimum preallocation in bytes\n\
        prealloc_max=xxx        maximum preallocation in bytes\n\
        xfer_size=xxx           optimal request size reported in st_blksize\n\
        buffer_size=xxx         set kernel buffer size\n\
        prefetch=xxx            metadata prefetch in bytes\n\
        sync_time=xxx           sync after this interval in seconds\n\
        no_checksums            do not check and make checksums\n\
        checksums               do check and make checksums\n\
        ino64=no,yes,force      use 64-bit inode numbers\n\
        usrquota                user quota\n\
        grpquota                group quota\n\
\n");
}

/* Report error. flags is the mask of TXFLAGS_* that will be set in txblock */

__cold void spadfs_error_(SPADFS *fs, unsigned flags, const char *msg, ...)
{
	unsigned long irqstate;
	if (msg) {
		va_list va;
		va_start(va, msg);
		vprintk(msg, va);
		va_end(va);
		dump_stack();
	}
	spin_lock_irqsave(&fs->txflags_new_lock, irqstate);
	fs->txflags_new |= flags;
	spin_unlock_irqrestore(&fs->txflags_new_lock, irqstate);
}

__cold static void *spadfs_alloc(size_t n_elements, size_t element_size, size_t extra)
{
	size_t size;
	if (element_size && n_elements > ((size_t)-1 - extra) / element_size)
		return NULL;
	size = n_elements * element_size + extra;
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,12,0) && !TEST_RHEL_VERSION(7,5)
	if (size <= PAGE_SIZE)
		return kmalloc(size, GFP_KERNEL);
	if (size <= KMALLOC_MAX_SIZE) {
		void *ptr = kmalloc(size, GFP_KERNEL | __GFP_NOWARN | __GFP_NORETRY);
		if (ptr)
			return ptr;
	}
	return vmalloc(size);
#else
	return kvmalloc(size, GFP_KERNEL);
#endif
}

#ifndef SIMPLE_SYNC_LOCK

__cold static void spadfs_do_done_sync_lock(SPADFS *fs);

__cold static int spadfs_do_init_sync_lock(SPADFS *fs)
{
	unsigned i;
	for (i = 0; i < spadfs_nr_cpus; i++) {
		const unsigned size = max((unsigned)sizeof(struct rw_semaphore),
					  (unsigned)L1_CACHE_BYTES);
		int node = cpu_possible(i) ? cpu_to_node(i) : numa_node_id();
		/* (unsigned) kills warning */
		fs->sync_locks[i] = kmalloc_node(size, GFP_KERNEL, node);
		if (!fs->sync_locks[i]) {
			spadfs_do_done_sync_lock(fs);
			return 1;
		}
		init_rwsem(fs->sync_locks[i]);
	}
	return 0;
}

__cold static void spadfs_do_done_sync_lock(SPADFS *fs)
{
	unsigned i;
	for (i = 0; i < spadfs_nr_cpus; i++) {
		kfree(fs->sync_locks[i]);
		fs->sync_locks[i] = NULL;
	}
}

void spadfs_do_down_read_sync_lock(SPADFS *fs, unsigned *cpu)
{
	/*
	 * Use raw_smp_processor_id() instead of smp_processor_id() to
	 * suppress warning message about using it with preempt enabled.
	 * Preempt doesn't really matter here, locking the lock on
	 * a different CPU will cause a small performance impact but
	 * no race condition.
	 */
	unsigned cp = raw_smp_processor_id();
	*cpu = cp;
	down_read(fs->sync_locks[cp]);
}

void spadfs_do_up_read_sync_lock(SPADFS *fs, unsigned cpu)
{
	up_read(fs->sync_locks[cpu]);
}

void spadfs_do_down_write_sync_lock(SPADFS *fs)
{
	unsigned i;
	for (i = 0; i < spadfs_nr_cpus; i++) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,18)
		down_write_nested(fs->sync_locks[i], i);
#else
		down_write(fs->sync_locks[i]);
#endif
	}
}

void spadfs_do_up_write_sync_lock(SPADFS *fs)
{
	unsigned i;
	for (i = 0; i < spadfs_nr_cpus; i++)
		up_write(fs->sync_locks[i]);
}

#endif

int spadfs_stop_cycles(SPADFS *fs, sector_t key, sector_t (*c)[2],
		       const char *msg)
{
	if (unlikely((*c)[0] == key && unlikely((*c)[1] != 0))) {
		spadfs_error(fs, TXFLAGS_FS_ERROR,
			"cycle detected on key %Lx in %s",
			(unsigned long long)key, msg);
		return 1;
	}
	(*c)[1]++;
	if (likely(!(((*c)[1] - 1) & (*c)[1])))
		(*c)[0] = key;
	return 0;
}

int spadfs_issue_flush(SPADFS *fs)
{
	int r;
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,9)
	r = 0;
#elif LINUX_VERSION_CODE < KERNEL_VERSION(2,6,35)
	r = blkdev_issue_flush(fs->s->s_bdev, NULL);
#elif LINUX_VERSION_CODE < KERNEL_VERSION(2,6,37)
	r = blkdev_issue_flush(fs->s->s_bdev, GFP_NOFS, NULL, BLKDEV_IFL_WAIT);
#elif LINUX_VERSION_CODE < KERNEL_VERSION(5,8,0) && !TEST_RHEL_VERSION(8,4)
	r = blkdev_issue_flush(fs->s->s_bdev, GFP_NOFS, NULL);
#elif LINUX_VERSION_CODE < KERNEL_VERSION(5,12,0) && !TEST_RHEL_VERSION(8,6)
	r = blkdev_issue_flush(fs->s->s_bdev, GFP_NOFS);
#else
	r = blkdev_issue_flush(fs->s->s_bdev);
#endif
	if (unlikely(r == -EOPNOTSUPP))
		r = 0;
	if (unlikely(r))
		spadfs_error(fs, TXFLAGS_IO_WRITE_ERROR, "flush error: %d", r);
	return r;
}

void spadfs_tx_block_checksum(struct txblock *tb)
{
	tb->checksum ^= CHECKSUM_BASE ^ __byte_sum(tb, 512);
}

static int spadfs_increase_cc(SPADFS *fs, int inc)
{
	struct txblock *tb;
	struct buffer_head *bh;
	int r, rr = 0;

	tb = spadfs_read_tx_block(fs, &bh, "spadfs_increase_cc");
	if (unlikely(IS_ERR(tb)))
		return PTR_ERR(tb);

	start_atomic_buffer_modify(fs, bh);
	if (likely(inc == 1)) {
		u16 cc = SPAD2CPU16_LV(&tb->cc);
		if (cc == 0xffff) {
			/* !!! FIXME: process wrap-around */
			spadfs_error(fs, TXFLAGS_FS_ERROR, "crash count wrap-around, remounting read-only");
			rr = 1;
			goto skip_inc;
		}
		cc += inc;
		CPU2SPAD16_LV(&tb->cc, cc);
		if (unlikely(SPAD2CPU32_LV(&fs->cct[cc]))) {
			spadfs_error(fs, TXFLAGS_FS_ERROR,
				"crash count table is damaged (non-zero at %04x: %08x)",
				(unsigned)SPAD2CPU16_LV(&tb->cc),
				(unsigned)SPAD2CPU32_LV(&fs->cct[cc]));
			rr = -EFSERROR;
		}
		fs->cc = cc - 1;
		fs->txc = SPAD2CPU32_LV(&fs->cct[fs->cc]);
	} else if (unlikely(inc == -1)) {
		CPU2SPAD16_LV(&tb->cc, SPAD2CPU16_LV(&tb->cc) + inc);
		/* If unmounting, clean up preallocated inodes */
		spin_lock(&fs->stable_ino_lock);
		if (fs->stable_ino) {
			CPU2SPAD64_LV(&tb->ino, fs->stable_ino);
			fs->stable_ino = 0;
		}
		spin_unlock(&fs->stable_ino_lock);
	}

skip_inc:
	spin_lock_irq(&fs->txflags_new_lock);
	fs->txflags |= fs->txflags_new;
	spin_unlock_irq(&fs->txflags_new_lock);

	CPU2SPAD32_LV(&tb->txflags, fs->txflags);
	spadfs_tx_block_checksum(tb);
	end_atomic_buffer_modify(fs, bh);

	r = spadfs_sync_dirty_buffer(bh);
	if (likely(!r))
		r = spadfs_issue_flush(fs);
	if (unlikely(r))
		spadfs_error(fs, TXFLAGS_IO_WRITE_ERROR,
			"write error at tx block: %d", r);

	spadfs_brelse(fs, bh);
	return unlikely(r) ? r : rr;
}

static int spadfs_update_error_flags(SPADFS *fs)
{
	/*
	 * Don't lock when reading txflags_new --- if we miss an update here,
	 * it would be updated later.
	 */
	if (unlikely(fs->txflags_new & ~fs->txflags)) {
		if (sb_rdonly(fs->s))
			return 0;
		return spadfs_increase_cc(fs, 0);
	}
	return 0;
}

static noinline int spadfs_new_stable_ino_slow(SPADFS *fs, u64 *stable_ino)
{
	int r;
	struct txblock *tb;
	struct buffer_head *bh;
	u64 next_stable_ino;

	/*sync_lock_decl*/
	down_write_sync_lock(fs);

	spin_lock(&fs->stable_ino_lock);
	*stable_ino = fs->stable_ino;
	if (unlikely((*stable_ino & (SPADFS_INO_BATCH - 1)) != 0)) {
		fs->stable_ino++;
		spin_unlock(&fs->stable_ino_lock);
		r = 0;
		goto unlock_ret_r;
	}
	spin_unlock(&fs->stable_ino_lock);

	tb = spadfs_read_tx_block(fs, &bh, "spadfs_new_stable_ino_slow");
	if (unlikely(IS_ERR(tb))) {
		r = PTR_ERR(tb);
		goto unlock_ret_r;
	}

	start_atomic_buffer_modify(fs, bh);

	*stable_ino = SPAD2CPU64_LV(&tb->ino);
	if (unlikely(!*stable_ino))
		*stable_ino = SPADFS_INO_ROOT + 1;
	next_stable_ino = (*stable_ino | (SPADFS_INO_BATCH - 1)) + 1;
	if (unlikely(next_stable_ino == SPADFS_INO_INITIAL_REGION))
		next_stable_ino = (u64)SPADFS_INO_INITIAL_REGION * 2;
	if (unlikely(!(next_stable_ino & 0xFFFFFFFF)))
		next_stable_ino |= (u64)SPADFS_INO_INITIAL_REGION * 2;
	CPU2SPAD64_LV(&tb->ino, next_stable_ino);

	spin_lock(&fs->stable_ino_lock);
	fs->stable_ino = *stable_ino + 1;
	spin_unlock(&fs->stable_ino_lock);

	spadfs_tx_block_checksum(tb);

	end_atomic_buffer_modify(fs, bh);
	spadfs_brelse(fs, bh);

	r = 0;

unlock_ret_r:
	up_write_sync_lock(fs);
	return r;
}

int spadfs_new_stable_ino(SPADFS *fs, u64 *stable_ino)
{
	if (unlikely(sb_rdonly(fs->s)))
		return -EROFS;
	spin_lock(&fs->stable_ino_lock);
	if (unlikely(!((*stable_ino = fs->stable_ino) &
		       (SPADFS_INO_BATCH - 1)))) {
		spin_unlock(&fs->stable_ino_lock);
		return spadfs_new_stable_ino_slow(fs, stable_ino);
	}
	fs->stable_ino++;
	spin_unlock(&fs->stable_ino_lock);
	return 0;
}

static int spadfs_start_new_tx(SPADFS *fs)
{
	int r;
	if (unlikely(!fs->max_allocation))
		fs->max_allocation = 1U << fs->sectors_per_disk_block_bits;
	if (unlikely(SPAD2CPU32_LV(&fs->cct[fs->cc]) == 0x7fffffff)) {
		if (unlikely(r = spadfs_increase_cc(fs, 1))) {
			if (r > 0)
				return 0;	/* it is read-only now */
			return r;
		}
	} else {
		if (unlikely(r = spadfs_update_error_flags(fs)))
			return r;
	}
	CPU2SPAD32_LV(&fs->cct[fs->cc], SPAD2CPU32_LV(&fs->cct[fs->cc]) + 1);
	fs->txc = SPAD2CPU32_LV(&fs->cct[fs->cc]);
	return 0;
}

int spadfs_commit_unlocked(SPADFS *fs)
{
	int r, rr;
	unsigned sector;
	void *cct_data;
	struct buffer_head *bh;

	assert_write_sync_lock(fs);

	fs->commit_sequence++;

	if (unlikely(sb_rdonly(fs->s)))
		return -EFSERROR;

	while (unlikely(!list_empty(&fs->clear_list))) {
		SPADFNODE *f = list_entry(fs->clear_list.prev, SPADFNODE, clear_entry);
		spadfs_clear_last_block(&f->vfs_inode);
	}

	mutex_lock(&fs->alloc_lock);
#ifdef SPADFS_META_PREALLOC
	spadfs_prealloc_discard_unlocked(fs, &fs->meta_prealloc);
#endif
	spadfs_prealloc_discard_unlocked(fs, &fs->small_prealloc);
	if (unlikely(fs->max_freed_run > fs->max_allocation))
		fs->max_allocation = fs->max_freed_run;
	fs->max_freed_run = 0;
	mutex_unlock(&fs->alloc_lock);

#ifdef SPADFS_QUOTA
	mutex_lock(&fs->quota_alloc_lock);
#endif

	r = sync_blockdev(fs->s->s_bdev);
	if (unlikely(r))
		spadfs_error(fs, TXFLAGS_IO_WRITE_ERROR, "write error: %d", r);

	sector = fs->cc / (512 / 4);
	sector &= ~((1U << fs->sectors_per_buffer_bits) - 1);
	cct_data = spadfs_get_new_sector(fs, fs->cct_sec + sector, &bh,
					 "spadfs_commit_unlocked");
	if (unlikely(IS_ERR(cct_data))) {
		if (!r)
			r = PTR_ERR(cct_data);
		goto unlock_ret;
	}

	start_atomic_buffer_modify(fs, bh);
	memcpy(cct_data, (char *)fs->cct + (sector << 9), 512U << fs->sectors_per_buffer_bits);
	end_atomic_buffer_modify(fs, bh);

	rr = spadfs_sync_dirty_buffer(bh);
	spadfs_brelse(fs, bh);

	if (likely(!rr))
		rr = spadfs_issue_flush(fs);

	if (unlikely(rr != 0)) {
		spadfs_error(fs, TXFLAGS_IO_WRITE_ERROR,
			"write error in crash count table: %d", rr);
		spadfs_update_error_flags(fs);
		if (!r)
			r = rr;
		goto unlock_ret;
	}

	rr = spadfs_start_new_tx(fs);
	if (unlikely(rr)) {
		if (!r)
			r = rr;
	}

	if (likely(!r))
		fs->need_background_sync = 0;

unlock_ret:

#ifdef SPADFS_QUOTA
	mutex_unlock(&fs->quota_alloc_lock);
#endif

	return r;
}

int spadfs_commit(SPADFS *fs)
{
	/*sync_lock_decl*/
	int r, rr;

	if (unlikely(sb_rdonly(fs->s)))
		return 0;

	/*
	 * Improves performance very much --- do most of the syncing
	 * outside the lock.
	 */
	r = sync_blockdev(fs->s->s_bdev);
	if (unlikely(r))
		spadfs_error(fs, TXFLAGS_IO_WRITE_ERROR, "write error: %d", r);

	down_write_sync_lock(fs);
	rr = spadfs_commit_unlocked(fs);
	if (unlikely(rr))
		r = rr;
	up_write_sync_lock(fs);

	return r;
}

static int spadfs_sync_fs(struct super_block *s, int wait)
{
	SPADFS *fs = spadfs(s);
	if (!READ_ONCE(fs->need_background_sync))
		return 0;
	else if (!wait)
		return filemap_fdatawrite(s->s_bdev->bd_inode->i_mapping);
	else
		return spadfs_commit(spadfs(s));
}

#ifndef NEW_WORKQUEUE
static void spadfs_sync_work(void *fs_)
{
	SPADFS *fs = fs_;
#else
static void spadfs_sync_work(struct work_struct *w)
{
	SPADFS *fs = container_of(w, SPADFS, spadfs_sync_work.work);
#endif
	spadfs_reclaim_max_allocation(fs);
	if (READ_ONCE(fs->need_background_sync)) {
		spadfs_commit(fs);
	}
	queue_delayed_work(fs->spadfs_syncer, &fs->spadfs_sync_work,
			   fs->spadfs_sync_time);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,18)
static int spadfs_statfs(struct dentry *dentry, struct kstatfs *ks)
{
	struct super_block *s = dentry->d_sb;
#else
static int spadfs_statfs(struct super_block *s, struct kstatfs *ks)
{
#endif
	SPADFS *fs = spadfs(s);
	sector_t freespace;

	mutex_lock(&fs->alloc_lock);

	ks->f_type = s->s_magic;
	ks->f_bsize = 512U << fs->sectors_per_disk_block_bits;
	ks->f_blocks = fs->size >> fs->sectors_per_disk_block_bits;

	freespace = spadfs_get_freespace(fs);
	if (likely(freespace >= fs->alloc_mem_sectors))
		freespace -= fs->alloc_mem_sectors;
	else
		freespace = 0;

	ks->f_bfree = freespace >> fs->sectors_per_disk_block_bits;
	ks->f_bavail = likely(freespace >= fs->reserve_sectors) ?
		(freespace - fs->reserve_sectors) >> fs->sectors_per_disk_block_bits :
		0;

	ks->f_files = (u64)fs->zones[0].grp_n << (fs->sectors_per_group_bits - fs->sectors_per_disk_block_bits);
	if (unlikely(ks->f_files > ks->f_blocks))
		ks->f_files = ks->f_blocks;
	ks->f_ffree = fs->zones[0].freespace >> fs->sectors_per_disk_block_bits;

	ks->f_namelen = MAX_NAME_LEN;

	mutex_unlock(&fs->alloc_lock);

	return 0;
}

__cold static int spadfs_remount_fs(struct super_block *s, int *flags, char *data)
{
	int r, o;
	SPADFS *fs = spadfs(s);
	/*sync_lock_decl*/

	down_write_sync_lock(fs);

	o = parse_opts(fs, data, 1);
	if (!o) {
		r = -EINVAL;
		goto unlock_ret_r;
	}
	if (o == 2) {
		spadfs_help();
		r = -EAGAIN;
		goto unlock_ret_r;
	}

	if ((*flags ^ s->s_flags) & SB_RDONLY) {
		if (*flags & SB_RDONLY) {
#ifdef SPADFS_QUOTA
			if ((r = dquot_suspend(s, -1))) {
				*flags &= ~SB_RDONLY;
				goto unlock_ret_r;
			}
#endif
			if ((r = spadfs_commit_unlocked(fs)))
				goto unlock_ret_r;
			if ((r = spadfs_increase_cc(fs, -1)))
				goto unlock_ret_r;
			CPU2SPAD32_LV(&fs->cct[fs->cc],
				      SPAD2CPU32_LV(&fs->cct[fs->cc]) - 1);
		} else {
			if ((r = spadfs_increase_cc(fs, 1))) {
				if (r < 0)
					goto unlock_ret_r;
				*flags |= SB_RDONLY;
			}
			if ((r = spadfs_start_new_tx(fs)))
				goto unlock_ret_r;
#ifdef SPADFS_QUOTA
			dquot_resume(s, -1);
#endif
		}
	}

	r = 0;

unlock_ret_r:
	up_write_sync_lock(fs);
	return r;
}

#if defined(SPADFS_QUOTA) && SPADFS_QUOTA >= 3
static const struct quotactl_ops spadfs_quotactl_ops = {
	.quota_on = dquot_quota_on,
	.quota_off = dquot_quota_off,
	.quota_sync = dquot_quota_sync,
	.get_state = dquot_get_state,
	.set_info = dquot_set_dqinfo,
	.get_dqblk = dquot_get_dqblk,
	.set_dqblk = dquot_set_dqblk,
	.get_nextdqblk = dquot_get_next_dqblk,
};
#endif

__cold static int spadfs_fill_super(struct super_block *s, void *options, int silent)
{
	SPADFS *fs;
	const char *wq_name =
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,13)
		"spadfssync";
#else
		"spadfs_syncer";
#endif
	int blocksize;
	int r;
	int o;
	unsigned i, n;
	sector_t sec;
	struct buffer_head *bh;
	struct superblock *sb;
	struct txblock *tb;
	const char *msg;
	int cycle;
	struct inode *root;
	size_t sizeof_spadfs = sizeof(SPADFS)
#ifndef SIMPLE_SYNC_LOCK
		+ spadfs_nr_cpus * sizeof(struct rw_semaphore *)
#endif
		;

	sb = kmalloc(512, GFP_KERNEL);
	if (unlikely(!sb)) {
		r = -ENOMEM;
		goto err0;
	}

	if (unlikely(!(fs = spadfs_alloc(1, sizeof_spadfs, 0)))) {
		r = -ENOMEM;
		goto err1;
	}

	s->s_fs_info = fs;
	s->s_flags |= SB_NOATIME;
	s->s_magic = MEMORY_SUPER_MAGIC;
	s->s_op = &spadfs_sops;
#ifdef SPADFS_QUOTA
	s->dq_op = &dquot_operations;
#if SPADFS_QUOTA >= 3
	s->s_qcop = &spadfs_quotactl_ops;
#endif
#if SPADFS_QUOTA >= 2
	s->s_quota_types = QTYPE_MASK_USR | QTYPE_MASK_GRP;
#endif
#endif
#ifdef SPADFS_XATTR
	s->s_xattr =
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,35)
		(struct xattr_handler **)
#endif
		spadfs_xattr_handlers;
#endif
	if (sizeof(sector_t) == 4)
		s->s_maxbytes = ((u64)512 << 32) - 512;
	else
		s->s_maxbytes = ((u64)512 << 48) - 512;
	if (s->s_maxbytes > MAX_LFS_FILESIZE)
		s->s_maxbytes = MAX_LFS_FILESIZE;
	memset(fs, 0, sizeof_spadfs);
	fs->s = s;
	mutex_init(&fs->alloc_lock);
	spadfs_allocmem_init(fs);
	fs->alloc_reservations = RB_ROOT;
	mutex_init(&fs->inode_list_lock);
	INIT_LIST_HEAD(&fs->apage_lru);
#ifdef SPADFS_QUOTA
	mutex_init(&fs->quota_alloc_lock);
#endif
	spin_lock_init(&fs->stable_ino_lock);
	spin_lock_init(&fs->txflags_new_lock);
	fs->commit_sequence = 1;
	mutex_init(&fs->trim_lock);

	spin_lock_init(&fs->clear_lock);
	INIT_LIST_HEAD(&fs->clear_list);

	spadfs_buffer_leaks_init(fs);

	init_sync_lock(fs, r = -ENOMEM; goto err2;);

	fs->uid = get_current_uid();
	fs->gid = get_current_gid();
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,30)
	fs->mode = 0777 & ~current->fs->umask;
#else
	fs->mode = 0777 & ~current_umask();
#endif
	set_prealloc_part(fs, SPADFS_DEFAULT_PREALLOC_PART);
	fs->min_prealloc = SPADFS_DEFAULT_MIN_PREALLOC;
	fs->max_prealloc = SPADFS_DEFAULT_MAX_PREALLOC;
	fs->xfer_size = 0;
	fs->metadata_prefetch = SPADFS_DEFAULT_METADATA_PREFETCH >> 9;
	fs->spadfs_sync_time = SPADFS_DEFAULT_SYNC_TIME;

#ifdef CHECK_ALLOCMEM
	/*
	 * Unit test for allocmem. It is normally not used much, so we
	 * must stress-test it here.
	 */
	r = spadfs_allocmem_unit_test(fs);
	if (r)
		goto err2;
#endif

	fs->inode_list = spadfs_alloc(SPADFS_INODE_HASH_SIZE, sizeof(struct hlist_node), 0);
	if (!fs->inode_list) {
		r = -ENOMEM;
		goto err2;
	}
	for (i = 0; i < SPADFS_INODE_HASH_SIZE; i++)
		INIT_HLIST_HEAD(&fs->inode_list[i]);

	o = parse_opts(fs, options, 0);
	if (!o) {
		r = -EINVAL;
		goto err2;
	}
	if (o == 2) {
		spadfs_help();
		r = -EAGAIN;
		goto err2;
	}

	if (!fs->buffer_size) {
		blocksize = sb_min_blocksize(s, 512);
		if (unlikely(!blocksize)) {
			if (!silent)
				printk(KERN_ERR "spadfs: unable to set blocksize\n");
			r = -EIO;
			goto err2;
		}
	} else {
		if (unlikely(!sb_set_blocksize(s, fs->buffer_size))) {
			printk(KERN_ERR "spadfs: can't set blocksize %d\n", fs->buffer_size);
			r = -EINVAL;
			goto err2;
		}
		blocksize = fs->buffer_size;
	}

	cycle = 0;

read_again:
	if (unlikely(blocksize & (blocksize - 1)) || unlikely(blocksize < 512))
		panic("spadfs: wrong blocksize: %d", blocksize);
	fs->sectors_per_buffer_bits = ffs(blocksize) - 1 - 9;
	fs->size = (sector_t)-1;

	bh = sb_bread(s, SUPERBLOCK_SECTOR >> fs->sectors_per_buffer_bits);
	if (!bh) {
		if (!silent)
			printk(KERN_ERR "spadfs: unable to read super block\n");
		r = -EIO;
		goto err2;
	}
	memcpy(sb, spadfs_buffer_data(fs, SUPERBLOCK_SECTOR, bh), 512);
	__brelse(bh);

	if (unlikely(memcmp(sb->signature, SUPERBLOCK_SIGNATURE, sizeof sb->signature))) {
		if (!silent)
			printk(KERN_ERR "spadfs: superblock not found\n");
		r = -EINVAL;
		goto err2;
	}
	if (SPAD2CPU64_LV(&sb->byte_sex) != 0x0123456789ABCDEFLL) {
		if (!silent)
			printk(KERN_ERR "spadfs: byte sex does not match: %16llx\n",
				(unsigned long long)SPAD2CPU64_LV(&sb->byte_sex));
		r = -EINVAL;
		goto err2;
	}
	if (unlikely(sb->version_major != SPADFS_VERSION_MAJOR)) {
		if (!silent)
			printk(KERN_ERR "spadfs: bad major version number (disk %d.%d.%d, driver %d.%d.%d)\n",
				sb->version_major,
				sb->version_middle,
				sb->version_minor,
				SPADFS_VERSION_MAJOR,
				SPADFS_VERSION_MIDDLE,
				SPADFS_VERSION_MINOR);
		r = -EINVAL;
		goto err2;
	}
	if (unlikely(SPAD2CPU32_LV(&sb->flags_compat_rw) & ~(0)) ||
	    unlikely(SPAD2CPU32_LV(&sb->flags_compat_ro) & ~(
				FLAG_COMPAT_RO_DIRECTORY_SIZES)) ||
	    unlikely(SPAD2CPU32_LV(&sb->flags_compat_none) & ~(
				FLAG_COMPAT_NONE_UNIX_NAMES |
				FLAG_COMPAT_NONE_DYNAMIC_MAGIC |
				FLAG_COMPAT_NONE_EXTRA_SPACE))) {
		if (!silent)
			printk(KERN_ERR "spadfs: incompatible feature flags in superblock: %08x/%08x/%08x/%08x\n",
				(unsigned)SPAD2CPU32_LV(&sb->flags_compat_fsck),
				(unsigned)SPAD2CPU32_LV(&sb->flags_compat_rw),
				(unsigned)SPAD2CPU32_LV(&sb->flags_compat_ro),
				(unsigned)SPAD2CPU32_LV(&sb->flags_compat_none));
		r = -EINVAL;
		goto err2;
	}
	if (unlikely(__byte_sum(sb, 512) != CHECKSUM_BASE)) {
		spadfs_error(fs, TXFLAGS_FS_ERROR, "bad checksum on superblock: %02x", __byte_sum(sb, 512) ^ CHECKSUM_BASE);
		r = -EFSERROR;
		goto err2;
	}
	if (unlikely((u64)(sector_t)SPAD2CPU64_LV(&sb->size) != SPAD2CPU64_LV(&sb->size))) {
		if (!silent)
			printk(KERN_ERR "spadfs: 48-bit filesystem not supported by vfs layer - enable it in kernel configuration\n");
		r = -EINVAL;
		goto err2;
	}
	if (unlikely((msg = validate_super(sb)) != NULL)) {
		spadfs_error(fs, TXFLAGS_FS_ERROR, "invalid superblock: %s", msg);
		r = -EFSERROR;
		goto err2;
	}
	fs->sectors_per_disk_block_bits = sb->sectors_per_block_bits;
	if (!fs->buffer_size) {
		unsigned char wanted_sectors_per_buffer_bits = min(fs->sectors_per_disk_block_bits, (unsigned char)(PAGE_SHIFT - 9));
		if (unlikely(fs->sectors_per_buffer_bits != wanted_sectors_per_buffer_bits)) {
			if (unlikely(cycle)) {
				if (!silent)
					printk(KERN_ERR "spadfs: superblock changed while setting device size\n");
				r = -EIO;
				goto err2;
			}
			blocksize = 512U << wanted_sectors_per_buffer_bits;
			if (unlikely(!sb_set_blocksize(s, blocksize))) {
				if (!silent)
					printk(KERN_ERR "spadfs: can't set blocksize %d\n", blocksize);
				r = -EINVAL;
				goto err2;
			}
			cycle = 1;
			goto read_again;
		}
	} else {
		if (fs->sectors_per_disk_block_bits < fs->sectors_per_buffer_bits) {
			if (!silent)
				printk(KERN_ERR "spadfs: buffer size (%u) is greater than block size (%u)\n",
					512U << fs->sectors_per_buffer_bits, 512U << fs->sectors_per_disk_block_bits);
			r = -EINVAL;
			goto err2;
		}
	}

	fs->sectors_per_fnodepage_bits = sb->sectors_per_fnodepage_bits;
	fs->sectors_per_page_bits = sb->sectors_per_page_bits;
	fs->pagesize_bits = fs->sectors_per_page_bits + 9;
	fs->apage_size = 1U << (fs->pagesize_bits - 1);
	fs->dnode_hash_bits = fs->pagesize_bits - DNODE_PAGE_ENTRY_BITS - 1;
	fs->dnode_data_size = DNODE_PAGE_ENTRY_SIZE << fs->dnode_hash_bits;
	fs->dnode_page_sectors = spadfs_roundup_blocksize(fs,
			DNODE_ENTRY_OFFSET + (fs->dnode_data_size << 1));
	fs->dnode_page_sectors >>= 9;
	fs->sectors_per_cluster_bits = sb->sectors_per_cluster_bits;
	fs->cluster_threshold = (512U << fs->sectors_per_cluster_bits) *
				SPAD2CPU16_LV(&sb->cluster_threshold);
	fs->sectors_per_group_bits = sb->sectors_per_group_bits;
	if (fs->sectors_per_group_bits > sizeof(sector_t) * 8 - 1)
		fs->sectors_per_group_bits = sizeof(sector_t) * 8 - 1;
	fs->group_mask = ((sector_t)1 << fs->sectors_per_group_bits) - 1;
	fs->n_apage_mappings = fs->apage_size >> (fs->sectors_per_buffer_bits + 9);
	if (unlikely(!fs->n_apage_mappings))
		fs->n_apage_mappings = 1;

	fs->txb_sec = SPAD2CPU64_LV(&sb->txblock);
	fs->apage_index0_sec = SPAD2CPU64_LV(&sb->apage_index[0]);
	fs->apage_index1_sec = SPAD2CPU64_LV(&sb->apage_index[1]);
	fs->cct_sec = SPAD2CPU64_LV(&sb->cct);
	fs->root_sec = SPAD2CPU64_LV(&sb->root);
	fs->size = SPAD2CPU64_LV(&sb->size);
	fs->reserve_sectors = SPAD2CPU64_LV(&sb->reserve_sectors);

	fs->flags_compat_fsck = SPAD2CPU32_LV(&sb->flags_compat_fsck);
	fs->flags_compat_rw = SPAD2CPU32_LV(&sb->flags_compat_rw);
	fs->flags_compat_ro = SPAD2CPU32_LV(&sb->flags_compat_ro);
	fs->flags_compat_none = SPAD2CPU32_LV(&sb->flags_compat_none);

	if (fs->mount_flags & MOUNT_FLAGS_CHECKSUMS_OVERRIDE) {
		if (fs->mount_flags & MOUNT_FLAGS_CHECKSUMS)
			fs->flags_compat_fsck &= ~FLAG_COMPAT_FSCK_NO_CHECKSUMS;
		else
			fs->flags_compat_fsck |= FLAG_COMPAT_FSCK_NO_CHECKSUMS;
	}

	if (!fs->xfer_size) {
		fs->xfer_size = fs->cluster_threshold;
		if (fs->xfer_size > SPADFS_DEFAULT_MAX_XFER_SIZE)
			fs->xfer_size = SPADFS_DEFAULT_MAX_XFER_SIZE;
	}
	for (i = 512; i < INT_MAX / 2; i <<= 1)
		if (i >= fs->xfer_size)
			break;
	fs->xfer_size = i;

	if (unlikely(IS_ERR(tb = spadfs_read_tx_block(fs, &bh,
						"spadfs_fill_super 1")))) {
		r = PTR_ERR(tb);
		goto err2;
	}
	fs->cc = SPAD2CPU16_LV(&tb->cc);
	fs->a_cc = tb->a_cc;  /* fs->a_cc and fs->a_txc are in disk-endianity */
	fs->a_txc = tb->a_txc;
	fs->txflags = SPAD2CPU32_LV(&tb->txflags);

	if (unlikely(fs->txflags & TXFLAGS_DIRTY))
		spadfs_error(fs, 0,
		     "filesystem was not cleanly unmounted");
	if (unlikely(fs->txflags & TXFLAGS_IO_READ_ERROR))
		spadfs_error(fs, 0,
		     "filesystem has encountered read i/o error");
	if (unlikely(fs->txflags & TXFLAGS_IO_WRITE_ERROR))
		spadfs_error(fs, 0,
		     "filesystem has encountered write i/o error");
	if (unlikely(fs->txflags & TXFLAGS_FS_ERROR))
		spadfs_error(fs, 0,
		     "filesystem has errors in structures");
	if (unlikely(fs->txflags & TXFLAGS_CHECKSUM_ERROR))
		spadfs_error(fs, 0,
		     "filesystem structures have bad checksums");
	if (unlikely(fs->txflags & TXFLAGS_EA_ERROR))
		spadfs_error(fs, 0,
		     "filesystem structures has errors in extended attributes");
	if (unlikely(fs->txflags & ~(
		TXFLAGS_DIRTY |
		TXFLAGS_IO_READ_ERROR |
		TXFLAGS_IO_WRITE_ERROR |
		TXFLAGS_FS_ERROR |
		TXFLAGS_CHECKSUM_ERROR |
		TXFLAGS_EA_ERROR)))
			spadfs_error(fs, 0,
				"filesystem has unknown error flags %08x",
				fs->txflags);
	if (unlikely(fs->txflags) && !sb_rdonly(s))
		spadfs_error(fs, 0, "running spadfsck is recommended");

	spadfs_brelse(fs, bh);

	if (unlikely(!(fs->cct = spadfs_alloc(1, CCT_SIZE, 0)))) {
		r = -ENOMEM;
		goto err3;
	}

	for (i = 0; i < CCT_SIZE / 512; i += 1U << fs->sectors_per_buffer_bits) {
		void *p = spadfs_read_sector(fs, fs->cct_sec + i, &bh,
				CCT_SIZE / 512 - i, "spadfs_fill_super 2");
		if (unlikely(IS_ERR(p))) {
			r = PTR_ERR(p);
			goto err3;
		}
		memcpy((u8 *)fs->cct + (i << 9), p, 512U << fs->sectors_per_buffer_bits);
		spadfs_brelse(fs, bh);
	}
	fs->txc = SPAD2CPU32_LV(&fs->cct[fs->cc]);

	fs->n_apages = N_APAGES(fs->size, fs->pagesize_bits, fs->sectors_per_disk_block_bits + 9);
	n = APAGE_INDEX_SECTORS(fs->n_apages, 512U << fs->sectors_per_disk_block_bits);

	fs->apage_index = spadfs_alloc(n, 512, sizeof(struct apage_index_entry));
	if (unlikely(!fs->apage_index)) {
		r = -ENOMEM;
		goto err3;
	}
	memset(fs->apage_index, 0, sizeof(struct apage_index_entry));
	fs->apage_index++;

	sec = CC_VALID(fs, &fs->a_cc, &fs->a_txc) ?
				fs->apage_index0_sec : fs->apage_index1_sec;
	for (i = 0; i < n;
	     i += 1U << fs->sectors_per_buffer_bits,
	     sec += 1U << fs->sectors_per_buffer_bits) {
		void *p = spadfs_read_sector(fs, sec, &bh, n - i, "spadfs_fill_super 3");
		if (unlikely(IS_ERR(p))) {
			r = PTR_ERR(p);
			goto err3;
		}
		memcpy((u8 *)fs->apage_index + ((unsigned long)i << 9), p, 512U << fs->sectors_per_buffer_bits);
		spadfs_brelse(fs, bh);
	}

	fs->apage_info = spadfs_alloc(fs->n_apages, sizeof(struct apage_info), 0);
	if (unlikely(!fs->apage_info)) {
		r = -ENOMEM;
		goto err3;
	}
	for (i = 0; i < fs->n_apages; i++) {
		fs->apage_info[i].mapping[0].map = NULL;
		spadfs_cond_resched();
	}
	for (i = 0; i < fs->n_apages; i++)  {
		APAGE_MAP *m;
		if (unlikely(!(m = kmalloc(fs->n_apage_mappings * 2 * sizeof(APAGE_MAP), GFP_KERNEL)))) {
			r = -ENOMEM;
			goto err3;
		}
		fs->apage_info[i].mapping[0].map = m;
		fs->apage_info[i].mapping[1].map = m + fs->n_apage_mappings;
		memset(m, 0, fs->n_apage_mappings * 2 * sizeof(APAGE_MAP));
		spadfs_cond_resched();
	}

	fs->n_groups = (fs->size + (u64)fs->group_mask) >> fs->sectors_per_group_bits;
	fs->zones[0].grp_start = 0;
	fs->zones[0].grp_n = SPAD2CPU16_LV(&sb->small_file_group);
	fs->zones[1].grp_start = SPAD2CPU16_LV(&sb->small_file_group);
	fs->zones[1].grp_n = SPAD2CPU16_LV(&sb->large_file_group) -
					SPAD2CPU16_LV(&sb->small_file_group);
	fs->zones[2].grp_start = SPAD2CPU16_LV(&sb->large_file_group);
	fs->zones[2].grp_n = fs->n_groups -
					SPAD2CPU16_LV(&sb->large_file_group);

	fs->group_info = spadfs_alloc(fs->n_groups, sizeof(struct spadfs_group_info), 0);
	if (unlikely(!fs->group_info)) {
		r = -ENOMEM;
		goto err3;
	}
	memset(fs->group_info, 0, fs->n_groups * sizeof(struct spadfs_group_info));
	for (i = 0; i < fs->n_groups; i++) {
		if (i < fs->zones[1].grp_start)
			fs->group_info[i].zone = &fs->zones[0];
		else if (i < fs->zones[2].grp_start)
			fs->group_info[i].zone = &fs->zones[1];
		else
			fs->group_info[i].zone = &fs->zones[2];
	}

	fs->tmp_map = kmalloc(fs->n_apage_mappings * sizeof(APAGE_MAP),
			      GFP_KERNEL);
	if (unlikely(!fs->tmp_map)) {
		r = -ENOMEM;
		goto err3;
	}
	memset(fs->tmp_map, 0, fs->n_apage_mappings * sizeof(APAGE_MAP));
	for (i = 0; i < fs->n_apage_mappings; i++) {
		fs->tmp_map[i].entry = kmalloc(512U << fs->sectors_per_buffer_bits, GFP_KERNEL);
		if (unlikely(!fs->tmp_map[i].entry)) {
			r = -ENOMEM;
			goto err3;
		}
	}

	for (i = 0; i < fs->n_apages; i++)
		if (!SPAD2CPU64_LV(&fs->apage_index[i].end_sector))
			break;
	fs->n_active_apages = i;

	r = spadfs_count_free_space(fs);
	if (r)
		goto err3;

	if (!sb_rdonly(s)) {
		if ((r = spadfs_increase_cc(fs, 1))) {
			if (r < 0)
				goto err3;
			s->s_flags |= SB_RDONLY;
		}
		if ((r = spadfs_start_new_tx(fs)))
			goto err4;
	}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,38)
	if (unlikely(!(fs->flags_compat_none & FLAG_COMPAT_NONE_UNIX_NAMES)))
		s->s_d_op = &spadfs_dops;
#endif
	root = spadfs_iget(s, make_fixed_spadfs_ino_t(fs->root_sec), 0, 0);
	if (unlikely(IS_ERR(root))) {
		r = PTR_ERR(root);
		goto err4;
	}
#if LINUX_VERSION_CODE < KERNEL_VERSION(3,3,0)
	s->s_root = d_alloc_root(root);
#else
	s->s_root = d_make_root(root);
#endif
	if (unlikely(!s->s_root)) {
#if LINUX_VERSION_CODE < KERNEL_VERSION(3,3,0)
		iput(root);
#endif
		printk(KERN_ERR "spadfs: unable to allocate root dentry\n");
		r = -ENOMEM;
		goto err4;
	}

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,38)
	spadfs_set_dentry_operations(fs, s->s_root);
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,20)
	fs->spadfs_syncer = create_singlethread_workqueue(wq_name);
#elif LINUX_VERSION_CODE < KERNEL_VERSION(2,6,38)
	fs->spadfs_syncer = create_freezeable_workqueue(wq_name);
#else
	fs->spadfs_syncer = create_freezable_workqueue(wq_name);
#endif
	if (unlikely(!fs->spadfs_syncer)) {
		r = -ENOMEM;
		goto err4;
	}
#ifndef NEW_WORKQUEUE
	INIT_WORK(&fs->spadfs_sync_work, spadfs_sync_work, fs);
#else
	INIT_DELAYED_WORK(&fs->spadfs_sync_work, spadfs_sync_work);
#endif
	queue_delayed_work(fs->spadfs_syncer, &fs->spadfs_sync_work, fs->spadfs_sync_time);

	kfree(sb);

	return 0;

err4:
	if (!sb_rdonly(s))
		spadfs_increase_cc(fs, -1);
err3:
	spadfs_update_error_flags(fs);
err2:
	spadfs_free_super(s);
err1:
	kfree(sb);
err0:
	return r;
}

__cold static void spadfs_free_super(struct super_block *s)
{
	SPADFS *fs = spadfs(s);
	unsigned i;

	if (fs->spadfs_syncer) {
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,38)
		cancel_rearming_delayed_workqueue(fs->spadfs_syncer,
						  &fs->spadfs_sync_work);
#else
		cancel_delayed_work_sync(&fs->spadfs_sync_work);
#endif
		destroy_workqueue(fs->spadfs_syncer);
	}

	spadfs_allocmem_done(fs);

	if (fs->apage_info) {
		spadfs_prune_cached_apage_buffers(fs);
		for (i = 0; i < fs->n_apages; i++) {
			kfree(fs->apage_info[i].mapping[0].map);
			spadfs_cond_resched();
		}
		kvfree(fs->apage_info);
	}
	spadfs_cond_resched();

	if (fs->tmp_map) {
		for (i = 0; i < fs->n_apage_mappings; i++)
			kfree(fs->tmp_map[i].entry);
		kfree(fs->tmp_map);
	}
	spadfs_cond_resched();

	kvfree(fs->group_info);
	spadfs_cond_resched();

	if (fs->apage_index)
		kvfree(fs->apage_index - 1);
	spadfs_cond_resched();

	kvfree(fs->cct);

	if (fs->inode_list) {
		for (i = 0; i < SPADFS_INODE_HASH_SIZE; i++)
			if (unlikely(!hlist_empty(&fs->inode_list[i])))
				panic("spadfs: spadfs_free_super: inodes leaked");
		kvfree(fs->inode_list);
	}

	done_sync_lock(fs);

	spadfs_buffer_leaks_done(fs);

	mutex_destroy(&fs->alloc_lock);
	mutex_destroy(&fs->inode_list_lock);
#ifdef SPADFS_QUOTA
	mutex_destroy(&fs->quota_alloc_lock);
#endif
	mutex_destroy(&fs->trim_lock);

	kvfree(fs);

	s->s_fs_info = NULL;
}

__cold static void spadfs_put_super(struct super_block *s)
{
	SPADFS *fs = spadfs(s);

#ifdef SPADFS_QUOTA
	dquot_disable(s, -1, DQUOT_USAGE_ENABLED | DQUOT_LIMITS_ENABLED);
#endif

	/*
	 * If it didn't commit due to I/O error, do not decrease crash count
	 * --- act as if crash happened
	 */
	if (!spadfs_commit(fs)) {
		if (fs->mount_flags & MOUNT_FLAGS_CLEAR_DIRTY_ON_UNMOUNT) {
			fs->txflags &= ~TXFLAGS_DIRTY;
			spin_lock_irq(&fs->txflags_new_lock);
			fs->txflags_new &= ~TXFLAGS_DIRTY;
			spin_unlock_irq(&fs->txflags_new_lock);
		}
		if (!sb_rdonly(s))
			spadfs_increase_cc(fs, -1);
	} else {
		spadfs_error(fs, TXFLAGS_IO_WRITE_ERROR,
			"i/o error at unmount - not commiting");
		spadfs_update_error_flags(fs);
	}
	/*{
		int i;
		printk("groups: ");
		for (i = 0; i < fs->n_groups; i++)
			printk(" %Ld", (long long)fs->group_info[i].freespace);
		printk("\nzones: %Ld %Ld %Ld\n",
			(long long)fs->zones[0].freespace,
			(long long)fs->zones[1].freespace,
			(long long)fs->zones[2].freespace);
	}*/
	spadfs_free_super(s);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,37)
__cold static struct dentry *spadfs_mount(struct file_system_type *fs_type, int flags,
				   const char *dev_name, void *data)
{
	return mount_bdev(fs_type, flags, dev_name, data, spadfs_fill_super);
}
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,18)
__cold static int spadfs_get_sb(struct file_system_type *fs_type, int flags,
			 const char *dev_name, void *data, struct vfsmount *mnt)
{
	return get_sb_bdev(fs_type, flags, dev_name, data, spadfs_fill_super,
			   mnt);
}
#else
__cold static struct super_block *spadfs_get_sb(struct file_system_type *fs_type,
					 int flags, const char *dev_name,
					 void *data)
{
	return get_sb_bdev(fs_type, flags, dev_name, data, spadfs_fill_super);
}
#endif

static
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,21)
const
#endif
struct super_operations spadfs_sops = {
	.alloc_inode = spadfs_alloc_inode,
	.destroy_inode = spadfs_destroy_inode,
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,36)
	.delete_inode = spadfs_delete_inode,
#else
	.evict_inode = spadfs_evict_inode,
#endif
	.put_super = spadfs_put_super,
	.sync_fs = spadfs_sync_fs,
	.statfs = spadfs_statfs,
	.remount_fs = spadfs_remount_fs,
	.show_options = spadfs_show_options,
#ifdef SPADFS_QUOTA
	.quota_read = spadfs_quota_read,
	.quota_write = spadfs_quota_write,
#if SPADFS_QUOTA >= 2
	.get_dquots = spadfs_quota_get,
#endif
#endif
};

static struct file_system_type spadfs_fs_type = {
	.owner = THIS_MODULE,
	.name = __stringify(SPADFS_NAME),
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,37)
	.mount = spadfs_mount,
#else
	.get_sb = spadfs_get_sb,
#endif
	.kill_sb = kill_block_super,
	.fs_flags = FS_REQUIRES_DEV
#if TEST_RHEL_VERSION(5,3)
		| FS_HAS_FALLOCATE
#endif
#if TEST_RHEL_VERSION(5,4)
		| FS_HAS_FIEMAP
#endif
};

#ifdef CONFIG_SMP
unsigned spadfs_nr_cpus;
int spadfs_unlocked_extent_cache;
unsigned spadfs_extent_cache_size;
#endif

__cold static void spadfs_free_caches(void)
{
	rcu_barrier();
	spadfs_free_cache(spadfs_inode_cachep);
	spadfs_free_cache(spadfs_ea_cachep);
	spadfs_free_cache(spadfs_extent_cachep);
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,31)
static int minimize_inode_size = 0;
#else
static bool minimize_inode_size = 0;
#endif
module_param(minimize_inode_size, bool, S_IRUGO);

__cold static int __init spadfs_init(void)
{
	int r;

	if (!spadfs_struct_check_correctness()) {
		printk(KERN_ERR "SPADFS: BAD STRUCTURE ALIGNMENT, THE PROGRAM WAS COMPILED WITH WRONG ABI\n");
		r = -EINVAL;
		goto ret_r;
	}

#ifdef CONFIG_SMP
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,15,0)
	{
		unsigned i;
		spadfs_nr_cpus = 1;
		for (i = 0; i < nr_cpumask_bits; i++)
			if (cpu_possible(i))
				spadfs_nr_cpus = i + 1;
	}
#else
	spadfs_nr_cpus = cpumask_last(cpu_possible_mask) + 1;
#endif

	spadfs_unlocked_extent_cache = sizeof(SPADFNODE) + spadfs_nr_cpus * sizeof(struct extent_cache) <= PAGE_SIZE;
	if (spadfs_nr_cpus > 1 && minimize_inode_size)
		spadfs_unlocked_extent_cache = 0;
	spadfs_extent_cache_size = spadfs_unlocked_extent_cache ? spadfs_nr_cpus : 1;
#endif

	spadfs_inode_cachep = kmem_cache_create(__stringify(SPADFS_NAME) "_inode_cache",
						sizeof(SPADFNODE) + spadfs_extent_cache_size * sizeof(struct extent_cache),
						FNODE_EA_ALIGN, SLAB_RECLAIM_ACCOUNT | SLAB_MEM_SPREAD | SLAB_ACCOUNT,
						fnode_ctor
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,23)
						, NULL
#endif
	);
	if (unlikely(!spadfs_inode_cachep)) {
		r = -ENOMEM;
		goto ret_r;
	}

	spadfs_ea_cachep = kmem_cache_create(__stringify(SPADFS_NAME) "_ea_cache",
					     FNODE_MAX_EA_SIZE,
					     FNODE_EA_ALIGN, SLAB_RECLAIM_ACCOUNT | SLAB_MEM_SPREAD | SLAB_ACCOUNT,
					     NULL
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,23)
					     , NULL
#endif
	);
	if (unlikely(!spadfs_ea_cachep)) {
		r = -ENOMEM;
		goto ret_r;
	}

	spadfs_extent_cachep = kmem_cache_create(__stringify(SPADFS_NAME) "_extent_cache",
						 sizeof(struct allocmem),
						 0, SLAB_MEM_SPREAD,
						 NULL
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,23)
						 , NULL
#endif
	);
	if (unlikely(!spadfs_extent_cachep)) {
		r = -ENOMEM;
		goto ret_r;
	}

	r = register_filesystem(&spadfs_fs_type);
	if (unlikely(r))
		goto ret_r;

	return 0;

ret_r:
	spadfs_free_caches();
	return r;
}

__cold static void __exit spadfs_exit(void)
{
	unregister_filesystem(&spadfs_fs_type);
	spadfs_free_caches();
}

module_init(spadfs_init)
module_exit(spadfs_exit)
MODULE_LICENSE("GPL");

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,9,0)
MODULE_ALIAS_FS(__stringify(SPADFS_NAME));
#endif

#if __GNUC__ <= 3 && BITS_PER_LONG == 32
/*
 * GCC 2.95.* has a bug --- it generates ".globl __udivdi3" into assembler
 * although it doesn't ever call this function. This causes module linking
 * problems.
 *
 * GCC 3.3.6 does it too. I have no idea why.
 */
void __udivdi3(void)
{
	printk(KERN_EMERG "__udivdi3 called!\n");
	BUG();
}
#endif
