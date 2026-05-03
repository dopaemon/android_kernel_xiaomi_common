// SPDX-License-Identifier: GPL-2.0
#include <linux/bio.h>
#include <linux/bitops.h>
#include <linux/freezer.h>
#include <linux/kfifo.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/swap.h>
#include <linux/vmalloc.h>

#include "kcompressd.h"

#define INIT_QUEUE_SIZE		4096
#define DEFAULT_NR_KCOMPRESSD	4

enum run_state {
	KCOMPRESSD_NOT_STARTED = 0,
	KCOMPRESSD_RUNNING,
	KCOMPRESSD_SLEEPING,
};

struct write_work {
	void *mem;
	struct bio *bio;
	compress_callback cb;
};

struct kcompressd_para {
	wait_queue_head_t *kcompressd_wait;
	struct kfifo *write_fifo;
	spinlock_t *fifo_lock;
	atomic_t *running;
};

static atomic_t enable_kcompressd;
static unsigned int nr_kcompressd;
static unsigned int queue_size_per_kcompressd;
static struct kcompress *kcompress;
static struct kcompressd_para *kcompressd_para;

int kcompressd_enabled(void)
{
	return likely(atomic_read(&enable_kcompressd));
}

static void kcompressd_try_to_sleep(struct kcompressd_para *p)
{
	DEFINE_WAIT(wait);

	if (!kfifo_is_empty_spinlocked(p->write_fifo, p->fifo_lock))
		return;
	if (freezing(current) || kthread_should_stop())
		return;

	atomic_set(p->running, KCOMPRESSD_SLEEPING);
	prepare_to_wait(p->kcompressd_wait, &wait, TASK_INTERRUPTIBLE);
	if (!kthread_should_stop() &&
	    kfifo_is_empty_spinlocked(p->write_fifo, p->fifo_lock))
		schedule();
	finish_wait(p->kcompressd_wait, &wait);
	atomic_set(p->running, KCOMPRESSD_RUNNING);
}

static int kcompressd(void *para)
{
	struct task_struct *tsk = current;
	struct kcompressd_para *p = para;

	tsk->flags |= PF_MEMALLOC | PF_KSWAPD;
	set_freezable();

	while (!kthread_should_stop()) {
		bool frozen;

		kcompressd_try_to_sleep(p);
		frozen = try_to_freeze();
		if (kthread_should_stop())
			break;
		if (frozen)
			continue;

		while (!kfifo_is_empty_spinlocked(p->write_fifo, p->fifo_lock)) {
			struct write_work entry;

			if (kfifo_out_spinlocked(p->write_fifo, &entry, sizeof(entry),
						 p->fifo_lock) != sizeof(entry))
				break;
			entry.cb(entry.mem, entry.bio);
			bio_put(entry.bio);
		}
	}

	tsk->flags &= ~(PF_MEMALLOC | PF_KSWAPD);
	atomic_set(p->running, KCOMPRESSD_NOT_STARTED);
	return 0;
}

static int init_write_queue(void)
{
	int i;
	unsigned int queue_len = queue_size_per_kcompressd * sizeof(struct write_work);

	for (i = 0; i < nr_kcompressd; i++) {
		if (kfifo_alloc(&kcompress[i].write_fifo, queue_len, GFP_KERNEL)) {
			while (--i >= 0)
				kfifo_free(&kcompress[i].write_fifo);
			return -ENOMEM;
		}
	}

	return 0;
}

static void clean_bio_queue(int idx)
{
	struct write_work entry;
	struct kcompress *kc = &kcompress[idx];

	while (kfifo_out_spinlocked(&kc->write_fifo, &entry, sizeof(entry),
				    &kc->fifo_lock) == sizeof(entry)) {
		entry.cb(entry.mem, entry.bio);
		bio_put(entry.bio);
	}
	kfifo_free(&kc->write_fifo);
}

static void drop_bio_queue_no_cb(int idx)
{
	struct write_work entry;
	struct kcompress *kc = &kcompress[idx];

	while (kfifo_out_spinlocked(&kc->write_fifo, &entry, sizeof(entry),
				    &kc->fifo_lock) == sizeof(entry))
		bio_put(entry.bio);
}

