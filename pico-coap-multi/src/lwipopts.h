#ifndef _LWIPOPTS_H
#define _LWIPOPTS_H

/* --- System mode (NO_SYS: no RTOS) ---------------------------------- */
#define NO_SYS                 1     /* required by pico_cyw43_arch_lwip_threadsafe_background */

/* --- Memory/pbufs (tweak to taste) ---------------------------------- */
#define MEM_ALIGNMENT          4
#define MEM_SIZE               4000
#define MEMP_NUM_PBUF          16
#define MEMP_NUM_UDP_PCB       6
#define PBUF_POOL_SIZE         16
#define PBUF_POOL_BUFSIZE      512

/* --- Core protocol enables ----------------------------------------- */
#define LWIP_ARP               1
#define LWIP_DHCP              1
#define LWIP_DNS               1
#define LWIP_ICMP              1
#define LWIP_UDP               1
#define LWIP_TCP               1

/* --- APIs to DISABLE for NO_SYS builds ------------------------------ */
#define LWIP_NETIF_API         0
#define LWIP_NETCONN           0
#define LWIP_SOCKET            0

/* Optional: hostname support */
#define LWIP_NETIF_HOSTNAME    1

#endif /* _LWIPOPTS_H */
