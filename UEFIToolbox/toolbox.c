/**
 * toolbox.c — UEFI 工具箱（GUI + 中文点阵字体）
 *
 * 自包含 UEFI 类型（无 EDK2 依赖），GOP 帧缓冲 GUI：
 *   主菜单：系统信息 / 内存测试 / 磁盘信息 / 启动项 / 关机 / 重启
 * 文本渲染：8x16 ASCII 点阵 + 16x16 中文点阵（unifont 提取）。
 *
 * 编译: clang --target=x86_64-unknown-windows-msvc -ffreestanding
 * 链接: lld-link /subsystem:efi_application /entry:efi_main
 */
#include <stdint.h>

#ifndef NULL
#define NULL ((void*)0)
#endif

typedef uint64_t EFI_STATUS;
typedef uint64_t UINTN;
typedef void*    EFI_HANDLE;
#define EFI_SUCCESS 0
#define EFI_BUFFER_TOO_SMALL 0x8000000000000005ull
#define EFI_NOT_FOUND       0x800000000000000Eull
#define EFI_UNSUPPORTED     0x8000000000000003ull

typedef struct { uint16_t ScanCode; uint16_t UnicodeChar; } EFI_INPUT_KEY;
typedef struct { uint32_t Data1; uint16_t Data2; uint16_t Data3; uint8_t Data4[8]; } EFI_GUID;
typedef struct { uint32_t Type; uint32_t Pad; uint64_t PhysicalStart, VirtualStart, NumberOfPages, Attribute; } EFI_MEMORY_DESCRIPTOR;

/* ---- 协议 ---- */
typedef struct {
    void* (*Reset)(void*, int);
    void* (*ReadKeyStroke)(void*, EFI_INPUT_KEY*);
} SIMPLE_INPUT;
typedef struct {
    void* (*Reset)(void*, int);
    void* (*OutputString)(void*, const uint16_t*);
} SIMPLE_OUTPUT;
typedef struct {
    uint32_t RedMask, GreenMask, BlueMask, ReservedMask;
} GOP_PIXEL_FORMAT;
typedef struct {
    uint32_t Version, HorizontalResolution, VerticalResolution, PixelFormat;
    GOP_PIXEL_FORMAT PixelInformation;
    uint32_t PixelsPerScanLine;
} GOP_MODE_INFO;
typedef struct {
    uint32_t MaxMode, Mode;
    GOP_MODE_INFO* Info;
    UINTN SizeOfInfo;
    void*  FrameBufferBase;
    UINTN FrameBufferSize;
} GOP_MODE;
typedef struct {
    void* (*QueryMode)(void*, uint32_t, UINTN*, GOP_MODE_INFO**);
    void* (*SetMode)(void*, uint32_t);
    void* (*GetInfo)(void*);
    void* (*GetMode)(void*);
    void* (*Blt)(void*);
    GOP_MODE* Mode;
} GOP;

/* ---- 服务表 ---- */
typedef struct {
    uint64_t Hdr[2];
    void* RaiseTpl;
    void* RestoreTpl;
    void* AllocatePages;
    void* FreePages;
    void* (*GetMemoryMap)(UINTN*, void*, UINTN*, UINTN*, uint32_t*);
    void* (*AllocatePool)(uint32_t, UINTN, void**);
    void* (*FreePool)(void*);
    void* pad1[19];
    void* (*ExitBootServices)(EFI_HANDLE, UINTN);
    void* pad2[5];
    void* (*OpenProtocol)(EFI_HANDLE, EFI_GUID*, void**, EFI_HANDLE, EFI_HANDLE, uint32_t);
    void* pad3[3];
    void* (*LocateHandleBuffer)(uint32_t, EFI_GUID*, void*, UINTN*, EFI_HANDLE**);
    void* (*LocateProtocol)(EFI_GUID*, void*, void**);
} BOOT_SERVICES;
typedef struct {
    uint64_t Hdr[2];
    void* GetTime;
    void* SetTime;
    void* GetWakeupTime;
    void* SetWakeupTime;
    void* SetVirtualAddressMap;
    void* ConvertPointer;
    void* (*GetVariable)(uint16_t*, EFI_GUID*, uint32_t*, UINTN*, void*);
    void* GetNextVariableName;
    void* SetVariable;
    void* GetNextHighMonotonicCount;
    void* (*ResetSystem)(int, EFI_STATUS, UINTN, void*);
} RUNTIME_SERVICES;
typedef struct {
    uint64_t   Signature;
    uint32_t   Revision;
    void*      FirmwareVendor;
    uint32_t   FirmwareRevision;
    EFI_HANDLE ConInHandle;
    SIMPLE_INPUT* ConIn;
    EFI_HANDLE ConOutHandle;
    SIMPLE_OUTPUT* ConOut;
    EFI_HANDLE StdErrHandle;
    SIMPLE_OUTPUT* StdErr;
    RUNTIME_SERVICES* RuntimeServices;
    BOOT_SERVICES*    BootServices;
} EFI_SYSTEM_TABLE;

