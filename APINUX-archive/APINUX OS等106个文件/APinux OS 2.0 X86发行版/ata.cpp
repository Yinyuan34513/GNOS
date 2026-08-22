// ================================================================
// ata.cpp — APinux OS ATA PIO 驱动（主通道 0x1F0，28-bit LBA）
// QEMU: -drive file=xxx.img,format=raw,if=ide → 主盘主设备
// ================================================================
#include "ata.h"
#include <string.h>

#define ATA_DATA    0x1F0
#define ATA_ERR     0x1F1
#define ATA_SECCNT  0x1F2
#define ATA_LBA_LOW 0x1F3
#define ATA_LBA_MID 0x1F4
#define ATA_LBA_HI  0x1F5
#define ATA_DRIVE   0x1F6
#define ATA_STATUS  0x1F7
#define ATA_CMD     0x1F7

#define ATA_SR_BSY  0x80
#define ATA_SR_DRDY 0x40
#define ATA_SR_DRQ  0x08
#define ATA_SR_ERR  0x01

static inline void outb(uint16_t port, uint8_t val) {
    asm volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t v;
    asm volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void insw(uint16_t port, void* buf, uint32_t count) {
    asm volatile("rep insw" : "+D"(buf), "+c"(count) : "d"(port) : "memory");
}

// 等待状态位就绪；timeout 单位是 io 轮询次数
static bool ata_wait(uint8_t mask_clear, uint8_t mask_set, int timeout) {
    for (int i = 0; i < timeout; i++) {
        uint8_t st = inb(ATA_STATUS);
        if (st & ATA_SR_ERR)
            return false;
        if (!(st & mask_clear) && (st & mask_set))
            return true;
    }
    return false;
}

bool ata_disk_present(void) {
    // 不发命令时通过 0x1F6 读回判断：不存在则收到 0xFF
    outb(ATA_DRIVE, 0xE0);
    uint8_t lo = inb(ATA_LBA_MID);
    uint8_t hi = inb(ATA_LBA_HI);
    if (lo == 0xFF && hi == 0xFF)
        return false;
    // 复位选择
    outb(ATA_DRIVE, 0xE0);
    return true;
}

bool ata_read_sectors(uint32_t lba, uint8_t count, void* buf) {
    if (count == 0)
        return true;
    if (!ata_wait(ATA_SR_BSY, ATA_SR_DRDY, 100000))
        return false;
    outb(ATA_DRIVE, 0xE0 | ((lba >> 24) & 0x0F));
    outb(ATA_SECCNT, count);
    outb(ATA_LBA_LOW, lba & 0xFF);
    outb(ATA_LBA_MID, (lba >> 8) & 0xFF);
    outb(ATA_LBA_HI, (lba >> 16) & 0xFF);
    outb(ATA_CMD, 0x20);   // READ SECTORS
    uint8_t* p = (uint8_t*)buf;
    for (uint8_t s = 0; s < count; s++) {
        if (!ata_wait(ATA_SR_BSY, ATA_SR_DRQ, 100000))
            return false;
        insw(ATA_DATA, p, 256);
        p += 512;
    }
    return true;
}

bool ata_identify(char* model_out, size_t model_len) {
    if (!ata_wait(ATA_SR_BSY, ATA_SR_DRDY, 100000))
        return false;
    outb(ATA_DRIVE, 0xE0);
    outb(ATA_SECCNT, 0);
    outb(ATA_LBA_LOW, 0);
    outb(ATA_LBA_MID, 0);
    outb(ATA_LBA_HI, 0);
    outb(ATA_CMD, 0xEC);
    if (!ata_wait(ATA_SR_BSY, ATA_SR_DRQ, 100000))
        return false;
    uint16_t buf[256];
    insw(ATA_DATA, buf, 256);
    if (model_out && model_len) {
        size_t n = 20;
        if (n > model_len / 2)
            n = model_len / 2;
        for (size_t i = 0; i < n; i++) {
            model_out[i * 2]     = (char)(buf[27 + i] >> 8);
            model_out[i * 2 + 1] = (char)(buf[27 + i] & 0xFF);
        }
        model_out[n * 2] = 0;
        for (size_t i = 0; i < n * 2 && model_out[i]; i++)
            if (model_out[i] == ' ')
                model_out[i] = 0;
    }
    return true;
}