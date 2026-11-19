// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

/* Secure Camera - Kernel Helper Module */

#include "seccam_test_driver.h"
#include <linux/cdev.h>
#include <linux/errno.h>
#include <linux/mem-buf.h>
#include <linux/module.h>
#include <linux/msm_ion.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/qcom-dma-mapping.h>
#include <linux/qti-smmu-proxy.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include "scm_wrapper.h"

#define SCMTEST_FIRST_MINOR 0
#define SCMTEST_MINOR_CNT 1

struct dma_mapping_info {
	struct device *dev;
	struct sg_table *table;
	struct dma_buf_attachment *attach;
	struct dma_buf *buf;
};

static struct cdev cdev_seccamtest;
static dev_t dev_seccamtest;
static struct class *class_seccamtest;
static struct dma_mapping_info g_mapping_info = {NULL};

static int32_t seccamtest_map_buffer(int32_t fd)
{
	int32_t ret = 0;
	struct dma_buf *dmabuf;
	struct dma_buf_attachment *attach;
	struct sg_table *table;
	struct device *dev = g_mapping_info.dev;

	if (dev == NULL) {
		pr_err("SECCAMTEST: No configuration in the device tree.\n");
		return -EINVAL;
	}

	dmabuf = dma_buf_get(fd);
	if (IS_ERR(dmabuf))
		return PTR_ERR(dmabuf);

	attach = dma_buf_attach(dmabuf, dev);
	if (IS_ERR(attach)) {
		pr_err("SECCAMTEST: failed to attach.\n");
		ret = -EFAULT;
		goto out;
	}

	attach->dma_map_attrs |= DMA_ATTR_QTI_SMMU_PROXY_MAP;
	table = dma_buf_map_attachment(attach, DMA_BIDIRECTIONAL);
	if (IS_ERR(table)) {
		pr_err("SECCAMTEST: failed to map.\n");
		ret = -EFAULT;
		goto out;
	}

	g_mapping_info.attach = attach;
	g_mapping_info.table = table;
	g_mapping_info.buf = dmabuf;

	pr_info("%s: addr: 0x%llx len: 0x%x\n", __func__,
			sg_dma_address(table->sgl), sg_dma_len(table->sgl));

	return ret;

out:
	dma_buf_put(dmabuf);

	return ret;
}

static int32_t seccamtest_unmap_buffer(void)
{
	int32_t ret = 0;
	struct dma_buf *dmabuf;
	struct dma_buf_attachment *attach;
	struct device *dev = g_mapping_info.dev;

	if (dev == NULL || g_mapping_info.table == NULL
			|| g_mapping_info.buf == NULL
			|| g_mapping_info.attach == NULL) {
		pr_err("SECCAMTEST: buffer has not been mapped.\n");
		return -EINVAL;
	}

	dmabuf = g_mapping_info.buf;
	attach = g_mapping_info.attach;

	dma_buf_unmap_attachment(attach, g_mapping_info.table, DMA_BIDIRECTIONAL);
	dma_buf_detach(dmabuf, attach);
	dma_buf_put(dmabuf);

	g_mapping_info.table = NULL;
	g_mapping_info.attach = NULL;
	g_mapping_info.buf = NULL;

	return ret;
}

static int32_t seccamtest_get_csf_version(struct csf_version *csf_version)
{
	int32_t ret = 0;

	if (g_mapping_info.dev) {
		ret = smmu_proxy_get_csf_version(csf_version);
	} else {
		ret = qcom_smc_get_version(sc_object, &(csf_version->arch_ver),
				&(csf_version->max_ver), &(csf_version->min_ver));
	}

	return ret;
}

static int seccamtest_open(struct inode *i, struct file *f)
{
	pr_info("SECCAMTEST: driver open\n");
	return 0;
}

static int seccamtest_close(struct inode *i, struct file *f)
{
	pr_info("SECCAMTEST: driver close\n");
	return 0;
}

static long seccamtest_ioctl(struct file *f, unsigned int cmd,
		unsigned long arg)
{
	int ret = 0;
	struct seccam_desc desc = {0};
	ktime_t start1, finish1;
	unsigned long val = 0;
	struct Object client_env = {0};
	struct csf_version csf_version;

	ret = get_client_env_object(&client_env);
	if (ret) {
		pr_err("SECCAMTEST: failed to get env object ,ret : %d\n", ret);
		goto exit;
	}

	pr_err("Calling IClientEnv_open\n");
	ret = IClientEnv_open(client_env, CTrustedCameraDriver_UID, &sc_object);
	if (ret) {
		pr_err("SECCAMTEST: failed to get seccam object , ret : %d\n", ret);
		goto exit;
	}

	if (copy_from_user(&desc, (void *)arg, sizeof(struct seccam_desc))) {
		pr_err("SECCAMTEST: copy_to_user failed\n");
		ret = -1;
		goto exit;
	}

	start1 = ktime_get();
	switch (cmd) {
	case SCM_PROTECT_ALL:
		ret = qcom_scm_camera_protect_all_wrapper(desc.protect, desc.param);
		break;
	case SCM_PROTECT_PHY_LANES:
		ret = qcom_scm_camera_protect_phy_lanes_wrapper(desc.protect, desc.param);
		break;
	case SMC_PROTECT:
		ret = qcom_smc_cam_protect(sc_object, desc.protect, desc.param,
				desc.csid_param, desc.cdm_param);
		break;
	case SMC_CONFIG_PORT:
		ret = qcom_smc_dynamic_config_port(sc_object, desc.hw_type,
				desc.hw_id_mask, desc.protect_port, desc.phy_id,
				desc.num_ports, desc.port_id, desc.port_info_len);
		break;
	case SMC_CONFIG_FD_PORT:
		ret = qcom_smc_config_fd_port(sc_object, desc.protect);
		break;
	case SMC_GET_VER:
		ret = seccamtest_get_csf_version(&csf_version);
		desc.arch_ver = csf_version.arch_ver;
		desc.max_ver = csf_version.max_ver;
		desc.min_ver = csf_version.min_ver;
		break;
	case SCM_MAP_BUFFER:
		ret = seccamtest_map_buffer(desc.mem_fd);
		break;
	case SCM_UNMAP_BUFFER:
		ret = seccamtest_unmap_buffer();
		break;
	default:
		pr_alert("incorrect cmd ID (%u)\n", cmd);
		ret = -1;
		goto exit;
	}

	if (ret) {
		pr_err("SECCAMTEST: SCM call failed =%d\n", ret);
		ret = -1;
		goto exit;
	}

	finish1 = ktime_get();
	val = ktime_us_delta(finish1, start1);
	desc.time = val;

	if (copy_to_user((void *)arg, &desc, sizeof(desc))) {
		pr_err("SECCAMTEST: copy_to_user failed\n");
		ret = -1;
		goto exit;
	}

exit:
	Object_ASSIGN_NULL(sc_object);
	Object_ASSIGN_NULL(client_env);
	return ret;
}

