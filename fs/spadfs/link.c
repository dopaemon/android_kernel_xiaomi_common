#include "spadfs.h"

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,13)
static int spadfs_follow_link(struct dentry *dentry, struct nameidata *nd)
#elif LINUX_VERSION_CODE < KERNEL_VERSION(4,2,0)
static void *spadfs_follow_link(struct dentry *dentry, struct nameidata *nd)
#elif LINUX_VERSION_CODE < KERNEL_VERSION(4,5,0)
static const char *spadfs_follow_link(struct dentry *dentry, void **cookie)
#else
static const char *spadfs_follow_link(struct dentry *dentry, struct inode *inode, struct delayed_call *done)
#endif
{
	sync_lock_decl
	SPADFNODE *f;
	struct fnode_ea *lnk;
	unsigned len;
	char *str;
	int r;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,5,0)
	if (!dentry)
		return ERR_PTR(-ECHILD);
#endif

	f = spadfnode(dentry->d_inode);

	down_read_sync_lock(f->fs);
	mutex_lock(&f->file_lock);

	lnk = GET_EA((struct fnode_ea *)f->ea, f->ea_size, EA_SYMLINK_MAGIC, FNODE_EA_MAGIC_MASK);

	if (unlikely(!lnk)) {
		spadfs_error(f->fs, TXFLAGS_EA_ERROR,
			"can't find symlink extended attribute on fnode %Lx/%x",
			(unsigned long long)f->fnode_block,
			f->fnode_pos);
		r = -EFSERROR;
		goto ret_error;
	}

	if (unlikely(lnk == GET_EA_ERROR)) {
		spadfs_error(f->fs, TXFLAGS_FS_ERROR,
			"error parsing extended attributes on symlink fnode %Lx/%x",
			(unsigned long long)f->fnode_block,
			f->fnode_pos);
		r = -EFSERROR;
		goto ret_error;
	}

	len = SPAD2CPU32_LV(&lnk->magic) >> FNODE_EA_SIZE_SHIFT;
	str = kmalloc(len + 1, GFP_NOFS);
	if (unlikely(!str)) {
		r = -ENOMEM;
		goto ret_error;
	}

	memcpy(str, lnk + 1, len);
	str[len] = 0;
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,13)
	nd_set_link(nd, str);
	r = 0;
ret_error:
	mutex_unlock(&f->file_lock); up_read_sync_lock(f->fs);
	return r;
#elif LINUX_VERSION_CODE < KERNEL_VERSION(4,2,0)
	nd_set_link(nd, str);
	mutex_unlock(&f->file_lock); up_read_sync_lock(f->fs);
	return str;
ret_error:
	mutex_unlock(&f->file_lock); up_read_sync_lock(f->fs);
	return ERR_PTR(r);
#elif LINUX_VERSION_CODE < KERNEL_VERSION(4,5,0)
	mutex_unlock(&f->file_lock); up_read_sync_lock(f->fs);
	return *cookie = str;
ret_error:
	mutex_unlock(&f->file_lock); up_read_sync_lock(f->fs);
	return ERR_PTR(r);
#else
	mutex_unlock(&f->file_lock); up_read_sync_lock(f->fs);
	set_delayed_call(done, kfree_link, str);
	return str;
ret_error:
	mutex_unlock(&f->file_lock); up_read_sync_lock(f->fs);
	return ERR_PTR(r);
#endif
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,13)
static void spadfs_put_link(struct dentry *dentry, struct nameidata *nd)
{
	char *str = nd_get_link(nd);
	kfree(str);
}
#elif LINUX_VERSION_CODE < KERNEL_VERSION(4,2,0)
static void spadfs_put_link(struct dentry *dentry, struct nameidata *nd, void *str)
{
	kfree(str);
}
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,21)
const
#endif
struct inode_operations spadfs_symlink_iops = {
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,10,0)
	.readlink = generic_readlink,
#endif
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,2,0)
	.follow_link = spadfs_follow_link,
	.put_link = spadfs_put_link,
#elif LINUX_VERSION_CODE < KERNEL_VERSION(4,5,0)
	.follow_link = spadfs_follow_link,
	.put_link = kfree_put_link,
#else
	.get_link = spadfs_follow_link,
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
};
