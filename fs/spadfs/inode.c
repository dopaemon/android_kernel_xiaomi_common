#include "spadfs.h"

/* Helpers for iget5_locked */

static int spadfs_iget_test(struct inode *inode, void *data)
{
	spadfs_ino_t *ino = data;
	return spadfnode(inode)->spadfs_ino == *ino;
}

static int spadfs_iget_init(struct inode *inode, void *data)
{
	spadfs_ino_t *ino = data;
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,37)
	static atomic_t next_ino = ATOMIC_INIT(0);
	inode->i_ino = (unsigned)atomic_inc_return(&next_ino) - 1;
#else
	inode->i_ino = get_next_ino();
#endif
	spadfnode(inode)->spadfs_ino = *ino;
	spadfnode(inode)->stable_ino = 0;
	return 0;
}

struct fnode_ea *spadfs_get_ea(SPADFNODE *f, u32 what, u32 mask)
{
	struct fnode_ea *ea = GET_EA((struct fnode_ea *)f->ea, f->ea_size,
				     what, mask);
	if (unlikely(ea == GET_EA_ERROR)) {
		spadfs_error(f->fs, TXFLAGS_FS_ERROR,
			"error parsing extended attributes on fnode %Lx/%x",
			(unsigned long long)f->fnode_block,
			f->fnode_pos);
		return ERR_PTR(-EFSERROR);
	}
	return ea;
}

void spadfs_find_ea_unx(SPADFNODE *f)
{
	struct ea_unx *ea;
	ea = (struct ea_unx *)spadfs_get_ea(f, EA_UNX_MAGIC, EA_UNX_MAGIC_MASK);
	if (unlikely(IS_ERR(ea)))
		ea = NULL;
	f->ea_unx = ea;
}

void spadfs_validate_stable_ino(u64 *result, u64 ino)
{
	if (likely(ino > SPADFS_INO_ROOT))
		*result = ino;
}

u64 spadfs_expose_inode_number(SPADFS *fs, u64 ino)
{
	if (unlikely(fs->mount_flags & MOUNT_FLAGS_64BIT_INO_FORCE)) {
		if (likely(!(ino & 0xFFFFFFFF00000000ULL)))
			ino |= 0xFFFFFFFF00000000ULL;
	}
	if (!(fs->mount_flags & MOUNT_FLAGS_64BIT_INO))
		ino = (u32)ino;
	if (unlikely(ino < SPADFS_INO_ROOT))
		ino += SPADFS_INO_INITIAL_REGION;
	return ino;
}

/*
 * Read extended attributes and fill inode bits. On error, do not return error
 * and continue as if there were no extended attributes. However, error is
 * written to syslog and TXFLAGS_EA_ERROR bit is set on filesystem, so that fsck
 * will automatically correct it on next reboot.
 */