static EFI_SYSTEM_TABLE* gST;
static GOP* g_gop;
static uint32_t g_fb_w, g_fb_h;
static uint32_t* g_fb;
static uint32_t g_fb_pitch;

static const EFI_GUID GUID_GOP = {0x9042a9de, 0x23dc, 0x4a38, {0x96,0xFB,0x7A,0xDE,0xD0,0x80,0x51,0x6A}};
static const EFI_GUID GUID_GLOBAL = {0x8BE4DF61, 0x93CA, 0x11d2, {0xAA,0x0D,0x00,0xE0,0x98,0x03,0x2B,0x8C}};

/* ---- 点阵字体 ---- */
#include "ascii_font.h"     /* ascii8x16[96][16] */
#include "chinese_font.h"   /* chinese_font[] + glyph16_t */

static const glyph16_t* find_zh(uint16_t uni) {
    int n = (int)(sizeof(chinese_font) / sizeof(chinese_font[0]));
    int i;
    for (i = 0; i < n; i++)
        if (chinese_font[i].unicode == uni) return &chinese_font[i];
    return NULL;
}

/* ---- 帧缓冲绘制 ---- */
static inline void put_pixel(uint32_t x, uint32_t y, uint32_t color) {
    if (x < g_fb_w && y < g_fb_h) g_fb[y * g_fb_pitch + x] = color;
}
static void draw_rect(int x, int y, int w, int h, uint32_t color) {
    int yy, xx;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)g_fb_w) w = (int)g_fb_w - x;
    if (y + h > (int)g_fb_h) h = (int)g_fb_h - y;
    if (w <= 0 || h <= 0) return;
    for (yy = y; yy < y + h; yy++)
        for (xx = x; xx < x + w; xx++)
            g_fb[yy * g_fb_pitch + xx] = color;
}

