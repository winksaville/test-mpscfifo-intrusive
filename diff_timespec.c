/**
 * This software is released into the public domain.
 */

#include "diff_timespec.h"

/**
 * Return the difference between to timespec in nano seconds
 */
double diff_timespec_ns(struct timespec* t1, struct timespec* t2) {
   double t1_ns = (t1->tv_sec * ns_flt) + t1->tv_nsec;
   double t2_ns = (t2->tv_sec * ns_flt) + t2->tv_nsec;
   double diff = t1_ns - t2_ns;
   return diff;
}
