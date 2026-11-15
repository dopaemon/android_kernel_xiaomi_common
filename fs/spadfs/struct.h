#ifndef _SPADFS_COMMON_STRUCT_H
#define _SPADFS_COMMON_STRUCT_H

/*	*******************
	* DATA STRUCTURES *
	*******************
disk:			(align)
superblock		block
txblock			block
apage_index[0]		block
apage_index[1]		block
apages			page		(alloc apages from last to first)
cc-table		block
root_fnode		block
root_fnode_directory	block

memory:				(alloc method)
fs (superblock/txblock)		in FS struct
apage_index			grab_contig_area(DATA|PHYSCONTIG)
apage_index_info		grab_contig_area(DATA)
	(total&contig free blocks in each apage + checksumless flag)
cc-table			grab_contig_area(DATA|PHYSCONTIG)
*/

#define CHECKSUM_BASE		((__u8)'C')

#define TXC_INVALID(txc)	((__s32)((__u32)(txc) - 0x80000000U))

#define MAKE_D_OFF(l,h)		(__make64(SPAD2CPU32_LV(&l), SPAD2CPU16_LV(&h)))
#define MAKE_PART_0(l)		(CPU2SPAD32((__u32)(l)))
#define MAKE_PART_1(l)		(CPU2SPAD16((__u16)((l) >> 31 >> 1)))

#define MAKE_D_OFF3(l,h,hh)	(__make64(SPAD2CPU16_LV(&l) |		\
					 ((__u32)SPAD2CPU16_LV(&h) << 16),\
					 SPAD2CPU16_LV(&hh)))
#define MAKE_PART_30(l)		(CPU2SPAD16((__u16)(l)))
#define MAKE_PART_31(l)		(CPU2SPAD16((__u16)((__u32)(l) >> 16)))
#define MAKE_PART_32(l)		(CPU2SPAD16((__u16)((l) >> 31 >> 1)))


#define BOOTLOADER_SECTORS		2

#define USERBLOCK_SECTOR		16383

#define USERBLOCK_MAGIC			0x004C4255

#define USERFLAGS_NEED_FSCK		0x01
#define USERFLAGS_REBUILD_APAGES	0x02
#define USERFLAGS_RESET_CC		0x04
#define USERFLAGS_EXTEND		0x08
#define USERFLAGS_SET_RESERVE		0x10
#define USERFLAGS_ZERO_FILE_TAILS	0x20

struct userblock {
	__u32 magic;
	__u8 checksum;
	__u8 userflags;
	__u8 pad[2];
	__u64 reserve;
};

#define SUPERBLOCK_SECTOR		16384

#define SUPERBLOCK_SIGNATURE		"FS SPAD\000"

#define SPADFS_VERSION_MAJOR		1
#define SPADFS_VERSION_MIDDLE		1
#define SPADFS_VERSION_MINOR		18

#define CCT_SIZE			(65536*4)	/* do not change it */
#define SSIZE_BITS			9		/* --- "" --- */

struct superblock {
	__u8 signature[8];

	__u64 byte_sex;			/* 0x01234567890ABCDEF */

	__u8 version_major;	/* must be EXACLY SAME */
	__u8 version_middle;	/* don't care */
	__u8 version_minor;	/* don't care */
	__u8 checksum;

	__u8 sectors_per_block_bits;	/* 0 -- block 512 bytes */
	__u8 sectors_per_fnodepage_bits;/* 4 -- fnode 8192 bytes */
	__u8 sectors_per_page_bits;	/* 6 -- page 32768 bytes */
	__u8 pad1;

	__u8 sectors_per_cluster_bits;	/* 6 -- that means pad files to 32768 */
	__u8 sectors_per_group_bits;
	__u16 cluster_threshold;	/* 4 -- that is pad files to 131072 */
	__u16 small_file_group;
	__u16 large_file_group;

	__u64 size;
	__u64 reserve_sectors;

	__u64 txblock;
	__u64 apage_index[2];
	__u64 cct;
	__u64 root;
	__u64 pad2;
	__u32 flags_compat_fsck;
	__u32 flags_compat_rw;
	__u32 flags_compat_ro;
	__u32 flags_compat_none;
};

#define FLAG_COMPAT_FSCK_NO_CHECKSUMS	0x00000001
#define FLAG_COMPAT_RO_DIRECTORY_SIZES	0x00000001
#define FLAG_COMPAT_NONE_UNIX_NAMES	0x00000001
#define FLAG_COMPAT_NONE_DYNAMIC_MAGIC	0x00000002
#define FLAG_COMPAT_NONE_EXTRA_SPACE	0x00000004

#define MAX_SECTORS_PER_BLOCK_BITS	7
#define MAX_SECTORS_PER_PAGE_BITS	8
#define MAX_ALLOC_MASK_BITS		16
#define MIN_SIZE			(SUPERBLOCK_SECTOR + 2048)
#define MAX_SIZE			((__u64)0x00010000 << 32)
#define SIZE_MASK			(MAX_SIZE - 1)

