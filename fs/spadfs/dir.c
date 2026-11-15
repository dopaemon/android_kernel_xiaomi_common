#include "spadfs.h"
#include "dir.h"

void really_do_fnode_block_checksum(struct fnode_block *fnode_block)
{
	fnode_block->flags |= FNODE_BLOCK_CHECKSUM_VALID;
	fnode_block->checksum ^= CHECKSUM_BASE ^
				 __byte_sum(fnode_block, FNODE_BLOCK_SIZE);
}

static int spadfs_access_dnode(SPADFS *fs, sector_t secno, unsigned version,
			       unsigned i, sector_t *result, int wr)
{
	struct buffer_head *bh;
	char *data;
	i = (i * DNODE_PAGE_ENTRY_SIZE) + DNODE_ENTRY_OFFSET;
	if (version)
		i += fs->dnode_data_size;

	data = spadfs_read_sector(fs, secno + (i >> 9), &bh, 0,
				  "spadfs_access_dnode 1");
	if (unlikely(IS_ERR(data)))
		return PTR_ERR(data);

	data += i & 511;
	if (likely((i & 511) <= 512 - DNODE_PAGE_ENTRY_SIZE)) {
		if (likely(!wr)) {
			*result = MAKE_D_OFF3(
				((struct dnode_page_entry *)data)->b0,
				((struct dnode_page_entry *)data)->b1,
				((struct dnode_page_entry *)data)->b2);
		} else {
			((struct dnode_page_entry *)data)->b0 =
							MAKE_PART_30(*result);
			((struct dnode_page_entry *)data)->b1 =
							MAKE_PART_31(*result);
			((struct dnode_page_entry *)data)->b2 =
							MAKE_PART_32(*result);
		}
	} else {
		struct dnode_page_entry dp;
		if (likely(!wr))
			memcpy(&dp, data, 512 - (i & 511));
		else {
			dp.b0 = MAKE_PART_30(*result);
			dp.b1 = MAKE_PART_31(*result);
			dp.b2 = MAKE_PART_32(*result);
			memcpy(data, &dp, 512 - (i & 511));
			mark_buffer_dirty(bh);
		}
		spadfs_brelse(fs, bh);

		data = spadfs_read_sector(fs, secno + (i >> 9) + 1, &bh, 0,
					  "spadfs_access_dnode 2");
		if (unlikely(IS_ERR(data)))
			return PTR_ERR(data);

		if (likely(!wr)) {
			memcpy((char *)&dp + (512 - (i & 511)), data,
			       DNODE_PAGE_ENTRY_SIZE - (512 - (i & 511)));
			*result = MAKE_D_OFF3(dp.b0, dp.b1, dp.b2);
		} else
			memcpy(data, (char *)&dp + (512 - (i & 511)),
			       DNODE_PAGE_ENTRY_SIZE - (512 - (i & 511)));
	}

	if (wr)
		mark_buffer_dirty(bh);

	spadfs_brelse(fs, bh);
	return 0;
}

static int spadfs_read_dnode(SPADFS *fs, sector_t secno, unsigned version,
			     unsigned i, sector_t *result)
{
	return spadfs_access_dnode(fs, secno, version, i, result, 0);
}

static int spadfs_write_dnode(SPADFS *fs, sector_t secno, unsigned version,
			      unsigned i, sector_t result)
{
	return spadfs_access_dnode(fs, secno, version, i, &result, 1);
}

static int dnode_version(SPADFS *fs, struct dnode_page *d)
{
	return !CC_VALID(fs, &d->cc, &d->txc);
}

static int spadfs_begin_modify_dnode(SPADFS *fs, sector_t dno)
{
	int version;
	struct buffer_head *bh;
	struct dnode_page *d;

	d = (struct dnode_page *)spadfs_read_fnode_block(fs, dno, &bh,
				SRFB_DNODE, "spadfs_begin_modify_dnode 1");
	if (unlikely(IS_ERR(d)))
		return PTR_ERR(d);

	version = dnode_version(fs, d);

	if (CC_CURRENT(fs, &d->cc, &d->txc)) {
		d->flags[version] &= ~DNODE_CHECKSUM_VALID;
		mark_buffer_dirty(bh);
		spadfs_brelse(fs, bh);
		return version;
	} else {
		unsigned from, to;
		char *datafrom, *datato;
		struct buffer_head *bhfrom, *bhto;
		unsigned remaining, len;

		start_atomic_buffer_modify(fs, bh);
		d->flags[version ^ 1] &= ~DNODE_CHECKSUM_VALID;
		CC_SET_CURRENT(fs, &d->cc, &d->txc);
		end_atomic_buffer_modify(fs, bh);
		spadfs_brelse(fs, bh);

		from = DNODE_ENTRY_OFFSET + version * fs->dnode_data_size;
		to = DNODE_ENTRY_OFFSET + (version ^ 1) * fs->dnode_data_size;
		remaining = fs->dnode_data_size;
		datafrom = spadfs_read_sector(fs, dno + (from >> 9), &bhfrom, 0,
					      "spadfs_begin_modify_dnode 2");
		if (unlikely(IS_ERR(datafrom)))
			return PTR_ERR(datafrom);

		datafrom += from & 511;
reread_to:
		datato = spadfs_read_sector(fs, dno + (to >> 9), &bhto, 0,
					    "spadfs_begin_modify_dnode 3");
		if (unlikely(IS_ERR(datato))) {
			spadfs_brelse(fs, bhfrom);
			return PTR_ERR(datato);
		}
		datato += to & 511;

copy_again:
		len = 512 - (from & 511);
		if (512 - (to & 511) < len)
			len = 512 - (to & 511);
		if (unlikely(remaining < len))
			len = remaining;
		memcpy(datato, datafrom, len);
		datafrom += len;
		datato += len;
		from += len;
		to += len;
		remaining -= len;
		if (remaining) {
			if (!(from & 511)) {
				spadfs_brelse(fs, bhfrom);
				datafrom = spadfs_read_sector(fs,
							      dno + (from >> 9),
							      &bhfrom, 0,
						"spadfs_begin_modify_dnode 4");
				if (unlikely(IS_ERR(datafrom))) {
					spadfs_brelse(fs, bhto);
					return PTR_ERR(datafrom);
				}
				datafrom += from & 511;
			}
			if (!(to & 511)) {
				mark_buffer_dirty(bhto);
				spadfs_brelse(fs, bhto);
				goto reread_to;
			}
			if (unlikely(from & 511))
				panic("spadfs: unaligned from pointer: %d, %d",
					from, to);
			goto copy_again;
		}
		spadfs_brelse(fs, bhfrom);
		mark_buffer_dirty(bhto);
		spadfs_brelse(fs, bhto);
		return version ^ 1;
	}
}

int spadfs_alloc_fixed_fnode_block(SPADFNODE *fn, sector_t hint, unsigned size,
				   u16 hint_small, u16 hint_big,
				   sector_t *result)
{
	SPADFS *fs = fn->fs;
	int r;
	struct fixed_fnode_block *fx;
	struct alloc al;
	struct buffer_head *bh;

	al.sector = hint;
	al.n_sectors = 1U << fs->sectors_per_disk_block_bits;
	al.extra_sectors = 0;
	al.flags = ALLOC_METADATA;
	al.reservation = NULL;
	r = spadfs_alloc_blocks(fs, &al);
	if (unlikely(r))
		return r;
	*result = al.sector;

	fx = spadfs_get_new_sector(fs, al.sector, &bh, "spadfs_alloc_fixed_fnode_block");
	if (unlikely(IS_ERR(fx)))
		return PTR_ERR(fx);
	memset(fx, 0, 512U << fs->sectors_per_buffer_bits);
	CPU2SPAD32_LV(&fx->magic, spadfs_magic(fs, al.sector, FIXED_FNODE_BLOCK_MAGIC));
	fx->flags = 0;
	/*
	 * cc/txc doesn't matter because this block won't be used in case of
	 * crash. Set to current (cc, txc) to prevent copy on next update.
	 */
	CPU2SPAD16_LV(&fx->cc, fs->cc);
	CPU2SPAD32_LV(&fx->txc, fs->txc);
	CPU2SPAD16_LV(&fx->hint_small, hint_small);
	CPU2SPAD16_LV(&fx->hint_large, hint_big);
	CPU2SPAD64_LV(&fx->nlink0, 1);
#define fnode	((struct fnode *)fx->fnode0)
	CPU2SPAD16_LV(&fnode->next, size);
#undef fnode
	do_fnode_block_checksum(fs, (struct fnode_block *)fx);

	mark_buffer_dirty(bh);
	spadfs_brelse(fs, bh);

	return 0;
}

static int spadfs_account_directory_blocks(SPADFNODE *fn, unsigned n_sectors)
{
	int r;
	loff_t old_i_size, new_i_size;

	if (likely(directory_size(fn->fs))) {

		old_i_size = inode(fn)->i_size;
		new_i_size = old_i_size + n_sectors * 512;

		if (unlikely(new_i_size < old_i_size)) {
			spadfs_error(fn->fs, TXFLAGS_FS_ERROR,
				     "directory size overflow, size %Lx, adding %x",
				     (unsigned long long)old_i_size,
				     n_sectors * 512);
			new_i_size = 0;
		}

#ifdef SPADFS_QUOTA
		r = dquot_alloc_space_nodirty(inode(fn),
					      new_i_size - old_i_size);
		if (unlikely(r))
			goto ret_r;
#else
		inode_add_bytes(inode(fn), new_i_size - old_i_size);
#endif

		i_size_write(inode(fn), new_i_size);

		r = spadfs_write_directory(fn);
		if (unlikely(r)) {
			i_size_write(inode(fn), old_i_size);
			goto unaccount_ret_r;
		}
	}
	return 0;

unaccount_ret_r:
#ifdef SPADFS_QUOTA
	dquot_free_space_nodirty(inode(fn), new_i_size - old_i_size);
#else
	inode_sub_bytes(inode(fn), new_i_size - old_i_size);
#endif

#ifdef SPADFS_QUOTA
ret_r:
#endif
	return r;
}

