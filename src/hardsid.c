/*
 * HardSID.dll replacement for SIDPLAY/Windows (Nov 2002) on Windows 98.
 * Two backends, chosen with Backend= in SIDBLAST.INI:
 *   SIDBlaster  a SIDBlaster-USB (FT232R + PIC16F886) on the FTDI VCP COM port
 *   NetSID      a Network SID Device server over TCP (SID-Device, JSIDPlay2, ...)
 *
 * SIDPLAY resolves only the old HardSID ISA API, all __stdcall:
 *   BYTE GetHardSIDCount(void)
 *   void WriteToHardSID(BYTE dev, BYTE reg, BYTE data)
 *   BYTE ReadFromHardSID(BYTE dev, BYTE reg)
 *   void MuteHardSID_Line(BOOL mute)
 * Writes arrive in real time from SIDPLAY's own 6510 emulation. Each one is
 * queued with its QueryPerformanceCounter stamp, and a writer thread hands
 * whatever has accumulated to the backend: the SIDBlaster gets it in one
 * WriteFile; NetSID turns the stamps into C64-cycle delays, so the server
 * replays the writes in time, a fixed lead behind.
 *
 * Config: SIDBLAST.INI next to the DLL   Log: SIDBLAST.LOG next to the DLL
 */
#include "sbcommon.h"
#include "netsid_net.h"
#include "minifmt.h"

#define QEV     4096            /* queued events, power of two */
#define QMASK   (QEV - 1)
#define TAKE    128             /* events per backend call (2 KB on the stack) */

enum { EV_WRITE, EV_MUTE, EV_UNMUTE };
typedef struct {
    LONGLONG t;                 /* QPC when the API call made it */
    BYTE type, reg, val;
} ev_t;

enum { BE_SIDBLASTER, BE_NETSID };

static HINSTANCE g_hinst;
static char      g_dir[MAX_PATH];
static char      g_ini[MAX_PATH + 16];
static int       g_backend = BE_SIDBLASTER;
static int       g_loglevel = 0;
static int       g_priority = THREAD_PRIORITY_TIME_CRITICAL;
/* SIDBlaster */
static char      g_port_name[64];
static DWORD     g_c64clk = 985248;    /* clock the tune was written for (PAL) */
static DWORD     g_sidclk = 1000000;   /* clock the SIDBlaster feeds its SID   */
static DWORD     g_ratio_q16 = 65536;  /* g_c64clk / g_sidclk in 16.16 fixed   */
/* NetSID */
static char      g_host[128];
static WORD      g_netport = NS_DEFAULT_PORT;
static DWORD     g_lead_ms = 100;
static int       g_model = 0;
static int       g_ntsc = 0;
static DWORD     g_min_spacing = 0;

static HANDLE    g_log = INVALID_HANDLE_VALUE;
static DWORD     g_t0;
/* Locks. g_wlock: API-side state (shadow, mute) and the order its events
   enter the queue. g_qlock: the queue. g_plock: the device (COM port or
   socket) and the backend's state. g_mlock: the writer's log messages.
   Nesting is only ever wlock -> qlock, and plock -> qlock. */
static CRITICAL_SECTION g_loglock, g_initlock, g_qlock, g_plock, g_wlock, g_mlock;

static int       g_inited, g_ok, g_muted;
static DWORD     g_open_tick;          /* last backend open attempt */
static HANDLE    g_evt, g_thr;
static DWORD     g_writer_tid;
static HANDLE    g_logevt, g_logthr;   /* logger (Log>=1 only) */
static volatile LONG g_stop;
static int       g_exiting;           /* in DLL_PROCESS_DETACH: take no locks */

static ev_t      g_q[QEV];
static DWORD     g_head, g_tail;       /* head = write pos, tail = read pos */
static BYTE      g_shadow[32];         /* what the tune wrote, for reads of $00-$18 */

static DWORD     g_qpf;                /* QPC ticks per second (0 if it doesn't fit 32 bits) */
static DWORD     g_qpf_khz;            /* QPC ticks per ms */

/* Stats. The writer thread only counts; once a second it copies the counters
   into g_snap and wakes the low-priority logger, which formats and writes the
   line. A disk write can block for a long time on Win98, and must never
   happen on the time-critical thread. All formatting is minifmt: USER32's
   wsprintf can take the Win16Mutex, and a low-priority logger preempted
   while holding it would hold up every thread that needs it, SIDPLAY's
   included. The DLL imports nothing from USER32. */
typedef struct {
    DWORD writes, batches, maxbatch, maxlat_us, maxwr_us, qmax, full, wrfail, reads, readfail;
    DWORD pkts, busy, maxrtt_us, primes, dropped, errors, up;   /* NetSID */
    DWORD ms;                          /* interval covered */
} stats_t;
static volatile LONG s_writes;          /* interlocked: API thread      */
static DWORD s_qmax, s_full;            /* under g_qlock                */
static DWORD s_batches, s_maxbatch, s_maxlat_us, s_maxwr_us, s_wrfail;  /* under g_plock */
static DWORD s_reads, s_readfail, s_dropped;                            /* under g_plock */
static stats_t g_snap;                  /* owned by the logger while g_snap_ready */
static volatile LONG g_snap_ready;

