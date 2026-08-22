// ================================================================
// console.cpp — APinux OS 控制台实现
//   1. 帧缓冲控制台：解析 Multiboot v1 info 的 framebuffer 字段，
//      用 8x16 字体把文本画到线性帧缓冲（32/24bpp），支持滚动。
//   2. VGA 文本控制台：回退到 0xB8000 文本模式（80x25）。
// ================================================================
#include "console.h"
#include "font8x16_data.h"

#include <stddef.h>

// ---- Multiboot v1 info（只用我们需要的字段） ----
struct multiboot_info {
    uint32_t flags;            // 0x00
    uint32_t mem_lower;        // 0x04
    uint32_t mem_upper;        // 0x08
    uint32_t boot_device;      // 0x0C
    uint32_t cmdline;          // 0x10
    uint32_t mods_count;       // 0x14
    uint32_t mods_addr;        // 0x18
    uint32_t syms[4];          // 0x1C
    uint32_t mmap_length;      // 0x2C
    uint32_t mmap_addr;        // 0x30
    uint32_t drives_length;    // 0x34
    uint32_t drives_addr;      // 0x38
    uint32_t config_table;     // 0x3C
    uint32_t boot_loader_name; // 0x40
    uint32_t apm_table;        // 0x44
    uint32_t vbe_control_info; // 0x48
    uint32_t vbe_mode_info;    // 0x4C
    uint16_t vbe_mode;         // 0x50
    uint16_t vbe_interface_seg;// 0x52
    uint16_t vbe_interface_off;// 0x54
    uint16_t vbe_interface_len;// 0x56
    uint64_t framebuffer_addr; // 0x58
    uint32_t framebuffer_pitch;// 0x60
    uint32_t framebuffer_width;// 0x64
    uint32_t framebuffer_height;//0x68
    uint8_t  framebuffer_bpp;  // 0x6C
    uint8_t  framebuffer_type; // 0x6D
    uint8_t  color_info[6];    // 0x6E
};

#define MB_FLAG_FRAMEBUFFER 0x4

// ---- VGA 文本模式常量 ----
#define VGA_MEM      ((volatile uint16_t*)0xB8000)
#define VGA_COLS     80
#define VGA_ROWS     25
#define VGA_ATTR     0x0F   // 白字黑底

// ---- Bochs VBE（QEMU stdvga / Bochs 兼容，LFB 在 0xE0000000） ----
#define VBE_DISPI_INDEX 0x01CE
#define VBE_DISPI_DATA  0x01CF
#define VBE_DISPI_ID        0x00
#define VBE_DISPI_XRES      0x01
#define VBE_DISPI_YRES      0x02
#define VBE_DISPI_BPP       0x03
#define VBE_DISPI_ENABLE    0x04
#define VBE_DISPI_ENABLE_LFB 0x41
#define VBE_LFB_PHYS        0xE0000000u

static inline void vbe_write(uint16_t idx, uint16_t val) {
    asm volatile("outw %0, %1" : : "a"(idx), "Nd"(VBE_DISPI_INDEX));
    asm volatile("outw %0, %1" : : "a"(val), "Nd"(VBE_DISPI_DATA));
}

static inline uint16_t vbe_read(uint16_t idx) {
    asm volatile("outw %0, %1" : : "a"(idx), "Nd"(VBE_DISPI_INDEX));
    uint16_t v;
    asm volatile("inw %1, %0" : "=a"(v) : "Nd"(VBE_DISPI_DATA));
    return v;
}

// ---- PCI 配置空间（0xCF8/0xCFC）：找显示设备的 LFB BAR ----
// QEMU -kernel 不跑 VGA BIOS，BAR 由 QEMU 自行分配（stdvga 通常
// BAR0 = 0xfd000000），Bochs 规范里的 0xE0000000 不适用，必须现读。
static inline uint32_t pci_config_read(uint32_t bus, uint32_t dev,
                                       uint32_t func, uint32_t off) {
    uint32_t addr = 0x80000000u | (bus << 16) | (dev << 11) |
                    (func << 8) | (off & 0xFC);
    asm volatile("outl %0, %1" : : "a"(addr), "Nd"((uint16_t)0xCF8));
    uint32_t val;
    asm volatile("inl %1, %0" : "=a"(val) : "Nd"((uint16_t)0xCFC));
    return val;
}