static int spadfs_free_directory_blocks(SPADFNODE *fn, sector_t start,
					unsigned n_sectors)
{
	int r;

	if (likely(directory_size(fn->fs))) {

		loff_t old_i_size, new_i_size;

		old_i_size = inode(fn)->i_size;

		if (likely(old_i_size >= n_sectors * 512))
			new_i_size = old_i_size - n_sectors * 512;
		else {
			spadfs_error(fn->fs, TXFLAGS_FS_ERROR,
				     "directory size miscounted, size %Lx, freeing %x",
				     (unsigned long long)old_i_size,
				     n_sectors * 512);

			new_i_size = 0;
		}


#ifdef SPADFS_QUOTA
		dquot_free_space_nodirty(inode(fn), old_i_size - new_i_size);
#else
		inode_sub_bytes(inode(fn), old_i_size - new_i_size);
#endif

		i_size_write(inode(fn), new_i_size);

		r = spadfs_write_directory(fn);
		if (unlikely(r))
			return r;
	}
	return spadfs_free_blocks_metadata(fn->fs, start, n_sectors);
}

int spadfs_alloc_leaf_page(SPADFNODE *fn, sector_t hint, unsigned n_sectors,
			   sector_t prev, sector_t *result, int noaccount)
{
	SPADFS *fs = fn->fs;
	int r;
	unsigned i, j;
	struct alloc al;

	al.sector = hint;
	al.n_sectors = n_sectors;
	al.extra_sectors = 0;
	al.flags = ALLOC_METADATA;
	al.reservation = NULL;
	r = spadfs_alloc_blocks(fs, &al);
	if (unlikely(r))
		return r;
	*result = al.sector;

	for (i = 0; i < n_sectors; i += 1U << fs->sectors_per_buffer_bits) {
		struct buffer_head *bh;
		struct fnode_block *b = spadfs_get_new_sector(fs, al.sector + i, &bh, "spadfs_alloc_leaf_page");
		if (unlikely(IS_ERR(b))) {
			r = PTR_ERR(b);
			goto free_return_r;
		}

		for (j = 0; j < 1U << fs->sectors_per_buffer_bits;
		     j++, b = (struct fnode_block *)((char *)b + FNODE_BLOCK_SIZE)) {
			struct fnode *fn;
			memset(b, 0, FNODE_BLOCK_SIZE);
			if (!i && !j) {
				b->prev0 = MAKE_PART_0(prev);
				b->prev1 = MAKE_PART_1(prev);
			}
			CPU2SPAD32_LV(&b->magic, spadfs_magic(fs, al.sector + (i + j), FNODE_BLOCK_MAGIC));
			b->flags = (FNODE_BLOCK_FIRST * (!i && !j)) |
				   (FNODE_BLOCK_LAST * (i + j == n_sectors - 1));
			CPU2SPAD32_LV(&b->txc, TXC_INVALID(0));
			CPU2SPAD16_LV(&b->cc, 0);
			fn = b->fnodes;
			CPU2SPAD16_LV(&fn->next, (FNODE_MAX_SIZE & FNODE_NEXT_SIZE) | FNODE_NEXT_FREE);
			CPU2SPAD16_LV(&fn->cc, 0);
			CPU2SPAD32_LV(&fn->txc, 0);
			do_fnode_block_checksum(fs, b);
		}
		mark_buffer_dirty(bh);
		spadfs_brelse(fs, bh);
	}
	if (!noaccount) {
		r = spadfs_account_directory_blocks(fn, n_sectors);
		if (unlikely(r))
			goto free_return_r;
	}
	return 0;

free_return_r:
	spadfs_free_blocks_metadata(fs, al.sector, n_sectors);
	return r;
}

static int spadfs_alloc_dnode_page(SPADFNODE *fn, sector_t hint,
				   sector_t *result, sector_t parent,
				   sector_t ptr_init)
{
	SPADFS *fs = fn->fs;
	int r;
	unsigned i;
	struct alloc al;

	*result = 0;	/* avoid warning */

	al.sector = hint;
	al.n_sectors = fs->dnode_page_sectors;
	al.extra_sectors = 0;
	al.flags = ALLOC_METADATA;
	al.reservation = NULL;
	r = spadfs_alloc_blocks(fs, &al);
	if (unlikely(r))
		return r;
	*result = al.sector;

	for (i = 0; i < fs->dnode_page_sectors; i += 1U << fs->sectors_per_buffer_bits) {
		struct buffer_head *bh;
		struct dnode_page *d = spadfs_get_new_sector(fs, al.sector + i, &bh, "spadfs_alloc_dnode_page");
		if (unlikely(IS_ERR(d))) {
			r = PTR_ERR(d);
			goto free_return_r;
		}

		memset(d, 0, 512U << fs->sectors_per_buffer_bits);
		if (!i) {
			CPU2SPAD32_LV(&d->magic, spadfs_magic(fs, al.sector, DNODE_PAGE_MAGIC));
			d->gflags = (fs->sectors_per_page_bits - 1) << DNODE_GFLAGS_PAGE_SIZE_BITS_MINUS_1_SHIFT;
			CPU2SPAD64_LV(&d->up_dnode, parent);
			CPU2SPAD32_LV(&d->txc, fs->txc);
			CPU2SPAD16_LV(&d->cc, fs->cc);
		}
		mark_buffer_dirty(bh);
		spadfs_brelse(fs, bh);
	}
	for (i = 0; i < 1U << fs->dnode_hash_bits; i++) {
		r = spadfs_write_dnode(fs, al.sector, 0, i, ptr_init);
		if (unlikely(r))
			goto free_return_r;
	}

	r = spadfs_account_directory_blocks(fn, fs->dnode_page_sectors);
	if (unlikely(r))
		goto free_return_r;

	return 0;

free_return_r:
	spadfs_free_blocks_metadata(fs, al.sector, fs->dnode_page_sectors);
	return r;
}

struct fnode_block *spadfs_find_hash_block(SPADFNODE *f, hash_t hash,
					   struct buffer_head **bhp,
					   sector_t *secno, hash_t *next_hash)
{
	sector_t old_secno;
	unsigned hash_bits;
	unsigned hpos;
	unsigned version;
	struct fnode_block *fnode_block;
	int r;
	sector_t c[2];
	c[1] = 0;
	*secno = f->root;
	hash_bits = 0;

	if (unlikely(next_hash != NULL))
		*next_hash = 0;
again:
	if (unlikely(spadfs_stop_cycles(f->fs, *secno, &c,
					"spadfs_find_hash_block")))
		return ERR_PTR(-EFSERROR);

	fnode_block = spadfs_read_fnode_block(f->fs, *secno, bhp,
					      SRFB_FNODE | SRFB_DNODE,
					      "spadfs_find_hash_block");
	if (unlikely(IS_ERR(fnode_block)))
		return fnode_block;

	if (likely(SPAD2CPU32_LV(&fnode_block->magic) ==
		   spadfs_magic(f->fs, *secno, FNODE_BLOCK_MAGIC)))
		return fnode_block;

	version = dnode_version(f->fs, (struct dnode_page *)fnode_block);
	spadfs_brelse(f->fs, *bhp);

	if (unlikely(hash_bits >= SPADFS_HASH_BITS)) {
		spadfs_error(f->fs, TXFLAGS_FS_ERROR,
			"too deep dnode tree, started at %Lx, looked up hash %08Lx",
			(unsigned long long)f->root,
			(unsigned long long)hash);
		return ERR_PTR(-EFSERROR);
	}
	hpos = (hash >> hash_bits) & ((1 << f->fs->dnode_hash_bits) - 1);
	old_secno = *secno;
	r = spadfs_read_dnode(f->fs, *secno, version, hpos, secno);
	if (unlikely(r))
		return ERR_PTR(r);
	if (unlikely(next_hash != NULL)) {
		int i;
		for (i = f->fs->dnode_hash_bits - 1; i >= 0; i--)
			if (!(hpos & (1 << i)) &&
			    likely(i + hash_bits < SPADFS_HASH_BITS)) {
				sector_t test_secno;
				r = spadfs_read_dnode(f->fs, old_secno, version,
					(hpos & ((1 << i) - 1)) | (1 << i),
					&test_secno);
				if (unlikely(r))
					return ERR_PTR(r);
				if (unlikely(!test_secno) ||
				    test_secno != *secno) {
#if 0
					if (test_secno != 0) {
						spadfs_prefetch_sector(f->fs, test_secno, f->fs->metadata_prefetch, "spadfs_find_hash_block");
					}
#endif
					*next_hash =
						(hash & (((hash_t)1 <<
							(hash_bits + i)) - 1)) |
						(hash_t)1 << (hash_bits + i);
					break;
				}
			}
	}
	hash_bits += f->fs->dnode_hash_bits;
	if (unlikely(!*secno))
		return NULL;
	goto again;
}

/*
 * Directory entry sector/position is returned in ino.
 * If for_delete is set, ino is input argument too (pointer to fixed fnode
 *	whose entry to delete) --- the reason for it is that during rename
 *	there are temporarily two entries with the same name in the directory.
 */

int spadfs_lookup_ino(SPADFNODE *f, struct qstr *qstr, spadfs_ino_t *ino,
		      int for_delete)
{
	struct buffer_head *bh;
	struct fnode_block *fnode_block;
	struct fnode *fnode;
	sector_t secno;
	unsigned size, namelen;
	sector_t c[2];
	c[1] = 0;

	fnode_block = spadfs_find_hash_block(f,
		name_len_hash((const char *)qstr->name, qstr->len), &bh, &secno, NULL);
	if (unlikely(!fnode_block))
		return 1;

next_fnode_block:
	if (unlikely(IS_ERR(fnode_block)))
		return PTR_ERR(fnode_block);
	fnode = fnode_block->fnodes;

next_fnode:
	VALIDATE_FNODE(f->fs, fnode_block, fnode, size, namelen,
		       ok, skip, bad_fnode);

ok:
	if (!spadfs_compare_names(f->fs, (const char *)qstr->name, qstr->len,
				  FNODE_NAME(fnode), namelen)) {
		if (unlikely(for_delete)) {
			if (!(fnode->flags & FNODE_FLAGS_HARDLINK))
				goto skip;
			if (unlikely(*ino != make_fixed_spadfs_ino_t(
			    MAKE_D_OFF(fnode->anode0, fnode->anode1))))
				goto skip;
			goto set_ino_to_entry;
		}
		if (likely(!(fnode->flags & FNODE_FLAGS_HARDLINK)))
set_ino_to_entry:
			*ino = make_spadfs_ino_t(secno,
				(char *)fnode - (char *)fnode_block);
		else
			*ino = make_fixed_spadfs_ino_t(
				MAKE_D_OFF(fnode->anode0, fnode->anode1));
		spadfs_brelse(f->fs, bh);
		return 0;
	}

skip:
	fnode = (struct fnode *)((char *)fnode + size);
	if (likely(((unsigned long)fnode & (FNODE_BLOCK_SIZE - 1)) != 0))
		goto next_fnode;

	if (!(fnode_block->flags & FNODE_BLOCK_LAST)) {
		spadfs_brelse(f->fs, bh);
		secno++;
read_next_fnode_block:
		if (unlikely(spadfs_stop_cycles(f->fs, secno, &c,
						"spadfs_lookup_ino")))
			return -EFSERROR;
		fnode_block = spadfs_read_fnode_block(f->fs, secno, &bh,
						      SRFB_FNODE,
						      "spadfs_lookup_ino");
		goto next_fnode_block;
	}

	if (unlikely(CC_VALID(f->fs, &fnode_block->cc, &fnode_block->txc))) {
		secno = MAKE_D_OFF(fnode_block->next0, fnode_block->next1);
		spadfs_brelse(f->fs, bh);
		goto read_next_fnode_block;
	}
	spadfs_brelse(f->fs, bh);
	return 1;

bad_fnode:
	spadfs_brelse(f->fs, bh);
	spadfs_error(f->fs, TXFLAGS_FS_ERROR,
		"lookup: bad fnode on block %Lx",
		(unsigned long long)secno);
	return -EFSERROR;
}

