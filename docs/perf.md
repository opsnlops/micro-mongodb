# Performance

Measured numbers against MongoDB Atlas free tier. The headline: on a
Pico W (Cortex-M0+ @ 133 MHz), bring-up to first-CRUD takes ~3-4
seconds, dominated by TLS handshake and SCRAM PBKDF2 — both pure
software crypto. The Pico 2 W (Cortex-M33 @ 150 MHz with a hardware
multiplier and better cycle counts on SHA-256 and modular exp) is
expected to be 3-4× faster on those paths.

## Pico W baseline (RP2040, M0+ @ 133 MHz)

Measured 2026-05-11, free-tier Atlas M0, residential WiFi to US-East
region.

### Auth pipeline (per cold start)

| Operation | Time | Bottleneck |
|---|---|---|
| WiFi assoc + DHCP | ~16 s | One-time per boot; not driver-related |
| DNS SRV + TXT | ~200-300 ms | Network RTT to your resolver |
| **TLS 1.2 handshake** | **~2.5 s** | Software P-256 ECDHE + ECDSA verify + AES-GCM setup |
| `hello` round-trip | ~100-180 ms | Network RTT to Atlas |
| **SCRAM-SHA-1 (PBKDF2 10000 iter)** | **~1.10 s** | Software HMAC-SHA-1 |
| **Connect-to-ready (TLS + SCRAM)** | **~3.7 s** | Dominated by the two crypto steps |

### CRUD over Atlas (warm connection)

| Operation | Range | Notes |
|---|---|---|
| ping | 25-50 ms | Lower bound is essentially RTT |
| insert | 70-110 ms | Atlas journal fsync amortized into this |
| find | 17-110 ms | High variance; depends on batch availability |
| update | 18-100 ms | |
| delete | 70-100 ms | |
| Full CRUD cycle | 200-450 ms | All five ops on a warm connection |

### Where the time goes (Pico W cold start)

```
WiFi assoc + DHCP    [████████████████████████████████] 16 s
SNTP                 [█]                                 4 s
DNS SRV+TXT          [▍]                                 0.3 s
TLS handshake        [████]                              2.5 s
hello                [▎]                                 0.1 s
SCRAM-SHA-1          [██]                                1.1 s
First insert         [▎]                                 0.1 s
                     └─────────────────────────────── 24 s total
```

After cold start, the per-cycle cost drops to roughly 200 ms because
WiFi/SNTP/TLS/SCRAM are amortized.

## Pico 2 W projected (RP2350, M33 @ 150 MHz)

Same firmware, just `cmake -DPICO_BOARD=pico2_w ..`. Expectations
based on M33's 1.5× higher clock and significantly better cycle counts
for SHA-256 / ECC / AES (the M33 has a single-cycle hardware multiplier
where the M0+ uses a multi-cycle iterative one):

| Operation | Pico W | Pico 2 W (expected) | Speedup |
|---|---|---|---|
| TLS handshake | ~2.5 s | ~600-800 ms | 3-4× |
| SCRAM PBKDF2 (SHA-1, 10000 iter) | ~1.10 s | ~250-400 ms | 3-4× |
| Connect-to-ready | ~3.7 s | ~1 s | ~4× |
| CRUD ops (network-bound) | unchanged | unchanged | ~1× |

CRUD operations are dominated by network round-trip time, so they
won't change much. The dramatic delta is on the crypto path.

To actually measure on a 2 W: flash, capture a few `perf [pico2_w]: ...`
lines and the SCRAM/TLS timings, compare against this baseline.

## Where SRAM goes

On Pico W (264 KB total):

```
ucHeap (FreeRTOS)         131 KB    pvPortMalloc allocations come from here
PBUF_POOL_base (lwIP)      37 KB    incoming network packet buffers
newlib gmtmem              24 KB    gmtime_r / strftime workspace
ram_heap (lwIP malloc)      8 KB    lwIP runtime allocations
FreeRTOS overhead           6 KB    task stacks, queue handles
cyw43_state                 2 KB    WiFi chip driver state
TLS session (transient)    30-50 KB allocated from ucHeap during TLS handshake
                          ────────
Total resident             ~215 KB  leaving ~50 KB free
```

On Pico 2 W (520 KB), with the same numbers, that's ~305 KB free.

## Flash usage

| | Text | BSS |
|---|---|---|
| Pico W (RP2040) | 685 KB / 2 MB | 216 KB / 264 KB |
| Pico 2 W (RP2350) | 657 KB / 4 MB | 215 KB / 520 KB |

The Pico W is bigger because RP2040 only has Thumb-1 instruction
encoding; RP2350 supports the more compact Thumb-2.

mbedTLS contributes the largest single chunk (~120 KB) thanks to TLS
1.2 + ECDSA + RSA + AES-GCM + X.509 + PEM all being enabled. libbson
adds ~110 KB. Our own driver code is roughly ~40 KB. The rest is
Pico SDK + FreeRTOS + lwIP.

## Things that would change the numbers

If you needed to squeeze more out (in approximate order of impact):

1. **Drop libbson's JSON output path.** Disable `bson-iso8601.c` and
   `bson-timegm.c` compilation and patch out `bson_as_json_*` in
   `bson.c`. Saves ~30 KB text + 24 KB BSS (newlib's `gmtmem`).
2. **Drop SCRAM-SHA-1 if you don't need it.** Atlas users created in
   the modern UI have SHA-256; you only need SHA-1 if you have legacy
   users. Saves a few KB plus mbedTLS's SHA-1 + MD5 modules.
3. **Replace mbedTLS with a hardware-accelerated TLS.** Some MCUs have
   crypto coprocessors. The Pico 2 W has the OTBN-style "Crypto
   Companion" but it's underused in mbedTLS today; that's a future
   optimization.
4. **Drop X.509 PEM parsing.** Switch the CA bundle to DER format.
   Saves ~10 KB.

For a temperature-logger demo, none of this is necessary.
