// ================================================================
// vfs.cpp — APinux OS 虚拟文件系统
//   1. 内存文件系统：根目录 + 文件/目录树（create/lookup/read/write）
//   2. FAT32 只读：整盘载入内存，解析 BPB/FAT/目录簇链
//   3. 挂载点：g_vfs.mount("ata0", FS_FAT32) 挂到 /ata
// ================================================================
#include "vfs.h"
#include "ata.h"
#include <cstring>

extern "C" void* malloc(size_t size);
extern "C" void free(void* ptr);

// ---- 节点池（静态，避免堆碎片） ----
#define NODE_POOL 512
static VNode g_pool[NODE_POOL];
static bool  g_used[NODE_POOL];
static uint32_t g_seq = 1;

static VNode* alloc_node(void) {
    for (int i = 0; i < NODE_POOL; i++) {
        if (!g_used[i]) {
            g_used[i] = true;
            VNode* n = &g_pool[i];
            memset(n, 0, sizeof(VNode));
            n->inode = g_seq++;
            return n;
        }
    }
    return nullptr;
}

static void free_node(VNode* n) {
    if (!n)
        return;
    if (n->data)
        free(n->data);
    if (n->blocks)
        free(n->blocks);
    for (int i = 0; i < NODE_POOL; i++)
        if (&g_pool[i] == n) {
            g_used[i] = false;
            break;
        }
}

// ---- 磁盘镜像（整盘载入；FAT32 镜像大小） ----
#define FAT32_DISK_SECTORS 32768   // 16MB
static FAT32FS* g_fat = nullptr;

static void fmt_u32(uint32_t v, char* out) {
    char t[12];
    int i = 0;
    if (!v) { out[0] = '0'; out[1] = 0; return; }
    while (v) { t[i++] = (char)('0' + v % 10); v /= 10; }
    for (int j = 0; j < i; j++) out[j] = t[i - 1 - j];
    out[i] = 0;
}

bool fat32_get_info(char* out, size_t len) {
    if (!g_fat)
        return false;
    char b[16];
    char* o = out;
    size_t left = len - 1;
#define PUSH(s) do { const char* _p = (s); while (*_p && left) { *o++ = *_p++; left--; } } while (0)
#define PUSHU(v) do { fmt_u32((v), b); for (const char* _q = b; *_q && left; ) *o++ = *_q++, left--; } while (0)
    PUSH("FAT32: bps=");    PUSHU(g_fat->bps);
    PUSH(" spc=");          PUSHU(g_fat->spc);
    PUSH(" reserved=");     PUSHU(g_fat->reserved);
    PUSH(" fats=");         PUSHU(g_fat->numfat);
    PUSH(" fatsz=");        PUSHU(g_fat->fatsz);
    PUSH(" root=");         PUSHU(g_fat->root_cluster);
#undef PUSH
#undef PUSHU
    *o = 0;
    return true;
}

VirtualFileSystem g_vfs;

VirtualFileSystem::VirtualFileSystem() {
    root = alloc_node();
    root->type = VN_DIR;
    strcpy(root->name, "/");
    mounted_type = FS_FAT32;
    fs_specific = nullptr;
}

// ================================================================
// 内存文件系统
// ================================================================
static VNode* append_child(VNode* dir, VNode* child) {
    child->parent = dir;
    child->next_sibling = nullptr;
    if (!dir->children) {
        dir->children = child;
    } else {
        VNode* p = dir->children;
        while (p->next_sibling)
            p = p->next_sibling;
        p->next_sibling = child;
    }
    return child;
}

static bool name_equal(const char* a, const char* b) {
    // FAT32 名小写存储，比较时忽略大小写
    size_t i = 0;
    for (; a[i] && b[i]; i++)
        if (a[i] != b[i]) {
            char x = a[i], y = b[i];
            if (x >= 'A' && x <= 'Z') x += 32;
            if (y >= 'A' && y <= 'Z') y += 32;
            if (x != y)
                return false;
        }
    return a[i] == b[i];
}

