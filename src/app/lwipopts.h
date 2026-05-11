#ifndef LWIPOPTS_H
#define LWIPOPTS_H

/* lwIP options for micro-mongodb on Pico 2 W + FreeRTOS.
 * Targets: TCP, UDP (for hand-rolled DNS SRV/TXT), DNS, altcp + altcp_tls.
 */

#define NO_SYS 0      /* run under FreeRTOS, not bare-metal */
#define LWIP_SOCKET 0 /* RAW/altcp API only */
#define LWIP_NETCONN 0
#define LWIP_NETIF_HOSTNAME 1
#define LWIP_NETIF_STATUS_CALLBACK 1
#define LWIP_NETIF_LINK_CALLBACK 1

#define LWIP_DHCP 1
#define LWIP_DNS 1
#define LWIP_UDP 1 /* required for our SRV/TXT queries */
#define LWIP_TCP 1
#define LWIP_RAW 0
#define LWIP_IPV4 1
#define LWIP_IPV6 0

/* altcp lets us swap plain TCP <-> TLS without touching call sites. mbedTLS
 * is wired in for mongodb+srv:// (Atlas) and any URI with tls=true. */
#define LWIP_ALTCP 1
#define LWIP_ALTCP_TLS 1
#define LWIP_ALTCP_TLS_MBEDTLS 1

/* Buffer & memory sizing. These are conservative; revisit if we see drops. */
#define MEM_LIBC_MALLOC 0
#define MEM_ALIGNMENT 4
#define MEM_SIZE 8000
#define MEMP_NUM_TCP_SEG 32
#define MEMP_NUM_ARP_QUEUE 10
#define PBUF_POOL_SIZE 24

#define LWIP_ARP 1
#define LWIP_ETHERNET 1
#define LWIP_ICMP 1
#define LWIP_CHECKSUM_ON_COPY 1

#define TCP_MSS 1460
#define TCP_WND (8 * TCP_MSS)
#define TCP_SND_BUF (8 * TCP_MSS)
#define TCP_SND_QUEUELEN ((4 * (TCP_SND_BUF) + (TCP_MSS - 1)) / (TCP_MSS))

#define LWIP_STATS 0
#define LWIP_STATS_DISPLAY 0

/* tcpip_thread settings (FreeRTOS task that owns the lwIP core) */
#define TCPIP_THREAD_STACKSIZE 2048
#define TCPIP_THREAD_PRIO 8
#define TCPIP_MBOX_SIZE 8
#define DEFAULT_UDP_RECVMBOX_SIZE 8
#define DEFAULT_TCP_RECVMBOX_SIZE 8
#define DEFAULT_ACCEPTMBOX_SIZE 8
#define DEFAULT_RAW_RECVMBOX_SIZE 8
#define DEFAULT_THREAD_STACKSIZE 1024
#define DEFAULT_THREAD_PRIO 4

#endif /* LWIPOPTS_H */
