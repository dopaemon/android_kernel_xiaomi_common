// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#define DEBUG
#define pr_fmt(fmt) "HDCP2P2Driver: %s: " fmt, __func__

#include <linux/cdev.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/hdcp_qseecom.h>
#include <linux/kernel.h> /* Needed for KERN_ALERT */
#include <linux/module.h> /* Needed by all modules */
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/version.h>

#include "hdcp2p2_test.h"

#define HDCPTEST_DRV "hdcptest"

static struct cdev hdcptest_cdev;
static struct class *hdcptest_class;
static struct device *class_dev;
static dev_t hdcptest_device_no;

enum hdcp_state {
	HDCP_STATE_INIT = 0x00,
	HDCP_STATE_APP_LOADED = 0x01,
	HDCP_STATE_SESSION_INIT = 0x02,
	HDCP_STATE_TXMTR_INIT = 0x04,
	HDCP_STATE_AUTHENTICATED = 0x08,
	HDCP_STATE_ERROR = 0x10
};

struct hdcp2p2_test_priv_data {
	void *hdcp_qseecom_handle;
	struct hdcp2_app_data *test_app_data;
};

static int hdcptest_open(struct inode *inode, struct file *file)
{
	int ret = 0;
	struct hdcp2p2_test_priv_data *test_data =
		kzalloc(sizeof(struct hdcp2p2_test_priv_data), GFP_KERNEL);

	if (!test_data) {
		pr_err("couldn't allocate test_data\n");
		ret = -ENOMEM;
	}
	file->private_data = test_data;
	return ret;
}

static int hdcptest_release(struct inode *inode, struct file *file)
{
	struct hdcp2p2_test_priv_data *test_data = file->private_data;

	kfree(test_data);
	return 0;
}

