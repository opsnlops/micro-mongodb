#include "mongo_transport.h"

#include <string.h>

#include "pico/cyw43_arch.h"

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#include "lwip/altcp.h"
#include "lwip/altcp_tcp.h"
#include "lwip/altcp_tls.h"
#include "lwip/dns.h"
#include "lwip/err.h"
#include "lwip/ip_addr.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"

#include "mbedtls/ssl.h"

#include "logging.h"
#include "mongo_ca.h"

/* Drain loop polls the rx pbuf chain. The semaphore wakes the caller when
 * a recv callback fires OR when an error/close signal arrives. */
typedef enum {
    XPORT_IDLE = 0,
    XPORT_CONNECTING,
    XPORT_CONNECTED,
    XPORT_CLOSED,
    XPORT_ERROR,
} xport_state_t;

struct mongo_transport {
    struct altcp_pcb *pcb;
    SemaphoreHandle_t connect_sem;
    SemaphoreHandle_t recv_sem;

    volatile xport_state_t state;
    volatile err_t last_err;

    struct pbuf *rx_head; /* head of inbound pbuf chain */
    uint16_t rx_offset;   /* bytes already drained from rx_head */

    /* TLS config -- NULL means plain TCP. The altcp_tls_config is owned by
     * lwIP/mbedTLS and freed via altcp_tls_free_config in transport_free. */
    struct altcp_tls_config *tls_config;
    /* 256 bytes matches MONGO_URI_HOST_MAX. Atlas shard hostnames can run
     * 50+ chars (`<shard>.<cluster>.<id>.mongodb.net`) and we still need
     * headroom for any future longer-form hostnames. */
    char sni_hostname[256];
    bool tls_enabled;
};

/* ---------------- lwIP callbacks (tcpip thread context) ---------------- */

/* These callbacks fire on the lwIP tcpip thread, which is a regular FreeRTOS
 * task -- NOT an ISR. So use the plain xSemaphoreGive (not _FromISR). The
 * FromISR variants worked accidentally on this build but break on SMP and
 * tickless-idle configurations. */

static err_t cb_connected(void *arg, struct altcp_pcb *pcb, err_t err) {
    (void)pcb;
    mongo_transport_t *t = arg;
    if (err != ERR_OK) {
        t->state = XPORT_ERROR;
        t->last_err = err;
    } else {
        t->state = XPORT_CONNECTED;
    }
    xSemaphoreGive(t->connect_sem);
    return ERR_OK;
}

static err_t cb_recv(void *arg, struct altcp_pcb *pcb, struct pbuf *p, err_t err) {
    (void)pcb;
    mongo_transport_t *t = arg;
    if (err != ERR_OK) {
        t->state = XPORT_ERROR;
        t->last_err = err;
        if (p) {
            pbuf_free(p);
        }
    } else if (p == NULL) {
        /* Peer FIN. */
        t->state = XPORT_CLOSED;
    } else {
        if (t->rx_head == NULL) {
            t->rx_head = p;
            t->rx_offset = 0;
        } else {
            pbuf_cat(t->rx_head, p);
        }
    }
    xSemaphoreGive(t->recv_sem);
    return ERR_OK;
}

static void cb_err(void *arg, err_t err) {
    mongo_transport_t *t = arg;
    if (!t) {
        return;
    }
    t->state = XPORT_ERROR;
    t->last_err = err;
    /* lwIP has already deallocated the pcb when err callback fires. */
    t->pcb = NULL;
    xSemaphoreGive(t->connect_sem);
    xSemaphoreGive(t->recv_sem);
}

/* DNS resolution helper. lwIP's resolver is callback-style; we synthesize a
 * blocking call by signalling a private semaphore. */
typedef struct {
    SemaphoreHandle_t sem;
    ip_addr_t addr;
    bool ok;
} dns_ctx_t;

