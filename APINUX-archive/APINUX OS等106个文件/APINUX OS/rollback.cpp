// ================================================================
// rollback.cpp — APinux OS 系统快照回滚引擎
// 依赖：vfs.h, kernel.h
// 功能：创建系统快照、列出快照、回滚到指定快照
// ================================================================
#include "vfs.h"
#include "config.h"
#include <cstring>

// ---------- 快照数据结构 ----------
struct SnapshotHeader {
    uint32_t magic;           // 0x534E4150 ("SNAP")
    uint32_t version;         // 1
    uint32_t timestamp;       // Unix时间戳
    uint32_t total_inodes;    // 快照包含的inode数量
    char description[128];    // 用户描述
};

struct SnapshotEntry {
    uint32_t inode;
    uint8_t  sha256[32];      // 文件内容哈希
    uint32_t size;
    uint32_t flags;           // 文件属性
};

// ---------- 快照管理器 ----------
class SnapshotManager {
    VirtualFileSystem* vfs;
    static constexpr size_t MAX_SNAPSHOTS = 16;
    uint32_t snapshot_ids[MAX_SNAPSHOTS];
    size_t   snapshot_count = 0;
    const char* snap_dir = "/.snapshots";  // 快照存储目录

public:
    void init(VirtualFileSystem* vfs_ptr) { vfs = vfs_ptr; }

    // 创建当前文件系统的快照
    bool create_snapshot(const char* description) {
        if (snapshot_count >= MAX_SNAPSHOTS) return false;

        // 确保快照目录存在
        if (!vfs->lookup(snap_dir)) {
            vfs->create_dir(snap_dir);
        }

        // 生成快照ID
        uint32_t snap_id = (snapshot_count == 0) ? 1 : snapshot_ids[snapshot_count - 1] + 1;
        char snap_name[128];
        sprintf(snap_name, "%s/%u", snap_dir, snap_id);
        vfs->create_dir(snap_name);

        // 遍历所有inode，记录文件状态
        VNode* root = vfs->get_root();
        if (!root) return false;

        uint32_t total = 0;
        traverse_and_save(root, snap_name, total);

        // 写入快照头信息
        SnapshotHeader header;
        header.magic = 0x534E4150;
        header.version = 1;
        header.timestamp = get_timestamp();
        header.total_inodes = total;
        strncpy(header.description, description, 127);

        char header_path[128];
        sprintf(header_path, "%s/%u/header", snap_dir, snap_id);
        VNode* header_node = vfs->create_file(header_path);
        if (header_node) {
            vfs->write_file(header_node, &header, 0, sizeof(header));
        }

        snapshot_ids[snapshot_count++] = snap_id;
        return true;
    }

    // 列出所有快照
    bool list_snapshots(char* out_buf, size_t buf_size) {
        out_buf[0] = '\0';
        for (size_t i = 0; i < snapshot_count; ++i) {
            char header_path[128];
            sprintf(header_path, "%s/%u/header", snap_dir, snapshot_ids[i]);
            VNode* node = vfs->lookup(header_path);
            if (node) {
                SnapshotHeader hdr;
                vfs->read_file(node, &hdr, 0, sizeof(hdr));
                char line[256];
                sprintf(line, "[%u] %s (inodes: %u)\n", snapshot_ids[i], hdr.description, hdr.total_inodes);
                strncat(out_buf, line, buf_size - strlen(out_buf) - 1);
            }
        }
        return true;
    }

    // 回滚到指定快照
    bool rollback_to(uint32_t snap_id) {
        char snap_name[128];
        sprintf(snap_name, "%s/%u", snap_dir, snap_id);
        VNode* snap_root = vfs->lookup(snap_name);
        if (!snap_root) return false;

        // 读取快照条目，逐一恢复文件
        char entries_path[128];
        sprintf(entries_path, "%s/%u/entries", snap_dir, snap_id);
        VNode* entries_node = vfs->lookup(entries_path);
        if (!entries_node) return false;

        size_t entry_count = entries_node->size / sizeof(SnapshotEntry);
        SnapshotEntry* entries = (SnapshotEntry*)malloc(entries_node->size);
        vfs->read_file(entries_node, entries, 0, entries_node->size);

        for (size_t i = 0; i < entry_count; ++i) {
            SnapshotEntry& e = entries[i];
            // 查找或创建对应inode的文件
            // 从快照中复制数据恢复（实际需更复杂的回写，此处简化）
            // 核心是恢复文件内容和属性
            VNode* target = vfs->lookup_by_inode(e.inode);
            if (target) {
                // 设置只读标记等属性
                target->flags = e.flags;
            }
        }
        free(entries);
        return true;
    }

private:
    void traverse_and_save(VNode* node, const char* snap_path, uint32_t& count) {
        if (!node) return;
        if (node->type == 0) { // 文件
            SnapshotEntry entry;
            entry.inode = node->inode;
            entry.size = node->size;
            entry.flags = node->flags;
            compute_sha256(node->data, node->size, entry.sha256);
            // 存储到快照目录
            char entry_name[128];
            sprintf(entry_name, "%s/entries", snap_path);
            VNode* entries_node = vfs->lookup(entry_name);
            if (!entries_node) entries_node = vfs->create_file(entry_name);
            vfs->write_file(entries_node, &entry, sizeof(SnapshotEntry) * count, sizeof(SnapshotEntry));
            count++;
        } else if (node->type == 1) { // 目录
            for (VNode* child = node->children; child; child = child->next_sibling) {
                traverse_and_save(child, snap_path, count);
            }
        }
    }

    uint32_t get_timestamp() {
        // 简化：从RTC获取，此处返回固定值
        return 1234567890;
    }

    void compute_sha256(const uint8_t* data, size_t len, uint8_t* out) {
        // 简化的哈希计算（生产环境应使用完整SHA256）
        for (int i = 0; i < 32; ++i) out[i] = (data[i % len] ^ 0x5A) + i;
    }
};

// ---------- 全局实例 ----------
SnapshotManager g_snapshot_mgr;