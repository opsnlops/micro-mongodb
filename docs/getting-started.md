# Getting started

Clone-to-telemetry-flowing-into-Atlas in roughly 15 minutes, most of which is
Atlas signup.

## 1. Toolchain

You need `arm-none-eabi-gcc`, CMake, and (optional) `picotool`.

### macOS

```bash
brew install cmake
brew install --cask gcc-arm-embedded
brew install picotool
```

Verify:

```
$ arm-none-eabi-gcc --version    # >= 13.x for RP2350 / armv8-m.main+fp
$ cmake --version                # >= 3.25
```

### Linux

Most distros' `gcc-arm-none-eabi` package is too old (≤ 10.x) and won't know
about Cortex-M33. Use the official ARM toolchain from
<https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads> or your
distro's "embedded" backport.

## 2. Clone

```bash
git clone git@github.com:your/micro-mongodb.git
cd micro-mongodb
git submodule update --init --recursive
```

The recursive flag matters: the Pico SDK has its own submodules
(`cyw43-driver`, `lwip`, `mbedtls`) we need.

FreeRTOS-Kernel is fetched automatically by CMake on first configure (one-time
network operation, cached in `build/_deps/`).

## 3. Atlas setup

If you have a local MongoDB to test against, skip ahead to step 4 and use
`mongodb://your-host:27017` as the URI.

Otherwise, on Atlas (free tier is fine):

1. **Create a cluster** if you don't have one. M0 (free) is enough.
2. **Database Access → Add new database user.**
   - Username + password (note the password — you'll bake it into the URI).
   - **Role:** `readWriteAnyDatabase` (or grant `readWrite` on the specific
     database your demo will use; `micro_mongodb` is the default).
3. **Network Access → Add IP Address.**
   - Whatever your network's egress IP is. (The Pico's outbound traffic comes
     from your home/office NAT, not the Pico itself.)
   - You can use `0.0.0.0/0` while testing but obviously don't ship that.
4. **Database → Connect → Drivers** → copy the connection string. It looks like:
   ```
   mongodb+srv://your-user:<password>@your-cluster.mongodb.net/
   ```
   Replace `<password>` with the actual password from step 2.

> **Note on auth mechanisms.** Some Atlas users (especially older ones, or
> ones created via the shell with explicit `mechanisms: ["SCRAM-SHA-1"]`)
> only have SCRAM-SHA-1 credentials stored. The driver handles this
> automatically — it asks the server which mechanism your user supports and
> picks accordingly — but you'll see a `[W] using legacy mechanism` line in
> the log. To get SCRAM-SHA-256, recreate the user via the Atlas UI on a
> modern cluster; new users get both mechanisms by default.

See [atlas-setup.md](atlas-setup.md) for the long version with screenshots'
worth of detail.

## 4. Build

```bash
mkdir build && cd build
cmake .. \
  -DPICO_BOARD=pico_w \
  -DWIFI_SSID="your-network" \
  -DWIFI_PASSWORD="your-password" \
  -DMONGO_URI="mongodb+srv://your-user:your-pass@your-cluster.mongodb.net/"
make -j
```

For Pico 2 W: `-DPICO_BOARD=pico2_w`.

Result: `build/src/app/micro_mongodb_app.uf2`.

### Optional cmake flags

| Flag | Default | What it does |
|---|---|---|
| `-DPICO_BOARD=` | `pico2_w` | `pico_w` for RP2040, `pico2_w` for RP2350 |
| `-DNTP_SERVER=` | `pool.ntp.org` | SNTP server for wall-clock sync |
| `-DMONGO_SCRAM_DEBUG=1` | off | Dump SCRAM intermediates to log. **Leaks salted-password material — only for debugging auth.** |

## 5. Flash

Hold the BOOTSEL button on the Pico, plug it in, then either:

- **Drag the .uf2** onto the USB mass-storage device that appears, or
- **`picotool load -f build/src/app/micro_mongodb_app.uf2 && picotool reboot`**

## 6. Watch the log

The Pico exposes a USB-CDC serial port. Connect with `screen` / `minicom` /
`tio`:

```bash
ls /dev/cu.usbmodem*           # macOS
ls /dev/ttyACM*                # Linux
screen /dev/cu.usbmodem1234 115200
```

Expected boot sequence:

```
=== micro-mongodb boot (board=pico_w) ===
[wifi] connecting to 'your-ssid' ...
[wifi] connected
[time] SNTP synced
[app] wall clock: 2026-05-11T20:32:45Z
[client] connecting to ...mongodb.net:27017 (TLS)
[auth] saslSupportedMechs for admin.your-user: [SCRAM-SHA-256]
[client] ready (host=...)
[client] created time-series collection micro_mongodb.telemetry
[telemetry] sample=655 ts=2026-05-11T20:32:51Z
[telemetry] sample=197 ts=2026-05-11T20:33:01Z
[agg] last 2 samples: avg=426.0 min=197 max=655
```

If something fails, the log includes structured error info. See
[troubleshooting.md](troubleshooting.md).

## 7. Verify in Atlas

Open Atlas → Database → Browse Collections. You should see
`micro_mongodb.telemetry` listed as a **Time Series** collection, with the
documents you've inserted so far. Each one has `ts`, `board`, and `value` fields.

For a nice visualization: Atlas Charts → add a chart → use `ts` as the time
axis, `value` as the metric, and `board` as a category. You'll get a live
line chart of your Pico's samples.

## Where to from here

- Replace the random-value placeholder with a real sensor: see [examples.md](examples.md).
- Build a Pico W vs Pico 2 W perf comparison: same firmware, just rebuild with
  the other `-DPICO_BOARD=...` value, then compare `[agg]` and connect/auth
  timings.