/* Log lines from the writer thread: formatted with minifmt (no USER32) into
   a ring, written to disk by the logger thread. */
#define MSGS    32
#define MSGLEN  200
static char  g_msg[MSGS][MSGLEN];
static DWORD g_msg_tick[MSGS];
static DWORD g_msg_head, g_msg_tail, g_msg_lost;   /* under g_mlock */

/* ---------------------------------------------------------------- log */
static void log_write(DWORD tick, const char *msg, DWORD len)
{
    char buf[MSGLEN + 16];
    DWORD n, done;
    n = mf_fmt(buf, 16, "[%7lu] ", tick - g_t0);
    if (len > MSGLEN) len = MSGLEN;
    memcpy(buf + n, msg, len);
    n += len;
    buf[n++] = '\r'; buf[n++] = '\n';
    if (g_exiting) { WriteFile(g_log, buf, n, &done, NULL); return; }
    EnterCriticalSection(&g_loglock);
    WriteFile(g_log, buf, n, &done, NULL);
    LeaveCriticalSection(&g_loglock);
}

static void logf(const char *fmt, ...)
{
    char buf[1100];
    DWORD n, done;
    va_list ap;
    if (g_log == INVALID_HANDLE_VALUE) return;
    n = mf_fmt(buf, 16, "[%7lu] ", GetTickCount() - g_t0);
    va_start(ap, fmt);
    n += mf_vfmt(buf + n, sizeof buf - n - 2, fmt, ap);
    va_end(ap);
    buf[n++] = '\r'; buf[n++] = '\n';
    if (g_exiting) { WriteFile(g_log, buf, n, &done, NULL); return; }
    EnterCriticalSection(&g_loglock);
    WriteFile(g_log, buf, n, &done, NULL);
    LeaveCriticalSection(&g_loglock);
}

/* Safe on the writer thread: memory only. */
static void wlogf(const char *fmt, ...)
{
    va_list ap;
    DWORD slot;
    if (g_log == INVALID_HANDLE_VALUE) return;
    if (g_exiting) {
        char buf[MSGLEN];
        va_start(ap, fmt);
        log_write(GetTickCount(), buf, mf_vfmt(buf, sizeof buf, fmt, ap));
        va_end(ap);
        return;
    }
    EnterCriticalSection(&g_mlock);
    if (g_msg_head - g_msg_tail >= MSGS) {
        g_msg_lost++;
        LeaveCriticalSection(&g_mlock);
        return;
    }
    slot = g_msg_head % MSGS;
    g_msg_tick[slot] = GetTickCount();
    va_start(ap, fmt);
    mf_vfmt(g_msg[slot], MSGLEN, fmt, ap);
    va_end(ap);
    g_msg_head++;
    LeaveCriticalSection(&g_mlock);
    if (g_logevt) SetEvent(g_logevt);
}

/* netsid_net logs through this. Once the logger thread runs, every line goes
   through the ring: callers may hold g_plock, which the writer waits on, so
   no disk I/O here. Before that (init), and at exit, write directly. */
