#include "spadfs.h"
#include "dir.h"

#if LINUX_VERSION_CODE < KERNEL_VERSION(3,3,0)
#define mode_type	int
#else
#define mode_type	umode_t
#endif

static struct dentry *spadfs_lookup(struct inode *dir, struct dentry *dentry,
#if LINUX_VERSION_CODE < KERNEL_VERSION(3,6,0)
				    struct nameidata *nd
#else
				    unsigned flags
#endif
				    )
{
	sync_lock_decl
	int r;
	spadfs_ino_t ino;
	struct inode *result;
	SPADFS *fs = spadfnode(dir)->fs;

	if (unlikely(dentry->d_name.len > MAX_NAME_LEN))
		return ERR_PTR(-ENAMETOOLONG);
	if (unlikely(is_deleted_file(spadfnode(dir))))
		return ERR_PTR(-ENOENT);

	down_read_sync_lock(fs);

	r = spadfs_lookup_ino(spadfnode(dir), &dentry->d_name, &ino, 0);
	if (unlikely(r)) {
		up_read_sync_lock(fs);
		if (r < 0)
			return ERR_PTR(r);
		goto not_found;
	}

	result = spadfs_iget(dir->i_sb, ino, spadfnode(dir)->fnode_block,
			     spadfnode(dir)->fnode_pos);
	up_read_sync_lock(fs);
	if (unlikely(IS_ERR(result)))
		return ERR_PTR(PTR_ERR(result));

	spadfs_set_dentry_operations(fs, dentry);
	d_add(dentry, result);
	return NULL;

not_found:
	spadfs_set_dentry_operations(fs, dentry);
	d_add(dentry, NULL);
	return NULL;
}

static int spadfs_get_dirent_args(struct fnode *fnode, unsigned size,
				  unsigned namelen, u64 *ino, unsigned *dt)
{
	struct fnode_ea *ea, *eas;
	unsigned ea_size;

	*dt = DT_REG;

	eas = (struct fnode_ea *)((char *)fnode + FNODE_EA_POS(namelen));
	ea_size = size - FNODE_EA_POS(namelen);

	ea = GET_EA(eas, ea_size, EA_UNX_MAGIC, EA_UNX_MAGIC_MASK);
	if (unlikely(!ea)) {
		ea = GET_EA(eas, ea_size, EA_SYMLINK_MAGIC,
			    EA_SYMLINK_MAGIC_MASK);
		if (unlikely(ea == GET_EA_ERROR))
			return -EFSERROR;

		if (likely(ea != NULL)) {
			*dt = DT_LNK;
			spadfs_validate_stable_ino(ino,
				((u64)SPAD2CPU32_LV(&fnode->run10)) |
				((u64)SPAD2CPU16_LV(&fnode->run11) << 32) |
				((u64)SPAD2CPU16_LV(&fnode->run1n) << 48));
		}
	} else if (unlikely(ea == GET_EA_ERROR)) {
		return -EFSERROR;
	} else {
		/*
		 * Dirent type is documented in include/linux/fs.h,
		 * so I hope it won't change
		 */
		*dt = (SPAD2CPU16_LV(&((struct ea_unx *)ea)->mode) >> 12) & 15;
		if (likely(SPAD2CPU32_LV(&ea->magic) == EA_UNX_MAGIC))
			spadfs_validate_stable_ino(ino,
				SPAD2CPU64_LV(&((struct ea_unx *)ea)->ino));
	}

	if (unlikely(fnode->flags & FNODE_FLAGS_DIR))
		*dt = DT_DIR;

	return 0;
}

