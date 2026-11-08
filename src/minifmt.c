/* minifmt.c - see minifmt.h. No C runtime; 32-bit division only. */
#include "minifmt.h"

unsigned mf_vfmt(char *buf, unsigned cap, const char *fmt, va_list ap)
{
    unsigned n = 0;
    if (!cap) return 0;
    while (*fmt) {
        char tmp[12];
        const char *s;
        unsigned len, width = 0, i, base;
        unsigned long v;
        int zero = 0, neg = 0, islong = 0;
        char pad;

        if (*fmt != '%') {
            if (n + 1 < cap) buf[n++] = *fmt;
            fmt++;
            continue;
        }
        fmt++;
        if (*fmt == '0') { zero = 1; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (unsigned)(*fmt++ - '0');
        if (*fmt == 'l') { islong = 1; fmt++; }

        s = tmp;
        len = 0;
        switch (*fmt) {
        case 's':
            s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            while (s[len]) len++;
            break;
        case 'c':
            tmp[0] = (char)va_arg(ap, int);
            len = 1;
            break;
        case 'd': case 'i': case 'u': case 'x': case 'X':
            if (*fmt == 'd' || *fmt == 'i') {
                long sv = islong ? va_arg(ap, long) : va_arg(ap, int);
                if (sv < 0) { neg = 1; v = (unsigned long)(-(sv + 1)) + 1; }
                else v = (unsigned long)sv;
            } else {
                v = islong ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int);
            }
            base = (*fmt == 'x' || *fmt == 'X') ? 16 : 10;
            i = sizeof tmp;
            do {   /* v fits 32 bits on Win32; the tests build 64-bit, where it may not */
                unsigned dgt = (unsigned)(v % base);
                tmp[--i] = (char)(dgt < 10 ? '0' + dgt : (*fmt == 'x' ? 'a' : 'A') + dgt - 10);
                v /= base;
            } while (v && i > 1);
            if (neg) tmp[--i] = '-';
            s = tmp + i;
            len = (unsigned)sizeof tmp - i;
            break;
        case '%':
            tmp[0] = '%';
            len = 1;
            break;
        case 0:
            fmt--;     /* lone % at the end */
            break;
        default:       /* unknown: print it as is */
            tmp[0] = '%';
            tmp[1] = *fmt;
            len = 2;
            break;
        }
        fmt++;

        pad = zero ? '0' : ' ';
        if (neg && pad == '0' && len < width) {   /* -0042, not 00-42 */
            if (n + 1 < cap) buf[n++] = '-';
            s++; len--; width--;
        }
        for (; width > len; width--)
            if (n + 1 < cap) buf[n++] = pad;
        for (i = 0; i < len; i++)
            if (n + 1 < cap) buf[n++] = s[i];
    }
    buf[n] = 0;
    return n;
}

unsigned mf_fmt(char *buf, unsigned cap, const char *fmt, ...)
{
    unsigned n;
    va_list ap;
    va_start(ap, fmt);
    n = mf_vfmt(buf, cap, fmt, ap);
    va_end(ap);
    return n;
}
