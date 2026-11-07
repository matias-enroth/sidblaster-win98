/*
 * HardSID.dll replacement for SIDPLAY/Windows (Nov 2002) on Windows 98,
 * driving a SIDBlaster-USB (FT232R + PIC16F886) through the FTDI VCP COM port.
 *
 * SIDPLAY resolves only the old HardSID ISA API, all __stdcall:
 *   BYTE GetHardSIDCount(void)
 *   void WriteToHardSID(BYTE dev, BYTE reg, BYTE data)
 *   BYTE ReadFromHardSID(BYTE dev, BYTE reg)
 *   void MuteHardSID_Line(BOOL mute)
 * Writes arrive in real time from SIDPLAY's own 6510 emulation; we queue them
 * and a writer thread pushes whatever has accumulated in one WriteFile.
 *
 * Config: SIDBLAST.INI next to the DLL   Log: SIDBLAST.LOG next to the DLL
 */
#include "sbcommon.h"

#define QSIZE   16384           /* bytes, power of two */
#define QMASK   (QSIZE - 1)

static HINSTANCE g_hinst;
static char      g_dir[MAX_PATH];
static char      g_ini[MAX_PATH + 16];
static char      g_port_name[64];
static int       g_loglevel = 0;
static int       g_priority = THREAD_PRIORITY_TIME_CRITICAL;
static DWORD     g_c64clk = 985248;    /* clock the tune was written for (PAL) */
static DWORD     g_sidclk = 1000000;   /* clock the SIDBlaster feeds its SID   */
static DWORD     g_ratio_q16 = 65536;  /* g_c64clk / g_sidclk in 16.16 fixed   */

static HANDLE    g_log = INVALID_HANDLE_VALUE;
static DWORD     g_t0;
/* Locks. g_wlock: API-side register state (shadow, retuned freqs, mute) and
   the order its writes enter the queue. g_qlock: the queue. g_plock: the COM
   port. Nesting is only ever wlock -> qlock, and plock -> qlock. */
static CRITICAL_SECTION g_loglock, g_initlock, g_qlock, g_plock, g_wlock;

static HANDLE    g_port = INVALID_HANDLE_VALUE;
static int       g_inited, g_ok, g_muted;
static HANDLE    g_evt, g_thr;
static HANDLE    g_logevt, g_logthr;   /* stats logger (Log>=1 only) */
static volatile LONG g_stop;
static int       g_exiting;           /* in DLL_PROCESS_DETACH: take no locks */

static BYTE      g_q[QSIZE];
static DWORD     g_head, g_tail;       /* head = write pos, tail = read pos */
static LARGE_INTEGER g_first_enq;      /* when queue went non-empty */
static BYTE      g_shadow[32];         /* what the tune wrote (unscaled)       */
static DWORD     g_sent_freq[3];        /* last scaled freq sent per voice      */

static DWORD     g_qpf_khz;            /* QPC ticks per ms */

/* Stats. The writer thread only counts; once a second it copies the counters
   into g_snap and wakes the low-priority logger, which formats and writes the
   line. A disk write (or USER32's wsprintf) can block for a long time on
   Win98, and must never happen on the time-critical thread. */
typedef struct {
    DWORD writes, batches, maxbatch, maxlat_us, maxwr_us, qmax, full, wrfail, reads, readfail;
    DWORD ms;                          /* interval covered */
} stats_t;
static volatile LONG s_writes;          /* interlocked: API thread      */
static DWORD s_qmax, s_full;            /* under g_qlock                */
static DWORD s_batches, s_maxbatch, s_maxlat_us, s_maxwr_us, s_wrfail;  /* under g_plock */
static DWORD s_reads, s_readfail;       /* under g_plock                */
static stats_t g_snap;                  /* owned by the logger while g_snap_ready */
static volatile LONG g_snap_ready;

