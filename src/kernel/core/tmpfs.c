/*
 * tmpfs.c — in-memory filesystem backing mount(2). (GPLv2)
 *
 * See tmpfs.h for the design.  Storage is three static regions:
 *   - g_pool[]      : every tmpfs node (dir/file/symlink) lives here, handed
 *                     out through g_free_stack so allocation is O(1) and there
 *                     is never any real freeing to get wrong.
 *   - g_arena[]     : all file and symlink payload bytes, handed out by a bump
 *                     pointer.  Unlinked data is leaked; fine for a one-boot fs.
 *   - g_insts[]     : the per-mount instance structs (root + inode counter).
 */
#include <stdint.h>
#include <stddef.h>

#include "tmpfs.h"
#include "vfs.h"
#include "sysnum.h"
#include "kstring.h"

/* Forward declaration: fill_vfs() references these before their definitions
 * below. */
static const vfs_ops_t g_tmpfs_dir_ops;
static const vfs_ops_t g_tmpfs_file_ops;

/* ---- tunables --------------------------------------------------------- */
#define MAX_TMPFS_NODES 2048
#define TMPFS_ARENA     (8u * 1024 * 1024)
#define TMPFS_MAX_INST  8

/* ---- node ------------------------------------------------------------- */
struct tmpfs_node {
    char             name[VFS_NAME_MAX];
    int              kind;          /* VFS_DIR / VFS_FILE / VFS_SYMLINK */
    uint64_t         size;
    uint32_t         mode;          /* low 12 permission bits */
    uint32_t         ino;
    uint8_t         *data;          /* file payload (into g_arena) */
    uint32_t         datacap;       /* allocated payload capacity */
    char            *target;        /* symlink target (into g_arena) */
    struct tmpfs_node *parent;
    struct tmpfs_node *children;     /* linked list of children */
    struct tmpfs_node *next;         /* sibling link */
    int              refs;          /* 1 = the tree, +1 per open fd */
    int              unlinked;      /* detached; lives until last fd closes */
};

static int tmpfs_node_setsize(struct tmpfs_node *n, uint64_t len);

struct tmpfs_inst {
    struct tmpfs_node *root;
    uint32_t           next_ino;
};

/* ---- static storage --------------------------------------------------- */
static struct tmpfs_node g_pool[MAX_TMPFS_NODES];
static int  g_free_stack[MAX_TMPFS_NODES];
static int  g_free_sp = 0;
static int  g_pool_inited = 0;

static uint8_t g_arena[TMPFS_ARENA];
static uint32_t g_arena_off = 0;

static struct tmpfs_inst g_insts[TMPFS_MAX_INST];
static int g_inst_used = 0;

/* ---- low-level alloc -------------------------------------------------- */
static void pool_ensure_init(void)
{
    if (g_pool_inited)
        return;
    for (int i = 0; i < MAX_TMPFS_NODES; i++)
        g_free_stack[i] = MAX_TMPFS_NODES - 1 - i;
    g_free_sp = MAX_TMPFS_NODES;
    g_pool_inited = 1;
}

static struct tmpfs_node *node_alloc(void)
{
    pool_ensure_init();
    if (g_free_sp == 0)
        return NULL;
    int idx = g_free_stack[--g_free_sp];
    struct tmpfs_node *n = &g_pool[idx];
    memset(n, 0, sizeof *n);
    n->refs = 1;                  /* the directory tree owns one reference */
    return n;
}

static void node_free(struct tmpfs_node *n)
{
    int idx = (int)(n - g_pool);
    if (idx >= 0 && idx < MAX_TMPFS_NODES)
        g_free_stack[g_free_sp++] = idx;
}

static uint8_t *arena_alloc(uint32_t n)
{
    if (n == 0)
        n = 1;
    if (g_arena_off + n > TMPFS_ARENA)
        return NULL;
    uint8_t *p = &g_arena[g_arena_off];
    g_arena_off += n;
    return p;
}

/* ---- tree helpers ----------------------------------------------------- */
static struct tmpfs_node *find_child(struct tmpfs_node *dir,
                                     const char *name, size_t len)
{
    for (struct tmpfs_node *c = dir->children; c; c = c->next)
        if (strlen(c->name) == len && memcmp(c->name, name, len) == 0)
            return c;
    return NULL;
}