static noinline int spadfs_get_hardlink_args(SPADFS *fs,
					     sector_t fixed_fnode_sec,
					     u64 *ino, unsigned *dt)
{
	int r;
	struct fnode_block *fixed_fnode_block;
	struct buffer_head *fbh;
	unsigned ffpos;
	struct fnode *fixed_fnode;
	unsigned size, namelen;

	fixed_fnode_block = spadfs_read_fnode_block(fs, fixed_fnode_sec, &fbh,
				SRFB_FIXED_FNODE, "spadfs_get_hardlink_args");
	if (unlikely(IS_ERR(fixed_fnode_block))) {
		r = PTR_ERR(fixed_fnode_block);
		goto brelse_ret;
	}

	r = spadfs_get_fixed_fnode_pos(fs, fixed_fnode_block, fixed_fnode_sec,
				       &ffpos);
	if (unlikely(r))
		goto brelse_ret_r;

	fixed_fnode = (struct fnode *)((char *)fixed_fnode_block + ffpos);

	size = SPAD2CPU16_LV(&fixed_fnode->next) & FNODE_NEXT_SIZE;
	namelen = fixed_fnode->namelen;

	if (unlikely(size < FNODE_EA_POS(namelen)) ||
	    unlikely(ffpos + size > FNODE_BLOCK_SIZE)) {
		spadfs_error(fs, TXFLAGS_FS_ERROR,
			"invalid size in fixed fnode %Lx",
			(unsigned long long)fixed_fnode_sec);
		r = -EFSERROR;
		goto brelse_ret_r;
	}

	r = spadfs_get_dirent_args(fixed_fnode, size, namelen, ino, dt);

brelse_ret_r:
	spadfs_brelse(fs, fbh);
brelse_ret:
	return r;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(3,11,0)
static int spadfs_readdir(struct file *file, void *dirent, filldir_t filldir)
#define f_pos				file->f_pos
#define emit(name, len, ino, type)	filldir(dirent, name, len, f_pos, ino, type)
#else
static int spadfs_readdir(struct file *file, struct dir_context *ctx)
#define f_pos				ctx->pos
#define emit(name, len, ino, type)	(!dir_emit(ctx, name, len, ino, type))
#endif
{
	sync_lock_decl
	int r;
	SPADFNODE *f;
	struct buffer_head *bh;
	struct fnode_block *fnode_block;
	sector_t secno;
	hash_t hash, next_hash;
	unsigned order, pos;
	unsigned current_order;
	sector_t c[2];
	c[1] = 0;
	f = spadfnode(file_inode(file));

	down_read_sync_lock(f->fs);

	if (unlikely(is_dir_off_t_special(f_pos))) {
		unsigned n = dir_off_t_special_n(f_pos);
		switch (n) {
			case 0:
				if (unlikely(emit(".", 1,
						spadfs_get_user_inode_number(file_inode(file)),
						DT_DIR)))
					goto ret_0;
				f_pos = make_special_dir_off_t(1);
				goto label_1;
			case 1:
			label_1:
				if (unlikely(emit("..", 2,
						spadfs_get_user_inode_number(
						    file_dentry(file)->d_parent->
						    d_inode),
						DT_DIR)))
					goto ret_0;
				f_pos = make_dir_off_t(0, 0,
							SIZEOF_FNODE_BLOCK);
				break;
			case 2:
				goto ret_0;
			case 3:
				r = -EFSERROR;
				goto ret_r;
			default:
				r = -EINVAL;
				goto ret_r;
		}
	}

	if (unlikely(is_deleted_file(f)))
		goto eof;

	hash = dir_off_t_hash(f_pos);
	order = dir_off_t_order(f_pos);
	pos = dir_off_t_pos(f_pos);

new_hash_lookup:
	fnode_block = spadfs_find_hash_block(f, hash, &bh, &secno, &next_hash);
	current_order = 0;
	if (unlikely(!fnode_block))
		goto use_next_hash;

next_fnode_block:
	if (unlikely(IS_ERR(fnode_block))) {
		r = PTR_ERR(fnode_block);
		goto ret_r;
	}
	if (likely(current_order >= order)) {
		unsigned size, namelen, fpos;
		struct fnode *fnode = fnode_block->fnodes;
next_fnode:
		VALIDATE_FNODE(f->fs, fnode_block, fnode, size, namelen,
			       ok, skip, bad_fnode);

ok:
		fpos = (char *)fnode - (char *)fnode_block;
		if (likely(fpos >= pos)) {
			u64 ino;
			unsigned dt;
			f_pos = make_dir_off_t(hash, current_order, fpos);
			if (likely(!(fnode->flags & FNODE_FLAGS_HARDLINK))) {
				ino = spadfs_ino_t_2_ino64_t(
						make_spadfs_ino_t(secno, fpos));
				if (unlikely(spadfs_get_dirent_args(fnode, size,
							namelen, &ino, &dt))) {
					spadfs_error(f->fs, TXFLAGS_FS_ERROR,
						"error parsing extended attributes on fnode %Lx/%x during readdir of %Lx/%x",
						(unsigned long long)secno,
						fpos,
						(unsigned long long)f->fnode_block,
						f->fnode_pos);
					goto brelse_ret_efserror;
				}
			} else {
				sector_t fixed_fnode_sec = MAKE_D_OFF(
						fnode->anode0, fnode->anode1);
				ino = spadfs_ino_t_2_ino64_t(
				      make_fixed_spadfs_ino_t(fixed_fnode_sec));
				dt = DT_UNKNOWN;
				r = spadfs_get_hardlink_args(f->fs,
						fixed_fnode_sec, &ino, &dt);
				if (unlikely(r))
					goto brelse_ret_r;
			}
			ino = spadfs_expose_inode_number(f->fs, ino);
			if (unlikely(emit(FNODE_NAME(fnode), namelen,
					     ino, dt))) {
				spadfs_brelse(f->fs, bh);
				goto ret_0;
			}
		}

skip:
		fnode = (struct fnode *)((char *)fnode + size);
		if (likely(((unsigned long)fnode & (FNODE_BLOCK_SIZE - 1)) !=
				0)) goto next_fnode;
		pos = SIZEOF_FNODE_BLOCK;
	}

	if (!(fnode_block->flags & FNODE_BLOCK_LAST)) {
		spadfs_brelse(f->fs, bh);
		secno++;
read_next_fnode_block:
		if (unlikely(spadfs_stop_cycles(f->fs, secno, &c,
						"spadfs_readdir"))) {
			r = -EFSERROR;
			goto ret_r;
		}
		current_order++;
		fnode_block = spadfs_read_fnode_block(f->fs, secno, &bh,
						SRFB_FNODE, "spadfs_readdir");
		goto next_fnode_block;
	}
	if (unlikely(CC_VALID(f->fs, &fnode_block->cc, &fnode_block->txc))) {
		secno = MAKE_D_OFF(fnode_block->next0, fnode_block->next1);
		spadfs_brelse(f->fs, bh);
		goto read_next_fnode_block;
	}
	spadfs_brelse(f->fs, bh);

use_next_hash:
	if (unlikely(next_hash != 0)) {
		order = 0;
		hash = next_hash;
		goto new_hash_lookup;
	}

eof:
	f_pos = make_special_dir_off_t(2);
ret_0:
	up_read_sync_lock(f->fs);
	return 0;

bad_fnode:
	spadfs_error(f->fs, TXFLAGS_FS_ERROR,
		"bad fnode on block %Lx when reading directory",
		(unsigned long long)secno);
brelse_ret_efserror:
	r = -EFSERROR;
brelse_ret_r:
	spadfs_brelse(f->fs, bh);
ret_r:
	/*
	 * On error, we must set invalid f_pos. Otherwise, Linux
	 * would loop on the erroneous entry forever.
	 */
	f_pos = make_special_dir_off_t(3);
	up_read_sync_lock(f->fs);
	return r;
#undef f_pos
#undef emit
}

static void spadfs_update_directory_times(struct inode *dir)
{
	time_t t = ktime_get_real_seconds();
	if (likely(t == dir->i_mtime.tv_sec) &&
	    likely(t == inode_get_ctime(dir).tv_sec))
		return;
	dir->i_mtime.tv_sec = t;
	dir->i_mtime.tv_nsec = 0;
	inode_set_ctime(dir, t, 0);
	spadfs_write_directory(spadfnode(dir));
}

static int spadfs_create(
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,12,0)
			 struct mnt_idmap *ns,
#endif
			 struct inode *dir, struct dentry *dentry,
			 mode_type mode,
#if LINUX_VERSION_CODE < KERNEL_VERSION(3,6,0)
			 struct nameidata *nd
#else
			 bool excl
#endif
			 )
{
	sync_lock_decl
	SPADFS *fs = spadfnode(dir)->fs;
	struct buffer_head *bh;
	sector_t fnode_address;
	unsigned fnode_off;
	struct fnode_block *fnode_block;
	struct fnode *fnode;
	int wlock = 0;
	int synced = 0;
	time_t ctime;
	struct inode *inode;
	int r;
	struct ea_unx *ea;
	u64 stable_ino;

#ifdef SPADFS_QUOTA
	dquot_initialize(dir);
#endif

	r = spadfs_new_stable_ino(fs, &stable_ino);
	if (unlikely(r))
		goto ret_r;

lock_and_again:
	ND_LOCK(fs, wlock);
	if (unlikely(is_deleted_file(spadfnode(dir)))) {
		r = -ENOENT;
		goto unlock_ret_r;
	}

	fnode = spadfs_add_fnode_to_directory(spadfnode(dir),
			(const char *)dentry->d_name.name, dentry->d_name.len,
			FNODE_EA_DO_ALIGN(sizeof(struct ea_unx)),
			&bh, &fnode_address, &fnode_off, &fnode_block, wlock);
	if (unlikely(IS_ERR(fnode))) {
		if (likely(fnode == ERR_PTR(-ENOSPC)) && !synced)
			goto unlock_do_sync;
		r = PTR_ERR(fnode);
		goto unlock_ret_r;
	}
	if (unlikely(fnode == NEED_SYNC))
		goto unlock_do_sync;
	if (unlikely(fnode == NEED_WLOCK)) {
		BUG_ON(wlock);
		ND_UNLOCK(fs, wlock);
		wlock = 1;
		goto lock_and_again;
	}

	ctime = ktime_get_real_seconds();
	fnode->size[0] = CPU2SPAD64_CONST(0);
	fnode->size[1] = CPU2SPAD64_CONST(0);
	fnode->ctime = fnode->mtime = CPU2SPAD32((u32)ctime);
	fnode->run10 = MAKE_PART_0(0);
	fnode->run11 = MAKE_PART_1(0);
	fnode->run1n = CPU2SPAD16_CONST(0);
	fnode->run20 = MAKE_PART_0(0);
	fnode->run21 = MAKE_PART_1(0);
	fnode->run2n = CPU2SPAD16_CONST(0);
	fnode->anode0 = MAKE_PART_0(0);
	fnode->anode1 = MAKE_PART_1(0);
	fnode->flags = 0;
	fnode->namelen = dentry->d_name.len;
	spadfs_set_name(fs, FNODE_NAME(fnode),
			(const char *)dentry->d_name.name, dentry->d_name.len);
	ea = (struct ea_unx *)((char *)fnode +
					FNODE_EA_POS(dentry->d_name.len));
	spadfs_init_unx_attribute(dir, ea, mode, stable_ino);
	do_fnode_block_checksum(fs, fnode_block);
	end_concurrent_atomic_buffer_modify(fs, bh);

	inode = spadfs_new_inode(dir, mode, ctime, ea,
				 FNODE_EA_DO_ALIGN(sizeof(struct ea_unx)),
				 fnode_address, fnode_off, 0, stable_ino);
	spadfs_brelse(fs, bh);
	if (unlikely(IS_ERR(inode))) {
		r = PTR_ERR(inode);
		goto remove_entry_unlock_ret_r;
	}

	spadfs_update_directory_times(dir);
	d_instantiate(dentry, inode);
	ND_UNLOCK(fs, wlock);
	return 0;

unlock_do_sync:
	ND_UNLOCK(fs, wlock);
	synced = 1;
	r = spadfs_commit(fs);
	if (unlikely(r))
		return r;
	goto lock_and_again;

remove_entry_unlock_ret_r:
	spadfs_remove_fnode_from_directory(spadfnode(dir), NULL,
					   &dentry->d_name);
unlock_ret_r:
	ND_UNLOCK(fs, wlock);
ret_r:
	return r;
}

