/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

/*****************************************************************************
 *    Context Switch Performance Test
 ****************************************************************************/
#ifndef _LINUX_SECCAM_DRIVER_H_
#define _LINUX_SECCAM_DRIVER_H_

#include <linux/types.h>
#include <linux/IClientEnv.h>

#define SCM_PROTECT_ALL         0x6
#define SCM_PROTECT_PHY_LANES   0x7
#define SMC_PROTECT             0x8
#define SMC_GET_VER             0x9
#define SCM_MAP_BUFFER          0xa
#define SCM_UNMAP_BUFFER        0xb
#define SMC_CONFIG_FD_PORT      0xc
#define SMC_CONFIG_PORT         0xd

struct Object sc_object;

struct seccam_desc {
	unsigned long time;
	uint32_t protect;
	uint64_t param;
	uint32_t csid_param;
	uint32_t cdm_param;
	uint32_t arch_ver;
	uint32_t max_ver;
	uint32_t min_ver;
	int32_t  mem_fd;
	uint32_t hw_type;
	uint32_t hw_id_mask;
	uint32_t protect_port;
	uint32_t phy_id;
	uint32_t num_ports;
	uint32_t port_id;
	size_t port_info_len;
};

#endif