static void detach(struct tmpfs_node *parent, struct tmpfs_node *child)
{
    struct tmpfs_node **pp = &parent->children;
    while (*pp) {
        if (*pp == child) {
            *pp = child->next;
            return;
        }
        pp = &(*pp)->next;
    }
}

/* Walk `rel` (absolute) from `root`, honouring "." and "..".  Returns the
 * node or NULL when a component is missing.  Symlink components are NOT
 * followed -- a toy fs, and boot never needs it. */
static struct tmpfs_node *walk(struct tmpfs_node *root, const char *rel)
{
    if (!rel || rel[0] != '/')
        return NULL;
    struct tmpfs_node *cur = root;
    const char *p = rel;
    while (*p == '/')
        p++;
    while (*p) {
        const char *start = p;
        while (*p && *p != '/')
            p++;
        size_t clen = (size_t)(p - start);
        if (clen == 0) {
            p++;
            continue;
        }
        if (clen == 1 && start[0] == '.') {
            /* self */
        } else if (clen == 2 && start[0] == '.' && start[1] == '.') {
            if (cur->parent)
                cur = cur->parent;
        } else {
            struct tmpfs_node *c = find_child(cur, start, clen);
            if (!c)
                return NULL;
            cur = c;
        }
        while (*p == '/')
            p++;
    }
    return cur;
}

/* Resolve the parent directory and leaf name of `rel`.  Returns the parent
 * node (never root) or NULL when `rel` names the root itself or is malformed.
 * `leaf` receives the NUL-terminated basename. */
static struct tmpfs_node *split_parent(struct tmpfs_node *root,
                                       const char *rel, char *leaf,
                                       size_t leafcap)
{
    char tmp[GNUOS_PATH_MAX];
    strncpy(tmp, rel, GNUOS_PATH_MAX - 1);
    tmp[GNUOS_PATH_MAX - 1] = 0;

    size_t L = strlen(tmp);
    while (L > 1 && tmp[L - 1] == '/')
        tmp[--L] = 0;
    if (L == 0 || strcmp(tmp, "/") == 0)
        return NULL;

    /* last slash separates parent from leaf */
    size_t slash = 0;
    for (size_t i = 0; i <= L; i++)
        if (tmp[i] == '/')
            slash = i;
    tmp[slash] = 0;
    const char *leafp = tmp + slash + 1;
    size_t llen = strlen(leafp);
    if (llen == 0 || llen >= leafcap)
        return NULL;
    memcpy(leaf, leafp, llen + 1);

    const char *parent_path = (tmp[0] == 0) ? "/" : tmp;
    return walk(root, parent_path);
}

/* ---- vfs_node fill ---------------------------------------------------- */
static void fill_vfs(struct tmpfs_node *n, vfs_node_t *out)
{
    memset(out, 0, sizeof *out);
    strncpy(out->name, n->name, VFS_NAME_MAX - 1);
    out->name[VFS_NAME_MAX - 1] = 0;
    out->kind  = n->kind;
    out->size  = n->size;
    out->ops   = (n->kind == VFS_DIR) ? &g_tmpfs_dir_ops : &g_tmpfs_file_ops;
    out->priv  = n;
    out->e2.ino  = n->ino;
    out->e2.mode = (uint16_t)(n->mode & 0x0FFF);
}

/* ---- instance factory ------------------------------------------------- */
tmpfs_t *tmpfs_create(void)
{
    if (g_inst_used >= TMPFS_MAX_INST)
        return NULL;
    struct tmpfs_inst *fs = &g_insts[g_inst_used++];
    struct tmpfs_node *root = node_alloc();
    if (!root)
        return NULL;
    root->kind = VFS_DIR;
    root->mode = 0755;
    root->ino  = 1;
    strncpy(root->name, "/", VFS_NAME_MAX - 1);
    root->name[VFS_NAME_MAX - 1] = 0;
    fs->root     = root;
    fs->next_ino = 2;
    return fs;
}

