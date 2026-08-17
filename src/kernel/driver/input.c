/*
 * input.c — the Linux evdev UAPI on the i8042 keyboard and mouse. (GPLv2)
 *
 * What libinput (and through it wlroots) needs from a keyboard/mouse, in
 * the exact struct layouts and ioctl numbers of the Linux UAPI:
 *
 *   - /dev/input/event0 (keyboard) and /dev/input/event1 (mouse), each a
 *     queue of struct input_event (24-byte time/type/code/value records)
 *     fed from the PS/2 IRQs;
 *   - EVIOCG* identity queries (version, id, name, capability bitmaps)
 *     and EVIOCGRAB, all of which libinput issues at open time;
 *   - blocking read(), non-blocking EAGAIN, and poll() readiness through
 *     the vfs poll hook;
 *   - EV_KEY with Linux KEY_* codes (scancode set 1 maps almost 1:1),
 *     EV_REL deltas from the mouse's three-byte packets, EV_SYN framing.
 *
 * The keyboard side is deliberately a *parallel* decoder to the one in
 * tty.c: the terminal needs its own map (CJK-aware, terminal-switch
 * hotkeys, line discipline), evdev clients need raw key codes, and the two
 * never need to agree about anything except that a PS/2 byte means the
 * same physical key.  Both decoders keep their own 0xE0 state.
 *
 * i8042 wrinkle: the controller's data port is shared, and tty.c's IRQ 1
 * drain would swallow mouse bytes sitting in the buffer.  The status
 * register bit 5 says the pending byte came from the auxiliary port, so
 * tty.c skips those and this driver's IRQ 12 handler drains exactly them.
 */
#include <stddef.h>
#include <stdint.h>
#include "vfs.h"
#include "proc.h"
#include "timer.h"
#include "idt.h"
#include "kstring.h"
#include "debugcon.h"
#include "io.h"
#include "input.h"

/* ---- i8042 -------------------------------------------------------------- */
#define KBD_DATA 0x60
#define KBD_CMD  0x64
#define KBD_STATUS 0x64
#define ST_OUT_BUF   0x01
#define ST_INPT_BUF  0x02
#define ST_AUX_DATA  0x20

/* ---- Linux input UAPI (values from <linux/input.h>) --------------------- */
#define EV_SYN 0x00
#define EV_KEY 0x01
#define EV_REL 0x02
#define EV_ABS 0x03
#define EV_LED 0x11

#define SYN_REPORT 0

#define REL_X 0x00
#define REL_Y 0x01

#define BTN_LEFT   0x110
#define BTN_RIGHT  0x111
#define BTN_MIDDLE 0x112

#define KEY_RESERVED 0

/* EV_VERSION the kernel reports -- 1.1.0, what Linux says today. */
#define EV_VERSION_MAJOR 0x01
#define EV_VERSION_MINOR 0x01
#define EV_VERSION_PATCH 0x00

#define BUS_I8042 0x11

/* The 24-byte record read() delivers.  Linux's own layout: a realtime
 * timeval (16 bytes), then the 2+2+4 type/code/value triple. */
typedef struct {
    int64_t  tv_sec;
    int64_t  tv_usec;
    uint16_t type;
    uint16_t code;
    int32_t  value;
} input_event_t;

typedef struct {
    uint32_t     head, tail, count;
    input_event_t ev[64];
} evdev_q_t;

typedef struct {
    evdev_q_t    *q;
    const char   *name;
    uint16_t      vendor;
    uint16_t      product;
    uint16_t      kind_bits[8];        /* EV_SYN/EV_KEY/EV_REL/EV_LED mask */
    uint16_t      key_bits[64];        /* 1024 keycodes */
    uint16_t      rel_bits[2];         /* 32 relative axes */
    int           grabbed;
    int           nonblock;
} evdev_dev_t;