static void spadfs_get_ea_unx(struct inode *inode, struct fnode *fnode)
{
	umode_t mode;
	struct ea_unx *ea;
	struct fnode_ea *lnk;
	SPADFS *fs = spadfnode(inode)->fs;
	lnk = spadfs_get_ea(spadfnode(inode),
			    EA_SYMLINK_MAGIC, EA_SYMLINK_MAGIC_MASK);
	if (unlikely(IS_ERR(lnk)))
		return;
	if (unlikely(lnk != NULL) && likely(!inode->i_size)) {
		inode->i_mode = S_IFLNK | S_IRWXUGO;
		inode->i_op = &spadfs_symlink_iops;
		inode->i_size = (SPAD2CPU32_LV(&lnk->magic) >> FNODE_EA_SIZE_SHIFT) &
				FNODE_EA_SIZE_MASK_1;
		if (fnode) {
			spadfs_validate_stable_ino(&spadfnode(inode)->stable_ino,
				((u64)SPAD2CPU32_LV(&fnode->run10)) |
				((u64)SPAD2CPU16_LV(&fnode->run11) << 32) |
				((u64)SPAD2CPU16_LV(&fnode->run1n) << 48));
			i_uid_write(inode, SPAD2CPU32_LV(&fnode->run20));
			i_gid_write(inode, SPAD2CPU16_LV(&fnode->run21) | ((u32)SPAD2CPU16_LV(&fnode->run2n) << 16));
		}
		return;
	}

	spadfs_find_ea_unx(spadfnode(inode));
	ea = spadfnode(inode)->ea_unx;
	if (unlikely(!ea))
		return;
	if (likely(SPAD2CPU32_LV(&ea->magic) == EA_UNX_MAGIC))
		spadfs_validate_stable_ino(&spadfnode(inode)->stable_ino,
					   SPAD2CPU64_LV(&ea->ino));
	mode = SPAD2CPU16_LV(&ea->mode);
	if (S_ISDIR(mode)) {
		if (unlikely(!S_ISDIR(inode->i_mode))) {
			spadfs_error(fs, TXFLAGS_EA_ERROR,
				"UNX extended attribute error on fnode %Lx/%x: non-directory has directory mode %06o",
				(unsigned long long)spadfnode(inode)->fnode_block,
				spadfnode(inode)->fnode_pos, mode);
			return;
		}
	} else {
		if (unlikely(!S_ISREG(inode->i_mode))) {
			spadfs_error(fs, TXFLAGS_EA_ERROR,
				"UNX extended attribute error on fnode %Lx/%x: directory has non-directory mode %06o",
				(unsigned long long)spadfnode(inode)->fnode_block,
				spadfnode(inode)->fnode_pos,
				mode);
			return;
		}
		if (unlikely(!S_ISREG(mode))) {
			dev_t rdev = 0;
			if (likely(S_ISCHR(mode)) || S_ISBLK(mode)) {
				struct ea_rdev *rd = (struct ea_rdev *)
					spadfs_get_ea(spadfnode(inode),
						EA_RDEV_MAGIC, ~0);
				if (unlikely(IS_ERR(rd)))
					return;
				if (unlikely(!rd)) {
					spadfs_error(fs, TXFLAGS_EA_ERROR,
						"UNX extended attribute error on fnode %Lx/%x: no rdev attribute for fnode with mode %06o",
						(unsigned long long)spadfnode(inode)->fnode_block,
						spadfnode(inode)->fnode_pos,
						mode);
					return;
				}
				rdev = huge_decode_dev(SPAD2CPU64_LV(&rd->dev));
			} else if (S_ISFIFO(mode) || likely(S_ISSOCK(mode))) {
			} else {
				spadfs_error(fs, TXFLAGS_EA_ERROR,
					"UNX extended attribute error on fnode %Lx/%x: file has invalid mode %06o",
					(unsigned long long)spadfnode(inode)->fnode_block,
					spadfnode(inode)->fnode_pos,
					mode);
				return;
			}
			init_special_inode(inode, mode, rdev);
		}
	}
	if (likely(fnode != NULL)) {
		if (likely(S_ISREG(mode))) {
			unsigned prealloc = SPAD2CPU32_LV(&ea->prealloc[
				    !CC_VALID(fs, &fnode->cc, &fnode->txc)]);
			if (unlikely(prealloc > inode->i_size)) {
				spadfs_error(fs, TXFLAGS_EA_ERROR,
					"UNX extended attribute error on fnode %Lx/%x: prealloc (%d) is larger than file size (%Ld)",
					(unsigned long long)spadfnode(inode)->fnode_block,
					spadfnode(inode)->fnode_pos,
					prealloc,
					(unsigned long long)inode->i_size);
				return;
			}
			inode->i_size -= prealloc;
			spadfnode(inode)->mmu_private = inode->i_size;
		} else {
			if (unlikely((SPAD2CPU32_LV(&ea->prealloc[0]) |
				      SPAD2CPU32_LV(&ea->prealloc[1])) != 0)) {
				spadfs_error(fs, TXFLAGS_EA_ERROR,
					"UNX extended attribute error on fnode %Lx/%x: fnode with mode %06o has non-zero prealloc",
					(unsigned long long)spadfnode(inode)->fnode_block,
					spadfnode(inode)->fnode_pos,
					mode);
				return;
			}
		}
	}
	inode->i_mode = mode;
	i_uid_write(inode, SPAD2CPU32_LV(&ea->uid));
	i_gid_write(inode, SPAD2CPU32_LV(&ea->gid));
}