/* ---- read / write (file nodes) --------------------------------------- */
int tmpfs_read(struct vfs_node *n, uint64_t off, void *buf, uint32_t len)
{
    struct tmpfs_node *node = (struct tmpfs_node *)n->priv;
    if (!node || node->kind != VFS_FILE)
        return -E_INVAL;
    uint64_t sz = node->size;
    if (off >= sz)
        return 0;
    uint64_t remain = sz - off;
    uint32_t tocopy = (remain < len) ? (uint32_t)remain : len;
    if (tocopy && node->data)
        memcpy(buf, node->data + off, tocopy);
    return (int32_t)tocopy;
}

int tmpfs_write(struct vfs_node *n, uint64_t off, const void *buf, uint32_t len)
{
    struct tmpfs_node *node = (struct tmpfs_node *)n->priv;
    if (!node || node->kind != VFS_FILE)
        return -E_INVAL;
    uint64_t end = off + len;
    if (end > node->datacap) {
        uint32_t newcap = (uint32_t)end;
        newcap = (newcap + 4095u) & ~(uint32_t)4095u;
        uint8_t *nd = arena_alloc(newcap);
        if (!nd)
            return -E_NOSPC;
        if (node->data && node->size)
            memcpy(nd, node->data, (uint32_t)node->size);
        node->data    = nd;
        node->datacap = newcap;
    }
    if (len)
        memcpy(node->data + off, buf, len);
    if (end > node->size)
        node->size = end;
    return (int32_t)len;
}

static int32_t tmpfs_dir_read(struct vfs_node *n, uint64_t off, void *buf,
                              uint32_t len)
{
    (void)n; (void)off; (void)buf; (void)len;
    return -E_ISDIR;
}

static int32_t tmpfs_dir_write(struct vfs_node *n, uint64_t off,
                               const void *buf, uint32_t len)
{
    (void)n; (void)off; (void)buf; (void)len;
    return -E_ISDIR;
}

static const vfs_ops_t g_tmpfs_dir_ops  = { .read = tmpfs_dir_read, .write = tmpfs_dir_write };
/* ---- open-file references (POSIX shm semantics) -----------------------
 * A vfs_file_t holds a copy of the resolved node whose priv points back at
 * the tmpfs_node.  Opening a file takes a reference and the last close
 * (release) drops it, so an unlinked-but-still-open object -- exactly what
 * shm_open + shm_unlink + ftruncate + mmap produces -- keeps its node and
 * data until every fd has gone away. */

static int tmpfs_node_truncate(vfs_node_t *n, uint64_t len)
{
    return tmpfs_node_setsize((struct tmpfs_node *)n->priv, len);
}

static void tmpfs_node_release(vfs_node_t *n)
{
    struct tmpfs_node *t = (struct tmpfs_node *)n->priv;
    if (!t)
        return;
    if (--t->refs == 0 && t->unlinked)
        node_free(t);
}

/* Does this resolved node name a tmpfs file?  (fchmod/ftruncate by fd need
 * to know so they can act on the node directly instead of by path, which
 * breaks for an unlinked object.) */
int tmpfs_is_file_node(const vfs_node_t *n)
{
    return n && n->ops == &g_tmpfs_file_ops;
}

/* Take a reference on a resolved tmpfs file node, for the open fd about to
 * be created from it.  The ops check is what keeps this from touching the
 * priv of every other node kind: device drivers and the like carry their
 * own data in priv, and writing a refcount into that would corrupt it. */
void tmpfs_retain(const vfs_node_t *n)
{
    if (!tmpfs_is_file_node(n))
        return;
    struct tmpfs_node *t = (struct tmpfs_node *)n->priv;
    if (t)
        t->refs++;
}

/* The live size of a resolved tmpfs file node.  An fd's vfs_node copy is
 * frozen at open time, but ftruncate (and the shared-memory keymap dance
 * where the file is unlinked) can change the real size afterwards, so mmap
 * and stat read this instead of the stale copy. */
uint64_t tmpfs_file_size(const vfs_node_t *n)
{
    struct tmpfs_node *t = (struct tmpfs_node *)n->priv;
    return t ? t->size : 0;
}

