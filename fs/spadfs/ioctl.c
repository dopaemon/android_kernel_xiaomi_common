#include "spadfs.h"

long spadfs_ioctl(struct file *file, unsigned cmd, unsigned long arg)
{
	switch (cmd) {
#ifdef SPADFS_FSTRIM
		case FITRIM: {
			SPADFS *fs = spadfs(file_inode(file)->i_sb);
			struct fstrim_range range;
			sector_t n_trimmed;
			int r;

			if (!capable(CAP_SYS_ADMIN))
				return -EPERM;
			if (copy_from_user(&range, (struct fstrim_range __user *)arg, sizeof(range)))
				return -EFAULT;
			r = spadfs_trim_fs(fs, range.start >> 9, (range.start + range.len) >> 9, (range.minlen + 511) >> 9, &n_trimmed);
			if (r)
				return r;
			range.len = (u64)n_trimmed << 9;
			if (copy_to_user((struct fstrim_range __user *)arg, &range, sizeof(range)))
				return -EFAULT;
			return 0;
		}
#endif
		default: {
			return -ENOIOCTLCMD;
		}
	}
}

#ifdef CONFIG_COMPAT
long spadfs_compat_ioctl(struct file *file, unsigned cmd, unsigned long arg)
{
	return spadfs_ioctl(file, cmd, (unsigned long)compat_ptr(arg));
}
#endif
