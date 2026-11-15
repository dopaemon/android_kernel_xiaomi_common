#ifndef __SPADFS_H
#define __SPADFS_H

#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/rbtree.h>
#include <linux/delay.h>
#include <linux/bio.h>
#include <linux/fs.h>
#include <linux/aio.h>
#include <linux/mount.h>
#include <linux/seq_file.h>
#include <linux/buffer_head.h>
#include <linux/blkdev.h>
#include <linux/parser.h>
#include <linux/statfs.h>
#include <linux/vmalloc.h>
#include <linux/namei.h>
#include <linux/quotaops.h>
#include <linux/version.h>
#include <linux/compiler.h>
#include <linux/random.h>
#include <linux/uio.h>
#include <linux/xattr.h>
#include <linux/compat.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,11,0)
#include <linux/sched/signal.h>
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,8,0)
#include <linux/fiemap.h>
#endif

#include "endian.h"

#include "linux-compat.h"

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,23) || TEST_RHEL_VERSION(5,3)
#include <linux/falloc.h>
#endif

#ifdef SPADFS_LITTLE_ENDIAN
#define SPAD2CPU16		le16_to_cpu
#define CPU2SPAD16		cpu_to_le16
#define SPAD2CPU32		le32_to_cpu
#define CPU2SPAD32		cpu_to_le32
#define SPAD2CPU64		le64_to_cpu
#define CPU2SPAD64		cpu_to_le64
#define SPAD2CPU16_LV		le16_to_cpup
#define CPU2SPAD16_LV(p, v)	(*(p) = cpu_to_le16(v))
#define SPAD2CPU32_LV		le32_to_cpup
#define CPU2SPAD32_LV(p, v)	(*(p) = cpu_to_le32(v))
#define SPAD2CPU64_LV		le64_to_cpup
#define CPU2SPAD64_LV(p, v)	(*(p) = cpu_to_le64(v))
#elif defined(SPADFS_BIG_ENDIAN)
#define SPAD2CPU16		be16_to_cpu
#define CPU2SPAD16		cpu_to_be16
#define SPAD2CPU32		be32_to_cpu
#define CPU2SPAD32		cpu_to_be32
#define SPAD2CPU64		be64_to_cpu
#define CPU2SPAD64		cpu_to_be64
#define SPAD2CPU16_LV		be16_to_cpup
#define CPU2SPAD16_LV(p, v)	(*(p) = cpu_to_be16(v))
#define SPAD2CPU32_LV		be32_to_cpup
#define CPU2SPAD32_LV(p, v)	(*(p) = cpu_to_be32(v))
#define SPAD2CPU64_LV		be64_to_cpup
#define CPU2SPAD64_LV(p, v)	(*(p) = cpu_to_be64(v))
#endif
#define SPAD2CPU16_CONST	SPAD2CPU16
#define CPU2SPAD16_CONST	CPU2SPAD16
#define SPAD2CPU32_CONST	SPAD2CPU32
#define CPU2SPAD32_CONST	CPU2SPAD32
#define SPAD2CPU64_CONST	SPAD2CPU64
#define CPU2SPAD64_CONST	CPU2SPAD64

#define __make64(lo, hi)	(((u64)(hi) << 32) | (u32)(lo))
#define __finline__		inline

#include "struct.h"

/* #define CHECK_BUFFER_LEAKS */
/* #define CHECK_BUFFER_WRITES */
/* #define CHECK_BUFFER_WRITES_RANDOMIZE */
/* #define CHECK_ALLOCMEM */

#define SPADFS_RESURRECT
#define SPADFS_OPTIMIZE_FSYNC

#if !defined(CONFIG_SMP) || (defined(CONFIG_DEBUG_LOCK_ALLOC) && NR_CPUS > MAX_LOCKDEP_SUBCLASSES)
#define SIMPLE_SYNC_LOCK
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,17)
#define SPADFS_DIRECT_IO
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,24)
#define SPADFS_DO_PREFETCH
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,33)
#define SPADFS_XATTR
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,38) && defined(CONFIG_QUOTA)
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,12,0)
#define SPADFS_QUOTA	3
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(3,19,0)
#define SPADFS_QUOTA	2
#else
#define SPADFS_QUOTA	1
#endif
#endif

#if defined(FITRIM)
#define SPADFS_FSTRIM
#endif

/*#define SPADFS_META_PREALLOC*/

/* Allocate in other zones if the group has 1/2 avg free. Must be power of 2 */
#define SPADFS_AVG_FREE_DIVIDE_OTHERZONE	2

