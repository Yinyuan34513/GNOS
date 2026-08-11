/*
 * procfs.c — /proc, generated on read. (GPLv2)
 *
 * There is no state here at all: opening a file under /proc records which
 * generator to call, and each read() re-renders that generator's output into
 * a scratch buffer and returns the requested slice.  That costs a full render
 * per read, which for files measured in hundreds of bytes is free, and it
 * removes every question about cache invalidation -- a reader cannot see a
 * stale interface counter because nothing is ever kept.
 *
 * The one thing worth being careful about is the *format*.  BusyBox parses
 * these files with sscanf against Linux's exact layout: ifconfig skips two
 * header lines in /proc/net/dev and then splits on ':', route(8) expects
 * eleven tab-separated columns with addresses as little-endian hex.  Getting
 * a column count wrong produces confident nonsense rather than an error, so
 * the layouts below are copied from Linux and not improvised.
 */
#include <stdint.h>

#include "procfs.h"
#include "sysnum.h"     /* DT_DIR / DT_REG, the getdents64 file types */
#include "kstring.h"
#include "net.h"
#include "pmm.h"
#include "timer.h"
#include "vfs.h"

/* One render never exceeds this; /proc/net/dev with two interfaces is the
 * largest and comes to a few hundred bytes. */
#define PROC_BUF 2048

/* ---- a tiny append-only string builder -------------------------------- */
/* The kernel has no snprintf, and /proc is the only place that needs one.
 * `pos` is allowed to run past `cap`: it then stops writing but keeps
 * counting, so a truncated render still reports the length it wanted. */
typedef struct {
    char    *buf;
    uint32_t cap;
    uint32_t pos;
} sbuf_t;

static void sb_char(sbuf_t *s, char c)
{
    if (s->pos < s->cap)
        s->buf[s->pos] = c;
    s->pos++;
}

static void sb_str(sbuf_t *s, const char *str)
{
    while (*str)
        sb_char(s, *str++);
}

/* Decimal, optionally right-aligned in `width` columns.  Alignment matters:
 * /proc/net/dev is a fixed-column table and ifconfig's parser walks past the
 * interface name by column, not by token. */
static void sb_dec(sbuf_t *s, uint64_t v, int width)
{
    char tmp[24];
    int  n = 0;
    do {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    } while (v);
    for (int i = n; i < width; i++)
        sb_char(s, ' ');
    while (n--)
        sb_char(s, tmp[n]);
}

/* Zero-padded lower-case hex, exactly `digits` wide. */
static void sb_hex(sbuf_t *s, uint64_t v, int digits, int upper)
{
    const char *d = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    for (int i = digits - 1; i >= 0; i--)
        sb_char(s, d[(v >> (i * 4)) & 0xF]);
}

/*
 * An IPv4 address the way Linux writes it into /proc/net/route: the __be32 as
 * it sits in memory, printed as a little-endian u32.  So 10.0.2.2, whose
 * bytes are 0a 00 02 02, comes out "0202000A".  Our addresses are kept in
 * host order, hence the byte swap.
 */
static void sb_route_addr(sbuf_t *s, uint32_t host_ip)
{
    uint32_t le = ((host_ip & 0xFF) << 24) | ((host_ip & 0xFF00) << 8) |
                  ((host_ip >> 8) & 0xFF00) | ((host_ip >> 24) & 0xFF);
    sb_hex(s, le, 8, 1);
}

/* ---- the generators ---------------------------------------------------- */
static void gen_net_dev(sbuf_t *s)
{
    sb_str(s, "Inter-|   Receive                                                |"
              "  Transmit\n");
    sb_str(s, " face |bytes    packets errs drop fifo frame compressed multicast|"
              "bytes    packets errs drop fifo colls carrier compressed\n");

    for (int i = 0; i < NET_IF_MAX; i++) {
        netif_t *n = net_if(i);
        if (!n || !n->name[0])
            continue;

        /* Linux right-aligns the name in six columns and follows it with the
         * colon ifconfig splits on. */
        uint32_t len = (uint32_t)strlen(n->name);
        for (uint32_t p = len; p < 6; p++)
            sb_char(s, ' ');
        sb_str(s, n->name);
        sb_char(s, ':');

        sb_dec(s, n->rx_bytes, 8);
        sb_dec(s, n->rx_packets, 8);
        sb_dec(s, 0, 5);                  /* errs */
        sb_dec(s, n->rx_dropped, 5);
        sb_str(s, "    0     0          0         0");   /* fifo..multicast */
        sb_dec(s, n->tx_bytes, 9);
        sb_dec(s, n->tx_packets, 8);
        sb_dec(s, 0, 5);                  /* errs */
        sb_dec(s, n->tx_dropped, 5);
        sb_str(s, "    0     0       0          0\n");   /* fifo..compressed */
    }
}

