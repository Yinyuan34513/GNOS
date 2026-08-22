/*
 * fbcon.c — framebuffer text consoles (VGA 8x16 + Unifont 16x16). (GPLv2)
 *
 * Draws directly into the linear framebuffer Limine handed us.  The console
 * scrolls by memmove-ing the pixel rows up one glyph height, which is slow but
 * keeps the whole thing dependency-free.
 *
 * There are several consoles and one framebuffer.  Each console keeps a cell
 * buffer that mirrors what its screen *would* look like, and only the active
 * one also paints pixels.  That split is what makes virtual terminals work: a
 * shell on tty3 goes on printing into cells while tty1 is on screen, and
 * fbcon_activate() repaints the whole grid from those cells when the user
 * presses Ctrl-Alt-F3.
 *
 * Two things set this apart from a 1980s VGA console:
 *
 *   - Every cell stores a full Unicode code point, not a byte.  UTF-8 arrives
 *     as a stream, fbcon decodes it (see con_emit / the utf8 state below), and
 *     East-Asian-wide characters take two cells with the glyph drawn across
 *     them -- so Chinese, Japanese and the box-drawing characters a TUI is made
 *     of all render instead of turning into mojibake.  Glyphs come from the
 *     bundled 8x16 VGA font for code points below 256 and from the embedded
 *     Unifont bitmap (cjkfont.c) for everything else.
 *
 *   - The console understands the ANSI/VT100 escape sequences -- SGR colour
 *     and attributes, cursor addressing, erase, insert/delete line and
 *     character, scroll regions, saved-cursor and the cursor-visibility toggle
 *   -- so ls --color, a coloured boot banner, full-screen TUI programs and the
 *   installer all behave.  Parsing lives in con_emit's escape state machine;
 *   see ANSI_* below.  It is the subset that real programs actually emit, not
 *   the whole ECMA-48 catalogue.
 */
#include <stdint.h>
#include "bootinfo.h"
#include "fbcon.h"
#include "font8x16.h"
#include "cjkfont.h"
#include "kstring.h"
#include "debugcon.h"
#include "heap.h"

/* ---- the framebuffer, shared by every console --------------------------- */
static struct {
    uint32_t *fb;
    uint32_t  w;          /* pixels */
    uint32_t  h;          /* pixels */
    uint32_t  stride;     /* uint32_t per scanline */
    uint32_t  cols;       /* glyph columns, clamped to FBCON_MAX_COLS */
    uint32_t  rows;       /* glyph rows,    clamped to FBCON_MAX_ROWS */
} g_fb;

/* ---- a single screen cell ----------------------------------------------- *
 * Once per cell, not per write: repaint after a switch and scroll and erase
 * must all reconstruct the screen exactly, including colour and wide glyphs. */
typedef struct {
    uint32_t cp;          /* Unicode code point (0 = blank)            */
    uint8_t  flags;       /* CELL_* below                              */
    uint8_t  fg;          /* colour index 0..15, 0 = console default   */
    uint8_t  bg;          /* colour index 0..15, 0 = console default   */
    uint8_t  _pad;
} cell_t;

#define CELL_BOLD       0x01
#define CELL_REVERSE    0x02
#define CELL_UNDERLINE  0x04
#define CELL_WIDE       0x08   /* first cell of a double-width glyph */
#define CELL_CONT       0x10   /* second cell of a double-width glyph */

typedef struct {
    int      live;
    uint32_t def_fg;      /* the colour fg/bg==0 resolve to */
    uint32_t def_bg;
    uint8_t  cur_fg;      /* current SGR foreground index   */
    uint8_t  cur_bg;      /* current SGR background index   */
    uint8_t  attrs;       /* CELL_BOLD / CELL_REVERSE / CELL_UNDERLINE */
    uint8_t  cursor_on;   /* 0 hides the cursor (DECTCEM off) */
    uint32_t cx;          /* glyph column */
    uint32_t cy;          /* glyph row */
    uint32_t scroll_top;  /* top of the scroll region (inclusive) */
    uint32_t scroll_bot;  /* bottom of the scroll region (inclusive) */
    uint32_t saved_cx, saved_cy;
    uint32_t saved_fg, saved_bg, saved_attrs;
    cell_t   cells[FBCON_MAX_ROWS * FBCON_MAX_COLS];

    /* Alternate screen (?1049h): a full-screen TUI (nano, vi) switches to a
     * private buffer on entry and back to the normal screen on exit, which
     * is what restores the shell prompt instead of leaving the editor's last
     * frame behind.  alt_cells is a heap copy of the normal screen made on
     * entry; everything else records where the cursor/scroll state was. */
    int      alt_screen;
    uint32_t alt_cx, alt_cy;
    uint32_t alt_fg, alt_bg, alt_attrs;
    uint32_t alt_scroll_top, alt_scroll_bot;
    cell_t  *alt_cells;
} console_t;

static console_t g_con[FBCON_MAX_CONS];
static int       g_active;

/* ---- the ANSI colour palette (x8r8g8b8) --------------------------------- */
/* Index 0 of each is the "default" slot and is never used directly; fg/bg of
 * 0 mean "the console's own colour", resolved at draw time. */
static const uint32_t g_palette[16] = {
    0x00000000, 0x00AA0000, 0x0000AA00, 0x00AA5500,   /* black red green yellow */
    0x000000AA, 0x00AA00AA, 0x0000AAAA, 0x00AAAAAA,   /* blue magenta cyan white */
    0x00555555, 0x00FF5555, 0x0055FF55, 0x00FFFF55,   /* bright variants */
    0x005555FF, 0x00FF55FF, 0x0055FFFF, 0x00FFFFFF,
};

static inline cell_t *cell_at(console_t *c, uint32_t col, uint32_t row)
{
    return &c->cells[row * FBCON_MAX_COLS + col];
}