/* ---- 文本渲染（8x16 ASCII + 16x16 中文点阵） ---- */
static void put_ascii(char c, int x, int y, uint32_t color) {
    const uint8_t* g;
    int yy, xx;
    if (c < 32 || c > 126) c = '?';
    g = ascii8x16[(uint8_t)c - 32];
    for (yy = 0; yy < 16; yy++)
        for (xx = 0; xx < 8; xx++)
            if (g[yy] & (0x80 >> xx)) put_pixel((uint32_t)(x + xx), (uint32_t)(y + yy), color);
}
static void put_zh(uint16_t uni, int x, int y, uint32_t color) {
    const glyph16_t* gl = find_zh(uni);
    int yy, xx;
    if (!gl) {   /* 未找到：画空心框 */
        for (xx = 0; xx < 16; xx++) { put_pixel((uint32_t)(x+xx), (uint32_t)y, color); put_pixel((uint32_t)(x+xx), (uint32_t)(y+15), color); }
        for (yy = 0; yy < 16; yy++) { put_pixel((uint32_t)x, (uint32_t)(y+yy), color); put_pixel((uint32_t)(x+15), (uint32_t)(y+yy), color); }
        return;
    }
    for (yy = 0; yy < 16; yy++) {
        uint8_t b1 = gl->bitmap[yy * 2], b2 = gl->bitmap[yy * 2 + 1];
        for (xx = 0; xx < 8; xx++) if (b1 & (0x80 >> xx)) put_pixel((uint32_t)(x + xx), (uint32_t)(y + yy), color);
        for (xx = 0; xx < 8; xx++) if (b2 & (0x80 >> xx)) put_pixel((uint32_t)(x + 8 + xx), (uint32_t)(y + yy), color);
    }
}
/* UTF-8 解码 */
static uint16_t utf8_next(const char** sp) {
    const uint8_t* s = (const uint8_t*)*sp;
    uint16_t uni;
    if (s[0] < 0x80) { *sp += 1; return s[0]; }
    if ((s[0] & 0xE0) == 0xC0) { uni = (uint16_t)(((s[0] & 0x1F) << 6) | (s[1] & 0x3F)); *sp += 2; return uni; }
    if ((s[0] & 0xF0) == 0xE0) { uni = (uint16_t)(((s[0] & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F)); *sp += 3; return uni; }
    *sp += 1; return 0x3F;
}
/* 文本宽度（ASCII 8 + 中文 16） */
static int text_width(const char* s) {
    int w = 0;
    while (*s) { uint16_t u = utf8_next(&s); w += (u < 0x80) ? 8 : 16; }
    return w;
}
static void draw_text(int x, int y, uint32_t color, const char* s) {
    while (*s) {
        uint16_t u = utf8_next(&s);
        if (u < 0x80) { put_ascii((char)u, x, y, color); x += 8; }
        else { put_zh(u, x, y, color); x += 16; }
    }
}
/* CHAR16（固件字符串）→ 点阵渲染（ASCII 子集） */
static void draw_text16(int x, int y, uint32_t color, const uint16_t* s16) {
    char buf[128];
    int i = 0;
    while (s16[i] && i < 127) { buf[i] = (s16[i] < 0x80) ? (char)s16[i] : '?'; i++; }
    buf[i] = '\0';
    draw_text(x, y, color, buf);
}

/* ---- 数字转字符串 ---- */
static void itoa10(uint64_t v, char* out) {
    char t[24];
    int i = 0, j = 0;
    if (v == 0) { out[0] = '0'; out[1] = 0; return; }
    while (v > 0 && i < 22) { t[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i > 0) out[j++] = t[--i];
    out[j] = 0;
}

/* ---- 屏幕/菜单 ---- */
#define MENU_ITEMS 6
static const char* menu_items[MENU_ITEMS] = {
    "1. 系统信息", "2. 内存测试", "3. 磁盘信息", "4. 启动项", "5. 关机", "6. 重启"
};
static int g_sel;

static void draw_panel(int x, int y, int w, int h) {
    draw_rect(x, y, w, h, 0x182838);                 /* 面板底 */
    draw_rect(x, y, w, 2, 0x50A0E0);                 /* 顶高亮 */
    draw_rect(x, y + h - 2, w, 2, 0x102030);         /* 底边 */
}
static void draw_button(int x, int y, int w, int h, int hl, const char* label) {
    draw_rect(x, y, w, h, hl ? 0x4A90D9 : 0x2A4A6A);
    draw_text(x + (w - text_width(label)) / 2, y + (h - 16) / 2, 0xFFFFFF, label);
}

/* ---- 工具：系统信息 ---- */
static void tool_info(void) {
    char buf[48];
    draw_panel(60, 50, 700, 380);
    draw_text(90, 70, 0x00FF80, "系统信息");
    draw_text(90, 110, 0xC0C0C0, "固件: ");
    if (gST->FirmwareVendor) draw_text16(90 + 50, 110, 0xFFFFFF, (const uint16_t*)gST->FirmwareVendor);
    itoa10(gST->FirmwareRevision, buf);
    draw_text(90, 140, 0xC0C0C0, "版本: "); draw_text(90 + 50, 140, 0xFFFFFF, buf);
    itoa10(g_fb_w, buf); draw_text(90, 170, 0xC0C0C0, "分辨率: "); draw_text(90 + 65, 170, 0xFFFFFF, buf);
    draw_text(90 + 40 + 45, 170, 0xC0C0C0, "x"); 
    itoa10(g_fb_h, buf); draw_text(90 + 40 + 55, 170, 0xFFFFFF, buf);
    if (g_gop && g_gop->Mode && g_gop->Mode->Info)
        itoa10(g_gop->Mode->Info->PixelFormat, buf);
    draw_text(90, 200, 0xC0C0C0, "像素格式: "); draw_text(90 + 65, 200, 0xFFFFFF, buf);
    draw_button(90, 300, 200, 40, 1, "确定(回车)");
}

/* ---- 工具：内存测试（写入读取 pattern） ---- */
#define MEMTEST_SIZE (64u * 1024)
static uint8_t memtest_buf[MEMTEST_SIZE];
static void tool_memtest(void) {
    char buf[48];
    uint64_t i;
    int ok = 1;
    draw_panel(60, 50, 700, 380);
    draw_text(90, 70, 0x00FF80, "内存测试");
    draw_text(90, 110, 0xC0C0C0, "正在写入读取 4MB 模式...");
    /* 写 */
    for (i = 0; i < MEMTEST_SIZE / 4; i++) ((uint32_t*)memtest_buf)[i] = (uint32_t)(0xA5A55A5A ^ (i * 0x9E3779B9u));
    /* 读校验 */
    for (i = 0; i < MEMTEST_SIZE / 4; i++)
        if (((uint32_t*)memtest_buf)[i] != (uint32_t)(0xA5A55A5A ^ (i * 0x9E3779B9u))) { ok = 0; break; }
    draw_text(90, 150, ok ? 0x00FF00 : 0xFF4040, ok ? "测试通过" : "测试失败");
    itoa10(MEMTEST_SIZE / 1024, buf);
    draw_text(90, 190, 0xC0C0C0, "测试大小: "); draw_text(90 + 70, 190, 0xFFFFFF, buf); draw_text(90 + 70 + 60, 190, 0xC0C0C0, " KB");
    draw_button(90, 300, 200, 40, 1, "确定(回车)");
}

/* ---- 工具：磁盘信息（BlockIO 枚举） ---- */
typedef struct {
    uint32_t MediaId;
    int RemovableMedia, MediaPresent, LogicalPartition, ReadOnly, WriteCaching;
    uint32_t BlockSize, IoAlign;
    void* LastBlock;
} BLOCK_IO_MEDIA;
typedef struct {
    uint64_t Revision;
    BLOCK_IO_MEDIA* Media;
    void* (*Reset)(void*, int);
    void* (*ReadBlocks)(void*, uint32_t, uint64_t, UINTN, void*);
} BLOCK_IO;
static const EFI_GUID GUID_BLOCK_IO = {0x964E5B21, 0x6459, 0x11d2, {0x8E,0x39,0x00,0xA0,0xC9,0x69,0x72,0x3B}};
static void tool_disk(void) {
    char buf[48];
    UINTN nhandles = 0, i;
    EFI_HANDLE* handles = NULL;
    draw_panel(60, 50, 700, 380);
    draw_text(90, 70, 0x00FF80, "磁盘信息 (BlockIO)");
    if (gST->BootServices->LocateHandleBuffer(3, (EFI_GUID*)&GUID_BLOCK_IO, NULL, &nhandles, &handles) == EFI_SUCCESS) {
        itoa10(nhandles, buf);
        draw_text(90, 110, 0xC0C0C0, "块设备数: "); draw_text(90 + 65, 110, 0xFFFFFF, buf);
        for (i = 0; i < nhandles && i < 8; i++) {
            BLOCK_IO* bio = NULL;
            if (gST->BootServices->OpenProtocol(handles[i], (EFI_GUID*)&GUID_BLOCK_IO, (void**)&bio, NULL, NULL, 3) == EFI_SUCCESS && bio && bio->Media) {
                draw_text(90, 130 + (int)i * 24, 0xFFFFFF, "设备 ");
                itoa10(bio->Media->BlockSize, buf);
                draw_text(90 + 45, 130 + (int)i * 24, 0x00FF00, buf);
                draw_text(90 + 45 + 40, 130 + (int)i * 24, 0xC0C0C0, "B/块 大小 ");
                /* LastBlock 是 uint64* — 简化显示低 32 位 */
                itoa10((uint64_t)(uintptr_t)bio->Media->LastBlock & 0xFFFFFFFF, buf);
                draw_text(90 + 45 + 40 + 55, 130 + (int)i * 24, 0x00FF00, buf);
            }
        }
    } else {
        draw_text(90, 110, 0xFF4040, "未检测到块设备");
    }
    draw_button(90, 300, 200, 40, 1, "确定(回车)");
}

/* ---- 工具：启动项（BootOrder + Boot####） ---- */
static void tool_boot(void) {
    char buf[48];
    uint16_t boot_order[16];
    UINTN size = sizeof(boot_order);
    uint32_t attrs;
    uint32_t i;
    draw_panel(60, 50, 700, 380);
    draw_text(90, 70, 0x00FF80, "启动项 (BootOrder)");
    if (gST->RuntimeServices && gST->RuntimeServices->GetVariable &&
        gST->RuntimeServices->GetVariable((uint16_t*)L"BootOrder", (EFI_GUID*)&GUID_GLOBAL, &attrs, &size, boot_order) == EFI_SUCCESS) {
        for (i = 0; i < size / 2 && i < 8; i++) {
            itoa10(boot_order[i], buf);
            draw_text(90, 110 + (int)i * 24, 0xFFFFFF, "Boot");
            draw_text(90 + 35, 110 + (int)i * 24, 0x00FF00, buf);
        }
    } else {
        draw_text(90, 110, 0xFF4040, "未读取到启动项");
    }
    draw_button(90, 300, 200, 40, 1, "确定(回车)");
}

/* ---- 工具：关机/重启 ---- */
static void tool_shutdown(void) {
    draw_panel(60, 50, 700, 380);
    draw_text(90, 70, 0x00FF80, "关机");
    draw_text(90, 120, 0xC0C0C0, "正在关闭系统...");
    if (gST->RuntimeServices && gST->RuntimeServices->ResetSystem)
        gST->RuntimeServices->ResetSystem(1, EFI_SUCCESS, 0, NULL);   /* EfiResetShutdown */
    for (;;) __asm__ volatile("hlt");
}
static void tool_reboot(void) {
    if (gST->RuntimeServices && gST->RuntimeServices->ResetSystem)
        gST->RuntimeServices->ResetSystem(0, EFI_SUCCESS, 0, NULL);   /* EfiResetCold */
    for (;;) __asm__ volatile("hlt");
}

/* ---- 主菜单 ---- */
static void draw_main_menu(void) {
    int i, y;
    draw_rect(0, 0, g_fb_w, g_fb_h, 0x0E1622);
    draw_rect(0, 0, g_fb_w, 3, 0x4A90D9);
    draw_text((int)(g_fb_w - text_width("UEFI 工具箱")) / 2, 20, 0x00FF80, "UEFI 工具箱");
    draw_text((int)(g_fb_w - text_width("中文点阵字体 GUI")) / 2, 44, 0x808080, "中文点阵字体 GUI");
    draw_panel(60, 90, 700, 380);
    for (i = 0; i < MENU_ITEMS; i++) {
        y = 110 + i * 52;
        draw_button(90, y, 640, 40, i == g_sel, menu_items[i]);
    }
    draw_text(60, 500, 0x808080, "按上/下选择  回车确认  ESC返回");
}

static void run_tool(void) {
    switch (g_sel) {
    case 0: tool_info(); break;
    case 1: tool_memtest(); break;
    case 2: tool_disk(); break;
    case 3: tool_boot(); break;
    case 4: tool_shutdown(); break;
    case 5: tool_reboot(); break;
    }
}

/* ---- 按键等待（当前工具页，回车/Esc 返回） ---- */
static int wait_key(void) {
    EFI_INPUT_KEY k;
    for (;;) {
        if (gST->ConIn && gST->ConIn->ReadKeyStroke(gST->ConIn, &k) == EFI_SUCCESS) {
            if (k.UnicodeChar == '\r' || k.UnicodeChar == '\n' || k.UnicodeChar == 27) return 0;
        }
    }
}

/* ---- UEFI 入口 ---- */
EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE* SystemTable) {
    (void)ImageHandle;
    gST = SystemTable;
    if (gST->ConOut && gST->ConOut->OutputString)
        gST->ConOut->OutputString(gST->ConOut, L"Toolbox starting\r\n");
    /* GOP */
    if (gST->BootServices->LocateProtocol((EFI_GUID*)&GUID_GOP, NULL, (void**)&g_gop) == EFI_SUCCESS && g_gop->Mode && g_gop->Mode->Info) {
        g_fb = (uint32_t*)g_gop->Mode->FrameBufferBase;
        g_fb_w = g_gop->Mode->Info->HorizontalResolution;
        g_fb_h = g_gop->Mode->Info->VerticalResolution;
        g_fb_pitch = g_gop->Mode->Info->PixelsPerScanLine;
    } else {
        return 0x8000000000000003ull;   /* EFI_UNSUPPORTED */
    }
    if (gST->ConOut && gST->ConOut->Reset) gST->ConOut->Reset(gST->ConOut, 0);

    /* 主循环 */
    for (;;) {
        EFI_INPUT_KEY k;
        draw_main_menu();
        for (;;) {
            if (gST->ConIn && gST->ConIn->ReadKeyStroke(gST->ConIn, &k) == EFI_SUCCESS) {
                if (k.ScanCode == 0x01) { if (g_sel > 0) g_sel--; draw_main_menu(); }
                else if (k.ScanCode == 0x02) { if (g_sel < MENU_ITEMS - 1) g_sel++; draw_main_menu(); }
                else if (k.UnicodeChar == '\r' || k.UnicodeChar == '\n') { run_tool(); wait_key(); break; }
            }
        }
    }
    return EFI_SUCCESS;
}
