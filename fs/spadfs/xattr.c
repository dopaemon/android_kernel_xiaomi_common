#include "spadfs.h"

#ifdef SPADFS_XATTR

static int spadfs_xattr_get(
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,4,0)
			    const struct xattr_handler *handler,
#endif
			    struct dentry *dentry,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,7,0)
			    struct inode *inode,
#endif
			    const char *name,
			    void *buffer, size_t buffer_size
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,4,0)
			    , int type
#endif
			    )
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,7,0)
	SPADFNODE *f = spadfnode(dentry->d_inode);
#else
	SPADFNODE *f = spadfnode(inode);
#endif
	SPADFS *fs = f->fs;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,4,0)
	int type = handler->flags;
#endif

	int r;

	struct fnode_ea *xat;
	unsigned xat_size;

	u8 *found;

	sync_lock_decl

	down_read_sync_lock(fs);

	xat = spadfs_get_ea(f, EA_XATTR_MAGIC, EA_XATTR_MAGIC_MASK);
	if (!xat) {
		r = -ENODATA;
		goto ret_r;
	}
	if (unlikely(IS_ERR(xat))) {
		r = PTR_ERR(xat);
		goto ret_r;
	}

	xat_size = (SPAD2CPU32_LV(&xat->magic) >> FNODE_EA_SIZE_SHIFT) &
		   FNODE_EA_SIZE_MASK_1;
	found = GET_XAT((u8 *)(xat + 1), xat_size, GET_XAT_TYPE_NAME,
			type, name, strlen(name));
	if (unlikely(found == GET_XAT_ERROR)) {
		spadfs_error(fs, TXFLAGS_EA_ERROR, "XAT extended attribute error on fnode %Lx/%x",
			(unsigned long long)f->fnode_block, f->fnode_pos);
		r = -EFSERROR;
		goto ret_r;
	}
	if (!found) {
		r = -ENODATA;
		goto ret_r;
	}
	if (buffer) {
		if (unlikely(found[2] > buffer_size)) {
			r = -ERANGE;
			goto ret_r;
		}
		memcpy(buffer, found + 3 + found[1], found[2]);
	}
	r = found[2];

ret_r:
	up_read_sync_lock(fs);

	return r;
#undef fs
}

static int spadfs_xattr_set(
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,4,0)
			    const struct xattr_handler *handler,
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,12,0)
			    struct mnt_idmap *ns,
#endif
			    struct dentry *dentry,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,7,0)
			    struct inode *inode,
#endif
			    const char *name,
			    const void *value, size_t valuelen, int flags
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,4,0)
			    , int type
#endif
			    )
{
	SPADFNODE *f = spadfnode(dentry->d_inode);
	SPADFS *fs = f->fs;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,4,0)
	int type = handler->flags;
#endif

	unsigned namelen = strlen(name);

	u8 *ea;
	unsigned ea_size;
	int r;

	struct fnode_ea *xat;
	unsigned xat_size;

	u8 *found;

	ea = kmalloc(FNODE_MAX_EA_SIZE, GFP_NOIO);
	if (unlikely(!ea))
		return -ENOMEM;

	down_write_sync_lock(fs);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,7,0)
	if (unlikely(dentry->d_inode != inode)) {
		r = -EINVAL;
		goto ret_r;
	}
#endif

	/* ext2 has these tests for NULL too, I don't know why */
	if (unlikely(!name)) {
		r = -EINVAL;
		goto ret_r;
	}
	if (unlikely(!value))
		valuelen = 0;

	if (unlikely(namelen > 0xff) || unlikely(valuelen > 0xff)) {
		r = -ERANGE;
		goto ret_r;
	}

	if (unlikely(!namelen)) {
		r = -EINVAL;
		goto ret_r;
	}

	ea_size = f->ea_size;
	memcpy(ea, f->ea, ea_size);

	xat = GET_EA((struct fnode_ea *)ea, ea_size, EA_XATTR_MAGIC, EA_XATTR_MAGIC_MASK);
	if (unlikely(xat == GET_EA_ERROR)) {
		spadfs_error(fs, TXFLAGS_FS_ERROR,
			"error parsing extended attributes on fnode %Lx/%x",
			(unsigned long long)f->fnode_block, f->fnode_pos);
		r = -EFSERROR;
		goto ret_r;
	}
	if (!xat) {
		const unsigned new_ea_size = FNODE_EA_DO_ALIGN(sizeof(struct fnode_ea));
		if (unlikely(ea_size + new_ea_size > FNODE_MAX_EA_SIZE)) {
			r = -ENOSPC;
			goto ret_r;
		}
		memset(ea + ea_size, 0, new_ea_size);
		xat = (struct fnode_ea *)(ea + ea_size);
		CPU2SPAD32_LV(&xat->magic, EA_XATTR_MAGIC);
		ea_size += new_ea_size;
	}

	xat_size = (SPAD2CPU32_LV(&xat->magic) >> FNODE_EA_SIZE_SHIFT) &
		   FNODE_EA_SIZE_MASK_1;

	found = GET_XAT((u8 *)(xat + 1), xat_size, GET_XAT_TYPE_NAME,
			type, name, namelen);
	if (unlikely(found == GET_XAT_ERROR)) {
		spadfs_error(fs, TXFLAGS_EA_ERROR, "XAT extended attribute error on fnode %Lx/%x",
			(unsigned long long)f->fnode_block, f->fnode_pos);
		r = -EFSERROR;
		goto ret_r;
	}
	if (found) {
		u8 *end;
		unsigned shrink;
		if (unlikely(flags & XATTR_CREATE)) {
			r = -EEXIST;
			goto ret_r;
		}
		if (valuelen == found[2])
			goto set_just_value;
		end = (u8 *)(xat + 1) + xat_size;
		shrink = 3 + found[1] + found[2];
		memmove(found, found + shrink, end - (found + shrink));
		r = RESIZE_EA(ea, &ea_size, xat, xat_size - shrink);
		if (unlikely(r)) {
			goto ret_r;
		}
		xat_size -= shrink;
		if (!valuelen) {
			if (!xat_size) {
				REMOVE_EA(ea, &ea_size, xat);
			}
			goto refile_ret;
		}
		goto add_new_key;
	} else {
		if (unlikely(flags & XATTR_REPLACE)) {
			r = -ENODATA;
			goto ret_r;
		}
		if (!valuelen) {
			r = 0;
			goto ret_r;
		}
add_new_key:
		r = RESIZE_EA(ea, &ea_size, xat, xat_size + 3 + namelen + valuelen);
		if (unlikely(r)) {
			goto ret_r;
		}
		found = (u8 *)(xat + 1) + xat_size;
	}

	found[0] = type;
	found[1] = namelen;
	found[2] = valuelen;
	memcpy(found + 3, name, namelen);