static void stop_all_kcompressd_thread(void)
{
	int i;

	if (!kcompress)
		return;

	for (i = 0; i < nr_kcompressd; i++) {
		if (kcompress[i].kcompressd) {
			kthread_stop(kcompress[i].kcompressd);
			kcompress[i].kcompressd = NULL;
		}
		clean_bio_queue(i);
	}
}

int schedule_bio_write(void *mem, struct bio *bio, compress_callback cb)
{
	int i;
	size_t sz_work = sizeof(struct write_work);
	struct write_work entry = {
		.mem = mem,
		.bio = bio,
		.cb = cb,
	};

	if (unlikely(!atomic_read(&enable_kcompressd)))
		return -EBUSY;
	if (!nr_kcompressd || !current_is_kswapd())
		return -EBUSY;

	bio_get(bio);

	for (i = 0; i < nr_kcompressd; i++) {
		bool submit_success;

		submit_success = (kfifo_in_spinlocked(&kcompress[i].write_fifo, &entry,
						      sz_work, &kcompress[i].fifo_lock) ==
				  sz_work);
		if (!submit_success)
			continue;

		switch (atomic_read(&kcompress[i].running)) {
		case KCOMPRESSD_NOT_STARTED:
			atomic_set(&kcompress[i].running, KCOMPRESSD_RUNNING);
			kcompress[i].kcompressd = kthread_run(kcompressd, &kcompressd_para[i],
							      "kcompressd:%d", i);
			if (IS_ERR(kcompress[i].kcompressd)) {
				atomic_set(&kcompress[i].running, KCOMPRESSD_NOT_STARTED);
				kcompress[i].kcompressd = NULL;
				drop_bio_queue_no_cb(i);
				return -EBUSY;
			}
			break;
		case KCOMPRESSD_RUNNING:
			break;
		case KCOMPRESSD_SLEEPING:
			wake_up_interruptible(&kcompress[i].kcompressd_wait);
			break;
		}

		return 0;
	}

	bio_put(bio);
	return -EBUSY;
}

static int __init kcompressd_init(void)
{
	int ret, i;

	nr_kcompressd = DEFAULT_NR_KCOMPRESSD;
	queue_size_per_kcompressd = INIT_QUEUE_SIZE;

	kcompress = kvmalloc_array(nr_kcompressd, sizeof(*kcompress), GFP_KERNEL);
	if (!kcompress)
		return -ENOMEM;

	kcompressd_para = kvmalloc_array(nr_kcompressd, sizeof(*kcompressd_para), GFP_KERNEL);
	if (!kcompressd_para) {
		kvfree(kcompress);
		kcompress = NULL;
		return -ENOMEM;
	}

	ret = init_write_queue();
	if (ret) {
		kvfree(kcompressd_para);
		kvfree(kcompress);
		kcompressd_para = NULL;
		kcompress = NULL;
		return ret;
	}

	for (i = 0; i < nr_kcompressd; i++) {
		init_waitqueue_head(&kcompress[i].kcompressd_wait);
		spin_lock_init(&kcompress[i].fifo_lock);
		kcompressd_para[i].kcompressd_wait = &kcompress[i].kcompressd_wait;
		kcompressd_para[i].write_fifo = &kcompress[i].write_fifo;
		kcompressd_para[i].fifo_lock = &kcompress[i].fifo_lock;
		kcompressd_para[i].running = &kcompress[i].running;
		atomic_set(&kcompress[i].running, KCOMPRESSD_NOT_STARTED);
	}

	atomic_set(&enable_kcompressd, true);
	return 0;
}

static void __exit kcompressd_exit(void)
{
	atomic_set(&enable_kcompressd, false);
	stop_all_kcompressd_thread();
	kvfree(kcompressd_para);
	kvfree(kcompress);
	kcompressd_para = NULL;
	kcompress = NULL;
}

module_init(kcompressd_init);
module_exit(kcompressd_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Offload zram write compression to helper threads");
