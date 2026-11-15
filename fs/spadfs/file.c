#include "spadfs.h"

#define SPADFS_MPAGE

#ifdef SPADFS_MPAGE
#include <linux/mpage.h>
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,15,0)
#include <linux/iomap.h>
#endif

#define BMAP_EXTEND	0
#define BMAP_MAP	1

static int file_make_memalloc(SPADFNODE *f, sector_t start, sector_t n_sectors)
{
	int r;
	sector_t d_off_sink;
	SPADFS *fs = f->fs;
	mutex_lock(&fs->alloc_lock);
	if (unlikely(spadfs_allocmem_find(fs, start, n_sectors, &d_off_sink))) {
		spadfs_error(fs, 0,
			"block range %Lx,%Lx does overlap with another memory-allocated block",
			(unsigned long long)start,
			(unsigned long long)n_sectors);
		r = -EFSERROR;
		goto unlock_ret;
	}
	r = spadfs_allocmem_add(fs, start, n_sectors);
	if (unlikely(r)) {
		printk(KERN_WARNING "spadfs: memory allocation failure, leaking disk space temporarily\n");
		r = 0;
		goto unlock_ret;
	}
	r = spadfs_free_blocks_unlocked(fs, start, n_sectors);
unlock_ret:
	mutex_unlock(&fs->alloc_lock);
	return r;
}

static int file_alloc_blocks(SPADFNODE *f, struct alloc *al)
{
	/*unsigned wanted = al->n_sectors;*/
	int r = spadfs_alloc_blocks(f->fs, al);
	/*if (!r) printk("wanted %x, allocated: %x\n", wanted, al->n_sectors);*/
	if (likely(!r) && unlikely(is_deleted_file(f)))
		r = file_make_memalloc(f, al->sector, al->n_sectors);
	return r;
}

static int file_free_blocks(SPADFNODE *f, sector_t start, sector_t n_sectors)
{
	int r;
	SPADFS *fs = f->fs;

	mutex_lock(&fs->alloc_lock);

	if (!is_deleted_file(f))
		r = spadfs_free_blocks_unlocked(fs, start, n_sectors);
	else {
		do {
			sector_t run_end;
			r = spadfs_allocmem_find(fs, start, 1, &run_end);
			if (!run_end || run_end > start + n_sectors)
				run_end = start + n_sectors;
			if (likely(r)) {
				spadfs_allocmem_delete(fs, start,
						       run_end - start);
			} else {
				r = spadfs_free_blocks_unlocked(fs, start,
							run_end - start);
				if (unlikely(r))
					goto unlock_ret_r;
			}
			n_sectors -= run_end - start;
			start = run_end;
		} while (n_sectors);
		r = 0;
	}

unlock_ret_r:
	mutex_unlock(&fs->alloc_lock);
	return r;
}

sector_t spadfs_size_2_sectors(SPADFS *fs, loff_t size)
{
	sector_t result;
	FILE_SECTORS(512U << fs->sectors_per_disk_block_bits, 512U << fs->sectors_per_cluster_bits,
		     fs->cluster_threshold, size, result);
	return result;
}

static void really_do_anode_checksum(struct anode *anode)
{
	anode->flags |= ANODE_CHECKSUM_VALID;
	anode->checksum ^= CHECKSUM_BASE ^ __byte_sum(anode, ANODE_SIZE);
}

static void do_anode_checksum(SPADFS *fs, struct anode *anode)
{
	if (unlikely(make_checksums(fs)))
		really_do_anode_checksum(anode);
	else
		anode->flags &= ~ANODE_CHECKSUM_VALID;
}

static struct anode *alloc_anode(SPADFNODE *f, sector_t hint,
				 struct buffer_head **bhp, sector_t *result)
{
	SPADFS *fs = f->fs;
	int r;
	struct alloc al;
	struct anode *anode;
	al.sector = hint;
	al.n_sectors = 1U << fs->sectors_per_disk_block_bits;
	al.extra_sectors = 0;
	al.flags = ALLOC_METADATA;
	al.reservation = NULL;
	r = file_alloc_blocks(f, &al);
	if (unlikely(r))
		return ERR_PTR(r);
	*result = al.sector;
	anode = spadfs_get_new_sector(fs, al.sector, bhp, "alloc_anode");
	if (unlikely(IS_ERR(anode)))
		return anode;
	memset(anode, 0, 512U << fs->sectors_per_buffer_bits);
	CPU2SPAD32_LV(&anode->magic, spadfs_magic(fs, al.sector, ANODE_MAGIC));
	return anode;
}

static void clear_extent_cache(SPADFNODE *f)
{
	unsigned i;
	if (likely(spadfs_unlocked_extent_cache)) {
		smp_wmb();
		WRITE_ONCE(f->extent_cache_seq, READ_ONCE(f->extent_cache_seq) + 1);
		smp_wmb();
	}
	for (i = 0; i < spadfs_extent_cache_size; i++)
		WRITE_ONCE(f->extent_cache[i].n_sectors, 0);
}

static int sync_bmap(SPADFNODE *f, sector_t lbn, sector_t *blk, sector_t *nblks,
		     sector_t *nback, int flags, sector_t *aptr,
		     const char *msg)
{
	int depth_now, depth_total;
	sector_t ano;
	sector_t nb;
	struct anode *anode;
	struct buffer_head *bh;
	unsigned vx;
	int direct, off;

	*aptr = 0;

	if (likely(lbn < f->blk1_n)) {
		*blk = f->blk1 + lbn;
		*nblks = f->blk1_n - lbn;
		*nback = lbn;
		if (unlikely(flags == BMAP_EXTEND)) {
			f->blk1_n = lbn + 1;
			f->blk2_n = 0;
			f->root = 0;
			clear_extent_cache(f);
		}
		return 0;
	}

	if (likely(lbn < f->blk1_n + f->blk2_n)) {
		*blk = f->blk2 + lbn - f->blk1_n;
		*nblks = f->blk1_n + f->blk2_n - lbn;
		*nback = lbn - f->blk1_n;
		if (unlikely(flags == BMAP_EXTEND)) {
			f->blk2_n = lbn - f->blk1_n + 1;
			f->root = 0;
			clear_extent_cache(f);
		}
		return 0;
	}

	ano = f->root;
	depth_now = 0;
	depth_total = 0;

subnode:
	anode = spadfs_read_anode(f->fs, ano, &bh, &vx, flags != BMAP_EXTEND,
				  msg);
	if (unlikely(IS_ERR(anode)))
		return PTR_ERR(anode);

	direct = find_direct(depth_now, depth_total);
	off = find_in_anode(anode, lbn, vx);
	if (unlikely(lbn >= SPAD2CPU64_LV(&anode->x[off].end_off)) ||
	    unlikely(lbn < SPAD2CPU64_LV(&anode->x[off - 1].end_off))) {
		spadfs_error(f->fs, TXFLAGS_FS_ERROR,
			"bmap(%s): out of range !(%Lx <= %Lx < %Lx), anode(%Lx -> %Lx)",
			msg,
			(unsigned long long)SPAD2CPU64_LV(&anode->x[off - 1].end_off),
			(unsigned long long)lbn,
			(unsigned long long)SPAD2CPU64_LV(&anode->x[off].end_off),
			(unsigned long long)f->root,
			(unsigned long long)ano);
		goto brelse_err;
	}
	if (unlikely(off >= direct)) {
		ano = SPAD2CPU64_LV(&anode->x[off].blk);
		if (unlikely(flags == BMAP_EXTEND)) {
			if (unlikely(anode->x[off].end_off !=
				     CPU2SPAD64((u64)-1)) ||
			    unlikely(anode->valid_extents != off + 1)) {
				start_concurrent_atomic_buffer_modify(f->fs,
								      bh);
				CPU2SPAD64_LV(&anode->x[off].end_off, (u64)-1);
				anode->valid_extents = off + 1;
				do_anode_checksum(f->fs, anode);
				clear_extent_cache(f);
				end_concurrent_atomic_buffer_modify(f->fs, bh);
			}
		}
		update_depth(&depth_now, &depth_total, off);
		if (flags != BMAP_EXTEND)
			end_concurrent_atomic_buffer_read(f->fs, bh);
		spadfs_brelse(f->fs, bh);
		goto subnode;
	}

	if (unlikely(!off) || (unlikely(off == 2) && unlikely(!depth_now)))
		*aptr = ano;

	if (unlikely(SPAD2CPU64_LV(&anode->x[off].end_off) <=
		     SPAD2CPU64_LV(&anode->x[off - 1].end_off))) {
		spadfs_error(f->fs, TXFLAGS_FS_ERROR,
			"bmap(%s): non-monotonic anode (%Lx -> %Lx), entry %d",
			msg,
			(unsigned long long)f->root,
			(unsigned long long)ano,
			off);
		goto brelse_err;
	}

	nb = lbn - SPAD2CPU64_LV(&anode->x[off - 1].end_off);
	*nback = nb;
	*blk = SPAD2CPU64_LV(&anode->x[off].blk) + nb;
	*nblks = SPAD2CPU64_LV(&anode->x[off].end_off) - lbn;
	if (flags == BMAP_EXTEND) {
		if (unlikely(SPAD2CPU64_LV(&anode->x[off].end_off) != lbn + 1)
		 || unlikely(anode->valid_extents != off + 1)) {
			start_concurrent_atomic_buffer_modify(f->fs, bh);
			CPU2SPAD64_LV(&anode->x[off].end_off, lbn + 1);
			anode->valid_extents = off + 1;
			do_anode_checksum(f->fs, anode);
			clear_extent_cache(f);
			end_concurrent_atomic_buffer_modify(f->fs, bh);
		}
	}
	if (flags != BMAP_EXTEND)
		end_concurrent_atomic_buffer_read(f->fs, bh);
	spadfs_brelse(f->fs, bh);
	return 0;

brelse_err:
	if (flags != BMAP_EXTEND)
		end_concurrent_atomic_buffer_read(f->fs, bh);
	spadfs_brelse(f->fs, bh);
	return -EFSERROR;
}

