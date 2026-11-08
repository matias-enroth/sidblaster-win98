/* netsid_net.c - see netsid_net.h. Winsock 1.1 only, loaded at run time. */
#include "netsid_net.h"

#define LOG(...) do { if (l->log) l->log(__VA_ARGS__); } while (0)

static int g_wsa;            /* 1 loaded, -1 failed */
static __typeof__(WSAStartup)      *p_WSAStartup;
static __typeof__(WSAGetLastError) *p_WSAGetLastError;
static __typeof__(socket)          *p_socket;
static __typeof__(connect)         *p_connect;
static __typeof__(send)            *p_send;
static __typeof__(recv)            *p_recv;
static __typeof__(select)          *p_select;
static __typeof__(ioctlsocket)     *p_ioctlsocket;
static __typeof__(setsockopt)      *p_setsockopt;
static __typeof__(closesocket)     *p_closesocket;
static __typeof__(gethostbyname)   *p_gethostbyname;
static __typeof__(inet_addr)       *p_inet_addr;

BOOL ns_net_startup(ns_logfn log)
{
    static const char *const names[] = {
        "WSAStartup", "WSAGetLastError", "socket", "connect", "send", "recv", "select",
        "ioctlsocket", "setsockopt", "closesocket", "gethostbyname", "inet_addr" };
    FARPROC *const slots[] = {
        (FARPROC *)&p_WSAStartup, (FARPROC *)&p_WSAGetLastError, (FARPROC *)&p_socket,
        (FARPROC *)&p_connect, (FARPROC *)&p_send, (FARPROC *)&p_recv, (FARPROC *)&p_select,
        (FARPROC *)&p_ioctlsocket, (FARPROC *)&p_setsockopt, (FARPROC *)&p_closesocket,
        (FARPROC *)&p_gethostbyname, (FARPROC *)&p_inet_addr };
    HMODULE h;
    WSADATA wd;
    unsigned i;
    int r;

    if (g_wsa) return g_wsa > 0;
    g_wsa = -1;
    h = LoadLibraryA("WSOCK32.DLL");
    if (!h) {
        if (log) log("netsid: can't load WSOCK32.DLL (error %lu): is TCP/IP installed?", GetLastError());
        return FALSE;
    }
    for (i = 0; i < sizeof names / sizeof names[0]; i++) {
        *slots[i] = GetProcAddress(h, names[i]);
        if (!*slots[i]) {
            if (log) log("netsid: WSOCK32.DLL has no %s", names[i]);
            return FALSE;
        }
    }
    r = p_WSAStartup(MAKEWORD(1, 1), &wd);
    if (r != 0) {
        if (log) log("netsid: WSAStartup(1.1) failed, error %d", r);
        return FALSE;
    }
    g_wsa = 1;
    return TRUE;
}

void ns_link_init(ns_link *l, ns_logfn log, DWORD qpf_khz)
{
    l->s = INVALID_SOCKET;
    l->log = log;
    l->timeout_ms = 500;
    l->read_timeout_ms = 1000;
    l->busy_ms = 2000;
    l->version = 0;
    l->stale = 0;
    l->addr = 0;
    l->qpf_khz = qpf_khz;
    l->pkts = l->busy = l->errors = l->max_rtt_us = 0;
}

void ns_link_close(ns_link *l)
{
    if (l->s != INVALID_SOCKET) p_closesocket(l->s);
    l->s = INVALID_SOCKET;
}

/* wait until the socket is readable (what=0) or writable (what=1);
   for what=1 a failed connect shows up as an error: returns -1 */
static int wait_sock(SOCKET s, int what, DWORD ms)
{
    fd_set fs, fe;
    struct timeval tv;
    int r;
    fs.fd_count = 1; fs.fd_array[0] = s;
    fe.fd_count = 1; fe.fd_array[0] = s;
    tv.tv_sec = (long)(ms / 1000);
    tv.tv_usec = (long)(ms % 1000) * 1000;
    r = p_select(0, what ? NULL : &fs, what ? &fs : NULL, &fe, &tv);
    if (r == SOCKET_ERROR) return -1;
    if (r == 0) return 0;
    if (fe.fd_count) return -1;
    return fs.fd_count ? 1 : 0;
}

/* ms left of `timeout` since t0; 0 once it has run out (read the clock once) */
static DWORD left(DWORD t0, DWORD timeout)
{
    DWORD e = GetTickCount() - t0;
    return e < timeout ? timeout - e : 0;
}

static DWORD elapsed(DWORD t0) { return GetTickCount() - t0; }

