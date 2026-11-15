#include "spadfs.h"

/* 3.8.11 is the chromebook kernel */
#if LINUX_VERSION_CODE < KERNEL_VERSION(3,11,0) && LINUX_VERSION_CODE != KERNEL_VERSION(3,8,11) && !TEST_RHEL_VERSION(7,0)
#define PASS_INODE
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,38)
static int spadfs_hash_dentry(struct dentry *dentry, struct qstr *qstr)
#else
static int spadfs_hash_dentry(const struct dentry *dentry,
#ifdef PASS_INODE
			      const struct inode *inode,
#endif
			      struct qstr *qstr)
#endif
{
	unsigned i;
	unsigned long hash;
	/*printk("hash '%.*s'\n", qstr->len, qstr->name);*/
	if (unlikely((unsigned)(qstr->len - 1) > (MAX_NAME_LEN - 1)))
		return -ENAMETOOLONG;
	if (unlikely(qstr->name[0] == '^'))
		return -EINVAL;
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,8,0)
	hash = init_name_hash();
#else
	hash = init_name_hash(dentry);
#endif
	for (i = 0; i < qstr->len; i++) {
		if (unlikely(qstr->name[i] == ':'))
			return -EINVAL;
		hash = partial_name_hash(qstr->name[i] & 0xdf, hash);
	}
	qstr->hash = hash;
	return 0;
}

static int spadfs_compare_names_internal(int unx, const char *n1, unsigned l1,
					 const char *n2, unsigned l2)
{
	if (l1 != l2)
		return 1;
	if (likely(unx)) {
		return memcmp(n1, n2, l1);
	} else {
		while (l1--) {
			char c1 = *n1++;
			char c2 = *n2++;
			if (c1 >= 'a' && c1 <= 'z') c1 -= 0x20;
			if (c2 >= 'a' && c2 <= 'z') c2 -= 0x20;
			if (c1 != c2) return 1;
		}
		return 0;
	}
}

int spadfs_compare_names(SPADFS *fs, const char *n1, unsigned l1,
				     const char *n2, unsigned l2)
{
	return spadfs_compare_names_internal(!!(fs->flags_compat_none & FLAG_COMPAT_NONE_UNIX_NAMES), n1, l1, n2, l2);
}

void spadfs_set_name(SPADFS *fs, char *dest, const char *src, unsigned len)
{
	strncpy(dest, src, (len + 7) & ~7);
	if (unlikely(!(fs->flags_compat_none & FLAG_COMPAT_NONE_UNIX_NAMES))) {
		while (len--) {
			if (*dest >= 'a' && *dest <= 'z') *dest -= 0x20;
			dest++;
		}
	}
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,38)
static int spadfs_compare_dentry(struct dentry *dentry,
				 struct qstr *a, struct qstr *b)
{
	return spadfs_compare_names_internal(0,
						(const char *)a->name, a->len,
						(const char *)b->name, b->len);
}
#else
static int spadfs_compare_dentry(
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,8,0)
				 const struct dentry *p_dentry,
#endif
#ifdef PASS_INODE
				 const struct inode *p_inode,
#endif
				 const struct dentry *dentry,
#ifdef PASS_INODE
				 const struct inode *inode,
#endif
				 unsigned len, const char *a,
				 const struct qstr *b)
{
	/*printk("compare '%.*s', '%.*s'\n", len, a, b->len, b->name);*/
	return spadfs_compare_names_internal(0, a, len, b->name, b->len);
}
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,30)
const
#endif
struct dentry_operations spadfs_dops = {
	.d_hash = spadfs_hash_dentry,
	.d_compare = spadfs_compare_dentry,
};
