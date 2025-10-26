#include <linux/module.h>
#include <linux/init.h>
#include <linux/string.h>
#include <linux/version.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/atomic.h>
#include <linux/smp.h>
#include <linux/printk.h>

#ifdef MODULE
#error "only build me as built-in (CONFIG_MAP_SPOOF=y)"
#endif

#define DEF_MAGIC 0x11111111
#define CMD_ADD_TO_LIST 11001
#define CMD_DESTROY_LIST 11002
#define CMD_ENABLE_SKIP_RWXP 11003
#define CMD_DISABLE_SKIP_RWXP 11004

struct string_entry {
    char *string;
    struct list_head list;
};
LIST_HEAD(string_list);

atomic_t skip_rwxp = ATOMIC_INIT(0);
EXPORT_SYMBOL(skip_rwxp);

static void __exit mapspoof_exit(void) {}

// SYSCALL_DEFINE4(reboot, int, magic1, int, magic2, unsigned int, cmd,
//		void __user *, arg)
// lkm_handle_sys_reboot(magic1, magic2, cmd, arg);
// PLAN
// magic1 main magic
// magic2 command
// cmd, unusable as ptr on 64-bit :(, maybe can be used as delimiter of some sort
// arg, data input, already user ptr so good

int lkm_handle_sys_reboot(int magic1, int magic2, unsigned int cmd, void __user **arg)
{
	int ok = DEF_MAGIC;

	if (magic1 != ok)
		return 0;

	pr_info("map_spoof: intercepted call! magic: 0x%x id: %d\n", magic1, magic2);

	if (magic2 == CMD_ADD_TO_LIST) {
		char buf[256] = {0};

		struct string_entry *new_entry, *entry;
		if (copy_from_user(buf, (const char __user *)*arg, sizeof(buf) - 1))
			return 0;

		buf[255] = '\0';

		new_entry = kmalloc(sizeof(*new_entry), GFP_KERNEL);
		if (!new_entry)
			return 0;

		new_entry->string = kstrdup(buf, GFP_KERNEL);
		if (!new_entry->string) {
			kfree(new_entry);
			return 0;
		}

		list_for_each_entry(entry, &string_list, list) {
			if (!strcmp(entry->string, buf)) {
				pr_info("map_spoof: %s is already here!\n", buf);
				kfree(new_entry->string);
				kfree(new_entry);
				return 0;
			}
		}

		pr_info("map_spoof: entry %s added!\n", buf);
		list_add(&new_entry->list, &string_list);
		smp_mb();

		if (copy_to_user((void __user *)*arg, &ok, sizeof(ok)))
			return 0;

	}

	if (magic2 == CMD_DESTROY_LIST) {
		struct string_entry *entry, *tmp;

		list_for_each_entry_safe(entry, tmp, &string_list, list) {
        		pr_info("map_spoof: entry %s removed!\n", entry->string);
        		list_del(&entry->list);
        		kfree(entry->string);
        		kfree(entry);
        	}
        	smp_mb();

		if (copy_to_user((void __user *)*arg, &ok, sizeof(ok)))
			return 0;

	}

	if (magic2 == CMD_ENABLE_SKIP_RWXP) {
		atomic_set(&skip_rwxp, 1);
		pr_info("map_spoof: skip_rwxp: 1\n");

		if (copy_to_user((void __user *)*arg, &ok, sizeof(ok)))
			return 0;
	}

	if (magic2 == CMD_DISABLE_SKIP_RWXP) {
		atomic_set(&skip_rwxp, 0);
		pr_info("map_spoof: skip_rwxp: 0\n");

		if (copy_to_user((void __user *)*arg, &ok, sizeof(ok)))
			return 0;

	}

	return 0;
}

static int __init mapspoof_init(void) 
{
	pr_info("map_spoof: init with magic: 0x%x\n", (int)DEF_MAGIC);
	return 0;
}

module_init(mapspoof_init);
module_exit(mapspoof_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("xx");
MODULE_DESCRIPTION("map spoof handler");

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 0, 0)
MODULE_IMPORT_NS(VFS_internal_I_am_really_a_filesystem_and_am_NOT_a_driver);
#endif