static int spadfs_add_extent(SPADFNODE *f, sector_t blk, sector_t n_blks)
{
	SPADFS *fs = f->fs;
	struct anode *anode;
	sector_t ano;
	int depth_now, depth_total;
	int direct;
	sector_t ano_l;
	sector_t end_off;
	struct buffer_head *bh;
	unsigned vx;

	if (!f->blk1_n) {
		unsigned mdb;
		f->blk1 = blk;
		mdb = MAX_DIRECT_BLKS(1U << fs->sectors_per_disk_block_bits);
		if (unlikely(n_blks > mdb)) {
			f->blk1_n = mdb;
			blk += mdb;
			n_blks -= mdb;
			goto again1;
		}
		f->blk1_n = n_blks;
		return 0;
	}

	if (!f->blk2_n) {
		unsigned mdb;
again1:
		f->blk2 = blk;
		mdb = MAX_DIRECT_BLKS(1U << fs->sectors_per_disk_block_bits);
		if (unlikely(n_blks > mdb)) {
			f->blk2_n = mdb;
			blk += mdb;
			n_blks -= mdb;
			goto again2;
		}
		f->blk2_n = n_blks;
		return 0;
	}

	if (!f->root) {
again2:
		anode = alloc_anode(f, spadfs_alloc_hint(f, HINT_META), &bh,
				    &f->root);
		if (unlikely(IS_ERR(anode)))
			return PTR_ERR(anode);

		anode->flags |= ANODE_ROOT;
		anode->valid_extents = 3;
		CPU2SPAD64_LV(&anode->start_off, 0);
		CPU2SPAD64_LV(&anode->x[0].blk, f->blk1);
		CPU2SPAD64_LV(&anode->x[0].end_off, f->blk1_n);
		CPU2SPAD64_LV(&anode->x[1].blk, f->blk2);
		CPU2SPAD64_LV(&anode->x[1].end_off, f->blk1_n + f->blk2_n);
		CPU2SPAD64_LV(&anode->x[2].blk, blk);
		CPU2SPAD64_LV(&anode->x[2].end_off, f->blk1_n + f->blk2_n + n_blks);
		do_anode_checksum(fs, anode);
		mark_buffer_dirty(bh);
		spadfs_brelse(fs, bh);
		return 0;
	}

	ano = f->root;
	ano_l = -1;
	depth_now = depth_total = 0;

subnode:
	anode = spadfs_read_anode(fs, ano, &bh, &vx, 0, "spadfs_add_extent 1");
	if (unlikely(IS_ERR(anode)))
		return PTR_ERR(anode);

	direct = find_direct(depth_now, depth_total);
	if (likely(vx < direct)) {
		start_concurrent_atomic_buffer_modify(fs, bh);
		CPU2SPAD64_LV(&anode->x[vx].blk, blk);
		CPU2SPAD64_LV(&anode->x[vx].end_off,
		    SPAD2CPU64_LV(&anode->x[anode->valid_extents - 1].end_off) +
		    n_blks);
		anode->valid_extents = vx + 1;
		do_anode_checksum(fs, anode);
		end_concurrent_atomic_buffer_modify(fs, bh);
		spadfs_brelse(fs, bh);
		return 0;
	}
	if (vx < ANODE_N_EXTENTS)
		ano_l = ano;
	if (vx > direct) {
		ano = SPAD2CPU64_LV(&anode->x[vx - 1].blk);
		update_depth(&depth_now, &depth_total, vx - 1);
		spadfs_brelse(fs, bh);
		goto subnode;
	}
	if (unlikely(ano_l == -1)) {
		spadfs_brelse(fs, bh);
		spadfs_error(fs, TXFLAGS_FS_ERROR,
			"anode %Lx is full",
			(unsigned long long)f->root);
		return -EFSERROR;
	}

	end_off = SPAD2CPU64_LV(&anode->x[vx - 1].end_off);
	spadfs_brelse(fs, bh);

	anode = alloc_anode(f, spadfs_alloc_hint(f, HINT_META), &bh, &ano);
	if (unlikely(IS_ERR(anode)))
		return PTR_ERR(anode);

	anode->valid_extents = 1;
	CPU2SPAD64_LV(&anode->start_off, end_off);
	CPU2SPAD64_LV(&anode->x[0].blk, blk);
	CPU2SPAD64_LV(&anode->x[0].end_off, end_off + n_blks);
	do_anode_checksum(fs, anode);
	mark_buffer_dirty(bh);
	spadfs_brelse(fs, bh);
	anode = spadfs_read_anode(fs, ano_l, &bh, &vx, 0,
				  "spadfs_add_extent 2");
	if (unlikely(IS_ERR(anode)))
		return PTR_ERR(anode);
	if (unlikely(vx == ANODE_N_EXTENTS)) {
		spadfs_error(fs, TXFLAGS_FS_ERROR, "anode %Lx filled under us",
			(unsigned long long)ano_l);
		spadfs_brelse(fs, bh);
		return -EFSERROR;
	}
	start_concurrent_atomic_buffer_modify(fs, bh);
	CPU2SPAD64_LV(&anode->x[vx].blk, ano);
	CPU2SPAD64_LV(&anode->x[vx].end_off, (u64)-1);
	CPU2SPAD64_LV(&anode->x[vx - 1].end_off, end_off);
	anode->valid_extents = vx + 1;
	do_anode_checksum(fs, anode);
	end_concurrent_atomic_buffer_modify(fs, bh);
	spadfs_brelse(fs, bh);
	return 0;
}

static int spadfs_extend_last_extent(SPADFNODE *f, sector_t n_blks)
{
	SPADFS *fs = f->fs;
	struct anode *anode;
	sector_t ano;
	int depth_now, depth_total;
	int direct;
	struct buffer_head *bh;
	unsigned vx;

	if (!f->blk2_n) {
		unsigned mdb = MAX_DIRECT_BLKS(1U << fs->sectors_per_disk_block_bits);
		if (f->blk1_n + n_blks > mdb) {
			n_blks -= mdb - f->blk1_n;
			f->blk1_n = mdb;
			return spadfs_add_extent(f, f->blk1 + mdb, n_blks);
		}
		f->blk1_n += n_blks;
		return 0;
	}

	if (!f->root) {
		unsigned mdb = MAX_DIRECT_BLKS(1U << fs->sectors_per_disk_block_bits);
		if (f->blk2_n + n_blks > mdb) {
			n_blks -= mdb - f->blk2_n;
			f->blk2_n = mdb;
			return spadfs_add_extent(f, f->blk2 + mdb, n_blks);
		}
		f->blk2_n += n_blks;
		return 0;
	}
	ano = f->root;
	depth_now = depth_total = 0;

subnode:
	anode = spadfs_read_anode(fs, ano, &bh, &vx, 0,
				  "spadfs_extend_last_extent");
	if (unlikely(IS_ERR(anode)))
		return PTR_ERR(anode);

	direct = find_direct(depth_now, depth_total);
	if (vx <= direct) {
		start_concurrent_atomic_buffer_modify(fs, bh);
		CPU2SPAD64_LV(&anode->x[vx - 1].end_off, SPAD2CPU64_LV(&anode->x[vx - 1].end_off) + n_blks);
		do_anode_checksum(fs, anode);
		end_concurrent_atomic_buffer_modify(fs, bh);
		spadfs_brelse(fs, bh);
		return 0;
	}

	ano = SPAD2CPU64_LV(&anode->x[vx - 1].blk);
	update_depth(&depth_now, &depth_total, vx - 1);
	spadfs_brelse(fs, bh);
	goto subnode;
}