static void fail(ns_link *l, const char *what)
{
    LOG("netsid: %s (error %d), link down", what, p_WSAGetLastError());
    ns_link_close(l);
}

static BOOL send_all(ns_link *l, const BYTE *p, unsigned n, DWORD timeout)
{
    DWORD t0 = GetTickCount();
    while (n) {
        int r = p_send(l->s, (const char *)p, (int)n, 0);
        if (r > 0) { p += r; n -= (unsigned)r; continue; }
        if (r == SOCKET_ERROR && p_WSAGetLastError() == WSAEWOULDBLOCK) {
            DWORD ms = left(t0, timeout);
            if (ms && wait_sock(l->s, 1, ms) > 0) continue;
        }
        fail(l, "send failed");
        return FALSE;
    }
    return TRUE;
}

/* throw away whatever is waiting (late ERROR text) */
static void drain_stale(ns_link *l)
{
    char junk[256];
    while (wait_sock(l->s, 0, 0) > 0 && p_recv(l->s, junk, sizeof junk, 0) > 0) ;
    l->stale = 0;
}

/* One request, one response. Returns the response length, or 0 after
   closing the link. resp must hold NS_INFO_MAX bytes. */
static int xfer(ns_link *l, const BYTE *pkt, unsigned len, BYTE *resp, DWORD timeout)
{
    unsigned n = 0;
    int want, r = 0;
    DWORD t0, ms;
    LARGE_INTEGER q0, q1;

    if (l->s == INVALID_SOCKET) return 0;
    if (l->stale) drain_stale(l);
    if (l->qpf_khz) QueryPerformanceCounter(&q0);
    if (!send_all(l, pkt, len, timeout)) return 0;
    t0 = GetTickCount();
    for (;;) {
        want = ns_resp_len(resp, n);
        if (want < 0) {
            LOG("netsid: unknown response code %u to command %u", resp[0], pkt[0]);
            ns_link_close(l);
            return 0;
        }
        if (want > 0) break;
        if (n >= NS_INFO_MAX || (ms = left(t0, timeout)) == 0
            || (r = wait_sock(l->s, 0, ms)) == 0) {
            LOG("netsid: no response to command %u within %lu ms, link down", pkt[0], timeout);
            ns_link_close(l);
            return 0;
        }
        if (r < 0 || (r = p_recv(l->s, (char *)resp + n, NS_INFO_MAX - n, 0)) == SOCKET_ERROR) {
            fail(l, "receive failed");
            return 0;
        }
        if (r == 0) {
            LOG("netsid: server closed the connection, link down");
            ns_link_close(l);
            return 0;
        }
        n += (unsigned)r;
    }
    if (l->qpf_khz) {
        DWORD us;
        QueryPerformanceCounter(&q1);
        us = (q1.QuadPart - q0.QuadPart) > 4000000 ? 4000000000u
           : (DWORD)(q1.QuadPart - q0.QuadPart) * 1000 / l->qpf_khz;
        if (us > l->max_rtt_us) l->max_rtt_us = us;
    }
    l->pkts++;
    if (resp[0] == NS_R_ERROR) {
        /* the message is unframed: take what arrives within a moment */
        char msg[80];
        unsigned i, k = 0;
        BYTE more[NS_INFO_MAX];
        for (i = 1; i < n && k < sizeof msg - 1; i++) msg[k++] = (char)resp[i];
        while (k < sizeof msg - 1 && wait_sock(l->s, 0, 20) > 0
               && (r = p_recv(l->s, (char *)more, sizeof more, 0)) > 0)
            for (i = 0; i < (unsigned)r && k < sizeof msg - 1; i++) msg[k++] = (char)more[i];
        for (i = 0; i < k; i++) if ((BYTE)msg[i] < 0x20 || msg[i] == 0x7F) msg[i] = '?';
        msg[k] = 0;
        LOG("netsid: server says ERROR to command %u%s%s", pkt[0], k ? ": " : "", msg);
        l->stale = 1;
        l->errors++;
    } else if ((unsigned)want < n) {
        LOG("netsid: %u stray bytes after the response to command %u", n - (unsigned)want, pkt[0]);
    }
    return want;
}

/* How long a packet plays, in ms (about 985 cycles each): TRY_DELAY's delay,
   or the sum of a TRY_WRITE's / TRY_READ's delays. */