static void nlog(const char *fmt, ...)
{
    char buf[MSGLEN];
    va_list ap;
    if (g_log == INVALID_HANDLE_VALUE) return;
    va_start(ap, fmt);
    mf_vfmt(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (g_logthr && !g_exiting) wlogf("%s", buf);
    else log_write(GetTickCount(), buf, lstrlenA(buf));
}

/* logger thread (and process exit): write out what the writer posted */
static void flush_msgs(void)
{
    char buf[MSGLEN];
    DWORD tick, lost;
    for (;;) {
        if (!g_exiting) EnterCriticalSection(&g_mlock);
        if (g_msg_head == g_msg_tail) {
            lost = g_msg_lost;
            g_msg_lost = 0;
            if (!g_exiting) LeaveCriticalSection(&g_mlock);
            if (lost) logf("(%lu log lines lost)", lost);
            return;
        }
        lstrcpynA(buf, g_msg[g_msg_tail % MSGS], MSGLEN);
        tick = g_msg_tick[g_msg_tail % MSGS];
        g_msg_tail++;
        if (!g_exiting) LeaveCriticalSection(&g_mlock);
        log_write(tick, buf, lstrlenA(buf));
    }
}

static DWORD ticks_to_us(LONGLONG d)
{
    DWORD t;
    if (d < 0) return 0;
    if (d > 4000000) d = 4000000;
    t = (DWORD)d;
    if (!g_qpf_khz) return 0;
    return t * 1000 / g_qpf_khz;
}

/* ---------------------------------------------------------- PAL pitch */
/* a/b in 16.16 fixed point, by long division (no 64-bit divide helper). */
static DWORD q16_ratio(DWORD a, DWORD b)
{
    DWORD q = a / b, r = a % b;
    int i;
    for (i = 0; i < 16; i++) {
        r <<= 1;
        q <<= 1;
        if (r >= b) { r -= b; q |= 1; }
    }
    return q;
}

/* The tune's 16-bit frequency, retuned for the SIDBlaster's clock:
   f_out = F * sidclk / 2^24, so F' = F * c64clk / sidclk keeps the pitch. */
static DWORD scale_freq(DWORD f)
{
    unsigned long long v = (unsigned long long)f * g_ratio_q16 + 0x8000;   /* mul only */
    DWORD r = (DWORD)(v >> 16);
    return r > 0xFFFF ? 0xFFFF : r;
}

/* reg is a frequency lo/hi register ($00/$01, $07/$08, $0E/$0F)? -> voice, else -1 */
static int freq_voice(BYTE reg)
{
    if (reg > 0x0F) return -1;
    if (reg % 7 > 1) return -1;
    return reg / 7;
}

/* ------------------------------------------------------------- config */
static void port_found(const char *port, const char *keypath, void *ctx)
{
    char *dst = ctx;
    logf("registry: %s -> %s", keypath, port);
    if (!dst[0]) lstrcpynA(dst, port, 63);
}

static void load_config(void)
{
    char *p, s[16];
    int pr;
    GetModuleFileNameA(g_hinst, g_dir, sizeof g_dir);
    for (p = g_dir + lstrlenA(g_dir); p > g_dir && p[-1] != '\\'; p--) ;
    *p = 0;
    lstrcpyA(g_ini, g_dir); lstrcatA(g_ini, "SIDBLAST.INI");
    g_loglevel = GetPrivateProfileIntA("SIDBlaster", "Log", 0, g_ini);
    pr = GetPrivateProfileIntA("SIDBlaster", "Priority", 15, g_ini);
    g_priority = (pr == 15) ? THREAD_PRIORITY_TIME_CRITICAL :
                 (pr == 2)  ? THREAD_PRIORITY_HIGHEST :
                 (pr == 1)  ? THREAD_PRIORITY_ABOVE_NORMAL : THREAD_PRIORITY_NORMAL;
    GetPrivateProfileStringA("SIDBlaster", "Backend", "SIDBlaster", s, sizeof s, g_ini);
    g_backend = lstrcmpiA(s, "NetSID") == 0 ? BE_NETSID : BE_SIDBLASTER;

    GetPrivateProfileStringA("SIDBlaster", "Port", "", g_port_name, sizeof g_port_name, g_ini);
    GetPrivateProfileStringA("SIDBlaster", "Clock", "PAL", s, sizeof s, g_ini);
    if (lstrcmpiA(s, "NTSC") == 0)     g_c64clk = 1022727;
    else if (lstrcmpiA(s, "OFF") == 0) g_c64clk = 0;        /* no correction */
    else                               g_c64clk = 985248;
    g_ntsc = g_c64clk == 1022727;                            /* NetSID: OFF = PAL */
    g_sidclk = GetPrivateProfileIntA("SIDBlaster", "SidClock", 1000000, g_ini);
    if (g_sidclk < 500000 || g_sidclk > 2000000) g_sidclk = 1000000;
    g_ratio_q16 = g_c64clk ? q16_ratio(g_c64clk, g_sidclk) : 65536;

    GetPrivateProfileStringA("SIDBlaster", "Host", "", g_host, sizeof g_host, g_ini);
    pr = (int)GetPrivateProfileIntA("SIDBlaster", "NetPort", NS_DEFAULT_PORT, g_ini);
    g_netport = (WORD)(pr > 0 && pr < 65536 ? pr : NS_DEFAULT_PORT);
    g_lead_ms = GetPrivateProfileIntA("SIDBlaster", "Lead", 100, g_ini);
    if (g_lead_ms < 10) g_lead_ms = 10;
    if (g_lead_ms > 1000) g_lead_ms = 1000;
    g_model = (int)GetPrivateProfileIntA("SIDBlaster", "Model", 0, g_ini);
    if (g_model < 0 || g_model > 65535) g_model = 0;
    g_min_spacing = GetPrivateProfileIntA("SIDBlaster", "MinSpacing", 0, g_ini);
    if (g_min_spacing > 1000) g_min_spacing = 1000;
}

static void open_log(void)
{
    char path[MAX_PATH + 16];
    if (g_loglevel <= 0) return;
    lstrcpyA(path, g_dir); lstrcatA(path, "SIDBLAST.LOG");
    g_log = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_log != INVALID_HANDLE_VALUE) SetFilePointer(g_log, 0, NULL, FILE_END);
}

