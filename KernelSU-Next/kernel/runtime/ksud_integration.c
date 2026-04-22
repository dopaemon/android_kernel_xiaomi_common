#include <linux/rcupdate.h>
#include <linux/slab.h>
#include <asm/current.h>
#include <linux/compat.h>
#include <linux/cred.h>
#include <linux/dcache.h>
#include <linux/err.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/version.h>
#include <linux/input-event-codes.h>
#include <linux/printk.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/namei.h>
#include <linux/workqueue.h>
#include <linux/uio.h>

#include "arch.h"
#include "klog.h" // IWYU pragma: keep
#include "ksu.h"
#include "runtime/ksud.h"
#include "runtime/ksud_boot.h"
#include "selinux/selinux.h"

bool ksu_init_rc_hook __read_mostly = true;
bool ksu_execveat_hook __read_mostly = true;
bool ksu_input_hook __read_mostly = true;

// clang-format off
static const char KERNEL_SU_RC[] =
    "\n"
    "on post-fs-data\n"
    "    start logd\n"
    // We should wait for the post-fs-data finish
    "    exec u:r:" KERNEL_SU_DOMAIN ":s0 root -- " KSUD_PATH " post-fs-data\n"
    "\n"
    "on nonencrypted\n"
    "    exec u:r:" KERNEL_SU_DOMAIN ":s0 root -- " KSUD_PATH " services\n"
    "\n"
    "on property:vold.decrypt=trigger_restart_framework\n"
    "    exec u:r:" KERNEL_SU_DOMAIN ":s0 root -- " KSUD_PATH " services\n"
    "\n"
    "on property:sys.boot_completed=1\n"
    "    exec u:r:" KERNEL_SU_DOMAIN ":s0 root -- " KSUD_PATH " boot-completed\n"
    "\n"
    "\n";
// clang-format on

static const char __user *get_user_arg_ptr(struct user_arg_ptr argv, int nr)
{
	const char __user *native;

#ifdef CONFIG_COMPAT
	if (unlikely(argv.is_compat)) {
		compat_uptr_t compat;

		if (get_user(compat, argv.ptr.compat + nr))
			return ERR_PTR(-EFAULT);

		return compat_ptr(compat);
	}
#endif

	if (get_user(native, argv.ptr.native + nr))
		return ERR_PTR(-EFAULT);

	return native;
}

/*
 * count() counts the number of strings in array ARGV.
 */

/*
 * Make sure old GCC compiler can use __maybe_unused,
 * Test passed in 4.4.x ~ 4.9.x when use GCC.
 */

static int __maybe_unused count(struct user_arg_ptr argv, int max)
{
    int i = 0;

    if (argv.ptr.native != NULL) {
        for (;;) {
            const char __user *p = get_user_arg_ptr(argv, i);

            if (!p)
                break;

            if (IS_ERR(p))
                return -EFAULT;

            if (i >= max)
                return -E2BIG;
            ++i;

            if (fatal_signal_pending(current))
                return -ERESTARTNOHAND;
        }
    }
    return i;
}

static bool check_argv(struct user_arg_ptr argv, int index, const char *expected, char *buf, size_t buf_len)
{
    const char __user *p;
    int argc;

    argc = count(argv, MAX_ARG_STRINGS);
    if (argc <= index)
        return false;

    p = get_user_arg_ptr(argv, index);
    if (!p || IS_ERR(p))
        goto fail;

    if (strncpy_from_user(buf, p, buf_len) <= 0)
        goto fail;

    buf[buf_len - 1] = '\0';
    return !strcmp(buf, expected);

fail:
    pr_err("check_argv failed\n");
    return false;
}

extern int ksu_handle_execveat_init(struct filename *filename, struct user_arg_ptr *argv_user, struct user_arg_ptr *envp_user);

// IMPORTANT NOTE: the call from execve_handler_pre WON'T provided correct value for envp and flags in GKI version
int ksu_handle_execveat_ksud(int *fd, struct filename **filename_ptr,
                             struct user_arg_ptr *argv,
                             struct user_arg_ptr *envp, int *flags)
{
    struct filename *filename;
    static const char app_process[] = "/system/bin/app_process";
    static bool first_zygote = true;

    /* This applies to versions Android 10+ */
    static const char system_bin_init[] = "/system/bin/init";
    static bool init_second_stage_executed = false;