static DWORD pkt_span_ms(const uint8_t *pkt, unsigned len)
{
    DWORD cyc = 0;
    unsigned i;
    if (pkt[0] == NS_TRY_DELAY && len >= NS_HDR + 2)
        cyc = (DWORD)pkt[4] << 8 | pkt[5];
    else if (pkt[0] == NS_TRY_WRITE || pkt[0] == NS_TRY_READ)
        for (i = NS_HDR; i + 2 <= len; i += 4) cyc += (DWORD)pkt[i] << 8 | pkt[i + 1];
    return cyc / 1000;
}

int ns_link_send(void *link, const uint8_t *pkt, unsigned len, uint8_t *readval)
{
    ns_link *l = link;
    BYTE r[NS_INFO_MAX];
    DWORD t0 = 0;
    int busy = 0;
    DWORD timeout = pkt[0] == NS_TRY_READ ? l->read_timeout_ms : l->timeout_ms;
    /* BUSY: the server is full until it has played about this packet's worth
       of time, so wait that long (2-20 ms) before trying again. Retrying every
       2 ms while pinned at the server's limit cost the K6-2 enough CPU to
       starve SIDPLAY: 137 BUSY round trips a second in a Win98 log. */
    DWORD wait = pkt_span_ms(pkt, len);
    if (wait < 2) wait = 2;
    if (wait > 20) wait = 20;

    for (;;) {
        if (!xfer(l, pkt, len, r, timeout)) return 0;
        if (r[0] == NS_R_BUSY) {
            l->busy++;
            if (!busy) { busy = 1; t0 = GetTickCount(); }
            else if (elapsed(t0) > l->busy_ms) {
                LOG("netsid: server BUSY for %lu ms on command %u, link down", l->busy_ms, pkt[0]);
                ns_link_close(l);
                return 0;
            }
            Sleep(wait);
            continue;
        }
        if (r[0] == NS_R_READ && readval) *readval = r[1];
        return 1;
    }
}

/* a request whose reply carries a value (VERSION, COUNT, INFO) */
static int query(ns_link *l, BYTE cmd, BYTE sid, BYTE expect, BYTE *r)
{
    BYTE p[NS_HDR];
    if (!xfer(l, p, ns_pkt0(p, cmd, sid), r, l->timeout_ms)) return 0;
    if (r[0] != expect) {
        LOG("netsid: command %u answered with code %u, not %u", cmd, r[0], expect);
        return -1;
    }
    return 1;
}

static BOOL cmd(ns_link *l, const BYTE *p, unsigned n)
{
    return ns_link_send(l, p, n, NULL);
}

static BOOL link_open(ns_link *l, const char *host, WORD port, const ns_setup *su, DWORD connect_ms);

/* The set-up replies get 2 s instead of the usual 500 ms: a JSIDDevice
   that was just started took over 500 ms to answer SET_SID_MODEL (building
   its SID emulation on a cold JVM). connect_ms still bounds a dead server. */
BOOL ns_link_open(ns_link *l, const char *host, WORD port, const ns_setup *su, DWORD connect_ms)
{
    DWORD timeout = l->timeout_ms;
    BOOL ok;
    if (l->timeout_ms < 2000) l->timeout_ms = 2000;
    ok = link_open(l, host, port, su, connect_ms);
    l->timeout_ms = timeout;
    return ok;
}

