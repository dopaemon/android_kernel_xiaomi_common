#include "spadfs.h"

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,3,0)
#define NEW_BIO
#endif

#ifdef CHECK_BUFFER_LEAKS
static void spadfs_track_buffer(SPADFS *fs, struct buffer_head *bh,
				const char *msg);
#else
#define spadfs_track_buffer(fs, bh, msg) do { } while (0)
#endif

static void *spadfs_read_sector_physical(SPADFS *fs, sector_t secno,
					 struct buffer_head **bhp, unsigned ahead,
					 const char *msg);

void *spadfs_read_sector(SPADFS *fs, sector_t secno, struct buffer_head **bhp,
			 unsigned ahead, const char *msg)
{
	struct buffer_head *bh;
	spadfs_cond_resched();
	if (likely((*bhp = bh = sb_find_get_block(fs->s,
			       secno >> fs->sectors_per_buffer_bits)) != NULL)) {
		if (unlikely(!buffer_uptodate(bh))) {
			__brelse(bh);
			*bhp = NULL;
			goto read_phys;
		}
		spadfs_track_buffer(fs, bh, msg);
		return spadfs_buffer_data(fs, secno, bh);
	}
read_phys:
	return spadfs_read_sector_physical(fs, secno, bhp, ahead, msg);
}

#ifdef SPADFS_DO_PREFETCH
#ifndef NEW_BIO
static void end_io_multibuffer_read(struct bio *bio, int err);
#else
static void end_io_multibuffer_read(struct bio *bio);
#endif
#endif

static noinline void *spadfs_read_sector_physical(SPADFS *fs, sector_t secno,
						  struct buffer_head **bhp,
						  unsigned ahead, const char *msg)
{
	struct buffer_head *bh;
	sector_t blockno;
	if (unlikely(secno + ahead < secno) ||
	    unlikely(secno + ahead >= fs->size)) {
		ahead = 0;
		if (unlikely(secno >= fs->size)) {
			spadfs_error(fs, TXFLAGS_FS_ERROR,
				"access out of device, block %Lx, size %Lx at %s",
				(unsigned long long)secno,
				(unsigned long long)fs->size,
				msg);
			return ERR_PTR(-EFSERROR);
		}
	}
	blockno = secno >> fs->sectors_per_buffer_bits;
#ifdef SPADFS_DO_PREFETCH
	if (unlikely(ahead > BIO_MAX_VECS * PAGE_SIZE / 512))
		ahead = BIO_MAX_VECS * PAGE_SIZE / 512;
	{
		unsigned max_sec;
		max_sec = queue_max_sectors(bdev_get_queue(fs->s->s_bdev));
		if (ahead > max_sec) {
			ahead = max_sec;
			if (unlikely(ahead & (ahead - 1)))
				ahead = 1U << (fls(ahead) - 1);
		}
	}
	ahead >>= fs->sectors_per_buffer_bits;
	if (ahead > 1) {
		struct bio *bio;
		void **link;
		sector_t reada_blockno = blockno;
		unsigned i, bio_pages;

		if (likely(!(ahead & (ahead - 1))) &&
		    likely(ahead > 1 <<
		    (fs->sectors_per_page_bits - fs->sectors_per_buffer_bits)))
			reada_blockno &= ~(sector_t)(ahead - 1);
retry_readahead:
		bio_pages = (ahead >> (PAGE_SHIFT - 9 - fs->sectors_per_buffer_bits));
		bio_pages += fs->sectors_per_buffer_bits != PAGE_SHIFT - 9;
		if (unlikely(bio_pages > BIO_MAX_VECS))
			bio_pages = BIO_MAX_VECS;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,18,0)
		bio = bio_alloc(fs->s->s_bdev, bio_pages, REQ_OP_READ | REQ_RAHEAD | REQ_SYNC, (GFP_NOIO & ~__GFP_DIRECT_RECLAIM) | __GFP_NORETRY | __GFP_NOWARN);
		if (unlikely(!bio))
			goto no_rdahead;
#else
		bio = bio_alloc((GFP_NOIO & ~__GFP_DIRECT_RECLAIM) | __GFP_NORETRY | __GFP_NOWARN, bio_pages);
		if (unlikely(!bio))
			goto no_rdahead;
		bio_set_dev(bio, fs->s->s_bdev);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,8,0)
		bio_set_op_attrs(bio, REQ_OP_READ, REQ_RAHEAD | REQ_SYNC);
