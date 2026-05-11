/*
 * bson-config.h for micro-mongodb on the RP2350 (Pico 2 W).
 *
 * Hand-written to replace the autoconf/cmake-generated config.h.in from
 * mongo-c-driver. Values reflect arm-none-eabi-gcc with newlib-nano under
 * pico-sdk + FreeRTOS.
 */

#ifndef BSON_CONFIG_H
#define BSON_CONFIG_H

/* RP2350 is little-endian. */
#define BSON_BYTE_ORDER 1234

/* newlib has stdbool.h */
#define BSON_HAVE_STDBOOL_H 1

/* Treat newlib as POSIX-ish. unistd.h, sys/time.h, etc. all exist. */
#define BSON_OS 1
#define BSON_OS_UNIX 1

/* newlib does provide clock_gettime via the pico-sdk overlays. */
#define BSON_HAVE_CLOCK_GETTIME 1

#define BSON_HAVE_STRINGS_H 1
#define BSON_HAVE_STRNLEN 1
#define BSON_HAVE_SNPRINTF 1
#define BSON_HAVE_GMTIME_R 1
#define BSON_HAVE_TIMESPEC 1

/* newlib doesn't reliably expose rand_r / strlcpy / aligned_alloc;
 * leave these off so the compat shims trigger. */
#undef BSON_HAVE_RAND_R
#undef BSON_HAVE_STRLCPY
#undef BSON_HAVE_ALIGNED_ALLOC

#endif /* BSON_CONFIG_H */
