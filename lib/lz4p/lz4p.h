/* SPDX-License-Identifier: GPL-2.0 */
/*
 * XRING shrink slabd Feature
 *
 * Copyright (C) 2024, X-Ring technologies Inc., All rights reserved.
 *
 */

#ifndef __LINUX_XRING_LZ4P_H
#define __LINUX_XRING_LZ4P_H

#ifdef CONFIG_XRING_ZRAM_LZ4P
#include "lz4p_compress.h"
#include "lz4p_decompress.h"
#include "lz4p_constants.h"

int lz4p_init(void);
void lz4p_exit(void);
#endif

#endif /* __LINUX_XRING_LZ4P_H */