static VNode* find_child(VNode* dir, const char* name) {
    for (VNode* c = dir->children; c; c = c->next_sibling)
        if (name_equal(c->name, name))
            return c;
    return nullptr;
}

// FAT32 目录延迟扫描：type 3 目录的 children 为 NULL 时按簇重建
static void fat_ensure_scanned(VNode* dir) {
    if (!dir || (dir->type != VN_FAT32_DIR && dir->type != VN_MOUNT))
        return;
    if (dir->children)
        return;
    if (g_fat && dir->type == VN_FAT32_DIR)
        dir->children = g_fat->scan_dir(dir->inode, dir);
}

bool VirtualFileSystem::mount(const char* device, FS_TYPE type) {
    (void)device;
    if (type != FS_FAT32 || g_fat)
        return false;
    uint8_t* img = (uint8_t*)malloc(FAT32_DISK_SECTORS * 512);
    if (!img)
        return false;
    for (uint32_t lba = 0; lba < FAT32_DISK_SECTORS; lba += 16) {
        if (!ata_read_sectors(lba, 16, img + (size_t)lba * 512)) {
            free(img);
            return false;
        }
    }
    g_fat = new FAT32FS();
    if (!g_fat->init(img)) {
        free(img);
        delete g_fat;
        g_fat = nullptr;
        return false;
    }
    // 挂载点节点 /ata
    VNode* mnt = alloc_node();
    mnt->type = VN_MOUNT;
    strcpy(mnt->name, "ata");
    mnt->inode = g_fat->root_cluster;
    mnt->children = g_fat->get_root();
    append_child(root, mnt);
    return true;
}

bool VirtualFileSystem::unmount() {
    if (g_fat) {
        if (g_fat->img)
            free(g_fat->img);
        delete g_fat;
        g_fat = nullptr;
    }
    // 摘掉 /ata
    VNode* prev = nullptr;
    for (VNode* c = root->children; c; c = c->next_sibling) {
        if (c->type == VN_MOUNT) {
            if (prev)
                prev->next_sibling = c->next_sibling;
            else
                root->children = c->next_sibling;
            free_node(c);
            break;
        }
        prev = c;
    }
    return true;
}

VNode* VirtualFileSystem::lookup(const char* path) {
    if (!path)
        return nullptr;
    if (strcmp(path, "/") == 0 || path[0] == 0)
        return root;
    VNode* cur = root;
    const char* p = path;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        char comp[256];
        int i = 0;
        while (*p && *p != '/' && i < 255)
            comp[i++] = *p++;
        comp[i] = 0;
        if (cur->type == VN_MOUNT || cur->type == VN_FAT32_DIR)
            fat_ensure_scanned(cur);
        VNode* next = find_child(cur, comp);
        if (!next)
            return nullptr;
        cur = next;
    }
    return cur;
}

VNode* VirtualFileSystem::create_file(const char* path) {
    // 只支持在内存目录中创建：父目录取最后一个 '/'
    char tmp[512];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = 0;
    char* slash = strrchr(tmp, '/');
    char* name = slash ? slash + 1 : tmp;
    if (slash) *slash = 0;
    VNode* dir = lookup(slash ? tmp : "/");
    if (!dir || (dir->type != VN_DIR && dir->type != VN_FAT32_DIR))
        return nullptr;
    if (find_child(dir, name))
        return nullptr;
    VNode* f = alloc_node();
    f->type = VN_FILE;
    strcpy(f->name, name);
    return append_child(dir, f);
}

bool VirtualFileSystem::delete_file(const char* path) {
    char tmp[512];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = 0;
    char* slash = strrchr(tmp, '/');
    char* name = slash ? slash + 1 : tmp;
    if (slash) *slash = 0;
    VNode* dir = lookup(slash ? tmp : "/");
    if (!dir)
        return false;
    VNode* prev = nullptr;
    for (VNode* c = dir->children; c; c = c->next_sibling) {
        if (name_equal(c->name, name)) {
            if (prev)
                prev->next_sibling = c->next_sibling;
            else
                dir->children = c->next_sibling;
            free_node(c);
            return true;
        }
        prev = c;
    }
    return false;
}

