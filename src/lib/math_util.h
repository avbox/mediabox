/**
 * Copyright (c) 2016-2017 Fernando Rodriguez - All rights reserved
 * This file is part of mediabox.
 */

#ifndef __MB_MATH_H__
#define __MB_MATH_H__

#include <math.h>

/* TODO: Just rename ours to something else */
#ifdef MIN
#undef MIN
#endif
#ifdef MAX
#undef MAX
#endif

/* MIN/MAX macros */
#define MAX(a, b) ((a > b) ? a : b)
#define MIN(a, b) ((a < b) ? a : b)

struct avbox_rational
{
	int num;
	int den;
};


/**
 * isprime() -- Determine if a number is prime.
 */
static inline int
isprime(int number)
{
	int i, max;

	max = (int) sqrt(number);

	for (i = 2; i <= max; i++) {
		if (number % i == 0) {
			return 0;
		}
	}
	return 1;
}


/**
 * Reduce a rational number.
 */
static inline void
avbox_rational_reduce(struct avbox_rational * const in,
	struct avbox_rational *out)
{
	int num, den, div, min;
	num = in->num;
	den = in->den;
	min = (num > den) ? den : num;
	for (div = den; div > min; div--) {
		if (num % div == 0 && den % div == 0) {
			num /= div;
			den /= div;
			break;
		}
	}
	out->num = num;
	out->den = den;
}


#endif