/* -------------------------------------------------------------- queue */
static DWORD q_used(void) { return (g_head - g_tail) & QMASK; }

static void q_put(BYTE type, BYTE reg, BYTE val)
{
    DWORD used, full = 0;
    ev_t *e;
    LARGE_INTEGER now;
    for (;;) {
        EnterCriticalSection(&g_qlock);
        used = q_used();
        if (used + 1 < QEV) break;
        LeaveCriticalSection(&g_qlock);
        full++;
        SetEvent(g_evt);
        Sleep(1);
    }
    s_full += full;
    QueryPerformanceCounter(&now);     /* under the lock: stamps stay in queue order */
    e = &g_q[g_head];
    e->t = now.QuadPart;
    e->type = type; e->reg = reg; e->val = val;
    g_head = (g_head + 1) & QMASK;
    used++;
    if (used > s_qmax) s_qmax = used;
    LeaveCriticalSection(&g_qlock);
    SetEvent(g_evt);
}

static DWORD q_take(ev_t *dst, DWORD max)
{
    DWORD n = 0;
    EnterCriticalSection(&g_qlock);
    while (n < max && g_tail != g_head) {
        dst[n++] = g_q[g_tail];
        g_tail = (g_tail + 1) & QMASK;
    }
    LeaveCriticalSection(&g_qlock);
    return n;
}

static void note_latency(LONGLONG since)
{
    LARGE_INTEGER now;
    DWORD lat;
    QueryPerformanceCounter(&now);
    lat = ticks_to_us(now.QuadPart - since);
    if (lat > s_maxlat_us) s_maxlat_us = lat;
}

/* ------------------------------------------------ backend: SIDBlaster */
static HANDLE g_port = INVALID_HANDLE_VALUE;
static BYTE   sbs_shadow[32];     /* the register state the chip was sent, unscaled */
static int    sbs_muted;
static DWORD  sbs_sent_freq[3];   /* last scaled freq sent per voice */

static BOOL sbs_open(void)
{
    DWORD err, baud;
    BOOL setok;

    if (!g_port_name[0]) {
        char found[64]; found[0] = 0;
        logf("init: no Port= in %s, scanning registry for FTDI ports", g_ini);
        sb_scan_ports(port_found, found);
        if (found[0]) lstrcpyA(g_port_name, found);
    }
    if (!g_port_name[0]) {
        logf("init: FAILED - no port. Set Port=COMx under [SIDBlaster] in %s", g_ini);
        return FALSE;
    }
    g_port = sb_open_port(g_port_name, &err, &baud, &setok);
    if (g_port == INVALID_HANDLE_VALUE) {
        logf("init: FAILED to open %s (error %lu)", g_port_name, err);
        return FALSE;
    }
    logf("init: opened %s, SetCommState %s (err %lu), driver reports %lu baud",
         g_port_name, setok ? "ok" : "FAILED", err, baud);
    if (baud != SB_BAUD) logf("init: WARNING baud readback is not %lu", (DWORD)SB_BAUD);

    sb_reset(g_port);
    sbs_sent_freq[0] = sbs_sent_freq[1] = sbs_sent_freq[2] = 0xFFFFFFFF;
    if (g_c64clk)
        logf("init: pitch correction %lu Hz tune clock on %lu Hz SID clock (ratio %lu/65536)",
             g_c64clk, g_sidclk, g_ratio_q16);
    else
        logf("init: pitch correction off (SID clock %lu Hz)", g_sidclk);
    return TRUE;
}

/* Writer thread, under g_plock. Retunes frequencies and applies the line
   mute (volume nibble), then sends the batch in one WriteFile. */
static void sbs_send(const ev_t *ev, DWORD n)
{
    BYTE buf[TAKE * 4];
    DWORD k = 0, i;
    LARGE_INTEGER t0, t1;

    for (i = 0; i < n; i++) {
        const ev_t *e = &ev[i];
        BYTE reg = e->reg, data = e->val;
        if (e->type != EV_WRITE) {
            /* No mute relay on the SIDBlaster: emulate with the master volume nibble. */
            sbs_muted = e->type == EV_MUTE;
            buf[k++] = SB_HDR_WRITE | 0x18;
            buf[k++] = (BYTE)(sbs_muted ? (sbs_shadow[0x18] & 0xF0) : sbs_shadow[0x18]);
            continue;
        }
        sbs_shadow[reg] = data;
        if (g_ratio_q16 != 65536) {
            int v = freq_voice(reg);
            if (v >= 0) {
                /* retune: send whichever bytes of the scaled 16-bit value changed */
                BYTE base = (BYTE)(v * 7);
                DWORD f = scale_freq(sbs_shadow[base] | ((DWORD)sbs_shadow[base + 1] << 8));
                DWORD old = sbs_sent_freq[v];
                if (old > 0xFFFF || (f & 0xFF) != (old & 0xFF)) {
                    buf[k++] = (BYTE)(SB_HDR_WRITE | base);
                    buf[k++] = (BYTE)(f & 0xFF);
                }
                if (old > 0xFFFF || (f >> 8) != ((old >> 8) & 0xFF)) {
                    buf[k++] = (BYTE)(SB_HDR_WRITE | (base + 1));
                    buf[k++] = (BYTE)(f >> 8);
                }
                sbs_sent_freq[v] = f;
                continue;
            }
        }
        if (reg == 0x18 && sbs_muted) data &= 0xF0;
        buf[k++] = (BYTE)(SB_HDR_WRITE | reg);
        buf[k++] = data;
    }
    if (!k) return;
    QueryPerformanceCounter(&t0);
    if (!sb_send(g_port, buf, k)) s_wrfail++;
    QueryPerformanceCounter(&t1);
    s_batches++;
    if (k > s_maxbatch) s_maxbatch = k;
    { DWORD lat = ticks_to_us(t0.QuadPart - ev[0].t);
      DWORD wr  = ticks_to_us(t1.QuadPart - t0.QuadPart);
      if (lat > s_maxlat_us) s_maxlat_us = lat;
      if (wr  > s_maxwr_us)  s_maxwr_us  = wr; }
}