#define TXBLOCK_MAGIC			0x4C425854

#define TXFLAGS_DIRTY			0x01
#define TXFLAGS_IO_READ_ERROR		0x02
#define TXFLAGS_IO_WRITE_ERROR		0x04
#define TXFLAGS_FS_ERROR		0x08
#define TXFLAGS_CHECKSUM_ERROR		0x10
#define TXFLAGS_EA_ERROR		0x20

struct txblock {
	__u32 magic;
	__u8 checksum;
	__u8 pad;
	__u16 cc;

	__u16 pad2;
	__u16 a_cc;	/* if CC_VALID(a_cc, a_txc), apage_index[0] is valid, */
	__s32 a_txc;	/* otherwise apage_index[1] */
	__u32 txflags;
	__u32 pad3;
	__u64 ino;
};

#define SPADFS_INO_INITIAL_REGION	0x40000000
#define SPADFS_INO_ROOT			1

/*********
 * APAGE *
 *********/

#define APAGE_INDEX_ENTRY_SIZE	(sizeof(struct apage_index_entry))

struct apage_index_entry {
	__u64 apage;
	__u64 end_sector;	/* 0 means free apage */
};

#define APAGE_HEAD_SIZE		(sizeof(struct apage_head))

#define APAGE_MAGIC		0x5041

#define APAGE_SIZE_BITS		0x07
#define  APAGE_SIZE_BITS_SHIFT		0
#define APAGE_BLOCKSIZE_BITS	0x38
#define  APAGE_BLOCKSIZE_BITS_SHIFT	3
#define APAGE_CHECKSUM_VALID	0x40
#define APAGE_BITMAP		0x80

#define APAGE_SIZE(flags)	((1 << SSIZE_BITS) <<			\
		(((flags) & APAGE_SIZE_BITS) >> APAGE_SIZE_BITS_SHIFT))
#define APAGE_SECTORS_PER_BLOCK_BITS(flags)				\
		(((flags) & APAGE_BLOCKSIZE_BITS) >> APAGE_BLOCKSIZE_BITS_SHIFT)

struct apage_subhead {
	union {
		struct {		/* for list */
			__u8 flags;
			__u8 checksum;
			__u16 freelist;
			__u16 last;
			__u16 first;
		} l;
		struct {		/* for bitmap */
			__u8 flags;
			__u8 checksum;
			__u16 start1;
			__u32 start0;
		} b;
	} u;
};

struct apage_head {	/* must: sizeof(apage_head) == sizeof(aentry) */
	__u16 magic;	/* these 3 entries are only in the first half */
	__u16 cc;	/* if CC_VALID(cc, txc), first half of page is valid, */
	__s32 txc;	/* otherwise second page */

	/* following entries are used on both halves of page */
	struct apage_subhead s;
};

struct aentry {
	__u64 start;
	__u32 len;	/* 0 -- entry is free (and is on freelist) */
	__u16 prev;	/* 0 -- head is previous (this entry is first) */
	__u16 next;	/* 0 -- head is next (this entry is last) */
};

#define APTR_ALIGN(x)		((__u16)(x) & ~(sizeof(struct aentry) - 1))
/* APTR_ALIGN must not return number < 0 */
#define APTR_INVALID(x, asize)	(unlikely((x) & (sizeof(struct aentry) - 1)) ||\
				 unlikely((x) >= (asize)))

#define BITMAP_SIZE(d)		(((d) - sizeof(struct apage_head)) * 8)

#define BITMAP_OFFSET(a, o)	(unlikely((((o) - (MAKE_D_OFF((a)->s.u.b.start0, (a)->s.u.b.start1))) & ~(__u64)0xffffffffu) != 0) ? (unsigned)-1 : ((unsigned)(o) - (unsigned)SPAD2CPU32_LV(&(a)->s.u.b.start0)) >> APAGE_SECTORS_PER_BLOCK_BITS((a)->s.u.b.flags))
#define BITMAP_LEN(a, l)	((l) >> APAGE_SECTORS_PER_BLOCK_BITS((a)->s.u.b.flags))

#define BITMAP_CLEAR(a, o)						\
do {									\
	__u32 *bp_ = ((__u32 *)((__u8 *)(a) + sizeof(struct apage_head)) + \
			       ((o) >> 5));				\
	CPU2SPAD32_LV(bp_, SPAD2CPU32_LV(bp_) & ~(1 << ((o) & 31)));	\
} while (0)

#define BITMAP_SET(a, o)						\
do {									\
	__u32 *bp_ = ((__u32 *)((__u8 *)(a) + sizeof(struct apage_head)) + \
			       ((o) >> 5));				\
	CPU2SPAD32_LV(bp_, SPAD2CPU32_LV(bp_) | (1 << ((o) & 31)));	\
} while (0)

	/* must return 0 or 1 */
#define BITMAP_TEST(a, o)	((SPAD2CPU32_LV((__u32 *)((__u8 *)(a) +	\
						sizeof(struct apage_head)) + \
						((o) >> 5)) >> ((o) & 31)) & 1)