#define SPADFS_DEFAULT_PREALLOC_PART		8
#define SPADFS_DEFAULT_MIN_PREALLOC		4096
#define SPADFS_DEFAULT_MAX_PREALLOC		1048576
#define SPADFS_DEFAULT_METADATA_PREFETCH	524288
#define SPADFS_DEFAULT_MAX_XFER_SIZE		1048576
#define SPADFS_DEFAULT_SYNC_TIME		(120 * HZ)

#define SPADFS_INO_BATCH			65536
#if BITS_PER_LONG == 32
#define SPADFS_INODE_HASH_SIZE			32768
#else
#define SPADFS_INODE_HASH_SIZE			262144
#endif

typedef struct __spadfnode SPADFNODE;
typedef struct __spadfs SPADFS;

#define EFSERROR	EUCLEAN

#define MEMORY_SUPER_MAGIC	('S' | ('P' << 8) | ('A' << 16) | ('D' << 24))

static inline void spadfs_cond_resched(void)
{
#ifndef CONFIG_PREEMPT
	cond_resched();
#endif
}

/*
 * Internal and external inode numbers:
 *	SpadFS doesn't have limit on number of files, but Linux has only 32-bit
 *	inode numbers. So the inode numbers can't be used to determine files.
 *
 *	We use 64-bit value spadfs_ino_t to denote a file. This value has
 *	embedded sector number and position within sector. The value should be
 *	created with make_spadfs_ino_t and its parts can be extracted with
 *	spadfs_ino_t_sec and spadfs_ino_t_pos. Position must be >= 24, <= 448
 *	and dividable by 8, as is specified by disk layout.
 *
 *	Fixed fnodes have position "0" and sector number is the number of fixed
 *	fnode block. The are created with make_fixed_spadfs_ino_t and tested
 *	with is_pos_fixed or is_fnode_fixed.
 *
 *	spadfs_ino_t_deleted is the special value that is assigned to
 *	deleted-but-open files. They have no on-disk representation.
 *
 *	To keep userspace happy, we must manufacture 32-bit ino_t value somehow.
 *	Use the fact that no two fnodes can be less than 64-bytes apart --- so
 *	use top 3 bits of position and bottom 29 bits of sector number.
 *	spadfs_ino_t_2_ino_t does this. If the disk has at most 256GiB, ino_t
 *	will be collision-free, otherwise collisions are possible but unlikely.
 *
 *	Kernels starting with 2.6.19 can return 64-bit ino, so
 *	spadfs_ino_t_2_ino64_t returns 64-bit value on them.
 */

typedef u64 spadfs_ino_t;

#define make_spadfs_ino_t(sec, pos)	((sec) | ((u64)(pos) << 55))
#define make_fixed_spadfs_ino_t(sec)	make_spadfs_ino_t(sec, 0)
#define spadfs_ino_t_sec(ino)		((sector_t)(ino) & (sector_t)(((u64)1 << 48) - 1))
#define spadfs_ino_t_pos(ino)		((unsigned)((ino) >> 55))

#define spadfs_ino_t_2_ino_t(ino)	((((u32)((ino) >> 61) | ((u32)(ino) << 3)) & (SPADFS_INO_INITIAL_REGION - 1)) | SPADFS_INO_INITIAL_REGION)
#define spadfs_ino_t_2_ino64_t(ino)	((u64)spadfs_ino_t_2_ino_t(ino) | (((ino) << 5) & (((u64)1 << 53) - ((u64)1 << 32))))

#define spadfs_ino_t_deleted		((spadfs_ino_t)1)
#define is_pos_fixed(pos)		(!(pos))

#define is_fnode_fixed(fnode)		(is_pos_fixed(spadfs_ino_t_pos((fnode)->spadfs_ino)))

#define is_deleted_file(fnode)		(!(fnode)->fnode_block)

/*
 * Encoding directory position into 64-bit off_t. Ideally, the directory
 * position would need 32+48+3 bits (hash+block in chain+position), really we
 * strip block in chain to 29 bits --- for directories that have more than 2^29
 * blocks in hash-collision chain, readdir won't work.
 *
 * Rotate the bits somehow so that if old application accesses directory with
 * only 32-bit offset, it most time works (16 bits --- hash, 3 bits ---
 * position, 13 bits --- chain).
 *
 * Special directory offsets --- used to return entries "." and ".." that do not
 * really exist in the directory. Readdir can use macros is_dir_off_t_special,
 * dir_off_t_special_n and make_special_dir_off_t to implement simple state
 * machine.
 */

