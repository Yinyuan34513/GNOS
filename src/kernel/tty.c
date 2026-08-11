/*
 * tty.c — console tty driver with a termios line discipline. (GPLv2)
 *
 * Output goes straight to the framebuffer console.  Input comes from the
 * PS/2 controller on IRQ 1: the interrupt handler translates scancode set 1
 * into ASCII and hands each byte to the line discipline, which edits a line
 * buffer, echoes, and pushes finished input into a ring buffer.  Any process
 * asleep waiting for input is then woken.  The interrupt handler never blocks
 * and never copies to user memory.
 *
 * What the discipline actually *does* is no longer hard-coded: it is driven
 * by one struct termios, the same one user space reads and writes through
 * tcgetattr()/tcsetattr() (ioctl TCGETS/TCSETS*).  Turn off ICANON and reads
 * stop waiting for Enter; turn off ECHO and nothing appears on screen; clear
 * ISIG and Ctrl-C becomes an ordinary byte.  That is the whole point of
 * termios, and it is what lets a real shell or editor drive the terminal.
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

static char     g_line[LINE_MAX];
static uint32_t g_line_len;

static uint16_t          g_ring[RING_SIZE];
static volatile uint32_t g_head, g_tail;

static int g_shift, g_caps, g_ctrl, g_alt;

/* Set after a 0xE0 prefix byte: the next scan code is the second half of an
 * extended sequence (cursor keys, edit keys, right-hand modifiers). */
static int g_ext;

/* The foreground process group; 0 means "nobody", i.e. no job control yet. */
static int g_fg_pgid;

/* The one and only line discipline state. */
static termios_t g_tio;

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

/* ---- the input ring --------------------------------------------------- */
static int ring_empty(void) { return g_head == g_tail; }

static uint32_t ring_avail(void)
{
    return (g_head + RING_SIZE - g_tail) % RING_SIZE;
}

uint32_t tty_input_avail(void)
{
    return ring_avail();
}

static void ring_put(uint16_t c)
{
    uint32_t next = (g_head + 1) % RING_SIZE;
    if (next == g_tail)
        return;                     /* full: drop, we have nowhere to block */
    g_ring[g_head] = c;
    g_head = next;
}

static void ring_flush(void)
{
    g_head = g_tail = 0;
}

/* ---- echo ------------------------------------------------------------- */
static void out_char(char c)
{
    fbcon_putc(c);
    dbg_putc(c);
}

static void out_str(const char *s)
{
    for (; *s; s++)
        out_char(*s);
}

/*
 * Echo one input byte the way c_lflag says to.  With ECHOCTL a control
 * character shows up as ^X rather than doing whatever the console would make
 * of it -- that is why a Ctrl-A does not blank half the screen.
 */
static void echo_char(uint8_t c)
{
    if (!(g_tio.c_lflag & ECHO))
        return;

    if ((g_tio.c_lflag & ECHOCTL) && c < 0x20 &&
        c != '\n' && c != '\r' && c != '\t') {
        out_char('^');
        out_char((char)(c + '@'));
        return;
    }
    if ((g_tio.c_lflag & ECHOCTL) && c == 0x7F) {
        out_str("^?");
        return;
    }
    out_char((char)c);
}

/* Rub out the last echoed character (ECHOE).  fbcon's '\b' already draws a
 * blank over the glyph and leaves the cursor on it, so one is enough. */
static void echo_erase(void)
{
    if ((g_tio.c_lflag & (ECHO | ECHOE)) == (ECHO | ECHOE))
        out_char('\b');
}

/* ---- canonical line buffer -------------------------------------------- */
static void line_flush(int with_newline)
{
    for (uint32_t i = 0; i < g_line_len; i++)
        ring_put((uint8_t)g_line[i]);
    if (with_newline)
        ring_put('\n');
    g_line_len = 0;
    sched_wake_reason(WAIT_TTY);
}

/* ---- the line discipline ---------------------------------------------- */
/*
 * The single entry point for "a key was pressed": used by both the keyboard
 * IRQ (after scancode decode) and the ttyinject(404) syscall, which lets a
 * headless test push bytes as though the IRQ had produced them.  Made global
 * on purpose -- see tty.h.
 */