#endif
#endif
#if LINUX_VERSION_CODE < KERNEL_VERSION(3,14,0)
		bio->bi_sector
#else
		bio->bi_iter.bi_sector
#endif
			= reada_blockno << fs->sectors_per_buffer_bits;
		bio->bi_end_io = end_io_multibuffer_read;
		link = &bio->bi_private;
		bio->bi_private = NULL;
		for (i = 0; i < ahead; i++) {
			struct buffer_head *bh;
			bh = sb_getblk(fs->s, reada_blockno + i);
			if (unlikely(!bh))
				break;
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,27)
			if (unlikely(test_set_buffer_locked(bh)))
#else
			if (unlikely(!trylock_buffer(bh)))
#endif
				goto brelse_break;
			if (unlikely(buffer_uptodate(bh)))
				goto unlock_brelse_break;
			if (unlikely(!bio_add_page(bio, bh->b_page, bh->b_size,
						   bh_offset(bh)))) {
unlock_brelse_break:
				unlock_buffer(bh);
brelse_break:
				__brelse(bh);
				break;
			}
			*link = bh;
			link = &bh->b_private;
			bh->b_private = NULL;
		}
		if (likely(reada_blockno + i > blockno)) {
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,8,0)
			submit_bio(READA, bio);
#else
			submit_bio(bio);
#endif
		} else {
#ifndef NEW_BIO
			bio_endio(bio, -EINTR);
#elif LINUX_VERSION_CODE < KERNEL_VERSION(4,13,0)
			bio->bi_error = -EINTR;
			bio_endio(bio);
#else
			bio->bi_status = BLK_STS_IOERR;
			bio_endio(bio);
#endif
			if (blockno != reada_blockno) {
				if (likely(!(ahead & (ahead - 1))))
					ahead -= (blockno & (ahead - 1));
				if (likely(ahead > 1)) {
					reada_blockno = blockno;
					goto retry_readahead;
				}
			}
		}
	}
no_rdahead:
#endif
#if 0
	if (!bhp)
		return NULL;
#endif
	if (likely((*bhp = bh = sb_bread(fs->s, blockno)) != NULL)) {
		spadfs_track_buffer(fs, bh, msg);
		return spadfs_buffer_data(fs, secno, bh);
	}
	spadfs_error(fs, TXFLAGS_IO_READ_ERROR, "read error, block %Lx at %s",
			(unsigned long long)secno, msg);
	return ERR_PTR(-EIO);
}

#ifdef SPADFS_DO_PREFETCH
#ifndef NEW_BIO
static void end_io_multibuffer_read(struct bio *bio, int err)
#else
static void end_io_multibuffer_read(struct bio *bio)
#endif
{
	struct buffer_head *bh = bio->bi_private;
	while (bh) {
		struct buffer_head *next_bh = bh->b_private;
		bh->b_private = NULL;
#ifndef NEW_BIO
		end_buffer_read_sync(bh, test_bit(BIO_UPTODATE,
				     &bio->bi_flags));
#elif LINUX_VERSION_CODE < KERNEL_VERSION(4,13,0)
		end_buffer_read_sync(bh, !bio->bi_error);
#else
		end_buffer_read_sync(bh, bio->bi_status == BLK_STS_OK);
#endif
		bh = next_bh;
	}
	bio_put(bio);
}
#endif

void *spadfs_get_new_sector(SPADFS *fs, sector_t secno, struct buffer_head **bhp, const char *msg)
{
	struct buffer_head *bh;
	spadfs_cond_resched();
	if (unlikely(secno >= fs->size)) {
		spadfs_error(fs, TXFLAGS_FS_ERROR,
			"creating block out of device, block %Lx, size %Lx at %s",
			(unsigned long long)secno,
			(unsigned long long)fs->size,
			msg);
		return ERR_PTR(-EFSERROR);
	}
	if (unlikely((unsigned long)secno & ((1 << fs->sectors_per_buffer_bits) - 1))) {
		spadfs_error(fs, TXFLAGS_FS_ERROR,
			"unaligned get new sector %Lx at %s",
			(unsigned long long)secno,
			msg);
		return ERR_PTR(-EFSERROR);
	}
	if (likely((*bhp = bh = sb_getblk(fs->s, secno >> fs->sectors_per_buffer_bits)) != NULL)) {
		if (!buffer_uptodate(bh))
			wait_on_buffer(bh);
		set_buffer_uptodate(bh);
		spadfs_track_buffer(fs, bh, msg);
		return bh->b_data;
	}
	spadfs_error(fs, TXFLAGS_IO_READ_ERROR,
		"can't get new sector %Lx at %s",
		(unsigned long long)secno,
		msg);
	return ERR_PTR(-ENOMEM);
}

