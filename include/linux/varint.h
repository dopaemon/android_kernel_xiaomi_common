/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Variable-Length Integer Encoding Library (varint)
 * Version: 1.0
 * Copyright (C) 2026 Masahito Suzuki
 *
 * Compact variable-length big-endian integer encoding that uses fewer
 * bytes for smaller values. The upper header_bits bits of the first byte
 * hold the encoded length minus one.
 *
 *   varint2 (2-bit header): 1-4 bytes, max 1,073,741,823 (u32 values)
 *   varint1 (1-bit header): 1-2 bytes, max 32,767        (u16 values)
 *
 * For each format <name> the following helpers are generated:
 *
 *   <name>__encode_len(value)      encoded length, no range check
 *   <name>_encode_len(value)       encoded length, or 0 if value > max
 *   <name>__encode(value, buf, o)  encode at buf[o], no range check
 *   <name>_encode(value, buf, o)   encode, or return 0 if value > max
 *   <name>_decode_len(buf, o)      length of the encoding at buf[o]
 *   <name>_decode(buf, o)          decode, returns {value, bytes_read}
 *
 * Decoding trusts the length in the header byte: callers must make sure
 * buf holds at least <name>_decode_len() bytes from offset.
 */
#ifndef _LINUX_VARINT_H
#define _LINUX_VARINT_H

#include <linux/types.h>

struct varint_decode_result {
	u32 value;
	u8 bytes_read;
};

/* max_bytes = 2^header_bits */
#define VARINT_MAX_BYTES(header_bits) (1 << (header_bits))

/* Max value for n-byte encoding */
#define VARINT_NBYTE_MAX(header_bits, bytes) \
	((1UL << ((bytes) * 8 - (header_bits))) - 1)

/* Max value for format */
#define VARINT_MAX(header_bits) \
	VARINT_NBYTE_MAX(header_bits, VARINT_MAX_BYTES(header_bits))

#define VARINT_DEFINE_ALL(name, vtype, header_bits)				\
static inline size_t name##__encode_len(vtype value)			\
{									\
	size_t i;							\
									\
	for (i = VARINT_MAX_BYTES(header_bits); i > 1; i--)		\
		if ((u32)value > VARINT_NBYTE_MAX(header_bits, i - 1))	\
			return i;					\
	return 1;							\
}									\
									\
static inline size_t name##_encode_len(vtype value)			\
{									\
	if ((u32)value > VARINT_MAX(header_bits))			\
		return 0;						\
	return name##__encode_len(value);				\
}									\
									\
static inline size_t name##__encode(vtype value, u8 *buf, size_t offset) \
{									\
	size_t len = name##__encode_len(value);				\
	u8 mask = (u8)((1 << (8 - (header_bits))) - 1);			\
	size_t i;							\
									\
	buf[offset] = (u8)(((len - 1) << (8 - (header_bits))) |		\
			   ((value >> ((len - 1) * 8)) & mask));	\
	for (i = 1; i < len; i++)					\
		buf[offset + i] = (u8)((value >> ((len - 1 - i) * 8)) & 0xff); \
	return len;							\
}									\
									\
static inline size_t name##_encode(vtype value, u8 *buf, size_t offset) \
{									\
	if ((u32)value > VARINT_MAX(header_bits))			\
		return 0;						\
	return name##__encode(value, buf, offset);			\
}									\
									\
static inline size_t name##_decode_len(const u8 *buf, size_t offset)	\
{									\
	return ((buf[offset] >> (8 - (header_bits))) &			\
		((1 << (header_bits)) - 1)) + 1;			\
}									\
									\
static inline struct varint_decode_result				\
name##_decode(const u8 *buf, size_t offset)				\
{									\
	size_t len = name##_decode_len(buf, offset);			\
	u8 mask = (u8)((1 << (8 - (header_bits))) - 1);			\
	u32 value = buf[offset] & mask;					\
	size_t i;							\
									\
	for (i = 1; i < len; i++)					\
		value = (value << 8) | buf[offset + i];			\
	return (struct varint_decode_result){ value, len };		\
}

VARINT_DEFINE_ALL(varint2, u32, 2)	/* 2-bit header, 1-4 bytes */
VARINT_DEFINE_ALL(varint1, u16, 1)	/* 1-bit header, 1-2 bytes */

#endif /* _LINUX_VARINT_H */