static int spadfs_do_truncate_blocks(SPADFNODE *f, sector_t blks, sector_t oldblks)
{
	SPADFS *fs = f->fs;
	sector_t d_off_sink;	/* write-only */
	int r;
	sector_t blk, bback, ano;

	spadfs_discard_reservation(fs, &f->res);

	if (!oldblks)
		return 0;

again:
	r = sync_bmap(f, blks - 1, &blk, &d_off_sink, &bback, BMAP_MAP, &ano,
		      "spadfs_do_truncate_blocks");
	if (unlikely(r))
		return r;

	blk++;
	bback++;
	if (oldblks >= bback && unlikely(ano != 0)) {
		r = file_free_blocks(f, ano, 1U << fs->sectors_per_disk_block_bits);
		if (unlikely(r))
			return r;
	}

	if (oldblks < bback)
		bback = oldblks;
	r = file_free_blocks(f, blk - bback, bback);
	if (unlikely(r))
		return r;

	blks -= bback;
	if ((oldblks -= bback))
		goto again;
	return 0;
}

void spadfs_create_memory_extents(SPADFNODE *f)
{
	SPADFS *fs = f->fs;
	sector_t lbn = 0;
	sector_t total_lbns = spadfs_size_2_sectors(fs, f->disk_size);
	sector_t d_off_sink;
	sector_t blk, blks, ano;
	int r;

	while (lbn < total_lbns) {
		r = sync_bmap(f, lbn, &blk, &blks, &d_off_sink, BMAP_MAP, &ano,
			      "spadfs_create_memory_extents");
		if (unlikely(r))
			return;
		if (blks > total_lbns - lbn)
			blks = total_lbns - lbn;
		r = file_make_memalloc(f, blk, blks);
		if (unlikely(r))
			return;
		if (unlikely(ano != 0)) {
			r = file_make_memalloc(f, ano, 1U << fs->sectors_per_disk_block_bits);
			if (unlikely(r))
				return;
		}
		lbn += blks;
	}
}

static void spadfs_set_disk_size(SPADFNODE *f, loff_t newsize)
{
	if (f->commit_sequence != f->fs->commit_sequence) {
		f->crash_disk_size = f->disk_size;
		f->commit_sequence = f->fs->commit_sequence;
	}
	f->disk_size = newsize;
}

static int spadfs_do_truncate(SPADFNODE *f, loff_t newsize)
{
	SPADFS *fs = f->fs;
	sector_t blks, oldblks, nsb;
	blks = spadfs_size_2_sectors(fs, f->disk_size);
	nsb = spadfs_size_2_sectors(fs, newsize);
	oldblks = blks - nsb;
#ifdef SPADFS_QUOTA
	dquot_free_space_nodirty(inode(f), (loff_t)oldblks << 9);
#else
	inode_sub_bytes(inode(f), (loff_t)oldblks << 9);
#endif
	spadfs_set_disk_size(f, spadfs_roundup_blocksize(fs, newsize));
	return spadfs_do_truncate_blocks(f, blks, oldblks);
}

static int spadfs_do_extend(SPADFNODE *f, loff_t size, unsigned extra_sectors, sector_t resurrect_blks)
{
	SPADFS *fs = f->fs;
	struct alloc al;
	sector_t d_off_sink;	/* write-only */
	sector_t blks, newblks, blks_to_do;
	sector_t blk;
	int flags;
	int r;

	BUG_ON((unsigned long)size & ((512U << fs->sectors_per_disk_block_bits) - 1));

	blks = spadfs_size_2_sectors(fs, f->disk_size);
	newblks = spadfs_size_2_sectors(fs, size) - blks;
	blks_to_do = newblks;

#ifdef SPADFS_QUOTA
	r = dquot_alloc_space_nodirty(inode(f), (loff_t)newblks << 9);
	if (unlikely(r))
		goto ret_r;
#else
	inode_add_bytes(inode(f), (loff_t)newblks << 9);
#endif

#ifdef SPADFS_RESURRECT
	while (unlikely(resurrect_blks != 0) && blks_to_do) {
		sector_t nfwd, nback, ano;
		r = sync_bmap(f, blks, &blk, &nfwd, &nback,
			      BMAP_MAP, &ano, "spadfs_do_extend (resurrect)");
		if (unlikely(r))
			goto unaccount_ret_r;

		if (nfwd > resurrect_blks)
			nfwd = resurrect_blks;
		if (nfwd > blks_to_do)
			nfwd = blks_to_do;

		if (unlikely(!nback) && ano != 0) {
			al.sector = ano;
			al.n_sectors = 1U << fs->sectors_per_disk_block_bits;
			al.extra_sectors = 0;
			al.flags = ALLOC_RESURRECT;
			al.reservation = NULL;
			r = spadfs_alloc_blocks(fs, &al);
			if (unlikely(r))
				goto unaccount_ret_r;
		}
		al.sector = blk;
		al.n_sectors = nfwd;
		al.extra_sectors = 0;
		al.flags = ALLOC_RESURRECT;
		al.reservation = NULL;
		r = spadfs_alloc_blocks(fs, &al);
		if (unlikely(r))
			goto unaccount_ret_r;

		resurrect_blks -= al.n_sectors;
		blks_to_do -= al.n_sectors;
		blks += al.n_sectors;
	}
#endif

	if (!blks_to_do)
		goto set_size_return;

	flags = size + (512U << fs->sectors_per_disk_block_bits) > fs->cluster_threshold ?
		ALLOC_BIG_FILE : ALLOC_SMALL_FILE;

	/*printk("size %Lx, disk_size %Lx, blks %Lx\n", (unsigned long long)size, (unsigned long long)f->disk_size, (unsigned long long)0);*/

	if (blks) {
		r = sync_bmap(f, blks - 1, &blk, &d_off_sink, &d_off_sink,
			      BMAP_EXTEND, &d_off_sink, "spadfs_do_extend");
		if (unlikely(r))
			goto unaccount_ret_r;
		blk++;
	} else {
		clear_extent_cache(f);
		blk = 0;
	}

	if (blks &&
	      (flags == ALLOC_SMALL_FILE ||
	      likely(f->disk_size > fs->cluster_threshold))) {
new_run:
		al.n_sectors = blks_to_do;
		al.extra_sectors = extra_sectors;
		al.flags = flags | ALLOC_PARTIAL_AT_GOAL;
alloc_c:
		al.reservation = &f->res;
		al.sector = blk;
		/*printk("cnt: bl %Lx, hi %Lx, ns %x, fl %x", (unsigned long long)blks, (unsigned long long)al.sector, al.n_sectors, al.flags);*/
		r = file_alloc_blocks(f, &al);
		if (unlikely(r)) {
			/*printk(" -> error %d\n", r);*/
			goto truncate_ret_r;
		}
		/*printk(" -> %Lx, %x\n", (unsigned long long)al.sector, al.n_sectors);*/
	} else {
		if (blks) {
new_extent:
			if ((unsigned long)blks &
			    ((1U << fs->sectors_per_cluster_bits) - 1)) {
				flags = 0;
				al.flags = ALLOC_SMALL_FILE | ALLOC_PARTIAL_AT_GOAL;
				al.n_sectors =
					(1U << fs->sectors_per_cluster_bits) -
					((unsigned long)blks &
					((1U << fs->sectors_per_cluster_bits) -
						1));
				if (al.n_sectors > blks_to_do)
					al.n_sectors = blks_to_do;
				al.extra_sectors = 0;
				goto alloc_c;
			}
			if (blk >= (sector_t)fs->zones[2].grp_start << fs->sectors_per_group_bits)
				goto new_run;
		} else {
			f->blk1_n = 0;
			f->blk2_n = 0;
			f->root = 0;
		}

		al.sector = spadfs_alloc_hint(f,
				flags & ALLOC_BIG_FILE ? HINT_BIG : HINT_SMALL);
		al.n_sectors = blks_to_do;
		al.extra_sectors = extra_sectors;
		al.flags = flags;
		al.reservation = &f->res;
		/*printk("new: bl %Lx, hi %Lx, ns %x, fl %x", (unsigned long long)blks, (unsigned long long)al.sector, al.n_sectors, al.flags);*/
		r = file_alloc_blocks(f, &al);
		if (unlikely(r)) {
			/*printk(" -> error %d\n", r);*/
			goto truncate_ret_r;
		}
		/*printk(" -> %Lx, %x\n", (unsigned long long)al.sector, al.n_sectors);*/

		if (unlikely(al.flags & ALLOC_NEW_GROUP_HINT))
			spadfs_set_new_hint(f, &al);
	}

	if (likely(al.sector == blk))
		r = spadfs_extend_last_extent(f, al.n_sectors);
	else
		r = spadfs_add_extent(f, al.sector, al.n_sectors);

	if (unlikely(r)) {
		int rr = file_free_blocks(f, al.sector, al.n_sectors);
		if (unlikely(rr))
			r = rr;
		goto truncate_ret_r;
	}

	blks_to_do -= al.n_sectors;
	blks += al.n_sectors;

	if (unlikely(blks_to_do != 0)) {
		blk = al.sector + al.n_sectors;
		if (unlikely(!flags)) {
			flags = ALLOC_BIG_FILE;
			goto new_extent;
		} else
			goto new_run;
	}

set_size_return:
	smp_wmb();
	spadfs_set_disk_size(f, size);
	return 0;

truncate_ret_r:
	spadfs_do_truncate_blocks(f, blks,
				blks - spadfs_size_2_sectors(fs, f->disk_size));

unaccount_ret_r:
#ifdef SPADFS_QUOTA
	dquot_free_space_nodirty(inode(f), (loff_t)newblks << 9);
#else
	inode_sub_bytes(inode(f), (loff_t)newblks << 9);
#endif

#ifdef SPADFS_QUOTA
ret_r:
#endif
	return r;
}

