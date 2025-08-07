/* SPDX-License-Identifier: GPL-2.0 */
/*
 * XRING shrink slabd Feature
 *
 * Copyright (C) 2024, X-Ring technologies Inc., All rights reserved.
 *
 */

#ifndef __LINUX_XRING_LZ4P_DECOMPRESS_H
#define __LINUX_XRING_LZ4P_DECOMPRESS_H

#include <linux/types.h>
#include <asm/simd.h>

#define LZ4_FAST_MARGIN                (128)

#include <asm/neon.h>
#include <asm/cputype.h>

static inline int lz4_decompress_accel_enable(void)
{
	return may_use_simd();
}

int LZ4P_decompress_safe(const char *source, char *dest,
	int compressedSize, int maxDecompressedSize, void *ctx);

#endif /* __LINUX_XRING_LZ4P_DECOMPRESS_H */
