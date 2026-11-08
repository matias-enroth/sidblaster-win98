/* SBTEST.EXE - SIDBlaster-USB sanity check for Windows 98 (no CRT)
   SBTEST               list FTDI ports from the registry and which COM ports open
   SBTEST COM3          reset the SID, play an arpeggio, read OSC3 back
   SBTEST NET host[:port]  the same through a Network SID Device server       */
#include "sbcommon.h"
#include "netsid_net.h"

static HANDLE g_out;

static void out(const char *fmt, ...)
{
    char buf[1100];   /* wvsprintf writes at most 1024 */
    DWORD n, done;
    va_list ap;
    va_start(ap, fmt);
    n = wvsprintfA(buf, fmt, ap);
    va_end(ap);
    WriteFile(g_out, buf, n, &done, NULL);
}

static void found(const char *port, const char *keypath, void *ctx)
{
    (void)ctx;
    out("  registry: HKLM\\%s -> %s\r\n", keypath, port);
}

static void w(HANDLE h, BYTE reg, BYTE val)
{
    BYTE b[2];
    b[0] = (BYTE)(SB_HDR_WRITE | reg);
    b[1] = val;
    if (!sb_send(h, b, 2)) out("  write $%02X failed (error %lu)\r\n", reg, GetLastError());
}

static const char *skip_arg0(const char *p)
{
    if (*p == '"') { p++; while (*p && *p != '"') p++; if (*p) p++; }
    else while (*p && *p != ' ' && *p != '\t') p++;
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

static void list_ports(void)
{
    int i;
    char name[8];
    HANDLE h;
    out("FTDI ports in registry:\r\n");
    sb_scan_ports(found, NULL);
    out("COM ports that open:\r\n");
    for (i = 1; i <= 9; i++) {
        wsprintfA(name, "COM%d", i);
        h = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) { out("  %s\r\n", name); CloseHandle(h); }
    }
    out("\r\nRun: SBTEST COMx   (with the SIDBlaster's port)\r\n");
    out("  or: SBTEST NET host[:port]   (a Network SID Device server, port 6581)\r\n");
}

static void run_test(const char *port)
{
    /* SID freq regs for a 1.000 MHz clock (SIDBlaster PIC drives 1 MHz):
       C4 E4 G4 C5 = 261.63 329.63 392.00 523.25 Hz                       */
    static const WORD notes[] = { 4389, 5530, 6577, 8779, 6577, 5530, 4389 };
    DWORD err, baud, t0, t1;
    BOOL setok;
    HANDLE h;
    BYTE v;
    int i, distinct = 0, last = -1;

    out("Opening %s at %lu baud...\r\n", port, (DWORD)SB_BAUD);
    h = sb_open_port(port, &err, &baud, &setok);
    if (h == INVALID_HANDLE_VALUE) { out("  open failed, error %lu\r\n", err); return; }
    out("  SetCommState: %s (error %lu), driver reports %lu baud\r\n",
        setok ? "ok" : "FAILED", err, baud);

    out("Resetting SID...\r\n");
    sb_reset(h);

    out("Arpeggio on voice 1 (pulse)...\r\n");
    w(h, 0x18, 0x0F);           /* volume max, no filter        */
    w(h, 0x02, 0x00);           /* pulse width $0800 = 50%      */
    w(h, 0x03, 0x08);
    w(h, 0x05, 0x09);           /* attack 0, decay 9            */
    w(h, 0x06, 0xA4);           /* sustain A, release 4         */
    for (i = 0; i < (int)(sizeof notes / sizeof notes[0]); i++) {
        w(h, 0x00, (BYTE)(notes[i] & 0xFF));
        w(h, 0x01, (BYTE)(notes[i] >> 8));
        w(h, 0x04, 0x41);       /* pulse + gate */
        Sleep(180);
        w(h, 0x04, 0x40);       /* release      */
        Sleep(70);
    }
    Sleep(400);

    out("Reading OSC3 ($1B) with voice 3 on noise, output muted (3OFF)...\r\n");
    w(h, 0x18, 0x80);           /* 3OFF, volume 0 */
    w(h, 0x0E, 0xFF);
    w(h, 0x0F, 0xFF);
    w(h, 0x12, 0x80);           /* noise */
    Sleep(20);
    for (i = 0; i < 8; i++) {
        t0 = GetTickCount();
        if (sb_read(h, 0x1B, &v)) {
            t1 = GetTickCount();
            out("  OSC3 = $%02X  (%lu ms)\r\n", v, t1 - t0);
            if (v != last) distinct++;
            last = v;
        } else {
            out("  read timed out / failed (error %lu)\r\n", GetLastError());
        }
    }
    w(h, 0x12, 0x00);
    w(h, 0x18, 0x00);
    CloseHandle(h);

    out("\r\n%s\r\n", distinct >= 3
        ? "Reads vary: the read path works."
        : "Reads don't vary: write path may still be fine if you heard the notes.");
    out("Heard the arpeggio? Then writes work and the DLL should too.\r\n");
}

/* ------------------------------------------------------------ NetSID */
static void netlog(const char *fmt, ...)
{
    char buf[1100];
    DWORD n, done;
    va_list ap;
    va_start(ap, fmt);
    n = wvsprintfA(buf, fmt, ap);
    va_end(ap);
    WriteFile(g_out, "  ", 2, &done, NULL);
    WriteFile(g_out, buf, n, &done, NULL);
    WriteFile(g_out, "\r\n", 2, &done, NULL);
}