static evdev_q_t  g_kbd_q, g_mouse_q;
static evdev_dev_t g_kbd_dev = { .q = &g_kbd_q,
                                 .name = "GNOS i8042 keyboard",
                                 .vendor = 0x0001, .product = 0x0001 };
static evdev_dev_t g_mouse_dev = { .q = &g_mouse_q,
                                   .name = "GNOS i8042 mouse",
                                   .vendor = 0x0002, .product = 0x0002 };

static void evdev_push(evdev_dev_t *d, uint16_t type, uint16_t code,
                       int32_t value)
{
    evdev_q_t *q = d->q;
    if (q->count == 64)
        return;                          /* full: drop, like Linux's overflow */
    uint32_t i = q->tail;
    input_event_t *e = &q->ev[i];
    e->tv_sec  = (int64_t)(timer_ticks() / SCHED_HZ + timer_boot_epoch());
    e->tv_usec = (int64_t)((timer_ticks() % SCHED_HZ) * (1000000ULL / SCHED_HZ));
    e->type  = type;
    e->code  = code;
    e->value = value;
    q->tail = (q->tail + 1) % 64;
    q->count++;
}

/* A queued event is exactly what a poll() sleeper was waiting for.  The
 * poll core parks evdev waiters in the WAIT_PIPE channel (see do_ppoll),
 * so waking that channel here is what makes blocking poll()/read() return. */
static void evdev_signal(void)
{
    sched_wake_reason(WAIT_PIPE);
}

/* ---- scancode set 1 -> Linux KEY_* --------------------------------------
 * The identity mapping is the gift that keeps on giving: for non-extended
 * make codes 0x01..0x53 the set-1 scan code IS the evdev code (KEY_ESC=1,
 * KEY_1=2, ..., KEY_SPACE=57, KEY_CAPSLOCK=58, KEY_F1..F12=59..69+18,
 * KEY_NUMLOCK=69, KEY_KP7..KPDOT=71..83).  Only the few stragglers and
 * every 0xE0-prefixed key need a table. */
static uint16_t scancode_to_key(uint8_t sc, int ext)
{
    if (ext) {
        switch (sc) {
        case 0x1C: return 96;   /* keypad enter */
        case 0x1D: return 97;   /* right ctrl */
        case 0x35: return 98;   /* keypad / */
        case 0x38: return 100;  /* right alt */
        case 0x47: return 102;  /* home */
        case 0x48: return 103;  /* up */
        case 0x49: return 104;  /* page up */
        case 0x4B: return 105;  /* left */
        case 0x4D: return 106;  /* right */
        case 0x4F: return 107;  /* end */
        case 0x50: return 108;  /* down */
        case 0x51: return 109;  /* page down */
        case 0x52: return 110;  /* insert */
        case 0x53: return 111;  /* delete */
        case 0x5B: return 125;  /* left meta */
        case 0x5C: return 126;  /* right meta */
        case 0x5D: return 127;  /* menu */
        }
        return KEY_RESERVED;
    }
    switch (sc) {
    case 0x54: return 84;   /* sysrq */
    case 0x57: return 87;   /* f11 */
    case 0x58: return 88;   /* f12 */
    }
    return sc;
}

static void kbd_translate(uint8_t sc)
{
    static int ext;
    static int e1;

    if (e1) {                            /* 0xE1 pause: swallow the tail */
        e1 = 0;
        return;
    }
    if (sc == 0xE1) {
        e1 = 1;
        return;
    }
    if (sc == 0xE0) {
        ext = 1;
        return;
    }

    uint16_t key = scancode_to_key(sc & 0x7F, ext);
    ext = 0;
    if (!key)
        return;

    evdev_push(&g_kbd_dev, EV_KEY, key, (sc & 0x80) ? 0 : 1);
    evdev_push(&g_kbd_dev, EV_SYN, SYN_REPORT, 0);
    evdev_signal();
}

/* ---- PS/2 mouse: three-byte packets, MSB-first deltas ------------------- */
static uint8_t g_mouse_pkt[3];
static int     g_mouse_n;