/* Resolve a cell's effective pixel colours, honouring bold and reverse. */
static void eff_colors(console_t *c, const cell_t *cell,
                       uint32_t *fgp, uint32_t *bgp)
{
    uint32_t fg = (cell->fg) ? g_palette[cell->fg & 0xF] : c->def_fg;
    uint32_t bg = (cell->bg) ? g_palette[cell->bg & 0xF] : c->def_bg;
    if (cell->flags & CELL_BOLD)
        fg = (fg & 0x00FF0000) | (((fg & 0x00FF00) << 1) & 0x00FF00) |
             (((fg & 0x0000FF) << 1) & 0x0000FF);   /* brighten */
    if (cell->flags & CELL_REVERSE) {
        uint32_t t = fg; fg = bg; bg = t;
    }
    *fgp = fg; *bgp = bg;
}

/* Paint one glyph at a cell, using the cell's own colours.  Drawing the
 * continuation half of a wide glyph is a no-op: the left cell already laid the
 * whole 16-pixel-wide bitmap down over it. */
static void draw_cell(console_t *c, uint32_t col, uint32_t row)
{
    if (!g_fb.fb)
        return;
    const cell_t *cell = cell_at(c, col, row);
    if (cell->flags & CELL_CONT)   /* right half of a wide glyph: nothing */
        return;
    if (!cell->cp)
        return;

    uint32_t fg, bg;
    eff_colors(c, cell, &fg, &bg);

    const uint8_t *glyph;
    uint32_t w = FONT_WIDTH, h = FONT_HEIGHT;
    int is_cjk = (cell->cp >= 256);

    if (is_cjk) {
        int gw = 0;
        glyph = cjkfont_glyph(cell->cp, &gw);
        if (glyph) { w = (uint32_t)gw; h = CJK_HEIGHT; }
        else        glyph = NULL;
    } else {
        glyph = &font8x16[(uint8_t)cell->cp * FONT_HEIGHT];
    }

    uint32_t px = col * FONT_WIDTH;
    uint32_t py = row * FONT_HEIGHT;

    if (!glyph) {
        /* Not in either font: draw a hollow box so "missing" is visible rather
         * than silent. */
        for (uint32_t y = 0; y < h; y++) {
            uint32_t *line = &g_fb.fb[(py + y) * g_fb.stride + px];
            for (uint32_t x = 0; x < w; x++)
                line[x] = ((y == 0 || y == h - 1 || x == 0 || x == w - 1))
                          ? fg : bg;
        }
        return;
    }

    for (uint32_t y = 0; y < h; y++) {
        uint8_t bits = glyph[y];
        uint32_t *line = &g_fb.fb[(py + y) * g_fb.stride + px];
        for (uint32_t x = 0; x < 8; x++)
            line[x] = (bits & (0x80 >> x)) ? fg : bg;
        if (w > 8) {
            uint8_t bits2 = glyph[y + CJK_HEIGHT];     /* second bitmap row */
            for (uint32_t x = 0; x < 8; x++)
                line[8 + x] = (bits2 & (0x80 >> x)) ? fg : bg;
        }
    }
}

/* Repaint one cell's background plus, when a wide glyph lives there, also the
 * cell to its right -- because clearing the left cell alone would leave the
 * right half's pixels behind. */
static void fill_cell_bg(console_t *c, uint32_t col, uint32_t row)
{
    cell_t *cell = cell_at(c, col, row);
    uint32_t fg, bg;
    eff_colors(c, cell, &fg, &bg);
    uint32_t px = col * FONT_WIDTH;
    uint32_t py = row * FONT_HEIGHT;
    for (uint32_t y = 0; y < FONT_HEIGHT; y++)
        for (uint32_t x = 0; x < FONT_WIDTH; x++)
            g_fb.fb[(py + y) * g_fb.stride + px + x] = bg;
    if (cell->flags & CELL_WIDE) {
        px += FONT_WIDTH;
        for (uint32_t y = 0; y < FONT_HEIGHT; y++)
            for (uint32_t x = 0; x < FONT_WIDTH; x++)
                g_fb.fb[(py + y) * g_fb.stride + px + x] = bg;
    }
}

/* Store a code point in the cell buffer and, when this is the console on
 * screen, paint it.  `cjk` says whether cp spans two cells (the caller has
 * already checked width); when set the next column is marked a continuation. */
static void put_codepoint(console_t *c, uint32_t col, uint32_t row,
                          uint32_t cp, int cjk)
{
    if (col >= g_fb.cols || row >= g_fb.rows)
        return;

    cell_t *cell = cell_at(c, col, row);
    cell->cp    = cp;
    cell->fg    = c->cur_fg;
    cell->bg    = c->cur_bg;
    cell->flags = c->attrs;
    if (cjk) {
        cell->flags |= CELL_WIDE;
        cell_t *right = cell_at(c, col + 1, row);
        right->cp = 0;
        right->flags = CELL_CONT;
        right->fg = c->cur_fg;
        right->bg = c->cur_bg;
    }

    if (c == &g_con[g_active]) {
        static int g_plog = 0;
        if (!g_plog) {
            g_plog = 1;
            dbg_puts("FBCON paint g_active="); dbg_puts_dec(g_active);
            dbg_puts(" cx="); dbg_puts_dec(c->cx);
            dbg_puts(" cy="); dbg_puts_dec(c->cy);
            dbg_puts(" cp="); dbg_puts_hexn(cp, 4);
            dbg_puts("\n");
        }
        fill_cell_bg(c, col, row);
        draw_cell(c, col, row);
        if (cjk && col + 1 < g_fb.cols)
            fill_cell_bg(c, col + 1, row);
    }
}

static void repaint(console_t *c)
{
    if (!g_fb.fb)
        return;
    for (uint32_t row = 0; row < g_fb.rows; row++)
        for (uint32_t col = 0; col < g_fb.cols; col++)
            draw_cell(c, col, row);
}