int spadfs_get_fixed_fnode_pos(SPADFS *fs, struct fnode_block *fnode_block,
			       sector_t secno, unsigned *pos)
{
#define fx	((struct fixed_fnode_block *)fnode_block)
	*pos = FIXED_FNODE_BLOCK_FNODE1 - CC_VALID(fs, &fx->cc, &fx->txc) *
			(FIXED_FNODE_BLOCK_FNODE1 - FIXED_FNODE_BLOCK_FNODE0);
	if (unlikely(*(u64 *)((char *)fnode_block + *pos -
		     (FIXED_FNODE_BLOCK_FNODE0 - FIXED_FNODE_BLOCK_NLINK0)) ==
		     CPU2SPAD64_CONST(0))) {
		spadfs_error(fs, TXFLAGS_FS_ERROR,
			"fixed fnode %Lx has zero nlink",
			(unsigned long long)secno);
		return -EFSERROR;
	}
	return 0;
#undef fx
}

/*
 * Read an inode. The inode to read is determined by (already filled) spadfs_ino
 * entry. The sector and position of inode is determined by spadfs_ino_t_sec and
 * spadfs_ino_t_pos macros from spadfs_ino. If position is 0, it is fixed fnode
 * (i.e. hardlink was created before to it) --- in this case spadfs_ino_t_sec
 * points to fixed fnode block.
 * If the fnode is in directory, parent is inode number of that directory, if
 * the fnode is fixed, parent is "don't care".
 */

static int spadfs_read_inode(struct inode *inode, sector_t parent_block, unsigned parent_pos)
{
	int r;
	struct fnode *fnode;
	struct buffer_head *bh;
	unsigned ea_pos, ea_size;
	int cc_valid;
	loff_t other_i_size;
	SPADFS *fs = spadfs(inode->i_sb);
	unsigned pos = spadfs_ino_t_pos(spadfnode(inode)->spadfs_ino);
	struct fnode_block *fnode_block;

	spadfnode(inode)->fnode_block =
				spadfs_ino_t_sec(spadfnode(inode)->spadfs_ino);
	fnode_block = spadfs_read_fnode_block(fs,
		spadfnode(inode)->fnode_block,
		&bh,
		SRFB_FNODE + unlikely(is_pos_fixed(pos)) * (SRFB_FIXED_FNODE - SRFB_FNODE),
		"spadfs_read_inode");
	if (unlikely(IS_ERR(fnode_block))) {
		r = PTR_ERR(fnode_block);
		goto make_bad;
	}

	if (unlikely(is_pos_fixed(pos))) {
		parent_block = 0;
		parent_pos = 0;
		r = spadfs_get_fixed_fnode_pos(fs, fnode_block,
					       spadfnode(inode)->fnode_block,
					       &pos);
		if (unlikely(r))
			goto brelse_make_bad;
	}
	spadfnode(inode)->fnode_pos = pos;
	fnode = (struct fnode *)((char *)fnode_block + pos);

	ea_pos = FNODE_EA_POS(fnode->namelen);
	ea_size = (SPAD2CPU16_LV(&fnode->next) & FNODE_NEXT_SIZE) - ea_pos;
	if (unlikely(pos + ea_pos + ea_size > FNODE_BLOCK_SIZE) ||
	    unlikely(ea_size > FNODE_MAX_EA_SIZE)) {
		spadfs_error(fs, TXFLAGS_FS_ERROR,
			"invalid extended attributes on fnode %Lx/%x",
			(unsigned long long)spadfnode(inode)->fnode_block,
			spadfnode(inode)->fnode_pos);
		r = -EFSERROR;
		goto brelse_make_bad;
	}
	if (unlikely(fnode->flags & FNODE_FLAGS_HARDLINK)) {
		spadfs_error(fs, TXFLAGS_FS_ERROR,
			"attempting to read hardlink as fnode: %Lx/%x",
			(unsigned long long)spadfnode(inode)->fnode_block,
			spadfnode(inode)->fnode_pos);
		r = -EFSERROR;
		goto brelse_make_bad;
	}
	if (unlikely(r = spadfs_ea_resize(spadfnode(inode), ea_size)))
		goto brelse_make_bad;
	spadfnode(inode)->ea_size = ea_size;
	memcpy(spadfnode(inode)->ea, (char *)fnode + ea_pos, ea_size);
	inode->i_mode = fs->mode;
	i_uid_write(inode, fs->uid);
	i_gid_write(inode, fs->gid);
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,18,0)
	inode->i_atime = current_kernel_time();
#else
	ktime_get_coarse_real_ts64(&inode->i_atime);