static int spadfs_mkdir(
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,12,0)
			struct mnt_idmap *ns,
#endif
			struct inode *dir, struct dentry *dentry,
			mode_type mode)
{
	sync_lock_decl
	SPADFS *fs = spadfnode(dir)->fs;
	struct buffer_head *bh;
	sector_t fnode_address;
	unsigned fnode_off;
	struct fnode_block *fnode_block;
	struct fnode *fnode;
	int wlock = 0;
	int synced = 0;
	time_t ctime;
	struct inode *inode;
	int r;
	struct ea_unx *ea;
	sector_t new_dir;
	u16 hint_small, hint_big;
	u64 stable_ino;

#ifdef SPADFS_QUOTA
	dquot_initialize(dir);
#endif

	r = spadfs_new_stable_ino(fs, &stable_ino);
	if (unlikely(r))
		goto ret_r;

	mode |= S_IFDIR;

lock_and_again:
	ND_LOCK(fs, wlock);
	/*
	 * if hint reading were after spadfs_add_fnode_to_directory,
	 * it could deadlock
	 */
	spadfs_get_dir_hint(spadfnode(dir), &hint_small, &hint_big);

	if (unlikely(is_deleted_file(spadfnode(dir)))) {
		ND_UNLOCK(fs, wlock);
		r = -ENOENT;
		goto ret_r;
	}
	r = spadfs_alloc_leaf_page(spadfnode(dir), spadfnode(dir)->fnode_block, 1U << fs->sectors_per_disk_block_bits, 0, &new_dir, 1);
	if (unlikely(r)) {
		if (likely(r == -ENOSPC) && !synced)
			goto unlock_do_sync;
		goto unlock_ret_r;
	}
	fnode = spadfs_add_fnode_to_directory(spadfnode(dir),
			(const char *)dentry->d_name.name, dentry->d_name.len,
			FNODE_EA_DO_ALIGN(sizeof(struct ea_unx)), &bh,
			&fnode_address, &fnode_off, &fnode_block, wlock);
	if (unlikely(IS_ERR(fnode))) {
		r = spadfs_free_blocks_metadata(fs, new_dir, 1U << fs->sectors_per_disk_block_bits);
		if (unlikely(r))
			goto unlock_ret_r;
		if (likely(fnode == ERR_PTR(-ENOSPC)) && !synced)
			goto unlock_do_sync;
		r = PTR_ERR(fnode);
		goto unlock_ret_r;
	}
	if (unlikely(fnode == NEED_SYNC)) {
		r = spadfs_free_blocks_metadata(fs, new_dir, 1U << fs->sectors_per_disk_block_bits);
		if (unlikely(r))
			goto unlock_ret_r;
		goto unlock_do_sync;
	}
	if (unlikely(fnode == NEED_WLOCK)) {
		BUG_ON(wlock);
		r = spadfs_free_blocks_metadata(fs, new_dir, 1U << fs->sectors_per_disk_block_bits);
		if (unlikely(r))
			goto unlock_ret_r;
		ND_UNLOCK(fs, wlock);
		wlock = 1;
		goto lock_and_again;
	}

	ctime = ktime_get_real_seconds();
	fnode->size[0] = fnode->size[1] = CPU2SPAD64(directory_size(fs) ? 512U << fs->sectors_per_disk_block_bits : 0);
	fnode->ctime = fnode->mtime = CPU2SPAD32((u32)ctime);
	fnode->run10 = MAKE_PART_0(new_dir);
	fnode->run11 = MAKE_PART_1(new_dir);
	fnode->run1n = CPU2SPAD16(hint_small);
	fnode->run20 = MAKE_PART_0(new_dir);
	fnode->run21 = MAKE_PART_1(new_dir);
	fnode->run2n = CPU2SPAD16(hint_big);
	fnode->anode0 = MAKE_PART_0(0);
	fnode->anode1 = MAKE_PART_1(0);
	fnode->flags = FNODE_FLAGS_DIR;
	fnode->namelen = dentry->d_name.len;
	spadfs_set_name(fs, FNODE_NAME(fnode),
			(const char *)dentry->d_name.name, dentry->d_name.len);
	ea = (struct ea_unx *)((char *)fnode +
					FNODE_EA_POS(dentry->d_name.len));
	spadfs_init_unx_attribute(dir, ea, mode, stable_ino);
	do_fnode_block_checksum(fs, fnode_block);
	end_concurrent_atomic_buffer_modify(fs, bh);

	inode = spadfs_new_inode(dir, mode, ctime, ea,
				 FNODE_EA_DO_ALIGN(sizeof(struct ea_unx)),
				 fnode_address, fnode_off, new_dir, stable_ino);
	spadfs_brelse(fs, bh);
	if (unlikely(IS_ERR(inode))) {
		r = PTR_ERR(inode);
		goto remove_entry_unlock_ret_r;
	}

	spadfs_update_directory_times(dir);
	d_instantiate(dentry, inode);
	ND_UNLOCK(fs, wlock);
	return 0;

unlock_do_sync:
	ND_UNLOCK(fs, wlock);
	synced = 1;
	r = spadfs_commit(fs);
	if (unlikely(r))
		return r;
	goto lock_and_again;

remove_entry_unlock_ret_r:
	spadfs_remove_fnode_from_directory(spadfnode(dir), NULL, &dentry->d_name);
	spadfs_free_blocks_metadata(fs, new_dir, 1U << fs->sectors_per_disk_block_bits);
unlock_ret_r:
	ND_UNLOCK(fs, wlock);
ret_r:
	return r;
}