#define BITMAP_TEST_32_FULL(a, o)	(*((__u32 *)((__u8 *)(a) + \
					sizeof(struct apage_head)) + \
					((o) >> 5)) == \
					CPU2SPAD32_CONST(0xffffffffu))

/*
 * if apage contains <= CONV_APAGE_SECTORS sectors, convert it to bitmap
 * instead of splitting it
 */
#define CONV_APAGE_SECTORS(page_bits, block_bits)		\
	((((1 << (page_bits)) / 2 - APAGE_HEAD_SIZE) * 8) <<	\
	 ((block_bits) - SSIZE_BITS))

/* minimum sectors in an apage */
#define MIN_APAGE_SECTORS(page_bits, block_bits)		\
		(CONV_APAGE_SECTORS(page_bits, block_bits) / 2)

/* leave 1/8 of apage free when rebuilding to avoid immediate split */
#define FSCK_APAGE_FREE_PERCENTAGE	8

/* number of apages for filesystem of given size */
#if !(defined(__linux__) && defined(__KERNEL__))
#define N_APAGES(size, page_bits, block_bits)				\
	(((size) + MIN_APAGE_SECTORS(page_bits, block_bits) - 1 - 1) /	\
	MIN_APAGE_SECTORS(page_bits, block_bits) + 1)
#else
static __finline__ __u64 N_APAGES(__u64 size, int page_bits, int block_bits)
{
	int a = 1;
	int mas = MIN_APAGE_SECTORS(page_bits, block_bits);
	__u64 as = mas;
	while (as + 1 < size) {
		a++;
		as += mas;
	}
	return a + 1;
}
#endif

#define MAX_APAGES	0x7ffffffe

/* number of sectors in each apage index */
#define APAGE_INDEX_SECTORS(n_apages, blocksize)			\
	((((__u64)(n_apages) * APAGE_INDEX_ENTRY_SIZE + (blocksize) - 1) &\
	~(__u64)((blocksize) - 1)) >> SSIZE_BITS)

/*********
 * FNODE *
 *********/

/*
 * 0x1F8 would be sufficient, but test more bits so that
 * fsck can resync better on corrupted directories
 */
#define FNODE_NEXT_SIZE		0x7FF8
#define FNODE_NEXT_FREE		0x0001
/*
 * fnode is free if: (next & FNODE_NEXT_FREE) && CC_VALID(cc, txc)
 * fnode can be allocated if: is_free && !CC_CURRENT(cc, txc)
 *			alloc: cc,txc = CURRENT ^ 0x80000000
 *			       FNODE_NEXT_FREE = 1
 *			       write at pos 1.
 * fnode can be updated if !is_free
 *		update: if (!CC_CURRENT) CC_SET_CURRENT, FNODE_NEXT_FREE = 0;
 *			write at pos !CC_VALID(cc, txc)
 * fnode can be deleted if !is_free
 *		delete: if (CC_CURRENT && FNODE_NEXT_FREE) cc, txc = 0;
 *			else {
 *				if (CC_VALID) copy data from pos 0 -> to pos 1;
 *				cc, txc = CURRENT;
 *				FNODE_NEXT_FREE = 1;
 *			}
 * free fnodes can be joined if both can be allocated.
 */

#define FNODE_FLAGS_DIR		0x01
#define FNODE_FLAGS_HARDLINK	0x02

#define FNODE_HEAD_SIZE		8

struct fnode {
	__u16 next;	/* size of fnode */
	__u16 cc;
	__s32 txc;
	__u64 size[2];	/* 0 if CC_VALID(cc, txc), 1 if !CC_VALID(cc, txc) */
	__u32 ctime;
	__u32 mtime;

#define MAX_DIRECT_BLKS(blksize)	(0x10000U - (blksize))

	__u32 run10;
	__u16 run11;
	__u16 run1n;
	__u32 run20;
	__u16 run21;
	__u16 run2n;
	__u32 anode0;
	__u16 anode1;
	/*
	 * for directories:
	 * if CC_VALID(cc, txc), run10/run11 is the root page;
	 *	       otherwise run20/run21 is the root page
	 * for hardlinks: anode0/anode1 is pointer to the fixed_fnode_block
	 */
	__u8 flags;

	__u8 namelen;
};

/*
 * this might be smaller than actual filesystem blocksize -- it is beacuse
 * block device guarantees atomic write of 512 bytes but not more.
 */
#define FNODE_BLOCK_SIZE		512
/*
 * sizeof(struct fnode_block)
 */
#define SIZEOF_FNODE_BLOCK		(6 * 4)


#define FNODE_MAX_SIZE		(FNODE_BLOCK_SIZE - SIZEOF_FNODE_BLOCK)/* 488 */
#define FNODE_EMPTY_MIN_SIZE	8

#define FNODE_NAME_POS		(sizeof(struct fnode))
#define FNODE_NAME(fnode)	((char *)(fnode) + FNODE_NAME_POS)

