/*
 * mongo_ca — embedded root CA bundle for TLS verification.
 *
 * Default: ISRG Root X1, the Let's Encrypt root. This is what every Atlas
 * cluster presents in its chain at time of writing. If you're connecting
 * to a private mongod with its own CA, pass that CA's PEM via
 * mongo_tls_config_t.ca_pem instead of using the default.
 */

#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

extern const char mongo_ca_default_pem[];
extern const size_t mongo_ca_default_pem_len;

#ifdef __cplusplus
}
#endif
