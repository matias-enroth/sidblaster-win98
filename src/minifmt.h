/* minifmt.h - a small wsprintf subset that doesn't touch USER32.
   On Windows 9x wsprintf lives in USER32, which can take the Win16Mutex and
   block; the time-critical writer thread formats its log lines with this.
   Supports %% %c %s %d %i %u %x %X, the 0 flag, a width and the l modifier. */
#ifndef MINIFMT_H
#define MINIFMT_H
#include <stdarg.h>

/* Writes at most cap-1 characters plus a NUL; returns the length written. */
unsigned mf_vfmt(char *buf, unsigned cap, const char *fmt, va_list ap);
unsigned mf_fmt(char *buf, unsigned cap, const char *fmt, ...);

#endif