int spadfs_check_directory_empty(SPADFNODE *f)
{
	struct fnode_block *fnode_block;
	struct buffer_head *bh;
	sector_t secno;
	struct fnode *fnode;
	unsigned size, namelen;
	hash_t hash = 0;
	hash_t next_hash;
	sector_t c[2];
	c[1] = 0;

new_hash_lookup:
	fnode_block = spadfs_find_hash_block(f, hash, &bh, &secno, &next_hash);
	if (unlikely(!fnode_block))
		goto use_next_hash;

next_fnode_block:
	if (unlikely(IS_ERR(fnode_block)))
		return PTR_ERR(fnode_block);
	fnode = fnode_block->fnodes;

next_fnode:
	VALIDATE_FNODE(f->fs, fnode_block, fnode, size, namelen,
		       ok, skip, bad_fnode);

ok:
	spadfs_brelse(f->fs, bh);
	return -ENOTEMPTY;

skip:
	fnode = (struct fnode *)((char *)fnode + size);
	if (likely(((unsigned long)fnode & (FNODE_BLOCK_SIZE - 1)) != 0))
		goto next_fnode;

	if (!(fnode_block->flags & FNODE_BLOCK_LAST)) {
		spadfs_brelse(f->fs, bh);
		secno++;
read_next_fnode_block:
		if (unlikely(spadfs_stop_cycles(f->fs, secno, &c,
					"spadfs_check_directory_empty"))) {
			return -EFSERROR;
		}
		fnode_block = spadfs_read_fnode_block(f->fs, secno, &bh,
						SRFB_FNODE,
						"spadfs_check_directory_empty");
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
		hash = next_hash;
		goto new_hash_lookup;
	}
	return 0;

bad_fnode:
	spadfs_brelse(f->fs, bh);
	spadfs_error(f->fs, TXFLAGS_FS_ERROR,
		"spadfs_check_directory_empty: bad fnode on block %Lx",
		(unsigned long long)secno);
	return -EFSERROR;
}

static int spadfs_remove_recursive(SPADFNODE *fn, sector_t root, int depth)
{
	SPADFS *fs = fn->fs;
	struct fnode_block *fnode_block;
	struct buffer_head *bh;
	sector_t c[2];
	int r;
	c[1] = 0;

next_in_chain:
	fnode_block = spadfs_read_fnode_block(fs, root, &bh,
					      SRFB_FNODE | SRFB_DNODE,
					      "spadfs_remove_recursive 1");
	if (unlikely(IS_ERR(fnode_block)))
		return PTR_ERR(fnode_block);

	if (likely(SPAD2CPU32_LV(&fnode_block->magic) ==
		   spadfs_magic(fs, root, FNODE_BLOCK_MAGIC))) {
		sector_t next_root;
		unsigned nsec = 1;
		while (!(fnode_block->flags & FNODE_BLOCK_LAST)) {
			spadfs_brelse(fs, bh);
			fnode_block = spadfs_read_fnode_block(fs, root + nsec,
						&bh, SRFB_FNODE,
						"spadfs_remove_recursive 2");
			if (unlikely(IS_ERR(fnode_block)))
				return PTR_ERR(fnode_block);

			nsec++;
			if (unlikely(nsec > 1U << fs->sectors_per_page_bits)) {
				spadfs_brelse(fs, bh);
				spadfs_error(fs, TXFLAGS_FS_ERROR,
					"too long fnode block run at %Lx",
					(unsigned long long)root);
				return -EFSERROR;
			}
		}
		next_root = 0;
		if (CC_VALID(fs, &fnode_block->cc, &fnode_block->txc))
			next_root = MAKE_D_OFF(fnode_block->next0,
					       fnode_block->next1);
		spadfs_brelse(fs, bh);
		r = spadfs_free_directory_blocks(fn, root, nsec);
		if (unlikely(r))
			return r;

		if (next_root) {
			if (unlikely(spadfs_stop_cycles(fs, next_root, &c,
						"spadfs_remove_recursive")))
				return -EFSERROR;
			root = next_root;
			goto next_in_chain;
		}
		return 0;
	} else {
		unsigned hp, i;
		sector_t ln;
		int version;
		if (unlikely(depth >= SPADFS_HASH_BITS)) {
			spadfs_brelse(fs, bh);
			spadfs_error(fs, TXFLAGS_FS_ERROR,
				"recurse overflow on block %Lx",
				(unsigned long long)root);
			return -EFSERROR;
		}
		version = dnode_version(fs, (struct dnode_page *)fnode_block);
		spadfs_brelse(fs, bh);
		ln = 0;
		hp = 0;
		while (1) {
			sector_t n = 0;	/* against warning */
			int r = spadfs_read_dnode(fs, root, version, hp, &n);
			if (unlikely(r))
				return r;
			if (n && n != ln) {
				ln = n;
				r = spadfs_remove_recursive(fn, n,
						depth + fs->dnode_hash_bits);
				if (unlikely(r))
					return r;
			}
			i = 1 << (fs->dnode_hash_bits - 1);
			while (!((hp ^= i) & i))
				if (unlikely(!(i >>= 1)))
					goto brk;
		}
brk:
		return spadfs_free_directory_blocks(fn, root,
						    fs->dnode_page_sectors);
	}
}

int spadfs_remove_directory(SPADFNODE *fn)
{
	int r;
	r = spadfs_remove_recursive(fn, fn->root, 0);
	if (unlikely(r))
		return r;

	if (likely(directory_size(fn->fs))) {
		if (unlikely(inode(fn)->i_size != 0))
			spadfs_error(fn->fs, TXFLAGS_FS_ERROR,
				     "directory size miscounted, %Lx leaked",
				     (unsigned long long)inode(fn)->i_size);
	}

	return 0;
}

sector_t spadfs_alloc_hint(SPADFNODE *f, int hint)
{
	SPADFS *fs = f->fs;
	struct fnode_block *fnode_block;
	struct fnode *fnode;
	struct buffer_head *bh;
	unsigned group;
	if (unlikely(is_deleted_file(f))) {
return_default:
		if (unlikely(hint == HINT_META))
			return 0;
		return (sector_t)fs->zones[1 + (hint == HINT_BIG)].grp_start <<
			fs->sectors_per_group_bits;
	}
	if (unlikely(hint == HINT_META))
		return f->fnode_block;

	if (unlikely(is_fnode_fixed(f))) {
		fnode_block = spadfs_read_fnode_block(fs, f->fnode_block, &bh,
						SRFB_FIXED_FNODE,
						"spadfs_alloc_hint (fixed)");
		if (unlikely(IS_ERR(fnode_block)))
			goto return_default;
		if (hint == HINT_SMALL)
			group = SPAD2CPU16_LV(&((struct fixed_fnode_block *)
					      fnode_block)->hint_small);
		else
			group = SPAD2CPU16_LV(&((struct fixed_fnode_block *)
					      fnode_block)->hint_large);
		goto brelse_ret;
	}
	fnode_block = spadfs_read_fnode_block(fs, f->parent_fnode_block, &bh,
					      SRFB_FNODE | SRFB_FIXED_FNODE,
					      "spadfs_alloc_hint");
	if (unlikely(IS_ERR(fnode_block)))
		goto return_default;
	fnode = (struct fnode *)((char *)fnode_block + f->parent_fnode_pos);
	if (hint == HINT_SMALL)
		group = SPAD2CPU16_LV(&fnode->run1n);
	else
		group = SPAD2CPU16_LV(&fnode->run2n);

brelse_ret:
	spadfs_brelse(fs, bh);
	return (sector_t)group << fs->sectors_per_group_bits;
}

void spadfs_set_new_hint(SPADFNODE *f, struct alloc *al)
{
	struct fnode_block *fnode_block;
	struct fnode *fnode;
	struct buffer_head *bh;
	unsigned hint;
	SPADFS *fs = f->fs;

	if (unlikely(is_deleted_file(f)))
		return;

	hint = al->sector >> fs->sectors_per_group_bits;
	if (unlikely(is_fnode_fixed(f))) {
		fnode_block = spadfs_read_fnode_block(fs, f->fnode_block, &bh,
						SRFB_FIXED_FNODE,
						"spadfs_set_new_hint (fixed)");
		if (unlikely(IS_ERR(fnode_block)))
			return;

		start_concurrent_atomic_buffer_modify(fs, bh);
		if (likely(al->flags & ALLOC_BIG_FILE))
			CPU2SPAD16_LV(&((struct fixed_fnode_block *)fnode_block)
							->hint_large, hint);
		else
			CPU2SPAD16_LV(&((struct fixed_fnode_block *)fnode_block)
							->hint_small, hint);
		goto checksum_done;
	}

	fnode_block = spadfs_read_fnode_block(fs, f->parent_fnode_block, &bh,
					SRFB_FNODE | SRFB_FIXED_FNODE,
					"spadfs_set_new_hint");
	if (unlikely(IS_ERR(fnode_block)))
		return;

	fnode = (struct fnode *)((char *)fnode_block + f->parent_fnode_pos);
	start_concurrent_atomic_buffer_modify(fs, bh);
	if (likely(al->flags & ALLOC_BIG_FILE))
		CPU2SPAD16_LV(&fnode->run2n, hint);
	else
		CPU2SPAD16_LV(&fnode->run1n, hint);
checksum_done:
	do_fnode_block_checksum(fs, fnode_block);
	end_concurrent_atomic_buffer_modify(fs, bh);
	spadfs_brelse(fs, bh);
}

