// SPDX-License-Identifier: GPL-2.0-only

#include <linux/module.h>

struct gki_quirks_hook {
	const char *module_name;
	const char *symbol_name;
	void *func;
};

static const struct gki_quirks_hook gki_quirks_list[] = {
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
