# micro-mongodb

A minimal MongoDB driver in C for the Raspberry Pi Pico 2 W (RP2350), built on
top of `pico-sdk` + FreeRTOS + lwIP + mbedTLS. Connects to MongoDB Atlas in
production and a local `mongod` container for development.

This is intentionally *not* a port of the full `mongo-c-driver`. It's a stripped
client that handles:

- BSON encode/decode (vendored libbson core)
- OP_MSG wire protocol
- SCRAM-SHA-256 auth (ASCII passwords only — see SASLprep note below)
- TLS via mbedTLS for Atlas
- `mongodb+srv://` SRV/TXT lookups
- `insert` / `find` (cursor iteration) / `update` / `delete` / `runCommand`

Connection pooling, retryable writes, sessions, change streams, GridFS,
transactions, SDAM, JSON conversion, decimal128, and non-ASCII SCRAM passwords
are explicitly out of scope.

## Hardware

Pico 2 W (RP2350 chip, 520 KB SRAM, 4 MB flash, CYW43439 WiFi).

## Build

You'll need:

- `arm-none-eabi-gcc` toolchain
- CMake ≥ 3.25
- The `pico-sdk` submodule (already wired in this repo): `git submodule update --init`

```bash
mkdir build && cd build
cmake .. \
  -DWIFI_SSID="your-network" \
  -DWIFI_PASSWORD="your-password" \
  -DMONGO_URI="mongodb://192.168.1.42:27017"
make -j
```

The output `micro_mongodb_app.uf2` can be flashed by holding BOOTSEL and
dragging it onto the Pico's mass-storage device.

`FreeRTOS-Kernel` is pulled in automatically via CMake `FetchContent` on first
configure. To pin a specific version, drop the kernel into `./FreeRTOS-Kernel/`
(submodule or plain clone) and CMake will use that instead.

## Connecting to MongoDB

### Local dev container

```bash
docker run -d --name mongo-dev -p 27017:27017 mongo:7
```

Find the host's LAN IP (`ipconfig getifaddr en0` on macOS) and pass it as
`MONGO_URI`. No TLS, no auth — the fastest bring-up loop.

### Atlas

Use the connection string from the Atlas UI as `MONGO_URI`. Pass a real SCRAM
user/password (Atlas allowlist must include your LAN's egress IP).

## Caveats

- **ASCII passwords only.** Full RFC 3454 SASLprep is ~100 KB of Unicode
  normalization tables; we punt and reject non-ASCII passwords with a clear
  error at config time. If you need a non-ASCII password, generate an ASCII one
  for the embedded device.
- **No `$clusterTime`/`lsid`.** Single-connection clients on Atlas work fine
  without these for simple ops.
- **No retries.** The caller handles transient failures.

## Layout

```
src/
  app/        FreeRTOS entry point + bring-up
  bson/       Vendored libbson core (added in milestone 2)
  umongoc/    Wire protocol, transport, SCRAM, URI/DNS (added later)
```

See `/Users/april/.claude/plans/i-want-to-make-shimmering-clover.md` for the
full design plan.