static void mouse_byte(uint8_t b)
{
    g_mouse_pkt[g_mouse_n++] = b;
    if (g_mouse_n < 3)
        return;
    g_mouse_n = 0;

    uint8_t  flags = g_mouse_pkt[0];
    int8_t   dx = (int8_t)g_mouse_pkt[1];
    int8_t   dy = (int8_t)g_mouse_pkt[2];

    evdev_push(&g_mouse_dev, EV_KEY, BTN_LEFT,   flags & 0x01);
    evdev_push(&g_mouse_dev, EV_KEY, BTN_RIGHT,  (flags >> 1) & 0x01);
    evdev_push(&g_mouse_dev, EV_KEY, BTN_MIDDLE, (flags >> 2) & 0x01);
    evdev_push(&g_mouse_dev, EV_REL, REL_X, dx);
    evdev_push(&g_mouse_dev, EV_REL, REL_Y, dy);
    evdev_push(&g_mouse_dev, EV_SYN, SYN_REPORT, 0);
    evdev_signal();
}

static void mouse_irq(regs_t *r)
{
    (void)r;
    while ((inb(KBD_STATUS) & (ST_OUT_BUF | ST_AUX_DATA)) ==
           (ST_OUT_BUF | ST_AUX_DATA))
        mouse_byte(inb(KBD_DATA));
}

/* ---- the /dev/input/eventN side ------------------------------------------ */
static int32_t evdev_read(vfs_node_t *n, uint64_t off, void *buf, uint32_t len)
{
    (void)off;
    evdev_dev_t *d = (evdev_dev_t *)n->priv;
    evdev_q_t   *q = d->q;
    uint32_t want = len / sizeof(input_event_t);
    if (want == 0)
        return 0;                        /* whole events only */

    /* Test-and-sleep atomic against the IRQ, the same cli/schedule dance
     * pipe_node_read uses. */
    asm volatile("cli");
    while (q->count == 0) {
        proc_t *me = proc_current();
        if (d->nonblock) {
            asm volatile("sti");
            return -E_AGAIN;
        }
        if (!me || proc_pending_signals(me)) {
            asm volatile("sti");
            return -E_INTR;
        }
        sched_block_irqoff(WAIT_PIPE);
    }

    uint32_t nout = want < q->count ? want : q->count;
    for (uint32_t i = 0; i < nout; i++) {
        memcpy((uint8_t *)buf + i * sizeof(input_event_t),
               &q->ev[q->head], sizeof(input_event_t));
        q->head = (q->head + 1) % 64;
        q->count--;
    }
    asm volatile("sti");
    return (int32_t)(nout * sizeof(input_event_t));
}

static int evdev_poll(vfs_node_t *n, int16_t events, int16_t *revents)
{
    evdev_dev_t *d = (evdev_dev_t *)n->priv;
    int16_t r = 0;
    if ((events & POLLIN) && d->q->count)
        r |= POLLIN;
    *revents = r;
    return 0;
}

/* fcntl(F_SETFL, O_NONBLOCK) mirror: libevdev flips the flag through fcntl
 * as often as at open.  Open-time O_NONBLOCK is not plumbed to the node by
 * the VFS, which is fine -- every real client (libinput, evtest) uses
 * poll() to gate reads anyway, and the mirror keeps those that fcntl. */
void input_set_nonblock(vfs_node_t *n, int nb)
{
    if (n->priv == &g_kbd_dev || n->priv == &g_mouse_dev)
        ((evdev_dev_t *)n->priv)->nonblock = nb;
}

/* ---- EVIOC* ioctls (the Linux numbers) ----------------------------------- */
#define EVIOCGVERSION   0x80044501
#define EVIOCGID        0x80084502
#define EVIOCGNAME(len) (0x80000000 | ((len) << 16) | 0x4506)
#define EVIOCGBIT(ev, len) (0x80000000 | ((len) << 16) | (0x45 << 8) | 0x20)
#define EVIOCGRAB       0x40044590
#define EVIOCSCLOCKID   0x400445A0