static const vfs_ops_t g_tmpfs_file_ops = {
    .read     = tmpfs_read,
    .write    = tmpfs_write,
    .truncate = tmpfs_node_truncate,
    .release  = tmpfs_node_release,
};

/* ---- resolve / readdir ------------------------------------------------ */
int tmpfs_resolve(tmpfs_t *fs, const char *rel, vfs_node_t *out)
{
    struct tmpfs_node *n = walk(fs->root, rel);
    if (!n)
        return -E_NOENT;
    fill_vfs(n, out);
    return 0;
}

int tmpfs_readdir(tmpfs_t *fs, const char *rel, uint32_t index,
                  char *name, uint8_t *type)
{
    struct tmpfs_node *dir = walk(fs->root, rel);
    if (!dir || dir->kind != VFS_DIR)
        return -E_NOENT;
    if (index == 0) {
        strncpy(name, ".", VFS_NAME_MAX - 1);
        name[VFS_NAME_MAX - 1] = 0;
        *type = DT_DIR;
        return 0;
    }
    if (index == 1) {
        strncpy(name, "..", VFS_NAME_MAX - 1);
        name[VFS_NAME_MAX - 1] = 0;
        *type = DT_DIR;
        return 0;
    }
    uint32_t i = 2;
    for (struct tmpfs_node *c = dir->children; c; c = c->next) {
        if (i == index) {
            strncpy(name, c->name, VFS_NAME_MAX - 1);
            name[VFS_NAME_MAX - 1] = 0;
            *type = (c->kind == VFS_DIR)  ? DT_DIR
                  : (c->kind == VFS_SYMLINK) ? DT_LNK
                  : DT_REG;
            return 0;
        }
        i++;
    }
    return -E_NOENT;
}

/* ---- mutating operations --------------------------------------------- */

/* Does `rel` name the root of this tmpfs -- that is, the mount point itself?
 * split_parent() cannot answer for "/": there is no parent to split off, so
 * it returns NULL and every caller below would report ENOENT.  That is the
 * wrong errno in each case, and not harmlessly so: `mkdir -p /tmp/x` calls
 * mkdir("/tmp") first and only carries on if the answer is EEXIST, so a
 * tmpfs that says ENOENT about its own mount point breaks mkdir -p for
 * everything underneath it. */
static int is_fs_root(const char *rel)
{
    if (!rel)
        return 1;
    while (*rel == '/')
        rel++;
    return *rel == 0;
}

int tmpfs_mkdir(tmpfs_t *fs, const char *rel, uint32_t mode)
{
    char leaf[VFS_NAME_MAX];
    if (is_fs_root(rel))
        return -E_EXIST;          /* the mount point is already a directory */
    struct tmpfs_node *parent = split_parent(fs->root, rel, leaf, sizeof leaf);
    if (!parent || parent->kind != VFS_DIR)
        return -E_NOENT;
    if (find_child(parent, leaf, strlen(leaf)))
        return -E_EXIST;
    struct tmpfs_node *n = node_alloc();
    if (!n)
        return -E_NOSPC;
    n->kind = VFS_DIR;
    n->mode = (mode & 07777) ? (mode & 07777) : 0755;
    n->ino  = fs->next_ino++;
    strncpy(n->name, leaf, VFS_NAME_MAX - 1);
    n->name[VFS_NAME_MAX - 1] = 0;
    n->parent  = parent;
    n->next    = parent->children;
    parent->children = n;
    return 0;
}

int tmpfs_create_file(tmpfs_t *fs, const char *rel, uint32_t mode)
{
    char leaf[VFS_NAME_MAX];
    if (is_fs_root(rel))
        return -E_ISDIR;          /* O_CREAT on a directory */
    struct tmpfs_node *parent = split_parent(fs->root, rel, leaf, sizeof leaf);
    if (!parent || parent->kind != VFS_DIR)
        return -E_NOENT;
    if (find_child(parent, leaf, strlen(leaf)))
        return -E_EXIST;
    struct tmpfs_node *n = node_alloc();
    if (!n)
        return -E_NOSPC;
    n->kind = VFS_FILE;
    n->mode = (mode & 07777) ? (mode & 07777) : 0644;
    n->ino  = fs->next_ino++;
    strncpy(n->name, leaf, VFS_NAME_MAX - 1);
    n->name[VFS_NAME_MAX - 1] = 0;
    n->parent  = parent;
    n->next    = parent->children;
    parent->children = n;
    return 0;
}