static BOOL sbs_read(BYTE reg, LONGLONG t, BYTE *val)
{
    (void)t;
    return sb_read(g_port, reg, val);
}

static BOOL sbs_present(void) { return TRUE; }

static void sbs_close(void)
{
    CloseHandle(g_port);
    g_port = INVALID_HANDLE_VALUE;
}

/* process exit: other threads are gone, silence the chip directly */
static void sbs_detach(void)
{
    static const BYTE quiet[] = { SB_HDR_WRITE|0x04, 0, SB_HDR_WRITE|0x0B, 0,
                                  SB_HDR_WRITE|0x12, 0, SB_HDR_WRITE|0x18, 0 };
    if (g_port == INVALID_HANDLE_VALUE) return;
    sb_send(g_port, quiet, sizeof quiet);
    CloseHandle(g_port);
}

/* ---------------------------------------------------- backend: NetSID */
static ns_link  g_link;           /* .s = INVALID_SOCKET from DllMain on */
static ns_sched g_sched;
static DWORD    g_try_tick;       /* last connect attempt, for rate limiting */
static BYTE     net_shadow[32];   /* what SIDPLAY wrote, as the writer has seen it */
static int      net_resync;       /* server lost its state: send net_shadow first */

/* under g_plock (or g_initlock before the writer exists) */
static BOOL net_connect(void)
{
    ns_setup su;
    g_try_tick = GetTickCount();
    su.model = g_model;
    su.ntsc = g_ntsc;
    if (!ns_link_open(&g_link, g_host, g_netport, &su, 500)) return FALSE;
    ns_sched_restart(&g_sched);
    g_sched.nvoices = g_link.version >= 3 ? 4 : 3;
    net_resync = 1;
    return TRUE;
}

static BOOL net_open(void)
{
    if (!g_host[0]) {
        logf("init: FAILED - Backend=NetSID needs Host= (the server's IP address) in %s", g_ini);
        return FALSE;
    }
    if (!g_qpf) {
        logf("init: FAILED - QueryPerformanceCounter runs too fast for NetSID");
        return FALSE;
    }
    if (!ns_net_startup(nlog)) return FALSE;
    ns_link_init(&g_link, nlog, g_qpf_khz);
    g_link.read_timeout_ms = 500 + 2 * g_lead_ms;
    ns_sched_init(&g_sched, g_ntsc ? NS_NTSC_HZ : NS_PAL_HZ, g_qpf, g_lead_ms, g_min_spacing,
                  ns_link_send, &g_link);
    logf("init: NetSID server %s:%u, %s clock, lead %lu ms, min spacing %lu cycles",
         g_host, g_netport, g_ntsc ? "NTSC" : "PAL", g_lead_ms, g_min_spacing);
    return net_connect();
}

static void net_down(void)
{
    ns_sched_restart(&g_sched);
    wlogf("netsid: dropping writes; trying to reconnect every 5 s while SIDPLAY plays");
}

/* After a FLUSH or a reconnect the server's registers aren't what SIDPLAY
   last wrote: send them all, stamped t, ahead of the next event. */
static int net_resync_at(LONGLONG t)
{
    BYTE r;
    if (!net_resync) return 1;
    net_resync = 0;
    for (r = 0; r <= 0x18; r++)
        if (!ns_sched_write(&g_sched, t, r, net_shadow[r])) return 0;
    return 1;
}

/* Writer thread, under g_plock.
   Mute(1) starts every tune: FLUSH drops the previous tune's queued tail and
   TRY_RESET silences the SID, so the new one starts exactly one lead behind (adding a lead on top of what
   is still queued would double it, up to the server's limit). The register
   state is re-sent afterwards, so a Mute(1) mid-tune loses nothing.      */
