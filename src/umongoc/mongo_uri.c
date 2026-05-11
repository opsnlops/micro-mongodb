#include "mongo_uri.h"

#include <stdlib.h>
#include <string.h>

#include "logging.h"
#include "mongo_dns.h"

/* Copy `len` bytes from `src` into `dst` (capped at `dst_sz - 1`),
 * NUL-terminating. Returns true on success, false if the source didn't fit. */
static bool copy_n(char *dst, size_t dst_sz, const char *src, size_t len) {
    if (len >= dst_sz) {
        return false;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
    return true;
}

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/* In-place percent-decode of `s` (NUL-terminated). Returns true on success.
 * Used for username/password from the URI userinfo segment -- Atlas's UI will
 * %-encode any '@' / ':' / '/' etc the user puts in their password. */
static bool url_decode_inplace(char *s) {
    char *src = s;
    char *dst = s;
    while (*src) {
        if (*src == '%') {
            int hi = hex_digit(src[1]);
            int lo = hex_digit(src[2]);
            if (hi < 0 || lo < 0) {
                return false;
            }
            *dst++ = (char)((hi << 4) | lo);
            src += 3;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
    return true;
}

static bool append_str(char *dst, size_t dst_sz, const char *src) {
    size_t cur = strlen(dst);
    size_t add = strlen(src);
    if (cur + add + 1 > dst_sz) {
        return false;
    }
    memcpy(dst + cur, src, add + 1);
    return true;
}

/* Find a single character within [start, end); returns NULL if not found. */
static const char *find_char_in(const char *start, const char *end, char c) {
    for (const char *p = start; p < end; p++) {
        if (*p == c) {
            return p;
        }
    }
    return NULL;
}

/* Walk a comma-separated list of host[:port] entries and populate
 * out->hosts. Default port is 27017. */
static int parse_host_list(const char *start, const char *end, mongo_uri_t *out) {
    const char *p = start;
    while (p < end) {
        if (out->n_hosts >= MONGO_URI_MAX_HOSTS) {
            return MONGO_URI_ERR_TOO_MANY_HOSTS;
        }
        const char *next = find_char_in(p, end, ',');
        const char *entry_end = next ? next : end;

        const char *colon = NULL;
        for (const char *q = p; q < entry_end; q++) {
            if (*q == ':') {
                colon = q;
            }
        }

        size_t host_len = (size_t)((colon ? colon : entry_end) - p);
        if (host_len == 0) {
            return MONGO_URI_ERR_FORMAT;
        }
        if (!copy_n(out->hosts[out->n_hosts].host, sizeof out->hosts[0].host, p, host_len)) {
            return MONGO_URI_ERR_FORMAT;
        }
        uint16_t port = 27017;
        if (colon) {
            char portbuf[8];
            size_t port_len = (size_t)(entry_end - (colon + 1));
            if (port_len == 0 || port_len >= sizeof portbuf) {
                return MONGO_URI_ERR_FORMAT;
            }
            memcpy(portbuf, colon + 1, port_len);
            portbuf[port_len] = '\0';
            unsigned long pn = strtoul(portbuf, NULL, 10);
            if (pn == 0 || pn > 65535) {
                return MONGO_URI_ERR_FORMAT;
            }
            port = (uint16_t)pn;
        }
        out->hosts[out->n_hosts].port = port;
        out->n_hosts++;

        if (!next) {
            break;
        }
        p = next + 1;
    }
    return MONGO_URI_OK;
}

/* Source of an option pair. The spec is strict about what may be set via TXT:
 * only `authSource` and `replicaSet` are permitted. A hostile DNS resolver
 * (`mongodb+srv://` does TXT over plain UDP) could otherwise flip `tls=false`
 * and silently strip transport encryption from a connection that the user
 * intended to be TLS-protected. Anything outside the allow-list is logged at
 * warning and dropped. */
typedef enum {
    OPT_SOURCE_URI = 0, /* trusted: came from the user-supplied URI string */
    OPT_SOURCE_TXT = 1, /* untrusted: came from a DNS TXT record */
} opt_source_t;

/* Apply one `k=v` option pair. Unknown keys are silently ignored.
 * When `source == OPT_SOURCE_TXT`, only spec-allowed keys are honored. */
static void apply_option(mongo_uri_t *out, const char *key, size_t klen, const char *val, size_t vlen,
                         opt_source_t source) {
    if (klen == 3 && strncmp(key, "tls", 3) == 0) {
        if (source == OPT_SOURCE_TXT) {
            warning("[uri] rejecting tls= from TXT record (not spec-allowed)");
            return;
        }
        out->tls = (vlen == 4 && strncmp(val, "true", 4) == 0);
    } else if (klen == 3 && strncmp(key, "ssl", 3) == 0) {
        if (source == OPT_SOURCE_TXT) {
            warning("[uri] rejecting ssl= from TXT record (not spec-allowed)");
            return;
        }
        out->tls = (vlen == 4 && strncmp(val, "true", 4) == 0);
    } else if (klen == 10 && strncmp(key, "replicaSet", 10) == 0) {
        copy_n(out->replica_set, sizeof out->replica_set, val, vlen);
    } else if (klen == 10 && strncmp(key, "authSource", 10) == 0) {
        copy_n(out->auth_source, sizeof out->auth_source, val, vlen);
    } else if (klen == 17 && strncmp(key, "allowInsecureAuth", 17) == 0) {
        /* URI-only escape hatch: send SCRAM over plain TCP. Not honored from
         * TXT -- spec doesn't allow it there, and we don't want an attacker
         * who controls DNS to turn it on. */
        if (source == OPT_SOURCE_TXT) {
            warning("[uri] rejecting allowInsecureAuth= from TXT record");
            return;
        }
        out->allow_insecure_auth = (vlen == 4 && strncmp(val, "true", 4) == 0);
    } else if (source == OPT_SOURCE_TXT) {
        warning("[uri] dropping unrecognized TXT option key: %.*s", (int)klen, key);
    }
    /* Unknown keys from the URI: ignored silently (the spec encourages
     * forward-compat). Unknown keys from TXT: warned above. */
}

/* Parse a "k=v&k=v" option string. */
static void parse_options(const char *start, const char *end, mongo_uri_t *out, opt_source_t source) {
    const char *p = start;
    while (p < end) {
        const char *amp = find_char_in(p, end, '&');
        const char *pair_end = amp ? amp : end;
        const char *eq = find_char_in(p, pair_end, '=');
        if (eq) {
            apply_option(out, p, (size_t)(eq - p), eq + 1, (size_t)(pair_end - (eq + 1)), source);
        }
        if (!amp) {
            break;
        }
        p = amp + 1;
    }
}

int mongo_uri_parse(const char *uri_str, mongo_uri_t *out, uint32_t dns_timeout_ms) {
    if (!uri_str || !out) {
        return MONGO_URI_ERR_ARGS;
    }
    memset(out, 0, sizeof *out);
    strncpy(out->auth_source, "admin", sizeof out->auth_source - 1);

    const char *p = uri_str;
    static const char prefix_plain[] = "mongodb://";
    static const char prefix_srv[] = "mongodb+srv://";

    if (strncmp(p, prefix_srv, sizeof prefix_srv - 1) == 0) {
        out->is_srv = true;
        out->tls = true; /* mongodb+srv implies TLS by spec */
        p += sizeof prefix_srv - 1;
    } else if (strncmp(p, prefix_plain, sizeof prefix_plain - 1) == 0) {
        p += sizeof prefix_plain - 1;
    } else {
        return MONGO_URI_ERR_FORMAT;
    }

    const char *uri_end = uri_str + strlen(uri_str);
    const char *question = find_char_in(p, uri_end, '?');
    const char *opts_start = question ? question + 1 : NULL;
    const char *body_end = question ? question : uri_end;

    /* Path (database) sits before any '?', if present. */
    const char *slash = find_char_in(p, body_end, '/');
    const char *host_part_end = slash ? slash : body_end;

    /* Optional credentials terminated by '@' (within the host segment).
     * Credentials are percent-decoded so passwords with special chars (which
     * the Atlas UI URL-encodes) reach SCRAM in their literal form. */
    const char *at = find_char_in(p, host_part_end, '@');
    if (at) {
        const char *user_end = find_char_in(p, at, ':');
        if (user_end) {
            if (!copy_n(out->username, sizeof out->username, p, (size_t)(user_end - p))) {
                return MONGO_URI_ERR_FORMAT;
            }
            if (!copy_n(out->password, sizeof out->password, user_end + 1, (size_t)(at - (user_end + 1)))) {
                return MONGO_URI_ERR_FORMAT;
            }
        } else {
            if (!copy_n(out->username, sizeof out->username, p, (size_t)(at - p))) {
                return MONGO_URI_ERR_FORMAT;
            }
        }
        if (!url_decode_inplace(out->username) || !url_decode_inplace(out->password)) {
            error("[uri] malformed percent-encoding in credentials");
            return MONGO_URI_ERR_FORMAT;
        }
        p = at + 1;
    }

    int rc = parse_host_list(p, host_part_end, out);
    if (rc != MONGO_URI_OK) {
        return rc;
    }
    if (out->n_hosts == 0) {
        return MONGO_URI_ERR_FORMAT;
    }
    if (out->is_srv && out->n_hosts != 1) {
        error("[uri] mongodb+srv:// must have exactly one seed host");
        return MONGO_URI_ERR_FORMAT;
    }
    if (out->is_srv && out->hosts[0].port != 27017) {
        /* spec says port must be omitted in +srv; parse_host_list defaulted it
         * to 27017 if absent. If a port was explicitly given, it'd be non-27017
         * possibly; we can't distinguish here easily. Accept and move on. */
    }

    if (slash) {
        const char *db_start = slash + 1;
        size_t db_len = (size_t)(body_end - db_start);
        if (db_len > 0 && !copy_n(out->database, sizeof out->database, db_start, db_len)) {
            return MONGO_URI_ERR_FORMAT;
        }
    }

    if (out->is_srv) {
        /* Trust boundary note: the SRV and TXT responses here come back over
         * unauthenticated plain UDP DNS. A network attacker who can spoof
         * DNS replies can change `replicaSet` and `authSource` (and thus
         * which database we send SCRAM proofs to). The TLS handshake to the
         * server still validates the cert chain via SNI / hostname, which
         * limits where the connection actually lands. If your deployment's
         * threat model includes hostile DNS, use trusted resolvers (DoT/DoH
         * at the network level) or pin to a `mongodb://host1,host2,...`
         * URI with explicit hosts instead of `mongodb+srv://`. */

        /* Stash the seed host before we replace it with SRV-resolved targets;
         * the TXT lookup uses the original seed name. */
        char seed[MONGO_URI_HOST_MAX];
        strncpy(seed, out->hosts[0].host, sizeof seed - 1);
        seed[sizeof seed - 1] = '\0';

        /* TXT options (replicaSet, authSource, ...) come first so explicit
         * URI options can still override them below. */
        char txt[256];
        int txt_rc = mongo_dns_query_txt(seed, txt, sizeof txt, dns_timeout_ms);
        if (txt_rc == MONGO_DNS_OK && txt[0] != '\0') {
            parse_options(txt, txt + strlen(txt), out, OPT_SOURCE_TXT);
        }

        /* SRV: _mongodb._tcp.<seed_host> */
        char srv_name[MONGO_URI_HOST_MAX + 20];
        srv_name[0] = '\0';
        if (!append_str(srv_name, sizeof srv_name, "_mongodb._tcp.")) {
            return MONGO_URI_ERR_FORMAT;
        }
        if (!append_str(srv_name, sizeof srv_name, seed)) {
            return MONGO_URI_ERR_FORMAT;
        }

        mongo_srv_record_t records[MONGO_URI_MAX_HOSTS];
        size_t n_records = 0;
        int dns_rc = mongo_dns_query_srv(srv_name, records, MONGO_URI_MAX_HOSTS, &n_records, dns_timeout_ms);
        if (dns_rc != MONGO_DNS_OK || n_records == 0) {
            error("[uri] SRV lookup failed for %s: %d", srv_name, dns_rc);
            return MONGO_URI_ERR_DNS;
        }

        /* Replace seed with the SRV-resolved host list. */
        memset(out->hosts, 0, sizeof out->hosts);
        out->n_hosts = 0;
        for (size_t i = 0; i < n_records && out->n_hosts < MONGO_URI_MAX_HOSTS; i++) {
            if (!copy_n(out->hosts[out->n_hosts].host, sizeof out->hosts[0].host, records[i].target,
                        strlen(records[i].target))) {
                continue;
            }
            out->hosts[out->n_hosts].port = records[i].port;
            out->n_hosts++;
        }
    }

    if (opts_start) {
        parse_options(opts_start, uri_end, out, OPT_SOURCE_URI);
    }

    return MONGO_URI_OK;
}