static uint32_t pci_find_vga_lfb(void) {
    for (uint32_t d = 0; d < 32; d++) {
        uint32_t vid = pci_config_read(0, d, 0, 0) & 0xFFFF;
        if (vid == 0xFFFF || vid == 0)
            continue;
        uint32_t cls = (pci_config_read(0, d, 0, 8) >> 24) & 0xFF; // class
        if (cls != 0x03)  // 显示控制器
            continue;
        uint32_t bar0 = pci_config_read(0, d, 0, 0x10);
        if ((bar0 & 1) || (bar0 & 4))  // I/O BAR 或 64-bit BAR 跳过
            continue;
        return bar0 & 0xFFFFFFF0u;
    }
    return 0;
}

static bool vbe_set_mode(uint32_t w, uint32_t h, uint32_t bpp) {
    vbe_write(VBE_DISPI_ID, 0xB0C0);          // 启用 VBE 扩展
    vbe_write(VBE_DISPI_XRES, (uint16_t)w);
    vbe_write(VBE_DISPI_YRES, (uint16_t)h);
    vbe_write(VBE_DISPI_BPP, (uint16_t)bpp);
    vbe_write(VBE_DISPI_ENABLE, VBE_DISPI_ENABLE_LFB); // 开 LFB
    return true;
}

// ---- 帧缓冲控制台参数 ----
static volatile uint8_t* fb_base = nullptr;
static uint32_t fb_pitch = 0;
static uint32_t fb_width = 0;
static uint32_t fb_height = 0;
static uint32_t fb_bpp = 0;

// ---- ANSI VT200 16 色调色板（标准 xterm 近似） ----
static const uint32_t ansi_palette[16] = {
    0x000000, 0xAA0000, 0x00AA00, 0xAA5500,
    0x0000AA, 0xAA00AA, 0x00AAAA, 0xAAAAAA,
    0x555555, 0xFF5555, 0x55FF55, 0xFFFF55,
    0x5555FF, 0xFF55FF, 0x55FFFF, 0xFFFFFF,
};

#define FB_DEF_FG 0xFFFFFF   // 白
#define FB_DEF_BG 0x000020   // 深蓝黑底

// ---- 当前前景/背景（fb 用 RGB，vga 用调色板索引） ----
static uint32_t g_fg = FB_DEF_FG;
static uint32_t g_bg = FB_DEF_BG;
static uint8_t  g_vga_fg = 0x0F;
static uint8_t  g_vga_bg = 0x00;

// ---- 光标位置（文本单元） ----
static uint32_t cur_col = 0;
static uint32_t cur_row = 0;
static uint32_t cols = VGA_COLS;
static uint32_t rows = VGA_ROWS;
static bool use_fb = false;

// ---- ANSI 转义解析状态 ----
static const uint8_t ANSI_IDLE = 0;
static const uint8_t ANSI_ESC  = 1;
static const uint8_t ANSI_CSI  = 2;
static uint8_t ansi_state = ANSI_IDLE;
static uint8_t ansi_params[8];
static uint8_t ansi_nparams = 0;
static uint8_t ansi_private = 0;

static inline void fb_put_pixel(uint32_t x, uint32_t y, uint32_t color) {
    if (x >= fb_width || y >= fb_height)
        return;
    uint8_t* p = (uint8_t*)(fb_base + (uint64_t)y * fb_pitch + (uint64_t)x * (fb_bpp / 8));
    p[0] = color & 0xFF;
    p[1] = (color >> 8) & 0xFF;
    p[2] = (color >> 16) & 0xFF;
}

static void fb_fill_line(uint32_t y, uint32_t color) {
    for (uint32_t x = 0; x < fb_width; x++)
        fb_put_pixel(x, y, color);
}

static void fb_clear_line(uint32_t row) {
    uint32_t y0 = row * FONT_HEIGHT;
    for (uint32_t r = 0; r < FONT_HEIGHT; r++)
        fb_fill_line(y0 + r, g_bg);
}