static void clear_console(console_t *c)
{
    for (uint32_t row = 0; row < g_fb.rows; row++)
        for (uint32_t col = 0; col < g_fb.cols; col++) {
            cell_t *cell = cell_at(c, col, row);
            cell->cp = 0;
            cell->flags = 0;
            cell->fg = 0;
            cell->bg = 0;
        }
    c->cx = 0;
    c->cy = 0;
    c->scroll_top = 0;
    c->scroll_bot = g_fb.rows - 1;

    if (c != &g_con[g_active] || !g_fb.fb) {
        static int g_clog = 0;
        if (!g_clog) {
            g_clog = 1;
            dbg_puts("FBCON clear-skip con="); dbg_puts_dec((uint32_t)(c - g_con));
            dbg_puts(" g_active="); dbg_puts_dec(g_active);
            dbg_puts("\n");
        }
        return;
    }
    { dbg_puts("FBCON clear-PAINT con="); dbg_puts_dec((uint32_t)(c - g_con));
      dbg_puts(" g_active="); dbg_puts_dec(g_active); dbg_puts("\n"); }
    for (uint32_t y = 0; y < g_fb.h; y++)
        for (uint32_t x = 0; x < g_fb.stride; x++)
            g_fb.fb[y * g_fb.stride + x] = c->def_bg;
}

/* Scroll the region [top, bot] up one row, blanking the bottom line. */
static void scroll_region(console_t *c, uint32_t top, uint32_t bot)
{
    for (uint32_t row = top; row < bot; row++)
        memmove(cell_at(c, 0, row), cell_at(c, 0, row + 1),
                g_fb.cols * sizeof(cell_t));
    for (uint32_t col = 0; col < g_fb.cols; col++) {
        cell_t *cell = cell_at(c, col, bot);
        cell->cp = 0; cell->flags = 0; cell->fg = 0; cell->bg = 0;
    }

    if (c != &g_con[g_active] || !g_fb.fb)
        return;

    uint32_t top_px = top * FONT_HEIGHT;
    uint32_t bot_px = (bot + 1) * FONT_HEIGHT;
    uint32_t keep  = (bot_px - top_px - FONT_HEIGHT) * g_fb.stride;

    memmove(g_fb.fb + top_px * g_fb.stride,
            g_fb.fb + (top_px + FONT_HEIGHT) * g_fb.stride,
            (uint64_t)keep * 4);
    for (uint32_t y = bot_px - FONT_HEIGHT; y < bot_px; y++)
        for (uint32_t x = 0; x < g_fb.stride; x++)
            g_fb.fb[y * g_fb.stride + x] = c->def_bg;
}

static void newline(console_t *c)
{
    c->cx = 0;
    if (c->cy >= c->scroll_bot)
        scroll_region(c, c->scroll_top, c->scroll_bot);
    else if (++c->cy >= g_fb.rows)
        c->cy = g_fb.rows - 1;
}

/* ====================================================================== *
 *  ANSI / VT100 escape sequence parsing
 *
 *  The state machine is fed one byte at a time by con_emit.  Control
 *  characters that are not part of a sequence (or that legitimately interrupt
 *  one, like a stray CR) are handled up front; everything else accumulates in
 *  the small scratch below until the terminating character closes a CSI.
 * ====================================================================== */
enum { ST_GROUND, ST_ESC, ST_CSI, ST_OSC };

typedef struct {
    int    state;
    int    params[16];     /* up to 16 numeric arguments */
    int    nparams;
    int    pcur;           /* current param being read */
    int    private;        /* private marker byte: '?', '>', '$', '\0' */
    int    osc_len;
    char   osc[256];
} esc_t;

static esc_t g_esc;

static inline void esc_reset(esc_t *e)
{
    e->state = ST_GROUND;
    e->nparams = 0;
    e->pcur = 0;
    e->private = 0;
    e->osc_len = 0;
    e->params[0] = 0;
}

static inline void esc_param_push(esc_t *e, int digit)
{
    if (e->nparams == 0)
        e->nparams = 1;
    int *p = &e->params[e->nparams - 1];
    if (*p < 100000)
        *p = *p * 10 + digit;
}

static inline int get_p(esc_t *e, int idx, int def)
{
    return (idx < e->nparams) ? e->params[idx] : def;
}

/* Apply an SGR (Select Graphic Rendition) parameter.  A leading 8-bit colour
 * mode in the 38/48 family consumes following parameters via *consumed. */
static void apply_sgr(console_t *c, esc_t *e)
{
    for (int i = 0; i < e->nparams; i++) {
        int p = e->params[i];
        switch (p) {
        case 0:  c->attrs = 0; c->cur_fg = 0; c->cur_bg = 0; break;
        case 1:  c->attrs |= CELL_BOLD; break;
        case 4:  c->attrs |= CELL_UNDERLINE; break;
        case 7:  c->attrs |= CELL_REVERSE; break;
        case 22: c->attrs &= ~CELL_BOLD; break;
        case 24: c->attrs &= ~CELL_UNDERLINE; break;
        case 27: c->attrs &= ~CELL_REVERSE; break;
        case 30 ... 37: c->cur_fg = (uint8_t)(p - 30); break;
        case 39: c->cur_fg = 0; break;
        case 40 ... 47: c->cur_bg = (uint8_t)(p - 40); break;
        case 49: c->cur_bg = 0; break;
        case 90 ... 97: c->cur_fg = (uint8_t)(p - 90 + 8); break;
        case 100 ... 107: c->cur_bg = (uint8_t)(p - 100 + 8); break;
        case 38: case 48: {
            /* 38;5;n / 38;2;r;g;b ;  foreground when p==38 */
            int mode = get_p(e, i + 1, 0);
            if (mode == 5) {
                int n = get_p(e, i + 2, 0);
                if (p == 38) c->cur_fg = (uint8_t)n; else c->cur_bg = (uint8_t)n;
                i += 2;
            } else if (mode == 2) {
                /* Map the 24-bit RGB to the nearest of the 16 palette colours;
                 * a real 16-bit framebuffer has no truecolour terminals here. */
                int r = get_p(e, i + 2, 0);
                int g = get_p(e, i + 3, 0);
                int b = get_p(e, i + 4, 0);
                int best = 0, bestd = 0x7FFFFFFF;
                for (int k = 1; k < 16; k++) {
                    int pr = (g_palette[k] >> 16) & 0xFF;
                    int pg = (g_palette[k] >> 8) & 0xFF;
                    int pb = g_palette[k] & 0xFF;
                    int d = (r - pr) * (r - pr) + (g - pg) * (g - pg) +
                            (b - pb) * (b - pb);
                    if (d < bestd) { bestd = d; best = k; }
                }
                if (p == 38) c->cur_fg = (uint8_t)best;
                else         c->cur_bg = (uint8_t)best;
                i += 4;
            }
            break;
        }
        default:
            break;      /* unknown SGR attribute: ignore, as a terminal would */
        }
    }
}