    if (!filename_ptr)
        return 0;

    filename = *filename_ptr;
    if (IS_ERR(filename)) {
        return 0;
    }

    // https://cs.android.com/android/platform/superproject/+/android-16.0.0_r2:system/core/init/main.cpp;l=77
    if (unlikely(!memcmp(filename->name, system_bin_init, sizeof(system_bin_init) - 1) && argv))
    {
        char buf[16];
        if (!init_second_stage_executed &&
            check_argv(*argv, 1, "second_stage", buf, sizeof(buf)))
        {
            pr_info("/system/bin/init second_stage executed\n");
            apply_kernelsu_rules();
            cache_sid();
            setup_ksu_cred();
            init_second_stage_executed = true;
        }
    }

    if (unlikely(first_zygote && !memcmp(filename->name, app_process, sizeof(app_process) - 1) && argv))
    {
        char buf[16];
        if (check_argv(*argv, 1, "-Xzygote", buf, sizeof(buf))) {
            pr_info("exec zygote, /data prepared, second_stage: %d\n", init_second_stage_executed);
            on_post_fs_data();
            first_zygote = false;
            ksu_execveat_hook = false;
            pr_info("ksu_execveat_hook: %d\n", ksu_execveat_hook);
        }
    }

    // - We need to run ksu_handle_execveat_init() at the very end in case the above checks are skipped
    (void)ksu_handle_execveat_init(filename, argv, envp);

    return 0;
}

static ssize_t (*orig_read)(struct file *, char __user *, size_t, loff_t *);
static ssize_t (*orig_read_iter)(struct kiocb *, struct iov_iter *);
static struct file_operations fops_proxy;
static ssize_t ksu_rc_pos = 0;
const size_t ksu_rc_len = sizeof(KERNEL_SU_RC) - 1;

// https://cs.android.com/android/platform/superproject/main/+/main:system/core/init/parser.cpp;l=144;drc=61197364367c9e404c7da6900658f1b16c42d0da
// https://cs.android.com/android/platform/superproject/main/+/main:system/libbase/file.cpp;l=241-243;drc=61197364367c9e404c7da6900658f1b16c42d0da
// The system will read init.rc file until EOF, whenever read() returns 0,
// so we begin append ksu rc when we meet EOF.

static ssize_t read_proxy(struct file *file, char __user *buf, size_t count, loff_t *pos)
{
    ssize_t ret = 0;
    size_t append_count;
    if (ksu_rc_pos && ksu_rc_pos < ksu_rc_len)
        goto append_ksu_rc;

    ret = orig_read(file, buf, count, pos);
    if (ret != 0 || ksu_rc_pos >= ksu_rc_len) {
        return ret;
    } else {
        pr_info("read_proxy: orig read finished, start append rc\n");
    }
append_ksu_rc:
    append_count = ksu_rc_len - ksu_rc_pos;
    if (append_count > count - ret)
        append_count = count - ret;
    // copy_to_user returns the number of not copied
    if (copy_to_user(buf + ret, KERNEL_SU_RC + ksu_rc_pos, append_count)) {
        pr_info("read_proxy: append error, totally appended %ld\n", ksu_rc_pos);
    } else {
        pr_info("read_proxy: append %ld\n", append_count);

        ksu_rc_pos += append_count;
        if (ksu_rc_pos == ksu_rc_len) {
            pr_info("read_proxy: append done\n");
        }
        ret += append_count;
    }

    return ret;
}

static ssize_t read_iter_proxy(struct kiocb *iocb, struct iov_iter *to)
{
    ssize_t ret = 0;
    size_t append_count;
    if (ksu_rc_pos && ksu_rc_pos < ksu_rc_len)
        goto append_ksu_rc;

    ret = orig_read_iter(iocb, to);
    if (ret != 0 || ksu_rc_pos >= ksu_rc_len) {
        return ret;
    } else {
        pr_info("read_iter_proxy: orig read finished, start append rc\n");
    }
append_ksu_rc:
    // copy_to_iter returns the number of copied bytes
    append_count = copy_to_iter(KERNEL_SU_RC + ksu_rc_pos, ksu_rc_len - ksu_rc_pos, to);
    if (!append_count) {
        pr_info("read_iter_proxy: append error, totally appended %ld\n", ksu_rc_pos);
    } else {
        pr_info("read_iter_proxy: append %ld\n", append_count);

        ksu_rc_pos += append_count;
        if (ksu_rc_pos == ksu_rc_len) {
            pr_info("read_iter_proxy: append done\n");
        }
        ret += append_count;
    }
    return ret;
}