static void spadfs_get_blocks_to_resurrect(SPADFNODE *fn, sector_t *result)
{
	SPADFS *fs = fn->fs;
	*result = 0;
	if (fn->commit_sequence == fs->commit_sequence &&
	    unlikely(fn->crash_disk_size > fn->disk_size)) {
		sector_t dblks = spadfs_size_2_sectors(fs, fn->crash_disk_size);
		sector_t sblks = spadfs_size_2_sectors(fs, fn->disk_size);
		if (unlikely(dblks > sblks))
			*result = dblks - sblks;
	}
}

static noinline int spadfs_extend_file(struct inode *i, sector_t target_blocks, int target_blocks_exact)
{
	sync_lock_decl
	SPADFS *fs = spadfnode(i)->fs;
	int synced = 0;
	loff_t needed_size;
	unsigned long long long_prealloc;
	unsigned prealloc;
	unsigned ts;
	int r;
	sector_t resurrect_blks;

retry:
	down_read_sync_lock(fs);
	mutex_lock(&spadfnode(i)->file_lock);

	spadfs_get_blocks_to_resurrect(spadfnode(i), &resurrect_blks);

#ifndef SPADFS_RESURRECT
	if (unlikely(resurrect_blks != 0))
		goto unlock_commit;
#endif

	needed_size = spadfnode(i)->disk_size + (512U << fs->sectors_per_disk_block_bits);

	long_prealloc = (((unsigned long long)target_blocks) << (fs->sectors_per_disk_block_bits + 9)) - needed_size;
	if (unlikely((long long)long_prealloc < 0))
		long_prealloc = 0;
	ts = 0;
	if (spadfnode(i)->disk_size >= fs->cluster_threshold && likely(!target_blocks_exact)) {
		uint64_t ts64;
		uint64_t ds = spadfnode(i)->disk_size;
		if (likely(fs->prealloc_part_bits >= 0))
			ds >>= fs->prealloc_part_bits;
		else
			do_div(ds, fs->prealloc_part);
		if (ds < fs->min_prealloc)
			ds = fs->min_prealloc;
		ts64 = ds >> 9;
		if (unlikely(ts64 >= 0x100000000ULL))
			ts = -1U;
		else
			ts = ts64;
		if (ds > fs->max_prealloc)
			ds = fs->max_prealloc;
		if (long_prealloc < (unsigned)ds)
			long_prealloc = (unsigned)ds;
	}

	/*{
		unsigned long long max_allocation;
		max_allocation = (unsigned long long)READ_ONCE(fs->max_allocation) * 512;
		if (likely(max_allocation >= 512U << fs->sectors_per_disk_block_bits))
			max_allocation -= 512U << fs->sectors_per_disk_block_bits;
		if (unlikely(long_prealloc > max_allocation))
			long_prealloc = max_allocation;
	}*/

	{
		long long freespace_limit = (unsigned long long)READ_ONCE(fs->freespace) * 512 - ((512U << fs->sectors_per_disk_block_bits) + (512U << fs->sectors_per_cluster_bits));
		if (unlikely((long long)long_prealloc > freespace_limit)) {
			if (freespace_limit < 0)
				long_prealloc = 0;
			else
				long_prealloc = freespace_limit;
		}
	}

	if (unlikely(long_prealloc > (u32)-(1024U << fs->sectors_per_disk_block_bits))) {
		unsigned long long remaining = long_prealloc - (u32)-(1024U << fs->sectors_per_disk_block_bits);
		remaining = (remaining + (512U << fs->sectors_per_disk_block_bits) - 1) >> 9;
		if (remaining >= 0x100000000ULL)
			ts = -1U;
		else if ((unsigned)remaining > ts)
			ts = remaining;
		prealloc = (u32)-(1024U << fs->sectors_per_disk_block_bits);
	} else {
		prealloc = long_prealloc;
	}
	prealloc &= ~((512U << fs->sectors_per_disk_block_bits) - 1);

	if (unlikely(!spadfnode(i)->ea_unx)) {
		prealloc >>= 9;
		if (likely(prealloc + ts >= ts))
			ts += prealloc;
		else
			ts = -1U;
		prealloc = 0;
	}
	ts &= -(1U << fs->sectors_per_disk_block_bits);

again_without_prealloc:
	/*printk("prealloc: %llx + %x = %llx\n", needed_size, prealloc, needed_size + prealloc);*/
	r = spadfs_do_extend(spadfnode(i), needed_size + prealloc, ts, resurrect_blks);
	if (unlikely(r)) {
		if (likely(r == -ENOSPC)) {
			if (prealloc && target_blocks_exact != 2) {
				prealloc = 0;
				goto again_without_prealloc;
			}
			if (!synced)
				goto unlock_commit;
		}
		goto unlock_return_r;
	}

	spadfs_write_file(spadfnode(i), 0, NULL, NULL);

	r = 0;

	if (likely(target_blocks_exact != 2)) {
		if (unlikely(READ_ONCE(spadfnode(i)->dont_truncate_prealloc)))
			spadfnode(i)->dont_truncate_prealloc = 0;
	} else {
		spadfnode(i)->dont_truncate_prealloc = 1;
	}

unlock_return_r:
	mutex_unlock(&spadfnode(i)->file_lock);
	up_read_sync_lock(fs);
	return r;

unlock_commit:
	mutex_unlock(&spadfnode(i)->file_lock);
	up_read_sync_lock(fs);

	if (unlikely(sb_rdonly(fs->s)))
		return -EROFS;

	r = spadfs_commit(fs);
	if (unlikely(r))
		return r;

	synced = 1;
	goto retry;
}

static int spadfs_inode_needs_clear(SPADFS *fs, struct inode *i)
{
	return (spadfnode(i)->clear_position & ((1U << fs->sectors_per_disk_block_bits) - 1)) != 0;
}

static noinline void spadfs_add_inode_to_clear_list(struct inode *i)
{
	SPADFS *fs = spadfnode(i)->fs;
	spadfnode(i)->clear_position = (sector_t)((spadfnode(i)->mmu_private + (512U << fs->sectors_per_buffer_bits) - 1) >> (fs->sectors_per_buffer_bits + 9)) << fs->sectors_per_buffer_bits;
	if (unlikely(list_empty(&spadfnode(i)->clear_entry)) && spadfs_inode_needs_clear(fs, i)) {
		spin_lock(&fs->clear_lock);
		list_add(&spadfnode(i)->clear_entry, &fs->clear_list);
		spin_unlock(&fs->clear_lock);
	}
}