static void fb_scroll(void) {
    for (uint32_t y = 0; y < fb_height - FONT_HEIGHT; y++)
        for (uint32_t x = 0; x < fb_width * (fb_bpp / 8); x++)
            fb_base[(uint64_t)y * fb_pitch + x] =
                fb_base[(uint64_t)(y + FONT_HEIGHT) * fb_pitch + x];
    fb_clear_line(rows - 1);
}

static void fb_draw_char(uint32_t row, uint32_t col, char c) {
    const unsigned char* glyph = &font8x16[(uint8_t)c * FONT_HEIGHT];
    uint32_t x0 = col * FONT_WIDTH;
    uint32_t y0 = row * FONT_HEIGHT;
    for (uint32_t r = 0; r < FONT_HEIGHT; r++) {
        for (uint32_t b = 0; b < FONT_WIDTH; b++) {
            uint32_t color = (glyph[r] & (0x80 >> b)) ? g_fg : g_bg;
            fb_put_pixel(x0 + b, y0 + r, color);
        }
    }
}

// ---- SGR：ANSI 颜色 -> 内部状态 ----
static void ansi_sgr(int p) {
    switch (p) {
        case 0:   g_fg = FB_DEF_FG; g_bg = FB_DEF_BG; g_vga_fg = 0x0F; g_vga_bg = 0; break;
        case 1:   g_fg = ansi_palette[(g_vga_fg & 7) | 8]; g_vga_fg |= 8; break;  // 加粗=亮色
        case 22:  g_fg = ansi_palette[g_vga_fg & 7]; g_vga_fg &= 7; break;
        case 7:   { uint32_t t = g_fg; g_fg = g_bg; g_bg = t;
                    uint8_t tv = g_vga_fg; g_vga_fg = (uint8_t)(g_vga_bg << 4 | (g_vga_fg & 7));
                    g_vga_bg = (uint8_t)(tv & 7); break; } // 反显
        default:
            if (p >= 30 && p <= 37) { g_vga_fg = (uint8_t)(p - 30); g_fg = ansi_palette[g_vga_fg]; }
            else if (p >= 90 && p <= 97) { g_vga_fg = (uint8_t)(p - 90 + 8); g_fg = ansi_palette[g_vga_fg]; }
            else if (p >= 40 && p <= 47) { g_vga_bg = (uint8_t)(p - 40); g_bg = ansi_palette[g_vga_bg]; }
            else if (p >= 100 && p <= 107) { g_vga_bg = (uint8_t)(p - 100 + 8); g_bg = ansi_palette[g_vga_bg]; }
            break;
    }
}

