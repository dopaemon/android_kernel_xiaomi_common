/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Binary-Merge Average Filter (bmavg)
 * Copyright (C) 2025 Masahito Suzuki
 *
 * A moving average filter that keeps O(log N) state and runs in amortized
 * O(1) time per sample. Samples are merged pairwise into power-of-two
 * sized buckets (like a binary counter carrying), so hist[i] holds the
 * average of a block of 2^i samples. Merges round half to even, and the
 * read path weights each bucket exactly.
 *
 *   variant  buckets  max window       max value per sample
 *   u8       5        32               31
 *   u16      12       4,096            4,095
 *   u32      28       134,217,728      134,217,727
 *   u64      58       2^58             2^58 - 1
 *
 * API, with XX one of 8, 16, 32 or 64:
 *
 *   bmavg_init_uXX(b)            reset the filter
 *   bmavg_write_uXX(b, v)        add a sample
 *   bmavg_read_uXX(b)            current average, 0 if empty
 *   bmavg_set_limit_uXX(b, n)    limit the window to 2^n samples,
 *                                1 <= n <= number of buckets
 *
 * The filter does no locking; callers serialize access.
 */
#ifndef _LINUX_BMAVG_H
#define _LINUX_BMAVG_H

#include <linux/bitops.h>
#include <linux/compiler.h>
#include <linux/math64.h>
#include <linux/string.h>
#include <linux/types.h>

/* Average of a and b, rounded half to even, without overflow. */
#define __bmavg_merge(a, b) ({						\
	typeof(a) __a = (a), __b = (b);					\
	typeof(a) __mid = (__a & __b) + ((__a ^ __b) >> 1);		\
	__mid + (__mid & ((__a ^ __b) & 1));				\
})

/* (hi:lo) / d for d != 0, truncated to 64 bits. */
static inline u64 __bmavg_div128(u64 hi, u64 lo, u64 d)
{
	u64 q = 0, r;
	int i;

	if (likely(!hi))
		return div64_u64(lo, d);

	div64_u64_rem(hi, d, &r);
	for (i = 63; i >= 0; i--) {
		u64 carry = r >> 63;

		r = (r << 1) | ((lo >> i) & 1);
		if (carry || r >= d) {
			r -= d;
			q |= 1ULL << i;
		}
	}
	return q;
}

#define DEFINE_BMAVG(BITLEN, HIST_COUNT)				\
struct bmavg_u##BITLEN {						\
	u##BITLEN hist[HIST_COUNT];					\
	u##BITLEN count;						\
	int limit_bitlen;						\
};									\
									\
static inline void bmavg_write_u##BITLEN(struct bmavg_u##BITLEN *b,	\
					 u##BITLEN v)			\
{									\
	u##BITLEN fmask = (u##BITLEN)1 << b->limit_bitlen;		\
	u##BITLEN hmask = (u##BITLEN)1 << (b->limit_bitlen - 1);		\
	u##BITLEN curr = b->count, next, carried = v, carry_bmp;	\
	bool full = curr == fmask - 1, should_halve;			\
	int pos;							\
									\
	if (unlikely(full))						\
		curr = hmask - 1;					\
	next = curr + 1;						\
	should_halve = !(next % hmask);					\
	carry_bmp = (next ^ curr) >> 1;					\
									\
	for (pos = 0; (carry_bmp >> pos) & 1; pos++) {			\
		carried = __bmavg_merge(carried, b->hist[pos]);		\
		b->hist[pos] = 0;					\
	}								\
	if (unlikely(full))						\
		carried = __bmavg_merge(carried, b->hist[pos]);		\
	b->hist[pos] = carried;						\
	b->count++;							\
	if (unlikely(should_halve))					\
		b->count = hmask;					\
}									\
									\
static inline u##BITLEN bmavg_read_u##BITLEN(struct bmavg_u##BITLEN *b)	\
{									\
	u64 hi = 0, lo = 0;						\
	int pos;							\
									\
	if (unlikely(!b->count))					\
		return 0;						\
									\
	pos = min_t(int, fls64(b->count) - 1, b->limit_bitlen);	\
	for (; pos >= 0; pos--) {					\
		u64 h, add;						\
									\
		if (!((b->count >> pos) & 1))				\
			continue;					\
		h = b->hist[pos];					\
		add = h << pos;						\
		if (pos)						\
			hi += h >> (64 - pos);				\
		lo += add;						\
		hi += lo < add;						\
	}								\
	return (u##BITLEN)__bmavg_div128(hi, lo, b->count);		\
}									\
									\
static inline void bmavg_init_u##BITLEN(struct bmavg_u##BITLEN *b)	\
{									\
	memset(b->hist, 0, sizeof(b->hist));				\
	b->limit_bitlen = HIST_COUNT;					\
	b->count = 0;							\
}									\
									\
static inline void bmavg_set_limit_u##BITLEN(struct bmavg_u##BITLEN *b,	\
					     int limit)			\
{									\
	int pos;							\
									\
	for (pos = limit; pos < HIST_COUNT; pos++)			\
		b->hist[pos] = 0;					\
	b->limit_bitlen = limit;					\
	b->count &= ((u##BITLEN)1 << limit) - 1;			\
}

DEFINE_BMAVG(8, 5)
DEFINE_BMAVG(16, 12)
DEFINE_BMAVG(32, 28)
DEFINE_BMAVG(64, 58)

#endif /* _LINUX_BMAVG_H */