#define MAX_NAME_LEN		255

#define INVALID_FILENAME_CHARACTER(pos, c, unix_names)			\
	(unlikely(!(c)) ||						\
	unlikely(((c) == '/')) ||					\
	(!(unix_names) &&						\
		((unlikely((c) >= 'a') && unlikely((c) <= 'z')) ||	\
		unlikely((c) == ':') ||					\
		unlikely((unsigned char)(c) < ' ') ||			\
		(unlikely((c) == '^') && !(pos)))))

#define FNODE_EA_POS(name_len)	(((FNODE_NAME_POS + (name_len)) + 7) & ~7)
#define FNODE_MAX_EA_SIZE	(FNODE_MAX_SIZE - sizeof(struct fnode) - 256)
		/* 176 */

#define FNODE_SIZE(name_len, ea_size)	\
			((FNODE_EA_POS(name_len) + (ea_size) + 7) & ~7)

struct fnode_ea {
	__u32 magic;
};

#define FNODE_EA_MAGIC_MASK	0x00FFFFFF
#define FNODE_EA_SIZE_SHIFT	24
#define FNODE_EA_SIZE_MASK_1	0xFF
#define FNODE_EA_SIZE_ADD	(7 + 4)
#define FNODE_EA_SIZE_MASK_2	0xFFFFFFF8
#define FNODE_EA_ALIGN		8
#define FNODE_EA_DO_ALIGN(n)	(((n) + FNODE_EA_ALIGN - 1) & \
							~(FNODE_EA_ALIGN - 1))

#define GET_EA_ERROR		((void *)1)

static __finline__ struct fnode_ea *GET_EA(struct fnode_ea *ea, unsigned ea_size,
				      __u32 what, __u32 mask)
{
	while (unlikely(ea_size)) {
		unsigned rec_size =
			(((SPAD2CPU32_LV(&ea->magic) >> FNODE_EA_SIZE_SHIFT) &
			 FNODE_EA_SIZE_MASK_1) + FNODE_EA_SIZE_ADD) &
			FNODE_EA_SIZE_MASK_2;
		if (unlikely(rec_size > ea_size))
			return GET_EA_ERROR;
		if ((SPAD2CPU32_LV(&ea->magic) & mask) == (what & mask))
			return ea;
		ea = (struct fnode_ea *)((char *)ea + rec_size);
		ea_size -= rec_size;
	}
	return NULL;
}

static __finline__ int RESIZE_EA(__u8 *ea_pool, unsigned *ea_pool_size,
			    struct fnode_ea *ea, unsigned new_size)
{
	unsigned old_size = (SPAD2CPU32_LV(&ea->magic) >> FNODE_EA_SIZE_SHIFT) &
				FNODE_EA_SIZE_MASK_1;
	unsigned asize_1 =
		(old_size + FNODE_EA_SIZE_ADD) & FNODE_EA_SIZE_MASK_2;
	unsigned asize_2 =
		(new_size + FNODE_EA_SIZE_ADD) & FNODE_EA_SIZE_MASK_2;
	if (unlikely(*ea_pool_size - asize_1 + asize_2 > FNODE_MAX_EA_SIZE))
		return -ENOSPC;
	memmove((__u8 *)ea + asize_2, (__u8 *)ea + asize_1,
		(ea_pool + *ea_pool_size) - ((__u8 *)ea + asize_1));
	memset((__u8 *)ea + sizeof(struct fnode_ea) + new_size, 0,
			asize_2 - (new_size + sizeof(struct fnode_ea)));
	*ea_pool_size = *ea_pool_size - asize_1 + asize_2;
	CPU2SPAD32_LV(&ea->magic, (SPAD2CPU32_LV(&ea->magic) &
			~(FNODE_EA_SIZE_MASK_1 << FNODE_EA_SIZE_SHIFT)) |
			(new_size << FNODE_EA_SIZE_SHIFT));
	return 0;
}

static __finline__ void REMOVE_EA(__u8 *ea_pool, unsigned *ea_pool_size,
			     struct fnode_ea *ea)
{
	unsigned old_size = (SPAD2CPU32_LV(&ea->magic) >> FNODE_EA_SIZE_SHIFT) &
				FNODE_EA_SIZE_MASK_1;
	unsigned asize_1 =
		(old_size + FNODE_EA_SIZE_ADD) & FNODE_EA_SIZE_MASK_2;
	memmove((__u8 *)ea, (__u8 *)ea + asize_1,
		(ea_pool + *ea_pool_size) - ((__u8 *)ea + asize_1));
	*ea_pool_size = *ea_pool_size - asize_1;
}

#define EA_UNX_MAGIC_MASK	((__u32)~(0x08 << 24))
#define EA_UNX_MAGIC		((0x00584E55 |				\
		((sizeof(struct ea_unx) - sizeof(struct fnode_ea)) << 24)))