void spadfs_get_dir_hint(SPADFNODE *f, u16 *small, u16 *big)
{
	SPADFS *fs = f->fs;
	struct fnode_block *fnode_block;
	struct fnode *fnode;
	struct buffer_head *bh;
	*small = *big = 0;

	if (unlikely(is_deleted_file(f)))
		return;

	fnode_block = spadfs_read_fnode_block(fs, f->fnode_block, &bh,
					      SRFB_FNODE | SRFB_FIXED_FNODE,
					      "spadfs_get_dir_hint");

	if (unlikely(IS_ERR(fnode_block)))
		return;

	fnode = (struct fnode *)((char *)fnode_block + f->fnode_pos);
	*small = SPAD2CPU16_LV(&fnode->run1n);
	*big = SPAD2CPU16_LV(&fnode->run2n);
	spadfs_brelse(fs, bh);
}

/* Update extended attributes */

static void write_ea(SPADFNODE *f, struct fnode *fnode)
{
	unsigned long ea = (unsigned long)fnode +
			   FNODE_EA_POS(*(volatile u8 *)&fnode->namelen);
	if (unlikely(((ea + f->ea_size - 1) &
		     ~(unsigned long)(FNODE_BLOCK_SIZE - 1)) !=
		     ((unsigned long)fnode &
		     ~(unsigned long)(FNODE_BLOCK_SIZE - 1)))) {
		spadfs_error(f->fs, TXFLAGS_FS_ERROR,
			"can't write extended attributes on fnode %Lx/%x(%lx)",
			(unsigned long long)f->fnode_block,
			f->fnode_pos,
			(unsigned long)fnode & (FNODE_BLOCK_SIZE - 1));
		return;
	}
	if (unlikely((SPAD2CPU16_LV(&fnode->next) & FNODE_NEXT_SIZE) !=
		     FNODE_SIZE(fnode->namelen, f->ea_size))) {
		spadfs_error(f->fs, TXFLAGS_FS_ERROR,
			"can't write extended attributes on fnode %Lx/%x(%lx): fnode_size %x, namelen %x, ea_size %x",
			(unsigned long long)f->fnode_block,
			f->fnode_pos,
			(unsigned long)fnode & (FNODE_BLOCK_SIZE - 1),
			(unsigned)SPAD2CPU16_LV(&fnode->next),
			fnode->namelen,
			f->ea_size);
		return;
	}
	memcpy((void *)ea, f->ea, f->ea_size);
}

static void set_spadfs_file(SPADFNODE *f, struct fnode *fnode, int part)
{
	loff_t is, pa;
	loff_t disk_othersize, want_othersize;

	fnode->ctime = CPU2SPAD32(inode_get_ctime(inode(f)).tv_sec);
	fnode->mtime = CPU2SPAD32(inode(f)->i_mtime.tv_sec);
	fnode->flags = 0;
	if (unlikely(S_ISLNK(inode(f)->i_mode))) {
		u64 ino;
		gid_t gid;
		ino = f->stable_ino;
		fnode->run10 = CPU2SPAD32(ino);
		fnode->run11 = CPU2SPAD16(ino >> 32);
		fnode->run1n = CPU2SPAD16(ino >> 48);
		fnode->run20 = CPU2SPAD32(i_uid_read(inode(f)));
		gid = i_gid_read(inode(f));
		fnode->run21 = CPU2SPAD16(gid);
		fnode->run2n = CPU2SPAD16(gid >> 16);
		fnode->anode0 = MAKE_PART_0(0);
		fnode->anode1 = MAKE_PART_1(0);
		CPU2SPAD64_LV(&fnode->size[0], 0);
		CPU2SPAD64_LV(&fnode->size[1], 0);
		return;
	}

	fnode->run10 = MAKE_PART_0(f->blk1);
	fnode->run11 = MAKE_PART_1(f->blk1);
	fnode->run1n = CPU2SPAD16(f->blk1_n);
	fnode->run20 = MAKE_PART_0(f->blk2);
	fnode->run21 = MAKE_PART_1(f->blk2);
	fnode->run2n = CPU2SPAD16(f->blk2_n);
	fnode->anode0 = MAKE_PART_0(f->root);
	fnode->anode1 = MAKE_PART_1(f->root);

	is = i_size_read(inode(f));

	if (unlikely(is > f->disk_size)) {
		CPU2SPAD64_LV(&fnode->size[part], f->disk_size);
		pa = 0;
	} else if (likely(is + (512U << f->fs->sectors_per_disk_block_bits) > f->disk_size)) {
		CPU2SPAD64_LV(&fnode->size[part], is);
		pa = 0;
	} else {
		pa = f->disk_size - (512U << f->fs->sectors_per_disk_block_bits) + 1;
		CPU2SPAD64_LV(&fnode->size[part], pa);
		pa -= is;
		if (unlikely((pa & ~0xffffffffULL) != 0))
			pa = 0xffffffff;
	}

	if (likely(f->ea_unx != NULL))
		CPU2SPAD32_LV(&f->ea_unx->prealloc[part], pa);

	disk_othersize = spadfs_roundup_blocksize(f->fs,
					SPAD2CPU64_LV(&fnode->size[part ^ 1]));
	if (likely(f->commit_sequence == f->fs->commit_sequence))
		want_othersize = f->crash_disk_size;
	else
		want_othersize = f->disk_size;
	if (unlikely(disk_othersize != want_othersize)) {
		CPU2SPAD64_LV(&fnode->size[part ^ 1], want_othersize);
		if (likely(f->ea_unx != NULL))
			CPU2SPAD32_LV(&f->ea_unx->prealloc[part ^ 1], 0);
	}
}

int spadfs_write_file(SPADFNODE *f, int datasync, int *optimized,
		      struct buffer_head **bhp)
{
	struct fnode_block *fnode_block;
	struct fnode *fnode;
	struct buffer_head *bh;
	unsigned old_prealloc;
	SPADFS *fs = f->fs;

	BUG_ON(S_ISDIR(inode(f)->i_mode));

	if (unlikely(is_deleted_file(f)))
		return 0;

	fnode_block = spadfs_read_fnode_block(fs, f->fnode_block, &bh,
					      SRFB_FNODE | SRFB_FIXED_FNODE,
					      "spadfs_write_file");
	if (unlikely(IS_ERR(fnode_block)))
		return PTR_ERR(fnode_block);

	fnode = (struct fnode *)((char *)fnode_block + f->fnode_pos);

	start_concurrent_atomic_buffer_modify(fs, bh);
	if (unlikely((SPAD2CPU16_LV(&fnode->next) & FNODE_NEXT_SIZE) !=
		     FNODE_SIZE(fnode->namelen, f->ea_size))) {
		spadfs_error(fs, TXFLAGS_FS_ERROR,
			"read different file fnode at %Lx/%x: %x",
			(unsigned long long)f->fnode_block,
			f->fnode_pos,
			SPAD2CPU16_LV(&fnode->next));
		end_concurrent_atomic_buffer_modify(fs, bh);
		spadfs_brelse(fs, bh);
		return -EFSERROR;
	}

	old_prealloc = 0;	/* against warning */
#ifdef SPADFS_OPTIMIZE_FSYNC
	if (!CC_CURRENT(fs, &fnode->cc, &fnode->txc)) {
		if (spadfs_roundup_blocksize(fs, SPAD2CPU64_LV(
		    &fnode->size[!CC_VALID(fs, &fnode->cc, &fnode->txc)])) ==
		    f->disk_size) {
			if (unlikely(optimized != NULL)) {
				if (datasync && f->ea_unx)
					old_prealloc = SPAD2CPU32_LV(
					    &f->ea_unx->prealloc[!CC_VALID(fs,
					    &fnode->cc, &fnode->txc)]);
				*optimized = 1;
			}
			goto optimize_it;
		}
		CC_SET_CURRENT(fs, &fnode->cc, &fnode->txc);
		CPU2SPAD16_LV(&fnode->next, SPAD2CPU16_LV(&fnode->next) &
							~FNODE_NEXT_FREE);
	}
optimize_it:
#endif
	set_spadfs_file(f, fnode, !CC_VALID(fs, &fnode->cc, &fnode->txc));
	write_ea(f, fnode);
	do_fnode_block_checksum(fs, fnode_block);

#ifdef SPADFS_OPTIMIZE_FSYNC
	if (unlikely(datasync) && likely(optimized != NULL) &&
	    likely(*optimized) && likely(!buffer_dirty(bh))) {
		unsigned current_prealloc = SPAD2CPU32_LV(&f->ea_unx->prealloc[
				CC_VALID(fs, &fnode->cc, &fnode->txc) ^
				CC_CURRENT(fs, &fnode->cc, &fnode->txc) ^ 1]);
		if (old_prealloc == current_prealloc) {
			end_concurrent_atomic_buffer_modify_nodirty(fs, bh);
			goto ret_0;
		}
	}
#endif

	end_concurrent_atomic_buffer_modify(fs, bh);

#ifdef SPADFS_OPTIMIZE_FSYNC
ret_0:
#endif
	if (likely(!bhp))
		spadfs_brelse(fs, bh);
	else
		*bhp = bh;

	return 0;
}

static void set_spadfs_directory(SPADFNODE *f, struct fnode *fnode, int part)
{
	fnode->ctime = CPU2SPAD32(inode_get_ctime(inode(f)).tv_sec);
	fnode->mtime = CPU2SPAD32(inode(f)->i_mtime.tv_sec);
	fnode->flags = FNODE_FLAGS_DIR;
	CPU2SPAD64_LV(&fnode->size[part], i_size_read(inode(f)));
	if (!part) {
		fnode->run10 = MAKE_PART_0(f->root);
		fnode->run11 = MAKE_PART_1(f->root);
	} else {
		fnode->run20 = MAKE_PART_0(f->root);
		fnode->run21 = MAKE_PART_1(f->root);
	}
}

