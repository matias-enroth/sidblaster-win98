/* netsid_net.h - Network SID Device transport over Winsock 1.1 (no C runtime).
   WSOCK32.DLL is loaded on first use, not imported, so the SIDBlaster backend
   and SBTEST COMx don't depend on networking being installed.             */
#ifndef NETSID_NET_H
#define NETSID_NET_H
#include <windows.h>
#include <winsock.h>
#include "netsid_proto.h"

typedef void (*ns_logfn)(const char *fmt, ...);   /* %s %d %u %lu %02X only */

typedef struct {
    SOCKET   s;               /* INVALID_SOCKET while down */
    ns_logfn log;
    DWORD    timeout_ms;      /* wait for a response */
    DWORD    read_timeout_ms; /* ... to a TRY_READ (the server answers in playback time) */
    DWORD    busy_ms;         /* give up after BUSY for this long */
    int      version;         /* server protocol version */
    int      stale;           /* bytes of an ERROR message may still arrive */
    unsigned long addr;       /* server address, resolved once (0 = not yet) */
    DWORD    qpf_khz;         /* for max_rtt_us; 0 = don't time */
    /* counters: the caller reads and resets them */
    DWORD    pkts, busy, errors, max_rtt_us;
} ns_link;

typedef struct {
    int model;      /* SID config index, or 6581 / 8580 for the first such config */
    int ntsc;       /* SET_CLOCKING: 0 PAL, 1 NTSC */
} ns_setup;

BOOL ns_net_startup(ns_logfn log);        /* load WSOCK32, WSAStartup(1.1); once */
void ns_link_init(ns_link *l, ns_logfn log, DWORD qpf_khz);
/* connect (connect_ms timeout) and run the init sequence; logs what it finds */
BOOL ns_link_open(ns_link *l, const char *host, WORD port, const ns_setup *su, DWORD connect_ms);
void ns_link_close(ns_link *l);
/* ns_emit_fn for ns_sched: sends one packet, retries on BUSY.
   Returns 0 (and closes the link) on a network error, a timeout or endless BUSY;
   an ERROR reply is logged and counted, and returns 1.                   */
int  ns_link_send(void *link, const uint8_t *pkt, unsigned len, uint8_t *readval);
/* best effort at exit: FLUSH, TRY_RESET(0), close; about 300 ms at most */
void ns_link_goodbye(ns_link *l);

#endif
