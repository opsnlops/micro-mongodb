# micro-mongodb

A minimal MongoDB driver written in C for the Raspberry Pi Pico W and Pico 2 W.
Connects directly to **MongoDB Atlas** over TLS + SCRAM, or to a local
`mongod` container. Runs under FreeRTOS on top of `pico-sdk` + lwIP + mbedTLS.

The included demo writes a sample document every 10 seconds to a Atlas
time-series collection, while a second task asks Atlas for a rolling
average of the last 10 samples every 19 seconds — all from a $7 microcontroller
with 264 KB of RAM.

```
[I] === micro-mongodb boot (board=pico_w) ===
[I] [wifi] connected
[I] [time] SNTP synced
[I] [app] wall clock: 2026-05-11T20:32:45Z
[I] [client] ready (host=conops-shard-00-01.ata6b.mongodb.net:27017)
[I] [client] created time-series collection micro_mongodb.telemetry
[I] [telemetry] sample=655 ts=2026-05-11T20:32:51Z
[I] [telemetry] sample=197 ts=2026-05-11T20:33:01Z
[I] [agg] last 10 samples: avg=463.4 min=168 max=1021
```

## What's in the box

A single high-level client API:

```c
#include "micro_mongodb.h"

mongo_client_t *c = mongo_client_new(&(mongo_client_config_t){
    .mongo_uri = "mongodb+srv://user:pass@cluster.mongodb.net/",
    .app_name = "my-pico-thing",
});

mongo_client_insert(c, "db", "coll", &doc, &reply, 5000);
mongo_client_find(c, "db", "coll", &filter, 10, &cursor, 5000);
mongo_client_update(c, "db", "coll", &filter, &update, false, &reply, 5000);
mongo_client_delete(c, "db", "coll", &filter, &reply, 5000);
mongo_client_run_command(c, "db", &cmd, &reply, 5000);
```

Underneath: TLS handshake against Atlas's chain, SCRAM-SHA-256 / SCRAM-SHA-1
authentication (chosen automatically based on what your user has stored),
replica-set primary discovery, automatic reconnect on transport errors,
and an internal mutex so multiple FreeRTOS tasks can share one client.

## Hardware

Built and tested on:

- **Raspberry Pi Pico W** (RP2040, dual Cortex-M0+ @ 133 MHz, 264 KB SRAM, 2 MB flash)
- **Raspberry Pi Pico 2 W** (RP2350, dual Cortex-M33 @ 150 MHz, 520 KB SRAM, 4 MB flash)

Either board uses the CYW43439 WiFi chip. Same firmware works on both.

## Quick start

You'll need:

- `arm-none-eabi-gcc` (15.x recommended — `brew install --cask gcc-arm-embedded` on macOS)
- CMake ≥ 3.25
- `picotool` (optional, for flashing over USB)
- The Pico SDK submodules: `git submodule update --init --recursive`

Build for Pico W:

```bash
mkdir build && cd build
cmake .. \
  -DPICO_BOARD=pico_w \
  -DWIFI_SSID="your-network" \
  -DWIFI_PASSWORD="your-password" \
  -DMONGO_URI="mongodb+srv://user:pass@cluster.mongodb.net/"
make -j
```

For Pico 2 W replace `pico_w` with `pico2_w`. Flash the resulting
`src/app/micro_mongodb_app.uf2` to a board in BOOTSEL mode.

See [docs/getting-started.md](docs/getting-started.md) for the full setup
including Atlas-side user creation and IP allowlisting.

## Docs

- [docs/getting-started.md](docs/getting-started.md) — clone-to-running demo, including Atlas-side setup
- [docs/architecture.md](docs/architecture.md) — module-by-module tour of the codebase
- [docs/api.md](docs/api.md) — `mongo_client_t` API reference with error codes
- [docs/atlas-setup.md](docs/atlas-setup.md) — creating the user, role, and IP allowlist in Atlas
- [docs/porting-libbson.md](docs/porting-libbson.md) — what was vendored from mongo-c-driver and why
- [docs/security.md](docs/security.md) — threat model + known limitations
- [docs/perf.md](docs/perf.md) — measured baselines and where time is spent
- [docs/troubleshooting.md](docs/troubleshooting.md) — diagnosing failures from the log output
- [docs/examples.md](docs/examples.md) — adding your own task, swapping random data for a real sensor

## What's *not* in the box

This is a deliberately stripped driver, not a port. Out of scope:

- Connection pooling (one connection per client)
- Server Discovery & Monitoring (SDAM beyond "follow the primary hint from `hello`")
- Retryable writes
- Sessions, `$clusterTime`, causal consistency
- Change streams
- GridFS
- Transactions
- JSON / Extended-JSON conversion in/out of BSON
- Decimal128 arithmetic
- Non-ASCII passwords (SASLprep would be ~100 KB of Unicode tables on its own)
- IPv6
- mongocrypt / CSFLE

For most of these you should be using the actual `mongo-c-driver` on a host
with an MMU and a megabyte of RAM. micro-mongodb is for "I want my
$7 microcontroller to write to Atlas."

## License

Apache 2.0, matching the vendored libbson code from mongo-c-driver.