void spadfs_clear_last_block(struct inode *i)
{
	SPADFNODE *f = spadfnode(i);
	SPADFS *fs = f->fs;
	spin_lock(&fs->clear_lock);
	list_del_init(&f->clear_entry);
	spin_unlock(&fs->clear_lock);
	if (spadfs_inode_needs_clear(fs, i)) {
		sector_t result, d_off_sink;
		int r;
		r = sync_bmap(f, f->clear_position, &result, &d_off_sink, &d_off_sink, BMAP_MAP, &d_off_sink, "spadfs_clear_last_block");
		if (unlikely(r))
			return;
		do {
			struct buffer_head *bh;
			void *data = spadfs_get_new_sector(fs, result, &bh, "spadfs_clear_last_block");
			if (unlikely(IS_ERR(data)))
				break;
			memset(data, 0, 512U << fs->sectors_per_buffer_bits);
			mark_buffer_dirty(bh);
			spadfs_brelse(fs, bh);
			result += 1U << fs->sectors_per_buffer_bits;
		} while (result & ((1U << fs->sectors_per_disk_block_bits) - 1));
	}
}

static inline void spadfs_test_and_clear_last_block(struct inode *i)
{
	if (unlikely(!list_empty(&spadfnode(i)->clear_entry)))
		spadfs_clear_last_block(i);
}

static int spadfs_get_block(struct inode *i, sector_t lblock, struct buffer_head *bh_result, int create)
{
	sector_t n_fwd, n_back;
	sector_t d_off_sink;	/* write-only */
	int r;
	sector_t result;
	u64 extent_cache_seq;
	struct extent_cache *x;
	SPADFS *fs = spadfnode(i)->fs;
	sector_t blk = (sector_t)lblock << fs->sectors_per_buffer_bits;

	if (unlikely(create)) {
		if (likely(blk >= (spadfnode(i)->mmu_private + 511) >> 9)) {
			set_buffer_new(bh_result);
			BUG_ON((loff_t)blk << 9 != spadfnode(i)->mmu_private);
			BUG_ON(spadfnode(i)->mmu_private > spadfnode(i)->disk_size);
			if (unlikely(spadfnode(i)->mmu_private == spadfnode(i)->disk_size)) {
				r = spadfs_extend_file(i, READ_ONCE(spadfnode(i)->target_blocks), READ_ONCE(spadfnode(i)->target_blocks_exact));
				if (unlikely(r))
					return r;
			}
			spadfnode(i)->mmu_private += 512U << fs->sectors_per_buffer_bits;
			if (unlikely(fs->sectors_per_buffer_bits != fs->sectors_per_disk_block_bits)) {
				sync_lock_decl
				down_read_sync_lock(fs);
				spadfs_add_inode_to_clear_list(i);
				up_read_sync_lock(fs);
			}
		}
	}

	if (likely(spadfs_unlocked_extent_cache)) {
		preempt_disable();
		x = &spadfnode(i)->extent_cache[smp_processor_id()];
	} else {
		mutex_lock(&spadfnode(i)->file_lock);
		x = &spadfnode(i)->extent_cache[0];
	}

	if (likely(blk >= READ_ONCE(x->logical_sector)) &&
	    likely(blk < READ_ONCE(x->logical_sector) + READ_ONCE(x->n_sectors))) {
		sector_t o = blk - READ_ONCE(x->logical_sector);
		n_fwd = READ_ONCE(x->n_sectors) - o;
		result = READ_ONCE(x->physical_sector) + o;
		goto preempt_en_ret_result;
	}

	extent_cache_seq = 0;	/* against warning */
	if (likely(spadfs_unlocked_extent_cache)) {
		preempt_enable();
		extent_cache_seq = READ_ONCE(spadfnode(i)->extent_cache_seq);
		smp_mb();
	}

	r = sync_bmap(spadfnode(i), blk, &result, &n_fwd, &n_back, BMAP_MAP, &d_off_sink, "spadfs_get_block");

	if (unlikely(r)) {
		if (unlikely(!spadfs_unlocked_extent_cache))
			mutex_unlock(&spadfnode(i)->file_lock);
		return r;
	}

	if (likely(spadfs_unlocked_extent_cache)) {
		preempt_disable();
		x = &spadfnode(i)->extent_cache[smp_processor_id()];
	}

	WRITE_ONCE(x->physical_sector, result - n_back);
	WRITE_ONCE(x->logical_sector, blk - n_back);
		/* truncate it from sector_t to unsigned long */
	WRITE_ONCE(x->n_sectors, n_back + n_fwd);

	if (likely(spadfs_unlocked_extent_cache)) {
		smp_mb();
		if (unlikely(READ_ONCE(spadfnode(i)->extent_cache_seq) != extent_cache_seq))
			WRITE_ONCE(x->n_sectors, 0);
	}

preempt_en_ret_result:
	if (likely(spadfs_unlocked_extent_cache))
		preempt_enable();
	else
		mutex_unlock(&spadfnode(i)->file_lock);

	if (bh_result->b_size >> 9 < n_fwd)
		n_fwd = bh_result->b_size >> 9;
	map_bh(bh_result, i->i_sb, result >> fs->sectors_per_buffer_bits);
	bh_result->b_size = (size_t)n_fwd << 9;
	return 0;
}

#ifdef SPADFS_DIRECT_IO

/* Direct I/O sometimes sends requests beyond file end */

static int spadfs_get_block_direct(struct inode *i, sector_t lblock, struct buffer_head *bh_result, int create)
{
	int r;
	sector_t blk, total_blocks;
	SPADFS *fs = spadfnode(i)->fs;

	BUG_ON(!inode_is_locked(i));

	blk = (sector_t)lblock << fs->sectors_per_buffer_bits;
	if (!create) {
		if (unlikely(blk >= (spadfnode(i)->mmu_private + 511) >> 9))
			return 0;
	} else {
		if (unlikely(blk > spadfnode(i)->mmu_private >> 9))
			return 0;
	}

	r = spadfs_get_block(i, lblock, bh_result, create);
	if (unlikely(r) || unlikely(!buffer_mapped(bh_result)))
		return r;

	lblock += bh_result->b_size >> (9 + fs->sectors_per_buffer_bits);
	total_blocks = (spadfnode(i)->mmu_private + ((512U << fs->sectors_per_buffer_bits) - 1)) >> (9 + fs->sectors_per_buffer_bits);
	if (unlikely(lblock > total_blocks))
		bh_result->b_size -= (lblock - total_blocks) << (9 + fs->sectors_per_buffer_bits);

	return 0;
}

#endif

static void spadfs_discard_prealloc(SPADFNODE *f)
{
	loff_t inode_size = i_size_read(inode(f));
	if (inode_size + (512U << f->fs->sectors_per_disk_block_bits) <= f->disk_size)
		spadfs_do_truncate(f, inode_size);
	spadfs_write_file(f, 0, NULL, NULL);
}

static void set_target_size(SPADFNODE *f, loff_t size, int exact)
{
	if (size < i_size_read(inode(f)))
		return;
	WRITE_ONCE(f->target_blocks, ((unsigned long long)size + ((512U << f->fs->sectors_per_disk_block_bits) - 1)) >> (f->fs->sectors_per_disk_block_bits + 9));
	WRITE_ONCE(f->target_blocks_exact, exact);
}

static void spadfs_truncate_unlocked(struct inode *i)
{
	loff_t newsize = i->i_size;

	set_target_size(spadfnode(i), newsize, 1);

	block_truncate_page(i->i_mapping, newsize, spadfs_get_block);

	BUG_ON(newsize > spadfnode(i)->mmu_private);
	BUG_ON(newsize > spadfnode(i)->disk_size);
	spadfnode(i)->mmu_private = newsize;
	spadfs_do_truncate(spadfnode(i), newsize);
	spadfs_write_file(spadfnode(i), 0, NULL, NULL);

	if (unlikely(spadfnode(i)->fs->sectors_per_buffer_bits != spadfnode(i)->fs->sectors_per_disk_block_bits)) {
		spadfs_add_inode_to_clear_list(i);
		spadfs_test_and_clear_last_block(i);
	}
}

