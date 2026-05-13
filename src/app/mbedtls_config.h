/* mbedTLS configuration for micro-mongodb on pico-sdk + lwIP altcp_tls.
 *
 * Targets MongoDB Atlas, which uses Let's Encrypt ECDSA certs on the
 * P-256 curve with ECDHE-ECDSA + AES-GCM cipher suites. We also keep
 * RSA + ECDHE-RSA paths enabled because some Atlas deployments and
 * many private mongod deployments still use RSA roots.
 *
 * Out of scope: TLS 1.3 (TLS 1.2 covers Atlas), server-side TLS, DTLS,
 * client certificates (would be needed for X.509 auth -- add later).
 *
 * Based on pico-sdk/test/kitchen_sink/mbedtls_config.h.
 */

#pragma once

/* Some mbedtls sources reference INT_MAX without including limits.h. */
#include <limits.h>

/* The Pico's crypto is software; entropy comes from the RP2x40/RP2350
 * ring oscillator via pico_mbedtls's hardware_poll. */
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT

/* TLS records up to 16 KB. Bumping ssl_in_content_len lets us handle the
 * larger TLS records Atlas occasionally sends (e.g. multi-cert chains in
 * Certificate). */
#define MBEDTLS_SSL_OUT_CONTENT_LEN 2048
#define MBEDTLS_SSL_IN_CONTENT_LEN 16384

#define MBEDTLS_ALLOW_PRIVATE_ACCESS
#define MBEDTLS_HAVE_TIME

/* Enable mbedTLS's hand-tuned inline assembly paths for bignum multiply
 * (see bn_mul.h). Without this, ECDHE / ECDSA / RSA all fall back to a
 * generic C MULADDC loop. The ARM-specific path uses Thumb-2 instructions
 * (notably UMAAL on M33; M0+ uses a different but still hand-tuned ASM
 * stanza) that materially beat the C version on the 256-bit bignum ops
 * we do every TLS handshake. */
#define MBEDTLS_HAVE_ASM

/* TODO: route SHA-256 through RP2350's hardware accelerator.
 *
 * The natural way is `#define MBEDTLS_SHA256_ALT` and lean on pico-sdk's
 * `pico_mbedtls` shim (src/rp2_common/pico_mbedtls/pico_mbedtls.c). That
 * shim implements init/free/starts/update/finish around `pico_sha256_*`
 * but skips `mbedtls_sha256_clone`. mbedTLS's TLS 1.2 code path calls
 * clone in three places (Finished MAC + handshake-hash extraction in
 * ssl_tls.c, plus a MAC computation in ssl_msg.c), so enabling the alt
 * define link-fails on `undefined reference to mbedtls_sha256_clone`.
 *
 * Good news: every TLS call site follows the `clone -> finish -> free`
 * pattern -- the cloned context is never updated again. So we don't
 * need a general-purpose clone; we just need clone-then-finish to work.
 *
 * The hardware exposes:
 *   - sum[0..7]  : read-only intermediate state (after each full block)
 *   - wdata      : write-only data port
 *   - no IV-load : no way to push an intermediate state back into hw
 *
 * That's enough. The plan:
 *   1. Extend the alt context with a 64-byte software-shadow buffer of
 *      the current partial block (mirroring what's been streamed to hw).
 *   2. Replace pico_mbedtls's alt shim with a richer one in umongoc/:
 *      same external API, but the context carries the shadow buffer +
 *      the byte counter + an is_hw_active flag.
 *   3. clone(): wait for hw_ready, snapshot sum[0..7] into the dst's
 *      state[8], copy the partial-block shadow + byte counter, mark
 *      dst hw_inactive.
 *   4. finish() on a hw-inactive ctx: small software SHA-256 finalize
 *      (one or two compressions of the padded final block) starting
 *      from the snapshotted state. ~50 LOC.
 *
 * ~150-200 LOC total + tests. Doable in a follow-up session; the
 * headline speedup on TLS would be a couple hundred ms on Pico 2 W. */

/* Make X.509 chain validation actually check the certificate's notBefore /
 * notAfter dates. Without MBEDTLS_HAVE_TIME_DATE the chain validator skips
 * date checks entirely, so an attacker can re-use an expired-but-otherwise-
 * valid cert that they extracted from a compromised host. We need a working
 * `time()`: we provide mongo_time_seconds() (SNTP-synced wall clock) via
 * MBEDTLS_PLATFORM_TIME_MACRO. Before SNTP completes mongo_time_seconds()
 * returns 0, which would make every cert appear not-yet-valid -- but we don't
 * attempt TLS until after SNTP sync (see app/main.c), so the window is closed.
 */
#define MBEDTLS_HAVE_TIME_DATE
#include "mongo_time.h"
#define MBEDTLS_PLATFORM_TIME_MACRO mongo_time_seconds

#define MBEDTLS_SSL_SERVER_NAME_INDICATION /* SNI is mandatory for Atlas */

/* Hash / MAC / cipher primitives. Keep SHA-1 around for older certs even
 * though Atlas doesn't need it. */
#define MBEDTLS_SHA1_C
#define MBEDTLS_SHA224_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA384_C
#define MBEDTLS_SHA512_C
#define MBEDTLS_MD_C
#define MBEDTLS_MD5_C /* needed by old PKCS#1 v1.5 RSA paths */
#define MBEDTLS_AES_C
#define MBEDTLS_AES_FEWER_TABLES /* save some flash */
#define MBEDTLS_GCM_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_CIPHER_MODE_CBC /* needed by some Atlas cipher fallbacks */

/* Public-key + curves. SECP256R1 is what Atlas + Let's Encrypt use; the
 * 384/521 curves cover broader server compatibility. */
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_ECP_C
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED

/* ASN.1 + X.509 cert parsing. */
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_OID_C
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_PEM_PARSE_C /* CA cert ships as PEM */
#define MBEDTLS_BASE64_C

/* PRNG + entropy. */
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_CTR_DRBG_C

/* TLS 1.2 client only. */
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED

/* PBKDF2 + HKDF for SCRAM-SHA-256 (task 8) -- enable now so the TLS build
 * already has them. */
#define MBEDTLS_PKCS5_C
#define MBEDTLS_HKDF_C

#define MBEDTLS_PLATFORM_C
#define MBEDTLS_ERROR_C
#define MBEDTLS_PLATFORM_MS_TIME_ALT