static void net_send(const ev_t *ev, DWORD n)
{
    DWORD i;
    int ok = 1;
    if (g_link.s != INVALID_SOCKET) note_latency(ev[0].t);
    for (i = 0; i < n; i++) {
        const ev_t *e = &ev[i];
        if (e->type == EV_WRITE) net_shadow[e->reg] = e->val;
        if (g_link.s == INVALID_SOCKET) {
            /* Retry while SIDPLAY is playing, at most every 5 s (a connect to a
               vanished host can take 500 ms). A reconnect mid-tune resumes it:
               the register state is re-sent before the next write. */
            if (GetTickCount() - g_try_tick >= 5000) {
                wlogf("netsid: reconnecting to %s:%u", g_host, (unsigned)g_netport);
                net_connect();
            }
            if (g_link.s == INVALID_SOCKET) {
                if (e->type == EV_WRITE) s_dropped++;
                continue;
            }
        }
        switch (e->type) {
        case EV_WRITE:
            ok = net_resync_at(e->t) && ns_sched_write(&g_sched, e->t, e->reg, e->val);
            break;
        case EV_MUTE:
            ok = ns_sched_drop(&g_sched) && ns_sched_mute(&g_sched, 1);
            net_resync = 1;
            break;
        case EV_UNMUTE:
            ok = ns_sched_mute(&g_sched, 0) && ns_sched_prime(&g_sched, e->t) && net_resync_at(e->t);
            break;
        }
        if (!ok) {
            if (e->type == EV_WRITE) s_dropped++;
            net_down();
        }
    }
    if (g_link.s != INVALID_SOCKET && !ns_sched_flush(&g_sched)) net_down();
    s_batches++;
}

static BOOL net_read(BYTE reg, LONGLONG t, BYTE *val)
{
    if (g_link.s == INVALID_SOCKET) return FALSE;
    if (net_resync_at(t) && ns_sched_read(&g_sched, t, reg, val)) return TRUE;
    net_down();
    return FALSE;
}

/* GetHardSIDCount: reconnect now if the link is down (at most every 2 s) */
static BOOL net_present(void)
{
    BOOL up;
    EnterCriticalSection(&g_plock);
    if (g_link.s == INVALID_SOCKET && GetTickCount() - g_try_tick >= 2000) net_connect();
    up = g_link.s != INVALID_SOCKET;
    LeaveCriticalSection(&g_plock);
    return up;
}

static void net_close(void) { ns_link_close(&g_link); }

/* Best effort. WSOCK32 was loaded after us, so at process exit it has had
   its own DLL_PROCESS_DETACH first; calls into it may just fail, and every
   wait is bounded. The server notices the closed socket either way. */
static void net_detach(void)
{
    if (g_link.s != INVALID_SOCKET) logf("netsid: flushing and resetting the server");
    ns_link_goodbye(&g_link);
}

/* ------------------------------------------------------------ backends */
typedef struct {
    const char *name;
    BOOL (*open)(void);           /* in init, under g_initlock */
    void (*send)(const ev_t *, DWORD);   /* writer thread, under g_plock */
    BOOL (*read)(BYTE reg, LONGLONG t, BYTE *val);   /* g_plock, queue drained */
    BOOL (*present)(void);        /* GetHardSIDCount, after init */
    void (*close)(void);          /* init failed after open */
    void (*detach)(void);         /* process exit: no locks, bounded time */
    int  retry_open;              /* GetHardSIDCount retries a failed open */
} backend_t;

static const backend_t g_backends[] = {
    { "SIDBlaster", sbs_open, sbs_send, sbs_read, sbs_present, sbs_close, sbs_detach, 0 },
    { "NetSID",     net_open, net_send, net_read, net_present, net_close, net_detach, 1 },
};
static const backend_t *g_be = &g_backends[0];

/* -------------------------------------------------------------- writer */
/* caller must hold g_plock */
static void drain_locked(void)
{
    ev_t buf[TAKE];
    DWORD n;
    while ((n = q_take(buf, TAKE)) != 0) g_be->send(buf, n);
}

/* caller must hold g_plock; takes g_qlock briefly. Memory only, no I/O.
   If the logger hasn't taken the last snapshot yet, the counters just keep
   running and the next snapshot covers the longer interval. */
