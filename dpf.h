/**
 * This software is released into the public domain.
 *
 * Define Debug Printf DPF
 */

#ifndef _DPF_H
#define _DPF_H

#include <stdio.h>

#ifdef NDEBUG
#define DPF(format, ...) ((void)(0))
#else
#define DPF(format, ...)  printf(format, __VA_ARGS__)
#endif

#endif