int spadfs_write_directory(SPADFNODE *f)
{
	struct fnode_block *fnode_block;
	struct fnode *fnode;
	struct buffer_head *bh;
	SPADFS *fs = f->fs;

	BUG_ON(!S_ISDIR(inode(f)->i_mode));

	if (unlikely(is_deleted_file(f)))
		return 0;

	fnode_block = spadfs_read_fnode_block(fs, f->fnode_block, &bh,
					      SRFB_FNODE | SRFB_FIXED_FNODE,
					      "spadfs_write_directory");
	if (unlikely(IS_ERR(fnode_block)))
		return PTR_ERR(fnode_block);

	fnode = (struct fnode *)((char *)fnode_block + f->fnode_pos);

	start_concurrent_atomic_buffer_modify(fs, bh);
	if (unlikely((SPAD2CPU16_LV(&fnode->next) & FNODE_NEXT_SIZE) !=
		     FNODE_SIZE(fnode->namelen, f->ea_size))) {
		spadfs_error(fs, TXFLAGS_FS_ERROR,
			"read different directory fnode at %Lx/%x: %x",
			(unsigned long long)f->fnode_block,
			f->fnode_pos,
			SPAD2CPU16_LV(&fnode->next));
		end_concurrent_atomic_buffer_modify(fs, bh);
		spadfs_brelse(fs, bh);
		return -EFSERROR;
	}
	if (!CC_CURRENT(fs, &fnode->cc, &fnode->txc)) {
		CC_SET_CURRENT(fs, &fnode->cc, &fnode->txc);
		CPU2SPAD16_LV(&fnode->next, SPAD2CPU16_LV(&fnode->next) &
			      ~FNODE_NEXT_FREE);
	}
	set_spadfs_directory(f, fnode, !CC_VALID(fs, &fnode->cc, &fnode->txc));
	write_ea(f, fnode);
	do_fnode_block_checksum(fs, fnode_block);
	end_concurrent_atomic_buffer_modify(fs, bh);
	spadfs_brelse(fs, bh);
	return 0;
}

static int spadfs_find_leaf_page_for_hash(SPADFNODE *fn, sector_t root,
					  hash_t hash, sector_t *fnode_sector,
					  sector_t *prev_dnode, int *hash_bits,
					  int *splitable)
{
	SPADFS *fs = fn->fs;
	struct buffer_head *bh;
	struct fnode_block *f;
	*prev_dnode = 0;
	*hash_bits = 0;
	*splitable = -1;
subnode:
	f = spadfs_read_fnode_block(fs, root, &bh, SRFB_FNODE | SRFB_DNODE,
				    "spadfs_find_leaf_page_for_hash");
	*fnode_sector = root;
	if (unlikely(IS_ERR(f)))
		return PTR_ERR(f);

	if (likely(SPAD2CPU32_LV(&f->magic) ==
		   spadfs_magic(fs, root, FNODE_BLOCK_MAGIC))) {
		spadfs_brelse(fs, bh);
		return 0;
	} else {
		int i, j;
		int r;
		sector_t new_root;
		hash_t hpos;
		int version = dnode_version(fs, (struct dnode_page *)f);
		spadfs_brelse(fs, bh);

		*prev_dnode = root;
		hpos = hash & ((1 << (fs->dnode_hash_bits)) - 1);

		r = spadfs_read_dnode(fs, root, version, hpos, &new_root);
		if (unlikely(r))
			return r;

		if (unlikely(!new_root)) {
			sector_t new_page_sector;
			int x;
			int step;
			r = spadfs_alloc_leaf_page(fn, root, 1U << fs->sectors_per_fnodepage_bits, 0, &new_page_sector, 0);
			if (unlikely(r == -ENOSPC) || unlikely(r == -EDQUOT))
				r = spadfs_alloc_leaf_page(fn, root, 1U << fs->sectors_per_disk_block_bits, 0, &new_page_sector, 0);
			if (unlikely(r))
				return r;
			version = spadfs_begin_modify_dnode(fs, root);
			if (unlikely(version < 0))
				return version;
			r = spadfs_write_dnode(fs, root, version, hpos, new_page_sector);
			if (unlikely(r))
				return r;
			step = 1 << fs->dnode_hash_bits;
			do {
				hpos ^= step >> 1;
				for (x = hpos; x < 1 << fs->dnode_hash_bits; x += step) {
					sector_t test;
					r = spadfs_read_dnode(fs, root, version,
							      x, &test);
					if (unlikely(r))
						return r;
					if (test)
						goto done;
				}
				for (x = hpos; x < 1 << fs->dnode_hash_bits; x += step) {
					r = spadfs_write_dnode(fs, root,
							       version, x,
							       new_page_sector);
					if (unlikely(r))
						return r;
				}
				step >>= 1;
				hpos &= ~step;
			} while (step > 1);
done:
			root = new_page_sector;
			goto subnode;
		}
		if (unlikely(*hash_bits % (unsigned)fs->dnode_hash_bits) ||
		    unlikely(*splitable != -1))
			goto bad_tree;

		j = 1 << fs->dnode_hash_bits;
		for (i = 1; i < j; i <<= 1) {
			sector_t test = 0;	/* against warning */
			r = spadfs_read_dnode(fs, root, version, hpos ^ i,
					      &test);
			if (unlikely(r))
				return r;
			if (test == new_root) {
				*splitable = hpos;
				break;
			}
			(*hash_bits)++;
		}
		hash >>= fs->dnode_hash_bits;
		if (unlikely(*hash_bits > SPADFS_HASH_BITS))
			goto bad_tree;
		root = new_root;
		goto subnode;
	}
bad_tree:
	spadfs_error(fs, TXFLAGS_FS_ERROR,
		"bad dnode tree on %Lx (parent %Lx/%x)",
		(unsigned long long)root,
		(unsigned long long)fn->fnode_block,
		fn->fnode_pos);
	return -EFSERROR;
}

#define HASH_1_SHIFT	16
#define HASH_1		(1 << HASH_1_SHIFT)
#define HASH_VAL	(HASH_1 - 1)

static int test_hash_bit(char *name, unsigned namelen, unsigned hash_bits)
{
	hash_t hash = name_len_hash(name, namelen);
	return ((hash >> (hash_bits & HASH_VAL)) ^
		(hash_bits >> HASH_1_SHIFT) ^ 1) & 1;
}

static int test_hash(SPADFS *fs, sector_t sec, int hash_bits)
{
	hash_t hash;
	int found = 0;
	struct buffer_head *bh;
	struct fnode_block *fnode_block;
	struct fnode *fnode;
	unsigned size, namelen;
next_block:
	fnode_block = spadfs_read_fnode_block(fs, sec, &bh, SRFB_FNODE,
					      "test_hash");
	if (unlikely(IS_ERR(fnode_block)))
		return PTR_ERR(fnode_block);

	fnode = fnode_block->fnodes;

next_fnode:
	VALIDATE_FNODE(fs, fnode_block, fnode, size, namelen,
		       used, free, bad_fnode);

used:
	hash = name_len_hash(FNODE_NAME(fnode), namelen);
	found |= 1 << ((hash >> hash_bits) & 1);
	if (found == 3) {
		spadfs_brelse(fs, bh);
		return 3;
	}

free:
	fnode = (struct fnode *)((char *)fnode + size);
	if (likely(((unsigned long)fnode & (FNODE_BLOCK_SIZE - 1)) != 0))
		goto next_fnode;

	if (!(fnode_block->flags & FNODE_BLOCK_LAST)) {
		spadfs_brelse(fs, bh);
		sec++;
		goto next_block;
	}
	spadfs_brelse(fs, bh);
	return found;

bad_fnode:
	spadfs_brelse(fs, bh);
	spadfs_error(fs, TXFLAGS_FS_ERROR,
		"test_hash: bad fnode on block %Lx",
		(unsigned long long)sec);
	return -EFSERROR;
}

/*
 * Copy fnodes from fnode blocks starting with src_sec up to the block with
 * FNODE_BLOCK_LAST to fnode blocks starting with dst_sec up to the block with
 * FNODE_BLOCK_LAST. Destination must be largrer or equal than source. Fix
 * pointer on in-memory fnoes.
 * Copy only fnodes that match condition (x is a cookie passed to it).
 * If preserve_pos is true, we must preserve positions of entries, otherwise
 * readdir could skip them.
 */