static BOOL snapshot_locked(DWORD ms)
{
    stats_t *s = &g_snap;
    if (g_snap_ready) return FALSE;
    s->writes = (DWORD)InterlockedExchange((LONG *)&s_writes, 0);
    s->batches = s_batches; s->maxbatch = s_maxbatch; s->maxlat_us = s_maxlat_us;
    s->maxwr_us = s_maxwr_us; s->wrfail = s_wrfail; s->reads = s_reads; s->readfail = s_readfail;
    s->dropped = s_dropped;
    s_batches = s_maxbatch = s_maxlat_us = s_maxwr_us = s_wrfail = s_reads = s_readfail = 0;
    s_dropped = 0;
    if (g_backend == BE_NETSID) {
        s->pkts = g_link.pkts; s->busy = g_link.busy; s->maxrtt_us = g_link.max_rtt_us;
        s->errors = g_link.errors; s->primes = g_sched.primes;
        s->up = g_link.s != INVALID_SOCKET;
        g_link.pkts = g_link.busy = g_link.max_rtt_us = g_link.errors = 0;
        g_sched.primes = 0;
    }
    EnterCriticalSection(&g_qlock);
    s->qmax = s_qmax; s->full = s_full;
    s_qmax = s_full = 0;
    LeaveCriticalSection(&g_qlock);
    s->ms = ms;
    InterlockedExchange((LONG *)&g_snap_ready, 1);   /* also a barrier: g_snap first */
    SetEvent(g_logevt);
    return TRUE;
}

static DWORD WINAPI writer_thread(LPVOID unused)
{
    DWORD last = GetTickCount(), now;
    (void)unused;
    while (!g_stop) {
        WaitForSingleObject(g_evt, 100);
        if (g_stop) break;
        EnterCriticalSection(&g_plock);
        drain_locked();
        if (g_logthr && (now = GetTickCount()) - last >= 1000 && snapshot_locked(now - last))
            last = now;
        LeaveCriticalSection(&g_plock);
    }
    return 0;
}

static void print_stats(const stats_t *s, const char *note)
{
    if (g_backend == BE_NETSID) {
        if (s->writes || s->reads || s->full || s->busy || s->dropped || s->errors || s->primes)
            logf("stats/%lums: writes=%lu wakeups=%lu pkts=%lu maxlat=%luus maxrtt=%luus "
                 "busy=%lu primes=%lu qmax=%lu qfull=%lu dropped=%lu errors=%lu "
                 "reads=%lu readfail=%lu%s%s",
                 s->ms, s->writes, s->batches, s->pkts, s->maxlat_us, s->maxrtt_us,
                 s->busy, s->primes, s->qmax, s->full, s->dropped, s->errors,
                 s->reads, s->readfail, s->up ? "" : " LINK DOWN", note);
    } else if (s->writes || s->reads || s->full || s->wrfail) {
        logf("stats/%lums: writes=%lu batches=%lu maxbatch=%luB maxlat=%luus maxWriteFile=%luus "
             "qmax=%lu qfull=%lu wrfail=%lu reads=%lu readfail=%lu%s",
             s->ms, s->writes, s->batches, s->maxbatch, s->maxlat_us, s->maxwr_us,
             s->qmax, s->full, s->wrfail, s->reads, s->readfail, note);
    }
}

/* Low priority: writes the writer's log lines and its stats snapshots. */
static DWORD WINAPI log_thread(LPVOID unused)
{
    stats_t s;
    (void)unused;
    while (!g_stop) {
        WaitForSingleObject(g_logevt, INFINITE);
        if (g_stop) break;
        flush_msgs();
        if (!g_snap_ready) continue;
        s = g_snap;
        InterlockedExchange((LONG *)&g_snap_ready, 0);   /* barrier: copy first */
        print_stats(&s, "");
    }
    return 0;
}

/* --------------------------------------------------------------- init */
/* under g_initlock: open the backend, then start the threads (once) */
static void try_open(void)
{
    DWORD tid;
    char self[MAX_PATH];

    g_open_tick = GetTickCount();
    if (!g_be->open()) return;

    if (!g_thr) {
        g_evt = CreateEventA(NULL, FALSE, FALSE, NULL);
        if (g_evt) g_thr = CreateThread(NULL, 0, writer_thread, NULL, 0, &g_writer_tid);
        if (!g_evt || !g_thr) {
            logf("init: FAILED to create writer thread (error %lu)", GetLastError());
            g_be->close();
            return;
        }
        SetThreadPriority(g_thr, g_priority);

        if (g_loglevel >= 1) {
            g_logevt = CreateEventA(NULL, FALSE, FALSE, NULL);
            if (g_logevt) g_logthr = CreateThread(NULL, 0, log_thread, NULL, 0, &tid);
            if (g_logthr) SetThreadPriority(g_logthr, THREAD_PRIORITY_BELOW_NORMAL);
            else logf("init: no stats logger thread (error %lu)", GetLastError());
        }

        /* Pin ourselves: SIDPLAY FreeLibrary()s the DLL when it drops the device,
           and the writer thread must never run inside an unloaded image. */
        GetModuleFileNameA(g_hinst, self, sizeof self);
        LoadLibraryA(self);
    }
    g_ok = 1;
    logf("init: ready");
}

