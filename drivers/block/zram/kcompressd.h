/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _KCOMPRESSD_H_
#define _KCOMPRESSD_H_

#include <linux/atomic.h>
#include <linux/kfifo.h>
#include <linux/spinlock.h>
#include <linux/wait.h>

struct bio;
struct task_struct;

typedef void (*compress_callback)(void *mem, struct bio *bio);

struct kcompress {
	struct task_struct *kcompressd;
	wait_queue_head_t kcompressd_wait;
	struct kfifo write_fifo;
	spinlock_t fifo_lock;
	atomic_t running;
};

int kcompressd_enabled(void);
int schedule_bio_write(void *mem, struct bio *bio, compress_callback cb);

#endif