#define EA_UNX_MAGIC_OLD	((0x00584E55 |				\
		((sizeof(struct ea_unx) - sizeof(struct fnode_ea)) << 24)) & \
		EA_UNX_MAGIC_MASK)
#define EA_SYMLINK_MAGIC_MASK	FNODE_EA_MAGIC_MASK
#define EA_SYMLINK_MAGIC	0x004D5953
#define EA_RDEV_MAGIC_MASK	((__u32)~0)
#define EA_RDEV_MAGIC		(0x00524E55 |				\
		((sizeof(struct ea_rdev) - sizeof(struct fnode_ea)) <<	\
		FNODE_EA_SIZE_SHIFT))
#define EA_XATTR_MAGIC_MASK	FNODE_EA_MAGIC_MASK
#define EA_XATTR_MAGIC		0x00544158

struct ea_unx {
	__u32 magic;
	__u16 mode;
	__u16 flags;		/* not used now */
	__u32 uid;
	__u32 gid;
	__u32 prealloc[2];
	__u64 ino;
};

struct ea_rdev {
	__u32 magic;
	__u32 pad;
	__u64 dev;
};

/*
struct ea_xattr {
	__u32 magic;
	__u8 type;
	__u8 namelen;
	__u8 valuelen;
	__u8 name[];
	__u8 value[];
};
*/

/* mode in ea_unx is compatible with Linux flags */
#define LINUX_S_IFMT		0170000
#define LINUX_S_IFSOCK		0140000
/* LINUX_S_IFLNK must not be set, symlinks are recognised by "SYM" attribute */
#define LINUX_S_IFLNK		0120000
#define LINUX_S_IFREG		0100000
#define LINUX_S_IFBLK		0060000
#define LINUX_S_IFDIR		0040000
#define LINUX_S_IFCHR		0020000
#define LINUX_S_IFIFO		0010000

#define LINUX_S_ISUID		0004000
#define LINUX_S_ISGID		0002000
#define LINUX_S_ISVTX		0001000

#define LINUX_S_IRWXU		00700
#define LINUX_S_IRUSR		00400
#define LINUX_S_IWUSR		00200
#define LINUX_S_IXUSR		00100

#define LINUX_S_IRWXG		00070
#define LINUX_S_IRGRP		00040
#define LINUX_S_IWGRP		00020
#define LINUX_S_IXGRP		00010

#define LINUX_S_IRWXO		00007
#define LINUX_S_IROTH		00004
#define LINUX_S_IWOTH		00002
#define LINUX_S_IXOTH		00001

#define LINUX_S_ISLNK(m)	(((m) & LINUX_S_IFMT) == LINUX_S_IFLNK)
#define LINUX_S_ISREG(m)	(((m) & LINUX_S_IFMT) == LINUX_S_IFREG)
#define LINUX_S_ISDIR(m)	(((m) & LINUX_S_IFMT) == LINUX_S_IFDIR)
#define LINUX_S_ISCHR(m)	(((m) & LINUX_S_IFMT) == LINUX_S_IFCHR)
#define LINUX_S_ISBLK(m)	(((m) & LINUX_S_IFMT) == LINUX_S_IFBLK)
#define LINUX_S_ISFIFO(m)	(((m) & LINUX_S_IFMT) == LINUX_S_IFIFO)
#define LINUX_S_ISSOCK(m)	(((m) & LINUX_S_IFMT) == LINUX_S_IFSOCK)


#define SPADFS_XATTR_END	0x00
#define SPADFS_XATTR_USER	0x55
#define SPADFS_XATTR_TRUSTED	0x54
#define SPADFS_XATTR_SECURITY	0x53
#define SPADFS_XATTR_ACL_ACCESS	0x41
#define SPADFS_XATTR_ACL_DEFAULT 0x44

#define GET_XAT_ERROR		((void *)1)

#define GET_XAT_TYPE_NAME	0
#define GET_XAT_TYPE		1
#define GET_XAT_ALL		2

static __finline__ __u8 *GET_XAT(__u8 *xat, unsigned xat_size, int mode, int type,
			    const char *name, unsigned namelen)
{
	while (xat_size) {
		unsigned this_size;

		if (unlikely(xat_size < 3))
			return GET_XAT_ERROR;

		if (unlikely(!xat[2]))
			return GET_XAT_ERROR;

		this_size = 3 + xat[1] + xat[2];

		if (unlikely(this_size > xat_size))
			return GET_XAT_ERROR;

		if (unlikely(mode == GET_XAT_ALL))
			return xat;
		if (type == xat[0]) {
			if (mode == GET_XAT_TYPE ||
			    (xat[1] == namelen &&
			    !memcmp(xat + 3, name, namelen)))
				return xat;
		}

		xat += this_size;
		xat_size -= this_size;
	}
	return NULL;
}


#define FILE_SECTORS(bl_size, cl_size, cl_thresh, size, result)		\
do {									\
	unsigned bsize_ = (bl_size);					\
	if ((size) >= (cl_thresh)) bsize_ = (cl_size);			\
	result = (((size) + bsize_ - 1) & ~(__u64)(bsize_ - 1)) >> SSIZE_BITS;\
} while (0)

