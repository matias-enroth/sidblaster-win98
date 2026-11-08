/* netsid_proto.c - see netsid_proto.h. Pure C, no C runtime, no 64-bit divide. */
#include "netsid_proto.h"

/* ------------------------------------------------------------- packets */
unsigned ns_hdr(uint8_t *p, uint8_t cmd, uint8_t sid, uint16_t len)
{
    p[0] = cmd;
    p[1] = sid;
    p[2] = (uint8_t)(len >> 8);    /* big-endian */
    p[3] = (uint8_t)len;
    return NS_HDR;
}

unsigned ns_pkt0(uint8_t *p, uint8_t cmd, uint8_t sid)
{
    return ns_hdr(p, cmd, sid, 0);
}

unsigned ns_pkt1(uint8_t *p, uint8_t cmd, uint8_t sid, uint8_t a)
{
    ns_hdr(p, cmd, sid, 1);
    p[4] = a;
    return NS_HDR + 1;
}

unsigned ns_pkt2(uint8_t *p, uint8_t cmd, uint8_t sid, uint8_t a, uint8_t b)
{
    ns_hdr(p, cmd, sid, 2);
    p[4] = a;
    p[5] = b;
    return NS_HDR + 2;
}

unsigned ns_pkt_delay(uint8_t *p, uint16_t cycles)
{
    return ns_pkt2(p, NS_TRY_DELAY, 0, (uint8_t)(cycles >> 8), (uint8_t)cycles);
}

int ns_resp_len(const uint8_t *b, unsigned n)
{
    unsigned i;
    if (n == 0) return 0;
    switch (b[0]) {
    case NS_R_OK: case NS_R_BUSY: case NS_R_ERROR:
        return 1;
    case NS_R_READ: case NS_R_VERSION: case NS_R_COUNT:
        return n >= 2 ? 2 : 0;
    case NS_R_INFO:
        for (i = 2; i < n && i < NS_INFO_MAX; i++)
            if (b[i] == 0) return (int)i + 1;
        return n >= NS_INFO_MAX ? NS_INFO_MAX : 0;   /* no NUL: take the maximum */
    }
    return -1;
}

/* --------------------------------------------------------------- clock */
void ns_conv_init(ns_conv *c, uint32_t clk_hz, uint32_t qpf)
{
    uint64_t r;
    int i;
    if (!qpf) qpf = 1;
    c->ratio = clk_hz / qpf;             /* 32-bit divide only */
    r = clk_hz % qpf;
    for (i = 0; i < 32; i++) {           /* long division for the fraction */
        r <<= 1;
        c->ratio <<= 1;
        if (r >= qpf) { r -= qpf; c->ratio |= 1; }
    }
    r <<= 1;
    if (r >= qpf) c->ratio++;            /* round to nearest */
    c->frac = NS_FRAC_HALF;
}

uint32_t ns_conv_step(ns_conv *c, uint32_t dticks)
{
    uint64_t a = (uint64_t)dticks * c->ratio + c->frac;
    c->frac = (uint32_t)a;
    return (uint32_t)(a >> 32);
}

uint64_t ns_ms_to_ticks(uint32_t qpf, uint32_t ms)
{
    return (uint64_t)(qpf / 1000) * ms + (qpf % 1000) * ms / 1000;
}

uint32_t ns_ms_to_cycles(uint32_t clk_hz, uint32_t ms)
{
    return (clk_hz / 1000) * ms + ((clk_hz % 1000) * ms + 500) / 1000;
}

/* ----------------------------------------------------------- scheduler */
static void put16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

void ns_sched_init(ns_sched *s, uint32_t clk_hz, uint32_t qpf, uint32_t lead_ms,
                   uint32_t min_spacing, ns_emit_fn emit, void *ctx)
{
    ns_conv_init(&s->conv, clk_hz, qpf);
    s->lead_cycles = ns_ms_to_cycles(clk_hz, lead_ms);
    s->lead_ticks = ns_ms_to_ticks(qpf, lead_ms);
    s->gap_ticks = s->lead_ticks * 2;
    s->min_spacing = min_spacing;
    s->emit = emit;
    s->ctx = ctx;
    s->nvoices = 4;
    s->primes = s->delays = 0;
    ns_sched_restart(s);
}

void ns_sched_restart(ns_sched *s)
{
    s->have_t = s->have_prime = 0;
    s->last_t = s->prime_t = 0;
    s->owed = 0;
    s->nw = 0;
    s->conv.frac = NS_FRAC_HALF;
}

/* TRY_DELAY packets totalling `cycles` (0 sends nothing) */
static int send_delay(ns_sched *s, uint32_t cycles)
{
    while (cycles) {
        uint32_t c = cycles > 0xFFFF ? 0xFFFF : cycles;
        unsigned n = ns_pkt_delay(s->aux, (uint16_t)c);
        if (!s->emit(s->ctx, s->aux, n, 0)) return 0;
        s->delays++;
        cycles -= c;
    }
    return 1;
}

