/**
 * vboot.h — FastBoot 唯一契约：UEFI/BIOS 双协议统一抽象
 *
 * 上层（menu.c / loader.c）只依赖本头；平台差异全部由
 * vboot_uefi.c（EFI 变体）与 vboot_bios.asm（BIN 变体）封闭。
 */
#ifndef VBOOT_H
#define VBOOT_H

#include <stdint.h>

#define VBOOT_MAGIC 0x544F4F42u   /* 'B','O','O','T' */

/* 统一引导信息（跨固件传给内核，语义对齐 GNOS bootinfo.h） */
typedef struct {
    uint32_t magic;              /* VBOOT_MAGIC */
    uint32_t mem_base;           /* 内存图数组物理地址 */
    uint32_t mem_count;          /* 内存图条目数 */
    uint32_t video_width;        /* 图形模式宽（0=文本模式） */
    uint32_t video_height;
    uint32_t video_bpp;
    void*    framebuffer;        /* 帧缓冲地址（图形模式时） */
    void*    kernel_entry;       /* 内核入口 */
} vboot_bootinfo_t;

/* ---- VBoot 统一 API ---- */
int  vb_init(void);                                     /* 初始化，返回 1 成功 */
void vb_putc(char c);                                   /* 输出一个字符 */
void vb_puts(const char* s);                            /* 输出字符串 */
int  vb_getc(void);                                     /* 读一个键（阻塞），返回 ASCII */
int  vb_read_disk(uint32_t lba, void* buf, uint32_t n); /* 读 n 个扇区到 buf，返回 1 */
int  vb_load_file(const char* path, void* buf,
                  uint32_t max, uint32_t* size);       /* 按路径加载文件，返回 1 */
int  vb_get_memory_map(void* out, uint32_t max,
                       uint32_t* n);                    /* 内存图，返回 1 */
int  vb_set_video_mode(uint16_t w, uint16_t h,
                       uint16_t bpp);                   /* 图形模式，返回 1 */
void vb_reboot(void);                                   /* 复位（不返回） */
void vb_boot_kernel(vboot_bootinfo_t* info);            /* 引导内核（不返回） */

#endif /* VBOOT_H */
