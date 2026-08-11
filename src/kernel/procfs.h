/*
 * procfs.h — /proc, the synthetic filesystem user space reads state out of.
 * (GPLv2)
 *
 * Every file here is generated on read from live kernel state; nothing is
 * stored.  The set is deliberately small and driven by demand: BusyBox's
 * ifconfig(8) refuses to enumerate interfaces without /proc/net/dev, route(8)
 * reads /proc/net/route, and an init system expects /proc/mounts and
 * /proc/uptime to exist.  Formats match Linux exactly, down to the header
 * lines, because the programs that read them are written to Linux's output
 * and a near-miss parses as garbage rather than failing loudly.
 */
#ifndef GNUCOS_PROCFS_H
#define GNUCOS_PROCFS_H

#include "vfs.h"

/* Resolve an absolute path under /proc.  Returns 0 and fills `out` when the
 * path names a procfs file or directory, -E_NOENT when it does not, and
 * -E_INVAL when the path is not under /proc at all. */
int procfs_resolve(const char *path, vfs_node_t *out);

/* Enumerate a procfs directory for getdents64.  `index` is the entry number
 * to report, starting at 0 (which is "."), and `name`/`type` are filled in.
 * Returns 0 on success or -E_NOENT once the directory is exhausted. */
int procfs_readdir(const char *dirpath, uint32_t index, char *name, uint8_t *type);

#endif