static int copy_fnodes_to_block(SPADFS *fs, sector_t dst_sec, sector_t src_sec,
				int (*condition)(char *name, unsigned namelen,
						 unsigned x),
				unsigned x, int preserve_pos)
{
	struct fnode_block *src_block, *dst_block;
	struct fnode *src_fnode, *dst_fnode;
	struct buffer_head *bh_src, *bh_dst;
	unsigned size, dsize;
	unsigned namelen;

	dst_block = spadfs_read_fnode_block(fs, dst_sec, &bh_dst, SRFB_FNODE,
					    "copy_fnodes_to_block 1");
	if (unlikely(IS_ERR(dst_block)))
		return PTR_ERR(dst_block);

	dst_fnode = dst_block->fnodes;
	dsize = FNODE_MAX_SIZE;

new_src_block:
	src_block = spadfs_read_fnode_block(fs, src_sec, &bh_src, SRFB_FNODE,
					    "copy_fnodes_to_block 2");
	if (unlikely(IS_ERR(src_block))) {
		spadfs_brelse(fs, bh_dst);
		return PTR_ERR(src_block);
	}
	src_fnode = src_block->fnodes;

next_fnode:
	VALIDATE_FNODE(fs, src_block, src_fnode, size, namelen,
		      ok, skip, bad_fnode);

ok:
	if (condition && !condition(FNODE_NAME(src_fnode), namelen, x)) {
skip:
		if (preserve_pos) {
	/*
	 * adding new entry will join successive free fnodes anyway, no
	 * need to mess with it now
	 */
			if (unlikely(dsize < size)) {
				if (unlikely(dsize))
					spadfs_error(fs, TXFLAGS_FS_ERROR,
						"destination is not in sync with source, from %Lx to %Lx, dsize %u, size %u",
						(unsigned long long)src_sec,
						(unsigned long long)dst_sec,
						dsize, size);
				goto new_dest;
			}
			CPU2SPAD16_LV(&dst_fnode->next, size | FNODE_NEXT_FREE);
			CPU2SPAD16_LV(&dst_fnode->cc, 0);
			CPU2SPAD32_LV(&dst_fnode->txc, 0);
			goto new_dst_fnode;
		}
		goto new_src_fnode;
	}
	if (likely(dsize >= size)) {
		memcpy(dst_fnode, src_fnode, size);
		spadfs_move_fnode_ptr(fs, src_sec,
				      (char *)src_fnode - (char *)src_block,
				      dst_sec,
				      (char *)dst_fnode - (char *)dst_block,
				      src_fnode->flags & FNODE_FLAGS_DIR);
new_dst_fnode:
		if (likely(dsize -= size)) {
			dst_fnode = (struct fnode *)((char *)dst_fnode + size);
			CPU2SPAD16_LV(&dst_fnode->next,
				      dsize | FNODE_NEXT_FREE);
			CPU2SPAD16_LV(&dst_fnode->cc, 0);
			CPU2SPAD32_LV(&dst_fnode->txc, 0);
		}
new_src_fnode:
		src_fnode = (struct fnode *)((char *)src_fnode + size);
		if (unlikely(!((unsigned long)src_fnode &
			       (FNODE_BLOCK_SIZE - 1)))) {
			if (unlikely((src_block->flags & FNODE_BLOCK_LAST) !=
				     0))
				goto end;
			spadfs_brelse(fs, bh_src);
			src_sec++;
			goto new_src_block;
		}
		goto next_fnode;
	} else {
new_dest:
		do_fnode_block_checksum(fs, dst_block);
		mark_buffer_dirty(bh_dst);
		if (unlikely(dst_block->flags & FNODE_BLOCK_LAST)) {
			spadfs_error(fs, TXFLAGS_FS_ERROR,
				"ran over destination when copying fnode blocks from %Lx to %Lx",
				(unsigned long long)src_sec,
				(unsigned long long)dst_sec);
			spadfs_brelse(fs, bh_dst);
			spadfs_brelse(fs, bh_src);
			return -EFSERROR;
		}
		spadfs_brelse(fs, bh_dst);
		dst_sec++;
		dst_block = spadfs_read_fnode_block(fs, dst_sec, &bh_dst,
						    SRFB_FNODE,
						    "copy_fnodes_to_block 3");
		if (unlikely(IS_ERR(dst_block))) {
			spadfs_brelse(fs, bh_src);
			return PTR_ERR(dst_block);
		}
		dst_fnode = dst_block->fnodes;
		dsize = FNODE_MAX_SIZE;
		goto next_fnode;
	}
end:
	spadfs_brelse(fs, bh_src);
	do_fnode_block_checksum(fs, dst_block);
	mark_buffer_dirty(bh_dst);
	spadfs_brelse(fs, bh_dst);
	return 0;

bad_fnode:
	spadfs_brelse(fs, bh_src);
	spadfs_brelse(fs, bh_dst);
	spadfs_error(fs, TXFLAGS_FS_ERROR,
		"bad fnode on block %Lx when copying fnodes",
		(unsigned long long)src_sec);
	return -EFSERROR;
}

struct fnode *spadfs_add_fnode_to_directory(SPADFNODE *dir,
					    const char *name, unsigned namelen,
					    unsigned ea_size,
					    struct buffer_head **bhp,
					    sector_t *fnode_address,
					    unsigned *fnode_off,
					    struct fnode_block **pfnode_block,
					    int wlock)
{
	SPADFS *fs = dir->fs;
	sector_t c[2];
	int hash_bits, splitable;
	int r;
	int pass2;
	unsigned long long chains;
	sector_t fnode_blk_sector;
	unsigned n_sec;
	sector_t dnode;
	int size, rsize;
	unsigned xnamelen;
	int restarts;
	struct fnode *fnode;
	struct fnode_block *fnode_block;
	hash_t hash;

	c[1] = 0;

	if (unlikely(namelen > MAX_NAME_LEN))
		return ERR_PTR(-ENAMETOOLONG);
	hash = name_len_hash(name, namelen);
	BUG_ON(ea_size > FNODE_MAX_EA_SIZE);
	rsize = FNODE_SIZE(namelen, ea_size);
	restarts = 0;

total_restart:
	if (unlikely(++restarts > SPADFS_HASH_BITS + 1)) {
		spadfs_error(fs, TXFLAGS_FS_ERROR,
			"too many splits at directory %Lx --- probably unreliable device",
			(unsigned long long)dir->root);
		return ERR_PTR(-EFSERROR);
	}

	r = spadfs_find_leaf_page_for_hash(dir, dir->root, hash,
					   &fnode_blk_sector, &dnode,
					   &hash_bits, &splitable);
	if (unlikely(r))
		return ERR_PTR(r);

	pass2 = 0;
	chains = 0;

next_fnode_page:
	n_sec = 0;

next_fnode_block:

	fnode_block = spadfs_read_fnode_block(fs, fnode_blk_sector + n_sec, bhp,
					      SRFB_FNODE,
					      "spadfs_add_fnode_to_directory");
	if (unlikely(IS_ERR(fnode_block)))
		return (void *)fnode_block;

	fnode = fnode_block->fnodes;

next_fnode:
retry_fnode:
	VALIDATE_FNODE(fs, fnode_block, fnode, size, xnamelen,
		       used, free, bad_fnode);

free:
	if (!CC_CURRENT(fs, &fnode->cc, &fnode->txc)) {
		if (size >= rsize) {
			*fnode_address = fnode_blk_sector + n_sec;
			*fnode_off = (char *)fnode - (char *)fnode_block;
			*pfnode_block = fnode_block;
			start_concurrent_atomic_buffer_modify(fs, *bhp);
			if (rsize < size) {
				struct fnode *afterfnode = (struct fnode *)
					((char *)fnode + rsize);
				CPU2SPAD16_LV(&afterfnode->next,
					(size - rsize) | FNODE_NEXT_FREE);
				CPU2SPAD16_LV(&afterfnode->cc, 0);
				CPU2SPAD32_LV(&afterfnode->txc, 0);
				CPU2SPAD16_LV(&fnode->next,
					rsize | FNODE_NEXT_FREE);
			}
			CC_SET_CURRENT_INVALID(fs, &fnode->cc, &fnode->txc);
			return fnode;
		} else {
			struct fnode *new_fnode = (struct fnode *)
						  ((char *)fnode + size);
			int new_size;
			if (unlikely(!((unsigned long) new_fnode &
				       (FNODE_BLOCK_SIZE - 1))))
				goto end_of_block;
			new_size = SPAD2CPU16_LV(&new_fnode->next) & FNODE_NEXT_SIZE;
			VALIDATE_FNODE(fs, fnode_block, new_fnode, new_size,
				       xnamelen,
				       s_used, s_free, bad_fnode);
s_free:
			if (likely(!CC_CURRENT(fs, &new_fnode->cc,
					       &new_fnode->txc))) {
				start_concurrent_atomic_buffer_modify(fs, *bhp);
				CPU2SPAD16_LV(&fnode->next,
					SPAD2CPU16_LV(&fnode->next) + new_size);
				do_fnode_block_checksum(fs, fnode_block);
				end_concurrent_atomic_buffer_modify(fs, *bhp);
				goto retry_fnode;
			}
s_used:
			fnode = new_fnode;
			size = new_size;
		}
	}

used:
	fnode = (struct fnode *)((char *)fnode + size);
	if (likely(((unsigned long)fnode & (FNODE_BLOCK_SIZE - 1)) != 0))
		goto next_fnode;

end_of_block:
	if (!(fnode_block->flags & FNODE_BLOCK_LAST)) {
		n_sec++;
		spadfs_brelse(fs, *bhp);
		goto next_fnode_block;
	}

	if (CC_VALID(fs, &fnode_block->cc, &fnode_block->txc)) {
		fnode_blk_sector = MAKE_D_OFF(fnode_block->next0,
					      fnode_block->next1);
		spadfs_brelse(fs, *bhp);
		if (unlikely(spadfs_stop_cycles(fs, fnode_blk_sector, &c,
					"spadfs_add_fnode_to_directory")))
			return ERR_PTR(-EFSERROR);
		pass2 = 1;
		chains++;
		goto next_fnode_page;
	}
	if (CC_CURRENT(fs, &fnode_block->cc, &fnode_block->txc)) {
		spadfs_brelse(fs, *bhp);
		return NEED_SYNC;
	}
	if (likely(!pass2)) {
		int th;
		sector_t fnode_block_0_sector;
		sector_t fnode_block_1_sector;
		int version;
		int bits;

		if (!wlock) {
			spadfs_brelse(fs, *bhp);
			return NEED_WLOCK;
		}

/* 1. REALLOC CURRENT BLOCK IF IT'S ROOT & SMALL */

		/*
		 * warning, n_sec is the last sector number
		 * --- i.e. there are n_sec+1 sectors
		 */
		if (!dnode && (n_sec + 1) * 2 <
		    1U << fs->sectors_per_fnodepage_bits) {
			unsigned new_sectors;
			sector_t new_sector;
			new_sectors = FNODE_BLOCK_SIZE / 512;
			while (new_sectors < (n_sec + 1) * 2)
				new_sectors <<= 1;
			r = spadfs_alloc_leaf_page(dir, fnode_blk_sector,
						   new_sectors, 0,
						   &new_sector, 0);
			if (unlikely(r)) {
				if (likely(r == -ENOSPC) || r == -EDQUOT)
					goto alloc_chain;
				goto brelse_return_r;
			}
			spadfs_brelse(fs, *bhp);
			/*
			 * trick: start copying to new_sector + n_sec + 1 (i.e.
			 * entirely newly allocated blocks) to make sure that
			 * readdir won't skip any entry. Otherwise we'd have to
			 * set preserve_pos --- it would waste space more.
			 */
			r = copy_fnodes_to_block(fs, new_sector + n_sec + 1,
						 fnode_blk_sector, NULL, 0, 0);
			if (unlikely(r)) {
				spadfs_free_directory_blocks(dir, new_sector,
							     new_sectors);
				return ERR_PTR(r);
			}
			spadfs_free_directory_blocks(dir, fnode_blk_sector,
						     n_sec + 1);
			/*
			 * don't handle spadfs_free_directory_blocks error
			 * --- we can't revert copied fnodes
			 */
			dir->root = new_sector;
			r = spadfs_write_directory(dir);
			if (unlikely(r))
				return ERR_PTR(r);
			fnode_blk_sector = new_sector;
			goto next_fnode_page;
		}

/* 2. IF IT'S END OF HASH, ALLOC CHAIN */

		if (unlikely(hash_bits >= SPADFS_HASH_BITS))
			goto alloc_chain;

/* 3. IF THE DNODE IS FULL, ALLOC NEW DNODE */

		if (splitable == -1) {
			sector_t new_dnode;
			r = spadfs_alloc_dnode_page(dir, fnode_blk_sector,
						    &new_dnode, dnode,
						    fnode_blk_sector);
			if (unlikely(r)) {
				if (likely(r == -ENOSPC) || r == -EDQUOT)
					goto alloc_chain;
				goto brelse_return_r;
			}
			spadfs_brelse(fs, *bhp);
			if (dnode) {
				unsigned i;
				int version = spadfs_begin_modify_dnode(fs,
									dnode);
				if (unlikely(version < 0))
					return ERR_PTR(version);
				for (i = 0; i < 1 << fs->dnode_hash_bits; i++) {
					sector_t test;
					r = spadfs_read_dnode(fs, dnode,
							version, i, &test);
					if (unlikely(r))
						return ERR_PTR(r);
					if (test == fnode_blk_sector)
						spadfs_write_dnode(fs, dnode,
								   version, i,
								   new_dnode);
				}
			} else {
				dir->root = new_dnode;
				r = spadfs_write_directory(dir);
				if (unlikely(r))
					return ERR_PTR(r);
			}
			dnode = new_dnode;
			splitable = 0;
			goto next_fnode_page;
		}

/* 4. TEST SPLITABILITY OF FNODE PAGE */

		th = test_hash(fs, fnode_blk_sector, hash_bits);
		if (unlikely(th < 0)) {
			spadfs_brelse(fs, *bhp);
			return ERR_PTR(th);
		}

/* 5. ALLOC NEW 2 FNODE PAGES AND SPLIT FNODE */

		if (th & 1) {
			r = spadfs_alloc_leaf_page(dir, fnode_blk_sector,
				1U << fs->sectors_per_fnodepage_bits, 0,
				&fnode_block_0_sector, 0);
			if (unlikely(r)) {
				if (likely(r == -ENOSPC) || r == -EDQUOT)
					goto alloc_chain;
				goto brelse_return_r;
			}
		} else
			fnode_block_0_sector = 0;

		if (th & 2) {
			r = spadfs_alloc_leaf_page(dir, fnode_blk_sector,
				1U << fs->sectors_per_fnodepage_bits, 0,
				&fnode_block_1_sector, 0);
			if (unlikely(r)) {
				if (fnode_block_0_sector) {
					int rr;
					rr = spadfs_free_directory_blocks(
						dir,
						fnode_block_0_sector,
						1U << fs->sectors_per_fnodepage_bits);
					if (unlikely(rr)) {
						r = rr;
						goto brelse_return_r;
					}
				}
				if (likely(r == -ENOSPC) || r == -EDQUOT)
					goto alloc_chain;
				goto brelse_return_r;
			}

			r = copy_fnodes_to_block(fs,
					fnode_block_1_sector, fnode_blk_sector,
					test_hash_bit, hash_bits | HASH_1, 0);
			if (unlikely(r))
				goto brelse_return_r;
		} else
			fnode_block_1_sector = 0;
		if (th & 1) {
			r = copy_fnodes_to_block(fs,
					fnode_block_0_sector, fnode_blk_sector,
					test_hash_bit, hash_bits, 1);
			if (unlikely(r))
				goto brelse_return_r;
		}

		spadfs_brelse(fs, *bhp);

/* 6. READ PARENT DNODE */

		version = spadfs_begin_modify_dnode(fs, dnode);
		if (unlikely(version < 0))
			return ERR_PTR(version);

/* 7. SPLIT POINTERS ON DNODE */

		bits = hash_bits % (unsigned)fs->dnode_hash_bits;
		splitable &= (1 << bits) - 1;
		do {
			r = spadfs_write_dnode(fs, dnode, version, splitable,
				fnode_block_0_sector);
			if (unlikely(r))
				return ERR_PTR(r);
			splitable += 1 << bits;
			r = spadfs_write_dnode(fs, dnode, version, splitable,
				fnode_block_1_sector);
			if (unlikely(r))
				return ERR_PTR(r);
			splitable += 1 << bits;
		} while (splitable < 1 << fs->dnode_hash_bits);
		r = spadfs_free_directory_blocks(dir, fnode_blk_sector,
						 n_sec + 1);
		if (unlikely(r))
			return ERR_PTR(r);
		goto total_restart;
	} else {
		if (unlikely(pass2 == 2)) {
			spadfs_brelse(fs, *bhp);
			spadfs_error(fs, TXFLAGS_FS_ERROR,
				"couldn't allocate fnode even in new page");
			return ERR_PTR(-EFSERROR);
		}

alloc_chain:
		if (unlikely(chains >= MAX_CHAIN_LENGTH)) {
			/* we couldn't readdir it on Linux. */
			spadfs_brelse(fs, *bhp);
			return ERR_PTR(-ENOSPC);
		}
		r = spadfs_alloc_leaf_page(dir, fnode_blk_sector + n_sec,
					   1U << fs->sectors_per_disk_block_bits,
					   fnode_blk_sector + n_sec,
					   &fnode_blk_sector, 0);
		if (unlikely(r))
			goto brelse_return_r;

		start_concurrent_atomic_buffer_modify(fs, *bhp);
		fnode_block->next0 = MAKE_PART_0(fnode_blk_sector);
		fnode_block->next1 = MAKE_PART_1(fnode_blk_sector);
		CPU2SPAD16_LV(&fnode_block->cc, fs->cc);
		CPU2SPAD32_LV(&fnode_block->txc, fs->txc);
		do_fnode_block_checksum(fs, fnode_block);
		end_concurrent_atomic_buffer_modify(fs, *bhp);
		spadfs_brelse(fs, *bhp);

		pass2 = 2;
		chains++;
		goto next_fnode_page;
	}

brelse_return_r:
	spadfs_brelse(fs, *bhp);
	return ERR_PTR(r);

bad_fnode:
	spadfs_brelse(fs, *bhp);
	spadfs_error(fs, TXFLAGS_FS_ERROR,
		"bad fnode on block %Lx when adding to directory",
		(unsigned long long)(fnode_blk_sector + n_sec));
	return ERR_PTR(-EFSERROR);
}