static void cb_dns(const char *name, const ip_addr_t *addr, void *arg) {
    (void)name;
    dns_ctx_t *ctx = arg;
    if (addr) {
        ctx->addr = *addr;
        ctx->ok = true;
    } else {
        ctx->ok = false;
    }
    /* Fires on tcpip_thread (task), not from an ISR. */
    xSemaphoreGive(ctx->sem);
}

static int resolve_host(const char *host, ip_addr_t *out, uint32_t timeout_ms) {
    /* Dotted-quad shortcut. */
    if (ip4addr_aton(host, ip_2_ip4(out))) {
        IP_SET_TYPE_VAL(*out, IPADDR_TYPE_V4);
        return MONGO_TRANSPORT_OK;
    }

    dns_ctx_t ctx = {0};
    ctx.sem = xSemaphoreCreateBinary();
    if (!ctx.sem) {
        return MONGO_TRANSPORT_ERR_ALLOC;
    }

    cyw43_arch_lwip_begin();
    err_t err = dns_gethostbyname(host, &ctx.addr, cb_dns, &ctx);
    cyw43_arch_lwip_end();

    if (err == ERR_OK) {
        /* Cached -- ctx.addr already populated. */
        *out = ctx.addr;
        vSemaphoreDelete(ctx.sem);
        return MONGO_TRANSPORT_OK;
    }
    if (err != ERR_INPROGRESS) {
        vSemaphoreDelete(ctx.sem);
        return MONGO_TRANSPORT_ERR_DNS;
    }
    if (xSemaphoreTake(ctx.sem, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        vSemaphoreDelete(ctx.sem);
        return MONGO_TRANSPORT_ERR_TIMEOUT;
    }
    bool ok = ctx.ok;
    *out = ctx.addr;
    vSemaphoreDelete(ctx.sem);
    return ok ? MONGO_TRANSPORT_OK : MONGO_TRANSPORT_ERR_DNS;
}

/* ---------------- public API ---------------- */

mongo_transport_t *mongo_transport_new(const mongo_tls_config_t *tls) {
    mongo_transport_t *t = pvPortMalloc(sizeof *t);
    if (!t) {
        return NULL;
    }
    memset(t, 0, sizeof *t);
    t->state = XPORT_IDLE;
    t->connect_sem = xSemaphoreCreateBinary();
    t->recv_sem = xSemaphoreCreateBinary();
    if (!t->connect_sem || !t->recv_sem) {
        if (t->connect_sem) {
            vSemaphoreDelete(t->connect_sem);
        }
        if (t->recv_sem) {
            vSemaphoreDelete(t->recv_sem);
        }
        vPortFree(t);
        return NULL;
    }

    if (tls) {
        const uint8_t *ca = (const uint8_t *)(tls->ca_pem ? tls->ca_pem : mongo_ca_default_pem);
        size_t ca_len = tls->ca_pem ? tls->ca_pem_len : mongo_ca_default_pem_len;

        cyw43_arch_lwip_begin();
        t->tls_config = altcp_tls_create_config_client(ca, ca_len);
        cyw43_arch_lwip_end();

        if (!t->tls_config) {
            error("[xport] altcp_tls_create_config_client failed (CA parse?)");
            mongo_transport_free(t);
            return NULL;
        }
        if (tls->sni_hostname) {
            strncpy(t->sni_hostname, tls->sni_hostname, sizeof t->sni_hostname - 1);
        }
        t->tls_enabled = true;
    }
    return t;
}

void mongo_transport_free(mongo_transport_t *t) {
    if (!t) {
        return;
    }
    mongo_transport_close(t);
    if (t->tls_config) {
        cyw43_arch_lwip_begin();
        altcp_tls_free_config(t->tls_config);
        cyw43_arch_lwip_end();
        t->tls_config = NULL;
    }
    if (t->connect_sem) {
        vSemaphoreDelete(t->connect_sem);
    }
    if (t->recv_sem) {
        vSemaphoreDelete(t->recv_sem);
    }
    vPortFree(t);
}

bool mongo_transport_is_connected(const mongo_transport_t *t) { return t && t->state == XPORT_CONNECTED; }

int mongo_transport_last_err(const mongo_transport_t *t) { return t ? (int)t->last_err : 0; }

int mongo_transport_connect(mongo_transport_t *t, const char *host, uint16_t port, uint32_t timeout_ms) {
    if (!t || !host) {
        return MONGO_TRANSPORT_ERR_ARGS;
    }
    if (t->state != XPORT_IDLE && t->state != XPORT_CLOSED) {
        return MONGO_TRANSPORT_ERR_STATE;
    }

    /* Drain stale semaphore counts from any previous use. */
    xSemaphoreTake(t->connect_sem, 0);
    xSemaphoreTake(t->recv_sem, 0);

    ip_addr_t addr;
    int rc = resolve_host(host, &addr, timeout_ms);
    if (rc != MONGO_TRANSPORT_OK) {
        error("[xport] DNS failed for %s: rc=%d", host, rc);
        return rc;
    }

    cyw43_arch_lwip_begin();
    if (t->tls_enabled) {
        t->pcb = altcp_tls_new(t->tls_config, IPADDR_TYPE_V4);
    } else {
        t->pcb = altcp_tcp_new();
    }
    if (t->pcb) {
        altcp_arg(t->pcb, t);
        altcp_err(t->pcb, cb_err);
        altcp_recv(t->pcb, cb_recv);
        altcp_nagle_disable(t->pcb);

        /* SNI: required for Atlas (its load balancer routes by SNI). For our
         * own deployments it's harmless. */
        if (t->tls_enabled && t->sni_hostname[0]) {
            mbedtls_ssl_context *ssl = (mbedtls_ssl_context *)altcp_tls_context(t->pcb);
            if (ssl) {
                int sni_rc = mbedtls_ssl_set_hostname(ssl, t->sni_hostname);
                if (sni_rc != 0) {
                    error("[xport] mbedtls_ssl_set_hostname: %d", sni_rc);
                }
            }
        }
    }
    t->state = XPORT_CONNECTING;
    err_t err = ERR_MEM;
    if (t->pcb) {
        err = altcp_connect(t->pcb, &addr, port, cb_connected);
    }
    cyw43_arch_lwip_end();

    if (!t->pcb) {
        t->state = XPORT_ERROR;
        return MONGO_TRANSPORT_ERR_ALLOC;
    }
    if (err != ERR_OK) {
        error("[xport] altcp_connect: err=%d", (int)err);
        t->last_err = err;
        mongo_transport_close(t);
        return MONGO_TRANSPORT_ERR_CONNECT;
    }

    if (xSemaphoreTake(t->connect_sem, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        error("[xport] connect timeout after %lums", (unsigned long)timeout_ms);
        mongo_transport_close(t);
        return MONGO_TRANSPORT_ERR_TIMEOUT;
    }

    if (t->state != XPORT_CONNECTED) {
        error("[xport] connect failed: state=%d err=%d", t->state, (int)t->last_err);
        mongo_transport_close(t);
        return MONGO_TRANSPORT_ERR_CONNECT;
    }
    return MONGO_TRANSPORT_OK;
}

int mongo_transport_send(mongo_transport_t *t, const uint8_t *data, size_t len, uint32_t timeout_ms) {
    if (!t || !data) {
        return MONGO_TRANSPORT_ERR_ARGS;
    }
    if (t->state != XPORT_CONNECTED) {
        return MONGO_TRANSPORT_ERR_STATE;
    }

    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (len > 0) {
        cyw43_arch_lwip_begin();
        u16_t snd = t->pcb ? altcp_sndbuf(t->pcb) : 0;
        size_t chunk = (len < snd) ? len : snd;
        err_t err = ERR_OK;
        if (t->pcb && chunk > 0) {
            err = altcp_write(t->pcb, data, chunk, TCP_WRITE_FLAG_COPY);
            if (err == ERR_OK) {
                err = altcp_output(t->pcb);
            }
        } else if (!t->pcb) {
            err = ERR_CLSD;
        }
        cyw43_arch_lwip_end();

        if (!t->pcb || t->state != XPORT_CONNECTED) {
            return MONGO_TRANSPORT_ERR_CLOSED;
        }
        if (err == ERR_OK && chunk > 0) {
            data += chunk;
            len -= chunk;
            continue;
        }
        if (err == ERR_MEM || chunk == 0) {
            /* Send buffer full -- back off and retry. The sent callback would
             * be a nicer wakeup, but the time-based wait is correct and
             * simpler for a single-connection MCU. */
            if (xTaskGetTickCount() >= deadline) {
                return MONGO_TRANSPORT_ERR_TIMEOUT;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        t->last_err = err;
        return MONGO_TRANSPORT_ERR_LWIP;
    }
    return MONGO_TRANSPORT_OK;
}

int mongo_transport_recv_exact(mongo_transport_t *t, uint8_t *buf, size_t want, uint32_t timeout_ms) {
    if (!t || !buf) {
        return MONGO_TRANSPORT_ERR_ARGS;
    }

    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    size_t got = 0;

    while (got < want) {
        size_t drained_this_pass = 0;

        cyw43_arch_lwip_begin();
        while (t->rx_head && got < want) {
            uint16_t avail = t->rx_head->len - t->rx_offset;
            size_t need = want - got;
            size_t copy = (avail < need) ? avail : need;
            memcpy(buf + got, (uint8_t *)t->rx_head->payload + t->rx_offset, copy);
            t->rx_offset += (uint16_t)copy;
            got += copy;
            drained_this_pass += copy;
            if (t->rx_offset >= t->rx_head->len) {
                struct pbuf *next = t->rx_head->next;
                if (next) {
                    pbuf_ref(next);
                }
                pbuf_free(t->rx_head);
                t->rx_head = next;
                t->rx_offset = 0;
            }
        }
        /* altcp_recved takes a uint16_t window-credit. Chunk so we don't
         * silently truncate when a single drain pass consumes more than
         * 64 KB (the cursor batch path can hit this on big find results). */
        if (drained_this_pass > 0 && t->pcb) {
            size_t remaining = drained_this_pass;
            while (remaining > 0) {
                uint16_t chunk = remaining > UINT16_MAX ? UINT16_MAX : (uint16_t)remaining;
                altcp_recved(t->pcb, chunk);
                remaining -= chunk;
            }
        }
        cyw43_arch_lwip_end();

        if (got >= want) {
            break;
        }
        if (t->state == XPORT_CLOSED) {
            return MONGO_TRANSPORT_ERR_CLOSED;
        }
        if (t->state == XPORT_ERROR) {
            return MONGO_TRANSPORT_ERR_LWIP;
        }

        TickType_t now = xTaskGetTickCount();
        if (now >= deadline) {
            return MONGO_TRANSPORT_ERR_TIMEOUT;
        }
        TickType_t wait = deadline - now;
        if (xSemaphoreTake(t->recv_sem, wait) != pdTRUE) {
            return MONGO_TRANSPORT_ERR_TIMEOUT;
        }
    }
    return MONGO_TRANSPORT_OK;
}

void mongo_transport_close(mongo_transport_t *t) {
    if (!t) {
        return;
    }

    cyw43_arch_lwip_begin();
    if (t->pcb) {
        /* Detach callbacks so any in-flight events don't touch us. */
        altcp_arg(t->pcb, NULL);
        altcp_recv(t->pcb, NULL);
        altcp_err(t->pcb, NULL);
        err_t err = altcp_close(t->pcb);
        if (err != ERR_OK) {
            /* altcp_close can fail under memory pressure; abort is the
             * fallback. abort always succeeds and frees the pcb. */
            altcp_abort(t->pcb);
        }
        t->pcb = NULL;
    }
    if (t->rx_head) {
        pbuf_free(t->rx_head);
        t->rx_head = NULL;
        t->rx_offset = 0;
    }
    cyw43_arch_lwip_end();

    t->state = XPORT_CLOSED;
}