long hdcptest_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	void __user *argp = (void __user *)arg;
	int ret = 0;
	struct hdcp2p2_test_priv_data *hdcp_test = file->private_data;
	struct hdcp2_app_data *test_driver_app_data;

	if (hdcp_test == NULL) {
		pr_err("private_data is NULL\n");
		ret = -EINVAL;
		goto out;
	}
	test_driver_app_data = hdcp_test->test_app_data;

	switch (cmd) {
	case HDCP_IOCTL_INIT: {
		struct hdcp2_test_driver_data test_driver_client_data;

		pr_debug("HDCP_IOCTL_INIT\n");
		if (copy_from_user(&test_driver_client_data, argp,
					 sizeof(test_driver_client_data))) {
			pr_err("copy from user failed\n");
			ret = -EFAULT;
			break;
		}

		hdcp_test->hdcp_qseecom_handle =
	(void *)hdcp2_init(test_driver_client_data.device_type);

		if (hdcp_test->hdcp_qseecom_handle == NULL) {
			pr_err("hdcp_qseecom_handle is NULL\n");
			ret = -EFAULT;
			break;
		}

		if (copy_to_user(argp, &test_driver_client_data,
				 sizeof(test_driver_client_data))) {
			pr_err("copy to user failed!\n");
			ret = -EFAULT;
			break;
		}
		break;
	}

	case HDCP_IOCTL_START: {
		struct hdcp2_test_driver_data test_driver_client_data;

		pr_debug("HDCP_IOCTL_START\n");
		if (copy_from_user(&test_driver_client_data, argp,
					sizeof(test_driver_client_data))) {
			pr_err("copy from user failed\n");
			ret = -EFAULT;
			break;
		}

		test_driver_app_data = kzalloc(sizeof(struct hdcp2_app_data), GFP_KERNEL);
		if (!test_driver_app_data) {
			ret = -ENOMEM;
			break;
		}
		hdcp_test->test_app_data = test_driver_app_data;
		test_driver_app_data->request.length = test_driver_client_data.req_length;

		ret = hdcp2_app_comm(hdcp_test->hdcp_qseecom_handle, HDCP2_CMD_START,
			test_driver_app_data);
		if (ret) {
			pr_err("hdcp2_app_comm failed, err %d!\n", ret);
			break;
		}

		test_driver_client_data.timeout = test_driver_app_data->timeout;
		test_driver_client_data.repeater_flag = test_driver_app_data->repeater_flag;
		test_driver_client_data.resp_length = test_driver_app_data->response.length;

		ret = copy_to_user((void __user *)test_driver_client_data.resp_data_from_TA,
					test_driver_app_data->response.data,
					test_driver_app_data->response.length);
		if (ret) {
			pr_err("copy to user failed! ret = %d\n", ret);
			ret = -EFAULT;
			break;
		}

		if (copy_to_user(argp, &test_driver_client_data,
				sizeof(test_driver_client_data))) {
			pr_err("copy to user failed!\n");
			ret = -EFAULT;
			break;
		}
		break;
	}
	case HDCP_IOCTL_START_AUTH: {
		struct hdcp2_test_driver_data test_driver_client_data;

		pr_debug("HDCP_IOCTL_START_AUTH\n");
		if (copy_from_user(&test_driver_client_data, argp,
					sizeof(test_driver_client_data))) {
			pr_err("copy from user failed\n");
			ret = -EFAULT;
			break;
		}

		if (!test_driver_app_data) {
			pr_err("test_driver_app_data is NULL\n");
			ret = -EINVAL;
			break;
		}
		ret = hdcp2_app_comm(hdcp_test->hdcp_qseecom_handle, HDCP2_CMD_START_AUTH,
			 test_driver_app_data);
		if (ret) {
			pr_err("hdcp2_app_comm failed, err %d!\n", ret);
			break;
		}

		test_driver_client_data.timeout = test_driver_app_data->timeout;
		test_driver_client_data.repeater_flag = test_driver_app_data->repeater_flag;

		if (sizeof(test_driver_client_data.resp_data_from_TA) <
			test_driver_app_data->response.length) {
			pr_err("client response buffer doesn't have enough space or buff is null!\n");
			ret = -EINVAL;
			break;
		}
		test_driver_client_data.resp_length = test_driver_app_data->response.length;

		memcpy(test_driver_client_data.resp_data_from_TA,
		test_driver_app_data->response.data,
		test_driver_app_data->response.length);

		if (copy_to_user(argp, &test_driver_client_data,
				sizeof(test_driver_client_data))) {
			pr_err("copy to user failed!\n");
			ret = -EFAULT;
			break;
		}
		break;
	}
	case HDCP_IOCTL_PROCESS_MSG: {
		struct hdcp2_test_driver_data test_driver_client_data;

		pr_debug("HDCP_IOCTL_PROCESS_MSG\n");
		if (copy_from_user(&test_driver_client_data, argp,
					sizeof(test_driver_client_data))) {
			pr_err("copy from user failed\n");
			ret = -EFAULT;
			break;
		}
		// to-do: change interface to have actual length
		if (test_driver_client_data.req_length > MAX_RX_MESSAGE_SIZE) {
			pr_err("client request length exceeds the maximum length allowed or buff is null!\n");
			ret = -EINVAL;
			break;
		}
		test_driver_app_data->request.length = test_driver_client_data.req_length;

		memcpy(test_driver_app_data->request.data,
		 test_driver_client_data.req_data_from_client,
		 test_driver_client_data.req_length);

		ret = hdcp2_app_comm(hdcp_test->hdcp_qseecom_handle, HDCP2_CMD_PROCESS_MSG,
			 test_driver_app_data);
		if (ret) {
			pr_err("hdcp2_app_comm failed, err %d!\n", ret);
			break;
		}

		test_driver_client_data.timeout = test_driver_app_data->timeout;
		test_driver_client_data.repeater_flag = test_driver_app_data->repeater_flag;

		if (sizeof(test_driver_client_data.resp_data_from_TA) <
			test_driver_app_data->response.length) {
			pr_err("client response buffer doesn't have enough space or buff is null!\n");
			ret = -EINVAL;
			break;
		}
		test_driver_client_data.resp_length = test_driver_app_data->response.length;

		memcpy(test_driver_client_data.resp_data_from_TA,
		 test_driver_app_data->response.data,
		 test_driver_app_data->response.length);

		if (copy_to_user(argp, &test_driver_client_data,
				 sizeof(test_driver_client_data))) {
			pr_err("copy to user failed!\n");
			ret = -EFAULT;
			break;
		}
		break;
	}
	case HDCP_IOCTL_QUERY_STREAM: {
		struct hdcp2_test_driver_data test_driver_client_data;

		pr_debug("HDCP_IOCTL_QUERY_STREAM\n");
		if (copy_from_user(&test_driver_client_data, argp,
					 sizeof(test_driver_client_data))) {
			pr_err("copy from user failed\n");
			ret = -EFAULT;
			break;
		}

		ret = hdcp2_app_comm(hdcp_test->hdcp_qseecom_handle, HDCP2_CMD_QUERY_STREAM,
			 test_driver_app_data);
		if (ret) {
			pr_err("hdcp2_app_comm failed, err %d!\n", ret);
			break;
		}

		test_driver_client_data.timeout = test_driver_app_data->timeout;

		if (sizeof(test_driver_client_data.resp_data_from_TA) <
			test_driver_app_data->response.length) {
			pr_err("client response buffer has low space or buff is null!\n");
			ret = -EINVAL;
			break;
		}
		test_driver_client_data.resp_length = test_driver_app_data->response.length;

		memcpy(test_driver_client_data.resp_data_from_TA,
		 test_driver_app_data->response.data,
		 test_driver_app_data->response.length);

		if (copy_to_user(argp, &test_driver_client_data,
				 sizeof(test_driver_client_data))) {
			pr_err("copy to user failed!\n");
			ret = -EFAULT;
			break;
		}
		break;
	}
	case HDCP_IOCTL_STOP: {
		struct hdcp2_test_driver_data test_driver_client_data;

		pr_debug("HDCP_IOCTL_STOP\n");
		if (copy_from_user(&test_driver_client_data, argp,
					 sizeof(test_driver_client_data))) {
			pr_err("copy from user failed\n");
			ret = -EFAULT;
			break;
		}

		ret = hdcp2_app_comm(hdcp_test->hdcp_qseecom_handle, HDCP2_CMD_STOP,
			 test_driver_app_data);
		if (ret) {
			pr_err("hdcp2_app_comm failed, err %d!\n", ret);
			break;
		}

		if (copy_to_user(argp, &test_driver_client_data,
				 sizeof(test_driver_client_data))) {
			pr_err("copy to user failed!\n");
			ret = -EFAULT;
			break;
		}
		break;
	}
	case HDCP_IOCTL_DEINIT: {

		pr_debug("HDCP_IOCTL_DEINIT\n");

		if (hdcp_test->hdcp_qseecom_handle == NULL) {
			pr_err("hdcp_qseecom_handle is null\n");
			ret = -EFAULT;
			break;
		}
		hdcp2_deinit(hdcp_test->hdcp_qseecom_handle);
		break;
	}
	case HDCP_IOCTL_ADD_STREAM: {
		struct hdcp2_test_driver_data test_driver_client_data;

		pr_debug("HDCP_IOCTL_ADD_STREAM\n");
		if (copy_from_user(&test_driver_client_data, argp,
					 sizeof(test_driver_client_data))) {
			pr_err("copy from user failed\n");
			ret = -EFAULT;
			break;
		}

		if (hdcp_test->hdcp_qseecom_handle == NULL) {
			pr_err("HDCP_IOCTL_ADD_STREAM handle is null\n");
			ret = -EFAULT;
			break;
		}

		ret = hdcp2_open_stream(hdcp_test->hdcp_qseecom_handle,
					test_driver_client_data.vc_payload_id,
					test_driver_client_data.stream_number,
					&test_driver_client_data.stream_id);

		if (copy_to_user(argp, &test_driver_client_data,
				 sizeof(test_driver_client_data))) {
			pr_err("copy to user failed!\n");
			ret = -EFAULT;
			break;
		}
		break;
	}
	case HDCP_IOCTL_REMOVE_STREAM: {
		struct hdcp2_test_driver_data test_driver_client_data;

		pr_debug("HDCP_IOCTL_REMOVE_STREAM\n");
		if (copy_from_user(&test_driver_client_data, argp,
					 sizeof(test_driver_client_data))) {
			pr_err("copy from user failed\n");
			ret = -EFAULT;
			break;
		}

		ret = hdcp2_close_stream(hdcp_test->hdcp_qseecom_handle,
					 test_driver_client_data.stream_id);
		break;
	}
	case HDCP_IOCTL_FORCE_ENCRYPTION: {
		struct hdcp2_test_driver_data test_driver_client_data;

		pr_debug("HDCP_IOCTL_FORCE_ENCRYPTION\n");

		if (copy_from_user(&test_driver_client_data, argp,
					 sizeof(test_driver_client_data))) {
			pr_err("copy from user failed\n");
			ret = -EFAULT;
			break;
		}

		ret = hdcp2_force_encryption(hdcp_test->hdcp_qseecom_handle,
				 test_driver_client_data.enable_encryption);

		break;
	}
	} // end switch

