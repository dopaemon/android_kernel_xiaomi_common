// SPDX-License-Identifier: GPL-2.0-only

#include <linux/module.h>

static struct pid *find_get_pid_global_ns(pid_t nr)
{
	struct pid *pid;

	rcu_read_lock();
	pid = get_pid(find_pid_ns(nr, &init_pid_ns));
	rcu_read_unlock();

	return pid;
}

struct gki_quirks_hook {
	const char *module_name;
	const char *symbol_name;
	void *func;
};

static const struct gki_quirks_hook gki_quirks_list[] = {
	/*
	 * Pixel 6/7: mali_kbase mistakenly uses find_get_pid
	 * with the global ids from task_struct of "current".
	 */
	{ "mali_kbase", "find_get_pid", find_get_pid_global_ns },
	// Ditto for MT6789 SoC devices, needed for Waydroid
	{ "mali_kbase_mt6789", "find_get_pid", find_get_pid_global_ns },

	{ } /* terminating entry must be last */
};

unsigned long gki_quirks_get_hooked_symbol_value(const struct module *mod,
				const char *name, unsigned long value)
{
	const struct gki_quirks_hook *hook;

	for (hook = gki_quirks_list; hook->module_name; hook++) {
		if (strcmp(mod->name, hook->module_name) == 0
			&& strcmp(name, hook->symbol_name) == 0) {
			pr_info("gki_quirks: hooking %s symbol for %s module\n",
					name, mod->name);
			return (unsigned long)hook->func;
		}
	}

	return value;
}