#define make_dir_off_t(hash, order, pos)	(((hash) & 0xffff) | ((((pos) + (0x40 - SIZEOF_FNODE_BLOCK)) & 0x1c0) << (16 - 6)) | (((order) & 0x1fff) << (16 + 3)) | (((u64)((hash) & 0xffff0000) << 16) | ((u64)((order) & 0x1fffe000) << (32 + 3))))
#define dir_off_t_hash(off)		(((u32)(off) & 0xffff) | ((u32)((off) >> 16) & 0xffff0000))
#define dir_off_t_order(off)		((((u32)(off) >> (16 + 3)) & 0x1fff) | ((u32)((off) >> (32 + 3)) & 0x1fffe000))
#define dir_off_t_pos(off)		((((u32)(off) >> (16 - 6)) & 0x1c0) - (0x40 - SIZEOF_FNODE_BLOCK))

#define is_dir_off_t_special(off)	(!((u32)(off) & (0x1c0 << (16 - 6))))
#define dir_off_t_special_n(off)	((u32)(off))
#define make_special_dir_off_t(n)	(n)

#define MAX_CHAIN_LENGTH		((1 << 29) - 1)

/*
 * One mapped apage. Really these structures form array of
 * [ pagesize / 2 * blocksize ] APAGE_MAPs.
 * Buffer head, entry is bh->b_data;
 */

typedef struct {
	struct aentry *entry;	/* [ blocksize / 16 ] */
	struct buffer_head *bh;
} APAGE_MAP;			/* [ pagesize / 2 / blocksize ] */

/*
 * Describes a zone. A total 3 zones exist --- metadata, small files, large
 * files.
 * grp_start of next zone must be grp_start+grp_n of previous zone, grp_start of
 * the first zone must be 0 --- a lot of code depends on it.
 */

struct spadfszone {
	sector_t freespace;
	unsigned grp_start;
	unsigned grp_n;
};

/*
 * Describes a group.
 */

struct spadfs_group_info {
	sector_t freespace;
	struct spadfszone *zone;
};

struct extent_cache {
	sector_t physical_sector;
	sector_t logical_sector;
	unsigned long n_sectors;
} ____cacheline_aligned_in_smp;

struct alloc_reservation {
	struct rb_node rb_node;
	sector_t start;
	unsigned len;
};

#define FNODE_EA_INLINE		((unsigned)sizeof(struct ea_unx))

struct __spadfnode {
	SPADFS *fs;
	loff_t disk_size; /* Size of file on disk (including prealloc) */
	/* If commit_sequence == fs->commit_sequence, then crash_disk_size is
	 * the valid file size in case of crash. */
	loff_t crash_disk_size;
	u64 commit_sequence;
	loff_t mmu_private;
	spadfs_ino_t spadfs_ino;
	spadfs_ino_t stable_ino;

	/* Copy of entries in fnode on disk */
	sector_t blk1;
	sector_t blk2;
	unsigned blk1_n;
	unsigned blk2_n;
	sector_t root;

	/* This fnode's and parent fnode's block and position. For fixed fnodes,
	 * parent values are 0. */
	sector_t fnode_block;
	sector_t parent_fnode_block;
	unsigned fnode_pos;
	unsigned parent_fnode_pos;

	/* List of all inodes */
	struct hlist_node inode_list;

	struct inode vfs_inode;

	/* Only for files. Directories are reasonably protected by the kernel */
	/* FIXME: can be removed too? will i_mutex do the right thing? It should
	   be checked */
	struct mutex file_lock;

	unsigned long target_blocks;
	int target_blocks_exact;
	int dont_truncate_prealloc;

	/* Real number of links, see comment on spadfs_set_nlink */
	u64 spadfs_nlink;

	struct alloc_reservation res;

	struct list_head clear_entry;
	sector_t clear_position;

#if defined(SPADFS_QUOTA) && SPADFS_QUOTA >= 2
	struct dquot *i_dquot[MAXQUOTAS];
#endif

	/* Pointer to UNX attribute within "ea" array to speed-up access.
	 * Or NULL if there is no UNX attribute. */
	struct ea_unx *ea_unx;

	/* Extended attributes */
	unsigned ea_size;
	u8 *ea;
	u8 ea_inline[FNODE_EA_INLINE] __attribute__((__aligned__(FNODE_EA_ALIGN)));

	/* Cache for one extent --- physical block, logical block, number of
	 * blocks */
	u64 extent_cache_seq;
	struct extent_cache extent_cache[0];
};

#define PREALLOC_TIMEOUT		HZ
#define PREALLOC_DISCARD_TIMEOUT	(15 * HZ)
#define PREALLOC_THRESHOLD		8

struct prealloc_state {
	unsigned long last_alloc;
	unsigned n_allocations;
	unsigned allocations_size;
	sector_t sector;
	unsigned n_sectors;
	pid_t pgrp;
};

