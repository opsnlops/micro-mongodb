#include "mongo_dns.h"

#include <ctype.h>
#include <string.h>

#include "pico/cyw43_arch.h"
#include "pico/rand.h"

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#include "lwip/dns.h"
#include "lwip/err.h"
#include "lwip/ip_addr.h"
#include "lwip/pbuf.h"
#include "lwip/udp.h"

#include "logging.h"

#define DNS_PORT 53
#define DNS_TYPE_TXT 16
#define DNS_TYPE_SRV 33
#define DNS_CLASS_IN 1
#define DNS_FLAGS_RD 0x0100         /* recursion desired */
#define DNS_FLAGS_QR 0x8000         /* response bit */
#define DNS_FLAGS_RCODE_MASK 0x000f /* low nibble of flags */
#define DNS_MAX_PACKET 1024         /* well over typical reply size */

/* Encode a textual name like "_mongodb._tcp.foo.bar" into RFC 1035 wire form:
 * <len><label><len><label>...<0>. Returns the number of bytes written, or
 * -1 on overflow / malformed input. */
static int encode_name(const char *name, uint8_t *out, size_t out_cap) {
    size_t name_len = strlen(name);
    size_t pos = 0;
    size_t i = 0;
    while (i < name_len) {
        /* find end of this label */
        size_t j = i;
        while (j < name_len && name[j] != '.') {
            j++;
        }
        size_t label_len = j - i;
        if (label_len == 0 || label_len > 63) {
            return -1;
        }
        if (pos + 1 + label_len + 1 > out_cap) {
            return -1;
        }
        out[pos++] = (uint8_t)label_len;
        memcpy(out + pos, name + i, label_len);
        pos += label_len;
        i = j + 1;
        if (j == name_len) {
            i = name_len; /* no trailing dot */
            break;
        }
    }
    out[pos++] = 0; /* terminating null label */
    return (int)pos;
}

/* Decode a possibly-compressed name from a DNS message into a NUL-terminated
 * dotted string. Returns the number of bytes consumed at the starting offset,
 * or -1 on malformed input. */
static int decode_name(const uint8_t *msg, size_t msg_len, size_t start, char *out, size_t out_cap) {
    size_t pos = start;
    size_t out_pos = 0;
    bool jumped = false;
    size_t bytes_consumed_at_start = 0;
    size_t safety = 0;

    while (pos < msg_len) {
        if (++safety > 256) {
            return -1; /* loop guard */
        }
        uint8_t b = msg[pos];
        if ((b & 0xc0) == 0xc0) {
            /* pointer: high two bits set, next 14 bits are an offset */
            if (pos + 1 >= msg_len) {
                return -1;
            }
            uint16_t off = (uint16_t)(((b & 0x3f) << 8) | msg[pos + 1]);
            if (!jumped) {
                bytes_consumed_at_start = (pos + 2) - start;
                jumped = true;
            }
            pos = off;
            continue;
        }
        if (b == 0) {
            pos += 1;
            if (!jumped) {
                bytes_consumed_at_start = pos - start;
            }
            if (out_cap > 0) {
                out[out_pos] = '\0';
            }
            return (int)bytes_consumed_at_start;
        }
        if ((b & 0xc0) != 0) {
            /* reserved label types */
            return -1;
        }
        if (out_pos > 0) {
            if (out_pos + 1 >= out_cap) {
                return -1;
            }
            out[out_pos++] = '.';
        }
        if (pos + 1 + b > msg_len) {
            return -1;
        }
        if (out_pos + b >= out_cap) {
            return -1;
        }
        memcpy(out + out_pos, msg + pos + 1, b);
        out_pos += b;
        pos += 1 + b;
    }
    return -1;
}

/* Holder for the udp_recv callback signal + reply buffer. */
typedef struct {
    SemaphoreHandle_t sem;
    uint8_t reply[DNS_MAX_PACKET];
    size_t reply_len;
    uint16_t expect_id;
    bool ok;
} dns_recv_ctx_t;

static void udp_recv_cb(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port) {
    (void)pcb;
    (void)addr;
    (void)port;
    dns_recv_ctx_t *ctx = arg;
    if (!ctx || !p) {
        if (p) {
            pbuf_free(p);
        }
        return;
    }
    if (p->tot_len < 12 || p->tot_len > DNS_MAX_PACKET) {
        pbuf_free(p);
        return;
    }
    /* Snapshot tot_len before freeing the pbuf -- accessing p->tot_len
     * after pbuf_free is a use-after-free. */
    const u16_t pkt_len = p->tot_len;
    u16_t copied = pbuf_copy_partial(p, ctx->reply, pkt_len, 0);
    pbuf_free(p);
    if (copied != pkt_len) {
        return;
    }
    /* Only commit the reply (and signal the waiter) once we know the packet
     * is the one we asked for. A second UDP packet that arrived between
     * udp_recv_cb firing and udp_remove() must not clobber ctx->reply. */
    uint16_t resp_id = (uint16_t)((ctx->reply[0] << 8) | ctx->reply[1]);
    if (resp_id != ctx->expect_id) {
        return; /* not ours -- keep waiting */
    }
    ctx->reply_len = copied;
    ctx->ok = true;
    xSemaphoreGive(ctx->sem);
}

