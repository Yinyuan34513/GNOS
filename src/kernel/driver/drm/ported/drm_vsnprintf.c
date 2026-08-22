/*
 * drm_vsnprintf.c — minimal vsnprintf/snprintf for the ported DRM core.
 * (GPLv2)
 *
 * GNOS's kernel deliberately has no snprintf (procfs.c formats numbers by
 * hand).  The Uinxed DRM files, however, print all over the place through
 * drm_print.c's drm_vprintf/drm_dev_printk, which need a real formatted
 * buffer writer.  This is a compact self-contained implementation covering
 * the conversions the DRM code actually uses:
 *
 *   %s %c %d %i %u %x %X %p %ld %lu %lx %zu %zd %lld %llu %%
 *
 * plus a leading '-' for negatives and a minimal '0' flag.  Widths and
 * precisions are not honoured -- nothing in the DRM core depends on them,
 * and implementing the full C grammar would be several hundred lines for no
 * caller.  The API surface matches libc so the ported code compiles
 * unchanged: vsnprintf/snprintf, and the va_list plumbing drm_print.c uses.
 */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Append one character to buf[0..size), counting everything written. */
static size_t kput(char *buf, size_t size, size_t *idx, char c)
{
    if (*idx < size && buf)
        buf[*idx] = c;
    (*idx)++;
    return *idx;
}

/* Emit an unsigned value in base 10 or 16, right-to-left. */
static size_t kuint(char *buf, size_t size, size_t *idx, uint64_t v, int base,
                    bool upper)
{
    char tmp[24];
    int  n = 0;
    const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";

    if (v == 0) {
        kput(buf, size, idx, '0');
        return *idx;
    }
    while (v) {
        tmp[n++] = dig[v % (unsigned)base];
        v /= (unsigned)base;
    }
    while (n--)
        kput(buf, size, idx, tmp[n]);
    return *idx;
}

static size_t kstr(char *buf, size_t size, size_t *idx, const char *s)
{
    if (!s)
        s = "(null)";
    while (*s)
        kput(buf, size, idx, *s++);
    return *idx;
}

int vsnprintf(char *str, size_t size, const char *fmt, va_list args)
{
    size_t idx = 0;
    char   c;

    while ((c = *fmt++) != 0) {
        if (c != '%') {
            kput(str, size, &idx, c);
            continue;
        }

        c = *fmt++;
        if (c == 0)
            break;

        /* Flags: only '0' is understood; skip others. */
        int pad0 = 0;
        while (c == '0' || c == '-') {
            if (c == '0') pad0 = 1;
            c = *fmt++;
        }
        /* Skip a field width (digits). */
        while (c >= '0' && c <= '9')
            c = *fmt++;
        /* Skip a precision ('.' digits). */
        if (c == '.') {
            c = *fmt++;
            while (c >= '0' && c <= '9')
                c = *fmt++;
        }
        (void)pad0;

        switch (c) {
        case 's': {
            const char *s = va_arg(args, const char *);
            kstr(str, size, &idx, s);
            break;
        }
        case 'c': {
            int ch = va_arg(args, int);
            kput(str, size, &idx, (char)ch);
            break;
        }
        case 'd':
        case 'i': {
            int64_t v = va_arg(args, int64_t);
            if (v < 0) {
                kput(str, size, &idx, '-');
                kuint(str, size, &idx, (uint64_t)(-v), 10, false);
            } else {
                kuint(str, size, &idx, (uint64_t)v, 10, false);
            }
            break;
        }
        case 'u': {
            uint64_t v = va_arg(args, uint64_t);
            kuint(str, size, &idx, v, 10, false);
            break;
        }
        case 'x':
            kuint(str, size, &idx, va_arg(args, uint64_t), 16, false);
            break;
        case 'X':
            kuint(str, size, &idx, va_arg(args, uint64_t), 16, true);
            break;
        case 'p':
            kstr(str, size, &idx, "0x");
            kuint(str, size, &idx, (uint64_t)(uintptr_t)va_arg(args, void *),
                  16, false);
            break;
        case 'l': {
            c = *fmt++;
            if (c == 'l') {                 /* %ll */
                c = *fmt++;
                if (c == 'd' || c == 'i') {
                    int64_t v = va_arg(args, int64_t);
                    if (v < 0) {
                        kput(str, size, &idx, '-');
                        kuint(str, size, &idx, (uint64_t)(-v), 10, false);
                    } else {
                        kuint(str, size, &idx, (uint64_t)v, 10, false);
                    }
                } else if (c == 'u') {
                    kuint(str, size, &idx, va_arg(args, uint64_t), 10, false);
                } else if (c == 'x') {
                    kuint(str, size, &idx, va_arg(args, uint64_t), 16, false);
                } else {
                    kput(str, size, &idx, '%');
                    kput(str, size, &idx, 'l');
                    kput(str, size, &idx, 'l');
                    kput(str, size, &idx, c);
                }
            } else if (c == 'd' || c == 'i') {
                int64_t v = va_arg(args, int64_t);
                if (v < 0) {
                    kput(str, size, &idx, '-');
                    kuint(str, size, &idx, (uint64_t)(-v), 10, false);
                } else {
                    kuint(str, size, &idx, (uint64_t)v, 10, false);
                }
            } else if (c == 'u') {
                kuint(str, size, &idx, va_arg(args, uint64_t), 10, false);
            } else if (c == 'x') {
                kuint(str, size, &idx, va_arg(args, uint64_t), 16, false);
            } else {
                kput(str, size, &idx, '%');
                kput(str, size, &idx, 'l');
                kput(str, size, &idx, c);
            }
            break;
        }
        case 'z': {
            c = *fmt++;
            if (c == 'd' || c == 'i') {
                int64_t v = (int64_t)va_arg(args, size_t);
                if (v < 0) {
                    kput(str, size, &idx, '-');
                    kuint(str, size, &idx, (uint64_t)(-v), 10, false);
                } else {
                    kuint(str, size, &idx, (uint64_t)v, 10, false);
                }
            } else if (c == 'u') {
                kuint(str, size, &idx, (uint64_t)va_arg(args, size_t), 10,
                      false);
            } else if (c == 'x') {
                kuint(str, size, &idx, (uint64_t)va_arg(args, size_t), 16,
                      false);
            } else {
                kput(str, size, &idx, '%');
                kput(str, size, &idx, 'z');
                kput(str, size, &idx, c);
            }
            break;
        }
        case '%':
            kput(str, size, &idx, '%');
            break;
        default:
            kput(str, size, &idx, '%');
            kput(str, size, &idx, c);
            break;
        }
    }

    if (str && size > 0)
        str[idx < size ? idx : size - 1] = 0;
    return (int)idx;
}

int snprintf(char *str, size_t size, const char *fmt, ...)
{
    va_list args;
    int     n;

    va_start(args, fmt);
    n = vsnprintf(str, size, fmt, args);
    va_end(args);
    return n;
}

/* Kernel log entry: format into a scratch buffer, then emit to the GNOS
 * debug console (the ported DRM core's analogue of printk). */
void plogk(const char *format, ...)
{
    extern void dbg_puts(const char *s);
    char    buf[512];
    va_list args;

    va_start(args, format);
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    dbg_puts(buf);
}
