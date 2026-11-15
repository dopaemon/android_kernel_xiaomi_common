#ifndef _SPADFS_LINUX_COMPAT_H
#define _SPADFS_LINUX_COMPAT_H

#define TEST_STABLE_BRANCH(v1,v2,v3)	(LINUX_VERSION_CODE >= KERNEL_VERSION(v1,v2,v3) && LINUX_VERSION_CODE < KERNEL_VERSION(v1,(v2)+1,0))

#ifndef RHEL_MAJOR
#define TEST_RHEL_VERSION(v1,v2)	0
#else
#define TEST_RHEL_VERSION(v1,v2)	(RHEL_MAJOR == (v1) && RHEL_MINOR >= (v2))
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,9)
static inline void vprintk(const char *fmt, va_list args)
{
	char buffer[256];
	vsnprintf(buffer, sizeof buffer, fmt, args);
	printk("%s", buffer);
}
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,12)
static inline void *kmalloc_node(size_t size, int flags, int node)
{
	return kmalloc(size, flags);
}
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,12)
static inline void cancel_rearming_delayed_workqueue(
			struct workqueue_struct *wq, struct work_struct *work)
{
	while (!cancel_delayed_work(work))
		flush_workqueue(wq);
}
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,13)
#define raw_smp_processor_id()	smp_processor_id()
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,12)
#define rcu_barrier()	synchronize_kernel()
#elif LINUX_VERSION_CODE < KERNEL_VERSION(2,6,15)
#define rcu_barrier()	synchronize_rcu()
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,15)
static inline int rwsem_is_locked(struct rw_semaphore *s)
{
	if (down_write_trylock(s)) {
		up_write(s);
		return 0;
	}
	return 1;
}
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,16)
#define mutex			semaphore
#define mutex_init		init_MUTEX
#define mutex_destroy(m)	do { } while (0)
#define mutex_lock		down
#define mutex_lock_nested(s, n)	down(s)
#define mutex_unlock		up
#define mutex_trylock(s)	(!down_trylock(s))
#define mutex_is_locked(s)	(atomic_read(&(s)->count) < 1)
#define i_mutex			i_sem
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,17)
#define SLAB_MEM_SPREAD	0
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,19) && !TEST_RHEL_VERSION(5,3)
static inline void clear_nlink(struct inode *inode)
{
	inode->i_nlink = 0;
}
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,19)
typedef kmem_cache_t spadfs_cache_t;
#define spadfs_free_cache(c)	do { if (c) { WARN_ON(kmem_cache_destroy(c)); (c) = NULL; } } while (0)
#else
typedef struct kmem_cache spadfs_cache_t;
#define spadfs_free_cache(c)	do { if (c) { kmem_cache_destroy(c); (c) = NULL; } } while (0)
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,20)
#define NEW_WORKQUEUE
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,22)
#define KMALLOC_MAX_SIZE	131072
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,23)
#define PTR_ALIGN(p, a)		((typeof(p))ALIGN((unsigned long)(p), (a)))
#endif