#endif
	inode_set_ctime(inode, SPAD2CPU32_LV(&fnode->ctime), 0);
	inode->i_mtime.tv_sec = SPAD2CPU32_LV(&fnode->mtime);
	inode->i_mtime.tv_nsec = 0;
	inode->i_generation = 0;
	spadfs_set_parent_fnode(spadfnode(inode), parent_block, parent_pos);
	spadfnode(inode)->ea_unx = NULL;
	spadfnode(inode)->spadfs_nlink = 1;
	spadfnode(inode)->commit_sequence =
				CC_CURRENT(fs, &fnode->cc, &fnode->txc) ?
				fs->commit_sequence : 0;
	cc_valid = CC_VALID(fs, &fnode->cc, &fnode->txc);
	inode->i_size = SPAD2CPU64_LV(&fnode->size[!cc_valid]);
	other_i_size = SPAD2CPU64_LV(&fnode->size[cc_valid]);
	if (likely(!(fnode->flags & FNODE_FLAGS_DIR))) {
		unsigned i;
		if (unlikely(is_fnode_fixed(spadfnode(inode))))
			spadfnode(inode)->spadfs_nlink = SPAD2CPU64_LV(FIXED_FNODE_NLINK_PTR(fnode));

		inode->i_mode = (inode->i_mode & ~0111) | S_IFREG;
		spadfnode(inode)->mmu_private = inode->i_size;
		spadfnode(inode)->disk_size = spadfs_roundup_blocksize(fs, inode->i_size);
		spadfnode(inode)->crash_disk_size = spadfs_roundup_blocksize(fs, other_i_size);
		inode->i_blocks = spadfs_size_2_sectors(fs, spadfnode(inode)->disk_size);

		spadfnode(inode)->blk1 = MAKE_D_OFF(fnode->run10, fnode->run11);
		spadfnode(inode)->blk1_n = SPAD2CPU16_LV(&fnode->run1n);
		spadfnode(inode)->blk2 = MAKE_D_OFF(fnode->run20, fnode->run21);
		spadfnode(inode)->blk2_n = SPAD2CPU16_LV(&fnode->run2n);

		for (i = 0; i < spadfs_extent_cache_size; i++) {
			spadfnode(inode)->extent_cache[i].physical_sector = spadfnode(inode)->blk1;
			spadfnode(inode)->extent_cache[i].logical_sector = 0;
			spadfnode(inode)->extent_cache[i].n_sectors = spadfnode(inode)->blk1_n;
		}

		spadfnode(inode)->root = MAKE_D_OFF(fnode->anode0, fnode->anode1);
		inode->i_op = &spadfs_file_iops;
		inode->i_fop = &spadfs_file_fops;
		inode->i_data.a_ops = &spadfs_file_aops;
	} else {
		if (unlikely(!(parent_block | parent_pos)))
			spadfnode(inode)->stable_ino = SPADFS_INO_ROOT;
		inode->i_mode |= S_IFDIR;
		inode->i_blocks = inode->i_size >> 9;
		spadfnode(inode)->root = cc_valid ?
					MAKE_D_OFF(fnode->run10, fnode->run11) :
					MAKE_D_OFF(fnode->run20, fnode->run21);
		inode->i_op = &spadfs_dir_iops;
		inode->i_fop = &spadfs_dir_fops;
	}
	spadfs_get_ea_unx(inode, fnode);
/* See the comment about internal and external nlink counts at spadfs_set_nlink */
	spadfs_set_nlink(inode);
	spadfs_brelse(fs, bh);
	return 0;

brelse_make_bad:
	spadfs_brelse(fs, bh);
make_bad:
	make_bad_inode(inode);
	return r;
}

/* Get an inode from cache or read it from disk */

struct inode *spadfs_iget(struct super_block *s, spadfs_ino_t spadfs_ino,
			  sector_t parent_block, unsigned parent_pos)
{
	struct inode *inode = iget5_locked(s, spadfs_ino_t_2_ino_t(spadfs_ino),
			       spadfs_iget_test, spadfs_iget_init, &spadfs_ino);
	if (unlikely(!inode)) {
		printk(KERN_ERR "spadfs: unable to allocate inode\n");
		return ERR_PTR(-ENOMEM);
	}

	if (unlikely(inode->i_state & I_NEW)) {
		int r = spadfs_read_inode(inode, parent_block, parent_pos);
		unlock_new_inode(inode);
		if (unlikely(r)) {
			iput(inode);
			return ERR_PTR(r);
		}
	}

	return inode;
}