/* The cursor's own cell has to be redrawn when the cursor moves, so that the
 * old position stops being whatever was painted there. */
static void refresh_cursor_cell(console_t *c, uint32_t col, uint32_t row)
{
    if (c != &g_con[g_active] || !g_fb.fb)
        return;
    if (col >= g_fb.cols || row >= g_fb.rows)
        return;
    draw_cell(c, col, row);
}

/* ---- alternate screen (?1049h / ?1047h / ?47h) ---------------------------
 * Entering swaps the live cell grid into a heap copy and hands the app a
 * clean screen; exiting copies it back, restoring the prompt exactly. */
static void alt_screen_enter(console_t *c)
{
    if (c->alt_screen)
        return;
    if (!c->alt_cells) {
        c->alt_cells = kmalloc(sizeof(c->cells));
        if (!c->alt_cells)
            return;
    }
    memcpy(c->alt_cells, c->cells, sizeof(c->cells));
    c->alt_cx = c->cx; c->alt_cy = c->cy;
    c->alt_fg = c->cur_fg; c->alt_bg = c->cur_bg; c->alt_attrs = c->attrs;
    c->alt_scroll_top = c->scroll_top; c->alt_scroll_bot = c->scroll_bot;
    c->alt_screen = 1;
    clear_console(c);               /* also paints the blank screen */
}

static void alt_screen_exit(console_t *c)
{
    if (!c->alt_screen)
        return;
    memcpy(c->cells, c->alt_cells, sizeof(c->cells));
    c->alt_screen = 0;
    c->cx = c->alt_cx; c->cy = c->alt_cy;
    c->cur_fg = (uint8_t)c->alt_fg; c->cur_bg = (uint8_t)c->alt_bg;
    c->attrs = (uint8_t)c->alt_attrs;
    c->scroll_top = c->alt_scroll_top; c->scroll_bot = c->alt_scroll_bot;
    if (c == &g_con[g_active] && g_fb.fb)
        repaint(c);
}