/* Build, send, and receive a DNS query of `qtype`. On success, the raw reply
 * is in ctx->reply and ctx->reply_len is set. */
static int dns_round_trip(const char *name, uint16_t qtype, dns_recv_ctx_t *ctx, uint32_t timeout_ms) {
    const ip_addr_t *server = dns_getserver(0);
    if (!server || ip_addr_isany(server)) {
        error("[dns] no nameserver from DHCP");
        return MONGO_DNS_ERR_NO_NAMESERVER;
    }

    /* Build the query. */
    uint8_t query[DNS_MAX_PACKET];
    /* Random transaction ID via the RP2x40/RP2350 ring-oscillator RNG.
     * A monotonic counter is trivially guessable, which would let an off-path
     * attacker race the legitimate response with a forged one (e.g. spoofed
     * SRV record). The 16-bit ID isn't a strong defense on its own --
     * cache-poisoning research shows ~1-in-65k birthday-style guesses succeed
     * inside the timeout window -- but it raises the bar from "always" to
     * "lottery" and combines with question-name verification below. */
    uint16_t id = (uint16_t)get_rand_32();
    if (id == 0) {
        id = 1;
    }
    ctx->expect_id = id;
    query[0] = (uint8_t)(id >> 8);
    query[1] = (uint8_t)(id & 0xff);
    query[2] = (uint8_t)(DNS_FLAGS_RD >> 8);
    query[3] = (uint8_t)(DNS_FLAGS_RD & 0xff);
    query[4] = 0;
    query[5] = 1; /* QDCOUNT = 1 */
    query[6] = 0;
    query[7] = 0; /* ANCOUNT = 0 */
    query[8] = 0;
    query[9] = 0; /* NSCOUNT = 0 */
    query[10] = 0;
    query[11] = 0; /* ARCOUNT = 0 */

    int name_bytes = encode_name(name, query + 12, sizeof query - 12 - 4);
    if (name_bytes < 0) {
        return MONGO_DNS_ERR_ARGS;
    }
    size_t qpos = 12 + (size_t)name_bytes;
    query[qpos++] = (uint8_t)(qtype >> 8);
    query[qpos++] = (uint8_t)(qtype & 0xff);
    query[qpos++] = (uint8_t)(DNS_CLASS_IN >> 8);
    query[qpos++] = (uint8_t)(DNS_CLASS_IN & 0xff);

    /* Open a UDP pcb bound to an ephemeral port, register the recv callback,
     * and send. */
    cyw43_arch_lwip_begin();
    struct udp_pcb *pcb = udp_new();
    if (!pcb) {
        cyw43_arch_lwip_end();
        return MONGO_DNS_ERR_ALLOC;
    }
    err_t err = udp_bind(pcb, IP_ANY_TYPE, 0);
    if (err != ERR_OK) {
        udp_remove(pcb);
        cyw43_arch_lwip_end();
        return MONGO_DNS_ERR_ALLOC;
    }
    udp_recv(pcb, udp_recv_cb, ctx);

    struct pbuf *out_p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)qpos, PBUF_RAM);
    if (!out_p) {
        udp_remove(pcb);
        cyw43_arch_lwip_end();
        return MONGO_DNS_ERR_ALLOC;
    }
    memcpy(out_p->payload, query, qpos);
    err = udp_sendto(pcb, out_p, server, DNS_PORT);
    pbuf_free(out_p);
    cyw43_arch_lwip_end();

    if (err != ERR_OK) {
        cyw43_arch_lwip_begin();
        udp_remove(pcb);
        cyw43_arch_lwip_end();
        error("[dns] udp_sendto: err=%d", (int)err);
        return MONGO_DNS_ERR_SEND;
    }

    BaseType_t took = xSemaphoreTake(ctx->sem, pdMS_TO_TICKS(timeout_ms));

    /* Tear the pcb down BEFORE the caller observes ctx->reply. Once the
     * callback is detached, no further packet can clobber ctx->reply or
     * ctx->reply_len -- which a second matching-id packet (legitimate
     * retry or malicious replay) could otherwise do in the tiny window
     * between our sem-take and a later udp_remove. */
    cyw43_arch_lwip_begin();
    udp_remove(pcb);
    cyw43_arch_lwip_end();

    int rc;
    if (took != pdTRUE) {
        rc = MONGO_DNS_ERR_TIMEOUT;
    } else if (!ctx->ok) {
        rc = MONGO_DNS_ERR_FORMAT;
    } else {
        rc = MONGO_DNS_OK;
    }
    return rc;
}

