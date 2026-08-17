/*
 * subsys.c — the built-in subsystem (driver) registry. (GPLv2)
 *
 * See subsys.h.  The table is static and small: this runs before the heap is
 * necessarily useful and before anything can report an allocation failure, so
 * a fixed array is the honest choice rather than a limitation.
 */
#include <stdint.h>
#include <stddef.h>

#include "subsys.h"
#include "kstring.h"
#include "debugcon.h"

static subsys_t g_tab[SUBSYS_MAX];
static int      g_n;

void subsys_init(void)
{
    memset(g_tab, 0, sizeof(g_tab));
    g_n = 0;
    dbg_puts("SUBSYS: registry ready\r\n");
}

static void copy_name(char *dst, const char *src)
{
    if (!src) {
        dst[0] = 0;
        return;
    }
    strncpy(dst, src, SUBSYS_NAME_MAX - 1);
    dst[SUBSYS_NAME_MAX - 1] = 0;
}

int subsys_register(const char *name, const char *dev, uint8_t cls,
                    uint16_t major, uint16_t minor)
{
    if (!name || !name[0] || g_n >= SUBSYS_MAX)
        return -1;
    if (subsys_find(name) >= 0)
        return -1;

    subsys_t *s = &g_tab[g_n];
    copy_name(s->name, name);
    copy_name(s->dev, dev);
    s->cls   = cls;
    s->state = SUBSYS_STATE_REGISTERED;
    s->major = major;
    s->minor = minor;
    return g_n++;
}

void subsys_set_state(int slot, uint8_t state)
{
    if (slot < 0 || slot >= g_n)
        return;
    g_tab[slot].state = state;
}

int subsys_find(const char *name)
{
    if (!name)
        return -1;
    for (int i = 0; i < g_n; i++)
        if (strcmp(g_tab[i].name, name) == 0)
            return i;
    return -1;
}

int subsys_count(void)
{
    return g_n;
}

const subsys_t *subsys_get(int slot)
{
    if (slot < 0 || slot >= g_n)
        return NULL;
    return &g_tab[slot];
}

const char *subsys_class_name(uint8_t cls)
{
    switch (cls) {
    case SUBSYS_CLASS_GRAPHIC: return "graphic";
    case SUBSYS_CLASS_INPUT:   return "input";
    case SUBSYS_CLASS_BLOCK:   return "block";
    case SUBSYS_CLASS_NET:     return "net";
    case SUBSYS_CLASS_SOUND:   return "sound";
    case SUBSYS_CLASS_TTY:     return "tty";
    case SUBSYS_CLASS_MEM:     return "mem";
    default:                   return "other";
    }
}

static const char *state_name(uint8_t st)
{
    switch (st) {
    case SUBSYS_STATE_LIVE:   return "live";
    case SUBSYS_STATE_FAILED: return "failed";
    default:                  return "registered";
    }
}

void subsys_dump(void)
{
    dbg_puts("SUBSYS: ");
    dbg_puts_dec((uint32_t)g_n);
    dbg_puts(" subsystem(s)\r\n");
    for (int i = 0; i < g_n; i++) {
        const subsys_t *s = &g_tab[i];
        dbg_puts("SUBSYS:   ");
        dbg_puts(s->name);
        dbg_puts(" class=");
        dbg_puts(subsys_class_name(s->cls));
        dbg_puts(" state=");
        dbg_puts(state_name(s->state));
        if (s->dev[0]) {
            dbg_puts(" dev=/dev/");
            dbg_puts(s->dev);
            dbg_puts(" ");
            dbg_puts_dec(s->major);
            dbg_puts(":");
            dbg_puts_dec(s->minor);
        }
        dbg_puts("\r\n");
    }
}
