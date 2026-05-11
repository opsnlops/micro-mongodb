/*
 * port_pico.c — micro-mongodb port shims for vendored libbson on the RP2350.
 *
 * Provides the few POSIX-shaped symbols libbson reaches for that newlib-nano
 * does not supply. Keep this file small and obvious -- if it grows, that's
 * usually a sign upstream is dragging in something we should be stubbing
 * inside the patched libbson source instead.
 */

#include "FreeRTOS.h"
#include "task.h"

/* libbson calls sched_yield() from the atomic-emulation fallback path
 * (bson-atomic.c::bson_thrd_yield, only used when the compiler lacks atomic
 * intrinsics for the integer width). On GCC + Cortex-M33 we should never
 * actually hit it, but the linker still resolves the symbol. */
int sched_yield (void)
{
    taskYIELD ();
    return 0;
}