/* RFC 1035 §3.1: DNS name comparison is case-insensitive across ASCII. */
static bool dns_name_eq_ci(const char *a, const char *b) {
    for (;; a++, b++) {
        int ca = (unsigned char)*a;
        int cb = (unsigned char)*b;
        if (ca >= 'A' && ca <= 'Z') {
            ca += 'a' - 'A';
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb += 'a' - 'A';
        }
        if (ca != cb) {
            return false;
        }
        if (ca == '\0') {
            return true;
        }
    }
}

/* Walk past the question section (one question; should be the same name we
 * sent). Verifies the qname matches `expect_name` -- without that check, an
 * off-path attacker could deliver a reply that won the tx-id lottery for some
 * other in-flight query and we'd happily parse its answers as ours. */
static int skip_question(const uint8_t *msg, size_t msg_len, size_t pos, const char *expect_name, size_t *new_pos) {
    char tmp[256];
    int consumed = decode_name(msg, msg_len, pos, tmp, sizeof tmp);
    if (consumed < 0) {
        return MONGO_DNS_ERR_FORMAT;
    }
    if (expect_name && !dns_name_eq_ci(tmp, expect_name)) {
        error("[dns] qname mismatch: got '%s' expected '%s'", tmp, expect_name);
        return MONGO_DNS_ERR_FORMAT;
    }
    pos += (size_t)consumed;
    if (pos + 4 > msg_len) {
        return MONGO_DNS_ERR_FORMAT;
    }
    pos += 4; /* qtype + qclass */
    *new_pos = pos;
    return MONGO_DNS_OK;
}

static int check_reply_header(const uint8_t *msg, size_t msg_len, uint16_t *ancount_out) {
    if (msg_len < 12) {
        return MONGO_DNS_ERR_FORMAT;
    }
    uint16_t flags = (uint16_t)((msg[2] << 8) | msg[3]);
    if (!(flags & DNS_FLAGS_QR)) {
        return MONGO_DNS_ERR_FORMAT;
    }
    uint16_t rcode = flags & DNS_FLAGS_RCODE_MASK;
    if (rcode != 0) {
        error("[dns] rcode=%u", (unsigned)rcode);
        return MONGO_DNS_ERR_RCODE;
    }
    uint16_t qdcount = (uint16_t)((msg[4] << 8) | msg[5]);
    if (qdcount != 1) {
        return MONGO_DNS_ERR_FORMAT;
    }
    *ancount_out = (uint16_t)((msg[6] << 8) | msg[7]);
    return MONGO_DNS_OK;
}

int mongo_dns_query_srv(const char *name, mongo_srv_record_t *out, size_t out_cap, size_t *n_out, uint32_t timeout_ms) {
    if (!name || !out || !n_out || out_cap == 0) {
        return MONGO_DNS_ERR_ARGS;
    }
    *n_out = 0;

    dns_recv_ctx_t *ctx = pvPortMalloc(sizeof *ctx);
    if (!ctx) {
        return MONGO_DNS_ERR_ALLOC;
    }
    memset(ctx, 0, sizeof *ctx);
    ctx->sem = xSemaphoreCreateBinary();
    if (!ctx->sem) {
        vPortFree(ctx);
        return MONGO_DNS_ERR_ALLOC;
    }

    int rc = dns_round_trip(name, DNS_TYPE_SRV, ctx, timeout_ms);
    if (rc != MONGO_DNS_OK) {
        vSemaphoreDelete(ctx->sem);
        vPortFree(ctx);
        return rc;
    }

    uint16_t ancount = 0;
    rc = check_reply_header(ctx->reply, ctx->reply_len, &ancount);
    if (rc != MONGO_DNS_OK) {
        vSemaphoreDelete(ctx->sem);
        vPortFree(ctx);
        return rc;
    }

    size_t pos = 12;
    rc = skip_question(ctx->reply, ctx->reply_len, pos, name, &pos);
    if (rc != MONGO_DNS_OK) {
        vSemaphoreDelete(ctx->sem);
        vPortFree(ctx);
        return rc;
    }

    size_t emitted = 0;
    for (uint16_t i = 0; i < ancount && emitted < out_cap; i++) {
        char rr_name[256];
        int consumed = decode_name(ctx->reply, ctx->reply_len, pos, rr_name, sizeof rr_name);
        if (consumed < 0) {
            break;
        }
        pos += (size_t)consumed;
        if (pos + 10 > ctx->reply_len) {
            break;
        }
        uint16_t type = (uint16_t)((ctx->reply[pos] << 8) | ctx->reply[pos + 1]);
        uint16_t rdlength = (uint16_t)((ctx->reply[pos + 8] << 8) | ctx->reply[pos + 9]);
        pos += 10;
        if (pos + rdlength > ctx->reply_len) {
            break;
        }
        if (type == DNS_TYPE_SRV && rdlength >= 7) {
            mongo_srv_record_t *rec = &out[emitted];
            rec->priority = (uint16_t)((ctx->reply[pos] << 8) | ctx->reply[pos + 1]);
            rec->weight = (uint16_t)((ctx->reply[pos + 2] << 8) | ctx->reply[pos + 3]);
            rec->port = (uint16_t)((ctx->reply[pos + 4] << 8) | ctx->reply[pos + 5]);
            if (decode_name(ctx->reply, ctx->reply_len, pos + 6, rec->target, sizeof rec->target) < 0) {
                pos += rdlength;
                continue;
            }
            emitted++;
        }
        pos += rdlength;
    }

    vSemaphoreDelete(ctx->sem);
    vPortFree(ctx);

    if (emitted == 0) {
        return MONGO_DNS_ERR_NO_RECORDS;
    }
    *n_out = emitted;
    return MONGO_DNS_OK;
}