static int spadfs_mknod(
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,12,0)
			struct mnt_idmap *ns,
#endif
			struct inode *dir, struct dentry *dentry,
			mode_type mode, dev_t rdev)
{
	sync_lock_decl
	SPADFS *fs = spadfnode(dir)->fs;
	struct buffer_head *bh;
	sector_t fnode_address;
	unsigned fnode_off;
	struct fnode_block *fnode_block;
	struct fnode *fnode;
	int wlock = 0;
	int synced = 0;
	time_t ctime;
	struct inode *inode;
	int r;
	struct ea_unx *ea;
	int rdev_needed;
	u64 stable_ino;

#ifdef SPADFS_QUOTA
	dquot_initialize(dir);
#endif

	r = spadfs_new_stable_ino(fs, &stable_ino);
	if (unlikely(r))
		goto ret_r;

	if (likely(S_ISFIFO(mode)) || likely(S_ISSOCK(mode))) {
		rdev_needed = 0;
	} else if (unlikely(S_ISBLK(mode)) || unlikely(S_ISCHR(mode))) {
		rdev_needed = 1;
		if (unlikely(!huge_valid_dev(rdev)))
			return -EINVAL;
	} else {
		return -EINVAL;
	}

lock_and_again:
	ND_LOCK(fs, wlock);
	if (unlikely(is_deleted_file(spadfnode(dir)))) {
		r = -ENOENT;
		goto unlock_ret_r;
	}

	fnode = spadfs_add_fnode_to_directory(spadfnode(dir),
			(const char *)dentry->d_name.name, dentry->d_name.len,
			FNODE_EA_DO_ALIGN(sizeof(struct ea_unx)) +
				(rdev_needed ?
				FNODE_EA_DO_ALIGN(sizeof(struct ea_rdev)) : 0),
			&bh, &fnode_address, &fnode_off, &fnode_block, wlock);
	if (unlikely(IS_ERR(fnode))) {
		if (likely(fnode == ERR_PTR(-ENOSPC)) && !synced)
			goto unlock_do_sync;
		r = PTR_ERR(fnode);
		goto unlock_ret_r;
	}
	if (unlikely(fnode == NEED_SYNC))
		goto unlock_do_sync;
	if (unlikely(fnode == NEED_WLOCK)) {
		BUG_ON(wlock);
		ND_UNLOCK(fs, wlock);
		wlock = 1;
		goto lock_and_again;
	}

	ctime = ktime_get_real_seconds();
	fnode->size[0] = CPU2SPAD64_CONST(0);
	fnode->size[1] = CPU2SPAD64_CONST(0);
	fnode->ctime = fnode->mtime = CPU2SPAD32((u32)ctime);
	fnode->run10 = MAKE_PART_0(0);
	fnode->run11 = MAKE_PART_1(0);
	fnode->run1n = CPU2SPAD16_CONST(0);
	fnode->run20 = MAKE_PART_0(0);
	fnode->run21 = MAKE_PART_1(0);
	fnode->run2n = CPU2SPAD16_CONST(0);
	fnode->anode0 = MAKE_PART_0(0);
	fnode->anode1 = MAKE_PART_1(0);
	fnode->flags = 0;
	fnode->namelen = dentry->d_name.len;
	spadfs_set_name(fs, FNODE_NAME(fnode),
			(const char *)dentry->d_name.name, dentry->d_name.len);
	ea = (struct ea_unx *)((char *)fnode +
					FNODE_EA_POS(dentry->d_name.len));
	spadfs_init_unx_attribute(dir, ea, mode, stable_ino);
	if (rdev_needed) {
		struct ea_rdev *rd = (struct ea_rdev *)((char *)ea +
				FNODE_EA_DO_ALIGN(sizeof(struct ea_unx)));
		CPU2SPAD32_LV(&rd->magic, EA_RDEV_MAGIC);
		CPU2SPAD32_LV(&rd->pad, 0);
		CPU2SPAD64_LV(&rd->dev, huge_encode_dev(rdev));
	}
	do_fnode_block_checksum(fs, fnode_block);
	end_concurrent_atomic_buffer_modify(fs, bh);

	inode = spadfs_new_inode(dir, S_IFREG, ctime, ea,
				 FNODE_EA_DO_ALIGN(sizeof(struct ea_unx)) +
				     (rdev_needed ?
				     FNODE_EA_DO_ALIGN(sizeof(struct ea_rdev)) :
				     0),
				 fnode_address, fnode_off, 0, stable_ino);
	spadfs_brelse(fs, bh);
	if (unlikely(IS_ERR(inode))) {
		r = PTR_ERR(inode);
		goto remove_entry_unlock_ret_r;
	}

	spadfs_update_directory_times(dir);
	d_instantiate(dentry, inode);
	ND_UNLOCK(fs, wlock);
	return 0;

unlock_do_sync:
	ND_UNLOCK(fs, wlock);
	synced = 1;
	r = spadfs_commit(fs);
	if (unlikely(r))
		return r;
	goto lock_and_again;

remove_entry_unlock_ret_r:
	spadfs_remove_fnode_from_directory(spadfnode(dir), NULL,
					   &dentry->d_name);
unlock_ret_r:
	ND_UNLOCK(fs, wlock);
ret_r:
	return r;
}

