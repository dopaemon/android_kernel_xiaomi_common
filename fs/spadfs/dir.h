void really_do_fnode_block_checksum(struct fnode_block *fnode_block);

/*
 * do_fnode_block_checksum is called after update to fnode block
 * --- must be within
 * start_concurrent_atomic_buffer_modify/end_concurrent_atomic_buffer_modify so
 * that partially modified block is not written to disk.
 */

static inline void do_fnode_block_checksum(SPADFS *fs,
					   struct fnode_block *fnode_block)
{
	if (unlikely(make_checksums(fs)))
		really_do_fnode_block_checksum(fnode_block);
	else
		fnode_block->flags &= ~FNODE_BLOCK_CHECKSUM_VALID;
}

/*
 * Test fnode validity and jump one of these labels:
 *	ok (fnode valid)
 *	skip_free (free fnode)
 *	bad_fnode (corrupted filesystem)
 * It should test for all cases of filesystem corruption and make sure that we
 * won't reference out-of-bound memory later, no matter what data are on disk.
 *
 * fs --- pointer to filesystem
 * fnode_block --- pointer to 512-byte fnode block
 * fnode --- pointer to fnode within this block
 * size --- fnode->next & FNODE_NEXT_SIZE is put here
 * name_len --- the name length is put here
 * --- the purpose of size & name_len arguments is just to not read the same
 * entry more times
 */

/*
 * volatile is used to make sure that the appropriate entry is read only once.
 * Without volatile, the compiler may read the entry multiple times and it could
 * cause out-of-memory accesses if the structure is simultaneously being
 * modified (simultaneous modification is a symptom of filesystem corruption
 * anyway, but it should not cause out-of-memory accesses).
 */

#define VALIDATE_FNODE(fs, fnode_block, fnode, size, name_len,		\
		       ok, skip_free, bad_fnode)			\
do {									\
	(size) = *(volatile u16 *)&(fnode)->next;			\
	(size) = SPAD2CPU16(size) & FNODE_NEXT_SIZE;			\
	if (unlikely((((unsigned long)(fnode) + (size) - 1) &		\
		     ~(unsigned long)(FNODE_BLOCK_SIZE - 1)) !=		\
		     ((unsigned long)(fnode_block) &			\
		     ~(unsigned long)(FNODE_BLOCK_SIZE - 1))))		\
		goto bad_fnode;						\
									\
	if ((SPAD2CPU16_LV(&(fnode)->next) & FNODE_NEXT_FREE) &&	\
	    CC_VALID((fs), &(fnode)->cc, &(fnode)->txc)) {		\
		if (unlikely((size) < FNODE_HEAD_SIZE))			\
			goto bad_fnode;					\
		goto skip_free;						\
	}								\
									\
	if (unlikely((size) <= FNODE_NAME_POS))				\
		goto bad_fnode;						\
									\
	(name_len) = *(volatile u8 *)&(fnode)->namelen;			\
	if (unlikely((unsigned)((size) - FNODE_EA_POS(name_len)) >	\
	    FNODE_MAX_EA_SIZE))						\
		goto bad_fnode;						\
									\
	/* prevent speculative fetch of other entries by the compiler */\
	barrier();							\
	goto ok;							\
} while (0)

/*
 * Create a pointer to fixed fnode instead of regular file --- for this pointer,
 * most fields are unused. We zero them anyway.
 */

static inline void make_fixed_fnode_reference(struct fnode *fnode, sector_t blk)
{
	fnode->size[0] = fnode->size[1] = CPU2SPAD64_CONST(0);
	fnode->ctime = fnode->mtime = CPU2SPAD32_CONST(0);
	fnode->run10 = MAKE_PART_0(0);
	fnode->run11 = MAKE_PART_1(0);
	fnode->run1n = CPU2SPAD16_CONST(0);
	fnode->run20 = MAKE_PART_0(0);
	fnode->run21 = MAKE_PART_1(0);
	fnode->run2n = CPU2SPAD16_CONST(0);
	fnode->anode0 = MAKE_PART_0(blk);
	fnode->anode1 = MAKE_PART_1(blk);
	fnode->flags = FNODE_FLAGS_HARDLINK;
}

/*
 * Lock filesystem for read or write when modifying directories (many functions
 * in namei.c). The usage is this:
 *	Lock for read, try to modify directory.
 *	When split is needed, NEED_WLOCK is returned --- so we unlock for read,
 *		lock for write and retry.
 *	When sync is needed (i.e. we are out of data but something can be freed
 *		by sync, NEED_SYNC is returned. --- so unlock, sync, retry.
 *		But do not sync again on NEED_SYNC to prevent livelock ---
 *		return -ENOSPC if NEED_SYNC is returned the second time.
 *
 * The rationale for locking is that when splitting directory pages, all
 * filesystem activity must stop because file updates update entries in
 * directories.
 *
 * Maybe
 * start_concurrent_atomic_buffer_modify/end_concurrent_atomic_buffer_modify
 * could be used --- i.e. updater reads fnode position, breads the buffer
 * (without checking magic), calls start_concurrent_atomic_buffer_modify,
 * rechecks the position again and if it is the same, he is now protected from
 * directory operations. If anyone runs into contention on this lock, I can
 * investigate this further.
 */

#define ND_LOCK(fs, wlock)					\
do {								\
	if (likely(!(wlock)))					\
		down_read_sync_lock(fs);			\
	else							\
		down_write_sync_lock(fs);			\
} while (0)

#define ND_UNLOCK(fs, wlock)					\
do {								\
	if (likely(!(wlock)))					\
		up_read_sync_lock(fs);				\
	else							\
		up_write_sync_lock(fs);				\
} while (0)

int spadfs_alloc_fixed_fnode_block(SPADFNODE *fn, sector_t hint, unsigned size,
				   u16 hint_small, u16 hint_big,
				   sector_t *result);
int spadfs_alloc_leaf_page(SPADFNODE *fn, sector_t hint, unsigned sectors,
			   sector_t prev, sector_t *result, int noaccount);
struct fnode_block *spadfs_find_hash_block(SPADFNODE *f, hash_t hash,
					   struct buffer_head **bhp,
					   sector_t *secno, hash_t *next_hash);
int spadfs_lookup_ino(SPADFNODE *f, struct qstr *qstr, spadfs_ino_t *ino,
		      int for_delete);
int spadfs_check_directory_empty(SPADFNODE *f);
int spadfs_remove_directory(SPADFNODE *fn);
/*
 * These two are returned by spadfs_add_fnode_to_directory (in addition of
 * -ERROR or pointer on success). See the description above ND_LOCK.
 */
#define NEED_SYNC	((void *)1)
#define NEED_WLOCK	((void *)2)
struct fnode *spadfs_add_fnode_to_directory(SPADFNODE *dir, const char *name,
					    unsigned namelen, unsigned ea_size,
					    struct buffer_head **bhp,
					    sector_t *fnode_address,
					    unsigned *fnode_off,
					    struct fnode_block **pfnode_block,
					    int wlock);
int spadfs_remove_fnode_from_directory(SPADFNODE *dir, SPADFNODE *file,
				       struct qstr *name);
int spadfs_move_fnode_to_directory(SPADFNODE *old_dir, struct qstr *old_name,
				   SPADFNODE *old_file, SPADFNODE *new_dir,
				   struct qstr *new_name,
				   struct dentry *new_dentry,
				   u8 *new_ea, unsigned new_ea_size);