struct __spadfs {
	u32 *cct; /* Crash count table --- an array of 65536 32-bit values */
	u16 cc; /* Current crash count */
	s32 txc; /* cct[cc] */
	s32 a_txc; /* txc/cc pair selecting one of two apage indices */
	u16 a_cc;

/* Values selected with mkspadfs. "_bits" suffix means log2 of the value */
	unsigned char sectors_per_buffer_bits;
	unsigned char sectors_per_disk_block_bits;
	unsigned char sectors_per_fnodepage_bits;
	unsigned char sectors_per_page_bits;
	unsigned char pagesize_bits;
	unsigned char dnode_hash_bits;
	unsigned char sectors_per_cluster_bits;
	unsigned char sectors_per_group_bits;
	unsigned apage_size;
	unsigned dnode_data_size;
	unsigned dnode_page_sectors;
	unsigned n_apage_mappings;
	u32 cluster_threshold;

/* Mount options */
	uid_t uid;
	gid_t gid;
	umode_t mode;
	s8 prealloc_part_bits;
	unsigned prealloc_part;
	unsigned min_prealloc;
	unsigned max_prealloc;
	unsigned metadata_prefetch;
	unsigned xfer_size;

/* Values read from superblock */
	sector_t txb_sec;
	sector_t apage_index0_sec;
	sector_t apage_index1_sec;
	sector_t cct_sec;
	sector_t root_sec;
	sector_t size;
	sector_t freespace;
	sector_t reserve_sectors;
	sector_t group_mask;
	unsigned max_allocation;
	unsigned max_freed_run;
	u32 flags_compat_fsck;
	u32 flags_compat_rw;
	u32 flags_compat_ro;
	u32 flags_compat_none;

	unsigned n_apages;		/* Total apages */
	unsigned n_active_apages;	/* Used apages */
	struct apage_index_entry *apage_index;
	struct apage_info *apage_info;
	unsigned cached_apage_buffers;
	unsigned mapped_apage_buffers;
	struct list_head apage_lru;

	unsigned n_groups;		/* Total groups */
	struct spadfs_group_info *group_info;	/* Group descriptors */
	struct spadfszone zones[3];		/* Zone descriptors */

	APAGE_MAP *tmp_map;
	struct mutex alloc_lock;
#ifdef SPADFS_META_PREALLOC
	struct prealloc_state meta_prealloc;
#endif
	struct prealloc_state small_prealloc;
	struct rb_root alloc_mem;
	sector_t alloc_mem_sectors;

	struct rb_root alloc_reservations;

	int need_background_sync;

	struct hlist_head *inode_list;/* List of all inodes */
	struct mutex inode_list_lock;/* List's lock */

#ifdef SPADFS_QUOTA
	/* Prevent quota file extension concurrently with commit */
	struct mutex quota_alloc_lock;
#endif

	struct super_block *s;	/* VFS part of superblock */

	u64 stable_ino;		/* Next free inode number */
	spinlock_t stable_ino_lock;	/* lock for "ino" */

	struct workqueue_struct *spadfs_syncer;	/* Syncer thread */
	unsigned long spadfs_sync_time;		/* Sync time */
#ifndef NEW_WORKQUEUE
	struct work_struct spadfs_sync_work;
#else
	struct delayed_work spadfs_sync_work;
#endif

	unsigned char mount_flags;
	unsigned char split_happened;

	u32 txflags;		/* Error flags */
	u32 txflags_new;	/* New error flags (if txflags_new & ~txflags,
				   error flags need to be written) */
	spinlock_t txflags_new_lock;

	u64 commit_sequence;

	struct mutex trim_lock;
	sector_t trim_start;
	unsigned trim_len;

	spinlock_t clear_lock;
	struct list_head clear_list;

	int buffer_size;

#ifdef CHECK_BUFFER_LEAKS
	struct mutex buffer_track_lock;
	struct hlist_head buffer_list;
	unsigned long buffer_oom_events;
#endif

#ifdef SIMPLE_SYNC_LOCK
	struct rw_semaphore sync_lock;
#else
	struct rw_semaphore *sync_locks[0];
#endif
};

#define MOUNT_FLAGS_CHECKSUMS_OVERRIDE		0x01
#define MOUNT_FLAGS_CHECKSUMS			0x02
#define MOUNT_FLAGS_CLEAR_DIRTY_ON_UNMOUNT	0x04
#define MOUNT_FLAGS_64BIT_INO			0x08
#define MOUNT_FLAGS_64BIT_INO_FORCE		0x10
#define MOUNT_FLAGS_USRQUOTA			0x20
#define MOUNT_FLAGS_GRPQUOTA			0x40

static inline u32 spadfs_magic(SPADFS *fs, sector_t block, u32 magic)
{
	if (likely(fs->flags_compat_none & FLAG_COMPAT_NONE_DYNAMIC_MAGIC))
		magic ^= block;
	return magic;
}

struct apage_mapping {
	APAGE_MAP *map;
	struct list_head list;
};

struct apage_info {
	struct apage_mapping mapping[2];
};