static int spadfs_symlink(
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,12,0)
			  struct mnt_idmap *ns,
#endif
			  struct inode *dir, struct dentry *dentry,
			  const char *symlink)
{
	sync_lock_decl
	SPADFS *fs = spadfnode(dir)->fs;
	struct buffer_head *bh;
	sector_t fnode_address;
	unsigned fnode_off;
	struct fnode_block *fnode_block;
	struct fnode *fnode;
	int wlock = 0;
	int synced = 0;
	time_t ctime;
	struct inode *inode;
	int r;
	struct fnode_ea *ea;
	unsigned strlen_symlink;
	unsigned ea_size;
	u64 stable_ino;
	umode_t mode;
	uid_t uid;
	gid_t gid;

#ifdef SPADFS_QUOTA
	dquot_initialize(dir);
#endif

	r = spadfs_new_stable_ino(fs, &stable_ino);
	if (unlikely(r))
		goto ret_r;

	mode = S_IFLNK | S_IRWXUGO;
	spadfs_get_initial_attributes(dir, &mode, &uid, &gid);

	strlen_symlink = strlen(symlink);
	ea_size = FNODE_EA_DO_ALIGN(sizeof(struct fnode_ea) + strlen_symlink);
	if (unlikely(ea_size > FNODE_MAX_EA_SIZE))
		return -ENAMETOOLONG;

lock_and_again:
	ND_LOCK(fs, wlock);
	if (unlikely(is_deleted_file(spadfnode(dir)))) {
		r = -ENOENT;
		goto unlock_ret_r;
	}
	fnode = spadfs_add_fnode_to_directory(spadfnode(dir),
			(const char *)dentry->d_name.name, dentry->d_name.len,
			ea_size,
			&bh, &fnode_address, &fnode_off, &fnode_block, wlock);
	if (unlikely(IS_ERR(fnode))) {
		if (likely(fnode == ERR_PTR(-ENOSPC)) && !synced)
			goto unlock_do_sync;
		r = PTR_ERR(fnode);
		goto unlock_ret_r;
	}
	if (unlikely(fnode == NEED_SYNC))
		goto unlock_do_sync;
	if (unlikely(fnode == NEED_WLOCK)) {
		BUG_ON(wlock);
		ND_UNLOCK(fs, wlock);
		wlock = 1;
		goto lock_and_again;
	}
	ctime = ktime_get_real_seconds();
	fnode->size[0] = CPU2SPAD64_CONST(0);
	fnode->size[1] = CPU2SPAD64_CONST(0);
	fnode->ctime = fnode->mtime = CPU2SPAD32((u32)ctime);
	fnode->run10 = CPU2SPAD32(stable_ino);
	fnode->run11 = CPU2SPAD16(stable_ino >> 32);
	fnode->run1n = CPU2SPAD16(stable_ino >> 48);
	fnode->run20 = CPU2SPAD32(uid);
	fnode->run21 = CPU2SPAD16(gid);
	fnode->run2n = CPU2SPAD16(gid >> 16);
	fnode->anode0 = MAKE_PART_0(0);
	fnode->anode1 = MAKE_PART_1(0);
	fnode->flags = 0;
	fnode->namelen = dentry->d_name.len;
	spadfs_set_name(fs, FNODE_NAME(fnode),
			(const char *)dentry->d_name.name, dentry->d_name.len);
	ea = (struct fnode_ea *)((char *)fnode +
					FNODE_EA_POS(dentry->d_name.len));
	CPU2SPAD32_LV(&ea->magic,
		EA_SYMLINK_MAGIC | (strlen_symlink << FNODE_EA_SIZE_SHIFT));
	strncpy((char *)(ea + 1), symlink, ea_size - sizeof(struct fnode_ea));
	do_fnode_block_checksum(fs, fnode_block);
	end_concurrent_atomic_buffer_modify(fs, bh);

	inode = spadfs_new_inode(dir, S_IFREG, ctime, ea, ea_size,
				 fnode_address, fnode_off, 0, stable_ino);
	spadfs_brelse(fs, bh);
	if (unlikely(IS_ERR(inode))) {
		r = PTR_ERR(inode);
		goto remove_entry_unlock_ret_r;
	}
	i_uid_write(inode, uid);
	i_gid_write(inode, gid);
	spadfs_update_directory_times(dir);
	d_instantiate(dentry, inode);
	ND_UNLOCK(fs, wlock);
	return 0;

unlock_do_sync:
	ND_UNLOCK(fs, wlock);
	synced = 1;
	r = spadfs_commit(fs);
	if (unlikely(r))
		return r;
	goto lock_and_again;

remove_entry_unlock_ret_r:
	spadfs_remove_fnode_from_directory(spadfnode(dir), NULL,
					   &dentry->d_name);
unlock_ret_r:
	ND_UNLOCK(fs, wlock);
ret_r:
	return r;
}