int ns_sched_flush(ns_sched *s)
{
    unsigned n = s->nw * 4;
    if (!n) return 1;
    s->nw = 0;
    ns_hdr(s->pkt, NS_TRY_WRITE, 0, (uint16_t)n);
    return s->emit(s->ctx, s->pkt, NS_HDR + n, 0);
}

/* Start the timeline over at t, `lead` ahead of the stamps. */
static int prime_at(ns_sched *s, int64_t t)
{
    s->have_t = 1;
    s->last_t = t;
    s->conv.frac = NS_FRAC_HALF;
    s->owed = 0;
    s->have_prime = 1;
    s->prime_t = t;
    s->primes++;
    return ns_sched_flush(s) && send_delay(s, s->lead_cycles);
}

/* The delay (<= 0xFFFF) to put on an event stamped t. Sends a prime instead
   when the timeline is new or after a long gap (the event then gets 0), and
   sends a gap's excess over 0xFFFF as TRY_DELAYs ahead of the event.       */
static int advance(ns_sched *s, int64_t t, uint32_t *delay)
{
    int64_t d;
    uint32_t c = 0;

    *delay = 0;
    if (!s->have_t) return prime_at(s, t);
    d = t - s->last_t;
    if (d < 0) d = 0;                     /* stamps from racing threads */
    if ((uint64_t)d > s->gap_ticks) return prime_at(s, t);
    s->last_t = t;
    while (d > 0x40000000) {              /* only with a GHz-rate QPC */
        c += ns_conv_step(&s->conv, 0x40000000);
        d -= 0x40000000;
    }
    c += ns_conv_step(&s->conv, (uint32_t)d);

    /* MinSpacing: never closer than min_spacing, but pay the extra back
       from later delays, so the timeline doesn't drift from the stamps. */
    if (c >= s->owed + s->min_spacing) {
        c -= s->owed;
        s->owed = 0;
    } else {
        s->owed = s->owed + s->min_spacing - c;
        c = s->min_spacing;
        /* writes steadily closer than min_spacing (a digi loop): forgive the
           excess rather than fall ever further behind (the lead grows) */
        if (s->owed > s->lead_cycles) s->owed = s->lead_cycles;
    }

    if (c > 0xFFFF) {
        if (!ns_sched_flush(s) || !send_delay(s, c - 0xFFFF)) return 0;
        c = 0xFFFF;
    }
    *delay = c;
    return 1;
}

int ns_sched_write(ns_sched *s, int64_t t, uint8_t reg, uint8_t val)
{
    uint32_t d;
    uint8_t *p;
    if (!advance(s, t, &d)) return 0;
    if (s->nw >= NS_WR_MAX && !ns_sched_flush(s)) return 0;
    p = s->pkt + NS_HDR + s->nw * 4;
    put16(p, d);
    p[2] = reg;
    p[3] = val;
    s->nw++;
    return 1;
}

int ns_sched_prime(ns_sched *s, int64_t t)
{
    if (s->have_prime && t >= s->prime_t && (uint64_t)(t - s->prime_t) < s->lead_ticks)
        return 1;                         /* just primed */
    return prime_at(s, t);
}

int ns_sched_mute(ns_sched *s, int mute)
{
    int v;
    unsigned n;
    if (!ns_sched_flush(s)) return 0;
    for (v = 0; v < s->nvoices; v++) {
        /* 1 mutes, 0 unmutes (as acid64c sends it) */
        n = ns_pkt2(s->aux, NS_MUTE, 0, (uint8_t)v, (uint8_t)(mute ? 1 : 0));
        if (!s->emit(s->ctx, s->aux, n, 0)) return 0;
    }
    return 1;
}

int ns_sched_drop(ns_sched *s)
{
    unsigned n = ns_pkt0(s->aux, NS_FLUSH, 0);
    ns_sched_restart(s);
    if (!s->emit(s->ctx, s->aux, n, 0)) return 0;
    n = ns_pkt1(s->aux, NS_TRY_RESET, 0, 0);
    return s->emit(s->ctx, s->aux, n, 0);
}

/* TRY_READ, with the pending writes in front of the read. */
int ns_sched_read(ns_sched *s, int64_t t, uint8_t reg, uint8_t *val)
{
    uint32_t d;
    uint8_t *p;
    unsigned n;
    *val = 0;
    if (!advance(s, t, &d)) return 0;
    if (s->nw >= NS_WR_MAX && !ns_sched_flush(s)) return 0;
    p = s->pkt + NS_HDR + s->nw * 4;
    put16(p, d);
    p[2] = reg;
    n = s->nw * 4 + 3;
    s->nw = 0;
    ns_hdr(s->pkt, NS_TRY_READ, 0, (uint16_t)n);
    return s->emit(s->ctx, s->pkt, NS_HDR + n, val);
}
