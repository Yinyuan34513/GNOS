// ================================================================
// readonly.cpp — APinux OS 文件只读保护引擎
// 依赖：vfs.h, kernel.h
// 功能：将文件标记为不可变（只读）、解除标记、查询状态
// ================================================================
#include "vfs.h"
#include <cstring>

// ---------- 只读标志定义 ----------
#define FLAG_READONLY   0x01    // 文件不可修改、不可删除
#define FLAG_APPEND     0x02    // 仅允许追加写入
#define FLAG_IMMUTABLE  0x04    // 完全不可变（连属性都不能改）

// ---------- VNode 扩展（已在 vfs.h 中添加 flags 字段） ----------
// 如果原始 vfs.h 未包含 flags，请在 VNode 结构体中添加：
//   uint32_t flags;
//   uint32_t owner_uid;   (可选)
//   uint32_t owner_gid;

// ---------- 只读管理器 ----------
class ReadOnlyManager {
    VirtualFileSystem* vfs;
public:
    void init(VirtualFileSystem* vfs_ptr) { vfs = vfs_ptr; }

    // 设置文件为只读
    bool set_readonly(const char* path) {
        VNode* node = vfs->lookup(path);
        if (!node) return false;
        node->flags |= FLAG_READONLY;
        return true;
    }

    // 解除只读
    bool clear_readonly(const char* path) {
        VNode* node = vfs->lookup(path);
        if (!node) return false;
        node->flags &= ~FLAG_READONLY;
        return true;
    }

    // 设置不可变
    bool set_immutable(const char* path) {
        VNode* node = vfs->lookup(path);
        if (!node) return false;
        node->flags |= FLAG_IMMUTABLE;
        return true;
    }

    // 检查文件是否只读
    bool is_readonly(const char* path) {
        VNode* node = vfs->lookup(path);
        if (!node) return false;
        return (node->flags & FLAG_READONLY) != 0;
    }

    // 标记系统关键文件为只读（启动时调用）
    void protect_system_files() {
        const char* critical_paths[] = {
            "/kernel",
            "/system/config",
            "/etc/apinux.conf",
            "/ai_engine.bin",
            nullptr
        };
        for (int i = 0; critical_paths[i]; ++i) {
            set_immutable(critical_paths[i]);
        }
    }
};

// ---------- 全局实例 ----------
ReadOnlyManager g_readonly_mgr;

// ---------- 内核系统调用接口 ----------
extern "C" int sys_set_readonly(const char* path) {
    return g_readonly_mgr.set_readonly(path) ? 0 : -1;
}

extern "C" int sys_clear_readonly(const char* path) {
    return g_readonly_mgr.clear_readonly(path) ? 0 : -1;
}

extern "C" int sys_is_readonly(const char* path) {
    return g_readonly_mgr.is_readonly(path) ? 1 : 0;
}

extern "C" int sys_snapshot_create(const char* desc) {
    return g_snapshot_mgr.create_snapshot(desc) ? 0 : -1;
}

extern "C" int sys_snapshot_rollback(uint32_t snap_id) {
    return g_snapshot_mgr.rollback_to(snap_id) ? 0 : -1;
}