static int spadfs_link(struct dentry *old_dentry, struct inode *new_dir,
		       struct dentry *new_dentry)
{
	/*sync_lock_decl*/
	struct inode *file = old_dentry->d_inode;
	struct inode *old_dir = old_dentry->d_parent->d_inode;
	SPADFS *fs = spadfnode(file)->fs;
	struct buffer_head *bh;
	sector_t link_address;
	unsigned link_off;
	struct fnode_block *link_fnode_block;
	struct fnode *fnode;
	int synced = 0;
	int r;

#ifdef SPADFS_QUOTA
	dquot_initialize(new_dir);
#endif

lock_and_again:
	down_write_sync_lock(fs);
	if (!is_fnode_fixed(spadfnode(file))) {
		/*
		 * This is the first link ---
		 * we need to move file to its own new block
		 */
		sector_t new_fxblk;
		u16 hint_small, hint_big;
		spadfs_get_dir_hint(spadfnode(old_dir), &hint_small, &hint_big);
		if (unlikely(spadfnode(file)->spadfs_nlink > 1)) {
			spadfs_error(fs, TXFLAGS_FS_ERROR,
				"file has more links but is in directory");
			r = -EFSERROR;
			goto unlock_ret_r;
		}

		r = spadfs_alloc_fixed_fnode_block(spadfnode(old_dir),
					spadfnode(old_dir)->fnode_block,
					FNODE_SIZE(0, spadfnode(file)->ea_size),
					hint_small, hint_big, &new_fxblk);
		if (unlikely(r)) {
			if (likely(r == -ENOSPC && !synced))
				goto unlock_do_sync;
			goto unlock_ret_r;
		}

		fnode = spadfs_add_fnode_to_directory(spadfnode(old_dir),
				(const char *)old_dentry->d_name.name,
				old_dentry->d_name.len,
				0, &bh,
				&link_address, &link_off, &link_fnode_block, 1);
		if (unlikely(IS_ERR(fnode))) {
			r = spadfs_free_blocks_metadata(fs, new_fxblk, 1U << fs->sectors_per_disk_block_bits);
			if (unlikely(r))
				goto unlock_ret_r;
add_fnode_error:
			if (likely(fnode == ERR_PTR(-ENOSPC)) && !synced)
				goto unlock_do_sync;
			r = PTR_ERR(fnode);
			goto unlock_ret_r;
		}
		if (unlikely(fnode == NEED_SYNC)) {
			r = spadfs_free_blocks_metadata(fs, new_fxblk, 1U << fs->sectors_per_disk_block_bits);
			if (unlikely(r))
				goto unlock_ret_r;
			goto unlock_do_sync;
		}
		BUG_ON(fnode == NEED_WLOCK);

		make_fixed_fnode_reference(fnode, new_fxblk);
		fnode->namelen = old_dentry->d_name.len;
		spadfs_set_name(fs, FNODE_NAME(fnode),
				(const char *)old_dentry->d_name.name,
				old_dentry->d_name.len);
		do_fnode_block_checksum(fs, link_fnode_block);
		end_concurrent_atomic_buffer_modify(fs, bh);
		spadfs_brelse(fs, bh);

		spadfs_remove_fnode_from_directory(spadfnode(old_dir),
					spadfnode(file), &old_dentry->d_name);

		spadfnode(file)->fnode_block = new_fxblk;
		spadfnode(file)->fnode_pos = FIXED_FNODE_BLOCK_FNODE0;
		spadfnode(file)->spadfs_ino =
					make_fixed_spadfs_ino_t(new_fxblk);
		spadfs_set_parent_fnode(spadfnode(file), 0, 0);
		remove_inode_hash(file);
		__insert_inode_hash(file, spadfs_ino_t_2_ino_t(spadfnode(file)->spadfs_ino));
		spadfs_write_file(spadfnode(file), 0, NULL, NULL);
	}