#define spadfs(s)	((SPADFS *)((s)->s_fs_info))
#define spadfnode(i)	list_entry(i, SPADFNODE, vfs_inode)
#define inode(i)	(&(i)->vfs_inode)

#ifndef CONFIG_SMP
#define spadfs_nr_cpus			1
#define spadfs_unlocked_extent_cache	1
#define spadfs_extent_cache_size	1
#else
extern unsigned spadfs_nr_cpus;
extern int spadfs_unlocked_extent_cache;
extern unsigned spadfs_extent_cache_size;
#endif

#ifdef SIMPLE_SYNC_LOCK
#define init_sync_lock(fs, fail)	init_rwsem(&(fs)->sync_lock)
#define done_sync_lock(fs)		do { } while (0)
#define down_read_sync_lock(fs)		(down_read(&(fs)->sync_lock))
#define up_read_sync_lock(fs)		(up_read(&(fs)->sync_lock))
#define down_write_sync_lock(fs)	(down_write(&(fs)->sync_lock))
#define up_write_sync_lock(fs)		(up_write(&(fs)->sync_lock))
#define sync_lock_decl
/* Linux doesn't allow us to differentiate between lock for read and write */
#define assert_write_sync_lock(fs)	BUG_ON(!rwsem_is_locked(&(fs)->sync_lock))
#else
void spadfs_do_down_read_sync_lock(SPADFS *fs, unsigned *cpu);
void spadfs_do_up_read_sync_lock(SPADFS *fs, unsigned cpu);
void spadfs_do_down_write_sync_lock(SPADFS *fs);
void spadfs_do_up_write_sync_lock(SPADFS *fs);
#define init_sync_lock(fs, fail)	do {				\
					if (spadfs_do_init_sync_lock(fs)) { \
						fail			\
					}				\
					} while (0)
#define done_sync_lock(fs)		spadfs_do_done_sync_lock(fs)
#define down_read_sync_lock(fs)		spadfs_do_down_read_sync_lock(fs, &cpu)
#define up_read_sync_lock(fs)		spadfs_do_up_read_sync_lock(fs, cpu)
#define down_write_sync_lock(fs)	spadfs_do_down_write_sync_lock(fs)
#define up_write_sync_lock(fs)		spadfs_do_up_write_sync_lock(fs)
#define sync_lock_decl			unsigned cpu;
/* Linux doesn't allow us to differentiate between lock for read and write */
#define assert_write_sync_lock(fs)	BUG_ON(!rwsem_is_locked((fs)->sync_locks[0]))
#endif

static inline int CC_VALID(SPADFS *fs, u16 *cc, s32 *txc)
{
	return (s32)(
		(u32)SPAD2CPU32_LV(&fs->cct[SPAD2CPU16_LV(cc)]) -
		(u32)SPAD2CPU32_LV((u32 *)txc)
		) >= 0;
}

static inline int CC_CURRENT(SPADFS *fs, u16 *cc, s32 *txc)
{
	return !(
		(SPAD2CPU16_LV(cc) ^ fs->cc) |
		(u32)((SPAD2CPU32_LV((u32 *)txc) ^ fs->txc) & 0x7fffffff)
		);
}

static inline void CC_SET_CURRENT(SPADFS *fs, u16 *cc, s32 *txc)
{
	CPU2SPAD32_LV(txc, (~(
		(u32)SPAD2CPU32_LV(&fs->cct[SPAD2CPU16_LV(cc)]) -
		(u32)SPAD2CPU32_LV((u32 *)txc)
		) & 0x80000000U) | fs->txc);
	CPU2SPAD16_LV(cc, fs->cc);
}

static inline void CC_SET_CURRENT_INVALID(SPADFS *fs, u16 *cc, s32 *txc)
{
	CPU2SPAD32_LV(txc, fs->txc ^ 0x80000000U);
	CPU2SPAD16_LV(cc, fs->cc);
}

/*
 * Use this if you don't want partially modified buffer be written to disk.
 */

static void set_need_background_sync(SPADFS *fs)
{
	if (unlikely(!READ_ONCE(fs->need_background_sync)))
		WRITE_ONCE(fs->need_background_sync, 1);
}

static inline void start_atomic_buffer_modify(SPADFS *fs,
					      struct buffer_head *bh)
{
	clear_buffer_dirty(bh);
	wait_on_buffer(bh);
}

static inline void end_atomic_buffer_modify(SPADFS *fs, struct buffer_head *bh)
{
	set_need_background_sync(fs);
	mark_buffer_dirty(bh);
}

/*
 * The same functionality, but can be called simultaneously for the same buffer.
 * Need to serialize.
 */

static inline void start_concurrent_atomic_buffer_modify(SPADFS *fs,
							 struct buffer_head *bh)
{
	lock_buffer(bh);
}