out:
	pr_info("%s return w/ %d\n", __func__, ret);
	return ret;
}

static const struct file_operations hdcptest_fops = {
		.owner = THIS_MODULE,
		.open = hdcptest_open,
		.release = hdcptest_release,
		.unlocked_ioctl = hdcptest_ioctl,
};

static int __init hdcptest_mod_init(void)
{
	int rc = 0;

	pr_info("init hdcp test module\n");

	rc = alloc_chrdev_region(&hdcptest_device_no, 0, 1, HDCPTEST_DRV);
	if (rc < 0) {
		pr_err("alloc_chrdev_region failed %d\n", rc);
		return rc;
	}
#if (KERNEL_VERSION(6, 3, 0) <= LINUX_VERSION_CODE)
	hdcptest_class = class_create(HDCPTEST_DRV);
#else
	hdcptest_class = class_create(THIS_MODULE, HDCPTEST_DRV);
#endif
	if (IS_ERR(hdcptest_class)) {
		rc = -ENOMEM;
		pr_err("class_create failed %d\n", rc);
		goto exit_unreg_chrdev_region;
	}
	class_dev = device_create(hdcptest_class, NULL, hdcptest_device_no, NULL,
					HDCPTEST_DRV);
	if (!class_dev) {
		pr_err("class_device_create failed %d\n", rc);
		rc = -ENOMEM;
		goto exit_destroy_class;
	}
	cdev_init(&hdcptest_cdev, &hdcptest_fops);
	hdcptest_cdev.owner = THIS_MODULE;

	rc = cdev_add(&hdcptest_cdev, MKDEV(MAJOR(hdcptest_device_no), 0), 1);
	if (rc < 0) {
		pr_err("cdev_add failed %d\n", rc);
		goto exit_destroy_device;
	}
	pr_info("hdcp_test init finished\n");
	return 0;

exit_destroy_device:
	device_destroy(hdcptest_class, hdcptest_device_no);
exit_destroy_class:
	class_destroy(hdcptest_class);
exit_unreg_chrdev_region:
	unregister_chrdev_region(hdcptest_device_no, 1);
	return rc;
}

static void __exit hdcptest_mod_exit(void)
{
	pr_info("exit hdcp test module\n");
	/* destroy device */
	device_destroy(hdcptest_class, hdcptest_device_no);
	/* destroy class */
	class_destroy(hdcptest_class);
	/* unregister chrdev region */
	unregister_chrdev_region(hdcptest_device_no, 1);
	pr_info("exit hdcp test module done\n");
}

module_init(hdcptest_mod_init);
module_exit(hdcptest_mod_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Kernel HDCP Test Driver");
