/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef _TORNADO_MOD_H_
#define _TORNADO_MOD_H_

#include <linux/types.h>

struct param_req {

	uint64_t param0;
	uint64_t param1;
	uint64_t param2;
	uint64_t param3;
	uint64_t param4;
	uint64_t param5;
	uint64_t param6;
	uint64_t param7;
	uint64_t param8;
	uint64_t param9;
};

/** SIP service call ID. */
#define TZ_OWNER_SIP 2U

#define TZ_SVC_PIL 2U /**< Peripheral image loading. */
#define TZ_SVC_INFO 6U /**< Miscellaneous information services. */

#define SECBOOT_QSEE_SW_TYPE 0x7
#define SECBOOT_CDSP_SW_TYPE 0x17
#define SECBOOT_Q6_CDSP_DTB_SW_TYPE 0x52

#define GET_ROLLBACK_VERSION_ID 0x0FU
#define GET_SECURE_STATE 0x4U
#define GET_FEATURE_VERSION_ID 0x03U

#define TZ_SYSCALL_CREATE_SMC_ID(o, s, f) \
	((uint32_t)((((uint32_t)(o & 0x3fU) << 24U) | (uint32_t)(s & 0xffU) << 8U) | (f & 0xffU)))

#define TZ_PIL_GET_ROLLBACK_VERSION_ID \
	TZ_SYSCALL_CREATE_SMC_ID(TZ_OWNER_SIP, TZ_SVC_PIL, 0x0FU)

#define TZ_INFO_GET_SECURE_STATE \
	TZ_SYSCALL_CREATE_SMC_ID(TZ_OWNER_SIP, TZ_SVC_INFO, 0x4U)

#define TZ_INFO_GET_FEATURE_VERSION_ID \
	TZ_SYSCALL_CREATE_SMC_ID(TZ_OWNER_SIP, TZ_SVC_INFO, 0x03U)


struct tornado_data {
	uint64_t req;
	uint64_t resp;
};

#endif