static inline void end_concurrent_atomic_buffer_modify(SPADFS *fs,
						       struct buffer_head *bh)
{
	set_need_background_sync(fs);
	unlock_buffer(bh);
	mark_buffer_dirty(bh);
}

static inline void end_concurrent_atomic_buffer_modify_nodirty(SPADFS *fs,
							struct buffer_head *bh)
{
	unlock_buffer(bh);
}

/*
 * The same as previous, but read-only access. Used when checking checksums.
 */

static inline void start_concurrent_atomic_buffer_read(SPADFS *fs,
						       struct buffer_head *bh)
{
	lock_buffer(bh);
}

static inline void end_concurrent_atomic_buffer_read(SPADFS *fs,
						     struct buffer_head *bh)
{
	unlock_buffer(bh);
}

#define check_checksums(fs)	\
		(!((fs)->flags_compat_fsck & FLAG_COMPAT_FSCK_NO_CHECKSUMS))
#define make_checksums(fs)	\
		(!((fs)->flags_compat_fsck & FLAG_COMPAT_FSCK_NO_CHECKSUMS))
#define directory_size(fs)	\
		((fs)->flags_compat_ro & FLAG_COMPAT_RO_DIRECTORY_SIZES)

static inline loff_t spadfs_roundup_blocksize(SPADFS *fs, loff_t size)
{
	unsigned mask = (512U << fs->sectors_per_disk_block_bits) - 1;
	return (size + mask) & ~(loff_t)mask;
}

/*
 * SpadFS allows more than 2^32 hardlinks to a file, however i_nlink is only
 * 32-bit. So keep internal 64-bit value "spadfs_nlink" and external value
 * i_nlink exposed to stat().
 */

static inline void spadfs_set_nlink(struct inode *inode)
{
	unsigned nlink = spadfnode(inode)->spadfs_nlink;
	if (unlikely(nlink != spadfnode(inode)->spadfs_nlink)) nlink = -1;
	set_nlink(inode, nlink);
}

/* alloc.c */

#define ALLOC_METADATA		0x00010000
#define ALLOC_SMALL_FILE	0x00020000
#define ALLOC_BIG_FILE		0x00040000
#define ALLOC_PARTIAL_AT_GOAL	0x00080000
#define ALLOC_NEW_GROUP_HINT	0x00100000
#ifdef SPADFS_RESURRECT
#define ALLOC_RESURRECT		0x00200000
#endif

#define ALLOC_FREE_FROM		0x80000000

#define ALLOC_MASK_MASK		0x0000FFFF
#define ALLOC_MASK_1		0x00000001
#define ALLOC_MASK(flags)	(((flags) / ALLOC_MASK_1) & ALLOC_MASK_MASK)

struct alloc {
	sector_t sector;
	sector_t top;
	unsigned n_sectors;
	unsigned extra_sectors;
	unsigned flags;
	struct alloc_reservation *reservation;
};

void spadfs_discard_reservation(SPADFS *fs, struct alloc_reservation *res);
void spadfs_prune_cached_apage_buffers(SPADFS *fs);
int spadfs_count_free_space(SPADFS *fs);
sector_t spadfs_get_freespace(SPADFS *fs);
int spadfs_alloc_blocks(SPADFS *fs, struct alloc *al);
int spadfs_free_blocks_unlocked(SPADFS *fs, sector_t start,
				sector_t n_sectors);

int spadfs_free_blocks_metadata(SPADFS *f, sector_t start,
				sector_t n_sectors);
void spadfs_prealloc_discard_unlocked(SPADFS *fs, struct prealloc_state *prealloc);
void spadfs_reclaim_max_allocation(SPADFS *fs);

/* allocmem.c */

struct allocmem {
	struct rb_node rb_node;
	sector_t start;
	sector_t len;
};

void spadfs_allocmem_init(SPADFS *fs);
void spadfs_allocmem_done(SPADFS *fs);
int spadfs_allocmem_find(SPADFS *fs, sector_t off, sector_t n_sec, sector_t *next_change);
int spadfs_allocmem_add(SPADFS *fs, sector_t off, sector_t n_sec);
void spadfs_allocmem_delete(SPADFS *fs, sector_t off, sector_t n_sec);
#ifdef SPADFS_FSTRIM
int spadfs_trim_fs(SPADFS *fs, u64 start, u64 end, u64 minlen, sector_t *result);
#endif
#ifdef CHECK_ALLOCMEM
int spadfs_allocmem_unit_test(SPADFS *fs);
#endif

/* buffer.c */

static inline void *spadfs_buffer_data(SPADFS *fs, sector_t secno, struct buffer_head *bh)
{
	return bh->b_data + (((unsigned long)secno & ((1U << fs->sectors_per_buffer_bits) - 1)) << 9);
}