	fnode = spadfs_add_fnode_to_directory(spadfnode(new_dir),
			(const char *)new_dentry->d_name.name,
			new_dentry->d_name.len,
			0, &bh,
			&link_address, &link_off, &link_fnode_block, 1);
	if (unlikely(IS_ERR(fnode)))
		goto add_fnode_error;
	if (unlikely(fnode == NEED_SYNC))
		goto unlock_do_sync;
	BUG_ON(fnode == NEED_WLOCK);

	make_fixed_fnode_reference(fnode, spadfnode(file)->fnode_block);
	fnode->namelen = new_dentry->d_name.len;
	spadfs_set_name(fs, FNODE_NAME(fnode),
			(const char *)new_dentry->d_name.name,
			new_dentry->d_name.len);
	do_fnode_block_checksum(fs, link_fnode_block);
	end_concurrent_atomic_buffer_modify(fs, bh);
	spadfs_brelse(fs, bh);

	/* if (!S_ISDIR(inode(file)->i_mode)) mutex_lock(&spadfnode(new_file)->file_lock); we're already in fs write lock */
	spadfnode(file)->spadfs_nlink++;
	spadfs_set_nlink(file);
	r = spadfs_refile_fixed_fnode(spadfnode(file), NULL, 0);
	if (unlikely(r)) {
		spadfnode(file)->spadfs_nlink--;
		spadfs_set_nlink(file);
	} else {
		atomic_inc(&file->i_count);
		spadfs_update_directory_times(new_dir);
		d_instantiate(new_dentry, file);
	}
	/* if (!S_ISDIR(inode(file)->i_mode)) mutex_unlock(&spadfnode(new_file)->file_lock); */
	up_write_sync_lock(fs);
	return r;

unlock_do_sync:
	up_write_sync_lock(fs);
	synced = 1;
	r = spadfs_commit(fs);
	if (unlikely(r))
		return r;
	goto lock_and_again;

unlock_ret_r:
	up_write_sync_lock(fs);
	return r;
}

static void set_deleted_file(SPADFNODE *file)
{
	if (unlikely(is_fnode_fixed(file)))
		spadfs_free_blocks_metadata(file->fs, file->fnode_block, 1U << file->fs->sectors_per_disk_block_bits);

	file->fnode_block = 0;
	file->fnode_pos = 0;
	spadfs_set_parent_fnode(file, 0, 0);
	file->spadfs_ino = spadfs_ino_t_deleted;
	remove_inode_hash(inode(file));
	__insert_inode_hash(inode(file), spadfs_ino_t_2_ino_t(spadfs_ino_t_deleted));
	file->spadfs_nlink = 0;
	clear_nlink(inode(file));
}

int spadfs_unlink_unlocked(SPADFNODE *dir, struct dentry *dentry)
{
	SPADFNODE *file = spadfnode(dentry->d_inode);
	int r;

	r = spadfs_remove_fnode_from_directory(dir, file, &dentry->d_name);
	if (unlikely(r))
		return r;

	if (unlikely(file->spadfs_nlink != 1) &&
	    likely(!S_ISDIR(inode(file)->i_mode))) {
		file->spadfs_nlink--;
		spadfs_set_nlink(inode(file));
		r = spadfs_refile_fixed_fnode(file, NULL, 0);
		return r;
	}
	if (likely(!S_ISDIR(inode(file)->i_mode)))
		spadfs_create_memory_extents(file);

	set_deleted_file(file);
	return 0;
}