void tty_input_char(uint8_t c)
{
    /* ---- c_iflag: input mapping ---------------------------------------- */
    if (g_tio.c_iflag & ISTRIP)
        c &= 0x7F;

    if (c == '\r') {
        if (g_tio.c_iflag & IGNCR)
            return;
        if (g_tio.c_iflag & ICRNL)
            c = '\n';
    } else if (c == '\n') {
        if (g_tio.c_iflag & INLCR)
            c = '\r';
    }

    /* ---- c_lflag & ISIG: the keys that generate signals ----------------- */
    if (g_tio.c_lflag & ISIG) {
        int sig = 0;
        if (c == g_tio.c_cc[VINTR])      sig = SIGINT;
        else if (c == g_tio.c_cc[VQUIT]) sig = SIGQUIT;
        else if (c == g_tio.c_cc[VSUSP]) sig = SIGTSTP;

        if (sig) {
            echo_char(c);
            if (g_tio.c_lflag & ECHO)
                out_char('\n');
            g_line_len = 0;             /* the half-typed line is gone */

            if (g_fg_pgid)
                proc_signal_group(g_fg_pgid, sig);

            /* A reader blocked in the foreground group has to come back out
             * of the kernel for the signal to take effect. */
            sched_wake_reason(WAIT_TTY);
            return;
        }
    }

    /* ---- c_iflag & IXON: software flow control -------------------------- */
    if (g_tio.c_iflag & IXON) {
        /* Output never backs up on a framebuffer, so ^S/^Q have nothing to
         * stop or start.  They are still swallowed rather than delivered,
         * which is what a program that leaves IXON on expects. */
        if (c == g_tio.c_cc[VSTOP] || c == g_tio.c_cc[VSTART])
            return;
    }

    /* ---- non-canonical: every byte goes straight through ---------------- */
    if (!(g_tio.c_lflag & ICANON)) {
        echo_char(c);
        ring_put(c);
        sched_wake_reason(WAIT_TTY);
        return;
    }

    /* ---- canonical: assemble a line ------------------------------------- */
    if (c == g_tio.c_cc[VEOF]) {
        if (g_line_len)
            line_flush(0);              /* deliver the partial line */
        else {
            ring_put(RING_EOF);
            sched_wake_reason(WAIT_TTY);
        }
        return;
    }

    if (c == g_tio.c_cc[VERASE]) {
        if (g_line_len) {
            g_line_len--;
            echo_erase();
        }
        return;
    }

    if (c == g_tio.c_cc[VKILL]) {
        if (g_tio.c_lflag & ECHOKE) {
            while (g_line_len) {
                g_line_len--;
                echo_erase();
            }
        } else {
            g_line_len = 0;
            if ((g_tio.c_lflag & (ECHO | ECHOK)) == (ECHO | ECHOK))
                out_char('\n');
        }
        return;
    }

    if (c == '\n' || (g_tio.c_cc[VEOL] && c == g_tio.c_cc[VEOL])) {
        if (g_tio.c_lflag & (ECHO | ECHONL))
            out_char('\n');
        line_flush(1);
        return;
    }

    if (g_line_len < LINE_MAX - 1) {
        g_line[g_line_len++] = (char)c;
        echo_char(c);
    }
}

/* Feed a whole buffer through the line discipline, byte by byte.  This is
 * the kernel side of the ttyinject(404) syscall: a test program hands us a
 * string of "keystrokes" and we treat each one exactly as the IRQ would have. */
void tty_inject(const char *buf, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++)
        tty_input_char((uint8_t)buf[i]);
}