void *spadfs_read_sector(SPADFS *fs, sector_t secno, struct buffer_head **bhp, unsigned ahead, const char *msg);
void *spadfs_get_new_sector(SPADFS *fs, sector_t secno, struct buffer_head **bhp, const char *msg);
#if 0
void spadfs_prefetch_sector(SPADFS *fs, sector_t secno, unsigned ahead, const char *msg);
#endif
void spadfs_discard_buffers(SPADFS *fs, sector_t start, sector_t n_sectors);

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,9)
static inline int spadfs_sync_dirty_buffer(struct buffer_head *bh)
{
	sync_dirty_buffer(bh);
	return 0;
}
#else
#define spadfs_sync_dirty_buffer	sync_dirty_buffer
#endif

#ifndef CHECK_BUFFER_LEAKS
#define spadfs_drop_reference(fs, buf)	do { } while (0)
#ifndef CHECK_BUFFER_WRITES
#define spadfs_brelse(fs, buf)		__brelse(buf)
#else
#define spadfs_brelse(fs, buf)		do { spadfs_sync_dirty_buffer(buf); __brelse(buf); } while (0)
#endif
#define spadfs_buffer_leaks_init(fs)	do { } while (0)
#define spadfs_buffer_leaks_done(fs)	do { } while (0)
#else
void spadfs_drop_reference(SPADFS *fs, struct buffer_head *bh);
void spadfs_brelse(SPADFS *fs, struct buffer_head *bh);
void spadfs_buffer_leaks_init(SPADFS *fs);
void spadfs_buffer_leaks_done(SPADFS *fs);
#endif

/* bufstruc.c */

struct txblock *spadfs_read_tx_block(SPADFS *fs, struct buffer_head **bhp, const char *msg);
#define SRFB_FNODE		1
#define SRFB_FIXED_FNODE	2
#define SRFB_DNODE		4
struct fnode_block *spadfs_read_fnode_block(SPADFS *fs, sector_t secno, struct buffer_head **bhp, int struct_type, const char *msg);
struct anode *spadfs_read_anode(SPADFS *fs, sector_t secno, struct buffer_head **bhp, unsigned *vx, int read_lock, const char *msg);

/* dir.c */

#define HINT_META	0
#define HINT_SMALL	1
#define HINT_BIG	2

sector_t spadfs_alloc_hint(SPADFNODE *f, int hint);
void spadfs_set_new_hint(SPADFNODE *f, struct alloc *al);
void spadfs_get_dir_hint(SPADFNODE *f, u16 *small, u16 *big);
int spadfs_write_file(SPADFNODE *f, int datasync, int *optimized, struct buffer_head **bhp);
int spadfs_write_directory(SPADFNODE *f);

int spadfs_refile_fixed_fnode(SPADFNODE *file, u8 *new_ea, unsigned new_ea_size);
int spadfs_refile_fnode(SPADFNODE *dir, struct qstr *qstr, SPADFNODE *file, u8 *new_ea, unsigned new_ea_size);

/* file.c */

sector_t spadfs_size_2_sectors(SPADFS *fs, loff_t size);

void spadfs_create_memory_extents(SPADFNODE *f);
void spadfs_clear_last_block(struct inode *i);
void spadfs_truncate(struct inode *i);
void spadfs_delete_file_content(SPADFNODE *f);
#if LINUX_VERSION_CODE < KERNEL_VERSION(5,12,0)
int spadfs_file_setattr(struct dentry *dentry, struct iattr *iattr);
#else
int spadfs_file_setattr(struct mnt_idmap *ns, struct dentry *dentry, struct iattr *iattr);
#endif

extern
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,21)
const
#endif
struct inode_operations spadfs_file_iops;
extern
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,17)
const
#endif
struct file_operations spadfs_file_fops;
extern
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,18)
const
#endif
struct address_space_operations spadfs_file_aops;

#ifdef SPADFS_QUOTA
ssize_t spadfs_quota_read(struct super_block *s, int type,
			  char *data, size_t len, loff_t off);
ssize_t spadfs_quota_write(struct super_block *s, int type,
			   const char *data, size_t len, loff_t off);
#if SPADFS_QUOTA >= 2
struct dquot **spadfs_quota_get(struct inode *inode);
#endif
#endif

/* inode.c */

