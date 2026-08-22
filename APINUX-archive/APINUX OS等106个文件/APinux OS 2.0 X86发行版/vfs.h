// ================================================================
// vfs.h — APinux 虚拟文件系统
// 支持：标准 FAT32, EXT2(只读), APFS (APinux FS)
// ================================================================
#pragma once
#include "kernel.h"
#include <cstdint>

// ---------- 通用文件/目录结构 ----------
// type: 0=内存文件, 1=内存目录, 2=挂载点(ata), 3=FAT32目录, 4=FAT32文件
enum { VN_FILE = 0, VN_DIR = 1, VN_MOUNT = 2, VN_FAT32_DIR = 3, VN_FAT32_FILE = 4 };

struct VNode {
    uint32_t inode;
    uint32_t type;       // 0=file, 1=directory, 2=mount, 3/4=FAT32
    char name[256];
    size_t size;
    uint8_t* data;       // 对于小文件直接内嵌，大文件用块指针
    uint32_t* blocks;    // 块号数组
    size_t num_blocks;
    VNode* parent;
    VNode* children;
    VNode* next_sibling;
};

enum FS_TYPE { FS_FAT32, FS_EXT2, FS_APFS };

class VirtualFileSystem {
    VNode* root;
    FS_TYPE mounted_type;
    void* fs_specific;   // 指向具体文件系统数据
public:
    VirtualFileSystem();
    bool mount(const char* device, FS_TYPE type);
    bool unmount();
    VNode* lookup(const char* path);
    VNode* create_file(const char* path);
    bool delete_file(const char* path);
    VNode* create_dir(const char* path);
    bool read_file(VNode* node, void* buf, size_t offset, size_t len);
    bool write_file(VNode* node, const void* buf, size_t offset, size_t len);
    VNode* read_dir(VNode* dir, size_t index);
};

// 构建修复：全局 VFS 实例（vfs.cpp 定义，ckr/userlib 引用）
extern VirtualFileSystem g_vfs;

// FAT32 BPB 摘要（供 shell 'fatinfo'；未挂载返回 false）
bool fat32_get_info(char* out, size_t len);

// ---------- FAT32 实现 ----------
class FAT32FS {
    VirtualFileSystem* vfs;
    uint32_t* fat;
    uint8_t* data_region;
    size_t cluster_size;
public:
    uint32_t bps, spc, reserved, numfat, fatsz, root_cluster;  // BPB 摘要
    bool init(uint8_t* disk_image);
    VNode* get_root();
    VNode* lookup(VNode* parent, const char* name);
    bool read_cluster(uint32_t cluster, void* buf);
    bool write_cluster(uint32_t cluster, const void* buf);
    uint32_t next_cluster(uint32_t c);
    VNode* scan_dir(uint32_t cluster, VNode* parent);
    void* img;   // 磁盘镜像（整盘载入内存）
};

// ---------- EXT2 只读实现 ----------
class EXT2FS {
    VirtualFileSystem* vfs;
    void* superblock;
public:
    bool init(uint8_t* disk_image);
    VNode* get_root();
    VNode* lookup(VNode* parent, const char* name);
    bool read_inode(uint32_t inode, VNode* out);
};

// ---------- APFS (APinux FS) 实现 (简化日志文件系统) ----------
class APFS {
    VirtualFileSystem* vfs;
public:
    bool format(uint8_t* disk, size_t size);
    bool init(uint8_t* disk);
    VNode* alloc_node();
    bool free_node(uint32_t inode);
};