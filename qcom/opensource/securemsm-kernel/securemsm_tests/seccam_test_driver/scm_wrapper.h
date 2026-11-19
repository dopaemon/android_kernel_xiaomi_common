/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

/*
 * Context Switch Performance Test
 */

#ifndef _LINUX_SCM_WRAPPER_H_
#define _LINUX_SCM_WRAPPER_H_

#if (KERNEL_VERSION(5, 2, 0) > LINUX_VERSION_CODE)

#include <linux/msm-bus-board.h>
#include <soc/qcom/scm.h>

/**
 * Macros used to create the Parameter ID associated with the syscall
 */
#define TZ_SYSCALL_CREATE_PARAM_ID_0 0
#define TZ_SYSCALL_CREATE_PARAM_ID_1(p1) \
	_TZ_SYSCALL_CREATE_PARAM_ID(1, p1, 0, 0, 0, 0, 0, 0, 0, 0, 0)

/**
 * Macro used to define an SCM ID based on the owner ID,
 * service ID, and function number.
 */
#define TZ_SYSCALL_CREATE_SCM_ID(o, s, f) \
		((uint32_t)((((o & 0x3f) << 24) | (s & 0xff) << 8) | (f & 0xff)))

#define TZ_MASK_BITS(h, l) ((0xffffffff >> (32 - ((h - l) + 1))) << l)
#define TZ_SYSCALL_PARAM_NARGS_MASK TZ_MASK_BITS(3, 0)
#define TZ_SYSCALL_PARAM_TYPE_MASK TZ_MASK_BITS(1, 0)

#define _TZ_SYSCALL_CREATE_PARAM_ID(nargs, p1, p2, p3, p4, p5, p6, p7, p8, p9, p10) \
	((nargs&TZ_SYSCALL_PARAM_NARGS_MASK)+ \
	((p1&TZ_SYSCALL_PARAM_TYPE_MASK)<<4)+ \
	((p2&TZ_SYSCALL_PARAM_TYPE_MASK)<<6)+ \
	((p3&TZ_SYSCALL_PARAM_TYPE_MASK)<<8)+ \
	((p4&TZ_SYSCALL_PARAM_TYPE_MASK)<<10)+ \
	((p5&TZ_SYSCALL_PARAM_TYPE_MASK)<<12)+ \
	((p6&TZ_SYSCALL_PARAM_TYPE_MASK)<<14)+ \
	((p7&TZ_SYSCALL_PARAM_TYPE_MASK)<<16)+ \
	((p8&TZ_SYSCALL_PARAM_TYPE_MASK)<<18)+ \
	((p9&TZ_SYSCALL_PARAM_TYPE_MASK)<<20)+ \
	((p10&TZ_SYSCALL_PARAM_TYPE_MASK)<<22))

/**
 * Parameter ID values
 */
/** A parameter of type value */
#define TZ_SYSCALL_PARAM_TYPE_VAL 0

 /** SIP service call ID */
#define TZ_OWNER_SIP 2

/** SIP service call groups */
#define TZ_SVC_BOOT 1 /* Boot (cold boot/warm boot) */
#define TZ_SVC_INFO 6 /* Misc. information services */

#define TZ_INFO_IS_SVC_AVAILABLE_ID \
	TZ_SYSCALL_CREATE_SCM_ID(TZ_OWNER_SIP, TZ_SVC_INFO, 0x01)

#define TZ_INFO_IS_SVC_AVAILABLE_ID_PARAM_ID \
	TZ_SYSCALL_CREATE_PARAM_ID_1(TZ_SYSCALL_PARAM_TYPE_VAL)

#define TZ_DO_MODE_SWITCH \
	TZ_SYSCALL_CREATE_SCM_ID(TZ_OWNER_SIP, TZ_SVC_BOOT, 0x0F)

#define TZ_SECURE_WDOG_TRIGGER_ID \
	TZ_SYSCALL_CREATE_SCM_ID(TZ_OWNER_SIP, TZ_SVC_BOOT, 0x08)

#define TZ_SECURE_WDOG_TRIGGER_ID_PARAM_ID \
	TZ_SYSCALL_CREATE_PARAM_ID_3(TZ_SYSCALL_PARAM_TYPE_VAL)

#define TZ_CONFIG_CPU_ERRATA_ID \
	TZ_SYSCALL_CREATE_SCM_ID(TZ_OWNER_SIP, TZ_SVC_BOOT, 0x12)

#define SCM_SVC_CAMERASS 0x18
/* These are purposely re-defined to avoid including specific header files */
#define SCM_PROTECT_ALL 0x6
#define SCM_PROTECT_PHY_LANES 0x7

inline bool qcom_scm_is_mode_switch_available_wrapper(void)
{

	int ret = 0;
	struct scm_desc desc = {0};
	uint32_t scm_id = TZ_INFO_IS_SVC_AVAILABLE_ID;

	desc.arginfo = TZ_INFO_IS_SVC_AVAILABLE_ID_PARAM_ID;
	desc.args[0] = TZ_DO_MODE_SWITCH;

	ret = scm_call2(scm_id, &desc);
	return ret ? false : true;
}