#define EV_CNT (EV_LED + 1)

static int32_t evdev_ioctl(vfs_node_t *n, uint64_t cmd, uint64_t arg)
{
    evdev_dev_t *d = (evdev_dev_t *)n->priv;
    uint64_t u = arg;

    switch (cmd) {
    case EVIOCGVERSION: {
        if (!user_ptr_ok(u, 4)) return -E_FAULT;
        uint32_t v = (EV_VERSION_MAJOR << 16) | (EV_VERSION_MINOR << 8) |
                     EV_VERSION_PATCH;
        memcpy((void *)(uintptr_t)u, &v, 4);
        return 0;
    }
    case EVIOCGID: {
        if (!user_ptr_ok(u, 8)) return -E_FAULT;
        /* struct input_id { bus, vendor, product, version }: 4 shorts. */
        uint16_t id[4] = { BUS_I8042, d->vendor, d->product, 0x0100 };
        memcpy((void *)(uintptr_t)u, id, sizeof(id));
        return 0;
    }
    case EVIOCGRAB: {
        if (!user_ptr_ok(u, 4)) return -E_FAULT;
        d->grabbed = (int)(*(int32_t *)(uintptr_t)u);
        return 0;
    }
    case EVIOCSCLOCKID:
        return 0;                        /* we always report realtime */
    }
    /* EVIOCGNAME(len) and EVIOCGBIT(ev,len) carry their length in the
     * command word; decode it like Linux's _IOC_SIZE.  EVIOCGBIT puts the
     * event type in the *nr* field: 0x20 + ev. */
    unsigned dir  = (unsigned)(cmd >> 30);
    unsigned size = (unsigned)((cmd >> 16) & 0x3FFF);
    unsigned nr   = (unsigned)(cmd & 0xFF);

    if (dir == 2 /* _IOC_READ */) {
        if (nr == 0x06) {                /* EVIOCGNAME */
            const char *name = d->name;
            uint32_t len = (uint32_t)size;
            if (!user_ptr_ok(u, len)) return -E_FAULT;
            uint32_t n = (uint32_t)strlen(name);
            if (n > len) n = len;
            memcpy((void *)(uintptr_t)u, name, n);
            return (int32_t)n;
        }
        if (nr >= 0x20 && nr < 0x20 + EV_CNT) {   /* EVIOCGBIT(ev, len) */
            unsigned ev = nr - 0x20;
            uint32_t len = (uint32_t)size;
            if (!user_ptr_ok(u, len)) return -E_FAULT;
            memset((void *)(uintptr_t)u, 0, len);
            uint16_t *bits = NULL;
            uint32_t nbits = 0;
            if (ev == EV_SYN) { bits = d->kind_bits; nbits = EV_CNT * 16; }
            else if (ev == EV_KEY) { bits = d->key_bits; nbits = 1024; }
            else if (ev == EV_REL) { bits = d->rel_bits; nbits = 32; }
            if (bits) {
                uint32_t bytes = (nbits + 7) / 8;
                if (bytes > len) bytes = len;
                memcpy((void *)(uintptr_t)u, bits, bytes);
            }
            return 0;
        }
        if (nr == 0x08)                  /* EVIOCGUNIQ: nothing to say */
            return 0;
    }
    return -E_INVAL;
}

static const vfs_ops_t g_evdev_ops = {
    .read  = evdev_read,
    .ioctl = evdev_ioctl,
    .poll  = evdev_poll,
};

