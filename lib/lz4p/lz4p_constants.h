/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2024, X-Ring technologies Inc., All rights reserved.
 */

#pragma once

//  Tunables
#define LZ4_COMPRESS_HASH_BITS 10
#define LZ4_COMPRESS_HASH_ENTRIES (1 << LZ4_COMPRESS_HASH_BITS)
#define LZ4_COMPRESS_HASH_MULTIPLY 2654435761U
#define LZ4_COMPRESS_HASH_SHIFT (32 - LZ4_COMPRESS_HASH_BITS)

//  Not tunables
#define LZ4_GOFAST_SAFETY_MARGIN 128
#define LZ4_DISTANCE_BOUND 65536