static int spadfs_unlink(struct inode *dir, struct dentry *dentry)
{
	sync_lock_decl
	int r;
	SPADFNODE *file = spadfnode(dentry->d_inode);

#ifdef SPADFS_QUOTA
	dquot_initialize(dir);
#endif

	down_read_sync_lock(file->fs);
	mutex_lock(&file->file_lock);
	r = spadfs_unlink_unlocked(spadfnode(dir), dentry);
	mutex_unlock(&file->file_lock);
	if (!r)
		spadfs_update_directory_times(dir);
	up_read_sync_lock(file->fs);

	return r;
}

static int spadfs_rmdir(struct inode *dir, struct dentry *dentry)
{
	sync_lock_decl
	SPADFNODE *file = spadfnode(dentry->d_inode);
	int r;

#ifdef SPADFS_QUOTA
	dquot_initialize(dir);
	dquot_initialize(inode(file));
#endif

	down_read_sync_lock(file->fs);

	r = spadfs_check_directory_empty(file);
	if (unlikely(r))
		goto unlock_ret_r;

	r = spadfs_remove_directory(file);
	if (unlikely(r))
		goto unlock_ret_r;

	r = spadfs_unlink_unlocked(spadfnode(dir), dentry);
	if (!r)
		spadfs_update_directory_times(dir);

unlock_ret_r:
	up_read_sync_lock(file->fs);
	return r;
}

static int spadfs_rename(
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,12,0)
			 struct mnt_idmap *ns,
#endif
			 struct inode *old_dir, struct dentry *old_dentry,
			 struct inode *new_dir, struct dentry *new_dentry
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,17,0)
			 , unsigned flags
#endif
			 )
{
	/*sync_lock_decl*/
	SPADFS *fs = spadfnode(old_dir)->fs;
	int r;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,17,0)
	if (unlikely(flags & ~RENAME_NOREPLACE))
		return -EINVAL;
#endif

#ifdef SPADFS_QUOTA
	dquot_initialize(old_dir);
	dquot_initialize(new_dir);
	if (new_dentry->d_inode)
		dquot_initialize(new_dentry->d_inode);
#endif

	down_write_sync_lock(fs);

	r = spadfs_move_fnode_to_directory(spadfnode(old_dir),
			&old_dentry->d_name, spadfnode(old_dentry->d_inode),
			spadfnode(new_dir), &new_dentry->d_name, new_dentry,
			NULL, 0);

	if (!r) {
		spadfs_update_directory_times(old_dir);
		spadfs_update_directory_times(new_dir);
	}

	up_write_sync_lock(fs);

	return r;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(5,12,0)
static int spadfs_dir_setattr(struct dentry *dentry, struct iattr *iattr)
#else
static int spadfs_dir_setattr(struct mnt_idmap *ns, struct dentry *dentry, struct iattr *iattr)
#endif
{
	sync_lock_decl
	struct inode *inode;
	int r;

#if LINUX_VERSION_CODE < KERNEL_VERSION(5,12,0)
	r = spadfs_setattr_common(dentry, iattr);
#else
	r = spadfs_setattr_common(ns, dentry, iattr);
#endif
	if (unlikely(r))
		return r;

	inode = dentry->d_inode;

	down_read_sync_lock(spadfnode(inode)->fs);
	spadfs_update_ea(inode);
	r = spadfs_write_directory(spadfnode(inode));
	up_read_sync_lock(spadfnode(inode)->fs);

	return r;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,35)
static int spadfs_dir_fsync(struct file *file, struct dentry *dentry,
			    int datasync)
#elif LINUX_VERSION_CODE < KERNEL_VERSION(3,1,0)
static int spadfs_dir_fsync(struct file *file, int datasync)
#else
static int spadfs_dir_fsync(struct file *file, loff_t start, loff_t end, int datasync)
#endif
{
	struct inode *i = file_inode(file);
	return spadfs_commit(spadfnode(i)->fs);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,21)
const
#endif
struct inode_operations spadfs_dir_iops = {
	.lookup = spadfs_lookup,
	.create = spadfs_create,
	.mkdir = spadfs_mkdir,
	.mknod = spadfs_mknod,
	.symlink = spadfs_symlink,
	.link = spadfs_link,
	.unlink = spadfs_unlink,
	.rmdir = spadfs_rmdir,
#if LINUX_VERSION_CODE < KERNEL_VERSION(3,17,0) || LINUX_VERSION_CODE >= KERNEL_VERSION(4,9,0)
	.rename = spadfs_rename,
#else
	.rename2 = spadfs_rename,
#endif
	.setattr = spadfs_dir_setattr,
	.getattr = spadfs_getattr,
#ifdef SPADFS_XATTR
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,9,0)
	.setxattr = generic_setxattr,
	.getxattr = generic_getxattr,
	.removexattr = generic_removexattr,
#endif
	.listxattr = spadfs_listxattr,
#endif
};

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,17)
const
#endif
struct file_operations spadfs_dir_fops = {
	.llseek = generic_file_llseek,
	.read = generic_read_dir,
#if LINUX_VERSION_CODE < KERNEL_VERSION(3,11,0)
	.readdir = spadfs_readdir,
#elif LINUX_VERSION_CODE < KERNEL_VERSION(4,7,0)
	.iterate = spadfs_readdir,
#else
	.iterate_shared = spadfs_readdir,
#endif
	.fsync = spadfs_dir_fsync,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,11)
	.unlocked_ioctl = spadfs_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = spadfs_compat_ioctl,
#endif
#endif
};