struct inode *spadfs_new_inode(struct inode *dir, umode_t mode, time_t ctime,
			       void *ea, unsigned ea_size,
			       sector_t fnode_block, unsigned fnode_pos,
			       sector_t dir_root, u64 stable_ino)
{
	SPADFS *fs = spadfnode(dir)->fs;
	spadfs_ino_t ino = make_spadfs_ino_t(fnode_block, fnode_pos);
	struct inode *inode;
	int r;

	inode = new_inode(dir->i_sb);
	if (unlikely(!inode)) {
		r = -ENOMEM;
		goto ret_r;
	}

	spadfs_iget_init(inode, &ino);
	i_uid_write(inode, 0);
	i_gid_write(inode, 0);
	inode->i_mode = mode;
	inode->i_blocks = 0;
	inode->i_generation = 0;
	inode_set_ctime(inode, ctime, 0);
	inode->i_mtime.tv_sec = ctime;
	inode->i_mtime.tv_nsec = 0;
	inode->i_atime.tv_sec = ctime;
	inode->i_atime.tv_nsec = 0;
	set_nlink(inode, 1);
	spadfnode(inode)->spadfs_nlink = 1;
	spadfnode(inode)->disk_size = 0;
	spadfnode(inode)->crash_disk_size = 0;
	spadfnode(inode)->commit_sequence = fs->commit_sequence;
	spadfnode(inode)->mmu_private = 0;
	spadfs_validate_stable_ino(&spadfnode(inode)->stable_ino, stable_ino);
	spadfnode(inode)->blk1 = 0;
	spadfnode(inode)->blk2 = 0;
	spadfnode(inode)->blk1_n = 0;
	spadfnode(inode)->blk2_n = 0;
	memset(&spadfnode(inode)->extent_cache, 0,
					sizeof spadfnode(inode)->extent_cache);
	spadfnode(inode)->root = dir_root;
	spadfnode(inode)->fnode_block = fnode_block;
	spadfnode(inode)->fnode_pos = fnode_pos;
	spadfs_set_parent_fnode(spadfnode(inode), spadfnode(dir)->fnode_block, spadfnode(dir)->fnode_pos);
	spadfnode(inode)->ea_unx = NULL;
	if (unlikely(r = spadfs_ea_resize(spadfnode(inode), ea_size)))
		goto drop_inode_ret_r;
	spadfnode(inode)->ea_size = ea_size;
	memcpy(spadfnode(inode)->ea, ea, ea_size);
	BUG_ON(ea_size & (FNODE_EA_ALIGN - 1));
	if (unlikely(S_ISDIR(mode))) {
		inode->i_op = &spadfs_dir_iops;
		inode->i_fop = &spadfs_dir_fops;
		inode->i_size = directory_size(fs) ? 512U << fs->sectors_per_disk_block_bits : 0;
	} else {
		inode->i_op = &spadfs_file_iops;
		inode->i_fop = &spadfs_file_fops;
		inode->i_data.a_ops = &spadfs_file_aops;
	}
	spadfs_get_ea_unx(inode, NULL);

#ifdef SPADFS_QUOTA
	dquot_initialize(inode);
	r = dquot_alloc_inode(inode);
	if (unlikely(r))
		goto drop_inode_ret_r;
#endif

	if (unlikely(S_ISDIR(mode)) &&
	    likely(directory_size(fs))) {
#ifdef SPADFS_QUOTA
		r = dquot_alloc_space_nodirty(inode, inode->i_size);
		if (unlikely(r))
			goto free_iquota_drop_inode_ret_r;
#else
		inode_add_bytes(inode, inode->i_size);
#endif
	}

	__insert_inode_hash(inode, spadfs_ino_t_2_ino_t(spadfnode(inode)->spadfs_ino));
	return inode;

#ifdef SPADFS_QUOTA
free_iquota_drop_inode_ret_r:
	dquot_free_inode(inode);
#endif

drop_inode_ret_r:
#ifdef SPADFS_QUOTA
	dquot_drop(inode);
#endif
	inode->i_flags |= S_NOQUOTA;
	clear_nlink(inode);
	iput(inode);

ret_r:
	return ERR_PTR(r);
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,36)

void spadfs_delete_inode(struct inode *i)
{
	sync_lock_decl
	SPADFS *fs;

	BUG_ON(spadfnode(i)->res.len);
	BUG_ON(!list_empty(&spadfnode(i)->clear_entry));

	if (unlikely(S_ISDIR(i->i_mode))) goto clr;
	truncate_inode_pages(&i->i_data, 0);
	fs = spadfnode(i)->fs;
	down_read_sync_lock(fs);
	/* no one can modify this inode anyway */
	/*mutex_lock(&spadfnode(i)->file_lock);*/
	spadfs_delete_file_content(spadfnode(i));
	/*mutex_unlock(&spadfnode(i)->file_lock);*/
	up_read_sync_lock(fs);
clr:
	clear_inode(i);
}