int tmpfs_unlink(tmpfs_t *fs, const char *rel)
{
    char leaf[VFS_NAME_MAX];
    if (is_fs_root(rel))
        return -E_ISDIR;
    struct tmpfs_node *parent = split_parent(fs->root, rel, leaf, sizeof leaf);
    if (!parent)
        return -E_NOENT;
    struct tmpfs_node *n = find_child(parent, leaf, strlen(leaf));
    if (!n)
        return -E_NOENT;
    if (n->kind == VFS_DIR)
        return -E_ISDIR;          /* use rmdir for directories */
    detach(parent, n);
    /* POSIX: an unlinked file lives until the last fd referencing it closes
     * (that is what makes shm_open + unlink + ftruncate work).  The tree
     * ref drops now; the file refs drop when their fds close. */
    if (--n->refs == 0)
        node_free(n);
    else
        n->unlinked = 1;
    return 0;
}

int tmpfs_rmdir(tmpfs_t *fs, const char *rel)
{
    char leaf[VFS_NAME_MAX];
    if (is_fs_root(rel))
        return -E_BUSY;           /* umount(2) is the way to remove a mount */
    struct tmpfs_node *parent = split_parent(fs->root, rel, leaf, sizeof leaf);
    if (!parent)
        return -E_NOENT;
    struct tmpfs_node *n = find_child(parent, leaf, strlen(leaf));
    if (!n)
        return -E_NOENT;
    if (n->kind != VFS_DIR)
        return -E_NOTDIR;
    if (n->children)
        return -E_NOTEMPTY;
    detach(parent, n);
    node_free(n);
    return 0;
}

int tmpfs_symlink(tmpfs_t *fs, const char *target, const char *rel)
{
    char leaf[VFS_NAME_MAX];
    if (is_fs_root(rel))
        return -E_EXIST;
    struct tmpfs_node *parent = split_parent(fs->root, rel, leaf, sizeof leaf);
    if (!parent || parent->kind != VFS_DIR)
        return -E_NOENT;
    if (find_child(parent, leaf, strlen(leaf)))
        return -E_EXIST;
    struct tmpfs_node *n = node_alloc();
    if (!n)
        return -E_NOSPC;
    n->kind = VFS_SYMLINK;
    n->mode = 0777;
    n->ino  = fs->next_ino++;
    strncpy(n->name, leaf, VFS_NAME_MAX - 1);
    n->name[VFS_NAME_MAX - 1] = 0;
    size_t tl = strlen(target);
    char *tp = (char *)arena_alloc((uint32_t)(tl + 1));
    if (!tp) {
        node_free(n);
        return -E_NOSPC;
    }
    memcpy(tp, target, tl + 1);
    n->target = tp;
    n->parent  = parent;
    n->next    = parent->children;
    parent->children = n;
    return 0;
}

int tmpfs_chmod(tmpfs_t *fs, const char *rel, uint32_t mode)
{
    struct tmpfs_node *n = walk(fs->root, rel);
    if (!n)
        return -E_NOENT;
    if (mode & 07777)
        n->mode = (mode & 07777);
    return 0;
}

int tmpfs_truncate(tmpfs_t *fs, const char *rel)
{
    struct tmpfs_node *n = walk(fs->root, rel);
    if (!n)
        return -E_NOENT;
    n->size = 0;                 /* payload bytes are leaked; writes restart at 0 */
    return 0;
}

/*
 * truncate(2)/ftruncate(2) to an arbitrary length.
 *
 * Both directions have to zero, and for the same reason: the arena is never
 * given back, so the bytes past the new end of file are still sitting there.
 * Shrinking without clearing them would let a later grow resurrect the old
 * contents -- one process's deleted data reappearing inside another's file.
 */