VNode* VirtualFileSystem::create_dir(const char* path) {
    char tmp[512];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = 0;
    char* slash = strrchr(tmp, '/');
    char* name = slash ? slash + 1 : tmp;
    if (slash) *slash = 0;
    VNode* dir = lookup(slash ? tmp : "/");
    if (!dir || dir->type != VN_DIR)
        return nullptr;
    if (find_child(dir, name))
        return nullptr;
    VNode* d = alloc_node();
    d->type = VN_DIR;
    strcpy(d->name, name);
    return append_child(dir, d);
}

bool VirtualFileSystem::read_file(VNode* node, void* buf, size_t offset, size_t len) {
    if (!node || node->type == VN_DIR || node->type == VN_MOUNT)
        return false;
    if (offset >= node->size)
        return false;
    if (offset + len > node->size)
        len = node->size - offset;
    if (node->data)
        memcpy(buf, node->data + offset, len);
    else
        memset(buf, 0, len);
    return true;
}

bool VirtualFileSystem::write_file(VNode* node, const void* buf, size_t offset, size_t len) {
    if (!node || node->type != VN_FILE)
        return false;
    size_t need = offset + len;
    if (need > node->size) {
        uint8_t* nd = (uint8_t*)malloc(need ? need : 1);
        if (!nd)
            return false;
        if (node->data) {
            memcpy(nd, node->data, node->size);
            free(node->data);
        }
        node->data = nd;
        node->size = need;
    }
    memcpy(node->data + offset, buf, len);
    return true;
}

VNode* VirtualFileSystem::read_dir(VNode* dir, size_t index) {
    if (!dir)
        return nullptr;
    if (dir->type == VN_FAT32_DIR || dir->type == VN_MOUNT)
        fat_ensure_scanned(dir);
    VNode* c = dir->children;
    for (size_t i = 0; c && i < index; i++)
        c = c->next_sibling;
    return c;
}

// ================================================================
// FAT32 只读实现
// ================================================================
bool FAT32FS::init(uint8_t* disk_image) {
    img = disk_image;
    const uint8_t* bpb = disk_image;
    bps = bpb[11] | (bpb[12] << 8);
    spc = bpb[13];
    reserved = bpb[14] | (bpb[15] << 8);
    numfat = bpb[16];
    fatsz = *(const uint32_t*)(bpb + 36);
    root_cluster = *(const uint32_t*)(bpb + 44);
    if (bps != 512 || spc == 0 || fatsz == 0)
        return false;
    cluster_size = (size_t)spc * bps;
    data_region = disk_image + ((size_t)reserved + (size_t)numfat * fatsz) * bps;
    fat = (uint32_t*)malloc((size_t)fatsz * bps);
    if (!fat)
        return false;
    memcpy(fat, disk_image + (size_t)reserved * bps, (size_t)fatsz * bps);
    return true;
}

uint32_t FAT32FS::next_cluster(uint32_t c) {
    uint32_t v = fat[c] & 0x0FFFFFFF;
    return (v >= 0x0FFFFFF8) ? 0 : v;   // 0 = EOC
}

bool FAT32FS::read_cluster(uint32_t cluster, void* buf) {
    if (cluster < 2)
        return false;
    uint8_t* src = data_region + (size_t)(cluster - 2) * cluster_size;
    // 防止越界：镜像 16MB
    if (src + cluster_size > (uint8_t*)img + FAT32_DISK_SECTORS * 512)
        return false;
    memcpy(buf, src, cluster_size);
    return true;
}

bool FAT32FS::write_cluster(uint32_t cluster, const void* buf) {
    (void)cluster;
    (void)buf;
    return false;   // 只读
}