#define FNODE_BLOCK_MAGIC		0x444F4E46
#define FNODE_BLOCK_CHECKSUM_VALID	0x01
#define FNODE_BLOCK_LAST		0x02
#define FNODE_BLOCK_FIRST		0x04

struct fnode_block {
	__u32 magic;
	__u8 flags;
	__u8 checksum;
	__u16 prev1;	/* 0 if first */
	__u32 prev0;	/* --- "" --- */
	__s32 txc;
	__u16 cc;
	__u16 next1;	/* valid if CC_VALID(cc, txc) */
	__u32 next0;	/* ---------- "" ------------ */
	struct fnode fnodes[1];
};

#define FIXED_FNODE_BLOCK_SIZE		512
#define FIXED_FNODE_BLOCK_MAGIC		0x4E465846
#define FIXED_FNODE_BLOCK_CHECKSUM_VALID FNODE_BLOCK_CHECKSUM_VALID

struct fixed_fnode_block {
	__u32 magic;
	/*
	 * flags & checksum must match struct fnode_block
	 * --- the same checksum routine is used.
	 */
	__u8 flags;
	__u8 checksum;
	__u16 cc;
	__s32 txc;
	__u16 hint_small;
	__u16 hint_large;
		/* &fnode0-&nlink0 must be equal to &fnode1-&nlink1 */
	__u64 nlink0;			/* if CC_VALID(cc, txc) */
	__u8 fnode0[FNODE_MAX_SIZE - 256];	/* 232 */
	__u64 reserved0;
	__u64 nlink1;			/* otherwise */
	__u8 fnode1[FNODE_MAX_SIZE - 256];	/* 232 */
	__u64 reserved1;
};

#define FIXED_FNODE_BLOCK_FNODE0	\
		((int)(long)&(((struct fixed_fnode_block *)NULL)->fnode0))
#define FIXED_FNODE_BLOCK_NLINK0	\
		((int)(long)&(((struct fixed_fnode_block *)NULL)->nlink0))
#define FIXED_FNODE_BLOCK_FNODE1	\
		((int)(long)&(((struct fixed_fnode_block *)NULL)->fnode1))
#define FIXED_FNODE_BLOCK_NLINK1	\
		((int)(long)&(((struct fixed_fnode_block *)NULL)->nlink1))

#define FIXED_FNODE_NLINK_PTR(fnode)	\
	((__u64 *)((char *)(fnode) -	\
	(FIXED_FNODE_BLOCK_FNODE0 - FIXED_FNODE_BLOCK_NLINK0)))

struct dnode_page_entry {
	__u16 b0;
	__u16 b1;
	__u16 b2;
}
#if defined(__KERNEL__) || defined(__GNUC__)
__attribute__((packed))
#endif
;

#define DNODE_PAGE_ENTRY_SIZE		sizeof(struct dnode_page_entry)
#define DNODE_PAGE_ENTRY_BITS		3

#define DNODE_PAGE_MAGIC		0x444F4E44

#define DNODE_CHECKSUM_VALID		0x01

		/* used for fsck */
#define DNODE_GFLAGS_PAGE_SIZE_BITS_MINUS_1		0x07
#define DNODE_GFLAGS_PAGE_SIZE_BITS_MINUS_1_SHIFT	0

struct dnode_page {
	__u32 magic;
	__u8 flags[2];
	__u8 gflags;
	__u8 pad;
	__u64 up_dnode;		/* 0 if top-level */
	__s32 txc;
	__u16 cc;
	__u8 checksum[2];
};

#define DNODE_ENTRY_OFFSET		sizeof(struct dnode_page)

/*
 * if (CC_VALID(cc, txc)) the first dnode version is valid
 * else the second version is valid
 */

typedef __u32 hash_t;
#define SPADFS_HASH_BITS	32

static __finline__ hash_t name_hash(const char *name)
{
	hash_t hash = 0;
	while (*name)
		hash = hash * 15 + (*name++ & 0xdf);
	return hash;
}

static __finline__ hash_t name_len_hash(const char *name, int len)
{
	hash_t hash = 0;
	while (len--)
		hash = hash * 15 + (*name++ & 0xdf);
	return hash;
}

/*********
 * ANODE *
 *********/

struct extent {
	__u64 blk;
	__u64 end_off;
};

#define ANODE_MAGIC			0x444F4E41

#define ANODE_CHECKSUM_VALID		0x01
#define ANODE_ROOT			0x02

#define ANODE_ROOT_NAME(anode)		((char *)	\
					    &(anode)->x[ANODE_N_EXTENTS - 1])
#define ANODE_ROOT_NAME_LEN		(sizeof(struct extent))

#define ANODE_SIZE			512
#define ANODE_N_EXTENTS			31

struct anode {
	__u32 magic;
	__u8 flags;
	__u8 checksum;
	__u8 valid_extents;
	__u8 pad;
	__u64 start_off;
	struct extent x[ANODE_N_EXTENTS];
};