static void ansi_exec(void) {
    // final 字节已存 ansi_params[0..ansi_nparams-1]，本函数由 final 分派
    // （ansi_params[0] 存 final 字符本身，见 ansi_putchar）
    uint8_t final = ansi_params[0];
    int n = ansi_nparams - 1;
    int p[8];
    for (int i = 0; i < n && i < 8; i++) p[i] = ansi_params[i + 1];
    if (ansi_private) { ansi_state = ANSI_IDLE; return; }  // ?25h/l 等忽略
    switch (final) {
        case 'm': {  // SGR
            if (n == 0) ansi_sgr(0);
            for (int i = 0; i < n; i++) ansi_sgr(p[i]);
            break;
        }
        case 'J': {
            int mode = (n > 0) ? p[0] : 0;
            if (mode == 2) {
                if (use_fb) {
                    for (uint32_t r = 0; r < rows; r++)
                        fb_clear_line(r);
                } else {
                    for (int i = 0; i < VGA_COLS * VGA_ROWS; i++)
                        VGA_MEM[i] = (uint16_t)((uint8_t)(g_vga_bg << 4 | g_vga_fg) << 8 | ' ');
                }
            }
            break;
        }
        case 'K': {  // 清行
            int mode = (n > 0) ? p[0] : 0;
            if (mode == 2 || mode == 0) {
                if (use_fb) {
                    for (uint32_t c = cur_col; c < cols; c++)
                        fb_draw_char(cur_row, c, ' ');
                } else {
                    uint16_t attr = (uint16_t)((uint8_t)(g_vga_bg << 4 | g_vga_fg) << 8 | ' ');
                    for (uint32_t c = cur_col; c < VGA_COLS; c++)
                        VGA_MEM[cur_row * VGA_COLS + c] = attr;
                }
            }
            break;
        }
        case 'H': case 'f': {
            int r = (n > 0 && p[0]) ? p[0] - 1 : 0;
            int c = (n > 1 && p[1]) ? p[1] - 1 : 0;
            cur_row = (uint32_t)(r < 0 ? 0 : r);
            cur_col = (uint32_t)(c < 0 ? 0 : c);
            if (cur_row >= rows) cur_row = rows - 1;
            if (cur_col >= cols) cur_col = cols - 1;
            break;
        }
        case 'A': cur_row = (cur_row > (uint32_t)((n > 0 && p[0]) ? p[0] : 1)) ?
                             cur_row - ((n > 0 && p[0]) ? p[0] : 1) : 0; break;
        case 'B': cur_row += (uint32_t)((n > 0 && p[0]) ? p[0] : 1);
                  if (cur_row >= rows) cur_row = rows - 1;
                  break;
        case 'C': cur_col += (uint32_t)((n > 0 && p[0]) ? p[0] : 1);
                  if (cur_col >= cols) cur_col = cols - 1;
                  break;
        case 'D': cur_col = (cur_col > (uint32_t)((n > 0 && p[0]) ? p[0] : 1)) ?
                             cur_col - ((n > 0 && p[0]) ? p[0] : 1) : 0; break;
        default: break;
    }
    ansi_state = ANSI_IDLE;
}

// ---- ANSI 状态机：喂入一个字节 ----
static void fb_putchar(char c);
static void vga_putchar(char c);
static void ansi_putchar(char c) {
    switch (ansi_state) {
        case ANSI_IDLE:
            if (c == 0x1B) ansi_state = ANSI_ESC;
            else if (use_fb) fb_putchar(c);
            else vga_putchar(c);
            break;
        case ANSI_ESC:
            if (c == '[') {
                ansi_state = ANSI_CSI;
                ansi_nparams = 0;
                ansi_private = 0;
                ansi_params[0] = 0;
            } else {
                ansi_state = ANSI_IDLE;   // 未知序列：丢弃 ESC
            }
            break;
        case ANSI_CSI:
            if (c >= '0' && c <= '9') {
                uint8_t* slot = &ansi_params[ansi_nparams];
                if (ansi_nparams == 0) ansi_params[0] = 0;
                if (*slot < 100) { *slot = (uint8_t)(*slot * 10 + (c - '0')); }
            } else if (c == ';') {
                if (ansi_nparams < 7) ansi_nparams++;
            } else if (c == '?') {
                ansi_private = 1;
            } else {
                // final 字节：param 槽 0 存 final，其余为数值参数
                for (int i = 7; i > 0; i--) ansi_params[i] = ansi_params[i - 1];
                ansi_params[0] = (uint8_t)c;
                ansi_nparams++;
                ansi_exec();
            }
            break;
    }
}

// ---- VGA 文本模式 ----
static void vga_putchar(char c) {
    volatile uint16_t* vga = VGA_MEM;
    switch (c) {
        case '\r':
            cur_col = 0;
            return;
        case '\n':
            cur_col = 0;
            cur_row++;
            break;
        case '\t':
            cur_col = (cur_col + 4) & ~3u;
            break;
        default: {
            uint16_t attr = (uint16_t)((uint8_t)(g_vga_bg << 4 | g_vga_fg) << 8 | (uint8_t)c);
            vga[cur_row * VGA_COLS + cur_col] = attr;
            cur_col++;
            break;
        }
    }
    if (cur_col >= VGA_COLS) {
        cur_col = 0;
        cur_row++;
    }
    if (cur_row >= VGA_ROWS) {
        // 上滚一行
        for (int i = 0; i < (VGA_ROWS - 1) * VGA_COLS; i++)
            vga[i] = vga[i + VGA_COLS];
        for (int i = (VGA_ROWS - 1) * VGA_COLS; i < VGA_ROWS * VGA_COLS; i++)
            vga[i] = (uint16_t)((uint8_t)(g_vga_bg << 4 | g_vga_fg) << 8 | ' ');
        cur_row = VGA_ROWS - 1;
    }
}

