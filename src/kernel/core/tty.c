/*
 * tty.c — virtual terminals with a termios line discipline. (GPLv2)
 *
 * There are NR_VT terminals over one framebuffer and one keyboard.  Each owns
 * a line discipline, an input ring, a termios, a foreground process group and
 * an fbcon console; exactly one is *active*, meaning it is the one on screen
 * and the one the keyboard feeds.  Ctrl-Alt-F1..F6, or the VT_ACTIVATE ioctl,
 * moves that focus.  Everything else -- a shell printing on tty3 while the
 * user reads tty1 -- goes on happening into the inactive console's cell
 * buffer and appears intact the moment it is switched to.
 *
 * Input comes from the PS/2 controller on IRQ 1: the handler translates
 * scancode set 1 into ASCII and hands each byte to the *active* terminal's
 * discipline, which edits a line buffer, echoes, and pushes finished input
 * into that terminal's ring.  Any process asleep waiting for input is then
 * woken.  The interrupt handler never blocks and never copies to user memory.
 *
 * What the discipline actually *does* is not hard-coded: it is driven by one
 * struct termios per terminal, the same one user space reads and writes
 * through tcgetattr()/tcsetattr() (ioctl TCGETS/TCSETS*).  Turn off ICANON
 * and reads stop waiting for Enter; turn off ECHO and nothing appears on
 * screen; clear ISIG and Ctrl-C becomes an ordinary byte.
 *
 * Job control lives here as well, because the terminal is the only thing
 * that knows which keys were pressed:
 *
 *   c_cc[VINTR] (^C)  -> SIGINT  to the foreground process group
 *   c_cc[VQUIT] (^\)  -> SIGQUIT to the foreground process group
 *   c_cc[VSUSP] (^Z)  -> SIGTSTP to the foreground process group
 *   c_cc[VEOF]  (^D)  -> end of input (read() returns 0)
 *
 * and a read() from a *background* group raises SIGTTIN on the reader, which
 * by default stops it -- the POSIX rule that keeps two jobs from fighting
 * over the same keyboard.  A background write is only stopped when TOSTOP is
 * set, and in both cases a process that is ignoring or blocking the signal is
 * let through instead of being stopped for ever: that exemption is what keeps
 * a shell (which ignores SIGTTOU precisely so it can pass the terminal back
 * and forth) from deadlocking against its own tty.
 */
#include <stddef.h>
#include <stdint.h>

#include "tty.h"
#include "subsys.h"
#include "fbcon.h"
#include "idt.h"
#include "vmm.h"
#include "vfs.h"
#include "proc.h"
#include "timer.h"
#include "debugcon.h"
#include "kstring.h"
#include "input.h"

#define KBD_DATA   0x60
#define KBD_STATUS 0x64

#define LINE_MAX   256
#define RING_SIZE  1024

/*
 * End-of-file is kept *out of band*.  Storing it as the byte 0x04 would make
 * a raw-mode read of a literal Ctrl-D indistinguishable from end of input, so
 * the ring holds 16-bit cells and the marker uses a value no byte can have.
 */
#define RING_EOF   0x100u

/* The PIT runs at SCHED_HZ (100) Hz and c_cc[VTIME] counts tenths of a
 * second, so one VTIME unit is ten ticks. */
#define TICKS_PER_DECISEC 10

/* One virtual terminal.  Everything that used to be a file-scope singleton
 * now lives in here, one copy per terminal. */
typedef struct {
    int      con;                    /* the fbcon console it draws on */

    char     line[LINE_MAX];         /* the line being edited (canonical) */
    uint32_t line_len;

    uint16_t ring[RING_SIZE];        /* finished input waiting to be read */
    volatile uint32_t head, tail;

    termios_t tio;                   /* this terminal's line discipline */
    int      fg_pgid;                /* foreground group, 0 = nobody */
    int      sid;                    /* session that claimed it, 0 = free */
} vt_t;

static vt_t g_vt[NR_VT];
static int  g_active;                /* the terminal on screen */

/* Modifier state.  The keyboard is one device shared by every terminal, so
 * unlike the discipline state this really is global. */
static int g_shift, g_caps, g_ctrl, g_alt;

/* Set after a 0xE0 prefix byte: the next scan code is the second half of an
 * extended sequence (cursor keys, edit keys, right-hand modifiers). */
static int g_ext;

