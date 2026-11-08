/* HSPLAY.EXE - drives HardSID.dll the way SIDPLAY/Windows does, for testing
   under Wine against tools/netsid_mock.py (not shipped).
     HSPLAY [seconds per tune] [tunes]
   Each tune: Mute(1), $00-$18 = 0, Mute(0), then 50 Hz frames of 25 writes.
   The first tune pauses for a second halfway (read $18, write $18=0, resume).
   Ends with a few reads of $1B, then exits (the DLL says goodbye on detach). */
#include <windows.h>
#include <stdarg.h>

typedef BYTE (WINAPI *count_fn)(void);
typedef void (WINAPI *write_fn)(BYTE, BYTE, BYTE);
typedef BYTE (WINAPI *read_fn)(BYTE, BYTE);
typedef void (WINAPI *mute_fn)(BOOL);

static count_fn Count;
static write_fn W;
static read_fn  R;
static mute_fn  Mute;
static HANDLE   g_out;
static LONGLONG g_frame, g_next;     /* QPC ticks per 50 Hz frame */

void *memset(void *d, int c, unsigned int n)
{ unsigned char *p = d; while (n--) *p++ = (unsigned char)c; return d; }

static void out(const char *fmt, ...)
{
    char buf[1100];
    DWORD n, done;
    va_list ap;
    va_start(ap, fmt);
    n = wvsprintfA(buf, fmt, ap);
    va_end(ap);
    WriteFile(g_out, buf, n, &done, NULL);
}

static LONGLONG now(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }

/* wait for the next 50 Hz frame, like SIDPLAY's player timer */
static void next_frame(void)
{
    g_next += g_frame;
    while (now() < g_next) Sleep(1);
}

static void tune_start(void)
{
    BYTE r;
    Mute(TRUE);
    for (r = 0; r <= 0x18; r++) W(0, r, 0);
    Mute(FALSE);
    g_next = now();
}

static void frame(int f)
{
    static const WORD notes[] = { 4455, 5613, 6675, 8910 };
    WORD n = notes[(f / 8) & 3];
    BYTE r;
    W(0, 0x00, (BYTE)n); W(0, 0x01, (BYTE)(n >> 8));
    W(0, 0x02, 0x00); W(0, 0x03, 0x08);
    W(0, 0x05, 0x09); W(0, 0x06, 0xA4);
    W(0, 0x04, (BYTE)((f & 7) == 7 ? 0x40 : 0x41));
    for (r = 0x07; r <= 0x14; r++) W(0, r, (BYTE)(f + r));   /* voices 2-3, filter: busywork */
    W(0, 0x15, 0); W(0, 0x16, 0x40); W(0, 0x17, 0xF1);
    W(0, 0x18, 0x0F);
}

void __cdecl mainCRTStartup(void)
{
    const char *p = GetCommandLineA();
    int secs = 5, tunes = 2, t, f, frames, i;
    HMODULE h;
    LARGE_INTEGER q;
    BYTE vol;

    g_out = GetStdHandle(STD_OUTPUT_HANDLE);
    QueryPerformanceFrequency(&q);
    g_frame = q.LowPart / 50;           /* no 64-bit divide: no libgcc here */
    /* args: skip argv[0], then up to two numbers */
    if (*p == '"') { p++; while (*p && *p != '"') p++; if (*p) p++; }
    else while (*p && *p != ' ') p++;
    while (*p == ' ') p++;
    if (*p >= '0' && *p <= '9') { secs = 0; while (*p >= '0' && *p <= '9') secs = secs * 10 + *p++ - '0'; }
    while (*p == ' ') p++;
    if (*p >= '0' && *p <= '9') { tunes = 0; while (*p >= '0' && *p <= '9') tunes = tunes * 10 + *p++ - '0'; }

    h = LoadLibraryA("HardSID.dll");
    if (!h) { out("can't load HardSID.dll (error %lu)\r\n", GetLastError()); ExitProcess(2); }
    Count = (count_fn)GetProcAddress(h, "GetHardSIDCount");
    W     = (write_fn)GetProcAddress(h, "WriteToHardSID");
    R     = (read_fn)GetProcAddress(h, "ReadFromHardSID");
    Mute  = (mute_fn)GetProcAddress(h, "MuteHardSID_Line");
    if (!Count || !W || !R || !Mute) { out("missing exports\r\n"); ExitProcess(2); }

    out("GetHardSIDCount -> %d\r\n", Count());
    frames = secs * 50;
    for (t = 0; t < tunes; t++) {
        out("tune %d: %d s\r\n", t + 1, secs);
        tune_start();
        for (f = 0; f < frames; f++) {
            next_frame();
            frame(f);
            if (t == 0 && f == frames / 2) {
                out("  pause 1 s\r\n");
                vol = R(0, 0x18);
                W(0, 0x18, 0);
                Sleep(1000);
                W(0, 0x18, vol);
                g_next = now();
            }
        }
    }
    for (i = 0; i < 4; i++) out("read $1B -> $%02X\r\n", R(0, 0x1B));
    out("GetHardSIDCount -> %d\r\n", Count());
    out("done\r\n");
    ExitProcess(0);
}