struct fnode_ea *spadfs_get_ea(SPADFNODE *f, u32 what, u32 mask);
void spadfs_find_ea_unx(SPADFNODE *f);
void spadfs_validate_stable_ino(u64 *result, u64 ino);
u64 spadfs_expose_inode_number(SPADFS *fs, u64 ino);
int spadfs_get_fixed_fnode_pos(SPADFS *fs, struct fnode_block *fnode_block, sector_t secno, unsigned *pos);
struct inode *spadfs_iget(struct super_block *s, spadfs_ino_t ino, sector_t parent_block, unsigned parent_pos);
struct inode *spadfs_new_inode(struct inode *dir, umode_t mode, time_t ctime, void *ea, unsigned ea_size, sector_t fnode_block, unsigned fnode_pos, sector_t dir_root, u64 stable_ino);
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,36)
void spadfs_delete_inode(struct inode *i);
#else
void spadfs_evict_inode(struct inode *i);
#endif
u64 spadfs_get_user_inode_number(struct inode *inode);
void spadfs_get_initial_attributes(struct inode *dir, umode_t *mode, uid_t *uid, gid_t *gid);
void spadfs_init_unx_attribute(struct inode *dir, struct ea_unx *ea, umode_t mode, u64 stable_ino);
#if LINUX_VERSION_CODE < KERNEL_VERSION(5,12,0)
int spadfs_setattr_common(struct dentry *dentry, struct iattr *iattr);
#else
int spadfs_setattr_common(struct mnt_idmap *ns, struct dentry *dentry, struct iattr *iattr);
#endif
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,11,0)
int spadfs_getattr(struct vfsmount *mnt, struct dentry *dentry, struct kstat *stat);
#elif LINUX_VERSION_CODE < KERNEL_VERSION(5,12,0)
int spadfs_getattr(const struct path *path, struct kstat *stat, u32 request_mask, unsigned query_flags);
#else
int spadfs_getattr(struct mnt_idmap *ns, const struct path *path, struct kstat *stat, u32 request_mask, unsigned query_flags);
#endif
void spadfs_update_ea(struct inode *inode);
void spadfs_set_parent_fnode(SPADFNODE *f, sector_t sec, unsigned pos);
void spadfs_move_parent_dir_ptr(SPADFS *fs, sector_t src_sec, unsigned src_pos, sector_t dst_sec, unsigned dst_pos);
void spadfs_move_fnode_ptr(SPADFS *fs, sector_t src_sec, unsigned src_pos, sector_t dst_sec, unsigned dst_pos, int is_dir);

/* ioctl.c */

long spadfs_ioctl(struct file *file, unsigned cmd, unsigned long arg);
#ifdef CONFIG_COMPAT
long spadfs_compat_ioctl(struct file *file, unsigned cmd, unsigned long arg);
#endif

/* link.c */

extern
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,21)
const
#endif
struct inode_operations spadfs_symlink_iops;

/* name.c */

int spadfs_compare_names(SPADFS *fs, const char *n1, unsigned l1, const char *n2, unsigned l2);
void spadfs_set_name(SPADFS *fs, char *dest, const char *src, unsigned len);

extern
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,30)
const
#endif
struct dentry_operations spadfs_dops;

static inline void spadfs_set_dentry_operations(SPADFS *fs, struct dentry *dentry)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,38)
	if (unlikely(!(fs->flags_compat_none & FLAG_COMPAT_NONE_UNIX_NAMES)))
		dentry->d_op = &spadfs_dops;
#endif
}

/* namei.c */

int spadfs_unlink_unlocked(SPADFNODE *dir, struct dentry *dentry);

extern
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,21)
const
#endif
struct inode_operations spadfs_dir_iops;
extern
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,17)
const
#endif
struct file_operations spadfs_dir_fops;

/* super.c */

extern spadfs_cache_t *spadfs_extent_cachep;

int spadfs_ea_alloc(SPADFNODE *f, unsigned ea_size);
static inline int spadfs_ea_resize(SPADFNODE *f, unsigned ea_size)
{
	if (likely(ea_size <= FNODE_EA_INLINE))
		return 0;
	return spadfs_ea_alloc(f, ea_size);
}

__cold void __attribute__ ((__format__ (__printf__, 3, 4))) spadfs_error_(SPADFS *fs, unsigned flags, const char *msg, ...);
#define spadfs_error(fs, flags, msg, ...)	spadfs_error_(fs, flags, KERN_ERR "spadfs: " msg "\n", ## __VA_ARGS__)

int spadfs_stop_cycles(SPADFS *fs, sector_t key, sector_t (*c)[2], const char *msg);
int spadfs_issue_flush(SPADFS *fs);
void spadfs_tx_block_checksum(struct txblock *tb);
int spadfs_new_stable_ino(SPADFS *fs, u64 *stable_ino);
int spadfs_commit_unlocked(SPADFS *fs);
int spadfs_commit(SPADFS *fs);

/* xattr.c */

ssize_t spadfs_listxattr(struct dentry *dentry, char *list, size_t list_size);
extern const struct xattr_handler *spadfs_xattr_handlers[];

#endif
