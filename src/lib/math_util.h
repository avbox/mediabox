/**
 * Copyright (c) 2016-2017 Fernando Rodriguez - All rights reserved
 * This file is part of mediabox.
 */

#ifndef __MB_MATH_H__
#define __MB_MATH_H__

#include <math.h>


/* MIN/MAX macros */
#define MAX(a, b) ((a > b) ? a : b)
#define MIN(a, b) ((a < b) ? a : b)


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

#endif
