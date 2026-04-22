#include <linux/compiler.h>
#include <linux/version.h>
#include <linux/slab.h>
#include <linux/task_work.h>
#include <linux/thread_info.h>
#include <linux/seccomp.h>
#include <linux/printk.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/uidgid.h>
#include <linux/susfs_def.h>
#include "selinux/selinux.h"

#include "policy/allowlist.h"
#include "hook/setuid_hook.h"
#include "klog.h" // IWYU pragma: keep
#include "manager/manager_identity.h"
#include "infra/seccomp_cache.h"
#include "supercall/supercall.h"
#include "feature/kernel_umount.h"

extern u32 susfs_zygote_sid;
extern struct cred *ksu_cred;

#ifdef CONFIG_KSU_SUSFS_SUS_PATH
extern void susfs_run_sus_path_loop(void);
#endif // #ifdef CONFIG_KSU_SUSFS_SUS_PATH

static void ksu_handle_extra_susfs_work(void)
{
    const struct cred *saved = override_creds(ksu_cred);

#ifdef CONFIG_KSU_SUSFS_SUS_PATH
    susfs_run_sus_path_loop();
#endif // #ifdef CONFIG_KSU_SUSFS_SUS_PATH

    revert_creds(saved);
}

int ksu_handle_setresuid(uid_t ruid, uid_t euid, uid_t suid)
{
    // we rely on the fact that zygote always call setresuid(3) with same uids
    uid_t new_uid = ruid;
    uid_t old_uid = current_uid().val;

    // We only interest in process spwaned by zygote
    if (!susfs_is_sid_equal(current_cred(), susfs_zygote_sid))
        return 0;

    // Check if spawned process is isolated service first, and force to do umount if so
    if (is_isolated_process(new_uid))
        goto do_umount;

    // - Since ksu maanger app uid is excluded in allow_list_arr, so ksu_uid_should_umount(manager_uid)
    //   will always return true, that's why we need to explicitly check if new_uid belongs to
    //   ksu manager
    if (is_uid_manager(new_uid)) {
        spin_lock_irq(&current->sighand->siglock);
        ksu_seccomp_allow_cache(current->seccomp.filter, __NR_reboot);
        spin_unlock_irq(&current->sighand->siglock);

        pr_info("install fd for manager: %d\n", new_uid);
        ksu_install_fd();
        return 0;
    }
    if (unlikely(new_uid == WEBVIEW_ZYGOTE_UID)) {
        // we should not umount for webview zygote
        return 0;
    }

    // Check if spawned process is normal user app and needs to be umounted
    if (likely(is_appuid(new_uid) && ksu_uid_should_umount(new_uid)))
        goto do_umount;

    if (ksu_is_allow_uid_for_current(new_uid)) {
        if (current->seccomp.mode == SECCOMP_MODE_FILTER &&
            current->seccomp.filter) {
            spin_lock_irq(&current->sighand->siglock);
            ksu_seccomp_allow_cache(current->seccomp.filter, __NR_reboot);
            spin_unlock_irq(&current->sighand->siglock);
        }
    }
    return 0;

do_umount:
    // Handle kernel umount
    ksu_handle_umount(old_uid, new_uid);

    // Handle extra susfs work
    ksu_handle_extra_susfs_work();

    // Mark current proc as umounted
    susfs_set_current_proc_umounted();

    return 0;
}

void __init ksu_setuid_hook_init(void)
{
    ksu_kernel_umount_init();
}

void __exit ksu_setuid_hook_exit(void)
{
    pr_info("ksu_core_exit\n");
    ksu_kernel_umount_exit();
}