static noinline void spadfs_swap_fnode(SPADFNODE *file, struct fnode *fnode)
{
	u64 s;

	unsigned fnode_size, ea_offset;
	struct ea_unx *ea_unx;

	if (unlikely(fnode->flags & FNODE_FLAGS_DIR)) {
		u32 r0 = fnode->run10;
		u16 r1 = fnode->run11;
		fnode->run10 = fnode->run20;
		fnode->run11 = fnode->run21;
		fnode->run20 = r0;
		fnode->run21 = r1;
	}
	s = fnode->size[0];
	fnode->size[0] = fnode->size[1];
	fnode->size[1] = s;

	/*
	 Warning: this can't be used here, in case we are growing ea and
	 refiling, we'd smash fnode block here.
	if (file->ea_unx) {
		u32 pa = file->ea_unx->prealloc[0];
		file->ea_unx->prealloc[0] = file->ea_unx->prealloc[1];
		file->ea_unx->prealloc[1] = pa;
		write_ea(file, fnode);
	}
	*/

	ea_offset = FNODE_EA_POS(fnode->namelen);
	fnode_size = SPAD2CPU16_LV(&fnode->next) & FNODE_NEXT_SIZE;
	if (unlikely(ea_offset > fnode_size) ||
	    unlikely(((unsigned long)fnode & (FNODE_BLOCK_SIZE - 1)) +
			fnode_size > FNODE_BLOCK_SIZE)) {
		spadfs_error(file->fs, TXFLAGS_FS_ERROR,
			"spadfs_swap_fnode: invalid fnode");
		return;
	}
	ea_unx = (struct ea_unx *)GET_EA((void *)((u8 *)fnode + ea_offset),
					fnode_size - ea_offset,
					EA_UNX_MAGIC, EA_UNX_MAGIC_MASK);
	if (unlikely(ea_unx == GET_EA_ERROR)) {
		spadfs_error(file->fs, TXFLAGS_FS_ERROR,
			"spadfs_swap_fnode: invalid extended attributes");
		return;
	}
	if (likely(ea_unx != NULL)) {
		u32 pa = ea_unx->prealloc[0];
		ea_unx->prealloc[0] = ea_unx->prealloc[1];
		ea_unx->prealloc[1] = pa;
	}
}

int spadfs_remove_fnode_from_directory(SPADFNODE *dir, SPADFNODE *file,
				       struct qstr *name)
{
	SPADFS *fs = dir->fs;
	struct fnode_block *fnode_block;
	struct fnode *fnode;
	struct buffer_head *bh;
	spadfs_ino_t ino;
	int r;

	if (likely(file != NULL)) {
		ino = file->spadfs_ino;
		if (unlikely(is_fnode_fixed(file))) {
			file = NULL;
			r = spadfs_lookup_ino(dir, name, &ino, 1);
			if (unlikely(r))
				goto lookup_error;
		}
	} else {
		/*
		 * This branch is taken if we encounter an error when creating
		 * a new file and we need to delete the directory entry.
		 */
		ino = 0;	/* against warning */
		r = spadfs_lookup_ino(dir, name, &ino, 0);
		if (unlikely(r))
			goto lookup_error;
	}

	fnode_block = spadfs_read_fnode_block(fs, spadfs_ino_t_sec(ino), &bh,
				SRFB_FNODE,
				"spadfs_remove_fnode_from_directory");
	if (unlikely(IS_ERR(fnode_block)))
		return PTR_ERR(fnode_block);