void spadfs_truncate(struct inode *i)
{
	sync_lock_decl
	SPADFS *fs = spadfnode(i)->fs;

	down_read_sync_lock(fs);
	mutex_lock(&spadfnode(i)->file_lock);

	spadfs_truncate_unlocked(i);

	mutex_unlock(&spadfnode(i)->file_lock);
	up_read_sync_lock(fs);
}

static int spadfs_release(struct inode *i, struct file *file)
{
	if (unlikely(file->f_mode & FMODE_WRITE)) {
		sync_lock_decl
		SPADFS *fs = spadfnode(i)->fs;

		spadfs_discard_reservation(fs, &spadfnode(i)->res);

		if (!inode_trylock(i)) {
			/*
			 * This can only happen if the file is still open.
			 * In this case, don't cleanup prealloc.
			 * This mutex_trylock prevents a deadlock in sys_swapon
			 * when the swapfile is invalid.
			 *
			 * The last block must be always cleared, so we take
			 * filesystem sync lock if we couldn't take inode lock.
			 */

			if (unlikely(!list_empty(&spadfnode(i)->clear_entry))) {
				down_write_sync_lock(fs);
				spadfs_test_and_clear_last_block(i);
				up_write_sync_lock(fs);
			}

			goto dont_cleanup;
		}
		down_read_sync_lock(fs);
		mutex_lock(&spadfnode(i)->file_lock);

		if (likely(!spadfnode(i)->dont_truncate_prealloc))
			spadfs_discard_prealloc(spadfnode(i));

		spadfs_test_and_clear_last_block(i);

		mutex_unlock(&spadfnode(i)->file_lock);
		up_read_sync_lock(fs);
		inode_unlock(i);
	}

dont_cleanup:
	return 0;
}

void spadfs_delete_file_content(SPADFNODE *f)
{
	spadfs_do_truncate(f, 0);
}

#ifndef SPADFS_MPAGE

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,19,0)
static int spadfs_read_folio(struct file *file, struct folio *folio)
{
	return block_read_full_folio(folio, spadfs_get_block);
}
#else
static int spadfs_readpage(struct file *file, struct page *page)
{
	return block_read_full_page(page, spadfs_get_block);
}
#endif

static int spadfs_writepage(struct page *page, struct writeback_control *wbc)
{
	return block_write_full_page(page, spadfs_get_block, wbc);
}

#else

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,19,0)
static int spadfs_read_folio(struct file *file, struct folio *folio)
{
	return mpage_read_folio(folio, spadfs_get_block);
}
#else
static int spadfs_readpage(struct file *file, struct page *page)
{
	return mpage_readpage(page, spadfs_get_block);
}
#endif

static int spadfs_writepage(struct page *page, struct writeback_control *wbc)
{
	/*return mpage_writepage(page, spadfs_get_block, wbc);*/
	return block_write_full_page(page, spadfs_get_block, wbc);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,8,0) || TEST_RHEL_VERSION(8,7)
static void spadfs_readahead(struct readahead_control *rac)
{
	mpage_readahead(rac, spadfs_get_block);
}
#else
static int spadfs_readpages(struct file *file, struct address_space *mapping,
			    struct list_head *pages, unsigned nr_pages)
{
	return mpage_readpages(mapping, pages, nr_pages, spadfs_get_block);
}
#endif

static int spadfs_writepages(struct address_space *mapping,
			     struct writeback_control *wbc)
{
	return mpage_writepages(mapping, wbc, spadfs_get_block);
}

#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,36)
static inline void spadfs_write_failed(struct address_space *mapping, loff_t to)
{
}
#else
static void spadfs_write_failed(struct address_space *mapping, loff_t to)
{
	sync_lock_decl
	struct inode *i = mapping->host;
	SPADFS *fs = spadfnode(i)->fs;

	down_read_sync_lock(fs);
	mutex_lock(&spadfnode(i)->file_lock);

	if (to > i->i_size) {
#if LINUX_VERSION_CODE < KERNEL_VERSION(3,12,0) && !TEST_RHEL_VERSION(7,1)
		truncate_pagecache(i, to, i->i_size);
#else
		truncate_pagecache(i, i->i_size);
#endif
		spadfs_truncate_unlocked(i);
	}

	mutex_unlock(&spadfnode(i)->file_lock);
	up_read_sync_lock(fs);
}
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,24)
static int spadfs_prepare_write(struct file *file, struct page *page,
				unsigned from, unsigned to)
{
	SPADFNODE *f = spadfnode(page->mapping->host);
	return cont_prepare_write(page, from, to, spadfs_get_block, &f->mmu_private);
}
#else
static int spadfs_write_begin(struct file *file, struct address_space *mapping,
			      loff_t pos, unsigned len,
#if LINUX_VERSION_CODE < KERNEL_VERSION(5,19,0)
			      unsigned flags,
#endif
			      struct page **pagep, void **fsdata)
{
	SPADFNODE *f = spadfnode(mapping->host);
	int r;

	*pagep = NULL;
#if LINUX_VERSION_CODE < KERNEL_VERSION(5,19,0)
	r = cont_write_begin(file, mapping, pos, len, flags, pagep, fsdata, spadfs_get_block, &f->mmu_private);
#else
	r = cont_write_begin(file, mapping, pos, len, pagep, fsdata, spadfs_get_block, &f->mmu_private);
#endif

	if (unlikely(r < 0)) {
		spadfs_write_failed(mapping, pos + len);
	}

	return r;
}
static int spadfs_write_end(struct file *file, struct address_space *mapping,
			    loff_t pos, unsigned len, unsigned copied,
			    struct page *page, void *fsdata)
{
	int r;

	r = generic_write_end(file, mapping, pos, len, copied, page, fsdata);

	if (unlikely(r < 0) || unlikely(r < len)) {
		spadfs_write_failed(mapping, pos + len);
	}

	return r;
}
#endif

static int spadfs_get_block_bmap(struct inode *i, sector_t lblock, struct buffer_head *bh_result, int create)
{
	SPADFS *fs = spadfnode(i)->fs;
	sector_t num_blocks;
	int r;

	num_blocks = spadfs_size_2_sectors(fs, spadfnode(i)->disk_size) >> fs->sectors_per_buffer_bits;
	if (lblock >= num_blocks)
		return 0;

	bh_result->b_size = -(size_t)(512U << fs->sectors_per_buffer_bits);

	r = spadfs_get_block(i, lblock, bh_result, 0);
	if (unlikely(r) || unlikely(!buffer_mapped(bh_result)))
		return r;

	if (bh_result->b_size >> (fs->sectors_per_buffer_bits + 9) >= num_blocks - lblock)
		bh_result->b_size = (num_blocks - lblock) << (fs->sectors_per_buffer_bits + 9);

	return 0;
}

static sector_t spadfs_bmap(struct address_space *mapping, sector_t block)
{
	sync_lock_decl
	SPADFNODE *f = spadfnode(mapping->host);
	SPADFS *fs = f->fs;
	sector_t result;

	spadfs_cond_resched();

	/*
	 * The kernel doesn't synchronize the bmap call with anything, so we
	 * must do synchronization on our own.
	 */
	if (likely(spadfs_unlocked_extent_cache)) {
		down_read_sync_lock(fs);
		mutex_lock(&f->file_lock);
	} else {
		down_write_sync_lock(fs);
	}

	result = generic_block_bmap(mapping, block, spadfs_get_block_bmap);

	if (likely(spadfs_unlocked_extent_cache)) {
		mutex_unlock(&f->file_lock);
		up_read_sync_lock(fs);
	} else {
		up_write_sync_lock(fs);
	}

	return result;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,15,0)
static int spadfs_iomap_begin(struct inode *inode, loff_t offset, loff_t length, unsigned flags, struct iomap *iomap, struct iomap *srcmap)
{
	unsigned int blkbits = inode->i_blkbits;
	int r;
	struct buffer_head tmp = {
		.b_size = 1 << blkbits,
	};

	spadfs_cond_resched();

	r = spadfs_get_block_bmap(inode, offset >> blkbits, &tmp, 0);
	if (unlikely(r))
		return r;

	iomap->bdev = inode->i_sb->s_bdev;
	iomap->offset = offset;
	if (!buffer_mapped(&tmp)) {
		iomap->type = IOMAP_HOLE;
		iomap->addr = IOMAP_NULL_ADDR;
		iomap->length = 1 << blkbits;
	} else {
		iomap->type = IOMAP_MAPPED;
		iomap->flags = IOMAP_F_MERGED;
		iomap->addr = (u64)tmp.b_blocknr << blkbits;
		iomap->length = tmp.b_size;
	}
	return 0;
}