#ifndef __cold
#define __cold
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,25) && !TEST_RHEL_VERSION(5,3)
static inline int is_vmalloc_addr(const void *ptr)
{
#ifdef CONFIG_MMU
	if ((unsigned long)ptr >= VMALLOC_START &&
	    (unsigned long)ptr < VMALLOC_END)
		return 1;
#endif
	return 0;
}
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,26) && !TEST_RHEL_VERSION(5,4)
static inline size_t match_strlcpy(char *dest, const substring_t *src, size_t size)
{
	size_t ret = src->to - src->from;

	if (size) {
		size_t len = ret >= size ? size - 1 : ret;
		memcpy(dest, src->from, len);
		dest[len] = '\0';
	}

	return ret;
}
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,27)
#define get_current_uid()	(current->uid)
#define get_current_gid()	(current->gid)
#define get_current_fsuid()	(current->fsuid)
#define get_current_fsgid()	(current->fsgid)
#elif LINUX_VERSION_CODE < KERNEL_VERSION(3,5,0)
#define get_current_uid()	current_uid()
#define get_current_gid()	current_gid()
#define get_current_fsuid()	current_fsuid()
#define get_current_fsgid()	current_fsgid()
#else
#define get_current_uid()	from_kuid(&init_user_ns, current_uid())
#define get_current_gid()	from_kgid(&init_user_ns, current_gid())
#define get_current_fsuid()	from_kuid(&init_user_ns, current_fsuid())
#define get_current_fsgid()	from_kgid(&init_user_ns, current_fsgid())
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,28)
#define nr_cpumask_bits		NR_CPUS
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,31)
static inline unsigned queue_max_sectors(struct request_queue *q)
{
	return q->max_sectors;
}
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,2,0)
#define HAVE_SET_NLINK
#endif
#if TEST_RHEL_VERSION(6,9)
#if RHEL_RELEASE >= 753
#define HAVE_SET_NLINK
#endif
#endif
#ifndef HAVE_SET_NLINK
static inline void set_nlink(struct inode *inode, unsigned nlink)
{
	inode->i_nlink = nlink;
}
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(3,5,0)
#define i_uid_read(i)		((i)->i_uid)
#define i_gid_read(i)		((i)->i_gid)
#define i_uid_write(i, u)	((i)->i_uid = (u))
#define i_gid_write(i, u)	((i)->i_gid = (u))
#define uid_eq(a, b)		((a) == (b))
#define gid_eq(a, b)		((a) == (b))
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(3,9,0)
#define file_inode(f)	(file_dentry(f)->d_inode)
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(3,15,0) && !TEST_STABLE_BRANCH(3,12,41) && !TEST_RHEL_VERSION(6,8) && !TEST_RHEL_VERSION(7,1)
static inline void kvfree(void *ptr)
{
	if (is_vmalloc_addr(ptr))
		vfree(ptr);
	else
		kfree(ptr);
}
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(3,19,0)
#define ktime_get_real_seconds	get_seconds
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(3,19,0)
#define file_dentry(f)	((f)->f_dentry)
#elif !(LINUX_VERSION_CODE >= KERNEL_VERSION(4,5,2) || (LINUX_VERSION_CODE >= KERNEL_VERSION(4,4,8) && LINUX_VERSION_CODE < KERNEL_VERSION(4,5,0)))
#define file_dentry(f)	((f)->f_path.dentry)
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(4,4,0)
#define __GFP_DIRECT_RECLAIM	__GFP_WAIT
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,4,0)
#define huge_valid_dev(dev)	1
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(4,4,21) && !TEST_RHEL_VERSION(7,4)
/* warning: do not define inode_lock, it collides with another symbol on kernels <= 2.6.38 */
static inline void inode_unlock(struct inode *inode)
{
	mutex_unlock(&inode->i_mutex);
}
static inline int inode_trylock(struct inode *inode)
{
	return mutex_trylock(&inode->i_mutex);
}
static inline int inode_is_locked(struct inode *inode)
{
	return mutex_is_locked(&inode->i_mutex);
}
static inline void inode_lock_nested(struct inode *inode, unsigned subclass)
{
	mutex_lock_nested(&inode->i_mutex, subclass);
}
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(4,5,0) && !defined(SLAB_ACCOUNT)
#define SLAB_ACCOUNT	0
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(4,14,0)
#define bio_set_dev(bio, dev)	((bio)->bi_bdev = (dev))
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(4,14,0)
#define SB_RDONLY	MS_RDONLY
#define SB_NOATIME	MS_NOATIME
static inline int sb_rdonly(const struct super_block *sb) { return (sb->s_flags & SB_RDONLY) != 0; }
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,6,0)
#define time_t u32
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(6,3,0)
#define mnt_idmap	user_namespace
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(6,6,0)
#define inode_get_ctime(inode)	((inode)->i_ctime)
static inline void inode_set_ctime(struct inode *inode, __u64 sec, long nsec)
{
	inode->i_ctime.tv_sec = sec;
	inode->i_ctime.tv_nsec = nsec;
}
#endif

#ifndef READ_ONCE
#if defined(ACCESS_ONCE)
#define READ_ONCE	ACCESS_ONCE
#else
#define READ_ONCE(x)	(*(volatile typeof(x) *)&(x))
#endif
#endif

#ifndef WRITE_ONCE
#if defined(ACCESS_ONCE_RW)
/* grsecurity hack */
#define WRITE_ONCE(x, y)	(ACCESS_ONCE_RW(x) = (y))
#elif defined(ACCESS_ONCE)
#define WRITE_ONCE(x, y)	(ACCESS_ONCE(x) = (y))
#else
#define WRITE_ONCE(x, y)	(*(volatile typeof(x) *)&(x)) = (y)
#endif
#endif

#ifndef BIO_MAX_VECS
#define BIO_MAX_VECS	BIO_MAX_PAGES
#endif

#endif
