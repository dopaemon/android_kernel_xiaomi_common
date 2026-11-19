// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */


#include <linux/cdev.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/msm_ion.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <soc/qcom/qseecom_scm.h>
#include "tornado_mod.h"

#define MDX "tornado"
#define RPMB_STR "tzbps_hlos_update_rollback_version"
#define TORNADO_FIRST_MINOR 0
#define TORNADO_MINOR_CNT 1

static struct cdev cdev_tornado;
static dev_t dev_tornado;
static struct class *class_tornado;

static int tornado_module_open(struct inode *i, struct file *f)
{
	pr_alert("tornado : driver open\n");
	return 0;
}

static int tornado_module_close(struct inode *i, struct file *f)
{
	pr_alert("tornado : driver close\n");
	return 0;
}

static long tornado_ioctl(struct file *f, unsigned int cmd,
		unsigned long arg)
{
	int ret = 0;
	struct qseecom_scm_desc desc = {0};
	uint32_t smc_id = 0;

	switch (cmd) {
	case GET_ROLLBACK_VERSION_ID: {
		struct param_req usr_data = { 0 };

		pr_alert("get anti roll back version\n");
		memset(&desc, 0x0, sizeof(struct qseecom_scm_desc));
		memset(&usr_data, 0x0, sizeof(usr_data));
		if (copy_from_user(&usr_data, (void __user *)arg,
				sizeof(usr_data))) {
			pr_alert("copy_from_user failed for args\n");
			return -EFAULT;
		}
		smc_id = TZ_PIL_GET_ROLLBACK_VERSION_ID;
		if (usr_data.param0 == SECBOOT_QSEE_SW_TYPE) {
			desc.args[0] = usr_data.param0;
			desc.arginfo = SCM_ARGS(1, SCM_VAL);
			ret = qcom_scm_qseecom_call(smc_id, &desc, false);
			if (ret) {
				pr_alert("scm call failed ret %d ", ret);
				break;
			}
			pr_alert("smc_id 0x%x\n", smc_id);
			pr_alert(
				"Response value from %s for SWID: SECBOOT_QSEE_SW_TYPE - Fuse: 0x%llx\n",
				RPMB_STR, desc.ret[0]);
		} else if (usr_data.param0 == SECBOOT_CDSP_SW_TYPE) {
			desc.args[0] = usr_data.param0;
			desc.arginfo = SCM_ARGS(1, SCM_VAL);
			ret = qcom_scm_qseecom_call(smc_id, &desc, false);
			if (ret) {
				pr_alert("scm call failed ret %d ", ret);
				break;
			}
			pr_alert("smc_id 0x%x\n", smc_id);
			pr_alert(
				"Response value from %s for SWID: SECBOOT_CDSP_SW_TYPE - Fuse: 0x%llx\n",
				RPMB_STR, desc.ret[0]);
		} else if (usr_data.param0 == SECBOOT_Q6_CDSP_DTB_SW_TYPE) {
			desc.args[0] = usr_data.param0;
			desc.arginfo = SCM_ARGS(1, SCM_VAL);
			ret = qcom_scm_qseecom_call(smc_id, &desc, false);
			if (ret) {
				pr_alert("scm call failed ret %d ", ret);
				break;
			}
			pr_alert("smc_id 0x%x\n", smc_id);
			pr_alert(
				"Response value from %s for SWID: SECBOOT_Q6_CDSP_DTB_SW_TYPE - Fuse: 0x%llx\n",
				RPMB_STR, desc.ret[0]);
		}
	break;
				}

	case GET_SECURE_STATE: {
		struct tornado_data usr_data = { 0 };

		pr_alert("get secure state of the device\n");
		memset(&desc, 0x0, sizeof(struct qseecom_scm_desc));

		smc_id = TZ_INFO_GET_SECURE_STATE;
		desc.arginfo = SCM_ARGS(0, SCM_VAL);
		ret = qcom_scm_qseecom_call(smc_id, &desc, true);
		if (ret) {
			pr_alert("scm call failed ret %d ", ret);
			break;
		}
		pr_alert("smc_id 0x%x\n", smc_id);
		pr_alert(
			"Response value from TZ_INFO_GET_SECURE_STATE: 0x%llx\n",
			desc.ret[0]);

		usr_data.resp = desc.ret[0];
		if (copy_to_user((void __user *)arg, &usr_data, sizeof(usr_data))) {
			pr_alert("copy_to_user failed\n");
			return -EFAULT;
		}
		break;
			}

	case GET_FEATURE_VERSION_ID: {
		struct tornado_data usr_data = { 0 };

		pr_alert("get secure state of the device\n");
		memset(&desc, 0x0, sizeof(struct qseecom_scm_desc));
		memset(&usr_data, 0x0, sizeof(usr_data));
		if (copy_from_user(&usr_data, (void __user *)arg,
						 sizeof(usr_data))) {
			pr_alert("copy_from_user failed for args\n");
			return -EFAULT;
		}

		smc_id = TZ_INFO_GET_FEATURE_VERSION_ID;
		desc.args[0] = usr_data.req;
		desc.arginfo = SCM_ARGS(1, SCM_VAL);
		ret = qcom_scm_qseecom_call(smc_id, &desc, true);
		if (ret) {
			pr_alert("scm call failed ret %d ", ret);
			break;
		}
		pr_alert("smc_id 0x%x\n", smc_id);
		pr_alert("Response value from TZ_INFO_GET_FEATURE_VERSION_ID: 0x%llx\n",
				desc.ret[0]);
		usr_data.resp = desc.ret[0];
		if (copy_to_user((void __user *)arg, &usr_data, sizeof(usr_data))) {
			pr_alert("copy_to_user failed\n");
			return -EFAULT;
		}
		break;
				}
	default:
		pr_alert("invalid IOCTL selection\n");
		break;
	}
	return ret;
}