static void gen_net_route(sbuf_t *s)
{
    sb_str(s, "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask"
              "\t\tMTU\tWindow\tIRTT\n");

    for (int i = 0; i < NET_IF_MAX; i++) {
        netif_t *n = net_if(i);
        if (!n || !n->up || !n->name[0])
            continue;

        /* The on-link route for the interface's own subnet.  Flags 0x0001 is
         * RTF_UP; the gateway column is 0 because there is not one. */
        sb_str(s, n->name);
        sb_char(s, '\t');
        sb_route_addr(s, n->ip & n->netmask);
        sb_char(s, '\t');
        sb_route_addr(s, 0);
        sb_str(s, "\t0001\t0\t0\t0\t");
        sb_route_addr(s, n->netmask);
        sb_str(s, "\t0\t0\t0\n");

        /* ...and the default route through the gateway, if it has one.
         * 0x0003 is RTF_UP|RTF_GATEWAY. */
        if (n->gateway) {
            sb_str(s, n->name);
            sb_char(s, '\t');
            sb_route_addr(s, 0);
            sb_char(s, '\t');
            sb_route_addr(s, n->gateway);
            sb_str(s, "\t0003\t0\t0\t0\t");
            sb_route_addr(s, 0);
            sb_str(s, "\t0\t0\t0\n");
        }
    }
}

/* The resolver's view of the world.  musl reads /etc/resolv.conf itself, so
 * this exists for the programs that print "which addresses do I have". */
static void gen_net_if_inet6(sbuf_t *s)
{
    (void)s;                    /* no IPv6: the file exists but is empty */
}

static void gen_uptime(sbuf_t *s)
{
    uint64_t ticks = timer_ticks();          /* 100 Hz, so ticks are centiseconds */
    sb_dec(s, ticks / 100, 0);
    sb_char(s, '.');
    sb_dec(s, (ticks % 100) / 10, 0);
    sb_dec(s, ticks % 10, 0);
    /* Second field is idle time summed over CPUs.  We do not account for it,
     * and repeating uptime would be a lie, so it reads as zero. */
    sb_str(s, " 0.00\n");
}

static void gen_meminfo(sbuf_t *s)
{
    uint64_t total_kb = pmm_total_frames() * 4;
    uint64_t free_kb  = pmm_free_frames() * 4;

    sb_str(s, "MemTotal:       ");
    sb_dec(s, total_kb, 8);
    sb_str(s, " kB\nMemFree:        ");
    sb_dec(s, free_kb, 8);
    sb_str(s, " kB\nMemAvailable:   ");
    sb_dec(s, free_kb, 8);
    sb_str(s, " kB\nBuffers:               0 kB\nCached:                0 kB\n"
              "SwapTotal:             0 kB\nSwapFree:              0 kB\n");
}

static void gen_version(sbuf_t *s)
{
    sb_str(s, "GNOS version 0.1 (x86_64)\n");
}

static void gen_cmdline(sbuf_t *s)
{
    sb_str(s, "\n");
}

static void gen_filesystems(sbuf_t *s)
{
    sb_str(s, "\text2\nnodev\tproc\nnodev\tdevtmpfs\n");
}

/* /proc/mounts and /proc/self/mounts are the same table.  An init system
 * reads this to decide whether it still has to mount /proc. */
static void gen_mounts(sbuf_t *s)
{
    sb_str(s, "/dev/root / ext2 rw,relatime 0 0\n");
    sb_str(s, "proc /proc proc rw,nosuid,nodev,noexec,relatime 0 0\n");
    sb_str(s, "dev /dev devtmpfs rw,relatime 0 0\n");
}

/* /proc/devices, in Linux's exact layout: a "Character devices:" heading,
 * then one "<major> <name>" line per major number, then the block section.
 * BusyBox's mdev and a hand-written coldplug script both parse it, and the
 * majors are the real Linux ones so a node created from this file has the
 * same identity it would on Linux. */
static void gen_devices(sbuf_t *s)
{
    sb_str(s, "Character devices:\n");
    sb_str(s, "  1 mem\n");
    sb_str(s, "  5 tty\n");
    sb_str(s, "\nBlock devices:\n");
}

/* ---- the file table ---------------------------------------------------- */
typedef void (*proc_gen_t)(sbuf_t *s);

typedef struct {
    const char *path;           /* absolute, always beginning "/proc" */
    proc_gen_t  gen;
} procfile_t;

static const procfile_t g_files[] = {
    { "/proc/net/dev",      gen_net_dev       },
    { "/proc/net/route",    gen_net_route     },
    { "/proc/net/if_inet6", gen_net_if_inet6  },
    { "/proc/uptime",       gen_uptime        },
    { "/proc/meminfo",      gen_meminfo       },
    { "/proc/version",      gen_version       },
    { "/proc/cmdline",      gen_cmdline       },
    { "/proc/filesystems",  gen_filesystems   },
    { "/proc/mounts",       gen_mounts        },
    { "/proc/self/mounts",  gen_mounts        },
    { "/proc/devices",      gen_devices       },
};
#define NFILES ((int)(sizeof(g_files) / sizeof(g_files[0])))

