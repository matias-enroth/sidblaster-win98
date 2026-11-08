/* netsid_proto.h - Network SID Device protocol (V2-V5 subset): packet encoding,
   response framing, QPC-ticks-to-C64-cycles conversion and the write scheduler.
   Pure C: no Win32, no C runtime, no 64-bit division. Builds natively for tests.
   Protocol: docs/netsiddev.adoc in github.com/WilfredC64/acid64c (implemented
   from the spec, not from GPL code).                                           */
#ifndef NETSID_PROTO_H
#define NETSID_PROTO_H
#include <stdint.h>

/* commands */
enum {
    NS_FLUSH = 0, NS_TRY_SET_SID_COUNT = 1, NS_MUTE = 2, NS_TRY_RESET = 3,
    NS_TRY_DELAY = 4, NS_TRY_WRITE = 5, NS_TRY_READ = 6, NS_GET_VERSION = 7,
    NS_TRY_SET_SAMPLING = 8, NS_SET_CLOCKING = 9, NS_GET_CONFIG_COUNT = 10,
    NS_GET_CONFIG_INFO = 11, NS_SET_SID_MODEL = 14
};
/* response codes */
enum {
    NS_R_OK = 0, NS_R_BUSY = 1, NS_R_ERROR = 2, NS_R_READ = 3,
    NS_R_VERSION = 4, NS_R_COUNT = 5, NS_R_INFO = 6
};

#define NS_HDR       4
/* Our TRY_WRITEs, header included. The spec suggests about 1 KB, but SID-Device
   takes each recv() as one request, so a packet must fit one TCP segment even
   at the 536-byte MSS of PPP links. A frame is about 25 writes anyway.     */
#define NS_PKT_MAX   512
#define NS_WR_MAX    ((NS_PKT_MAX - NS_HDR) / 4)  /* 127 writes per TRY_WRITE        */
#define NS_PKT_BUF   (NS_PKT_MAX + 4)            /* room for a TRY_READ's 3-byte tail */
#define NS_INFO_MAX  (2 + 256)                   /* INFO: code, model, name <= 255, NUL */
#define NS_PAL_HZ    985248u
#define NS_NTSC_HZ   1022727u
#define NS_DEFAULT_PORT 6581

/* Packet builders; each returns the packet length. */
unsigned ns_hdr(uint8_t *p, uint8_t cmd, uint8_t sid, uint16_t len);
unsigned ns_pkt0(uint8_t *p, uint8_t cmd, uint8_t sid);
unsigned ns_pkt1(uint8_t *p, uint8_t cmd, uint8_t sid, uint8_t a);
unsigned ns_pkt2(uint8_t *p, uint8_t cmd, uint8_t sid, uint8_t a, uint8_t b);
unsigned ns_pkt_delay(uint8_t *p, uint16_t cycles);   /* TRY_DELAY; cycles != 0 */

/* Response framing. Returns the full response length once b[0..n) holds a
   complete response, 0 if more bytes are needed, -1 for an unknown code.
   ERROR counts as 1 byte: its optional message is unframed, so the caller
   drains whatever else arrives within a short timeout.                     */
int ns_resp_len(const uint8_t *b, unsigned n);

/* Fixed-point clock conversion: cycles = ticks * clk / qpf, with the fraction
   carried from call to call so that long runs don't drift.               */
typedef struct {
    uint64_t ratio;     /* clk / qpf in 32.32 fixed point */
    uint32_t frac;      /* carried fraction, 1/2^32 cycles */
} ns_conv;
#define NS_FRAC_HALF 0x80000000u   /* start at 1/2: the running total rounds to nearest */
void     ns_conv_init(ns_conv *c, uint32_t clk_hz, uint32_t qpf);
uint32_t ns_conv_step(ns_conv *c, uint32_t dticks);  /* dticks <= a few seconds' worth */
uint64_t ns_ms_to_ticks(uint32_t qpf, uint32_t ms);
uint32_t ns_ms_to_cycles(uint32_t clk_hz, uint32_t ms);  /* ms <= 4000 */

/* ---- scheduler ----------------------------------------------------------
   Turns time-stamped register writes into TRY_WRITE / TRY_DELAY packets.
   Delays come from the stamps (QPC ticks), so the server replays the writes
   with the spacing they had when the caller made them, plus a constant lead:
   - the first write, and Mute(0) via ns_sched_prime, send TRY_DELAY packets
     totalling `lead` before carrying on (the cushion that absorbs stalls);
   - a gap of more than 2 x lead (pause, stop) re-primes instead of sending the
     gap: the server has run dry by then, and would otherwise add the whole
     gap to the lead;
   - a prime within `lead` of the previous one is skipped, so a gap-prime
     right before Mute(0) doesn't double the lead.
   Packets go out through `emit`, one at a time. Functions return 1, or 0 when
   `emit` failed (link down); then call ns_sched_restart.                  */
typedef int (*ns_emit_fn)(void *ctx, const uint8_t *pkt, unsigned len, uint8_t *readval);
    /* returns 1 if the server took the packet (for TRY_READ: *readval set), 0 if the link failed */

typedef struct {
    ns_conv  conv;
    uint32_t lead_cycles, min_spacing;
    uint64_t lead_ticks, gap_ticks;
    ns_emit_fn emit;
    void    *ctx;
    int      nvoices;          /* MUTE voices: 3, or 4 on protocol V3+ */

    int      have_t, have_prime;
    int64_t  last_t, prime_t;
    uint32_t owed;             /* cycles MinSpacing has pushed us ahead of the stamps */
    unsigned nw;               /* writes in pkt */
    uint8_t  pkt[NS_PKT_BUF];
    uint8_t  aux[8];

    uint32_t primes, delays;   /* counters, for stats: caller may reset */
} ns_sched;

void ns_sched_init(ns_sched *s, uint32_t clk_hz, uint32_t qpf, uint32_t lead_ms,
                   uint32_t min_spacing, ns_emit_fn emit, void *ctx);
void ns_sched_restart(ns_sched *s);   /* forget the timeline and pending writes */
int  ns_sched_write(ns_sched *s, int64_t t, uint8_t reg, uint8_t val);
int  ns_sched_prime(ns_sched *s, int64_t t);
int  ns_sched_mute(ns_sched *s, int mute);   /* flushes, then MUTE every voice */
/* FLUSH, then TRY_RESET(0): the server drops what it has queued and goes
   quiet, and its SID starts from reset rather than the old register state
   (which it would otherwise play during the next lead). Pending writes are
   dropped too, and the timeline starts over (the next write primes).     */
int  ns_sched_drop(ns_sched *s);
int  ns_sched_read(ns_sched *s, int64_t t, uint8_t reg, uint8_t *val);
int  ns_sched_flush(ns_sched *s);             /* send the pending TRY_WRITE, if any */

#endif