/* ---- keyboard interrupt ----------------------------------------------- */
static void kbd_irq(regs_t *r)
{
    (void)r;

    uint8_t sc = inb(KBD_DATA);

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
        if (release)
            return;
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

/* ---- read / write ----------------------------------------------------- */
int32_t tty_write(const char *buf, uint32_t len)
{
    proc_t *p = proc_current();

    /*
     * A background write only stops the writer when TOSTOP is set, and never
     * when the writer is ignoring or blocking SIGTTOU -- POSIX carves that
     * exemption out so a shell can reclaim the terminal without stopping
     * itself.  Skipping the check would hang the machine on the first prompt.
     */
    if (p && g_fg_pgid && p->pgid != g_fg_pgid && (g_tio.c_lflag & TOSTOP) &&
        !proc_signal_blocked(p, SIGTTOU)) {
        proc_signal(p, SIGTTOU);
        return -E_INTR;
    }

    for (uint32_t i = 0; i < len; i++) {
        char c = buf[i];

        if (g_tio.c_oflag & OPOST) {
            if (c == '\n' && (g_tio.c_oflag & ONLCR)) {
                out_char('\r');
                out_char('\n');
                continue;
            }
            if (c == '\r' && (g_tio.c_oflag & OCRNL))
                c = '\n';
        } else if (c == '\n') {
            /* No post-processing: a bare LF moves down a row and leaves the
             * column alone, the way a real terminal behaves. */
            fbcon_lf();
            dbg_putc('\n');
            continue;
        }

        out_char(c);
    }
    return (int32_t)len;
}

int32_t tty_read(char *buf, uint32_t len)
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
    if (p && g_fg_pgid && p->pgid != g_fg_pgid) {
        if (proc_signal_blocked(p, SIGTTIN))
            return -E_IO;
        proc_signal(p, SIGTTIN);
        return -E_INTR;
    }

    int canon = (g_tio.c_lflag & ICANON) != 0;

    /* In non-canonical mode VMIN/VTIME decide when a read is "done": how few
     * bytes will do, and how long to wait for them. */
    uint32_t vmin  = canon ? 1 : g_tio.c_cc[VMIN];
    uint32_t vtime = canon ? 0 : g_tio.c_cc[VTIME];
    uint32_t want  = vmin > len ? len : vmin;

    asm volatile("cli");

    uint64_t deadline   = 0;
    uint32_t last_avail = 0;

    for (;;) {
        if (proc_pending_signals(p)) {
            asm volatile("sti");
            return -E_INTR;
        }

        uint32_t avail = ring_avail();
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
    while (n < len && !ring_empty()) {
        uint16_t c = g_ring[g_tail];
        g_tail = (g_tail + 1) % RING_SIZE;
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

void tty_set_pgrp(int pgid) { g_fg_pgid = pgid; }
int  tty_get_pgrp(void)     { return g_fg_pgid; }

int tty_check_ttou(void)
{
    proc_t *p = proc_current();

    if (!p || !g_fg_pgid || p->pgid == g_fg_pgid)
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

/* ---- termios ---------------------------------------------------------- */
void tty_get_termios(termios_t *t)
{
    *t = g_tio;
}

void tty_set_termios(const termios_t *t, int flush)
{
    asm volatile("cli");

    /* Leaving canonical mode with a half-typed line would strand those bytes
     * in a buffer nobody reads any more, so hand them over first. */
    if ((g_tio.c_lflag & ICANON) && !(t->c_lflag & ICANON) && g_line_len)
        line_flush(0);

    g_tio = *t;
    g_tio.c_line = 0;

    if (flush) {
        g_line_len = 0;
        ring_flush();
    }

    asm volatile("sti");

    /* A reader parked under the old rules may already be satisfied by the
     * new ones (VMIN dropping to 0, say), so let it look again. */
    sched_wake_reason(WAIT_TTY);
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

/* ---- VFS glue --------------------------------------------------------- */
static int32_t tty_node_read(vfs_node_t *n, uint64_t off, void *buf, uint32_t len)
{
    (void)n; (void)off;
    return tty_read((char *)buf, len);
}

static int32_t tty_node_write(vfs_node_t *n, uint64_t off, const void *buf,
                              uint32_t len)
{
    (void)n; (void)off;
    return tty_write((const char *)buf, len);
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
    (void)n;

    switch (cmd) {
    case TCGETS:
        if (!user_ptr_ok(arg, sizeof(termios_t)))
            return -E_FAULT;
        tty_get_termios((termios_t *)(uintptr_t)arg);
        return 0;

    case TCSETS:
    case TCSETSW:
    case TCSETSF:
        if (!user_ptr_ok(arg, sizeof(termios_t)))
            return -E_FAULT;
        if (tty_check_ttou())
            return -E_INTR;
        tty_set_termios((const termios_t *)(uintptr_t)arg, cmd == TCSETSF);
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
        *(int *)(uintptr_t)arg = tty_get_pgrp();
        return 0;

    case TIOCSPGRP:
        if (!user_ptr_ok(arg, sizeof(int)))
            return -E_FAULT;
        if (tty_check_ttou())
            return -E_INTR;
        tty_set_pgrp(*(const int *)(uintptr_t)arg);
        return 0;

    /* There is one terminal and every process shares it, so acquiring or
     * releasing it as a controlling tty is already true by the time you ask. */
    case TIOCSCTTY:
    case TIOCNOTTY:
        return 0;

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
    g_line_len = 0;
    g_head = g_tail = 0;
    g_shift = g_caps = g_ctrl = g_alt = 0;
    g_ext = 0;
    g_fg_pgid = 0;

    termios_defaults(&g_tio);

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
    vfs_register_dev("tty", &g_tty_ops, NULL);
    vfs_register_dev("console", &g_tty_ops, NULL);
    /* Two names, one terminal: /dev/tty is "my controlling terminal" and
     * /dev/console is "where the kernel talks", and with a single console
     * they are the same device.  An init system opens one or the other with
     * no way to ask which exists, so both have to. */
    subsys_set_state(subsys_register("tty", "tty", SUBSYS_CLASS_TTY, 5, 0),
                     SUBSYS_STATE_LIVE);
    subsys_set_state(subsys_register("console", "console", SUBSYS_CLASS_TTY, 5, 1),
                     SUBSYS_STATE_LIVE);

    dbg_puts("GNOS: tty ready (PS/2 keyboard on IRQ1, termios line discipline)\r\n");
}
