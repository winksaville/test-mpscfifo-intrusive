/**
 * This software is released into the public domain.
 */

#ifndef _DIFF_TIMESPEC_H
#define _DIFF_TIMESPEC_H

#include <sys/types.h>
#include <stdint.h>
#include <time.h>

static const uint64_t ns_u64 = 1000000000ll;
static const double ns_flt = 1000000000.0;

/**
 * Return the difference between to timespec in nano seconds
 */
double diff_timespec_ns(struct timespec* t1, struct timespec* t2);


#endif
