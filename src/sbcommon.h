/* sbcommon.h - shared SIDBlaster-USB serial helpers for Win9x (no CRT) */
#include <windows.h>
#include <stdarg.h>

/* Compiler may emit calls to these even with -ffreestanding. */
void *memset(void *d, int c, unsigned int n)
{ unsigned char *p = d; while (n--) *p++ = (unsigned char)c; return d; }
void *memcpy(void *d, const void *s, unsigned int n)
{ unsigned char *p = d; const unsigned char *q = s; while (n--) *p++ = *q++; return d; }

#define SB_VERSION   "1.0"

#define SB_BAUD      500000
#define SB_HDR_WRITE 0xE0   /* SYN | WRITE | RST high | reg */
#define SB_HDR_READ  0xA0   /* SYN | READ  | RST high | reg */
#define SB_HDR_RESET 0x80   /* SYN | READ  | RST low  (asserts /RES) */

/* Open COM port at 500 kbaud 8N1, no flow control.
   On return *actual_baud holds what GetCommState reports back. */
static HANDLE sb_open_port(const char *name, DWORD *err, DWORD *actual_baud, BOOL *setstate_ok)
{
    HANDLE h;
    DCB dcb;
    COMMTIMEOUTS to;
    char path[4 + 64];

    *err = 0; *actual_baud = 0; *setstate_ok = FALSE;
    lstrcpynA(path, name, 64);           /* Port= can be up to 63 chars */
    h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE && lstrlenA(path) > 4) {
        /* COM10 and up need the \\.\ prefix (NT); try it anyway */
        lstrcpyA(path, "\\\\.\\");
        lstrcpynA(path + 4, name, 64);
        h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    }
    if (h == INVALID_HANDLE_VALUE) { *err = GetLastError(); return h; }

    SetupComm(h, 4096, 4096);
    memset(&dcb, 0, sizeof dcb);
    dcb.DCBlength = sizeof dcb;
    GetCommState(h, &dcb);
    dcb.BaudRate = SB_BAUD;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fParity = FALSE;
    dcb.fOutxCtsFlow = FALSE;     /* SIDBlaster has no CTS/DSR/DTR/RTS wired */
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
    dcb.fDsrSensitivity = FALSE;
    dcb.fTXContinueOnXoff = TRUE;
    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;
    dcb.fErrorChar = FALSE;
    dcb.fNull = FALSE;
    dcb.fAbortOnError = FALSE;
    *setstate_ok = SetCommState(h, &dcb);
    if (!*setstate_ok) *err = GetLastError();

    memset(&dcb, 0, sizeof dcb);
    dcb.DCBlength = sizeof dcb;
    if (GetCommState(h, &dcb)) *actual_baud = dcb.BaudRate;

    to.ReadIntervalTimeout = 0;
    to.ReadTotalTimeoutMultiplier = 0;
    to.ReadTotalTimeoutConstant = 100;   /* FTDI latency timer defaults to 16 ms */
    to.WriteTotalTimeoutMultiplier = 0;
    to.WriteTotalTimeoutConstant = 1000;
    SetCommTimeouts(h, &to);
    PurgeComm(h, PURGE_TXCLEAR | PURGE_RXCLEAR | PURGE_TXABORT | PURGE_RXABORT);
    return h;
}

static BOOL sb_send(HANDLE h, const BYTE *buf, DWORD n)
{
    DWORD done = 0;
    return WriteFile(h, buf, n, &done, NULL) && done == n;
}

/* Pulse /RES: the 0x80 header leaves /RES low until the next header byte.
   The PIC answers each header with a (garbage) read byte, so purge RX after. */
static void sb_reset(HANDLE h)
{
    static const BYTE seq[] = { SB_HDR_RESET, SB_HDR_RESET, SB_HDR_RESET,
                                SB_HDR_WRITE | 0x18, 0x00 };
    sb_send(h, seq, sizeof seq);
    Sleep(30);
    PurgeComm(h, PURGE_RXCLEAR);
}

/* Synchronous register read (only meaningful for $19-$1C). */
static BOOL sb_read(HANDLE h, BYTE reg, BYTE *val)
{
    BYTE cmd = (BYTE)(SB_HDR_READ | (reg & 0x1F));
    DWORD got = 0;
    PurgeComm(h, PURGE_RXCLEAR);
    if (!sb_send(h, &cmd, 1)) return FALSE;
    if (!ReadFile(h, val, 1, &got, NULL) || got != 1) return FALSE;
    return TRUE;
}

/* --- registry scan for FTDI virtual COM ports ------------------------- */
typedef void (*sb_port_cb)(const char *port, const char *keypath, void *ctx);

static void sb_scan_key(HKEY root, const char *path, int depth, sb_port_cb cb, void *ctx)
{
    HKEY k;
    char val[64], sub[200], child[400];
    DWORD type, len, sublen, i;
    FILETIME ft;

    if (RegOpenKeyExA(root, path, 0, KEY_READ, &k) != ERROR_SUCCESS) return;
    len = sizeof val - 1;
    if (RegQueryValueExA(k, "PortName", NULL, &type, (BYTE *)val, &len) == ERROR_SUCCESS
        && type == REG_SZ) {
        val[len < sizeof val ? len : sizeof val - 1] = 0;
        cb(val, path, ctx);
    }
    if (depth > 0) {
        for (i = 0; ; i++) {
            sublen = sizeof sub;
            if (RegEnumKeyExA(k, i, sub, &sublen, NULL, NULL, NULL, &ft) != ERROR_SUCCESS) break;
            if ((DWORD)lstrlenA(path) + sublen + 2 >= sizeof child) continue;
            lstrcpyA(child, path); lstrcatA(child, "\\"); lstrcatA(child, sub);
            sb_scan_key(root, child, depth - 1, cb, ctx);
        }
    }
    RegCloseKey(k);
}

static void sb_scan_ports(sb_port_cb cb, void *ctx)
{
    /* Win98: device nodes live under HKLM\Enum. NT paths included for testing on XP. */
    sb_scan_key(HKEY_LOCAL_MACHINE, "Enum\\USB\\VID_0403&PID_6001", 2, cb, ctx);
    sb_scan_key(HKEY_LOCAL_MACHINE, "Enum\\FTDIBUS", 3, cb, ctx);
    sb_scan_key(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Enum\\FTDIBUS", 3, cb, ctx);
}