static ns_sched g_ns;
static int64_t  g_cyc;          /* the arpeggio's timeline, in PAL cycles */
static int      g_netok;

static void nw(BYTE reg, BYTE val)
{
    if (g_netok && !ns_sched_write(&g_ns, g_cyc, reg, val)) g_netok = 0;
}

static void nwait(DWORD ms) { g_cyc += ns_ms_to_cycles(NS_PAL_HZ, ms); }

static void run_net(const char *arg)
{
    /* SID freq regs for a PAL C64's 985248 Hz: C4 E4 G4 C5 */
    static const WORD notes[] = { 4455, 5613, 6675, 8910, 6675, 5613, 4455 };
    char host[64];
    unsigned port = NS_DEFAULT_PORT;
    int i, n = 0, distinct = 0, last = -1;
    ns_link l;
    ns_setup su;
    DWORD t0, t1;
    BYTE v;

    while (arg[n] && arg[n] != ':' && n < 63) { host[n] = arg[n]; n++; }
    host[n] = 0;
    if (arg[n] == ':') {
        port = 0;
        for (n++; arg[n] >= '0' && arg[n] <= '9'; n++) port = port * 10 + (unsigned)(arg[n] - '0');
    }
    if (!host[0] || !port || port > 65535) { out("Usage: SBTEST NET host[:port]\r\n"); return; }

    out("Connecting to %s:%u...\r\n", host, port);
    if (!ns_net_startup(netlog)) return;
    ns_link_init(&l, netlog, 0);
    su.model = 0;
    su.ntsc = 0;
    t0 = GetTickCount();
    if (!ns_link_open(&l, host, (WORD)port, &su, 2000)) { out("Failed.\r\n"); return; }
    out("  (%lu ms to connect and set up)\r\n", GetTickCount() - t0);

    /* the timeline is in cycles already: "QPC" = the C64 clock */
    ns_sched_init(&g_ns, NS_PAL_HZ, NS_PAL_HZ, 100, 0, ns_link_send, &l);
    g_ns.nvoices = l.version >= 3 ? 4 : 3;
    g_netok = 1;
    g_cyc = 0;

    out("Arpeggio on voice 1 (pulse), sent with C64-cycle delays...\r\n");
    t0 = GetTickCount();
    nw(0x18, 0x0F);
    nw(0x02, 0x00);
    nw(0x03, 0x08);
    nw(0x05, 0x09);
    nw(0x06, 0xA4);
    for (i = 0; i < (int)(sizeof notes / sizeof notes[0]); i++) {
        nw(0x00, (BYTE)(notes[i] & 0xFF));
        nw(0x01, (BYTE)(notes[i] >> 8));
        nw(0x04, 0x41);
        nwait(180);
        nw(0x04, 0x40);
        nwait(70);
    }
    nwait(400);
    if (g_netok && !ns_sched_flush(&g_ns)) g_netok = 0;
    if (!g_netok) { out("Lost the connection.\r\n"); return; }
    out("  sent in %lu ms: %lu packets, %lu BUSY replies (the server pacing us)\r\n",
        GetTickCount() - t0, l.pkts, l.busy);

    out("Reading OSC3 ($1B) with voice 3 on noise, output muted (3OFF)...\r\n");
    nw(0x18, 0x80);
    nw(0x0E, 0xFF);
    nw(0x0F, 0xFF);
    nw(0x12, 0x80);
    nwait(20);
    for (i = 0; i < 8 && g_netok; i++) {
        t0 = GetTickCount();
        if (ns_sched_read(&g_ns, g_cyc, 0x1B, &v)) {
            t1 = GetTickCount();
            out("  OSC3 = $%02X  (%lu ms)\r\n", v, t1 - t0);
            if (v != last) distinct++;
            last = v;
        } else {
            g_netok = 0;
        }
        nwait(20);
    }
    nw(0x12, 0x00);
    nw(0x18, 0x00);
    if (g_netok) ns_sched_flush(&g_ns);
    Sleep(500);                       /* let the server play out its buffer */
    ns_link_goodbye(&l);

    if (!g_netok) { out("Lost the connection.\r\n"); return; }
    out("\r\n%s\r\n", distinct >= 3
        ? "Reads vary: the server emulates OSC3."
        : "Reads don't vary: fine if you heard the notes (some servers answer reads with 0).");
    out("Heard the arpeggio, at the right pitch? Then Backend=NetSID should work.\r\n");
}

void __cdecl mainCRTStartup(void)
{
    const char *arg;
    char port[32];
    int n = 0;

    g_out = GetStdHandle(STD_OUTPUT_HANDLE);
    out("SBTEST " SB_VERSION " - SIDBlaster-USB check for Windows 9x\r\n\r\n");
    arg = skip_arg0(GetCommandLineA());
    while (arg[n] && arg[n] != ' ' && arg[n] != '\t' && n < 31) { port[n] = arg[n]; n++; }
    port[n] = 0;
    if (lstrcmpiA(port, "NET") == 0) {
        arg += n;
        while (*arg == ' ' || *arg == '\t') arg++;
        run_net(arg);
    } else if (!port[0]) list_ports(); else run_test(port);
    ExitProcess(0);
}