/* The directories.  There are few enough to list rather than derive. */
static const char *g_dirs[] = { "/proc", "/proc/net", "/proc/self" };
#define NDIRS ((int)(sizeof(g_dirs) / sizeof(g_dirs[0])))

/* ---- read -------------------------------------------------------------- */
static int32_t procfs_read(vfs_node_t *n, uint64_t off, void *buf, uint32_t len)
{
    static char scratch[PROC_BUF];

    const procfile_t *f = (const procfile_t *)n->priv;
    if (!f)
        return -E_INVAL;

    sbuf_t s = { scratch, PROC_BUF, 0 };
    f->gen(&s);

    uint32_t size = (s.pos < PROC_BUF) ? s.pos : PROC_BUF;
    if (off >= size)
        return 0;                        /* end of file */

    uint32_t avail = size - (uint32_t)off;
    if (len > avail)
        len = avail;
    memcpy(buf, scratch + off, len);
    return (int32_t)len;
}

static const vfs_ops_t g_proc_ops = { .read = procfs_read, .write = NULL };

/* A directory's read is never called -- getdents64 goes through
 * procfs_readdir -- but the node needs non-NULL ops so an accidental read()
 * reports EISDIR-ish behaviour instead of dereferencing NULL. */
static int32_t procdir_read(vfs_node_t *n, uint64_t off, void *buf, uint32_t len)
{
    (void)n; (void)off; (void)buf; (void)len;
    return -E_ISDIR;
}

static const vfs_ops_t g_procdir_ops = { .read = procdir_read, .write = NULL };

/* ---- lookup ------------------------------------------------------------ */
/* The last component of a path, which is what a directory entry is named. */
static const char *basename_of(const char *path)
{
    const char *slash = path;
    for (const char *p = path; *p; p++)
        if (*p == '/')
            slash = p + 1;
    return slash;
}

int procfs_resolve(const char *path, vfs_node_t *out)
{
    if (!path || strncmp(path, "/proc", 5) != 0)
        return -E_INVAL;
    /* "/proc" itself, or something below it -- but not "/procfoo". */
    if (path[5] != '\0' && path[5] != '/')
        return -E_INVAL;

    for (int i = 0; i < NDIRS; i++) {
        if (strcmp(path, g_dirs[i]) != 0)
            continue;
        memset(out, 0, sizeof(*out));
        strncpy(out->name, basename_of(g_dirs[i]), VFS_NAME_MAX - 1);
        out->kind = VFS_DIR;
        out->ops  = &g_procdir_ops;
        out->priv = NULL;
        return 0;
    }

    for (int i = 0; i < NFILES; i++) {
        if (strcmp(path, g_files[i].path) != 0)
            continue;
        memset(out, 0, sizeof(*out));
        strncpy(out->name, basename_of(g_files[i].path), VFS_NAME_MAX - 1);
        out->kind = VFS_FILE;
        out->ops  = &g_proc_ops;
        out->priv = (void *)&g_files[i];
        /* Linux reports size 0 for generated files and every reader copes by
         * reading until EOF; claiming a size we would have to render twice to
         * know would be worse. */
        out->size = 0;
        return 0;
    }

    return -E_NOENT;
}

/* ---- readdir ----------------------------------------------------------- */
/* Is `path` a direct child of `dir`?  "/proc/net/dev" is a child of
 * "/proc/net" but not of "/proc", which is what keeps `ls /proc` from listing
 * the whole table flat. */
static int is_child_of(const char *dir, const char *path)
{
    size_t dl = strlen(dir);
    if (strncmp(path, dir, dl) != 0 || path[dl] != '/')
        return 0;
    for (const char *p = path + dl + 1; *p; p++)
        if (*p == '/')
            return 0;
    return 1;
}

int procfs_readdir(const char *dirpath, uint32_t index, char *name, uint8_t *type)
{
    int is_dir = 0;
    for (int i = 0; i < NDIRS; i++)
        if (strcmp(dirpath, g_dirs[i]) == 0)
            is_dir = 1;
    if (!is_dir)
        return -E_NOTDIR;

    /* "." and ".." come first, as they do on a real directory: shells and
     * find(1) both notice when they are missing. */
    uint32_t n = 0;
    if (index == n++) {
        strncpy(name, ".", VFS_NAME_MAX - 1);
        *type = DT_DIR;
        return 0;
    }
    if (index == n++) {
        strncpy(name, "..", VFS_NAME_MAX - 1);
        *type = DT_DIR;
        return 0;
    }

    for (int i = 0; i < NDIRS; i++) {
        if (!is_child_of(dirpath, g_dirs[i]))
            continue;
        if (index == n++) {
            strncpy(name, basename_of(g_dirs[i]), VFS_NAME_MAX - 1);
            *type = DT_DIR;
            return 0;
        }
    }

    for (int i = 0; i < NFILES; i++) {
        if (!is_child_of(dirpath, g_files[i].path))
            continue;
        if (index == n++) {
            strncpy(name, basename_of(g_files[i].path), VFS_NAME_MAX - 1);
            *type = DT_REG;
            return 0;
        }
    }

    return -E_NOENT;
}