static void handle_csi(console_t *c, esc_t *e, char fin)
{
    int n, m;
    switch (fin) {
    case 'm':  apply_sgr(c, e); break;

    case 'A':  /* CUU: cursor up */
        n = get_p(e, 0, 1);
        c->cy = (n > (int)c->cy) ? 0 : c->cy - (uint32_t)n;
        refresh_cursor_cell(c, c->cx, c->cy);
        break;
    case 'B':  /* CUD: cursor down */
        n = get_p(e, 0, 1);
        c->cy = (c->cy + (uint32_t)n >= g_fb.rows) ? g_fb.rows - 1 : c->cy + (uint32_t)n;
        refresh_cursor_cell(c, c->cx, c->cy);
        break;
    case 'C':  /* CUF: cursor right */
        n = get_p(e, 0, 1);
        c->cx = (c->cx + (uint32_t)n >= g_fb.cols) ? g_fb.cols - 1 : c->cx + (uint32_t)n;
        refresh_cursor_cell(c, c->cx, c->cy);
        break;
    case 'D':  /* CUB: cursor left */
        n = get_p(e, 0, 1);
        c->cx = (n > (int)c->cx) ? 0 : c->cx - (uint32_t)n;
        refresh_cursor_cell(c, c->cx, c->cy);
        break;
    case 'E':  /* CNL: down n, column 0 */
        n = get_p(e, 0, 1);
        c->cy = (c->cy + (uint32_t)n >= g_fb.rows) ? g_fb.rows - 1 : c->cy + (uint32_t)n;
        c->cx = 0;
        break;
    case 'F':  /* CPL: up n, column 0 */
        n = get_p(e, 0, 1);
        c->cy = (n > (int)c->cy) ? 0 : c->cy - (uint32_t)n;
        c->cx = 0;
        break;
    case 'G':  /* CHA: cursor to column n (1-based) */
        n = get_p(e, 0, 1);
        c->cx = (n <= 0) ? 0 : (n > (int)g_fb.cols ? g_fb.cols - 1 : (uint32_t)(n - 1));
        refresh_cursor_cell(c, c->cx, c->cy);
        break;
    case 'H': case 'f':  /* CUP / HVP: row;col (1-based) */
        n = get_p(e, 0, 1);
        m = get_p(e, 1, 1);
        c->cy = (n <= 0) ? 0 : (n > (int)g_fb.rows ? g_fb.rows - 1 : (uint32_t)(n - 1));
        c->cx = (m <= 0) ? 0 : (m > (int)g_fb.cols ? g_fb.cols - 1 : (uint32_t)(m - 1));
        refresh_cursor_cell(c, c->cx, c->cy);
        break;
    case 'd':  /* VPA: vertical position absolute, row n */
        n = get_p(e, 0, 1);
        c->cy = (n <= 0) ? 0 : (n > (int)g_fb.rows ? g_fb.rows - 1 : (uint32_t)(n - 1));
        break;

    case 'J':  /* ED: erase in display */
        n = get_p(e, 0, 0);
        {
            uint32_t r, col;
            if (n == 0) {                  /* cursor to end */
                for (col = c->cx; col < g_fb.cols; col++) {
                    cell_t *cell = cell_at(c, col, c->cy);
                    cell->cp = 0; cell->flags = 0;
                }
                for (r = c->cy + 1; r < g_fb.rows; r++)
                    for (col = 0; col < g_fb.cols; col++) {
                        cell_t *cell = cell_at(c, col, r);
                        cell->cp = 0; cell->flags = 0;
                    }
            } else if (n == 1) {           /* start to cursor */
                for (r = 0; r < c->cy; r++)
                    for (col = 0; col < g_fb.cols; col++) {
                        cell_t *cell = cell_at(c, col, r);
                        cell->cp = 0; cell->flags = 0;
                    }
                for (col = 0; col <= c->cx; col++) {
                    cell_t *cell = cell_at(c, col, c->cy);
                    cell->cp = 0; cell->flags = 0;
                }
            } else if (n == 2 || n == 3) {  /* whole screen / scrollback */
                for (r = 0; r < g_fb.rows; r++)
                    for (col = 0; col < g_fb.cols; col++) {
                        cell_t *cell = cell_at(c, col, r);
                        cell->cp = 0; cell->flags = 0;
                    }
                c->cx = 0; c->cy = 0;
            }
            if (c == &g_con[g_active] && g_fb.fb)
                repaint(c);
        }
        break;

    case 'K':  /* EL: erase in line */
        n = get_p(e, 0, 0);
        {
            uint32_t col;
            if (n == 0)
                for (col = c->cx; col < g_fb.cols; col++) {
                    cell_t *cell = cell_at(c, col, c->cy);
                    cell->cp = 0; cell->flags = 0;
                }
            else if (n == 1)
                for (col = 0; col <= c->cx; col++) {
                    cell_t *cell = cell_at(c, col, c->cy);
                    cell->cp = 0; cell->flags = 0;
                }
            else if (n == 2)
                for (col = 0; col < g_fb.cols; col++) {
                    cell_t *cell = cell_at(c, col, c->cy);
                    cell->cp = 0; cell->flags = 0;
                }
            if (c == &g_con[g_active] && g_fb.fb)
                repaint(c);
        }
        break;

    case 'L':  /* IL: insert n lines at cursor */
        n = get_p(e, 0, 1);
        for (uint32_t k = 0; k < (uint32_t)n; k++) {
            uint32_t bot = c->scroll_bot;
            for (uint32_t r = bot; r > c->cy; r--)
                memmove(cell_at(c, 0, r), cell_at(c, 0, r - 1),
                        g_fb.cols * sizeof(cell_t));
            for (uint32_t col = 0; col < g_fb.cols; col++) {
                cell_t *cell = cell_at(c, col, c->cy);
                cell->cp = 0; cell->flags = 0; cell->fg = 0; cell->bg = 0;
            }
        }
        if (c == &g_con[g_active] && g_fb.fb)
            repaint(c);
        break;

    case 'M':  /* DL: delete n lines at cursor */
        n = get_p(e, 0, 1);
        for (uint32_t k = 0; k < (uint32_t)n; k++) {
            uint32_t bot = c->scroll_bot;
            for (uint32_t r = c->cy; r < bot; r++)
                memmove(cell_at(c, 0, r), cell_at(c, 0, r + 1),
                        g_fb.cols * sizeof(cell_t));
            for (uint32_t col = 0; col < g_fb.cols; col++) {
                cell_t *cell = cell_at(c, bot, col);
                cell->cp = 0; cell->flags = 0; cell->fg = 0; cell->bg = 0;
            }
        }
        if (c == &g_con[g_active] && g_fb.fb)
            repaint(c);
        break;

    case '@':  /* ICH: insert n chars at cursor (shift right) */
        n = get_p(e, 0, 1);
        {
            uint32_t col = c->cx;
            while (n-- > 0 && col < g_fb.cols) {
                uint32_t last = g_fb.cols - 1;
                for (uint32_t x = last; x > col; x--)
                    *cell_at(c, x, c->cy) = *cell_at(c, x - 1, c->cy);
                cell_t *cell = cell_at(c, col, c->cy);
                cell->cp = 0; cell->flags = 0;
                col++;
            }
            if (c == &g_con[g_active] && g_fb.fb)
                repaint(c);
        }
        break;

    case 'P':  /* DCH: delete n chars at cursor (shift left) */
        n = get_p(e, 0, 1);
        {
            for (uint32_t k = 0; k < (uint32_t)n; k++) {
                uint32_t x;
                for (x = c->cx; x + 1 < g_fb.cols; x++)
                    *cell_at(c, x, c->cy) = *cell_at(c, x + 1, c->cy);
                cell_t *cell = cell_at(c, x, c->cy);
                cell->cp = 0; cell->flags = 0;
            }
            if (c == &g_con[g_active] && g_fb.fb)
                repaint(c);
        }
        break;

    case 'X':  /* ECH: erase n chars at cursor */
        n = get_p(e, 0, 1);
        for (uint32_t x = c->cx; x < g_fb.cols && n-- > 0; x++) {
            cell_t *cell = cell_at(c, x, c->cy);
            cell->cp = 0; cell->flags = 0;
        }
        if (c == &g_con[g_active] && g_fb.fb)
            repaint(c);
        break;

    case 'S':  /* SU: scroll up n lines */
        n = get_p(e, 0, 1);
        for (int k = 0; k < n; k++)
            scroll_region(c, c->scroll_top, c->scroll_bot);
        if (c == &g_con[g_active] && g_fb.fb)
            repaint(c);
        break;

    case 'T':  /* SD: scroll down n lines */
        n = get_p(e, 0, 1);
        for (int k = 0; k < n; k++) {
            for (uint32_t r = c->scroll_bot; r > c->scroll_top; r--)
                memmove(cell_at(c, 0, r), cell_at(c, 0, r - 1),
                        g_fb.cols * sizeof(cell_t));
            for (uint32_t col = 0; col < g_fb.cols; col++) {
                cell_t *cell = cell_at(c, col, c->scroll_top);
                cell->cp = 0; cell->flags = 0; cell->fg = 0; cell->bg = 0;
            }
        }
        if (c == &g_con[g_active] && g_fb.fb)
            repaint(c);
        break;

    case 'r':  /* DECSTBM: set scroll region [top;bottom] (1-based) */
        n = get_p(e, 0, 1);
        m = get_p(e, 1, g_fb.rows);
        if (n >= 1 && m >= 1 && (uint32_t)n <= g_fb.rows &&
            (uint32_t)m <= g_fb.rows && n < m) {
            c->scroll_top = (uint32_t)n - 1;
            c->scroll_bot = (uint32_t)m - 1;
            c->cx = 0;
            c->cy = c->scroll_top;
        }
        break;

    case 'h': case 'l':  /* DEC private modes via '?' */
        if (e->private == '?') {
            for (int i = 0; i < e->nparams; i++) {
                int p = e->params[i];
                if (p == 25)               /* DECTCEM: visible cursor */
                    c->cursor_on = (fin == 'h');
                else if (p == 1049 || p == 1047 || p == 47) {
                    if (fin == 'h')        /* alternate screen on */
                        alt_screen_enter(c);
                    else                   /* ... and back */
                        alt_screen_exit(c);
                }
            }
        }
        break;

    case 's':  /* save cursor */
        c->saved_cx = c->cx; c->saved_cy = c->cy;
        c->saved_fg = c->cur_fg; c->saved_bg = c->cur_bg;
        c->saved_attrs = c->attrs;
        break;
    case 'u':  /* restore cursor */
        c->cx = c->saved_cx; c->cy = c->saved_cy;
        c->cur_fg = (uint8_t)c->saved_fg; c->cur_bg = (uint8_t)c->saved_bg;
        c->attrs = (uint8_t)c->saved_attrs;
        refresh_cursor_cell(c, c->cx, c->cy);
        break;

    default:
        break;      /* unrecognised CSI final: ignore */
    }
}