static const struct file_operations module_fops = {.owner = THIS_MODULE,
	.open = tornado_module_open,
	.release = tornado_module_close,
	.unlocked_ioctl = tornado_ioctl
};

static int __init tornado_init(void)
{
	int ret = 0;
	struct device *dev_ret;

	pr_alert("tornado: init\n");
	/* Dynamically allocate device numbers */
	ret = alloc_chrdev_region(&dev_tornado, TORNADO_FIRST_MINOR,
			TORNADO_MINOR_CNT, "tornado");
	if (ret < 0) {
		pr_err("tornado : alloc_chrdev_region() failed with error: %d\n", ret);
		ret = -EFAULT;
		goto exit;
	}

	pr_info("tornado : allocated major=%u, minor=%u\n",
			MAJOR(dev_tornado), MINOR(dev_tornado));

	cdev_init(&cdev_tornado, &module_fops);
	ret = cdev_add(&cdev_tornado, dev_tornado, TORNADO_MINOR_CNT);
	if (ret < 0) {
		pr_err("tornado : cdev_add() failed with error: %d\n", ret);
		ret = -EFAULT;
		goto exit;
	}

#if (KERNEL_VERSION(6, 3, 0) <= LINUX_VERSION_CODE)
	class_tornado = class_create("char");
#else
	class_tornado = class_create(THIS_MODULE, "char");
#endif
	if (IS_ERR_OR_NULL(class_tornado)) {
		cdev_del(&cdev_tornado);
		unregister_chrdev_region(dev_tornado, TORNADO_MINOR_CNT);
		pr_err("tornado : class_create() failed\n");
		ret = -EFAULT;
		goto exit;
	}

	/* create a device and registers it with sysfs */
	dev_ret = device_create(class_tornado, NULL, dev_tornado, NULL,
			"tornado");
	if (IS_ERR_OR_NULL(dev_ret)) {
		class_destroy(class_tornado);
		cdev_del(&cdev_tornado);
		unregister_chrdev_region(dev_tornado, TORNADO_MINOR_CNT);
		pr_err("tornado: device_create() failed\n");
		ret = -EFAULT;
		goto exit;
	}

exit:
	return ret;
}

static void __exit tornado_exit(void)
{
	pr_alert("tornado module exit.\n");

	device_destroy(class_tornado, dev_tornado);
	class_destroy(class_tornado);
	cdev_del(&cdev_tornado);
	unregister_chrdev_region(dev_tornado, TORNADO_MINOR_CNT);
}

module_init(tornado_init);
module_exit(tornado_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("SECURITY TEAM TEST MODULE");