static const struct iomap_ops spadfs_iomap_ops = {
	.iomap_begin = spadfs_iomap_begin,
};
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,28) || TEST_RHEL_VERSION(5,4)
int spadfs_fiemap(struct inode *inode, struct fiemap_extent_info *fieinfo, u64 start, u64 len)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,15,0)
	int ret;
	inode_lock(inode);
	len = min_t(u64, len, i_size_read(inode));
	ret = iomap_fiemap(inode, fieinfo, start, len, &spadfs_iomap_ops);
	inode_unlock(inode);
	return ret;
#else
	return generic_block_fiemap(inode, fieinfo, start, len, spadfs_get_block_bmap);
#endif
}
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(4,1,0)
static ssize_t spadfs_file_write(struct file *file, const char __user *buf,
				 size_t count, loff_t *ppos)
{
	ssize_t r;
	SPADFNODE *f = spadfnode(file_inode(file));

	/* This is just advisory, so it needs no locking */
	set_target_size(f, *ppos + count, 0);

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,19)
	r = generic_file_write(file, buf, count, ppos);
#elif LINUX_VERSION_CODE < KERNEL_VERSION(3,16,0)
	r = do_sync_write(file, buf, count, ppos);
#else
	r = new_sync_write(file, buf, count, ppos);
#endif
	return r;
}
#else
static ssize_t spadfs_file_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	SPADFNODE *f = spadfnode(file_inode(iocb->ki_filp));

	/* This is just advisory, so it needs no locking */
	set_target_size(f, iocb->ki_pos + from->count, 0);

	return generic_file_write_iter(iocb, from);
}
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,35)
static int spadfs_file_fsync(struct file *file, struct dentry *dentry,
			     int datasync)
#elif LINUX_VERSION_CODE < KERNEL_VERSION(3,1,0)
static int spadfs_file_fsync(struct file *file, int datasync)
#else
static int spadfs_file_fsync(struct file *file, loff_t start, loff_t end, int datasync)
#endif
{
	int r;
	int optimized;
	struct buffer_head *bh;
	sync_lock_decl
	SPADFNODE *f = spadfnode(file_inode(file));
	SPADFS *fs = f->fs;

	if (unlikely(sb_rdonly(fs->s)))
		return 0;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,1,0)
	r = filemap_write_and_wait_range(inode(f)->i_mapping, start, end);
	if (unlikely(r))
		return r;
#endif

	down_read_sync_lock(fs);
	mutex_lock(&f->file_lock);
	optimized = 0;
	bh = NULL;
	r = spadfs_write_file(f, datasync, &optimized, &bh);
	mutex_unlock(&f->file_lock);
	up_read_sync_lock(fs);

	if (unlikely(r)) {
		if (bh)
			spadfs_brelse(fs, bh);
		return r;
	}

	if (likely(optimized)) {
		if (likely(bh != NULL)) {
			r = spadfs_sync_dirty_buffer(bh);
			spadfs_brelse(fs, bh);
			if (unlikely(r))
				return r;
			r = spadfs_issue_flush(fs);
			if (unlikely(r))
				return r;
		}
		return 0;
	}

	if (unlikely(bh != NULL))
		spadfs_brelse(fs, bh);

	return spadfs_commit(fs);
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(5,12,0)
int spadfs_file_setattr(struct dentry *dentry, struct iattr *iattr)
#else
int spadfs_file_setattr(struct mnt_idmap *ns, struct dentry *dentry, struct iattr *iattr)
#endif
{
	sync_lock_decl
	struct inode *inode = dentry->d_inode;
	int r;
	if (iattr->ia_valid & ATTR_SIZE) {
		if (unlikely(iattr->ia_size > inode->i_size)) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,16)
			set_target_size(spadfnode(inode), iattr->ia_size, 1);
			r = generic_cont_expand_simple(inode, iattr->ia_size);
			if (unlikely(r))
				return r;
#else
			return -EPERM;
#endif
		}
	}
#if LINUX_VERSION_CODE < KERNEL_VERSION(5,12,0)
	r = spadfs_setattr_common(dentry, iattr);
#else
	r = spadfs_setattr_common(ns, dentry, iattr);
#endif
	if (unlikely(r))
		return r;

	down_read_sync_lock(spadfnode(inode)->fs);
	mutex_lock(&spadfnode(inode)->file_lock);
	spadfs_update_ea(inode);
	r = spadfs_write_file(spadfnode(inode), 0, NULL, NULL);
	mutex_unlock(&spadfnode(inode)->file_lock);
	up_read_sync_lock(spadfnode(inode)->fs);

	return r;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,23) || TEST_RHEL_VERSION(5,3)
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,38)
static long spadfs_file_fallocate(struct inode *inode, int mode, loff_t offset, loff_t len)
{
#else
static long spadfs_file_fallocate(struct file *file, int mode, loff_t offset, loff_t len)
{
	struct inode *inode = file->f_mapping->host;
#endif
	SPADFS *fs = spadfnode(inode)->fs;
	int r;
	if (unlikely(mode & ~FALLOC_FL_KEEP_SIZE))
		return -EOPNOTSUPP;
	inode_lock_nested(inode, 0);
	offset += len;
	r = 0;
	if (!(mode & FALLOC_FL_KEEP_SIZE)) {
		if (likely(offset > inode->i_size)) {
			time_t t = ktime_get_real_seconds();
			inode_set_ctime(inode, t, 0);
			inode->i_mtime.tv_sec = t;
			inode->i_mtime.tv_nsec = 0;
			set_target_size(spadfnode(inode), offset, 1);
			r = generic_cont_expand_simple(inode, offset);
		}
	} else {
		sector_t target_blocks = (unsigned long long)(offset + (512U << fs->sectors_per_disk_block_bits) - 1) >> (fs->sectors_per_disk_block_bits + 9);
		sector_t existing_blocks = (unsigned long long)(spadfnode(inode)->disk_size + (512U << fs->sectors_per_disk_block_bits) - 1) >> (fs->sectors_per_disk_block_bits + 9);
		if (likely(target_blocks > existing_blocks)) {
			r = spadfs_extend_file(inode, target_blocks, 2);
		}
	}
	inode_unlock(inode);
	return r;
}
#endif

#ifdef SPADFS_DIRECT_IO
#if LINUX_VERSION_CODE < KERNEL_VERSION(3,16,0)
static ssize_t spadfs_direct_io(int rw, struct kiocb *iocb,
				const struct iovec *iov, loff_t offset,
				unsigned long nr_segs)
#elif LINUX_VERSION_CODE < KERNEL_VERSION(4,1,0)
static ssize_t spadfs_direct_io(int rw, struct kiocb *iocb,
				struct iov_iter *iter, loff_t offset)
#elif LINUX_VERSION_CODE < KERNEL_VERSION(4,7,0)
static ssize_t spadfs_direct_io(struct kiocb *iocb,
				struct iov_iter *iter, loff_t offset)
#else
static ssize_t spadfs_direct_io(struct kiocb *iocb,
				struct iov_iter *iter)
#endif
{
	struct file *file = iocb->ki_filp;
	struct inode *inode = file_inode(file);
	int r;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,1,0)
	int rw = iov_iter_rw(iter);
#endif
#if LINUX_VERSION_CODE < KERNEL_VERSION(3,16,0)
	size_t count = iov_length(iov, nr_segs);
#else
	size_t count = iov_iter_count(iter);
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,7,0)
	loff_t offset = iocb->ki_pos;
#endif

	if (rw == WRITE) {
		/* Copied from fat_direct_IO */
		loff_t size = offset + count;
		if (spadfnode(inode)->mmu_private < size)
			return 0;
	}

#if LINUX_VERSION_CODE < KERNEL_VERSION(3,1,0)
	r = blockdev_direct_IO(rw, iocb, inode, inode->i_sb->s_bdev, iov,
			       offset, nr_segs, spadfs_get_block_direct,
			       NULL);
#elif LINUX_VERSION_CODE < KERNEL_VERSION(3,16,0)
	r = blockdev_direct_IO(rw, iocb, inode, iov, offset, nr_segs,
			       spadfs_get_block_direct);
#elif LINUX_VERSION_CODE < KERNEL_VERSION(4,1,0)
	r = blockdev_direct_IO(rw, iocb, inode, iter, offset,
			       spadfs_get_block_direct);
#elif LINUX_VERSION_CODE < KERNEL_VERSION(4,7,0)
	r = blockdev_direct_IO(iocb, inode, iter, offset,
			       spadfs_get_block_direct);
