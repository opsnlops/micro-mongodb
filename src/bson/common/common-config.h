/* common-config.h for micro-mongodb. Hand-written replacement for the
 * autoconf/cmake-generated header from mongo-c-driver. */

#ifndef COMMON_CONFIG_H
#define COMMON_CONFIG_H

/* Debug assertions are off in release builds. Override at build time
 * with -DMONGOC_ENABLE_DEBUG_ASSERTIONS=1 if needed. */
#ifndef MONGOC_ENABLE_DEBUG_ASSERTIONS
#define MONGOC_ENABLE_DEBUG_ASSERTIONS 0
#endif
#if MONGOC_ENABLE_DEBUG_ASSERTIONS != 1
#  undef MONGOC_ENABLE_DEBUG_ASSERTIONS
#endif

#endif /* COMMON_CONFIG_H */