inline int qcom_scm_config_cpu_errata_wrapper(void)
{

	uint32_t scm_id = TZ_CONFIG_CPU_ERRATA_ID;
	struct scm_desc desc = {0};

	desc.arginfo = 0xFFFFFFFF;

	return scm_call2(scm_id, &desc);
}

inline bool qcom_scm_is_secure_wdog_trigger_available_wrapper(void)
{

	int ret = 0;
	uint32_t scm_id = TZ_INFO_IS_SVC_AVAILABLE_ID;
	struct scm_desc desc = {0};

	desc.arginfo = TZ_INFO_IS_SVC_AVAILABLE_ID_PARAM_ID;
	desc.args[0] = TZ_SECURE_WDOG_TRIGGER_ID;

	ret = scm_call2(scm_id, &desc);
	return ret ? false : true;
}

inline int qcom_scm_camera_protect_all_wrapper(protect, param)
{

	struct scm_desc desc = {0};
	uint32_t scm_id = SCM_SIP_FNID(SCM_SVC_CAMERASS, SCM_PROTECT_ALL);

	desc.arginfo = SCM_ARGS(2, SCM_VAL, SCM_VAL);
	desc.args[0] = protect;
	desc.args[1] = param;

	return scm_call2(scm_id, &desc);
}

inline int qcom_scm_camera_protect_phy_lanes_wrapper(protect, param)
{

	struct scm_desc desc = {0};
	uint32_t scm_id = SCM_SIP_FNID(SCM_SVC_CAMERASS, SCM_PROTECT_PHY_LANES);

	desc.arginfo = SCM_ARGS(2, SCM_VAL, SCM_VAL);
	desc.args[0] = protect;
	desc.args[1] = param;

	return scm_call2(scm_id, &desc);
}

#else

#if (KERNEL_VERSION(6, 3, 0) <= LINUX_VERSION_CODE)
#include <linux/firmware/qcom/qcom_scm.h>
#else
#include <linux/qcom_scm.h>
#endif
#include <linux/ITrustedCameraDriver.h>
#include <linux/CTrustedCameraDriver.h>

inline bool qcom_scm_is_mode_switch_available_wrapper(void)
{
	return qcom_scm_is_mode_switch_available();
}

inline int qcom_scm_config_cpu_errata_wrapper(void)
{
	return qcom_scm_config_cpu_errata();
}

inline bool qcom_scm_is_secure_wdog_trigger_available_wrapper(void)
{
	return qcom_scm_is_secure_wdog_trigger_available();
}

inline int qcom_scm_camera_protect_all_wrapper(uint32_t protect, uint32_t param)
{
	return qcom_scm_camera_protect_all(protect, param);
}

inline int qcom_scm_camera_protect_phy_lanes_wrapper(uint32_t protect,
		uint32_t param)
{
	return qcom_scm_camera_protect_phy_lanes(protect, param);
}

inline int qcom_smc_cam_protect(struct Object me, uint32_t protect,
		uint32_t phy_param, uint32_t csid_param, uint32_t cdm_param)
{
	ITCDriverSensorInfo *phy_sel_info =
		kmalloc(sizeof(ITCDriverSensorInfo), GFP_KERNEL);

	if (phy_sel_info == NULL)
		return -ENOMEM;
	phy_sel_info->protect = protect;
	phy_sel_info->phy_lane_sel_mask = phy_param;
	phy_sel_info->csid_hw_idx_mask = csid_param;
	phy_sel_info->cdm_hw_idx_mask = cdm_param;

	return ITrustedCameraDriver_dynamicProtectSensor(me, phy_sel_info);
}

inline int qcom_smc_config_fd_port(struct Object me, uint32_t protect)
{
	return ITrustedCameraDriver_dynamicConfigureFDPort(me, protect);
}

inline int qcom_smc_get_version(struct Object me, uint32_t *arch_ver,
		uint32_t *max_ver, uint32_t *min_ver)
{
	return ITrustedCameraDriver_getVersion(me, arch_ver, max_ver, min_ver);
}

inline int qcom_smc_dynamic_config_port(struct Object me, uint32_t hw_type,
		uint32_t hw_id_mask, uint32_t protect_port, uint32_t phy_id,
		uint32_t num_ports, uint32_t port_id, size_t port_info_len)
{
	struct PortInfo *port_info_ptr = kmalloc(sizeof(struct PortInfo), GFP_KERNEL);

	if (port_info_ptr == NULL)
		return -ENOMEM;
	port_info_ptr->hw_type = hw_type;
	port_info_ptr->hw_id_mask = hw_id_mask;
	port_info_ptr->protect = protect_port;
	port_info_ptr->phy_id = phy_id;
	port_info_ptr->num_ports = num_ports;
	port_info_ptr->port_id[0] = port_id;

	return ITrustedCameraDriver_dynamicConfigurePortsV2(me,
			port_info_ptr, port_info_len);
}
#endif

#endif
