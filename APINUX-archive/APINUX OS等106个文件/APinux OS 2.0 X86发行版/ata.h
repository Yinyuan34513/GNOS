// ================================================================
// ata.h — APinux OS ATA PIO 驱动（28-bit LBA，主通道）
// ================================================================
#pragma once
#include <cstdint>
#include <cstddef>

bool ata_disk_present(void);
bool ata_read_sectors(uint32_t lba, uint8_t count, void* buf);
bool ata_identify(char* model_out, size_t model_len);