/* ---------------------------------------------------------------- log */
static void logf(const char *fmt, ...)
{
    char buf[1100];   /* wvsprintf writes at most 1024 */
    DWORD n, done;
    va_list ap;
    if (g_log == INVALID_HANDLE_VALUE) return;
    n = wsprintfA(buf, "[%7lu] ", GetTickCount() - g_t0);
    va_start(ap, fmt);
    n += wvsprintfA(buf + n, fmt, ap);
    va_end(ap);
    buf[n++] = '\r'; buf[n++] = '\n';
    if (g_exiting) { WriteFile(g_log, buf, n, &done, NULL); return; }
    EnterCriticalSection(&g_loglock);
    WriteFile(g_log, buf, n, &done, NULL);
    LeaveCriticalSection(&g_loglock);
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
    char *p;
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
    GetPrivateProfileStringA("SIDBlaster", "Port", "", g_port_name, sizeof g_port_name, g_ini);
    {
        char clk[16];
        GetPrivateProfileStringA("SIDBlaster", "Clock", "PAL", clk, sizeof clk, g_ini);
        if (lstrcmpiA(clk, "NTSC") == 0)     g_c64clk = 1022727;
        else if (lstrcmpiA(clk, "OFF") == 0) g_c64clk = 0;        /* no correction */
        else                                 g_c64clk = 985248;
    }
    g_sidclk = GetPrivateProfileIntA("SIDBlaster", "SidClock", 1000000, g_ini);
    if (g_sidclk < 500000 || g_sidclk > 2000000) g_sidclk = 1000000;
    g_ratio_q16 = g_c64clk ? q16_ratio(g_c64clk, g_sidclk) : 65536;
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

static void q_put2(BYTE a, BYTE b)
{
    DWORD used, full = 0;
    for (;;) {
        EnterCriticalSection(&g_qlock);
        used = q_used();
        if (used + 2 < QSIZE) break;
        LeaveCriticalSection(&g_qlock);
        full++;
        SetEvent(g_evt);
        Sleep(1);
    }
    s_full += full;
    if (used == 0) QueryPerformanceCounter(&g_first_enq);
    g_q[g_head] = a; g_head = (g_head + 1) & QMASK;
    g_q[g_head] = b; g_head = (g_head + 1) & QMASK;
    used += 2;
    if (used > s_qmax) s_qmax = used;
    LeaveCriticalSection(&g_qlock);
    SetEvent(g_evt);
}

/* take up to max bytes; returns count and enqueue time of the oldest byte */
static DWORD q_take(BYTE *dst, DWORD max, LARGE_INTEGER *since)
{
    DWORD n = 0;
    EnterCriticalSection(&g_qlock);
    *since = g_first_enq;
    while (n < max && g_tail != g_head) {
        dst[n++] = g_q[g_tail];
        g_tail = (g_tail + 1) & QMASK;
    }
    if (g_tail != g_head) QueryPerformanceCounter(&g_first_enq);  /* remainder: restart clock */
    LeaveCriticalSection(&g_qlock);
    return n;
}

/* caller must hold g_plock */
static void drain_locked(void)
{
    BYTE buf[2048];
    DWORD n;
    LARGE_INTEGER since, t0, t1;
    while ((n = q_take(buf, sizeof buf, &since)) != 0) {
        QueryPerformanceCounter(&t0);
        if (!sb_send(g_port, buf, n)) s_wrfail++;
        QueryPerformanceCounter(&t1);
        s_batches++;
        if (n > s_maxbatch) s_maxbatch = n;
        { DWORD lat = ticks_to_us(t0.QuadPart - since.QuadPart);
          DWORD wr  = ticks_to_us(t1.QuadPart - t0.QuadPart);
          if (lat > s_maxlat_us) s_maxlat_us = lat;
          if (wr  > s_maxwr_us)  s_maxwr_us  = wr; }
    }
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
    s_batches = s_maxbatch = s_maxlat_us = s_maxwr_us = s_wrfail = s_reads = s_readfail = 0;
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

/* Low priority: formats and writes the stats line the writer snapshotted. */
static DWORD WINAPI log_thread(LPVOID unused)
{
    stats_t s;
    (void)unused;
    while (!g_stop) {
        WaitForSingleObject(g_logevt, INFINITE);
        if (g_stop) break;
        if (!g_snap_ready) continue;
        s = g_snap;
        InterlockedExchange((LONG *)&g_snap_ready, 0);   /* barrier: copy first */
        if (s.writes || s.reads || s.full || s.wrfail)
            logf("stats/%lums: writes=%lu batches=%lu maxbatch=%luB maxlat=%luus maxWriteFile=%luus "
                 "qmax=%luB qfull=%lu wrfail=%lu reads=%lu readfail=%lu",
                 s.ms, s.writes, s.batches, s.maxbatch, s.maxlat_us, s.maxwr_us,
                 s.qmax, s.full, s.wrfail, s.reads, s.readfail);
    }
    return 0;
}

/* --------------------------------------------------------------- init */
static void sb_init(void)
{
    DWORD err, baud, tid;
    BOOL setok;
    LARGE_INTEGER f;
    char self[MAX_PATH];

    EnterCriticalSection(&g_initlock);
    if (g_inited) { LeaveCriticalSection(&g_initlock); return; }
    g_inited = 1;

    if (QueryPerformanceFrequency(&f) && f.HighPart == 0) g_qpf_khz = f.LowPart / 1000;
    logf("init: QPC %lu kHz, log level %d, priority %d", g_qpf_khz, g_loglevel, g_priority);

    if (!g_port_name[0]) {
        char found[64]; found[0] = 0;
        logf("init: no Port= in %s, scanning registry for FTDI ports", g_ini);
        sb_scan_ports(port_found, found);
        if (found[0]) lstrcpyA(g_port_name, found);
    }
    if (!g_port_name[0]) {
        logf("init: FAILED - no port. Set Port=COMx under [SIDBlaster] in %s", g_ini);
        LeaveCriticalSection(&g_initlock);
        return;
    }

    g_port = sb_open_port(g_port_name, &err, &baud, &setok);
    if (g_port == INVALID_HANDLE_VALUE) {
        logf("init: FAILED to open %s (error %lu)", g_port_name, err);
        LeaveCriticalSection(&g_initlock);
        return;
    }
    logf("init: opened %s, SetCommState %s (err %lu), driver reports %lu baud",
         g_port_name, setok ? "ok" : "FAILED", err, baud);
    if (baud != SB_BAUD) logf("init: WARNING baud readback is not %lu", (DWORD)SB_BAUD);

    sb_reset(g_port);
    g_sent_freq[0] = g_sent_freq[1] = g_sent_freq[2] = 0xFFFFFFFF;
    if (g_c64clk)
        logf("init: pitch correction %lu Hz tune clock on %lu Hz SID clock (ratio %lu/65536)",
             g_c64clk, g_sidclk, g_ratio_q16);
    else
        logf("init: pitch correction off (SID clock %lu Hz)", g_sidclk);

    g_evt = CreateEventA(NULL, FALSE, FALSE, NULL);
    g_thr = CreateThread(NULL, 0, writer_thread, NULL, 0, &tid);
    if (!g_evt || !g_thr) {
        logf("init: FAILED to create writer thread (error %lu)", GetLastError());
        CloseHandle(g_port); g_port = INVALID_HANDLE_VALUE;
        LeaveCriticalSection(&g_initlock);
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

    g_ok = 1;
    logf("init: ready");
    LeaveCriticalSection(&g_initlock);
}

/* ------------------------------------------------------------ exports */
__declspec(dllexport) BYTE WINAPI GetHardSIDCount(void)
{
    sb_init();
    if (g_loglevel >= 1) logf("GetHardSIDCount -> %d", g_ok ? 1 : 0);
    return (BYTE)(g_ok ? 1 : 0);
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
    if (!g_ok) { LeaveCriticalSection(&g_wlock); return; }
    InterlockedIncrement((LONG *)&s_writes);

    if (g_ratio_q16 != 65536) {
        int v = freq_voice(reg);
        if (v >= 0) {
            /* retune: send whichever bytes of the scaled 16-bit value changed */
            BYTE base = (BYTE)(v * 7);
            DWORD f = scale_freq(g_shadow[base] | ((DWORD)g_shadow[base + 1] << 8));
            DWORD old = g_sent_freq[v];
            if (old > 0xFFFF || (f & 0xFF) != (old & 0xFF))
                q_put2((BYTE)(SB_HDR_WRITE | base), (BYTE)(f & 0xFF));
            if (old > 0xFFFF || (f >> 8) != ((old >> 8) & 0xFF))
                q_put2((BYTE)(SB_HDR_WRITE | (base + 1)), (BYTE)(f >> 8));
            g_sent_freq[v] = f;
            LeaveCriticalSection(&g_wlock);
            return;
        }
    }
    if (reg == 0x18 && g_muted) data &= 0xF0;
    q_put2((BYTE)(SB_HDR_WRITE | reg), data);
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
        EnterCriticalSection(&g_plock);
        drain_locked();
        s_reads++;
        if (!sb_read(g_port, reg, &v)) { v = 0; s_readfail++; }
        LeaveCriticalSection(&g_plock);
    }
    if (g_loglevel >= 2) logf("R dev=%u $%02X -> %02X", dev, reg, v);
    return v;
}

__declspec(dllexport) void WINAPI MuteHardSID_Line(BOOL mute)
{
    if (!g_inited) sb_init();
    if (g_loglevel >= 1) logf("MuteHardSID_Line(%d)", mute ? 1 : 0);
    /* No mute relay on the SIDBlaster: emulate with the master volume nibble. */
    EnterCriticalSection(&g_wlock);
    g_muted = mute ? 1 : 0;
    if (g_ok)
        q_put2(SB_HDR_WRITE | 0x18, (BYTE)(g_muted ? (g_shadow[0x18] & 0xF0) : g_shadow[0x18]));
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
        load_config();
        open_log();
        logf("---- SIDBlaster HardSID shim " SB_VERSION " loaded from %s", g_dir);
        logf("config: %s Port=%s", g_ini, g_port_name[0] ? g_port_name : "(auto)");
    } else if (reason == DLL_PROCESS_DETACH) {
        g_exiting = 1;   /* a killed thread may still own one of our locks */
        g_stop = 1;
        if (g_port != INVALID_HANDLE_VALUE) {
            /* process exit: other threads are gone, silence the chip directly */
            static const BYTE quiet[] = { SB_HDR_WRITE|0x04, 0, SB_HDR_WRITE|0x0B, 0,
                                          SB_HDR_WRITE|0x12, 0, SB_HDR_WRITE|0x18, 0 };
            sb_send(g_port, quiet, sizeof quiet);
            CloseHandle(g_port);
        }
        logf("---- unloaded");
        if (g_log != INVALID_HANDLE_VALUE) CloseHandle(g_log);
    }
    return TRUE;
}
