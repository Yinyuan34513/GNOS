/*
 * drm_vsnprintf.h — declarations for the ported DRM formatting layer.
 * (GPLv2)
 *
 * The Uinxed DRM code formats messages through vsnprintf/snprintf and the
 * kernel log entry plogk(); GNOS provides neither (its kernel is written
 * around dbg_puts/dbg_puts_dec/dbg_puts_hex).  drm_vsnprintf.c supplies a
 * minimal self-contained vsnprintf/snprintf, and a plogk() that formats into
 * a scratch buffer and emits it via dbg_puts -- the GNOS console.  The API
 * shape matches libc so the ported callers compile unchanged.
 */
#ifndef GNUCOS_DRM_VSNPRINTF_H
#define GNUCOS_DRM_VSNPRINTF_H

#include <stdarg.h>
#include <stddef.h>

int vsnprintf(char *str, size_t size, const char *fmt, va_list args);
int snprintf(char *str, size_t size, const char *fmt, ...);

/* Uinxed kernel-log entry point: format + emit to the debug console. */
void plogk(const char *format, ...);

#endif /* GNUCOS_DRM_VSNPRINTF_H */
