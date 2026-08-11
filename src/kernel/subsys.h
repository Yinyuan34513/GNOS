/*
 * subsys.h — the built-in subsystem (driver) registry. (GPLv2)
 *
 * GNOS has no loadable .ko files: every driver is linked into the kernel
 * image.  What a module system actually buys you, though, is not the dynamic
 * loading -- it is the *registry*: a single place that knows which drivers
 * exist, what class they belong to, whether their init succeeded, and which
 * /dev node (if any) each one published.  That is what this file provides.
 *
 * Two things depend on it:
 *   - the boot log and /proc-style introspection, so a headless test can
 *     assert "the framebuffer subsystem came up" instead of grepping prose;
 *   - the coldplug helper, which walks the registry to decide which device
 *     nodes user space should be able to see.
 *
 * A subsystem registers itself from its own init function, so adding a driver
 * never means editing a table in another file.
 */
#ifndef GNUCOS_SUBSYS_H
#define GNUCOS_SUBSYS_H

#include <stdint.h>

#define SUBSYS_MAX       32
#define SUBSYS_NAME_MAX  24

/* Device classes, mirroring the names Linux uses under /sys/class so a
 * coldplug script written against one reads the same against the other. */
#define SUBSYS_CLASS_OTHER   0
#define SUBSYS_CLASS_GRAPHIC 1   /* framebuffer, drm      */
#define SUBSYS_CLASS_INPUT   2   /* keyboard, mouse       */
#define SUBSYS_CLASS_BLOCK   3   /* disks                 */
#define SUBSYS_CLASS_NET     4   /* NICs                  */
#define SUBSYS_CLASS_SOUND   5   /* audio codecs          */
#define SUBSYS_CLASS_TTY     6   /* terminals             */
#define SUBSYS_CLASS_MEM     7   /* /dev/null, /dev/zero  */

/* State of a registered subsystem. */
#define SUBSYS_STATE_REGISTERED 0   /* known, init not run / not needed */
#define SUBSYS_STATE_LIVE       1   /* init succeeded, device usable    */
#define SUBSYS_STATE_FAILED     2   /* init ran and failed              */

typedef struct {
    char     name[SUBSYS_NAME_MAX];   /* "fb", "kbd", "e1000", ...          */
    char     dev[SUBSYS_NAME_MAX];    /* /dev node published, "" if none    */
    uint8_t  cls;                     /* SUBSYS_CLASS_*                     */
    uint8_t  state;                   /* SUBSYS_STATE_*                     */
    uint16_t major;                   /* Linux-ish major/minor for coldplug */
    uint16_t minor;
} subsys_t;

/* Reset the registry.  Called once, before any driver init. */
void subsys_init(void);

/*
 * Publish a subsystem.  `dev` may be NULL or "" for a driver with no device
 * node.  Returns the registry slot, or -1 if the table is full or the name is
 * already taken.  Registering is deliberately cheap and cannot fail for lack
 * of memory: the table is static, because a driver that cannot announce
 * itself during early boot has no way to report the problem.
 */
int subsys_register(const char *name, const char *dev, uint8_t cls,
                    uint16_t major, uint16_t minor);

/* Move a slot to LIVE or FAILED once its probe has run. */
void subsys_set_state(int slot, uint8_t state);

/* Look a subsystem up by name; returns its slot or -1. */
int subsys_find(const char *name);

/* How many slots are in use, and read-only access to one of them. */
int             subsys_count(void);
const subsys_t *subsys_get(int slot);

/* Print the whole registry to the debug console, one line per subsystem. */
void subsys_dump(void);

/* Human-readable class name ("graphic", "input", ...) for logging and for
 * the coldplug listing user space reads. */
const char *subsys_class_name(uint8_t cls);

#endif