#else
	r = blockdev_direct_IO(iocb, inode, iter, spadfs_get_block_direct);
#endif

	if (unlikely(r < 0) && rw == WRITE) {
		spadfs_write_failed(file->f_mapping, offset + count);
	}

	return r;
}
#endif

#ifdef SPADFS_QUOTA

static void *spadfs_read_file_sector(SPADFNODE *f, sector_t blk, struct buffer_head **bhp, int get_new)
{
	SPADFS *fs = f->fs;
	struct buffer_head tmp_bh;
	sector_t phys_sector;
	int r;
	void *file;

	tmp_bh.b_state = 0;
	tmp_bh.b_size = 512U << fs->sectors_per_buffer_bits;

	r = spadfs_get_block(inode(f), blk, &tmp_bh, 0);
	if (unlikely(r))
		return ERR_PTR(r);

	BUG_ON(!buffer_mapped(&tmp_bh));

	phys_sector = tmp_bh.b_blocknr << fs->sectors_per_buffer_bits;

	if (likely(!get_new))
		file = spadfs_read_sector(fs, phys_sector, bhp, 0,
					  "spadfs_read_file_sector");
	else
		file = spadfs_get_new_sector(fs, phys_sector, bhp,
					     "spadfs_read_file_sector");

	return file;
}

static noinline int spadfs_quota_extend(SPADFNODE *f, unsigned bytes)
{
	SPADFS *fs = f->fs;
	loff_t i_size = i_size_read(inode(f));
	int r;

	if (i_size >= f->disk_size) {
		BUG_ON(i_size != f->disk_size);

		r = spadfs_do_extend(f, i_size + (512U << fs->sectors_per_buffer_bits), 0, 0);
		if (unlikely(r))
			return r;
	}

	i_size += bytes;
	BUG_ON(i_size > f->disk_size);
	i_size_write(inode(f), i_size);
	f->mmu_private = i_size;

	spadfs_write_file(f, 0, NULL, NULL);

	return 0;
}

static ssize_t spadfs_quota_rw(struct super_block *s, struct inode *inode,
			       char *data, size_t len, loff_t position, int rw)
{
	SPADFS *fs = spadfs(s);
	size_t bytes_left;
	int r;

	if (unlikely(position > i_size_read(inode)))
		return 0;

	bytes_left = len;
	while (bytes_left > 0) {
		unsigned off;
		unsigned to_copy;
		struct buffer_head *bh;
		char *file;
		loff_t i_size;

		i_size = i_size_read(inode);

		off = position & ((512U << fs->sectors_per_buffer_bits) - 1);
		to_copy = (512U << fs->sectors_per_buffer_bits) - off;
		if (to_copy > bytes_left)
			to_copy = bytes_left;

		if (unlikely(position == i_size)) {
			if (!rw)
				break;
			r = spadfs_quota_extend(spadfnode(inode), to_copy);
			if (unlikely(r))
				return r;
			i_size = i_size_read(inode);
		}

		if (unlikely(position + to_copy > i_size))
			to_copy = i_size - position;

		file = spadfs_read_file_sector(spadfnode(inode),
			position >> (fs->sectors_per_buffer_bits + 9), &bh,
			rw && to_copy == 512U << fs->sectors_per_buffer_bits);
		if (unlikely(IS_ERR(file)))
			return PTR_ERR(file);

		if (!rw)
			memcpy(data, file + off, to_copy);
		else {
			lock_buffer(bh);
			memcpy(file + off, data, to_copy);
			flush_dcache_page(bh->b_page);
			unlock_buffer(bh);
			mark_buffer_dirty(bh);
		}

		spadfs_brelse(fs, bh);

		data += to_copy;
		position += to_copy;
		bytes_left -= to_copy;
	}
	return len - bytes_left;
}

ssize_t spadfs_quota_read(struct super_block *s, int type,
			  char *data, size_t len, loff_t position)
{
	struct inode *inode = sb_dqopt(s)->files[type];

	return spadfs_quota_rw(s, inode, data, len, position, 0);
}

ssize_t spadfs_quota_write(struct super_block *s, int type,
			   const char *data, size_t len, loff_t position)
{
	struct inode *inode = sb_dqopt(s)->files[type];
	SPADFS *fs = spadfs(s);
	ssize_t r;

#if LINUX_VERSION_CODE < KERNEL_VERSION(3,5,0)
	inode_lock_nested(inode, I_MUTEX_QUOTA);
#endif
	mutex_lock(&fs->quota_alloc_lock);

	r = spadfs_quota_rw(s, inode, (char *)data, len, position, 1);

	mutex_unlock(&fs->quota_alloc_lock);
#if LINUX_VERSION_CODE < KERNEL_VERSION(3,5,0)
	inode_unlock(inode);
#endif

	return r;
}

#if defined(SPADFS_QUOTA) && SPADFS_QUOTA >= 2
struct dquot **spadfs_quota_get(struct inode *inode)
{
	return spadfnode(inode)->i_dquot;
}
#endif

#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,21)
const
#endif
struct inode_operations spadfs_file_iops = {
#if LINUX_VERSION_CODE < KERNEL_VERSION(3,8,0)
	.truncate = spadfs_truncate,
#endif
	.setattr = spadfs_file_setattr,
	.getattr = spadfs_getattr,
#ifdef SPADFS_XATTR
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,9,0)
	.setxattr = generic_setxattr,
	.getxattr = generic_getxattr,
	.removexattr = generic_removexattr,
#endif
	.listxattr = spadfs_listxattr,
#endif
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,23) && LINUX_VERSION_CODE < KERNEL_VERSION(2,6,38)) || TEST_RHEL_VERSION(5,3)
	.fallocate = spadfs_file_fallocate,
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,28) || TEST_RHEL_VERSION(5,4)
	.fiemap = spadfs_fiemap,
#endif
};

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,17)
const
#endif
struct file_operations spadfs_file_fops = {
	.llseek = generic_file_llseek,
#if LINUX_VERSION_CODE < KERNEL_VERSION(3,16,0)
	.read = do_sync_read,
#elif LINUX_VERSION_CODE < KERNEL_VERSION(4,1,0)
	.read = new_sync_read,
#endif
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,1,0)
	.write = spadfs_file_write,
#endif
	.mmap = generic_file_mmap,
	.fsync = spadfs_file_fsync,
#if LINUX_VERSION_CODE < KERNEL_VERSION(3,16,0)
	.aio_read = generic_file_aio_read,
	.aio_write = generic_file_aio_write,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,17)
	.splice_read = generic_file_splice_read,
	.splice_write = generic_file_splice_write,
#endif
#else
	.read_iter = generic_file_read_iter,
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,1,0)
	.write_iter = generic_file_write_iter,
#else
	.write_iter = spadfs_file_write_iter,
#endif
#if LINUX_VERSION_CODE < KERNEL_VERSION(6,5,0)
	.splice_read = generic_file_splice_read,
#else
	.splice_read = filemap_splice_read,
#endif
	.splice_write = iter_file_splice_write,
#endif
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,23)
	.sendfile = generic_file_sendfile,
#endif
	.release = spadfs_release,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,11)
	.unlocked_ioctl = spadfs_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = spadfs_compat_ioctl,
#endif
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,38)
	.fallocate = spadfs_file_fallocate,
#endif
};

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,18)
const
#endif
struct address_space_operations spadfs_file_aops = {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,18,0)
	.dirty_folio = block_dirty_folio,
	.invalidate_folio = block_invalidate_folio,
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(5,14,0)
	.set_page_dirty = __set_page_dirty_buffers,
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,19,0)
	.read_folio = spadfs_read_folio,
#else
	.readpage = spadfs_readpage,
#endif
	.writepage = spadfs_writepage,
#ifdef SPADFS_MPAGE
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,8,0) || TEST_RHEL_VERSION(8,7)
	.readahead = spadfs_readahead,
#else
	.readpages = spadfs_readpages,
#endif
	.writepages = spadfs_writepages,
#endif
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,39)
	.sync_page = block_sync_page,
#endif
#ifdef SPADFS_DIRECT_IO
	.direct_IO = spadfs_direct_io,
#endif
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,24)
	.prepare_write = spadfs_prepare_write,
#else
	.write_begin = spadfs_write_begin,
	.write_end = spadfs_write_end,
#endif
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,26)
	.commit_write = generic_commit_write,
#endif
	.bmap = spadfs_bmap,
};
