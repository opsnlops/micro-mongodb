# Porting libbson

micro-mongodb vendors a stripped libbson from
[mongo-c-driver](https://github.com/mongodb/mongo-c-driver), specifically the
**1.28.0** tag. This document explains what's vendored, what was modified,
and why — useful both for syncing with upstream and for understanding the
driver's licensing/origin story.

## Why 1.28.0?

1.28 is the last release where libbson was self-contained — i.e. didn't
depend on a separate `common/mlib` library that mongo-c-driver introduced
in the 1.29 refactor. That refactor pulls in a substantial amount of
shared infrastructure (atomics, threading, string utilities, `mlib/`
header-only helpers) that's a pain to vendor onto an MCU.

For our use case, the BSON spec hasn't changed in years; staying on 1.28
costs us nothing and saves significant porting effort.

## What's vendored

```
src/bson/
├── bson/         # libbson source (src/libbson/src/bson/ from mongo-c-driver 1.28.0)
└── common/       # selected files from src/common/ (b64 only -- thread.c/md5.c skipped)
```

### Compiled

| File | Notes |
|---|---|
| `bson.c`, `bson-iter.c`, `bson-writer.c` | Core encode/iterate/buffer-writer |
| `bson-oid.c`, `bson-context.c` | ObjectID generation |
| `bson-utf8.c`, `bson-string.c` | String utilities |
| `bson-value.c` | bson_value_t union |
| `bson-memory.c` | malloc/free hooks (we use libc/newlib heap; could route through pvPortMalloc) |
| `bson-clock.c`, `bson-keys.c`, `bson-atomic.c` | Clock, keys, atomics |
| `bson-decimal128.c` | The decimal128 type (used by iter, even if we don't surface decimal128 in our API) |
| `bson-error.c` | Patched -- see below |
| `bson-iso8601.c`, `bson-timegm.c` | Used internally by bson.c's JSON output path |
| `bcon.c`, `bson-version-functions.c` | Convenience, ~5 KB total |
| `common-b64.c` | Patched -- see below |

### Not compiled (skipped at the CMake level)

| File | Why skipped |
|---|---|
| `bson-md5.c` | Used only by the legacy SCRAM-SHA-1 path, which we drive from mbedTLS instead. |
| `bson-json.c`, `jsonsl.*` | JSON *parsing*. We don't need it; the firmware only emits BSON, never receives JSON. |
| `bson-reader.c` | File-I/O streaming BSON reader. We use `bson_init_static` on in-memory buffers instead. |
| `common-thread.c`, `common-md5.c` | Pthread-based threading and shared MD5. Replaced/avoided. |

## Patches applied to upstream code

Each patch is small and marked with `/* PORT: */` style comments where
relevant. Listed roughly in order of severity:

### `src/bson/bson/bson-error.c`

`bson_strerror_r` walked through a chain of `#if defined(_WIN32) / __APPLE__
/ _XOPEN_SOURCE >= 700 / _GNU_SOURCE` blocks looking for a thread-safe
`strerror` variant. None of them matched arm-none-eabi + newlib-nano, so
the source hit `#error "Unable to find a supported strerror_r candidate"`
at compile time.

Added a branch at the top:

```c
#if defined(__arm__) || defined(__riscv)
    // newlib-nano single-threaded; plain strerror() is safe and
    // strerror_l/uselocale aren't available.
    char *msg = strerror(err_code);
    if (msg) (void)bson_strncpy(buf, msg, buflen);
#elif defined(_WIN32) ...
```

### `src/bson/bson/bson-atomic.c`

`bson_thrd_yield()` calls `sched_yield()` (POSIX). newlib ships `<sched.h>`
but gates the declaration behind feature-test macros that aren't set by
pico-sdk's compile flags.

Added an explicit `extern int sched_yield(void);` forward declaration.
The definition lives in `src/bson/port_pico.c` and calls FreeRTOS's
`taskYIELD()`.

### `src/bson/common/common-b64.c`

`common-b64.c` had its own little `mongoc_common_once_t` (separate from the
`bson_once` in `common-thread-private.h`) that used `pthread_once` directly
on POSIX systems. Same problem — no pthreads on Pico.

Added an `#if defined(__arm__) || defined(__riscv)` branch that uses our
single-flag once primitive (same as the one in our shimmed
`common-thread-private.h`).

### `src/bson/common/common-thread-private.h`

The upstream version is ~140 lines of pthread wrappers (`bson_mutex_t`,
`bson_thread_t`, `bson_once_t`, etc.). libbson 1.28 only actually uses
`bson_once` (once, in `bson-context.c`'s default-context lazy-init), so
we replaced the whole file with a ~30-line shim that:

- Defines `bson_once_t` as `int` with `BSON_ONCE_INIT = 0`.
- Implements `bson_once` as a `do { if (*flag == 0) { fn(); *flag = 1; } } while (0)`.
- Provides `mcommon_thread_create` / `mcommon_thread_join` *declarations*
  but no definitions (so anything calling them link-errors — but nothing
  in libbson actually does).

### `src/bson/port_pico.c`

A new file we added. Contains:

- `sched_yield()` → `taskYIELD()`
- `gethostname()` → returns `"pico"` (libbson's `bson_context_t` seeds OID
  generation with a hostname hash; we don't have a real hostname).

### Hand-rolled config headers

| File | Replaces |
|---|---|
| `src/bson/bson/bson-config.h` | `bson/config.h.in` (autoconf/cmake template) |
| `src/bson/bson/bson-version.h` | `bson/bson-version.h.in` |
| `src/bson/common/common-config.h` | `common/common-config.h.in` |

The values mostly indicate platform features (`BSON_HAVE_STDBOOL_H = 1`,
`BSON_HAVE_CLOCK_GETTIME = 1`, etc.). Endianness is fixed to little (RP2040
and RP2350 are both little-endian Cortex-M).

## Updating to a newer libbson

If a future MongoDB BSON change made it worth upgrading:

1. **Try the patch-on-top approach.** Bump the vendor tree to the new tag,
   reapply the patches above (they're small and well-documented), and see
   what breaks. The error-handling and threading shims are the most likely
   to need updates.

2. **Brace for the common/mlib refactor.** Anything 1.29 or newer drags
   in `src/common/src/` and `src/common/src/mlib/`. You'd need to vendor
   those directories too, plus reshape the include paths so
   `<common-*-private.h>` and `<mlib/*.h>` resolve.

3. **Consider the JSON path tax.** `bson.c` in newer versions includes
   `common-json-private.h` and `mlib/intencode.h`, which pull in more
   dependencies. The cleanest port may end up needing `common-string.c`,
   `common-json.c`, and the `mlib/` headers.

For the demo and any reasonable telemetry use case, 1.28.0 is fine.

## License

libbson (and the rest of mongo-c-driver) is Apache 2.0. micro-mongodb
inherits that license for the vendored code; our own code in `src/umongoc/`,
`src/app/`, `src/logging/` matches.