#if 0
void spadfs_prefetch_sector(SPADFS *fs, sector_t secno, unsigned ahead, const char *msg)
{
#ifdef SPADFS_DO_PREFETCH
	struct buffer_head *bh;
	if (likely((bh = sb_find_get_block(fs->s,
			       secno >> fs->sectors_per_buffer_bits)) != NULL)) {
		__brelse(bh);
		return;
	}
	spadfs_read_sector_physical(fs, secno, NULL, ahead, msg);
#endif
}
#endif

void spadfs_discard_buffers(SPADFS *fs, sector_t start, sector_t n_sectors)
{
	start >>= fs->sectors_per_buffer_bits;
	n_sectors >>= fs->sectors_per_buffer_bits;
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,10,0)
	for (; n_sectors; n_sectors--, start++)
		bforget(sb_find_get_block(fs->s, start));
#else
	clean_bdev_aliases(fs->s->s_bdev, start, n_sectors);
#endif
}

#ifdef CHECK_BUFFER_LEAKS

struct tracked_buffer {
	struct hlist_node list;
	sector_t block;
	const char *msg;
};

static void spadfs_track_buffer(SPADFS *fs, struct buffer_head *bh,
				const char *msg)
{
	struct tracked_buffer *tb;
	tb = kmalloc(sizeof(struct tracked_buffer), GFP_NOFS);
	mutex_lock(&fs->buffer_track_lock);
	if (likely(tb != NULL)) {
		tb->block = bh->b_blocknr;
		tb->msg = msg;
		hlist_add_head(&tb->list, &fs->buffer_list);
	} else {
		fs->buffer_oom_events++;
	}
	mutex_unlock(&fs->buffer_track_lock);
}

void spadfs_brelse(SPADFS *fs, struct buffer_head *bh)
{
	spadfs_drop_reference(fs, bh);
#ifdef CHECK_BUFFER_WRITES
#ifdef CHECK_BUFFER_WRITES_RANDOMIZE
	if (!(random32() & 63))
#endif
		spadfs_sync_dirty_buffer(bh);
#endif
	__brelse(bh);
}

void spadfs_drop_reference(SPADFS *fs, struct buffer_head *bh)
{
	struct tracked_buffer *tb;
#if LINUX_VERSION_CODE < KERNEL_VERSION(3,9,0)
	struct hlist_node *n;
#endif
	mutex_lock(&fs->buffer_track_lock);
#if LINUX_VERSION_CODE < KERNEL_VERSION(3,9,0)
	hlist_for_each_entry(tb, n, &fs->buffer_list, list)
#else
	hlist_for_each_entry(tb, &fs->buffer_list, list)
#endif
	{
		if (tb->block == bh->b_blocknr) {
			hlist_del(&tb->list);
			kfree(tb);
			goto found;
		}
		spadfs_cond_resched();
	}
	BUG_ON(!fs->buffer_oom_events);
	fs->buffer_oom_events--;
found:
	mutex_unlock(&fs->buffer_track_lock);
}

void spadfs_buffer_leaks_init(SPADFS *fs)
{
	mutex_init(&fs->buffer_track_lock);
	INIT_HLIST_HEAD(&fs->buffer_list);
	fs->buffer_oom_events = 0;
}

void spadfs_buffer_leaks_done(SPADFS *fs)
{
	while (unlikely(!hlist_empty(&fs->buffer_list))) {
		struct tracked_buffer *tb = list_entry(fs->buffer_list.first,
						struct tracked_buffer, list);
		printk(KERN_ERR "spadfs internal error: buffer %Lx leaked at %s\n",
			(unsigned long long)tb->block, tb->msg);
		hlist_del(&tb->list);
		kfree(tb);
	}
	if (unlikely(fs->buffer_oom_events != 0))
		printk(KERN_ERR "spadfs internal error: %lx unknown buffer leaked\n",
			fs->buffer_oom_events);
	mutex_destroy(&fs->buffer_track_lock);
}

#endif