#else

void spadfs_evict_inode(struct inode *i)
{
	sync_lock_decl
	SPADFS *fs;
	int want_delete = 0;

	BUG_ON(spadfnode(i)->res.len);
	BUG_ON(!list_empty(&spadfnode(i)->clear_entry));

	if (!i->i_nlink && !is_bad_inode(i)) {
		want_delete = 1;
#ifdef SPADFS_QUOTA
		dquot_initialize(i);
		dquot_free_inode(i);
#endif
	}

	truncate_inode_pages(&i->i_data, 0);

	if (!want_delete || unlikely(S_ISDIR(i->i_mode)))
		goto end;

	fs = spadfnode(i)->fs;
	down_read_sync_lock(fs);
	/* no one can modify this inode anyway */
	/*mutex_lock(&spadfnode(i)->file_lock);*/
	spadfs_delete_file_content(spadfnode(i));
	/*mutex_unlock(&spadfnode(i)->file_lock);*/

	up_read_sync_lock(fs);

end:
#ifdef SPADFS_QUOTA
	dquot_drop(i);
#endif
#if LINUX_VERSION_CODE < KERNEL_VERSION(3,5,0)
	end_writeback(i);
#else
	clear_inode(i);
#endif
}

#endif

u64 spadfs_get_user_inode_number(struct inode *inode)
{
	u64 result;

	/*
	 * Kernel returns -EOVERFLOW on stat32 if ino >= 2^32.
	 * This would cause random program fails.
	 */

	if (likely(spadfnode(inode)->stable_ino != 0))
		result = spadfnode(inode)->stable_ino;
	else
		result = spadfs_ino_t_2_ino64_t(spadfnode(inode)->spadfs_ino);

	return spadfs_expose_inode_number(spadfnode(inode)->fs, result);
}

/* Initial UNX attribute */

void spadfs_get_initial_attributes(struct inode *dir, umode_t *mode, uid_t *uid, gid_t *gid)
{
	*uid = get_current_fsuid();
	*gid = get_current_fsgid();
	if (unlikely(dir->i_mode & S_ISGID)) {
		*gid = i_gid_read(dir);
		if (unlikely(S_ISDIR(*mode))) {
			*mode |= S_ISGID;
		}
	}
}

void spadfs_init_unx_attribute(struct inode *dir, struct ea_unx *ea,
			       umode_t mode, u64 stable_ino)
{
	uid_t uid;
	gid_t gid;

	CPU2SPAD32_LV(&ea->magic, EA_UNX_MAGIC);
	CPU2SPAD16_LV(&ea->flags, 0);
	CPU2SPAD32_LV(&ea->prealloc[0], 0);
	CPU2SPAD32_LV(&ea->prealloc[1], 0);
	CPU2SPAD64_LV(&ea->ino, stable_ino);

	spadfs_get_initial_attributes(dir, &mode, &uid, &gid);

	CPU2SPAD32_LV(&ea->uid, uid);
	CPU2SPAD32_LV(&ea->gid, gid);
	CPU2SPAD16_LV(&ea->mode, mode);
}