static BOOL link_open(ns_link *l, const char *host, WORD port, const ns_setup *su, DWORD connect_ms)
{
    struct sockaddr_in sa;
    unsigned long addr, one = 1;
    BYTE r[NS_INFO_MAX], p[8];
    int i, count, k;
    int pick = su->model;
    DWORD busy_ms = l->busy_ms;
    BOOL nodelay = TRUE;

    ns_link_close(l);
    l->stale = 0;
    /* A name is looked up on the first connect only: gethostbyname can block
       for seconds, and reconnects run on the writer thread. */
    addr = l->addr ? l->addr : p_inet_addr(host);
    if (addr == INADDR_NONE) {
        struct hostent *he = p_gethostbyname(host);   /* blocks; an IP address avoids it */
        if (!he || !he->h_addr_list[0]) {
            LOG("netsid: can't resolve %s (error %d)", host, p_WSAGetLastError());
            return FALSE;
        }
        addr = *(unsigned long *)he->h_addr_list[0];
    }
    l->addr = addr;
    l->s = p_socket(AF_INET, SOCK_STREAM, 0);
    if (l->s == INVALID_SOCKET) {
        LOG("netsid: socket() failed (error %d)", p_WSAGetLastError());
        return FALSE;
    }
    p_ioctlsocket(l->s, FIONBIO, &one);       /* non-blocking from here on */
    p_setsockopt(l->s, IPPROTO_TCP, TCP_NODELAY, (const char *)&nodelay, sizeof nodelay);
    sa.sin_family = AF_INET;
    sa.sin_port = (u_short)((port >> 8) | ((port & 0xFF) << 8));   /* htons */
    sa.sin_addr.s_addr = addr;
    for (i = 0; i < 8; i++) sa.sin_zero[i] = 0;
    if (p_connect(l->s, (struct sockaddr *)&sa, sizeof sa) == SOCKET_ERROR
        && p_WSAGetLastError() != WSAEWOULDBLOCK) {
        fail(l, "connect failed");
        return FALSE;
    }
    if ((k = wait_sock(l->s, 1, connect_ms)) <= 0) {
        LOG("netsid: can't connect to %s:%u (%s)", host, port,
            k < 0 ? "refused or unreachable" : "timed out");
        ns_link_close(l);
        return FALSE;
    }

    /* 1. version */
    if (query(l, NS_GET_VERSION, 0, NS_R_VERSION, r) <= 0) goto bad;
    l->version = r[1];
    LOG("netsid: connected to %s:%u, protocol version %d", host, port, l->version);
    if (l->version < 2) {
        LOG("netsid: version 2 or later needed");
        goto bad;
    }
    /* 2.-3. SID configs; pick one by index or by model */
    if ((k = query(l, NS_GET_CONFIG_COUNT, 0, NS_R_COUNT, r)) == 0) return FALSE;
    count = k > 0 ? r[1] : 0;
    if (su->model == 6581 || su->model == 8580) pick = -1;
    for (i = 0; i < count; i++) {
        if ((k = query(l, NS_GET_CONFIG_INFO, (BYTE)i, NS_R_INFO, r)) == 0) return FALSE;
        if (k < 0) continue;
        r[NS_INFO_MAX - 1] = 0;                        /* a name with no NUL */
        LOG("netsid: config %d: %s \"%s\"", i, r[1] ? "8580" : "6581", (const char *)r + 2);
        if (pick < 0 && r[1] == (su->model == 8580)) pick = i;
    }
    if (pick < 0) {
        LOG("netsid: no %d config, using config 0", su->model);
        pick = 0;
    } else if (!(su->model == 6581 || su->model == 8580) && pick >= count) {
        if (count) LOG("netsid: there's no config %d, using config 0", pick);
        pick = 0;
    }
    if (count) LOG("netsid: using config %d, %s clock", pick, su->ntsc ? "NTSC" : "PAL");

    /* 4.-8. set up; the TRY_ ones retry on BUSY for up to a second.
       The SID count goes first: JSIDDevice (JSIDPlay2 4.14) creates its SID
       objects on TRY_SET_SID_COUNT + SET_SID_MODEL, and a model, clock or
       reset sent before the count hits a missing SID and drops the client.
       acid64c uses this order too. */
    l->busy_ms = 1000;
    if (!cmd(l, p, ns_pkt0(p, NS_TRY_SET_SID_COUNT, 1))
        || !(count == 0 || cmd(l, p, ns_pkt1(p, NS_SET_SID_MODEL, 0, (BYTE)pick)))
        || !cmd(l, p, ns_pkt1(p, NS_SET_CLOCKING, 0, (BYTE)(su->ntsc ? 1 : 0)))
        || !cmd(l, p, ns_pkt1(p, NS_TRY_SET_SAMPLING, 0, 1))
        || !cmd(l, p, ns_pkt1(p, NS_TRY_RESET, 0, 0))) {
        l->busy_ms = busy_ms;
        return FALSE;
    }
    for (i = 0; i < (l->version >= 3 ? 4 : 3); i++)
        if (!cmd(l, p, ns_pkt2(p, NS_MUTE, 0, (BYTE)i, 0))) { l->busy_ms = busy_ms; return FALSE; }
    l->busy_ms = busy_ms;
    LOG("netsid: ready");
    return TRUE;

bad:
    ns_link_close(l);
    return FALSE;
}

void ns_link_goodbye(ns_link *l)
{
    BYTE p[8], r[NS_INFO_MAX];
    if (l->s == INVALID_SOCKET) return;
    l->stale = 1;           /* a request may have been cut off mid-way */
    if (xfer(l, p, ns_pkt0(p, NS_FLUSH, 0), r, 100))
        xfer(l, p, ns_pkt1(p, NS_TRY_RESET, 0, 0), r, 100);
    ns_link_close(l);
}
