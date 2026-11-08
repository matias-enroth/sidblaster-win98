/* Native unit tests for netsid_proto.c and minifmt.c.  Run: tests/run.sh */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/netsid_proto.h"
#include "../src/minifmt.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECKEQ(a, b) do { long long _a = (long long)(a), _b = (long long)(b); checks++; \
    if (_a != _b) { fails++; printf("FAIL %s:%d: %s == %lld, expected %lld\n", __FILE__, __LINE__, #a, _a, _b); } } while (0)

/* ------------------------------------------------------- capture emit */
typedef struct { uint8_t b[NS_PKT_BUF]; unsigned n; } pkt_t;
static pkt_t cap[4096];
static unsigned ncap;
static int fail_after = -1;      /* emit fails once this many packets were taken */
static uint8_t read_answer = 0x5A;

static int cap_emit(void *ctx, const uint8_t *p, unsigned n, uint8_t *rv)
{
    (void)ctx;
    if (fail_after >= 0 && (int)ncap >= fail_after) return 0;
    if (ncap < sizeof cap / sizeof cap[0]) { memcpy(cap[ncap].b, p, n); cap[ncap].n = n; }
    ncap++;
    if (p[0] == NS_TRY_READ && rv) *rv = read_answer;
    return 1;
}
static void cap_reset(void) { ncap = 0; fail_after = -1; }
static unsigned be16(const uint8_t *p) { return (unsigned)p[0] << 8 | p[1]; }
static unsigned plen(const pkt_t *p) { return be16(p->b + 2); }

/* total cycles of all delays in captured packets [from, ncap) */
static unsigned long long cap_cycles(unsigned from)
{
    unsigned long long t = 0;
    unsigned i, j;
    for (i = from; i < ncap; i++) {
        const pkt_t *p = &cap[i];
        if (p->b[0] == NS_TRY_DELAY) t += be16(p->b + 4);
        if (p->b[0] == NS_TRY_WRITE) for (j = 0; j < plen(p); j += 4) t += be16(p->b + 4 + j);
        if (p->b[0] == NS_TRY_READ) {
            for (j = 0; j + 3 < plen(p); j += 4) t += be16(p->b + 4 + j);
            t += be16(p->b + 4 + j);
        }
    }
    return t;
}

#define QPF 1193182u
#define MS(x) ((int64_t)(x) * QPF / 1000)

/* -------------------------------------------------------------- tests */
static void test_packets(void)
{
    uint8_t p[8];
    CHECKEQ(ns_hdr(p, NS_TRY_WRITE, 3, 0x1234), 4);
    CHECK(p[0] == 5 && p[1] == 3 && p[2] == 0x12 && p[3] == 0x34);
    CHECKEQ(ns_pkt_delay(p, 0xABCD), 6);
    CHECK(p[0] == NS_TRY_DELAY && p[1] == 0 && p[2] == 0 && p[3] == 2 && p[4] == 0xAB && p[5] == 0xCD);
    CHECKEQ(ns_pkt0(p, NS_GET_VERSION, 0), 4);
    CHECK(p[0] == 7 && p[2] == 0 && p[3] == 0);
    CHECKEQ(ns_pkt1(p, NS_TRY_RESET, 0, 0x0F), 5);
    CHECK(p[3] == 1 && p[4] == 0x0F);
    CHECKEQ(ns_pkt2(p, NS_MUTE, 0, 3, 1), 6);
    CHECK(p[0] == 2 && p[3] == 2 && p[4] == 3 && p[5] == 1);
}

static void test_responses(void)
{
    uint8_t b[300];
    unsigned i;
    CHECKEQ(ns_resp_len(b, 0), 0);
    b[0] = NS_R_OK;    CHECKEQ(ns_resp_len(b, 1), 1);
    b[0] = NS_R_BUSY;  CHECKEQ(ns_resp_len(b, 1), 1);
    b[0] = NS_R_ERROR; CHECKEQ(ns_resp_len(b, 1), 1);
    b[0] = NS_R_READ;  CHECKEQ(ns_resp_len(b, 1), 0);
    b[1] = 0x42;       CHECKEQ(ns_resp_len(b, 2), 2);
    b[0] = NS_R_VERSION; CHECKEQ(ns_resp_len(b, 2), 2);
    b[0] = NS_R_COUNT;   CHECKEQ(ns_resp_len(b, 1), 0);
    b[0] = 7;          CHECKEQ(ns_resp_len(b, 1), -1);
    b[0] = 0xFF;       CHECKEQ(ns_resp_len(b, 1), -1);
    /* INFO: code, model, name, NUL - arriving in pieces */
    memcpy(b, "\x06\x01reSID (8580)", 14);
    b[14] = 0;
    for (i = 0; i <= 14; i++) CHECKEQ(ns_resp_len(b, i), 0);
    CHECKEQ(ns_resp_len(b, 15), 15);
    CHECKEQ(ns_resp_len(b, 20), 15);          /* extra bytes aren't part of it */
    b[1] = 0; b[2] = 0;                        /* empty name */
    CHECKEQ(ns_resp_len(b, 3), 3);
    memset(b + 2, 'x', sizeof b - 2);          /* name never terminated */
    CHECKEQ(ns_resp_len(b, 257), 0);
    CHECKEQ(ns_resp_len(b, 258), 258);
    CHECKEQ(ns_resp_len(b, 300), 258);
}

static void conv_hour(uint32_t clk, uint32_t qpf)
{
    ns_conv c;
    unsigned long long ticks = 0, cycles = 0;
    long double worst = 0;
    unsigned long long next_check = qpf;
    srand(clk ^ qpf);
    ns_conv_init(&c, clk, qpf);
    while (ticks < 3600ULL * qpf) {
        uint32_t d = (uint32_t)(rand() % (qpf / 20));     /* up to 50 ms */
        if (rand() % 50 == 0) d = (uint32_t)(rand() % 3);  /* bursts */
        ticks += d;
        cycles += ns_conv_step(&c, d);
        if (ticks >= next_check) {
            long double exact = (long double)ticks * clk / qpf;
            long double err = (long double)cycles - exact;
            if (err < 0) err = -err;
            if (err > worst) worst = err;
            next_check += qpf;
        }
    }
    printf("  conv %u Hz from QPF %u: worst error over an hour %.3Lf cycles\n", clk, qpf, worst);
    CHECK(worst < (qpf == QPF ? 1.0 : 10.0));   /* Win9x's QPC: under a cycle an hour */
}

static void test_conv(void)
{
    ns_conv c;
    CHECKEQ(ns_ms_to_cycles(NS_PAL_HZ, 100), 98525);
    CHECKEQ(ns_ms_to_cycles(NS_NTSC_HZ, 100), 102273);
    CHECKEQ(ns_ms_to_cycles(NS_PAL_HZ, 4000), 3940992);
    CHECKEQ(ns_ms_to_ticks(QPF, 100), 119318);
    CHECKEQ(ns_ms_to_ticks(3000000000u, 2000), 6000000000ULL);
    ns_conv_init(&c, 1000000, 1000000);
    CHECKEQ(c.ratio, 1ULL << 32);
    CHECKEQ(ns_conv_step(&c, 12345), 12345);
    ns_conv_init(&c, NS_PAL_HZ, QPF);
    CHECKEQ(ns_conv_step(&c, QPF), NS_PAL_HZ);   /* a second is a second */
    conv_hour(NS_PAL_HZ, QPF);
    conv_hour(NS_NTSC_HZ, QPF);
    conv_hour(NS_PAL_HZ, 3579545);    /* NT ACPI timer */
    conv_hour(NS_PAL_HZ, 10000000);   /* Windows 10 */
    conv_hour(NS_PAL_HZ, 1000);       /* slower than the C64 clock */
}

static ns_sched S;

static void sched_new(uint32_t lead_ms, uint32_t spacing)
{
    cap_reset();
    ns_sched_init(&S, NS_PAL_HZ, QPF, lead_ms, spacing, cap_emit, NULL);
}

static void test_prime_and_bursts(void)
{
    int i;
    unsigned from;
    int64_t t0 = 1000000000LL;   /* QPC doesn't start at 0 */

    sched_new(100, 0);
    CHECK(ns_sched_write(&S, t0, 0x18, 0x0F));
    /* first write primes: TRY_DELAYs totalling the lead, nothing else yet */
    CHECKEQ(ncap, 2);
    CHECK(cap[0].b[0] == NS_TRY_DELAY && be16(cap[0].b + 4) == 0xFFFF);
    CHECK(cap[1].b[0] == NS_TRY_DELAY && be16(cap[1].b + 4) == 98525 - 0xFFFF);
    for (i = 1; i < 25; i++) CHECK(ns_sched_write(&S, t0 + i * 3, (uint8_t)i, (uint8_t)i));
    CHECKEQ(ncap, 2);              /* still buffered */
    CHECK(ns_sched_flush(&S));
    CHECKEQ(ncap, 3);
    CHECK(cap[2].b[0] == NS_TRY_WRITE && plen(&cap[2]) == 25 * 4 && cap[2].n == 4 + 100);
    CHECKEQ(be16(cap[2].b + 4), 0);                       /* first write: right after the lead */
    CHECK(cap[2].b[6] == 0x18 && cap[2].b[7] == 0x0F);
    CHECKEQ(be16(cap[2].b + 8), 2);                       /* 3 ticks = 2.48 cycles */
    CHECKEQ(be16(cap[2].b + 12), 3);                      /* 4.95 in all: fraction carried */
    CHECK(ns_sched_flush(&S));
    CHECKEQ(ncap, 3);              /* nothing pending: no packet */

    /* next frame, 20 ms after the first write */
    from = ncap;
    CHECK(ns_sched_write(&S, t0 + MS(20), 0x04, 0x41));
    CHECK(ns_sched_flush(&S));
    CHECKEQ(ncap, from + 1);
    /* 20 ms = 19705 cycles, less the ~60 already spent on the burst */
    CHECK(cap[from].b[0] == NS_TRY_WRITE);
    CHECK(be16(cap[from].b + 4) >= 19705 - 62 && be16(cap[from].b + 4) <= 19705 - 58);
    CHECKEQ(S.primes, 1);
}

static void test_split_1k(void)
{
    int i;
    sched_new(100, 0);
    CHECKEQ(NS_WR_MAX, 127);
    for (i = 0; i < 300; i++) CHECK(ns_sched_write(&S, 5000 + i, (uint8_t)(i & 0x1F), (uint8_t)i));
    CHECK(ns_sched_flush(&S));
    CHECKEQ(ncap, 2 + 3);
    CHECKEQ(cap[2].n, 512); CHECKEQ(plen(&cap[2]), 508);
    CHECKEQ(cap[3].n, 512); CHECKEQ(plen(&cap[3]), 508);
    CHECKEQ(cap[4].n, 4 + 46 * 4);
    CHECK(cap[4].b[4 + 45 * 4 + 3] == (uint8_t)299);
}

static void test_gaps(void)
{
    unsigned from;
    int64_t t = 77;
    sched_new(100, 0);
    CHECK(ns_sched_write(&S, t, 0, 1));
    CHECK(ns_sched_flush(&S));

    /* 150 ms: under 2 x lead, so the gap is kept: 147787 cycles, the excess
       over 0xFFFF as TRY_DELAYs in front of the write */
    from = ncap;
    t += MS(150);
    CHECK(ns_sched_write(&S, t, 0, 2));
    CHECK(ns_sched_flush(&S));
    CHECKEQ(ncap, from + 3);
    CHECK(cap[from].b[0] == NS_TRY_DELAY && cap[from + 1].b[0] == NS_TRY_DELAY);
    CHECK(cap[from + 2].b[0] == NS_TRY_WRITE && be16(cap[from + 2].b + 4) == 0xFFFF);
    CHECK(cap_cycles(from) >= 147786 && cap_cycles(from) <= 147788);

    /* pending writes go out before the TRY_DELAYs */
    from = ncap;
    CHECK(ns_sched_write(&S, t + 10, 0, 3));
    t += 10 + MS(100);
    CHECK(ns_sched_write(&S, t, 0, 4));
    CHECKEQ(ncap, from + 2);
    CHECK(cap[from].b[0] == NS_TRY_WRITE && cap[from].b[7] == 3);
    CHECK(cap[from + 1].b[0] == NS_TRY_DELAY);
    CHECK(ns_sched_flush(&S));
    CHECK(cap[from + 2].b[0] == NS_TRY_WRITE && cap[from + 2].b[7] == 4);

    /* 300 ms: the server has run dry, so re-prime instead */
    from = ncap;
    t += MS(300);
    CHECK(ns_sched_write(&S, t, 0, 5));
    CHECK(ns_sched_flush(&S));
    CHECKEQ(cap_cycles(from), 98525);
    CHECK(cap[ncap - 1].b[0] == NS_TRY_WRITE && be16(cap[ncap - 1].b + 4) == 0);
    CHECKEQ(S.primes, 2);
}

static void test_prime_holdoff(void)
{
    unsigned from;
    int64_t t = 5;
    sched_new(100, 0);
    /* playback start: Mute(1), zeros, Mute(0) - one lead, not two */
    CHECK(ns_sched_mute(&S, 1));
    CHECK(ns_sched_write(&S, t, 0x00, 0));
    CHECK(ns_sched_write(&S, t + 4, 0x18, 0));
    CHECK(ns_sched_mute(&S, 0));
    CHECK(ns_sched_prime(&S, t + 9));
    CHECKEQ(S.primes, 1);
    CHECKEQ(cap_cycles(0), 98525 + 3);
    /* Mute(0) during continuous play (next tune): another lead */
    t += MS(20);
    CHECK(ns_sched_write(&S, t, 0x01, 1));
    from = ncap;
    t += MS(150);
    CHECK(ns_sched_prime(&S, t));
    CHECKEQ(S.primes, 2);
    CHECK(cap[from].b[0] == NS_TRY_WRITE);       /* pending write first */
    CHECKEQ(cap_cycles(from + 1), 98525);
    /* the next write's delay counts from the prime */
    from = ncap;
    CHECK(ns_sched_write(&S, t + MS(10), 0x02, 2));
    CHECK(ns_sched_flush(&S));
    CHECK(cap_cycles(from) >= 9852 && cap_cycles(from) <= 9853);
}

static void test_mute_read(void)
{
    uint8_t v = 0;
    unsigned from;
    sched_new(100, 0);
    CHECK(ns_sched_write(&S, 100, 0x12, 0x80));
    from = ncap;
    CHECK(ns_sched_mute(&S, 1));
    CHECKEQ(ncap, from + 5);
    CHECK(cap[from].b[0] == NS_TRY_WRITE);
    CHECK(cap[from + 1].b[0] == NS_MUTE && cap[from + 1].b[4] == 0 && cap[from + 1].b[5] == 1);
    CHECK(cap[from + 4].b[0] == NS_MUTE && cap[from + 4].b[4] == 3 && cap[from + 4].b[5] == 1);
    S.nvoices = 3;
    from = ncap;
    CHECK(ns_sched_mute(&S, 0));
    CHECKEQ(ncap, from + 3);
    CHECK(cap[from + 2].b[4] == 2 && cap[from + 2].b[5] == 0);

    /* read: pending writes travel in the TRY_READ */
    CHECK(ns_sched_write(&S, 200, 0x0E, 0xFF));
    CHECK(ns_sched_write(&S, 300, 0x0F, 0xFF));
    from = ncap;
    CHECK(ns_sched_read(&S, 400, 0x1B, &v));
    CHECKEQ(v, 0x5A);
    CHECKEQ(ncap, from + 1);
    CHECK(cap[from].b[0] == NS_TRY_READ && plen(&cap[from]) == 2 * 4 + 3 && cap[from].n == 4 + 11);
    CHECK(cap[from].b[4 + 2] == 0x0E && cap[from].b[4 + 8 + 2] == 0x1B);
    /* a read on its own */
    from = ncap;
    CHECK(ns_sched_read(&S, 500, 0x1C, &v));
    CHECK(cap[from].b[0] == NS_TRY_READ && plen(&cap[from]) == 3 && cap[from].b[6] == 0x1C);
}

static void test_drop(void)
{
    unsigned from;
    sched_new(100, 0);
    CHECK(ns_sched_write(&S, 10, 0x04, 0x41));
    CHECK(ns_sched_flush(&S));
    CHECK(ns_sched_write(&S, 20, 0x04, 0x40));     /* pending: never sent */
    from = ncap;
    CHECK(ns_sched_drop(&S));
    CHECKEQ(ncap, from + 2);
    CHECK(cap[from].b[0] == NS_FLUSH && cap[from].n == 4);
    CHECK(cap[from + 1].b[0] == NS_TRY_RESET && cap[from + 1].n == 5 && cap[from + 1].b[4] == 0);
    /* the next write primes again, and a Mute(0) right after doesn't add more */
    CHECK(ns_sched_write(&S, 30, 0x00, 0));
    CHECK(ns_sched_prime(&S, 40));
    CHECK(ns_sched_flush(&S));
    CHECKEQ(cap_cycles(from + 2), 98525);
    CHECKEQ(S.primes, 2);
}

static void test_min_spacing(void)
{
    int i;
    int64_t t = 1;
    sched_new(100, 10);
    CHECK(ns_sched_write(&S, t, 0, 0));
    for (i = 1; i < 5; i++) CHECK(ns_sched_write(&S, t, (uint8_t)i, 0));
    CHECK(ns_sched_write(&S, t + MS(20), 9, 9));
    CHECK(ns_sched_flush(&S));
    CHECK(cap[2].b[0] == NS_TRY_WRITE && plen(&cap[2]) == 6 * 4);
    CHECKEQ(be16(cap[2].b + 4 + 0), 0);
    for (i = 1; i < 5; i++) CHECKEQ(be16(cap[2].b + 4 + 4 * i), 10);
    /* the 40 extra cycles come off the next delay: still 19705 after the first */
    CHECK(be16(cap[2].b + 4 + 20) >= 19705 - 40 - 1 && be16(cap[2].b + 4 + 20) <= 19705 - 40 + 1);
    CHECK(cap_cycles(2) >= 19704 && cap_cycles(2) <= 19706);
    /* a digi loop writing faster than min_spacing: the debt is capped at the lead */
    sched_new(100, 100);
    for (i = 0; i < 20000; i++) CHECK(ns_sched_write(&S, 1 + i, 0x18, (uint8_t)i));
    CHECK(S.owed <= S.lead_cycles);
}

static void test_emit_failure(void)
{
    int i, ok = 1;
    uint8_t v;
    sched_new(100, 0);
    fail_after = 1;                           /* second TRY_DELAY of the prime fails */
    CHECK(!ns_sched_write(&S, 10, 0, 0));
    sched_new(100, 0);
    fail_after = 2;                           /* prime goes out, the TRY_WRITE fails */
    for (i = 0; i < NS_WR_MAX; i++) ok &= ns_sched_write(&S, 10 + i, 0, 0);
    CHECK(ok);
    CHECK(!ns_sched_write(&S, 300, 0, 0));    /* one more needs a flush */
    CHECK(!ns_sched_mute(&S, 1));
    CHECK(!ns_sched_read(&S, 400, 0x1B, &v));
    ns_sched_restart(&S);
    CHECKEQ(S.nw, 0);
    CHECKEQ(S.have_t, 0);
}

/* A minute of 50 Hz frames with jittery stamps: total delay sent equals the
   lead plus the stamps' span, to within a cycle. */
static void test_timeline(void)
{
    int f, w;
    int64_t t0 = 123456789, t = t0, first = -1, last = t0;
    long double exact;
    sched_new(100, 0);
    srand(7);
    for (f = 0; f < 3000; f++) {
        t = t0 + (int64_t)f * QPF / 50 + rand() % 2000;   /* up to ~1.7 ms late */
        for (w = 0; w < 25; w++) {
            CHECK(ns_sched_write(&S, t + w * 4, (uint8_t)w, (uint8_t)f));
            if (first < 0) first = t;
            last = t + w * 4;
        }
        CHECK(ns_sched_flush(&S));
    }
    exact = 98525 + (long double)(last - first) * NS_PAL_HZ / QPF;
    printf("  timeline: %llu cycles sent, %.1Lf expected, %u packets\n", cap_cycles(0), exact, ncap);
    CHECK((long double)cap_cycles(0) - exact < 1.0 && exact - (long double)cap_cycles(0) < 1.0);
    CHECKEQ(S.primes, 1);
    CHECKEQ(ncap, 2 + 3000);
}

static void test_minifmt(void)
{
    char b[64];
    CHECKEQ(mf_fmt(b, sizeof b, "x=%lu y=%d z=%u", 4000000000UL, -42, 7u), 22);
    CHECK(strcmp(b, "x=4000000000 y=-42 z=7") == 0);
    mf_fmt(b, sizeof b, "$%02X=%02x %X %5d|%-3", 0x1B, 0xa, 0xBEEF, 42);
    CHECK(strcmp(b, "$1B=0a BEEF    42|%-3") == 0);
    mf_fmt(b, sizeof b, "%s:%c:%s%%", "ab", 'Q', (char *)0);
    CHECK(strcmp(b, "ab:Q:(null)%") == 0);
    mf_fmt(b, sizeof b, "%05d %ld %d", -42, -2147483647L - 1, 0);
    CHECK(strcmp(b, "-0042 -2147483648 0") == 0);
    CHECKEQ(mf_fmt(b, 8, "%s", "0123456789"), 7);
    CHECK(strcmp(b, "0123456") == 0);
    CHECKEQ(mf_fmt(b, 1, "abc"), 0);
    CHECK(b[0] == 0);
    mf_fmt(b, sizeof b, "end%");
    CHECK(strcmp(b, "end") == 0);
}

int main(void)
{
    test_packets();
    test_responses();
    test_conv();
    test_prime_and_bursts();
    test_split_1k();
    test_gaps();
    test_prime_holdoff();
    test_mute_read();
    test_drop();
    test_min_spacing();
    test_emit_failure();
    test_timeline();
    test_minifmt();
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
