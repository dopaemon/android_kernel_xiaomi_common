// SPDX-License-Identifier: GPL-2.0
/*
 * Boot watchdog: panic if userspace never disarms it.
 *
 * Debugging aid for boot hangs on devices without a serial console. If the
 * watchdog is not disarmed within boot_watchdog.timeout seconds, dump the
 * blocked tasks, all CPU backtraces and the workqueue state, then panic so
 * that the kmsg dumpers (ramoops, mtdoops) persist the log of the hung boot.
 *
 * Disarm it once the system has booted:
 *   echo 0 > /sys/module/boot_watchdog/parameters/timeout
 */

#define pr_fmt(fmt) "boot_watchdog: " fmt

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/moduleparam.h>
#include <linux/nmi.h>
#include <linux/panic.h>
#include <linux/sched/debug.h>
#include <linux/workqueue.h>

static unsigned int timeout = CONFIG_BOOT_WATCHDOG_TIMEOUT;
static bool started;

static void boot_watchdog_fire(struct work_struct *work)
{
	pr_emerg("boot did not complete within %u seconds\n", timeout);

	pr_emerg("blocked tasks:\n");
	show_state_filter(TASK_UNINTERRUPTIBLE);

	pr_emerg("CPU backtraces:\n");
	if (!trigger_all_cpu_backtrace())
		dump_stack();

	pr_emerg("workqueues:\n");
	show_all_workqueues();

	panic("boot watchdog expired after %u seconds", timeout);
}

static DECLARE_DELAYED_WORK(boot_watchdog_work, boot_watchdog_fire);

static void boot_watchdog_arm(void)
{
	if (timeout)
		mod_delayed_work(system_unbound_wq, &boot_watchdog_work,
				 msecs_to_jiffies(timeout * MSEC_PER_SEC));
	else if (cancel_delayed_work_sync(&boot_watchdog_work))
		pr_info("disarmed\n");
}

static int boot_watchdog_set_timeout(const char *val,
				     const struct kernel_param *kp)
{
	int ret = param_set_uint(val, kp);

	if (!ret && READ_ONCE(started))
		boot_watchdog_arm();
	return ret;
}

static const struct kernel_param_ops boot_watchdog_timeout_ops = {
	.set = boot_watchdog_set_timeout,
	.get = param_get_uint,
};
module_param_cb(timeout, &boot_watchdog_timeout_ops, &timeout, 0644);
MODULE_PARM_DESC(timeout, "Seconds until panic, 0 disarms the watchdog");

static int __init boot_watchdog_init(void)
{
	WRITE_ONCE(started, true);
	if (timeout)
		pr_info("armed, panic in %u seconds unless disarmed\n",
			timeout);
	boot_watchdog_arm();
	return 0;
}
late_initcall(boot_watchdog_init);