static noinline int spadfs_make_unx_attribute(struct dentry *dentry)
{
#define new_ea_size	FNODE_EA_DO_ALIGN(sizeof(struct ea_unx) - 8)
	int r;
	SPADFNODE *f = spadfnode(dentry->d_inode);
	SPADFS *fs = f->fs;
	u8 *ea;
	unsigned ea_size;
	struct ea_unx *ea_unx;
	/*sync_lock_decl*/

	ea = kmalloc(FNODE_MAX_EA_SIZE, GFP_NOIO);
	if (unlikely(!ea))
		return -ENOMEM;

	down_write_sync_lock(fs);

	if (unlikely(f->ea_unx != NULL)) {
		r = 0;
		goto unlock_ret_r;
	}

	ea_size = f->ea_size;
	if (unlikely(ea_size + new_ea_size > FNODE_MAX_EA_SIZE)) {
		r = -EOVERFLOW;
		goto unlock_ret_r;
	}

	memcpy(ea, f->ea, ea_size);
	ea_unx = (struct ea_unx *)(ea + ea_size);
	CPU2SPAD32_LV(&ea_unx->magic, EA_UNX_MAGIC_OLD);
	CPU2SPAD16_LV(&ea_unx->flags, 0);
	CPU2SPAD32_LV(&ea_unx->prealloc[0], 0);
	CPU2SPAD32_LV(&ea_unx->prealloc[1], 0);
	CPU2SPAD32_LV(&ea_unx->uid, i_uid_read(&f->vfs_inode));
	CPU2SPAD32_LV(&ea_unx->gid, i_gid_read(&f->vfs_inode));
	CPU2SPAD16_LV(&ea_unx->mode, f->vfs_inode.i_mode);
	ea_size += new_ea_size;

	r = spadfs_refile_fnode(spadfnode(dentry->d_parent->d_inode), &dentry->d_name, f, ea, ea_size);

unlock_ret_r:
	up_write_sync_lock(fs);
	kfree(ea);

	return r;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(5,12,0)
int spadfs_setattr_common(struct dentry *dentry, struct iattr *iattr)
#else
int spadfs_setattr_common(struct mnt_idmap *ns, struct dentry *dentry, struct iattr *iattr)
#endif
{
	struct inode *inode = dentry->d_inode;
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,9,0) && !TEST_STABLE_BRANCH(3,2,84) && !TEST_STABLE_BRANCH(3,16,39) && !TEST_STABLE_BRANCH(4,1,37)
	int r = inode_change_ok(inode, iattr);
#elif LINUX_VERSION_CODE < KERNEL_VERSION(5,12,0)
	int r = setattr_prepare(dentry, iattr);
#else
	int r = setattr_prepare(ns, dentry, iattr);
#endif
	if (unlikely(r))
		return r;

	if (likely(!S_ISLNK(inode->i_mode))) {
		if (unlikely(!spadfnode(inode)->ea_unx) &&
		    iattr->ia_valid & (ATTR_MODE | ATTR_UID | ATTR_GID)) {
			r = spadfs_make_unx_attribute(dentry);
			if (unlikely(r))
				return r;
		}
	}

#ifdef SPADFS_QUOTA
#if LINUX_VERSION_CODE < KERNEL_VERSION(6,0,0)
	if (is_quota_modification(inode, iattr))
		dquot_initialize(inode);
	if ((iattr->ia_valid & ATTR_UID && !uid_eq(iattr->ia_uid, inode->i_uid)) ||
	    (iattr->ia_valid & ATTR_GID && !gid_eq(iattr->ia_gid, inode->i_gid))) {
		r = dquot_transfer(inode, iattr);
		if (unlikely(r))
			return r;
	}
#else
	if (is_quota_modification(ns, inode, iattr))
		dquot_initialize(inode);
	if (i_uid_needs_update(ns, iattr, inode) ||
	    i_gid_needs_update(ns, iattr, inode)) {
		r = dquot_transfer(ns, inode, iattr);
		if (unlikely(r))
			return r;
	}
#endif
#endif
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,36)
	return inode_setattr(inode, iattr);
#else
	if (iattr->ia_valid & ATTR_SIZE) {
#if LINUX_VERSION_CODE < KERNEL_VERSION(3,8,0)
		r = vmtruncate(inode, iattr->ia_size);
		if (unlikely(r))
			return r;
#else
		truncate_setsize(inode, iattr->ia_size);
		spadfs_truncate(inode);
#endif
	}
#if LINUX_VERSION_CODE < KERNEL_VERSION(5,12,0)
	setattr_copy(inode, iattr);
#else
	setattr_copy(ns, inode, iattr);
#endif
	return 0;
#endif
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(4,11,0)
int spadfs_getattr(struct vfsmount *mnt, struct dentry *dentry, struct kstat *stat)
#elif LINUX_VERSION_CODE < KERNEL_VERSION(5,12,0)
int spadfs_getattr(const struct path *path, struct kstat *stat, u32 request_mask, unsigned query_flags)
#else
int spadfs_getattr(struct mnt_idmap *ns, const struct path *path, struct kstat *stat, u32 request_mask, unsigned query_flags)
#endif
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,11,0)
	struct inode *inode = dentry->d_inode;
	generic_fillattr(inode, stat);
#elif LINUX_VERSION_CODE < KERNEL_VERSION(5,12,0)
	struct inode *inode = d_inode(path->dentry);
	generic_fillattr(inode, stat);