/* Feed one byte through the escape parser.  Returns 1 when the byte was
 * consumed as part of a sequence (the caller must not treat it as a glyph),
 * 0 when the caller still needs to interpret it as a printable/control byte. */
static int esc_feed(console_t *c, uint8_t b)
{
    esc_t *e = &g_esc;

    switch (e->state) {
    case ST_GROUND:
        if (b == 0x1B) {                  /* ESC */
            e->state = ST_ESC;
            return 1;
        }
        return 0;

    case ST_ESC:
        if (b == '[') {
            e->state = ST_CSI;
            e->nparams = 0; e->pcur = 0; e->private = 0;
            e->params[0] = 0;
            return 1;
        }
        if (b == ']') {                   /* OSC: title/colour, ignore body */
            e->state = ST_OSC;
            e->osc_len = 0;
            return 1;
        }
        if (b == 'c') {                   /* RIS: full reset */
            e->state = ST_GROUND;
            c->attrs = 0; c->cur_fg = 0; c->cur_bg = 0;
            c->cursor_on = 1;
            c->scroll_top = 0; c->scroll_bot = g_fb.rows - 1;
            if (c->alt_screen)
                alt_screen_exit(c);
            else
                clear_console(c);
            return 1;
        }
        if (b == '7') {                   /* DECSC: save cursor + attrs */
            e->state = ST_GROUND;
            c->saved_cx = c->cx; c->saved_cy = c->cy;
            c->saved_fg = c->cur_fg; c->saved_bg = c->cur_bg;
            c->saved_attrs = c->attrs;
            return 1;
        }
        if (b == '8') {                   /* DECRC: restore cursor + attrs */
            e->state = ST_GROUND;
            c->cx = c->saved_cx; c->cy = c->saved_cy;
            c->cur_fg = (uint8_t)c->saved_fg; c->cur_bg = (uint8_t)c->saved_bg;
            c->attrs = (uint8_t)c->saved_attrs;
            refresh_cursor_cell(c, c->cx, c->cy);
            return 1;
        }
        if (b == '=' || b == '>') {       /* DECKPAM/DECKPNM: keypad mode */
            e->state = ST_GROUND;         /* no keypad here: ignore */
            return 1;
        }
        if (b == '(' || b == ')' || b == '*' || b == '+') {
            e->state = ST_GROUND;         /* charset designation: skip 1 byte */
            return 1;
        }
        /* Unknown two-byte escape: drop it and emit the byte as a glyph
         * (ESC '[' etc. are the only sequences we really care about). */
        e->state = ST_GROUND;
        return 0;

    case ST_CSI:
        if (b == '?') {
            e->private = '?';
            return 1;
        }
        if (b >= '0' && b <= '9') {
            esc_param_push(e, b - '0');
            return 1;
        }
        if (b == ';') {
            if (e->nparams < 16)
                e->nparams++;
            if (e->nparams < 16)
                e->params[e->nparams - 1] = 0;
            return 1;
        }
        if (b == ':')                     /* sub-parameter separator: ignore */
            return 1;
        if ((b >= 0x20 && b <= 0x2F))    /* intermediate: ignore */
            return 1;
        if (b >= 0x40 && b <= 0x7E) {     /* final byte: dispatch */
            handle_csi(c, e, (char)b);
            e->state = ST_GROUND;
            return 1;
        }
        /* anything else: abandon */
        e->state = ST_GROUND;
        return 0;

    case ST_OSC:
        if (b == 0x07) {                  /* BEL terminates */
            e->state = ST_GROUND;
        } else if (b == 0x1B) {           /* or ESC \ (ST) */
            e->state = ST_ESC;
        } else if (e->osc_len < (int)sizeof e->osc - 1) {
            e->osc[e->osc_len++] = (char)b;
        }
        return 1;
    }
    e->state = ST_GROUND;
    return 0;
}