int mongo_dns_query_txt(const char *name, char *out, size_t out_cap, uint32_t timeout_ms) {
    if (!name || !out || out_cap == 0) {
        return MONGO_DNS_ERR_ARGS;
    }
    out[0] = '\0';

    dns_recv_ctx_t *ctx = pvPortMalloc(sizeof *ctx);
    if (!ctx) {
        return MONGO_DNS_ERR_ALLOC;
    }
    memset(ctx, 0, sizeof *ctx);
    ctx->sem = xSemaphoreCreateBinary();
    if (!ctx->sem) {
        vPortFree(ctx);
        return MONGO_DNS_ERR_ALLOC;
    }

    int rc = dns_round_trip(name, DNS_TYPE_TXT, ctx, timeout_ms);
    if (rc != MONGO_DNS_OK) {
        vSemaphoreDelete(ctx->sem);
        vPortFree(ctx);
        return rc;
    }

    uint16_t ancount = 0;
    rc = check_reply_header(ctx->reply, ctx->reply_len, &ancount);
    if (rc != MONGO_DNS_OK) {
        vSemaphoreDelete(ctx->sem);
        vPortFree(ctx);
        return rc;
    }

    size_t pos = 12;
    rc = skip_question(ctx->reply, ctx->reply_len, pos, name, &pos);
    if (rc != MONGO_DNS_OK) {
        vSemaphoreDelete(ctx->sem);
        vPortFree(ctx);
        return rc;
    }

    bool got_one = false;
    for (uint16_t i = 0; i < ancount && !got_one; i++) {
        char rr_name[256];
        int consumed = decode_name(ctx->reply, ctx->reply_len, pos, rr_name, sizeof rr_name);
        if (consumed < 0) {
            break;
        }
        pos += (size_t)consumed;
        if (pos + 10 > ctx->reply_len) {
            break;
        }
        uint16_t type = (uint16_t)((ctx->reply[pos] << 8) | ctx->reply[pos + 1]);
        uint16_t rdlength = (uint16_t)((ctx->reply[pos + 8] << 8) | ctx->reply[pos + 9]);
        pos += 10;
        if (pos + rdlength > ctx->reply_len) {
            break;
        }
        if (type == DNS_TYPE_TXT && rdlength > 0) {
            size_t rdata_end = pos + rdlength;
            size_t scan = pos;
            size_t out_pos = 0;
            while (scan < rdata_end && out_pos + 1 < out_cap) {
                uint8_t seg_len = ctx->reply[scan++];
                if (scan + seg_len > rdata_end) {
                    break;
                }
                size_t to_copy = seg_len;
                if (out_pos + to_copy >= out_cap) {
                    to_copy = out_cap - 1 - out_pos;
                }
                memcpy(out + out_pos, ctx->reply + scan, to_copy);
                out_pos += to_copy;
                scan += seg_len;
            }
            out[out_pos] = '\0';
            got_one = true;
        }
        pos += rdlength;
    }

    vSemaphoreDelete(ctx->sem);
    vPortFree(ctx);

    if (!got_one) {
        return MONGO_DNS_ERR_NO_RECORDS;
    }
    return MONGO_DNS_OK;
}