int tmpfs_setsize(tmpfs_t *fs, const char *rel, uint64_t len)
{
    struct tmpfs_node *n = walk(fs->root, rel);
    if (!n)
        return -E_NOENT;
    return tmpfs_node_setsize(n, len);
}

/* Resize a tmpfs file by node, not by path: ftruncate(2) on a shared-memory
 * object must work after the object was unlinked but is still open. */
int tmpfs_node_setsize(struct tmpfs_node *n, uint64_t len)
{
    if (n->kind == VFS_DIR)
        return -E_ISDIR;
    if (n->kind != VFS_FILE)
        return -E_INVAL;

    if (len > n->datacap) {
        uint32_t newcap = ((uint32_t)len + 4095u) & ~(uint32_t)4095u;
        uint8_t *nd = arena_alloc(newcap);
        if (!nd)
            return -E_NOSPC;
        if (n->data && n->size)
            memcpy(nd, n->data, (uint32_t)n->size);
        n->data    = nd;
        n->datacap = newcap;
    }

    if (n->data) {
        if (len > n->size)
            memset(n->data + n->size, 0, (uint32_t)(len - n->size));
        else if (len < n->size)
            memset(n->data + len, 0, (uint32_t)(n->size - len));
    }
    n->size = len;
    return 0;
}

/* readlink(2).  Returns the target length, like the ext2 one, so a caller can
 * tell "empty target" from "not a symlink". */
int tmpfs_readlink(tmpfs_t *fs, const char *rel, char *buf, uint32_t cap)
{
    struct tmpfs_node *n = walk(fs->root, rel);
    if (!n)
        return -E_NOENT;
    if (n->kind != VFS_SYMLINK || !n->target)
        return -E_INVAL;

    uint32_t len = (uint32_t)strlen(n->target);
    uint32_t w   = len < cap ? len : cap;
    memcpy(buf, n->target, w);
    return (int)len;
}

/*
 * rename(2) within one tmpfs.  Cross-filesystem moves are the caller's
 * problem (vfs_rename refuses them with EXDEV so coreutils falls back to
 * copy-then-unlink), which is what makes this a pointer shuffle: the node
 * keeps its identity, its payload and its children, and only its name and
 * its place in the parent's child list change.
 */
int tmpfs_rename(tmpfs_t *fs, const char *srel, const char *drel)
{
    char sleaf[VFS_NAME_MAX], dleaf[VFS_NAME_MAX];

    if (is_fs_root(srel) || is_fs_root(drel))
        return -E_BUSY;

    struct tmpfs_node *sp = split_parent(fs->root, srel, sleaf, sizeof sleaf);
    if (!sp)
        return -E_NOENT;
    struct tmpfs_node *src = find_child(sp, sleaf, strlen(sleaf));
    if (!src)
        return -E_NOENT;

    struct tmpfs_node *dp = split_parent(fs->root, drel, dleaf, sizeof dleaf);
    if (!dp || dp->kind != VFS_DIR)
        return -E_NOENT;

    /* Moving a directory into its own subtree would splice the tree into a
     * ring and hang the next walk() that goes through it. */
    for (struct tmpfs_node *a = dp; a; a = a->parent)
        if (a == src)
            return -E_INVAL;

    struct tmpfs_node *dst = find_child(dp, dleaf, strlen(dleaf));
    if (dst == src) {
        /* rename("a", "a"): POSIX says do nothing and succeed. */
        return 0;
    }
    if (dst) {
        /* POSIX: an existing destination is replaced, but only by something
         * of a compatible kind, and a directory must be empty first. */
        if (dst->kind == VFS_DIR && src->kind != VFS_DIR)
            return -E_ISDIR;
        if (dst->kind != VFS_DIR && src->kind == VFS_DIR)
            return -E_NOTDIR;
        if (dst->kind == VFS_DIR && dst->children)
            return -E_NOTEMPTY;
        detach(dp, dst);
        node_free(dst);
    }

    detach(sp, src);
    strncpy(src->name, dleaf, VFS_NAME_MAX - 1);
    src->name[VFS_NAME_MAX - 1] = 0;
    src->parent   = dp;
    src->next     = dp->children;
    dp->children  = src;
    return 0;
}