static __finline__ int find_direct(int depth_now, int depth_total)
{
	if (likely(!depth_now))
		return 20;	/* 20 -- direct; 10 -- indirect; 1 -- name */
	if (depth_now == depth_total)
		return 31;
	return 1;
}

static __finline__ void update_depth(int *depth_now, int *depth_total, int off)
{
	if (likely(!*depth_now))
		*depth_total = off - 20 + 1;
	(*depth_now)++;
}

static __finline__ unsigned find_in_anode(struct anode *ano, __u64 lbn, unsigned h)
{
	unsigned l = 0;
	h--;
	if (unlikely(h >= ANODE_N_EXTENTS))
		h = 0;
	while (l < h) {
		unsigned d = (l + h) >> 1;
		if (lbn >= SPAD2CPU64_LV(&ano->x[d].end_off))
			l = d + 1;
		else
			h = d;
	}
	return l;
}

#define FILE_END_1_MAGIC		(((__u64)0x4946 << 32) | 0x315F454C)
#define FILE_END_2_MAGIC		(((__u64)0x4946 << 32) | 0x325F454C)

struct file_end {
	__u32 size;
	__u8 sectors_per_block_bits;
	__u8 sectors_per_cluster_bits;
	__u16 magic1;
	__u32 magic0;
};

struct file_end_2 {
	__u32 run10;
	__u16 run11;
	__u16 run1n;
	__u32 size;
	__u8 sectors_per_block_bits;
	__u8 sectors_per_cluster_bits;
	__u16 magic1;
	__u32 magic0;
};

static __finline__ int validate_range(__u64 fssize, unsigned blockmask, __u64 start, __u64 len)
{
	__u64 end;
	if (unlikely(((unsigned)start | (unsigned)len) & blockmask))
		return 0;
	end = start + len;
	if (unlikely(end <= start))
		return 0;
	if (unlikely(end > fssize))
		return 0;
	if (unlikely(start <= SUPERBLOCK_SECTOR)) {
		if (unlikely(end > USERBLOCK_SECTOR))
			return 0;
		if (unlikely(start < BOOTLOADER_SECTORS))
			return 0;
	}
	return 1;
}

static __finline__ const char *validate_super(struct superblock *super)
{
	unsigned blockmask;
	__u64 groups;
	__u64 n_apages;
	__u64 apage_index_sectors;
	if (unlikely(super->sectors_per_block_bits >
		     MAX_SECTORS_PER_BLOCK_BITS))
		return "invalid sectors_per_block_bits";
	if (unlikely(super->sectors_per_cluster_bits >= 31 - SSIZE_BITS))
		return "invalid sectors_per_cluster_bits";
	if (unlikely(super->sectors_per_cluster_bits > MAX_ALLOC_MASK_BITS))
		return "too large sectors_per_cluster_bits";
	if (unlikely(super->sectors_per_page_bits < 1))
		return "zero sectors_per_page_bits";
	if (unlikely(super->sectors_per_page_bits > MAX_SECTORS_PER_PAGE_BITS))
		return "invalid sectors_per_page_bits";
	if (unlikely(super->sectors_per_block_bits >
		     super->sectors_per_cluster_bits))
		return "sectors_per_block_bits larger than sectors_per_cluster_bits";
	if (unlikely(super->sectors_per_block_bits >
		     super->sectors_per_page_bits))
		return "sectors_per_block_bits larger than sectors_per_page_bits";
	if (unlikely(super->sectors_per_fnodepage_bits >
		     super->sectors_per_page_bits))
		return "sectors_per_fnodepage_bits larger than sectors_per_page_bits";
	if (unlikely(super->sectors_per_fnodepage_bits <
		     super->sectors_per_block_bits))
		return "sectors_per_block_bits larger than sectors_per_fnodepage_bits";
	if (unlikely((__u64)SPAD2CPU16_LV(&super->cluster_threshold) <<
		     super->sectors_per_cluster_bits >= 1 << (31 - SSIZE_BITS)))
		return "too large cluster threshold";
	if (unlikely(super->sectors_per_group_bits > 48))
		return "invalid sectors_per_group_bits";
	if (unlikely(super->sectors_per_group_bits <
		     super->sectors_per_cluster_bits))
		return "sectors_per_cluster_bits larger than sectors_per_group_bits";
	blockmask = (1 << super->sectors_per_block_bits) - 1;
	if (unlikely(SPAD2CPU64_LV(&super->size) < MIN_SIZE))
		return "size too small";
	if (unlikely(SPAD2CPU64_LV(&super->size) > MAX_SIZE))
		return "size too large";
	if (unlikely((unsigned)SPAD2CPU64_LV(&super->size) & blockmask))
		return "size unaligned";
	groups = (SPAD2CPU64_LV(&super->size) +
		((__u64)1 << super->sectors_per_group_bits) - 1) >>
		super->sectors_per_group_bits;
	if (unlikely(groups > 0xffff))
		return "too many groups (group size too small)";
	if (unlikely(SPAD2CPU16_LV(&super->small_file_group) > groups))
		return "invalid small file group";
	if (unlikely(SPAD2CPU16_LV(&super->large_file_group) <
		     SPAD2CPU16_LV(&super->small_file_group)) ||
	    unlikely(SPAD2CPU16_LV(&super->large_file_group) > groups))
		return "invalid large file group";
	if (unlikely(!validate_range(SPAD2CPU64_LV(&super->size), blockmask, SPAD2CPU64_LV(&super->txblock), blockmask + 1)))
		return "txblock invalid";
	n_apages = N_APAGES(SPAD2CPU64_LV(&super->size),
				super->sectors_per_page_bits + SSIZE_BITS,
				super->sectors_per_block_bits + SSIZE_BITS);
	if (unlikely(n_apages >= MAX_APAGES))
		return "too many apages";
	apage_index_sectors = APAGE_INDEX_SECTORS(n_apages,
			1 << SSIZE_BITS << super->sectors_per_block_bits);
	if (unlikely(!validate_range(SPAD2CPU64_LV(&super->size), blockmask, SPAD2CPU64_LV(&super->apage_index[0]), apage_index_sectors)))
		return "apage_index[0] invalid";
	if (unlikely(!validate_range(SPAD2CPU64_LV(&super->size), blockmask, SPAD2CPU64_LV(&super->apage_index[1]), apage_index_sectors)))
		return "apage_index[1] invalid";
	if (unlikely(!validate_range(SPAD2CPU64_LV(&super->size), blockmask, SPAD2CPU64_LV(&super->cct), CCT_SIZE >> SSIZE_BITS)))
		return "cct invalid";
	if (unlikely(!validate_range(SPAD2CPU64_LV(&super->size), blockmask, SPAD2CPU64_LV(&super->root), blockmask + 1)))
		return "root invalid";
	return (char *)0;
}

