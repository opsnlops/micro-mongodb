/*
 * common-thread-private.h — micro-mongodb embedded port.
 *
 * Replaces mongo-c-driver's pthread-based threading shim with the minimum
 * libbson 1.28.0 actually uses: `bson_once` for the default-context's lazy
 * initializer (bson-context.c). Everything else (mutexes, shared mutexes,
 * thread create/join) is intentionally omitted -- libbson does not call into
 * them, and libmongoc is not being ported.
 *
 * Single-task semantics: callers are expected to initialize libbson before
 * spawning multiple FreeRTOS tasks, OR to ensure that bson_context_get_default()
 * is called from only one task. The volatile flag below is correct under
 * either of those conditions.
 *
 * Original copyright: Copyright 2009-present MongoDB, Inc. (Apache 2.0).
 */

#include "common-prelude.h"
#include "common-config.h"
#include "common-macros-private.h"

#ifndef COMMON_THREAD_PRIVATE_H
#define COMMON_THREAD_PRIVATE_H

#define BSON_INSIDE
#include "bson/bson-compat.h"
#include "bson/bson-config.h"
#include "bson/bson-macros.h"
#undef BSON_INSIDE

BSON_BEGIN_DECLS

#define mcommon_thread_create COMMON_NAME (thread_create)
#define mcommon_thread_join COMMON_NAME (thread_join)

/* --- bson_once: single-shot initializer used only by bson_context_get_default ---
 *
 * Sequence: bson_once(&flag, &init_fn). If `flag` is zero, call `init_fn`
 * and set `flag` to 1. Subsequent calls are no-ops.
 *
 * Safe under cooperative scheduling and under preemption *if* the caller
 * ensures it's only invoked from one task during startup. The `volatile`
 * keeps the compiler from caching the flag in a register across calls.
 */
typedef int bson_once_t;
#define BSON_ONCE_INIT 0
#define BSON_ONCE_FUN(n) void n (void)
#define BSON_ONCE_RETURN return
#define bson_once(o, c)                                  \
   do {                                                  \
      if (*(volatile bson_once_t *) (o) == 0) {          \
         (c) ();                                         \
         *(volatile bson_once_t *) (o) = 1;              \
      }                                                  \
   } while (0)

/* libbson never calls these on its own; the declarations exist solely so
 * code that #includes this header continues to compile. If anything ever
 * references them, link will fail loudly. */
int mcommon_thread_create (void *thread, void *(*func) (void *), void *arg);
int mcommon_thread_join (void *thread);

BSON_END_DECLS

#endif /* COMMON_THREAD_PRIVATE_H */
