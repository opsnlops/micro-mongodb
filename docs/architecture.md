# Architecture

A module-by-module tour of the codebase, bottom-up.

## Layout

```
src/
├── app/         # Demo: WiFi bring-up, telemetry task, aggregator task
├── bson/        # Vendored libbson 1.28.0 + minimal common/ files
├── logging/     # Queue-based async logger (April's standard, ported from creature-controller)
└── umongoc/     # The driver itself
    ├── mongo_transport.{h,c}   # lwIP altcp wrapper with blocking semantics
    ├── mongo_dns.{h,c}         # SRV + TXT DNS over lwIP raw UDP
    ├── mongo_uri.{h,c}         # mongodb:// + mongodb+srv:// parser
    ├── mongo_wire.{h,c}        # OP_MSG framing + run_command
    ├── mongo_crud.{h,c}        # insert / update / delete wrappers
    ├── mongo_cursor.{h,c}      # find + cursor iteration (getMore loop)
    ├── mongo_auth.{h,c}        # hello + SCRAM-SHA-{1,256}
    ├── mongo_ca.{h,c}          # Embedded ISRG Root X1 (Let's Encrypt) CA
    ├── mongo_time.{h,c}        # SNTP-synced wall clock
    ├── mongo_wifi.{h,c}        # cyw43_arch_* glue
    ├── mongo_client.{h,c}      # High-level API: lazy connect, primary discovery, mutex
    └── micro_mongodb.h         # Umbrella header for end-user apps
```

## Layered overview

```
+----------------------------------------------------+
| Application (src/app/*)                            |  ← your code
+----------------------------------------------------+
| mongo_client    (lazy connect, mutex, reconnect)   |
+----------------------------------------------------+
| mongo_crud, mongo_cursor, mongo_auth               |  ← OP_MSG-level operations
+----------------------------------------------------+
| mongo_wire      (OP_MSG framing + run_command)     |
+----------------------------------------------------+
| mongo_transport (TLS/TCP, blocking facade)         |
+----------------------------------------------------+
| lwIP altcp + mbedTLS + cyw43-driver  (pico-sdk)    |
+----------------------------------------------------+
```

End-user code only ever touches the top two layers (`mongo_client_*`) via
`micro_mongodb.h`. The lower layers are public for advanced use (writing
your own client, embedding the driver in a custom architecture, etc.) but
the umbrella header doesn't expose them.

## Module responsibilities

### `mongo_transport`

Wraps lwIP's callback-style altcp API into a blocking `send` / `recv_exact`
suitable for serialized request-response use. Handles both plain TCP (via
`altcp_tcp_new`) and TLS (via `altcp_tls_new`). Single connection per
transport; the client layer manages reconnects.

Threading: lwIP's callbacks fire on the `tcpip_thread` task; we signal the
owner task with a binary semaphore. App-side calls into lwIP are protected
with `cyw43_arch_lwip_begin()` / `lwip_end()`.

Notable subtleties:
- `pbuf_cat` / `pbuf_ref` dance in the recv path so we can free head pbufs
  without cascading the rest of the chain.
- `altcp_recved` is chunked for >64 KB drains (the underlying function takes
  a `uint16_t` credit).

### `mongo_dns`

lwIP's stock resolver does A/AAAA only. `mongodb+srv://` URIs need SRV
records (for the host list) and TXT records (for `replicaSet` / `authSource`),
so we hand-craft RFC 1035 query packets over UDP. Same callback-to-semaphore
pattern as the transport layer. Includes proper handling of DNS name
compression (the `0xc0` pointer labels) — that's the part most hand-rolled
DNS parsers get wrong.

### `mongo_uri`

Parses `mongodb://[user:password@]host[:port][,host2...][/db][?opts]` and
`mongodb+srv://`. For `+srv`, calls `mongo_dns` for the SRV (host list) and
TXT (options) lookups. URL-decodes credentials so `@`, `/`, `:` etc. in
passwords work. Fixed-size buffers (8 hosts max, 256-char names) keep the
parser allocation-free.

### `mongo_wire`

OP_MSG framing (opcode 2013). Header + flagBits + section type 0 (body).
Section type 1 (document sequences) isn't currently emitted — bulk insert
inlines documents as an array in section 0. That's slightly less efficient
over the wire but much simpler in code.

Also includes the shared error-formatting helper `mongo_log_reply_error`
that surfaces server-side `errmsg` / `code` / `codeName` / `writeErrors[0]`.

### `mongo_crud`, `mongo_cursor`

Thin wrappers over `mongo_run_command` that build the appropriate command
BSON for `insert`, `update`, `delete`, and `find`. The cursor handles the
`getMore` loop transparently — `mongo_cursor_next` returns documents one
at a time until the batch drains, then refills from the server.