/* ====================================================================== *
 *  UTF-8 decoding (tiny state machine, fed one byte at a time)
 * ====================================================================== */
static struct {
    uint32_t cp;
    int      need;
    int      got;
    int      lower;       /* lowest valid continuation value, for overlong rej */
} g_u8;

/* Decode one byte; returns 0 while a multi-byte sequence is incomplete, or a
 * code point (possibly U+FFFD for an illegal input) when one is ready. */
static uint32_t utf8_feed(uint8_t b)
{
    if (g_u8.need == 0) {
        if (b < 0x80) {
            return b;
        } else if ((b & 0xE0) == 0xC0) {       /* 110xxxxx: 2 bytes */
            if ((b & 0x1E) == 0) return 0xFFFD;   /* 0xC0/0xC1 lead: overlong */
            g_u8.cp = b & 0x1F; g_u8.need = 1; g_u8.got = 0; g_u8.lower = 0x80;
        } else if ((b & 0xF0) == 0xE0) {       /* 1110xxxx: 3 bytes */
            g_u8.cp = b & 0x0F; g_u8.need = 2; g_u8.got = 0; g_u8.lower = 0x80;
        } else if ((b & 0xF8) == 0xF0) {       /* 11110xxx: 4 bytes */
            if ((b & 0x07) == 0) return 0xFFFD;   /* 0xF0+0x0..3: overlong */
            g_u8.cp = b & 0x07; g_u8.need = 3; g_u8.got = 0; g_u8.lower = 0x80;
        } else {
            return 0xFFFD;                       /* 0x80..0xBF stray, or 0xF8+ */
        }
        return 0;
    }

    /* continuation byte expected */
    if ((b & 0xC0) != 0x80)
        goto bad;
    if (b < g_u8.lower)
        goto bad;
    g_u8.lower = 0x80;
    g_u8.cp = (g_u8.cp << 6) | (b & 0x3F);
    if (++g_u8.got < g_u8.need)
        return 0;

    /* complete */
    uint32_t cp = g_u8.cp;
    g_u8.need = 0;
    if (cp > 0x10FFFF)             return 0xFFFD;   /* beyond Unicode */
    if (cp >= 0xD800 && cp <= 0xDFFF) return 0xFFFD; /* UTF-16 surrogates */
    return cp;

bad:
    g_u8.need = 0;
    return 0xFFFD;
}

/* ====================================================================== *
 *  The central entry point: one byte from a tty (or the kernel itself)
 * ====================================================================== */
static void con_emit(console_t *c, uint8_t b)
{
    /* Control characters still mean something mid-sequence: a CR or LF must
     * act now, not wait for the sequence to end.  Anything else is handed to
     * the escape parser first. */
    if (esc_feed(c, b))
        return;

    switch (b) {
    case '\n':
        newline(c);
        return;
    case '\r':
        c->cx = 0;
        return;
    case '\b':
        if (c->cx > 0)
            c->cx--;
        else if (c->cy > 0) {
            c->cy--;
            c->cx = g_fb.cols - 1;
        }
        return;
    case '\t':
        do {
            con_emit(c, ' ');
        } while (c->cx & 7);
        return;
    }

    uint32_t cp = utf8_feed(b);
    if (cp == 0)                  /* mid-sequence: nothing to draw yet */
        return;
    if (cp == 0xFFFD && b == 0x1B)   /* never reached; guard for clarity */
        return;

    int w = cjkfont_width(cp);
    if (w == 0)                  /* control / combining: take no cell */
        return;

    int cjk = (w == 2);

    /* A wide glyph that would run off the right edge wraps to the next line,
     * the way a real terminal does. */
    if (cjk && c->cx + 1 >= g_fb.cols) {
        newline(c);
    }
    if (c->cx >= g_fb.cols)
        newline(c);

    put_codepoint(c, c->cx, c->cy, cp, cjk);

    if (cjk)
        c->cx += 2;
    else
        c->cx += 1;

    if (c->cx >= g_fb.cols)
        newline(c);
}

/* ---- setup ------------------------------------------------------------- */
void fbcon_init(const bootinfo_t *bi)
{
    cjkfont_init();

    g_fb.fb     = (uint32_t *)(uintptr_t)bi->fb_addr;
    g_fb.w      = bi->fb_width;
    g_fb.h      = bi->fb_height;
    g_fb.stride = bi->fb_pitch / 4;

    g_fb.cols = g_fb.w / FONT_WIDTH;
    g_fb.rows = g_fb.h / FONT_HEIGHT;
    if (g_fb.cols > FBCON_MAX_COLS)
        g_fb.cols = FBCON_MAX_COLS;
    if (g_fb.rows > FBCON_MAX_ROWS)
        g_fb.rows = FBCON_MAX_ROWS;

    memset(g_con, 0, sizeof(g_con));
    g_active = 0;
    esc_reset(&g_esc);

    g_con[0].live      = 1;
    g_con[0].def_fg    = 0x00CCCCCC;
    g_con[0].def_bg    = 0x00000000;
    g_con[0].cursor_on = 1;
    g_con[0].scroll_bot = g_fb.rows - 1;
    clear_console(&g_con[0]);

    dbg_puts("FBCON init g_active="); dbg_puts_dec(g_active);
    dbg_puts(" fb="); dbg_puts_hexn((uint64_t)(uintptr_t)g_fb.fb, 8);
    dbg_puts(" cols="); dbg_puts_dec(g_fb.cols);
    dbg_puts(" rows="); dbg_puts_dec(g_fb.rows);
    dbg_puts("\n");
}

