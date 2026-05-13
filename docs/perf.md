# Performance

Measured numbers against MongoDB Atlas free tier. The headline: on a
Pico W (Cortex-M0+ @ 133 MHz), bring-up to first-CRUD takes ~3-4
seconds, dominated by TLS handshake and SCRAM PBKDF2 — both pure
software crypto. The Pico 2 W (Cortex-M33 @ 150 MHz) comes in about
**1.2× faster on TLS and 1.9× faster on SCRAM** — real improvement
but more modest than the back-of-envelope 3-4× we initially projected.

The numbers below are with mbedTLS doing all crypto in software on
both boards. RP2350 actually has a hardware **SHA-256 accelerator**
(see `pico/sha256.h`); we aren't wired into it yet. RP2350 does *not*
have hardware AES or P-256 ECC, so the remaining handshake cost
(ECDHE + ECDSA + AES-GCM record encryption) is unavoidable in
software. A future task to plumb `MBEDTLS_SHA256_ALT` through to
`pico_sha256_*` would compress the TLS PRF / Finished / cert-sig-hash
slice further on the M33.

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

### Where the time goes (Pico 2 W cold start)

Same shape, crypto slice is the only thing that moves:

```
WiFi assoc + DHCP    [████████████████████]              11 s
SNTP                 [██]                                 3 s
DNS SRV+TXT          [▍]                                 0.2 s
TLS handshake        [████]                              2.1 s
hello                [▎]                                 0.1 s
SCRAM-SHA-1          [█]                                 0.6 s
First insert         [▎]                                 0.1 s
                     └─────────────────────────────── 17 s total
```

WiFi/SNTP are CYW43439-bound, not CPU-bound, so the wins there
(when present) are pure variance. The crypto wins are real.

## Pico 2 W measured (RP2350, M33 @ 150 MHz)

Same firmware, just `cmake -DPICO_BOARD=pico2_w ..`. Measured against
the same Atlas free-tier cluster on 2026-05-13:

| Operation | Pico W | Pico 2 W | Speedup |
|---|---|---|---|
| TLS handshake + hello | ~2.5 s | **~2.1 s** | 1.2× |
| SCRAM-SHA-1 (PBKDF2 10000 iter) | ~1.10 s | **~590 ms** | 1.9× |
| Connect-to-ready (single handshake) | ~3.7 s | ~2.7 s | ~1.4× |
| CRUD ops (network-bound) | unchanged | unchanged | ~1× |

CRUD operations are dominated by network round-trip time, so they
don't change. The crypto path does speed up — but less than we
projected when we wrote this page from first principles.

### Why the speedup is smaller than projected

The original projection assumed M33 would close the gap on SHA-256 and
P-256 ECC the way it does on general multiply-heavy code. In practice:

- **Hardware SHA-256 is present but not used.** RP2350 has a dedicated
  SHA-256 accelerator that mbedTLS doesn't know about; software SHA-256
  is what runs today. Wiring `MBEDTLS_SHA256_ALT` through to
  `pico_sha256_*` is an open optimization.
- **No hardware P-256 ECC on RP2350.** ECDHE and ECDSA verify both
  reduce to modular exponentiation in software. The M33 hardware
  multiplier helps the inner bignum ops, but the overall path is
  still bound by the same algorithm with the same iteration counts.
- **No hardware AES on RP2350.** TLS-record symmetric crypto is a
  small fraction of handshake cost (most of the work is the asymmetric
  side), so this doesn't dominate, but it's also software.
- **Clock bump:** 150 MHz / 133 MHz = 1.13×. That, combined with M33's
  ~2× better cycle counts on multiply-heavy bignum, is essentially
  the entire speedup we measured.

The demo still lands — there's a visible, repeatable speedup — but the
honest framing is "M33 is ~2× faster on auth crypto," not "M33 closes
the gap to hardware-accelerated TLS." Wiring `MBEDTLS_SHA256_ALT` to
the RP2350 hardware unit is the most leveraged next step; it would
help every SHA-256-touching path (TLS PRF, TLS Finished, cert-sig
hash, HMAC-SHA-256, SCRAM-SHA-256 PBKDF2).

### Caveats

- Times vary 10-20% across runs depending on which Atlas shard answers
  the connection and on network jitter.
- The "connect-to-ready" headline assumes a single TLS handshake. When
  the SRV cycle lands on a secondary first and the driver redirects
  to the primary, you pay the TLS+hello cost twice (~4.8 s end-to-end
  in the test run on 2026-05-13). Caching the last-known primary
  (which the client does on reconnect) skips the second handshake on
  steady-state cycles.

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
