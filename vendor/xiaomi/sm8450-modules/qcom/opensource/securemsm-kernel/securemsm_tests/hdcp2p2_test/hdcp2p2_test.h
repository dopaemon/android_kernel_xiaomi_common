/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */


#ifndef _HDCP2P2_TEST_H_
#define _HDCP2P2_TEST_H_

#include <linux/ioctl.h>
#include <linux/types.h>

#define MAX_TX_MESSAGE_SIZE 129
#define MAX_RX_MESSAGE_SIZE 534

/*test driver handle for clients*/
struct hdcp2_test_driver_data {
	uint32_t timeout;
	bool repeater_flag;
	unsigned char req_data_from_client[MAX_RX_MESSAGE_SIZE];
	uint32_t req_length;
	unsigned char resp_data_from_TA[MAX_TX_MESSAGE_SIZE];
	uint32_t resp_length;
	uint32_t device_type;
	uint8_t vc_payload_id;
	uint8_t stream_number;
	uint32_t stream_id;
	bool enable_encryption;
};

#define HDCP_IOC_MAGIC 0x98

#define HDCP_IOCTL_INIT _IOWR(HDCP_IOC_MAGIC, 1, void *)

#define HDCP_IOCTL_START _IOWR(HDCP_IOC_MAGIC, 2, void *)

#define HDCP_IOCTL_START_AUTH _IOW(HDCP_IOC_MAGIC, 3, void *)

#define HDCP_IOCTL_PROCESS_MSG _IOWR(HDCP_IOC_MAGIC, 4, void *)

#define HDCP_IOCTL_QUERY_STREAM _IOWR(HDCP_IOC_MAGIC, 5, void *)

#define HDCP_IOCTL_ADD_STREAM _IOWR(HDCP_IOC_MAGIC, 6, void *)

#define HDCP_IOCTL_REMOVE_STREAM _IOW(HDCP_IOC_MAGIC, 7, void *)

#define HDCP_IOCTL_FORCE_ENCRYPTION _IOW(HDCP_IOC_MAGIC, 8, void *)

#define HDCP_IOCTL_STOP _IOWR(HDCP_IOC_MAGIC, 9, void *)

#define HDCP_IOCTL_DEINIT _IO(HDCP_IOC_MAGIC, 10)

#endif /* _HDCP2P2_TEST_H_ */
