/* SBTEST.EXE - SIDBlaster-USB sanity check for Windows 98 (no CRT)
   SBTEST          list FTDI ports from the registry and which COM ports open
   SBTEST COM3     reset the SID, play an arpeggio, read OSC3 back            */
#include "sbcommon.h"

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
    if (!port[0]) list_ports(); else run_test(port);
    ExitProcess(0);
}