set_just_value:
	memcpy(found + 3 + namelen, value, valuelen);

refile_ret:
	r = spadfs_refile_fnode(spadfnode(dentry->d_parent->d_inode), &dentry->d_name, f, ea, ea_size);

ret_r:
	up_write_sync_lock(fs);
	kfree(ea);

	return r;
#undef fs
}

ssize_t spadfs_listxattr(struct dentry *dentry, char *list, size_t list_size)
{
	SPADFNODE *f = spadfnode(dentry->d_inode);
	SPADFS *fs = f->fs;

	int r;

	struct fnode_ea *xat;

	u8 *found, *end;
	unsigned xat_size;

	sync_lock_decl

	down_read_sync_lock(fs);

	xat = spadfs_get_ea(f, EA_XATTR_MAGIC, EA_XATTR_MAGIC_MASK);
	if (!xat) {
		r = 0;
		goto ret_r;
	}
	if (unlikely(IS_ERR(xat))) {
		r = PTR_ERR(xat);
		goto ret_r;
	}

	xat_size = (SPAD2CPU32_LV(&xat->magic) >> FNODE_EA_SIZE_SHIFT) &
		   FNODE_EA_SIZE_MASK_1;
	found = (u8 *)(xat + 1);
	end = found + xat_size;
	r = 0;
	while (1) {
		unsigned len, prefixlen;
		const struct xattr_handler **handler_p;

		found = GET_XAT(found, end - found, GET_XAT_ALL, 0, NULL, 0);
		if (unlikely(found == GET_XAT_ERROR)) {
			spadfs_error(fs, TXFLAGS_EA_ERROR, "XAT extended attribute error on fnode %Lx/%x",
				(unsigned long long)f->fnode_block, f->fnode_pos);
			r = -EFSERROR;
			goto ret_r;
		}
		if (!found)
			break;

		if (found[0] == SPADFS_XATTR_TRUSTED && !capable(CAP_SYS_ADMIN))
			goto skip_this;

		for (handler_p = spadfs_xattr_handlers; *handler_p; handler_p++)
			if ((*handler_p)->flags == found[0])
				goto found_handler;
		goto skip_this;
found_handler:

		prefixlen = strlen((*handler_p)->prefix);

		len = prefixlen + found[1] + 1;

		if (!list) {
			r += len;
		} else {
			if (list_size < len) {
				r = -ERANGE;
				goto ret_r;
			}
			memcpy(list, (*handler_p)->prefix, prefixlen);
			memcpy(list + prefixlen, found + 3, found[1]);
			list[prefixlen + found[1]] = 0;
			list_size -= len;
			list += len;
			r += len;
		}

skip_this:
		found += 3 + found[1] + found[2];
	}

ret_r:
	up_read_sync_lock(fs);

	return r;
#undef fs
}

static const struct xattr_handler spadfs_xattr_user_handler = {
	.prefix	= XATTR_USER_PREFIX,
	.get	= spadfs_xattr_get,
	.set	= spadfs_xattr_set,
	.flags	= SPADFS_XATTR_USER,
};

static const struct xattr_handler spadfs_xattr_security_handler = {
	.prefix	= XATTR_SECURITY_PREFIX,
	.get	= spadfs_xattr_get,
	.set	= spadfs_xattr_set,
	.flags	= SPADFS_XATTR_SECURITY,
};

static const struct xattr_handler spadfs_xattr_trusted_handler = {
	.prefix	= XATTR_TRUSTED_PREFIX,
	.get	= spadfs_xattr_get,
	.set	= spadfs_xattr_set,
	.flags	= SPADFS_XATTR_TRUSTED,
};

const struct xattr_handler *spadfs_xattr_handlers[] = {
	&spadfs_xattr_user_handler,
	&spadfs_xattr_security_handler,
	&spadfs_xattr_trusted_handler,
	NULL
};

#endif