static const struct file_operations seccamtest_fops = {.owner = THIS_MODULE,
	.open = seccamtest_open,
	.release = seccamtest_close,
	.unlocked_ioctl = seccamtest_ioctl
};

static int seccamtest_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;

	pr_info("%s: probe.\n", __func__);

	g_mapping_info.dev = dev;

	dev_info(dev, "%s: succeeds.\n", __func__);

	return 0;
}

static const struct of_device_id seccamtest_match_table[] = {
	{.compatible = "qcom,seccamtest"},
	{},
};

static struct platform_driver seccamtest_driver = {
	.probe = seccamtest_probe,
	.driver = {
		.name = "seccamtest",
		.of_match_table = seccamtest_match_table,
	},
};

static int __init seccam_init(void)
{
	int ret = 0;
	struct device *dev_ret;

	pr_info("SECCAMTEST: init\n");
	/* Dynamically allocate device numbers */
	ret = alloc_chrdev_region(&dev_seccamtest, SCMTEST_FIRST_MINOR,
			SCMTEST_MINOR_CNT, "seccamtest");
	if (ret < 0) {
		pr_err("SECCAMTEST: alloc_chrdev_region() failed with error: %d\n", ret);
		ret = -EFAULT;
		goto exit;
	}

	pr_info("SECCAMTEST: allocated major=%u, minor=%u\n",
			MAJOR(dev_seccamtest), MINOR(dev_seccamtest));

	cdev_init(&cdev_seccamtest, &seccamtest_fops);
	ret = cdev_add(&cdev_seccamtest, dev_seccamtest, SCMTEST_MINOR_CNT);
	if (ret < 0) {
		pr_err("SECCAMTEST: cdev_add() failed with error: %d\n", ret);
		ret = -EFAULT;
		goto exit;
	}

#if (KERNEL_VERSION(6, 3, 0) <= LINUX_VERSION_CODE)
	class_seccamtest = class_create("char");
#else
	class_seccamtest = class_create(THIS_MODULE, "char");
#endif
	if (IS_ERR_OR_NULL(class_seccamtest)) {
		cdev_del(&cdev_seccamtest);
		unregister_chrdev_region(dev_seccamtest, SCMTEST_MINOR_CNT);
		pr_err("SECCAMTEST: class_create() failed\n");
		ret = -EFAULT;
		goto exit;
	}

	/* create a device and registers it with sysfs */
	dev_ret = device_create(class_seccamtest, NULL, dev_seccamtest, NULL,
			"seccamtest");
	if (IS_ERR_OR_NULL(dev_ret)) {
		class_destroy(class_seccamtest);
		cdev_del(&cdev_seccamtest);
		unregister_chrdev_region(dev_seccamtest, SCMTEST_MINOR_CNT);
		pr_err("SECCAMTEST: device_create() failed\n");
		ret = -EFAULT;
		goto exit;
	}

	/*
	 * Even if there is no configuration related to the seccamtest driver
	 * in the device tree, the driver still can run to support the legacy
	 * commands.
	 */
	ret = platform_driver_register(&seccamtest_driver);
	if (ret < 0) {
		pr_err("SECCAMTEST: platform_driver_register() failed with error: %d\n", ret);
		pr_warn("SECCAMTEST: run with legacy mode.\n");
		ret = 0;
	}

exit:
	return ret;
}

static void __exit seccam_exit(void)
{
	pr_alert("Goodbye world.\n");

	if (g_mapping_info.dev) {
		platform_driver_unregister(&seccamtest_driver);
		g_mapping_info.dev = NULL;
		g_mapping_info.table = NULL;
		g_mapping_info.attach = NULL;
		g_mapping_info.buf = NULL;
	}
	device_destroy(class_seccamtest, dev_seccamtest);
	class_destroy(class_seccamtest);
	cdev_del(&cdev_seccamtest);
	unregister_chrdev_region(dev_seccamtest, SCMTEST_MINOR_CNT);
}

module_init(seccam_init);
module_exit(seccam_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("SECURITY SECCAM TEST");
#if (KERNEL_VERSION(6, 13, 0) <= LINUX_VERSION_CODE)
MODULE_IMPORT_NS("DMA_BUF");
#elif (KERNEL_VERSION(5, 15, 0) <= LINUX_VERSION_CODE)
MODULE_IMPORT_NS(DMA_BUF);
#endif
