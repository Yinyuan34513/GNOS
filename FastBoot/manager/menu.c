/**
 * menu.c — FastBoot 引导管理器（只调 VBoot，零平台代码）
 * 完整版：显示真实系统信息（内存图条数 / 磁盘可读性）。
 */
#include "vboot.h"

static char g_num[16];
static const char* itoa32(uint32_t v) {
    int i = 15;
    g_num[i] = '\0';
    if (v == 0) g_num[--i] = '0';
    while (v > 0 && i > 0) { g_num[--i] = (char)('0' + (v % 10)); v /= 10; }
    return g_num + i;
}

static const char* items[] = {
    "Test kernel (embedded)",
    "32os kernel (demo: stub)",
    "Reboot"
};
#define NITEMS 3

void boot_manager(void) {
    int sel = 0;
    for (;;) {
        int i, c;
        uint32_t mm = 0;
        uint32_t lba0[128];

        vb_puts("\r\nFastBoot -- UEFI/BIOS boot manager (VBoot)\r\n");
        vb_get_memory_map(0, 0, &mm);
        vb_puts("mem=");
        vb_puts(itoa32(mm));
        vb_puts("  disk=");
        vb_puts(vb_read_disk(0, lba0, 1) ? "yes" : "no");
        vb_puts("\r\n------------------------------------------\r\n");
        for (i = 0; i < NITEMS; i++) {
            vb_puts(i == sel ? "  > " : "    ");
            vb_puts(items[i]);
            vb_puts("\r\n");
        }
        vb_puts("------------------------------------------\r\n");
        vb_puts("w/s: move   Enter: boot   r: reboot\r\n");

        c = vb_getc();
        if (c == 'w' || c == 'W' || c == 0x11) {
            if (sel > 0) sel--;
        } else if (c == 's' || c == 'S' || c == 0x1F) {
            if (sel < NITEMS - 1) sel++;
        } else if (c == '\r' || c == '\n') {
            if (sel == 0) {
                vb_puts("\r\nBooting test kernel...\r\n");
                vb_boot_kernel(0);          /* 不返回 */
            } else if (sel == 1) {
                vb_puts("\r\n32os boot: demo stub (VBoot disk layer pending)\r\n");
            } else {
                vb_reboot();
            }
        } else if (c == 'r' || c == 'R') {
            vb_reboot();
        }
    }
}