static inline uint8_t inb(uint16_t port)
{
    uint8_t v;
    asm volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void outb(uint16_t port, uint8_t v)
{
    asm volatile("outb %0, %1" : : "a"(v), "Nd"(port));
}

/* The 8042 command port, separate from the keyboard data port. */
#define KBD_CMD     0x64
#define KBD_ENABLE  0xAE            /* enable the first PS/2 port (keyboard) */
#define KBD_DISABLE 0xAD

/*
 * Extended (0xE0-prefixed) make codes that produce text.  Indexed by make
 * code; NULL means "nothing to type" (a bare modifier, a release, ...).  The
 * strings are the ANSI escape sequences a real terminal emits for the
 * cursor and editing keys, which is exactly what readline expects in raw
 * mode -- so the arrow keys move the cursor instead of printing garbage.
 */
static const char * const kbd_ext_map[128] = {
    [0x47] = "\033[1~",   /* Home     */
    [0x48] = "\033[A",    /* Up       */
    [0x49] = "\033[5~",   /* PageUp   */
    [0x4B] = "\033[D",    /* Left     */
    [0x4D] = "\033[C",    /* Right    */
    [0x4F] = "\033[4~",   /* End      */
    [0x50] = "\033[B",    /* Down     */
    [0x51] = "\033[6~",   /* PageDown */
    [0x52] = "\033[2~",   /* Insert   */
    [0x53] = "\033[3~",   /* Delete   */
};

/*
 * Scancode set 1, US layout.  Index = make code, 0 = no ASCII meaning.
 *
 * Enter produces CR and Backspace produces DEL, which is what a real
 * terminal sends.  Turning CR into NL is the line discipline's job (ICRNL),
 * and DEL is what c_cc[VERASE] defaults to -- so a program that clears ICRNL
 * really does see the carriage return, instead of us having decided for it.
 */
static const char kbd_map[128] = {
    0,   27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 0x7F,
    '\t','q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\r',
    0,   'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0,   '\\','z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/',
    0,   '*', 0,   ' ',
};

static const char kbd_map_shift[128] = {
    0,   27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', 0x7F,
    '\t','Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\r',
    0,   'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0,   '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?',
    0,   '*', 0,   ' ',
};

/* ---- which terminal is this call about? -------------------------------- */
/*
 * /dev/tty is the odd one out.  Every /dev/ttyN node carries a pointer to its
 * own vt_t in node->priv, so a descriptor names a terminal directly; /dev/tty
 * carries NULL and means "my controlling terminal", which has to be looked up
 * in the caller's PCB every time.  Falling back to the active terminal covers
 * the two cases with no caller to ask: the kernel itself, and a process that
 * has never claimed a controlling terminal.
 */
static vt_t *vt_current(void)
{
    proc_t *p = proc_current();
    if (p && p->ctty >= 0 && p->ctty < NR_VT)
        return &g_vt[p->ctty];
    return &g_vt[g_active];
}

static vt_t *vt_of_node(const vfs_node_t *n)
{
    if (n && n->priv)
        return (vt_t *)n->priv;
    return vt_current();
}

static int vt_index(const vt_t *v)
{
    return (int)(v - g_vt);
}

int tty_vt_active(void)
{
    return g_active;
}

/* ---- the input ring ---------------------------------------------------- */
static int ring_empty(const vt_t *v) { return v->head == v->tail; }

static uint32_t ring_avail(const vt_t *v)
{
    return (v->head + RING_SIZE - v->tail) % RING_SIZE;
}

uint32_t tty_input_avail(void)
{
    return ring_avail(vt_current());
}

uint32_t tty_node_input_avail(const vfs_node_t *n)
{
    return ring_avail(vt_of_node(n));
}

static void ring_put(vt_t *v, uint16_t c)
{
    uint32_t next = (v->head + 1) % RING_SIZE;
    if (next == v->tail)
        return;                     /* full: drop, we have nowhere to block */
    v->ring[v->head] = c;
    v->head = next;
}

static void ring_flush(vt_t *v)
{
    v->head = v->tail = 0;
}

/* ---- echo -------------------------------------------------------------- */
/*
 * Output goes to this terminal's console, and is teed to the debug port as
 * well.  The tee is not decoration: `make test` runs headless and the debug
 * log is the only place the boot can be read afterwards, so a getty prompt on
 * tty4 has to show up there even though nothing ever paints it.
 */
static void out_char(vt_t *v, char c)
{
    fbcon_putc_on(v->con, c);
    dbg_putc(c);
}

static void out_str(vt_t *v, const char *s)
{
    for (; *s; s++)
        out_char(v, *s);
}
/*
 * Echo one input byte the way c_lflag says to.  With ECHOCTL a control
 * character shows up as ^X rather than doing whatever the console would make
 * of it -- that is why a Ctrl-A does not blank half the screen.
 */
static void echo_char(vt_t *v, uint8_t c)
{
    if (!(v->tio.c_lflag & ECHO))
        return;

    if ((v->tio.c_lflag & ECHOCTL) && c < 0x20 &&
        c != '\n' && c != '\r' && c != '\t') {
        out_char(v, '^');
        out_char(v, (char)(c + '@'));
        return;
    }
    if ((v->tio.c_lflag & ECHOCTL) && c == 0x7F) {
        out_str(v, "^?");
        return;
    }
    out_char(v, (char)c);
}

/* Rub out the last echoed character (ECHOE).  fbcon's '\b' already draws a
 * blank over the glyph and leaves the cursor on it, so one is enough. */
static void echo_erase(vt_t *v)
{
    if ((v->tio.c_lflag & (ECHO | ECHOE)) == (ECHO | ECHOE))
        out_char(v, '\b');
}

/* ---- canonical line buffer --------------------------------------------- */
static void line_flush(vt_t *v, int with_newline)
{
    for (uint32_t i = 0; i < v->line_len; i++)
        ring_put(v, (uint8_t)v->line[i]);
    if (with_newline)
        ring_put(v, '\n');
    v->line_len = 0;
    sched_wake_reason(WAIT_TTY);
}

/* ---- the line discipline ----------------------------------------------- */
static void vt_input_char(vt_t *v, uint8_t c)
{
    /* ---- c_iflag: input mapping ---------------------------------------- */
    if (v->tio.c_iflag & ISTRIP)
        c &= 0x7F;

    if (c == '\r') {
        if (v->tio.c_iflag & IGNCR)
            return;
        if (v->tio.c_iflag & ICRNL)
            c = '\n';
    } else if (c == '\n') {
        if (v->tio.c_iflag & INLCR)
            c = '\r';
    }

    /* ---- c_lflag & ISIG: the keys that generate signals ----------------- */
    if (v->tio.c_lflag & ISIG) {
        int sig = 0;
        if (c == v->tio.c_cc[VINTR])      sig = SIGINT;
        else if (c == v->tio.c_cc[VQUIT]) sig = SIGQUIT;
        else if (c == v->tio.c_cc[VSUSP]) sig = SIGTSTP;

        if (sig) {
            echo_char(v, c);
            if (v->tio.c_lflag & ECHO)
                out_char(v, '\n');
            v->line_len = 0;            /* the half-typed line is gone */

            if (v->fg_pgid)
                proc_signal_group(v->fg_pgid, sig);

            /* A reader blocked in the foreground group has to come back out
             * of the kernel for the signal to take effect. */
            sched_wake_reason(WAIT_TTY);
            return;
        }
    }

    /* ---- c_iflag & IXON: software flow control -------------------------- */
    if (v->tio.c_iflag & IXON) {
        /* Output never backs up on a framebuffer, so ^S/^Q have nothing to
         * stop or start.  They are still swallowed rather than delivered,
         * which is what a program that leaves IXON on expects. */
        if (c == v->tio.c_cc[VSTOP] || c == v->tio.c_cc[VSTART])
            return;
    }

    /* ---- non-canonical: every byte goes straight through ---------------- */
    if (!(v->tio.c_lflag & ICANON)) {
        echo_char(v, c);
        ring_put(v, c);
        sched_wake_reason(WAIT_TTY);
        return;
    }

    /* ---- canonical: assemble a line ------------------------------------- */
    if (c == v->tio.c_cc[VEOF]) {
        if (v->line_len)
            line_flush(v, 0);           /* deliver the partial line */
        else {
            ring_put(v, RING_EOF);
            sched_wake_reason(WAIT_TTY);
        }
        return;
    }

    if (c == v->tio.c_cc[VERASE]) {
        if (v->line_len) {
            v->line_len--;
            echo_erase(v);
        }
        return;
    }

    if (c == v->tio.c_cc[VKILL]) {
        if (v->tio.c_lflag & ECHOKE) {
            while (v->line_len) {
                v->line_len--;
                echo_erase(v);
            }
        } else {
            v->line_len = 0;
            if ((v->tio.c_lflag & (ECHO | ECHOK)) == (ECHO | ECHOK))
                out_char(v, '\n');
        }
        return;
    }

    if (c == '\n' || (v->tio.c_cc[VEOL] && c == v->tio.c_cc[VEOL])) {
        if (v->tio.c_lflag & (ECHO | ECHONL))
            out_char(v, '\n');
        line_flush(v, 1);
        return;
    }

    if (v->line_len < LINE_MAX - 1) {
        v->line[v->line_len++] = (char)c;
        echo_char(v, c);
    }
}

/*
 * The single entry point for a real key press: used only by the keyboard IRQ
 * after scancode decode.  Keystrokes always land on the terminal that is on
 * screen -- that is what "active" means.
 */
void tty_input_char(uint8_t c)
{
    vt_input_char(&g_vt[g_active], c);
}

/* Feed a whole buffer through the line discipline, byte by byte.  This is
 * the kernel side of the ttyinject(404) syscall: a test program hands us a
 * string of "keystrokes" and we treat each one exactly as the IRQ would have.
 *
 * Unlike a real key, injected input targets the *calling* terminal (its
 * controlling terminal), not whatever happens to be on screen.  A reader
 * always pulls from its own ctty, so injecting anywhere else would let a
 * stray VT switch during the test strand the reader's VEOF on a different
 * terminal and hang it forever -- which is exactly the boot-time "readline
 * scrolls on its own and the network test never runs" symptom. */
void tty_inject(const char *buf, uint32_t len)
{
    vt_t *v = vt_current();
    for (uint32_t i = 0; i < len; i++)
        vt_input_char(v, (uint8_t)buf[i]);
}
/* ---- switching terminals ----------------------------------------------- */
/*
 * Moving the focus is almost free: the incoming terminal's console already
 * holds every character ever written to it, so fbcon repaints from cells and
 * nothing has to be replayed.  What does *not* move is any of the discipline
 * state -- a half-typed line on tty1 is still sitting there when you come
 * back, which is exactly the behaviour a real Linux console has.
 *
 * The sleeping-reader wakeup is the subtle part.  A process blocked in
 * tty_read() on the terminal being switched *away from* is still correctly
 * blocked, but VT_WAITACTIVE sleepers are waiting on precisely this event, so
 * everyone parked on WAIT_TTY gets a look.  They re-check their own condition
 * and go back to sleep if it has not been met.
 */
void tty_vt_switch(int n)
{
    if (n < 0 || n >= NR_VT || n == g_active)
        return;

    /* The Ctrl-Alt that triggered the switch is, by definition, still held:
     * its release scancodes belong to the *new* terminal's key stream and may
     * never arrive (the user let go while another VT was up, or the release
     * was dropped).  Clearing the modifier state here means a switch can never
     * leave Ctrl/Alt latched on -- which would otherwise turn the next F-key
     * into another silent switch, or the next letter into a control char. */
    g_shift = g_ctrl = g_alt = g_ext = 0;

    g_active = n;
    fbcon_activate(g_vt[n].con);
    sched_wake_reason(WAIT_TTY);
}

void tty_vt_release_session(int sid)
{
    if (!sid)
        return;
    for (int i = 0; i < NR_VT; i++)
        if (g_vt[i].sid == sid) {
            g_vt[i].sid     = 0;
            g_vt[i].fg_pgid = 0;
        }
}

/* ---- keyboard interrupt ------------------------------------------------ */
/*
 * Ctrl-Alt-F1..F6.  Function keys F1..F6 are make codes 0x3B..0x40 in
 * scancode set 1, contiguous, so the terminal index is a subtraction.  This
 * is checked before the key is translated into text: the combination must
 * never reach the line discipline, or every switch would also type garbage
 * into whatever shell was listening.
 */
#define KEY_F1  0x3B
#define KEY_F6  0x40

static int vt_hotkey(uint8_t code)
{
    if (!g_ctrl || !g_alt)
        return 0;
    if (code < KEY_F1 || code > KEY_F6)
        return 0;
    tty_vt_switch(code - KEY_F1);
    return 1;
}

/*
 * Decode one scancode into the line discipline (or into the modifier/VT
 * state).  Pulled out of kbd_irq so the IRQ handler can drain every byte the
 * 8042 has queued without re-reading the data port.
 */
static void kbd_feed(uint8_t sc)
{
    /* The evdev decoder runs first: it wants every byte, prefix included,
     * and keeps its own 0xE0 state -- see input.c. */
    input_kbd_scancode(sc);

    /* 0xE0 begins an extended sequence: remember it and wait for the real
     * code on the next IRQ.  0xE1 (Pause/Break) is a multi-byte escape we do
     * not decode, so just drop the prefix and the following bytes. */
    if (sc == 0xE0 || sc == 0xE1) {
        g_ext = (sc == 0xE0);
        return;
    }

    int release = sc & 0x80;
    uint8_t code = (uint8_t)(sc & 0x7F);

    /* Second half of an extended sequence.  Modifier releases do nothing;
     * key releases are ignored; otherwise the make code yields an escape
     * sequence (or sets a right-hand modifier). */
    if (g_ext) {
        g_ext = 0;
        if (release) {
            /* Right-hand modifiers have to be *cleared* on release too, or
             * AltGr would leave g_alt stuck on and the next F-key would
             * silently switch terminals. */
            if (code == 0x1D)
                g_ctrl = 0;
            else if (code == 0x38)
                g_alt = 0;
            return;
        }
        if (code == 0x1D) {             /* right control */
            g_ctrl = 1;
            return;
        }
        if (code == 0x38) {             /* right alt */
            g_alt = 1;
            return;
        }
        const char *seq = kbd_ext_map[code];
        if (seq) {
            while (*seq)
                tty_input_char((uint8_t)*seq++);
        }
        return;
    }

    switch (code) {
    case 0x2A: case 0x36:            /* left / right shift */
        g_shift = !release;
        return;
    case 0x1D:                       /* left control */
        g_ctrl = !release;
        return;
    case 0x38:                       /* left alt */
        g_alt = !release;
        return;
    case 0x3A:                       /* caps lock */
        if (!release)
            g_caps = !g_caps;
        return;
    }
    if (release)
        return;

    /* Terminal switching wins over typing, and consumes the key. */
    if (vt_hotkey(code))
        return;

    char c = g_shift ? kbd_map_shift[code] : kbd_map[code];
    if (!c)
        return;

    if (g_caps && c >= 'a' && c <= 'z')
        c = (char)(c - 'a' + 'A');
    else if (g_caps && g_shift && c >= 'A' && c <= 'Z')
        c = (char)(c - 'A' + 'a');

    if (g_ctrl && ((c | 0x20) >= 'a' && (c | 0x20) <= 'z'))
        c = (char)((c | 0x20) - 'a' + 1);      /* Ctrl-A .. Ctrl-Z */
    else if (g_ctrl && c == '\\')
        c = 0x1C;                              /* Ctrl-\ */

    tty_input_char((uint8_t)c);
}

static void kbd_irq(regs_t *r)
{
    (void)r;

    /*
     * The 8042 raises IRQ 1 on output-buffer-full.  That edge can be left
     * pending after the byte was already drained elsewhere -- the init-time
     * drain below, or a coalesced IRQ on real hardware -- so the handler can
     * run with nothing to read.  Reading the data port then returns bus
     * garbage, and if that garbage decodes to a key while a modifier happens
     * to be stuck the line discipline gets a spurious, self-repeating
     * keystroke (the symptom: the cursor scrolls on its own, or a ^C storm).
     * Only touch the port when a byte is genuinely waiting, and drain every
     * byte the controller has queued so a make and its break are never
     * reordered into a stuck key.
     */
    if (!(inb(KBD_STATUS) & 1)) {
        dbg_puts("KBDIRQ: spurious (buffer empty)\n");
        return;
    }

    do {
        /* The 8042's data port is shared with the auxiliary (mouse) port.
         * Status bit 5 marks a byte the aux device produced: those belong
         * to the mouse driver's IRQ 12 drain, not to the keyboard, and
         * stealing one would corrupt the mouse packet stream. */
        if (inb(KBD_STATUS) & 0x20)
            continue;
        uint8_t sc = inb(KBD_DATA);
        kbd_feed(sc);
    } while (inb(KBD_STATUS) & 1);
}
/* ---- read / write ------------------------------------------------------ */
static int32_t vt_write(vt_t *v, const char *buf, uint32_t len)
{
    proc_t *p = proc_current();

    /*
     * A background write only stops the writer when TOSTOP is set, and never
     * when the writer is ignoring or blocking SIGTTOU -- POSIX carves that
     * exemption out so a shell can reclaim the terminal without stopping
     * itself.  Skipping the check would hang the machine on the first prompt.
     */
    if (p && v->fg_pgid && p->pgid != v->fg_pgid && (v->tio.c_lflag & TOSTOP) &&
        !proc_signal_blocked(p, SIGTTOU)) {
        proc_signal(p, SIGTTOU);
        return -E_INTR;
    }

    for (uint32_t i = 0; i < len; i++) {
        char c = buf[i];

        if (v->tio.c_oflag & OPOST) {
            if (c == '\n' && (v->tio.c_oflag & ONLCR)) {
                out_char(v, '\r');
                out_char(v, '\n');
                continue;
            }
            if (c == '\r' && (v->tio.c_oflag & OCRNL))
                c = '\n';
        } else if (c == '\n') {
            /* No post-processing: a bare LF moves down a row and leaves the
             * column alone, the way a real terminal behaves. */
            fbcon_lf_on(v->con);
            dbg_putc('\n');
            continue;
        }

        out_char(v, c);
    }
    return (int32_t)len;
}

static int32_t vt_read(vt_t *v, char *buf, uint32_t len)
{
    if (len == 0)
        return 0;

    proc_t *p = proc_current();

    /*
     * Background jobs are not allowed to read the terminal.  SIGTTIN's
     * default action stops the offending process, so it simply freezes until
     * the shell brings it to the foreground with fg.  If the reader cannot be
     * stopped -- it is ignoring or blocking SIGTTIN -- POSIX says fail with
     * EIO rather than let it steal the user's keystrokes.
     */
    if (p && v->fg_pgid && p->pgid != v->fg_pgid) {
        if (proc_signal_blocked(p, SIGTTIN))
            return -E_IO;
        proc_signal(p, SIGTTIN);
        return -E_INTR;
    }

    int canon = (v->tio.c_lflag & ICANON) != 0;

    /* In non-canonical mode VMIN/VTIME decide when a read is "done": how few
     * bytes will do, and how long to wait for them. */
    uint32_t vmin  = canon ? 1 : v->tio.c_cc[VMIN];
    uint32_t vtime = canon ? 0 : v->tio.c_cc[VTIME];
    uint32_t want  = vmin > len ? len : vmin;

    asm volatile("cli");

    uint64_t deadline   = 0;
    uint32_t last_avail = 0;

    for (;;) {
        if (proc_pending_signals(p)) {
            asm volatile("sti");
            return -E_INTR;
        }

        uint32_t avail = ring_avail(v);
        if (canon) {
            if (avail)
                break;
        } else {
            if (avail && avail >= want)
                break;
            if (want == 0 && vtime == 0)
                break;                      /* a pure poll: 0 bytes is fine */
        }

        if (!p) {
            /* No scheduler yet (very early boot): just idle. */
            asm volatile("sti; hlt; cli");
            continue;
        }

        /* With VTIME the wait is bounded.  MIN == 0 starts the timer with the
         * read itself; MIN > 0 makes it an inter-byte timer, armed only once
         * the first byte has landed and restarted by every byte after it. */
        if (!canon && vtime && (vmin == 0 || avail > 0)) {
            uint64_t now = timer_ticks();
            if (!deadline || avail != last_avail)
                deadline = now + (uint64_t)vtime * TICKS_PER_DECISEC;
            last_avail = avail;
            if (now >= deadline)
                break;
            sched_block_timeout(WAIT_TTY, deadline - now);
            continue;
        }

        sched_block_irqoff(WAIT_TTY);
    }

    uint32_t n = 0;
    int eof = 0;
    while (n < len && !ring_empty(v)) {
        uint16_t c = v->ring[v->tail];
        v->tail = (v->tail + 1) % RING_SIZE;
        if (c == RING_EOF) {
            eof = 1;
            break;
        }
        buf[n++] = (char)c;
        if (canon && c == '\n')
            break;
    }

    asm volatile("sti");

    if (eof && n == 0)
        return 0;
    return (int32_t)n;
}

int32_t tty_write(const char *buf, uint32_t len)
{
    return vt_write(vt_current(), buf, len);
}

int32_t tty_read(char *buf, uint32_t len)
{
    return vt_read(vt_current(), buf, len);
}

void tty_set_pgrp(int pgid) { vt_current()->fg_pgid = pgid; }
int  tty_get_pgrp(void)     { return vt_current()->fg_pgid; }

static int vt_check_ttou(vt_t *v)
{
    proc_t *p = proc_current();

    if (!p || !v->fg_pgid || p->pgid == v->fg_pgid)
        return 0;

    /* The exemption again: a process that is ignoring or blocking SIGTTOU is
     * telling us it knows what it is doing with the terminal.  Every shell
     * does, which is the only reason handing a job the terminal from the
     * (background) child of a fork works at all. */
    if (proc_signal_blocked(p, SIGTTOU))
        return 0;

    proc_signal(p, SIGTTOU);
    return 1;
}

int tty_check_ttou(void)
{
    return vt_check_ttou(vt_current());
}

/* ---- termios ----------------------------------------------------------- */
static void vt_get_termios(vt_t *v, termios_t *t)
{
    *t = v->tio;
}

static void vt_set_termios(vt_t *v, const termios_t *t, int flush)
{
    asm volatile("cli");

    /* Leaving canonical mode with a half-typed line would strand those bytes
     * in a buffer nobody reads any more, so hand them over first. */
    if ((v->tio.c_lflag & ICANON) && !(t->c_lflag & ICANON) && v->line_len)
        line_flush(v, 0);

    v->tio = *t;
    v->tio.c_line = 0;

    if (flush) {
        v->line_len = 0;
        ring_flush(v);
    }

    asm volatile("sti");

    /* A reader parked under the old rules may already be satisfied by the
     * new ones (VMIN dropping to 0, say), so let it look again. */
    sched_wake_reason(WAIT_TTY);
}

void tty_get_termios(termios_t *t)
{
    vt_get_termios(vt_current(), t);
}

void tty_set_termios(const termios_t *t, int flush)
{
    vt_set_termios(vt_current(), t, flush);
}

void tty_get_winsize(winsize_t *ws)
{
    uint32_t cols = 0, rows = 0;
    fbcon_size(&cols, &rows);
    ws->ws_row    = (uint16_t)rows;
    ws->ws_col    = (uint16_t)cols;
    ws->ws_xpixel = 0;
    ws->ws_ypixel = 0;
}
/* ---- VFS glue ---------------------------------------------------------- */
static int32_t tty_node_read(vfs_node_t *n, uint64_t off, void *buf, uint32_t len)
{
    (void)off;
    return vt_read(vt_of_node(n), (char *)buf, len);
}

static int32_t tty_node_write(vfs_node_t *n, uint64_t off, const void *buf,
                              uint32_t len)
{
    (void)off;
    return vt_write(vt_of_node(n), (const char *)buf, len);
}

/* ---- the VT_* ioctls --------------------------------------------------- */
/*
 * Terminal numbers are 1-based on the user-space side (they are the N in
 * /dev/ttyN) and 0-based inside this file.  Every conversion happens here.
 */
static int32_t vt_ioctl(vt_t *v, uint64_t cmd, uint64_t arg)
{
    switch (cmd) {
    case VT_GETSTATE: {
        if (!user_ptr_ok(arg, sizeof(vt_stat_t)))
            return -E_FAULT;
        vt_stat_t *st = (vt_stat_t *)(uintptr_t)arg;
        st->v_active = (uint16_t)(g_active + 1);
        st->v_signal = 0;
        /* Bit N means terminal N exists.  All of them always do here -- they
         * are statically allocated -- and bit 0 is set the way Linux sets it,
         * for a terminal 0 that has never existed. */
        st->v_state = (uint16_t)((1u << (NR_VT + 1)) - 1);
        return 0;
    }

    case VT_OPENQRY: {
        if (!user_ptr_ok(arg, sizeof(int)))
            return -E_FAULT;
        int free_vt = -1;
        for (int i = 0; i < NR_VT; i++)
            if (!g_vt[i].sid) {
                free_vt = i + 1;
                break;
            }
        *(int *)(uintptr_t)arg = free_vt;
        return 0;
    }

    case VT_ACTIVATE:
        if (arg < 1 || arg > NR_VT)
            return -E_INVAL;
        tty_vt_switch((int)arg - 1);
        return 0;

    case VT_WAITACTIVE:
        if (arg < 1 || arg > NR_VT)
            return -E_INVAL;
        /* Already there is the common case and must not block. */
        while (g_active != (int)arg - 1) {
            proc_t *p = proc_current();
            if (!p)
                return -E_INVAL;
            if (proc_pending_signals(p))
                return -E_INTR;
            sched_block(WAIT_TTY);
        }
        return 0;

    case VT_GETMODE: {
        if (!user_ptr_ok(arg, sizeof(vt_mode_t)))
            return -E_FAULT;
        vt_mode_t *m = (vt_mode_t *)(uintptr_t)arg;
        m->mode  = VT_AUTO;
        m->waitv = 0;
        m->relsig = m->acqsig = m->frsig = 0;
        return 0;
    }

    case VT_SETMODE:
        /* The kernel always switches on its own authority; there is no
         * process to negotiate with, so VT_AUTO is the only honest answer
         * and asking for VT_PROCESS is refused rather than quietly ignored. */
        if (!user_ptr_ok(arg, sizeof(vt_mode_t)))
            return -E_FAULT;
        if (((const vt_mode_t *)(uintptr_t)arg)->mode != VT_AUTO)
            return -E_INVAL;
        return 0;

    case VT_DISALLOCATE:
        /* Terminals are static.  Clearing the session is the only part of
         * "deallocate" that means anything here, and it is what lets a fresh
         * getty claim the line. */
        if (arg > NR_VT)
            return -E_INVAL;
        if (arg == 0)
            v->sid = 0;
        else
            g_vt[arg - 1].sid = 0;
        return 0;

    default:
        return -E_NOTTY;
    }
}

/*
 * Terminal ioctls, answered on the VFS char-device path rather than via the
 * syscall-layer tty_ops() fallback.  This is what makes musl's isatty()
 * (a TCGETS probe) and tcgetattr()/tcsetattr() succeed on /dev/tty and
 * /dev/console -- without it a shell like bash refuses to start in interactive
 * mode, because it decides "interactive" from isatty(0) && isatty(1).
 */
static int32_t tty_node_ioctl(vfs_node_t *n, uint64_t cmd, uint64_t arg)
{
    vt_t *v = vt_of_node(n);

    switch (cmd) {
    case TCGETS:
        if (!user_ptr_ok(arg, sizeof(termios_t)))
            return -E_FAULT;
        vt_get_termios(v, (termios_t *)(uintptr_t)arg);
        return 0;

    case TCSETS:
    case TCSETSW:
    case TCSETSF:
        if (!user_ptr_ok(arg, sizeof(termios_t)))
            return -E_FAULT;
        if (vt_check_ttou(v))
            return -E_INTR;
        vt_set_termios(v, (const termios_t *)(uintptr_t)arg, cmd == TCSETSF);
        return 0;

    case TIOCGWINSZ:
        if (!user_ptr_ok(arg, sizeof(winsize_t)))
            return -E_FAULT;
        tty_get_winsize((winsize_t *)(uintptr_t)arg);
        return 0;

    case TIOCSWINSZ:
        /* The console is exactly as large as the framebuffer makes it. */
        return 0;

    case TIOCGPGRP:
        if (!user_ptr_ok(arg, sizeof(int)))
            return -E_FAULT;
        *(int *)(uintptr_t)arg = v->fg_pgid;
        return 0;

    case TIOCSPGRP:
        if (!user_ptr_ok(arg, sizeof(int)))
            return -E_FAULT;
        if (vt_check_ttou(v))
            return -E_INTR;
        v->fg_pgid = *(const int *)(uintptr_t)arg;
        return 0;

    /*
     * TIOCSCTTY is what a getty issues after setsid() to say "this terminal
     * is mine".  With several terminals it finally has work to do: it records
     * the index in the caller's PCB, so every later /dev/tty open by that
     * process (or anything it forks) resolves to this line and not to
     * whichever console the user is looking at.
     */
    case TIOCSCTTY: {
        proc_t *p = proc_current();
        if (!p)
            return -E_PERM;
        p->ctty = vt_index(v);
        if (!v->sid || v->sid == p->sid || arg /* force */) {
            v->sid = p->sid;
            /*
             * Claiming the terminal hands the caller's group the foreground
             * too, exactly as Linux's __proc_set_tty() does -- and the
             * "exactly" matters.  Terminal 1 still carries the process group
             * of /etc/rc, long since exited, when init starts a getty on it;
             * leaving that stale pgid in place would make the new session a
             * *background* one on its own terminal, and its very next
             * tcsetpgrp() would earn it a SIGTTOU and stop it dead.
             */
            v->fg_pgid = p->pgid;
        }
        return 0;
    }

    case TIOCNOTTY: {
        proc_t *p = proc_current();
        if (p) {
            p->ctty = -1;
            if (v->sid == p->sid)
                v->sid = 0;
        }
        return 0;
    }

    case VT_OPENQRY: case VT_GETMODE: case VT_SETMODE: case VT_GETSTATE:
    case VT_ACTIVATE: case VT_WAITACTIVE: case VT_DISALLOCATE:
        return vt_ioctl(v, cmd, arg);

    default:
        return -E_NOTTY;
    }
}

static const vfs_ops_t g_tty_ops = {
    .read  = tty_node_read,
    .write = tty_node_write,
    .ioctl = tty_node_ioctl,
};

const void *tty_ops(void)
{
    return &g_tty_ops;
}

/* ---- setup ------------------------------------------------------------- */
static void termios_defaults(termios_t *t)
{
    memset(t, 0, sizeof(*t));

    t->c_iflag = ICRNL | IXON;
    t->c_oflag = OPOST | ONLCR;
    t->c_cflag = B38400 | CS8 | CREAD | CLOCAL | HUPCL;
    t->c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | ECHOCTL | ECHOKE |
                 IEXTEN;

    t->c_cc[VINTR]    = 0x03;      /* ^C */
    t->c_cc[VQUIT]    = 0x1C;      /* ^\ */
    t->c_cc[VERASE]   = 0x7F;      /* DEL */
    t->c_cc[VKILL]    = 0x15;      /* ^U */
    t->c_cc[VEOF]     = 0x04;      /* ^D */
    t->c_cc[VTIME]    = 0;
    t->c_cc[VMIN]     = 1;
    t->c_cc[VSTART]   = 0x11;      /* ^Q */
    t->c_cc[VSTOP]    = 0x13;      /* ^S */
    t->c_cc[VSUSP]    = 0x1A;      /* ^Z */
    t->c_cc[VEOL]     = 0;
    t->c_cc[VREPRINT] = 0x12;      /* ^R */
    t->c_cc[VDISCARD] = 0x0F;      /* ^O */
    t->c_cc[VWERASE]  = 0x17;      /* ^W */
    t->c_cc[VLNEXT]   = 0x16;      /* ^V */
    t->c_cc[VEOL2]    = 0;
}

void tty_init(void)
{
    memset(g_vt, 0, sizeof(g_vt));
    g_active = 0;
    g_shift = g_caps = g_ctrl = g_alt = 0;
    g_ext = 0;

    for (int i = 0; i < NR_VT; i++) {
        /* Terminal 1 draws on console 0, the one fbcon_init() already made
         * and the kernel has been logging to since boot -- so the boot
         * messages stay on screen and the first shell continues below them. */
        g_vt[i].con = (i == 0) ? 0 : fbcon_alloc();
        termios_defaults(&g_vt[i].tio);
    }

    /* ---- 8042 controller bring-up --------------------------------------
     * QEMU's emulated keyboard already works with no init, but real hardware
     * (and a legacy-emulated USB keyboard) can leave the port disabled or its
     * output buffer full of firmware leftovers.  Briefly wait for the
     * controller to accept a command, enable the keyboard port, then drain
     * its buffer so the first IRQ is a real keystroke.
     */
    while (inb(KBD_STATUS) & 0x02)          /* wait for input buffer empty */
        ;
    outb(KBD_CMD, KBD_ENABLE);              /* enable PS/2 port 1 (keyboard) */
    while (inb(KBD_STATUS) & 1)
        (void)inb(KBD_DATA);

    irq_install(1, kbd_irq);

    /*
     * /dev/tty1 .. /dev/tty6 name one terminal each; /dev/tty is "mine",
     * resolved per caller; /dev/console is where the kernel talks, which is
     * always terminal 1.  An init system opens one or the other with no way
     * to ask which exists, so all of them have to.
     */
    static const char *vt_names[NR_VT] = {
        "tty1", "tty2", "tty3", "tty4", "tty5", "tty6",
    };
    for (int i = 0; i < NR_VT; i++) {
        vfs_register_dev(vt_names[i], &g_tty_ops, &g_vt[i]);
        subsys_set_state(subsys_register(vt_names[i], vt_names[i],
                                         SUBSYS_CLASS_TTY, 4, i + 1),
                         SUBSYS_STATE_LIVE);
    }
    vfs_register_dev("tty", &g_tty_ops, NULL);
    vfs_register_dev("console", &g_tty_ops, &g_vt[0]);
    subsys_set_state(subsys_register("tty", "tty", SUBSYS_CLASS_TTY, 5, 0),
                     SUBSYS_STATE_LIVE);
    subsys_set_state(subsys_register("console", "console", SUBSYS_CLASS_TTY, 5, 1),
                     SUBSYS_STATE_LIVE);

    dbg_puts("GNOS: tty ready (");
    dbg_puts_dec(NR_VT);
    dbg_puts(" virtual terminals, Ctrl-Alt-F1..F");
    dbg_puts_dec(NR_VT);
    dbg_puts(" switches)\r\n");
}