/* ---- init ---------------------------------------------------------------- */
static void kbd_enable_aux(void)
{
    while (inb(KBD_STATUS) & ST_INPT_BUF)
        ;
    outb(KBD_CMD, 0xA8);                 /* enable auxiliary port */
    while (inb(KBD_STATUS) & ST_INPT_BUF)
        ;
    outb(KBD_CMD, 0x60);                 /* write command byte */
    while (inb(KBD_STATUS) & ST_INPT_BUF)
        ;
    uint8_t cmd = inb(KBD_DATA);
    cmd |= 0x02;                          /* enable IRQ 12 */
    outb(KBD_DATA, cmd);
    while (inb(KBD_STATUS) & ST_INPT_BUF)
        ;
    outb(KBD_DATA, 0xF4);                /* mouse: start reporting */
    while (inb(KBD_STATUS) & 1)          /* drain its ack (0xFA) */
        (void)inb(KBD_DATA);
}

void input_init(void)
{
    /* Capability bitmaps, built once: the keyboard says SYN/KEY/LED, the
     * mouse says SYN/KEY/REL.  Keys cover every code scancode_to_key can
     * produce, so libinput's probe finds everything it might ask for. */
    g_kbd_dev.kind_bits[EV_SYN]  |= (1u << EV_SYN);
    g_kbd_dev.kind_bits[EV_KEY]  |= (1u << EV_KEY);
    g_kbd_dev.kind_bits[EV_LED]  |= (1u << EV_LED);
    for (uint16_t sc = 1; sc < 128; sc++) {
        uint16_t k = scancode_to_key((uint8_t)sc, 0);
        if (k && k < 1024)
            g_kbd_dev.key_bits[k >> 4] |= (uint16_t)(1u << (k & 15));
    }
    static const uint16_t extkeys[] = {
        96, 97, 98, 100, 102, 103, 104, 105, 106, 107, 108, 109,
        110, 111, 125, 126, 127,
    };
    for (uint32_t i = 0; i < sizeof(extkeys) / sizeof(extkeys[0]); i++)
        g_kbd_dev.key_bits[extkeys[i] >> 4] |=
            (uint16_t)(1u << (extkeys[i] & 15));

    g_mouse_dev.kind_bits[EV_SYN] |= (1u << EV_SYN);
    g_mouse_dev.kind_bits[EV_KEY] |= (1u << EV_KEY);
    g_mouse_dev.kind_bits[EV_REL] |= (1u << EV_REL);
    g_mouse_dev.key_bits[BTN_LEFT >> 4]   |= (uint16_t)(1u << (BTN_LEFT & 15));
    g_mouse_dev.key_bits[BTN_RIGHT >> 4]  |= (uint16_t)(1u << (BTN_RIGHT & 15));
    g_mouse_dev.key_bits[BTN_MIDDLE >> 4] |= (uint16_t)(1u << (BTN_MIDDLE & 15));
    g_mouse_dev.rel_bits[REL_X >> 4] |= (uint16_t)(1u << (REL_X & 15));
    g_mouse_dev.rel_bits[REL_Y >> 4] |= (uint16_t)(1u << (REL_Y & 15));

    kbd_enable_aux();
    irq_install(12, mouse_irq);

    if (vfs_register_dev("input/event0", &g_evdev_ops, &g_kbd_dev) != 0 ||
        vfs_register_dev("input/event1", &g_evdev_ops, &g_mouse_dev) != 0) {
        dbg_puts("INPUT: failed to publish /dev/input/eventN\n");
        return;
    }
    dbg_puts("INPUT: /dev/input/event0 keyboard, event1 mouse\n");
}

/* ---- the keyboard hook tty.c calls for every PS/2 byte -------------------- */
void input_kbd_scancode(uint8_t sc)
{
    kbd_translate(sc);
}

/* inputinject(405): push a synthetic event into event0 for headless tests. */
int64_t sys_inputinject(uint64_t type, uint64_t code, uint64_t value)
{
    if (type > 0xFF || code > 0xFFFF)
        return -E_INVAL;
    evdev_push(&g_kbd_dev, (uint16_t)type, (uint16_t)code, (int32_t)value);
    evdev_signal();
    return 0;
}