static bool is_init_rc(struct file *fp)
{
    if (strcmp(current->comm, "init")) {
        // we are only interest in `init` process
        return false;
    }

    if (!d_is_reg(fp->f_path.dentry)) {
        return false;
    }

    const char *short_name = fp->f_path.dentry->d_name.name;
    if (strcmp(short_name, "init.rc")) {
        // we are only interest `init.rc` file name file
        return false;
    }
    char path[256];
    char *dpath = d_path(&fp->f_path, path, sizeof(path));

    if (IS_ERR(dpath)) {
        return false;
    }

    if (strcmp(dpath, "/system/etc/init/hw/init.rc")) {
        return false;
    }

    return true;
}

static void ksu_install_rc_hook(struct file *file)
{
    if (!is_init_rc(file)) {
        return;
    }

    // we only process the first read
    static bool rc_hooked = false;
    if (rc_hooked) {
        // we don't need these hooks, unregister it!

        return;
    }
    rc_hooked = true;

    ksu_init_rc_hook = false;
    pr_info("ksu_init_rc_hook: %d\n", ksu_init_rc_hook);

    // now we can sure that the init process is reading
    // `/system/etc/init/init.rc`

    pr_info("read init.rc, comm: %s, rc_count: %zu\n", current->comm, ksu_rc_len);

    // Now we need to proxy the read and modify the result!
    // But, we can not modify the file_operations directly, because it's in read-only memory.
    // We just replace the whole file_operations with a proxy one.
    memcpy(&fops_proxy, file->f_op, sizeof(struct file_operations));
    orig_read = file->f_op->read;
    if (orig_read) {
        fops_proxy.read = read_proxy;
    }
    orig_read_iter = file->f_op->read_iter;
    if (orig_read_iter) {
        fops_proxy.read_iter = read_iter_proxy;
    }
    // replace the file_operations
    file->f_op = &fops_proxy;
}

void ksu_handle_sys_read(unsigned int fd, char __user **buf_ptr, size_t *count_ptr)
{
    struct file *file = fget(fd);
    if (!file) {
        return;
    }
    ksu_install_rc_hook(file);
    fput(file);
}

static unsigned int volumedown_pressed_count = 0;

static bool is_volumedown_enough(unsigned int count)
{
    return count >= 3;
}

int ksu_handle_input_handle_event(unsigned int *type, unsigned int *code, int *value)
{
    if (!ksu_input_hook)
        return 0;

    if (*type == EV_KEY && *code == KEY_VOLUMEDOWN) {
        int val = *value;
        pr_info("KEY_VOLUMEDOWN val: %d\n", val);
        if (val) {
            // key pressed, count it
            volumedown_pressed_count += 1;
            if (is_volumedown_enough(volumedown_pressed_count)) {
                ksu_input_hook = false;
                pr_info("ksu_input_hook: %d\n", ksu_input_hook);
            }
        }
    }

    return 0;
}

bool ksu_is_safe_mode()
{
    static bool safe_mode = false;
    if (safe_mode) {
        // don't need to check again, userspace may call multiple times
        return true;
    }

    // stop hook first!
    ksu_input_hook = false;
    pr_info("ksu_input_hook: %d\n", ksu_input_hook);

    pr_info("volumedown_pressed_count: %d\n", volumedown_pressed_count);
    if (is_volumedown_enough(volumedown_pressed_count)) {
        // pressed over 3 times
        pr_info("KEY_VOLUMEDOWN pressed max times, safe mode detected!\n");
        safe_mode = true;
        return true;
    }

    return false;
}

void ksu_handle_vfs_fstat(int fd, loff_t *kstat_size_ptr)
{
    loff_t new_size = *kstat_size_ptr + ksu_rc_len;
    struct file *file = fget(fd);

    if (!file)
        return;

    if (is_init_rc(file)) {
        pr_info("stat init.rc");
        pr_info("adding ksu_rc_len: %lld -> %lld", *kstat_size_ptr, new_size);
        *kstat_size_ptr = new_size;
    }
    fput(file);
}

// ksud: module support
void __init ksu_ksud_init()
{
}

void __exit ksu_ksud_exit()
{
}