// ---- 帧缓冲控制台 ----
static void fb_putchar(char c) {
    switch (c) {
        case '\r':
            cur_col = 0;
            return;
        case '\n':
            cur_col = 0;
            cur_row++;
            break;
        case '\t':
            cur_col = (cur_col + 4) & ~3u;
            break;
        default:
            fb_draw_char(cur_row, cur_col, c);
            cur_col++;
            break;
    }
    if (cur_col >= cols) {
        cur_col = 0;
        cur_row++;
    }
    if (cur_row >= rows) {
        fb_scroll();
        cur_row = rows - 1;
    }
}

// ---- 对外接口 ----
void console_clear(void) {
    cur_col = 0;
    cur_row = 0;
    if (use_fb) {
        for (uint32_t r = 0; r < rows; r++)
            fb_clear_line(r);
    } else {
        uint16_t attr = (uint16_t)((uint8_t)(g_vga_bg << 4 | g_vga_fg) << 8 | ' ');
        for (int i = 0; i < VGA_COLS * VGA_ROWS; i++)
            VGA_MEM[i] = attr;
    }
}

void console_putchar(char c) {
    ansi_putchar(c);
}

void console_write(const char* s) {
    while (s && *s)
        console_putchar(*s++);
}

void console_init(uint32_t magic, uint32_t info) {
    // 通道 1：Multiboot 帧缓冲（GRUB 图形模式路径）
    if (magic == 0x2BADB002 && info) {
        const multiboot_info* mbi = (const multiboot_info*)(uintptr_t)info;
        if ((mbi->flags & MB_FLAG_FRAMEBUFFER) && mbi->framebuffer_addr &&
            mbi->framebuffer_width && mbi->framebuffer_height) {
            fb_base   = (volatile uint8_t*)(uintptr_t)mbi->framebuffer_addr;
            fb_pitch  = mbi->framebuffer_pitch;
            fb_width  = mbi->framebuffer_width;
            fb_height = mbi->framebuffer_height;
            fb_bpp    = mbi->framebuffer_bpp;
            if (fb_base && (fb_bpp == 32 || fb_bpp == 24)) {
                use_fb = true;
                cols = fb_width / FONT_WIDTH;
                rows = fb_height / FONT_HEIGHT;
                cur_col = 0;
                cur_row = 0;
                for (uint32_t r = 0; r < rows; r++)
                    fb_clear_line(r);
                console_write("APinux console: multiboot framebuffer\r\n");
                return;
            }
        }
    }
    // 通道 2：Bochs VBE 自举（QEMU stdvga，1024x600x32，LFB 0xE0000000）
    fb_pitch  = 1024 * 4;
    fb_width  = 1024;
    fb_height = 600;
    fb_bpp    = 32;
    vbe_set_mode(fb_width, fb_height, fb_bpp);
    fb_base = (volatile uint8_t*)(uintptr_t)pci_find_vga_lfb();
    if (!fb_base)
        fb_base = (volatile uint8_t*)(uintptr_t)VBE_LFB_PHYS; // 兜底
    if (fb_base) {
        use_fb = true;
        cols = fb_width / FONT_WIDTH;
        rows = fb_height / FONT_HEIGHT;
        cur_col = 0;
        cur_row = 0;
        for (uint32_t r = 0; r < rows; r++)
            fb_clear_line(r);
        console_write("APinux console: Bochs VBE 1024x600x32\r\n");
        return;
    }
    // 通道 3：回退 VGA 文本模式（清屏）
    use_fb = false;
    cols = VGA_COLS;
    rows = VGA_ROWS;
    cur_col = 0;
    cur_row = 0;
    for (int i = 0; i < VGA_COLS * VGA_ROWS; i++)
        VGA_MEM[i] = (uint16_t)((VGA_ATTR << 8) | ' ');
    console_write("APinux console: VGA text mode\r\n");
}