#define RESERVE_PERCENT_SMALL	1/50
#define RESERVE_PERCENT_BIG	1/200
#define RESERVE_LIMIT		((__u64)67108864/512)

static __finline__ __u64 get_default_reserved(__u64 size)
{
	__u64 reserve = size * RESERVE_PERCENT_SMALL;
	if (reserve > RESERVE_LIMIT) {
		reserve = RESERVE_LIMIT;
		if (reserve < size * RESERVE_PERCENT_BIG)
			reserve = size * RESERVE_PERCENT_BIG;
	}
	return reserve;
}

#define OPTIMAL_GROUPS		512
#define MINIMAL_GROUP_SECTORS	131072
#define MINIMAL_GROUPS		32

static __finline__ unsigned get_default_group_bits(__u64 size, unsigned cluster_bits)
{
	unsigned group_bits;

	for (group_bits = 1;
		(size + ((__u64)1 << (group_bits - 1))) >> group_bits >
		 OPTIMAL_GROUPS ||
		(__u64)1 << group_bits < MINIMAL_GROUP_SECTORS;
	     group_bits++) ;

	while (group_bits && (size + ((__u64)1 << group_bits) - 1) >> group_bits
	      < MINIMAL_GROUPS)
		group_bits--;
	if (group_bits < cluster_bits)
		group_bits = cluster_bits;
	return group_bits;
}

#define METADATA_PART		36
#define SMALLFILE_PART		8

static __finline__ unsigned get_default_metadata_groups(unsigned group_bits,
						   unsigned groups)
{
	unsigned metadata_groups;
	unsigned min_groups;
	metadata_groups = groups / METADATA_PART;
	min_groups = (SUPERBLOCK_SECTOR + (CCT_SIZE >> SSIZE_BITS)) >>
			group_bits;
	if (metadata_groups <= min_groups)
		metadata_groups = min_groups + 1;
	if (metadata_groups >= groups)
		metadata_groups = 0;
	return metadata_groups;
}

static __finline__ unsigned get_default_smallfile_groups(unsigned group_bits,
						    unsigned groups,
						    unsigned metadata_groups)
{
	unsigned smallfile_groups;
	groups -= metadata_groups;
	smallfile_groups = groups / SMALLFILE_PART;
	if (!smallfile_groups && groups >= 2)
		smallfile_groups = 1;
	return smallfile_groups;
}

#ifndef __SPAD__
static __finline__ unsigned char __byte_sum(void *__ptr, int __len)
{
	unsigned long __sum = 0;
	void *__e = (char *)__ptr + __len;
	barrier();
	do {
		__sum ^= *(unsigned long *)__ptr;
		__ptr = (char *)__ptr + sizeof(unsigned long);
	} while (__ptr < __e);
	__sum ^= __sum >> 31 >> 1;
	__sum ^= __sum >> 16;
	__sum ^= __sum >> 8;
	barrier();
	return __sum;
}
#endif

static __finline__ int spadfs_struct_check_correctness(void)
{
	return sizeof(struct dnode_page_entry) == 6;
}

#endif