static void sb_init(void)
{
    LARGE_INTEGER f;

    EnterCriticalSection(&g_initlock);
    if (g_inited) { LeaveCriticalSection(&g_initlock); return; }
    g_inited = 1;

    if (QueryPerformanceFrequency(&f) && f.HighPart == 0) {
        g_qpf = f.LowPart;
        g_qpf_khz = f.LowPart / 1000;
    }
    logf("init: %s backend, QPC %lu kHz, log level %d, priority %d",
         g_be->name, g_qpf_khz, g_loglevel, g_priority);
    try_open();
    LeaveCriticalSection(&g_initlock);
}

/* ------------------------------------------------------------ exports */
__declspec(dllexport) BYTE WINAPI GetHardSIDCount(void)
{
    BYTE n;
    sb_init();
    if (!g_ok && g_be->retry_open) {
        EnterCriticalSection(&g_initlock);
        if (!g_ok && GetTickCount() - g_open_tick >= 2000) {
            logf("init: trying again");
            try_open();
        }
        LeaveCriticalSection(&g_initlock);
    }
    n = (BYTE)(g_ok && g_be->present() ? 1 : 0);
    if (g_loglevel >= 1) logf("GetHardSIDCount -> %d", n);
    return n;
}

__declspec(dllexport) void WINAPI InitHardSID_Mapper(void)
{
    sb_init();
    if (g_loglevel >= 1) logf("InitHardSID_Mapper");
}

__declspec(dllexport) void WINAPI WriteToHardSID(BYTE dev, BYTE reg, BYTE data)
{
    reg &= 0x1F;
    if (!g_inited) sb_init();
    if (g_loglevel >= 2) logf("W dev=%u $%02X=%02X", dev, reg, data);
    if (reg > 0x18) return;                 /* $19-$1C are read-only */
    EnterCriticalSection(&g_wlock);
    g_shadow[reg] = data;
    if (g_ok) {
        InterlockedIncrement((LONG *)&s_writes);
        q_put(EV_WRITE, reg, data);
    }
    LeaveCriticalSection(&g_wlock);
}

__declspec(dllexport) BYTE WINAPI ReadFromHardSID(BYTE dev, BYTE reg)
{
    BYTE v;
    reg &= 0x1F;
    if (!g_inited) sb_init();
    if (reg <= 0x18 || !g_ok) {
        /* write-only on a real SID; HardSID callers expect the last written value */
        v = g_shadow[reg];
    } else {
        LARGE_INTEGER t;
        EnterCriticalSection(&g_plock);
        drain_locked();
        QueryPerformanceCounter(&t);
        s_reads++;
        if (!g_be->read(reg, t.QuadPart, &v)) { v = 0; s_readfail++; }
        LeaveCriticalSection(&g_plock);
    }
    if (g_loglevel >= 2) logf("R dev=%u $%02X -> %02X", dev, reg, v);
    return v;
}

__declspec(dllexport) void WINAPI MuteHardSID_Line(BOOL mute)
{
    if (!g_inited) sb_init();
    if (g_loglevel >= 1) logf("MuteHardSID_Line(%d)", mute ? 1 : 0);
    EnterCriticalSection(&g_wlock);
    g_muted = mute ? 1 : 0;
    if (g_ok) q_put((BYTE)(g_muted ? EV_MUTE : EV_UNMUTE), 0, 0);
    LeaveCriticalSection(&g_wlock);
}

/* ------------------------------------------------------------ DllMain */
BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_hinst = h;
        g_t0 = GetTickCount();
        DisableThreadLibraryCalls(h);
        InitializeCriticalSection(&g_loglock);
        InitializeCriticalSection(&g_initlock);
        InitializeCriticalSection(&g_qlock);
        InitializeCriticalSection(&g_plock);
        InitializeCriticalSection(&g_wlock);
        InitializeCriticalSection(&g_mlock);
        g_link.s = INVALID_SOCKET;
        load_config();
        g_be = &g_backends[g_backend];
        open_log();
        logf("---- SIDBlaster HardSID shim " SB_VERSION " loaded from %s", g_dir);
        if (g_backend == BE_NETSID)
            logf("config: %s Backend=NetSID Host=%s NetPort=%u", g_ini,
                 g_host[0] ? g_host : "(none)", (unsigned)g_netport);
        else
            logf("config: %s Port=%s", g_ini, g_port_name[0] ? g_port_name : "(auto)");
    } else if (reason == DLL_PROCESS_DETACH) {
        g_exiting = 1;   /* a killed thread may still own one of our locks */
        g_stop = 1;
        if (g_log != INVALID_HANDLE_VALUE) {
            flush_msgs();
            /* a snapshot the logger never took: it got no CPU time for a while */
            if (g_snap_ready) print_stats(&g_snap, " (written at exit: the logger thread didn't get to it)");
        }
        if (g_ok) g_be->detach();
        logf("---- unloaded");
        if (g_log != INVALID_HANDLE_VALUE) CloseHandle(g_log);
    }
    return TRUE;
}
