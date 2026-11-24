// SPDX-License-Identifier: GPL-2.0
/*
 * /dev/doracore: read-only identity node. Userspace tools (DoraCore Toolkit)
 * read it to confirm they run on a DoraCore kernel.
 */

#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>

static const char doracore_id[] = "doracore\n";

static ssize_t doracore_read(struct file *file, char __user *buf,
			     size_t count, loff_t *ppos)
{
	return simple_read_from_buffer(buf, count, ppos, doracore_id,
				       sizeof(doracore_id) - 1);
}

static const struct file_operations doracore_fops = {
	.owner	= THIS_MODULE,
	.read	= doracore_read,
	.llseek	= default_llseek,
};

static struct miscdevice doracore_dev = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "doracore",
	.fops	= &doracore_fops,
	.mode	= 0444,
};

static int __init doracore_init(void)
{
	return misc_register(&doracore_dev);
}
device_initcall(doracore_init);

MODULE_DESCRIPTION("DoraCore identity node");
MODULE_LICENSE("GPL");
