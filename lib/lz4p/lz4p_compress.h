/* SPDX-License-Identifier: GPL-2.0 */
/*
 * XRING shrink slabd Feature
 *
 * Copyright (C) 2024, X-Ring technologies Inc., All rights reserved.
 *
 */

#ifndef __LINUX_XRING_LZ4P_COMPRESS_H
#define __LINUX_XRING_LZ4P_COMPRESS_H

#include <linux/lz4.h>

#include "lz4p_constants.h"

struct lz4_hash_entry_t {
	uint32_t offset;
	uint32_t word;
};

int LZ4P_compress_default(const char *source, char *dest, int inputSize,
	int maxOutputSize, void *wrkmem);

#endif /* __LINUX_XRING_LZ4P_COMPRESS_H */