#elif LINUX_VERSION_CODE < KERNEL_VERSION(6,6,0)
	struct inode *inode = d_inode(path->dentry);
	generic_fillattr(ns, inode, stat);
#else
	struct inode *inode = d_inode(path->dentry);
	generic_fillattr(ns, request_mask, inode, stat);
#endif
	stat->blksize = spadfnode(inode)->fs->xfer_size;
	stat->ino = spadfs_get_user_inode_number(inode);
	return 0;
}

void spadfs_update_ea(struct inode *inode)
{
	if (unlikely(!spadfnode(inode)->ea_unx))
		return;
	CPU2SPAD16_LV(&spadfnode(inode)->ea_unx->mode, inode->i_mode);
	CPU2SPAD32_LV(&spadfnode(inode)->ea_unx->uid, i_uid_read(inode));
	CPU2SPAD32_LV(&spadfnode(inode)->ea_unx->gid, i_gid_read(inode));
}

static unsigned spadfs_parent_hash(sector_t sec, unsigned pos)
{
	unsigned n;
	n = ((unsigned long)sec / SPADFS_INODE_HASH_SIZE) ^
		(unsigned long)sec ^
		((unsigned long)pos * (SPADFS_INODE_HASH_SIZE / 512));
	return n & (SPADFS_INODE_HASH_SIZE - 1);
}

static void spadfs_set_parent_fnode_unlocked(SPADFNODE *f, sector_t sec, unsigned pos)
{
	SPADFS *fs = f->fs;
	if (f->parent_fnode_block) {
		hlist_del(&f->inode_list);
	}
	f->parent_fnode_block = sec;
	f->parent_fnode_pos = pos;
	if (sec) {
		hlist_add_head(&f->inode_list, &fs->inode_list[spadfs_parent_hash(sec, pos)]);
	}
}

void spadfs_set_parent_fnode(SPADFNODE *f, sector_t sec, unsigned pos)
{
	mutex_lock(&f->fs->inode_list_lock);
	spadfs_set_parent_fnode_unlocked(f, sec, pos);
	mutex_unlock(&f->fs->inode_list_lock);
}

void spadfs_move_parent_dir_ptr(SPADFS *fs, sector_t src_sec, unsigned src_pos,
				sector_t dst_sec, unsigned dst_pos)
{
	SPADFNODE *f;
	unsigned h;
	unsigned char s = 0;
#if LINUX_VERSION_CODE < KERNEL_VERSION(3,9,0)
	struct hlist_node *hn;
#endif
	struct hlist_node *n;

	mutex_lock(&fs->inode_list_lock);
	h = spadfs_parent_hash(src_sec, src_pos);
#if LINUX_VERSION_CODE < KERNEL_VERSION(3,9,0)
	hlist_for_each_entry_safe(f, hn, n, &fs->inode_list[h], inode_list)
#else
	hlist_for_each_entry_safe(f, n, &fs->inode_list[h], inode_list)
#endif
	{
		if (f->parent_fnode_block == src_sec &&
		    f->parent_fnode_pos == src_pos) {
			spadfs_set_parent_fnode_unlocked(f, dst_sec, dst_pos);
		}
		if (!++s)
			spadfs_cond_resched();
	}
	mutex_unlock(&fs->inode_list_lock);
}

void spadfs_move_fnode_ptr(SPADFS *fs, sector_t src_sec, unsigned src_pos,
			   sector_t dst_sec, unsigned dst_pos, int is_dir)
{
	spadfs_ino_t spadfs_ino = make_spadfs_ino_t(src_sec, src_pos);
	struct inode *inode = ilookup5(fs->s, spadfs_ino_t_2_ino_t(spadfs_ino),
				       spadfs_iget_test, &spadfs_ino);
	if (unlikely(inode != NULL)) {
		spadfnode(inode)->fnode_block = dst_sec;
		spadfnode(inode)->fnode_pos = dst_pos;
		spadfnode(inode)->spadfs_ino = make_spadfs_ino_t(dst_sec, dst_pos);
		remove_inode_hash(inode);
		__insert_inode_hash(inode, spadfs_ino_t_2_ino_t(spadfnode(inode)->spadfs_ino));
		iput(inode);
	}
	if (unlikely(is_dir))
		spadfs_move_parent_dir_ptr(fs, src_sec, src_pos,
						dst_sec, dst_pos);
}