`mongo_cursor` returns *borrowed* `bson_t *` views into the internal batch;
the doc is invalidated by the next `_next()` or `_destroy()` call. This
keeps the iteration zero-allocation in the common case at the cost of a
small lifetime gotcha.

### `mongo_auth`

`hello` handshake (with optional `saslSupportedMechs` query to learn which
auth mechanisms a specific user supports) + SCRAM-SHA-256 / SCRAM-SHA-1
state machine.

The SCRAM exchange is parameterized over a `scram_mech_t` descriptor
(hash type, key length, password preprocessor) so the same code body
handles both mechanisms. SCRAM-SHA-1 brings along MongoDB's legacy
`md5(user + ":mongo:" + password)` derivation, which SCRAM-SHA-256 doesn't
do.

Crypto primitives come from mbedTLS: PBKDF2-HMAC-SHA{1,256}, HMAC-SHA{1,256},
SHA-{1,256}, base64, MD5 (SHA-1 only).

ASCII-only passwords. Full RFC 7613 SASLprep would be ~100 KB of Unicode
tables; we punt and reject non-ASCII passwords with a clear error.

### `mongo_client`

The end-user API. Owns:

- The `mongo_uri_t` (parsed once at `new`).
- The current `mongo_transport_t` (lazily created, rebuilt on reconnect).
- A target `host:port` that may drift across cycles via primary-discovery
  redirect.
- An internal mutex so multiple FreeRTOS tasks can share one client.

CRUD calls go through a `CLIENT_DO` macro that takes the mutex, lazy-connects
if needed, invokes the underlying `mongo_*` function, marks the client
disconnected on transport errors, and unlocks. Connect logic is factored
into `connect_locked` so the macro doesn't double-take the mutex.

### `mongo_time`

SNTP-synced wall clock. lwIP's built-in SNTP module calls
`SNTP_SET_SYSTEM_TIME(sec)` (defined in `lwipopts.h` to point at our
callback) each time a fix lands. We translate seconds into a "boot offset"
so the Pico's monotonic clock keeps running and adding the offset gives
Unix-epoch milliseconds. Re-syncs automatically every `SNTP_UPDATE_DELAY`
(1 hour by default).

### `mongo_wifi`

Three-line wrapper around `cyw43_arch_init` + `cyw43_arch_enable_sta_mode` +
`cyw43_arch_wifi_connect_timeout_ms`, just so app `main()` doesn't carry
that boilerplate.

### `mongo_ca`

The ISRG Root X1 (Let's Encrypt) PEM as a `const char[]`. Atlas's chain
roots here. Apps can substitute their own CA via `mongo_tls_config_t.ca_pem`
for self-hosted deployments.

## Build system

CMake. The top-level `CMakeLists.txt`:

1. Defaults `PICO_SDK_PATH` to the vendored submodule (overriding any stale
   environment variable — a common gotcha).
2. Branches on `PICO_BOARD` to select the right FreeRTOS-Kernel port
   (`RP2040` vs `RP2350_ARM_NTZ`) and platform define.
3. Fetches FreeRTOS-Kernel via CMake `execute_process(git clone)` on first
   configure. Cached in `build/_deps/`.
4. Pulls in Pico SDK and FreeRTOS, then `add_subdirectory(src)`.

`src/bson/CMakeLists.txt` builds the vendored libbson as a static library
with carefully chosen `.c` files (we skip the JSON parser, file-I/O reader,
and a few other peripherals — see [porting-libbson.md](porting-libbson.md)).

`src/umongoc/CMakeLists.txt` builds the driver lib. The `NTP_SERVER` cmake
option lands here as a `target_compile_definitions`.

`src/app/CMakeLists.txt` builds the demo binary. WiFi credentials and the
Mongo URI come in as cmake cache variables.

## What runs on which core

The CYW43 driver + lwIP + FreeRTOS port doesn't cleanly tolerate the
network stack being preempted across cores. We pin everything network-
adjacent to core 0 via `vTaskCoreAffinitySet(handle, 1 << 0)`. Core 1 is
free for user work that doesn't touch the network.

## Logging

`src/logging/` is April's standard queue-based async logger ported from
the `creature-controller` project. Call sites use `info()` / `debug()` /
`error()` etc.; messages go through a FreeRTOS queue to a dedicated reader
task that timestamps them and hands them to `acw_post_logging_hook`
(implemented in `src/app/logger_hook.c` to print to USB-CDC stdout).

The logger is safe to call from ISR or task context — it dispatches via
the CMSIS `mrs ipsr` instruction (more portable than `xPortIsInsideInterrupt`
which isn't exposed on the RP2040 port).