	fnode = (struct fnode *)((char *)fnode_block + spadfs_ino_t_pos(ino));
	start_concurrent_atomic_buffer_modify(fs, bh);
	if (CC_CURRENT(fs, &fnode->cc, &fnode->txc) &&
	    SPAD2CPU16_LV(&fnode->next) & FNODE_NEXT_FREE) {
		CPU2SPAD16_LV(&fnode->cc, 0);
		CPU2SPAD32_LV(&fnode->txc, 0);
	} else {
		if (likely(file != NULL) &&
		    CC_VALID(fs, &fnode->cc, &fnode->txc) ^
		    CC_CURRENT(fs, &fnode->cc, &fnode->txc))
			spadfs_swap_fnode(file, fnode);
		CPU2SPAD16_LV(&fnode->cc, fs->cc);
		CPU2SPAD32_LV(&fnode->txc, fs->txc);
		CPU2SPAD16_LV(&fnode->next, SPAD2CPU16_LV(&fnode->next) |
					    FNODE_NEXT_FREE);
	}
	do_fnode_block_checksum(fs, fnode_block);
	end_concurrent_atomic_buffer_modify(fs, bh);
	spadfs_brelse(fs, bh);
	return 0;

lookup_error:
	if (r > 0) {
		spadfs_error(fs, TXFLAGS_FS_ERROR,
			"can't find entry to delete in directory %Lx/%x",
			(unsigned long long)dir->fnode_block,
			dir->fnode_pos);
		r = -EFSERROR;
	}
	return r;
}

/*
 * Move the fnode "old_file" with name "old_name" from "old_dir" to
 * the directory "new_dir" under "new_name".
 *
 * If there was another fnode with "new_name" in "new_dir", "new_dentry" must
 * point to it --- it will be deleted.
 *
 * The filesystem must be locked for write.
 */
int spadfs_move_fnode_to_directory(SPADFNODE *old_dir, struct qstr *old_name,
				   SPADFNODE *old_file,
				   SPADFNODE *new_dir, struct qstr *new_name,
				   struct dentry *new_dentry,
				   u8 *new_ea, unsigned new_ea_size)
{
	SPADFS *fs = old_file->fs;
	sector_t fnode_address;
	unsigned fnode_off;
	struct fnode_block *fnode_block;
	struct fnode *fnode;
	struct buffer_head *bh;
	int r;
	int synced = 0;
	u16 hint_small, hint_big;
	struct inode *new_file = new_dentry ? new_dentry->d_inode : NULL;

	assert_write_sync_lock(fs);

	/*
	 * If this were after spadfs_add_fnode_to_directory, it would deadlock
	 */
	if (unlikely(S_ISDIR(inode(old_file)->i_mode)))
		spadfs_get_dir_hint(old_file, &hint_small, &hint_big);

	/*
	 * If moving over an existing directory, check that it's empty.
	 */
	if (unlikely(new_file != NULL)) {
		if (unlikely(S_ISDIR(new_file->i_mode))) {
			r = spadfs_check_directory_empty(spadfnode(new_file));
			if (unlikely(r))
				goto unlock_ret_r;
		}
	}

	/*
	 * Add the fnode to the new directory.
	 */
again:
	fnode = spadfs_add_fnode_to_directory(new_dir,
		(const char *)new_name->name, new_name->len,
		unlikely(is_fnode_fixed(old_file)) ? 0 :
			unlikely(new_ea != NULL) ? new_ea_size :
			old_file->ea_size,
		&bh, &fnode_address, &fnode_off, &fnode_block, 1);

	if (unlikely(IS_ERR(fnode))) {
		if (likely(fnode == ERR_PTR(-ENOSPC)) && !synced)
			goto do_sync;
		r = PTR_ERR(fnode);
		goto unlock_ret_r;
	}
	if (unlikely(fnode == NEED_SYNC)) {
do_sync:
		if (unlikely(r = spadfs_commit_unlocked(fs)))
			goto unlock_ret_r;
		synced = 1;
		goto again;
	}
	BUG_ON(fnode == NEED_WLOCK);

	/*
	 * Set the fnode attributes.
	 */
	fnode->namelen = new_name->len;
	spadfs_set_name(fs, FNODE_NAME(fnode), (const char *)new_name->name, new_name->len);

	if (unlikely(is_fnode_fixed(old_file))) {
		make_fixed_fnode_reference(fnode, old_file->fnode_block);
	} else {
		if (likely(!S_ISDIR(inode(old_file)->i_mode))) {
			set_spadfs_file(old_file, fnode, 1);
			fnode->flags = 0;
		} else {
			set_spadfs_directory(old_file, fnode, 1);
			fnode->run1n = SPAD2CPU16_LV(&hint_small);
			fnode->run2n = SPAD2CPU16_LV(&hint_big);
			fnode->flags = FNODE_FLAGS_DIR;
		}
		if (unlikely(new_ea != NULL)) {
			memcpy(old_file->ea, new_ea, new_ea_size);
			old_file->ea_size = new_ea_size;
			spadfs_find_ea_unx(old_file);
		}
		write_ea(old_file, fnode);
	}
	do_fnode_block_checksum(fs, fnode_block);
	end_concurrent_atomic_buffer_modify(fs, bh);
	spadfs_brelse(fs, bh);

	/*
	 * Remove the fnode from the old directory.
	 */
	spadfs_remove_fnode_from_directory(old_dir, old_file, old_name);
	if (likely(!is_fnode_fixed(old_file))) {
		spadfs_move_fnode_ptr(fs,
				old_file->fnode_block, old_file->fnode_pos,
				fnode_address, fnode_off,
				S_ISDIR(inode(old_file)->i_mode));
		spadfs_set_parent_fnode(old_file, new_dir->fnode_block, new_dir->fnode_pos);
		if (unlikely(old_file->fnode_block != fnode_address) ||
		    unlikely(old_file->fnode_pos != fnode_off) ||
		    unlikely(old_file->spadfs_ino !=
				make_spadfs_ino_t(fnode_address, fnode_off))) {
			panic("spadfs: spadfs_move_fnode_ptr didn't do its job: %Lx != %Lx || %x != %x || %Lx != %Lx",
				(unsigned long long)old_file->fnode_block,
				(unsigned long long)fnode_address,
				old_file->fnode_pos, fnode_off,
				(unsigned long long)old_file->spadfs_ino,
				(unsigned long long)make_spadfs_ino_t(fnode_address, fnode_off));
		}
	}

	/*
	 * If some other fnode with the same name exists in the new directory,
	 * delete it.
	 */
	if (unlikely(new_file != NULL)) {
		if (unlikely(S_ISDIR(new_file->i_mode)))
			spadfs_remove_recursive(spadfnode(new_file), spadfnode(new_file)->root, 0);
		spadfs_unlink_unlocked(new_dir, new_dentry);
	}

	r = 0;
unlock_ret_r:
	return r;
}

/*
 * Need to be called after change of length of extended attributes or change
 * of link count --- will reinsert fnode to directory and drop old instance.
 * Must be called with read lock on the filesystem and lock on file (if
 * "file" argument is a file) or with write lock on the filesystem.
 */

int spadfs_refile_fixed_fnode(SPADFNODE *file, u8 *new_ea, unsigned new_ea_size)
{
	SPADFS *fs = file->fs;
	struct fnode *fnode, *old_fnode;
	unsigned old_fnode_pos;
	struct buffer_head *bh;
	struct fixed_fnode_block *fixed_fnode_block;

	if (unlikely(is_deleted_file(file)))
		return 0;
	if (unlikely(!is_fnode_fixed(file))) {
		spadfs_error(fs, TXFLAGS_FS_ERROR,
			"fnode is not fixed: %Lx/%x",
			(unsigned long long)file->fnode_block,
			file->fnode_pos);
		return -EFSERROR;
	}

	fixed_fnode_block = (struct fixed_fnode_block *)
		spadfs_read_fnode_block(fs, file->fnode_block, &bh,
					SRFB_FIXED_FNODE,
					"spadfs_refile_fixed_fnode");
	if (unlikely(IS_ERR(fixed_fnode_block)))
		return PTR_ERR(fixed_fnode_block);

	start_concurrent_atomic_buffer_modify(fs, bh);
	old_fnode = (struct fnode *)
				((char *)fixed_fnode_block + file->fnode_pos);
	old_fnode_pos = file->fnode_pos;
	if (!CC_CURRENT(fs, &fixed_fnode_block->cc, &fixed_fnode_block->txc)) {
		CC_SET_CURRENT(fs, &fixed_fnode_block->cc,
				   &fixed_fnode_block->txc);
		file->fnode_pos = FIXED_FNODE_BLOCK_FNODE0 +
				FIXED_FNODE_BLOCK_FNODE1 - file->fnode_pos;
	}

	if (unlikely(new_ea != NULL)) {
		memcpy(file->ea, new_ea, new_ea_size);
		file->ea_size = new_ea_size;
		spadfs_find_ea_unx(file);
	}

	fnode = (struct fnode *)((char *)fixed_fnode_block + file->fnode_pos);
	CPU2SPAD64_LV(FIXED_FNODE_NLINK_PTR(fnode), file->spadfs_nlink);
	CPU2SPAD16_LV(&fnode->next, FNODE_SIZE(0, file->ea_size));
	/* We need to set current cc/txc because of possible resurrect */
	CPU2SPAD16_LV(&fnode->cc, fs->cc);
	CPU2SPAD32_LV(&fnode->txc, fs->txc);
	fnode->namelen = 0;
	if (likely(!S_ISDIR(inode(file)->i_mode))) {
		set_spadfs_file(file, fnode, 0);
		fnode->flags = 0;
	} else {
		set_spadfs_directory(file, fnode, 0);
		fnode->flags = FNODE_FLAGS_DIR;
		fnode->run1n = old_fnode->run1n; /* alloc hints */
		fnode->run2n = old_fnode->run2n;
		spadfs_move_parent_dir_ptr(fs, file->fnode_block, old_fnode_pos,
					   file->fnode_block, file->fnode_pos);
	}
	write_ea(file, fnode);
	do_fnode_block_checksum(fs, (struct fnode_block *)fixed_fnode_block);
	end_concurrent_atomic_buffer_modify(fs, bh);
	spadfs_brelse(fs, bh);
	return 0;
}

int spadfs_refile_fnode(SPADFNODE *dir, struct qstr *qstr, SPADFNODE *file,
			u8 *new_ea, unsigned new_ea_size)
{
	SPADFS *fs = file->fs;
	int r;

	assert_write_sync_lock(fs);

	if (unlikely(r = spadfs_ea_resize(file, new_ea_size)))
		return r;

	if (unlikely(is_deleted_file(file))) {
		memcpy(file->ea, new_ea, new_ea_size);
		file->ea_size = new_ea_size;
		spadfs_find_ea_unx(file);
		return 0;
	}

	if (is_fnode_fixed(file))
		r = spadfs_refile_fixed_fnode(file, new_ea, new_ea_size);
	else
		r = spadfs_move_fnode_to_directory(dir, qstr, file, dir, qstr,
						   NULL, new_ea, new_ea_size);
	return r;
}