// 8.3 短名 → 显示名（小写，去空格）
static void fat_short_name(const uint8_t* e, char* out) {
    int n = 0;
    for (int i = 0; i < 8 && e[i] && e[i] != ' '; i++)
        out[n++] = (char)e[i];
    if (e[8] && e[8] != ' ') {
        out[n++] = '.';
        for (int i = 8; i < 11 && e[i] && e[i] != ' '; i++)
            out[n++] = (char)e[i];
    }
    out[n] = 0;
    for (int i = 0; out[i]; i++)
        if (out[i] >= 'A' && out[i] <= 'Z')
            out[i] += 32;
}

VNode* FAT32FS::scan_dir(uint32_t cluster, VNode* parent) {
    VNode* head = nullptr;
    VNode* tail = nullptr;
    uint8_t* buf = (uint8_t*)malloc(cluster_size);
    if (!buf)
        return nullptr;
    while (cluster) {
        if (!read_cluster(cluster, buf))
            break;
        for (size_t off = 0; off < cluster_size; off += 32) {
            const uint8_t* e = buf + off;
            if (e[0] == 0) {            // 目录结束
                cluster = 0;
                break;
            }
            if (e[0] == 0xE5)           // 已删除
                continue;
            uint8_t attr = e[11];
            if (attr & 0x0F)            // LFN / 卷标 / 系统保留
                continue;
            VNode* n = alloc_node();
            if (!n) {
                free(buf);
                return head;
            }
            fat_short_name(e, n->name);
            n->parent = parent;
            uint32_t hi = (e[20] | (e[21] << 8));
            uint32_t lo = (e[26] | (e[27] << 8));
            n->inode = (hi << 16) | lo;
            if (attr & 0x10) {          // 子目录
                n->type = VN_FAT32_DIR;
            } else {
                n->type = VN_FAT32_FILE;
                n->size = *(const uint32_t*)(e + 28);
                if (n->size && n->size < (2u << 20)) {
                    uint8_t* data = (uint8_t*)malloc(n->size ? n->size : 1);
                    if (data) {
                        uint32_t c = n->inode;
                        size_t done = 0;
                        while (c && done < n->size) {
                            if (!read_cluster(c, buf)) break;
                            size_t chunk = cluster_size;
                            if (done + chunk > n->size)
                                chunk = n->size - done;
                            memcpy(data + done, buf, chunk);
                            done += chunk;
                            c = next_cluster(c);
                        }
                        n->data = data;
                    }
                }
            }
            if (tail)
                tail->next_sibling = n;
            else
                head = n;
            tail = n;
        }
        if (cluster)
            cluster = next_cluster(cluster);
    }
    free(buf);
    return head;
}

VNode* FAT32FS::get_root() {
    VNode* r = alloc_node();
    if (!r)
        return nullptr;
    r->type = VN_FAT32_DIR;
    r->inode = root_cluster;
    strcpy(r->name, "/");
    r->children = scan_dir(root_cluster, r);
    return r;
}

VNode* FAT32FS::lookup(VNode* parent, const char* name) {
    if (!parent)
        return nullptr;
    if (!parent->children)
        parent->children = scan_dir(parent->inode, parent);
    return find_child(parent, name);
}

// ================================================================
// EXT2 / APFS 桩（保留接口，未实现）
// ================================================================
bool EXT2FS::init(uint8_t* disk_image) { (void)disk_image; return false; }
VNode* EXT2FS::get_root() { return nullptr; }
VNode* EXT2FS::lookup(VNode* parent, const char* name) {
    (void)parent; (void)name; return nullptr;
}
bool EXT2FS::read_inode(uint32_t inode, VNode* out) {
    (void)inode; (void)out; return false;
}

bool APFS::format(uint8_t* disk, size_t size) { (void)disk; (void)size; return false; }
bool APFS::init(uint8_t* disk) { (void)disk; return false; }
VNode* APFS::alloc_node() { return nullptr; }
bool APFS::free_node(uint32_t inode) { (void)inode; return false; }