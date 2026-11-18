// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2013 Davidlohr Bueso <davidlohr.bueso@hp.com>
 *
 *  int_sqrt() is based on the isqrt library:
 *  Copyright (C) 2026 Masahito Suzuki
 *  https://github.com/firelzrd/isqrt
 *
 *  int_sqrt64() is based on the shift-and-subtract algorithm for
 *  computing integer square root from Guy L. Steele.
 */

#include <linux/export.h>
#include <linux/bitops.h>
#include <linux/limits.h>
#include <linux/math.h>

/*
 * Initial estimate table, indexed by (exponent parity << 6 | top 6
 * mantissa bits). Entry i holds floor(sqrt((1 or 2) * (1 + i/64)) * 256):
 *   0..63:   even exponent, sqrt(1 + i/64) * 256
 *   64..127: odd exponent,  sqrt(2 * (1 + i/64)) * 256
 * The resulting estimate is within ~0.8% of sqrt(x).
 */
static const u16 int_sqrt_est[128] = {
	256, 257, 259, 261, 263, 265, 267, 269, 271, 273, 275, 277, 278, 280, 282, 284,
	286, 288, 289, 291, 293, 295, 296, 298, 300, 301, 303, 305, 306, 308, 310, 311,
	313, 315, 316, 318, 320, 321, 323, 324, 326, 327, 329, 331, 332, 334, 335, 337,
	338, 340, 341, 343, 344, 346, 347, 349, 350, 352, 353, 354, 356, 357, 359, 360,
	362, 364, 367, 370, 373, 375, 378, 381, 384, 386, 389, 391, 394, 397, 399, 402,
	404, 407, 409, 412, 414, 417, 419, 422, 424, 426, 429, 431, 434, 436, 438, 441,
	443, 445, 448, 450, 452, 454, 457, 459, 461, 463, 465, 468, 470, 472, 474, 476,
	478, 481, 483, 485, 487, 489, 491, 493, 495, 497, 499, 501, 503, 505, 507, 509,
};

/**
 * int_sqrt - computes the integer square root
 * @x: integer of which to calculate the sqrt
 *
 * Computes: floor(sqrt(x))
 *
 * A table lookup on the leading bits of @x gives an initial estimate
 * accurate to ~0.8%, which one (32-bit) or two (64-bit) Newton-Raphson
 * steps refine to within a few units; a final multiply-compare fixup
 * makes the result exact. This runs in constant time instead of looping
 * once per pair of input bits.
 */
unsigned long int_sqrt(unsigned long x)
{
	const unsigned long max_y = (1UL << (BITS_PER_LONG / 2)) - 1;
	unsigned long y;
	unsigned int e, frac;

	if (x <= 1)
		return x;

	e = __fls(x);
	if (e >= 6)
		frac = (x >> (e - 6)) & 0x3f;
	else
		frac = (x << (6 - e)) & 0x3f;

	y = ((unsigned long)int_sqrt_est[((e & 1) << 6) | frac] << (e >> 1)) >> 8;
	y |= !y;

	y = (y + x / y) >> 1;
#if BITS_PER_LONG > 32
	y = (y + x / y) >> 1;
#endif
	if (y > max_y)
		y = max_y;

	while (y * y > x)
		y--;
	while (y < max_y && (y + 1) * (y + 1) <= x)
		y++;

	return y;
}
EXPORT_SYMBOL(int_sqrt);

#if BITS_PER_LONG < 64
/**
 * int_sqrt64 - strongly typed int_sqrt function when minimum 64 bit input
 * is expected.
 * @x: 64bit integer of which to calculate the sqrt
 */
u32 int_sqrt64(u64 x)
{
	u64 b, m, y = 0;

	if (x <= ULONG_MAX)
		return int_sqrt((unsigned long) x);

	m = 1ULL << ((fls64(x) - 1) & ~1ULL);
	while (m != 0) {
		b = y + m;
		y >>= 1;

		if (x >= b) {
			x -= b;
			y += m;
		}
		m >>= 2;
	}

	return y;
}
EXPORT_SYMBOL(int_sqrt64);
#endif