int fbcon_alloc(void)
{
    for (int i = 1; i < FBCON_MAX_CONS; i++) {
        if (g_con[i].live)
            continue;
        g_con[i].live      = 1;
        g_con[i].def_fg    = 0x00CCCCCC;
        g_con[i].def_bg    = 0x00000000;
        g_con[i].cursor_on = 1;
        g_con[i].scroll_bot = g_fb.rows - 1;
        clear_console(&g_con[i]);       /* not active: cells only */
        return i;
    }
    return -1;
}

void fbcon_activate(int con)
{
    if (con < 0 || con >= FBCON_MAX_CONS || !g_con[con].live || con == g_active)
        return;
    dbg_puts("FBCON activate con="); dbg_puts_dec(con);
    dbg_puts(" from="); dbg_puts_dec(g_active); dbg_puts("\n");
    g_active = con;
    repaint(&g_con[con]);
}

int fbcon_active(void)
{
    return g_active;
}

/* ---- per-console output ------------------------------------------------ */
static console_t *pick(int con)
{
    if (con < 0 || con >= FBCON_MAX_CONS || !g_con[con].live)
        return NULL;
    return &g_con[con];
}

void fbcon_putc_on(int con, char c)
{
    console_t *t = pick(con);
    if (t)
        con_emit(t, (uint8_t)c);
}

void fbcon_lf_on(int con)
{
    console_t *t = pick(con);
    if (!t)
        return;
    if (++t->cy >= g_fb.rows)
        scroll_region(t, t->scroll_top, t->scroll_bot);
}

void fbcon_clear_on(int con)
{
    dbg_puts("FBCON fbcon_clear_on con="); dbg_puts_dec(con); dbg_puts("\n");
    console_t *t = pick(con);
    if (t)
        clear_console(t);
}

void fbcon_set_color_on(int con, uint32_t fg, uint32_t bg)
{
    console_t *t = pick(con);
    if (!t)
        return;
    /* This old API set raw pixels; keep it meaningful by mapping it to the
     * default colours and clearing, so callers still get "reset". */
    t->def_fg = fg;
    t->def_bg = bg;
    t->cur_fg = 0;
    t->cur_bg = 0;
    t->attrs  = 0;
}

/* ---- console 0 --------------------------------------------------------- */
void fbcon_putc(char c)                       { fbcon_putc_on(0, c); }
void fbcon_lf(void)                           { fbcon_lf_on(0); }
void fbcon_clear(void)                        { fbcon_clear_on(0); }
void fbcon_set_color(uint32_t fg, uint32_t bg){ fbcon_set_color_on(0, fg, bg); }

void fbcon_puts(const char *s)
{
    for (; *s; s++)
        fbcon_putc_on(0, (uint8_t)*s);
}

void fbcon_size(uint32_t *cols, uint32_t *rows)
{
    if (cols)
        *cols = g_fb.cols;
    if (rows)
        *rows = g_fb.rows;
}

/*
 * The framebuffer's pixel geometry, for the drivers that draw into the same
 * memory the console owns (the DRM driver's SETCRTC blit) and for fbdev's
 * ioctls, which must report the *current* mode rather than the one the
 * bootloader set up before the first modeset.
 */
void fbcon_geometry(uint32_t *w, uint32_t *h, uint32_t *pitch)
{
    if (w)     *w = g_fb.w;
    if (h)     *h = g_fb.h;
    if (pitch) *pitch = g_fb.stride * 4;
}

void *fbcon_fb(void)
{
    return g_fb.fb;
}

uint32_t fbcon_pitch(void)
{
    return g_fb.stride * 4;
}

/*
 * A modeset changed the scanout resolution.  Re-derive the text grid, clamp
 * every console's cursor and scroll region into it, and repaint the active
 * console so the screen does not come back as garbage from the old mode's
 * pitch.  The framebuffer pointer itself never moves -- mode switches on the
 * bochs VBE keep the same linear BAR.
 */
void fbcon_resize(uint32_t w, uint32_t h, uint32_t pitch)
{
    if (w < 320 || h < 200 || pitch < w * 4)
        return;

    g_fb.w = w;
    g_fb.h = h;
    g_fb.stride = pitch / 4;
    g_fb.cols = (w / FONT_WIDTH)  > FBCON_MAX_COLS ? FBCON_MAX_COLS
                                                    : w / FONT_WIDTH;
    g_fb.rows = (h / FONT_HEIGHT) > FBCON_MAX_ROWS ? FBCON_MAX_ROWS
                                                    : h / FONT_HEIGHT;

    for (int i = 0; i < FBCON_MAX_CONS; i++) {
        console_t *c = &g_con[i];
        if (!c->live)
            continue;
        c->scroll_top = 0;
        c->scroll_bot = g_fb.rows - 1;
        /* The old cells were laid out for the old width; repainting a
         * half-stale buffer would smear half-rows across the screen. */
        for (uint32_t cell = 0; cell < FBCON_MAX_ROWS * FBCON_MAX_COLS; cell++) {
            c->cells[cell].cp = 0;
            c->cells[cell].flags = 0;
            c->cells[cell].fg = 0;
            c->cells[cell].bg = 0;
        }
        c->cx = 0;
        c->cy = 0;
    }

    clear_console(&g_con[g_active]);
    repaint(&g_con[g_active]);
    dbg_puts("FBCON: resized to ");
    dbg_puts_dec(g_fb.cols);
    dbg_puts("x");
    dbg_puts_dec(g_fb.rows);
    dbg_puts(" glyphs\r\n");
}

void fbcon_panic(void)
{
    /* Whatever terminal the user was on, the panic goes on console 0 -- so
     * bring console 0 to the front before painting it red. */
    fbcon_activate(0);
    g_active = 0;

    g_con[0].def_bg = 0x00800000;          /* dark red */
    g_con[0].def_fg = 0x00FFFFFF;
    clear_console(&g_con[0]);
}
