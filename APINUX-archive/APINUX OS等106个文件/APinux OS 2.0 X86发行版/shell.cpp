// ================================================================
// shell.cpp — APinux OS 内核 shell
//   输入：COM1 + PS/2 键盘，带行编辑（退格/Ctrl+C）
//   输出：串口 + 帧缓冲控制台双通道（VT200 ANSI 彩色）
//   命令：help/echo/ps/mem/clear/ver/pci/reboot/halt/
//         ls/cat/write/rm/mount/fatinfo/disk
// ================================================================
#include "shell.h"
#include "console.h"
#include "kernel.h"
#include "kbd.h"
#include "vfs.h"
#include "ata.h"
#include <cstring>

extern void serial_write(const char* s);
extern FlowMemPool g_kernel_pool;
extern ProcessManager g_proc_mgr;

// ---- VT200 ANSI 颜色 ----
#define C_RESET  "\x1b[0m"
#define C_CYAN   "\x1b[36m"
#define C_GREEN  "\x1b[32m"
#define C_RED    "\x1b[31m"
#define C_BLUE   "\x1b[34m"
#define C_YELLOW "\x1b[33m"

static inline uint8_t inb(uint16_t port) {
    uint8_t v;
    asm volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void outb(uint16_t port, uint8_t val) {
    asm volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint32_t inl(uint16_t port) {
    uint32_t v;
    asm volatile("inl %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void outl(uint16_t port, uint32_t val) {
    asm volatile("outl %0, %1" : : "a"(val), "Nd"(port));
}

// ---- 输出：串口 + 控制台 ----
static void sh_out(const char* s) {
    serial_write(s);
    console_write(s);
}

static void sh_outc(char c) {
    char b[2] = {c, 0};
    serial_write(b);
    console_putchar(c);
}

// ---- 串口输入 ----
static int serial_rx_ready(void) { return (inb(0x3F8 + 5) & 1) != 0; }
static char serial_getc(void)     { return (char)inb(0x3F8); }

// ---- 数字格式化 ----
static void u32_to_dec(uint32_t v, char* out) {
    char tmp[12];
    int i = 0;
    if (v == 0) { out[0] = '0'; out[1] = 0; return; }
    while (v) { tmp[i++] = (char)('0' + v % 10); v /= 10; }
    for (int j = 0; j < i; j++) out[j] = tmp[i - 1 - j];
    out[i] = 0;
}

static void u32_to_hex(uint32_t v, char* out) {
    static const char* H = "0123456789ABCDEF";
    out[0] = '0'; out[1] = 'x';
    for (int i = 0; i < 8; i++) out[2 + i] = H[(v >> (28 - i * 4)) & 0xF];
    out[10] = 0;
}

// ---- 命令实现 ----
static void cmd_help(void) {
    sh_out(C_CYAN "APinux shell commands:" C_RESET "\r\n"
           "  help      - this list\r\n"
           "  echo      - echo arguments\r\n"
           "  ps        - list processes\r\n"
           "  mem       - memory pool info\r\n"
           "  pci       - scan PCI bus 0\r\n"
           "  clear     - clear screen\r\n"
           "  ver       - version info\r\n"
           "  reboot    - reboot machine\r\n"
           "  halt      - halt CPU\r\n"
           "  ls [path] - list directory (VFS)\r\n"
           "  cat <p>   - print file\r\n"
           "  write <p> <text> - create file in memory\r\n"
           "  rm <p>    - delete file\r\n"
           "  mount     - mount FAT32 disk at /ata\r\n"
           "  fatinfo   - FAT32 BPB info\r\n"
           "  disk      - ATA identify\r\n");
}

static void cmd_ps(void) {
    for (Process* p = g_proc_mgr.first_process(); p; p = p->next) {
        char line[64];
        char buf[8];
        char* o = line;
        *o++ = ' '; *o++ = ' ';
        u32_to_dec(p->pid, buf); const char* b = buf; while (*b) *o++ = *b++;
        *o++ = ' '; *o++ = ' ';
        const char* n = p->name; while (*n && *n != ' ') *o++ = *n++;
        *o++ = ' ';
        switch (p->state) {
            case 0: *o++ = 'r'; *o++ = 'e'; *o++ = 'a'; *o++ = 'd'; *o++ = 'y'; break;
            case 1: *o++ = 'r'; *o++ = 'u'; *o++ = 'n'; break;
            default: *o++ = 'b'; *o++ = 'l'; *o++ = 'k'; break;
        }
        *o++ = '\r'; *o++ = '\n'; *o = 0;
        sh_out(line);
    }
}

static void cmd_mem(void) {
    char line[64];
    char buf[12];
    char* o = line;
    const char* p = "heap pool total: ";
    while (*p) *o++ = *p++;
    u32_to_dec((uint32_t)(g_kernel_pool.get_pool_size() / 1024 / 1024), buf);
    p = buf; while (*p) *o++ = *p++;
    p = " MB\r\n"; while (*p) *o++ = *p++;
    *o = 0;
    sh_out(line);
}

static void cmd_pci(void) {
    for (uint32_t d = 0; d < 32; d++) {
        uint32_t addr = 0x80000000u | (d << 11);
        outl(0xCF8, addr);
        uint32_t id = inl(0xCFC);
        uint16_t vid = id & 0xFFFF;
        if (vid == 0xFFFF || vid == 0)
            continue;
        outl(0xCF8, addr | 8);
        uint32_t cc = inl(0xCFC);
        char line[64];
        char b0[16];
        char* o = line;
        const char* p = " dev ";
        while (*p) *o++ = *p++;
        u32_to_dec(d, b0); p = b0; while (*p) *o++ = *p++;
        p = " vendor="; while (*p) *o++ = *p++;
        u32_to_hex(vid, b0); p = b0; while (*p) *o++ = *p++;
        p = " device="; while (*p) *o++ = *p++;
        u32_to_hex((id >> 16) & 0xFFFF, b0); p = b0; while (*p) *o++ = *p++;
        p = " class="; while (*p) *o++ = *p++;
        u32_to_hex((cc >> 24) & 0xFF, b0); p = b0; while (*p) *o++ = *p++;
        *o++ = '\r'; *o++ = '\n'; *o = 0;
        sh_out(line);
    }
}

static void cmd_echo(char* args) {
    while (*args == ' ') args++;
    sh_out(args);
    sh_out("\r\n");
}

static void cmd_ver(void) {
    sh_out(C_CYAN "APinux OS 2.0 x86_64 kernel (console mode)" C_RESET "\r\n");
}

// ---- VFS / 磁盘命令 ----
static void cmd_ls(char* args) {
    while (*args == ' ') args++;
    const char* path = *args ? args : "/";
    VNode* dir = g_vfs.lookup(path);
    if (!dir || (dir->type != VN_DIR && dir->type != VN_FAT32_DIR &&
                 dir->type != VN_MOUNT)) {
        sh_out(C_RED "ls: not a directory" C_RESET "\r\n");
        return;
    }
    size_t idx = 0;
    for (;;) {
        VNode* c = g_vfs.read_dir(dir, idx++);
        if (!c)
            break;
        char b[16];
        char out[360];
        char* o = out;
        if (c->type == VN_DIR || c->type == VN_FAT32_DIR || c->type == VN_MOUNT) {
            const char* p = C_BLUE; while (*p) *o++ = *p++;
            const char* n = c->name; while (*n) *o++ = *n++;
            *o++ = '/';
            p = C_RESET; while (*p) *o++ = *p++;
        } else {
            const char* n = c->name; while (*n) *o++ = *n++;
            *o++ = ' '; *o++ = ' ';
            u32_to_dec((uint32_t)c->size, b);
            const char* q = b; while (*q) *o++ = *q++;
            *o++ = ' '; *o++ = 'b'; *o++ = 'y'; *o++ = 't'; *o++ = 'e'; *o++ = 's';
        }
        *o++ = '\r'; *o++ = '\n'; *o = 0;
        sh_out(out);
    }
}

static void cmd_cat(char* args) {
    while (*args == ' ') args++;
    if (!*args) {
        sh_out(C_RED "usage: cat <path>" C_RESET "\r\n");
        return;
    }
    VNode* f = g_vfs.lookup(args);
    if (!f || f->type == VN_DIR || f->type == VN_FAT32_DIR || f->type == VN_MOUNT) {
        sh_out(C_RED "cat: not found" C_RESET "\r\n");
        return;
    }
    if (f->size > 4096) {
        sh_out(C_RED "cat: file too large" C_RESET "\r\n");
        return;
    }
    char buf[4096];
    if (!g_vfs.read_file(f, buf, 0, f->size)) {
        sh_out(C_RED "cat: read error" C_RESET "\r\n");
        return;
    }
    for (size_t i = 0; i < f->size; i++)
        sh_outc(buf[i]);
    sh_out("\r\n");
}

static void cmd_write(char* args) {
    while (*args == ' ') args++;
    char* sp = strchr(args, ' ');
    if (!sp || !*args) {
        sh_out(C_RED "usage: write <path> <text>" C_RESET "\r\n");
        return;
    }
    *sp = 0;
    const char* path = args;
    const char* text = sp + 1;
    VNode* f = g_vfs.lookup(path);
    if (!f)
        f = g_vfs.create_file(path);
    if (!f) {
        sh_out(C_RED "write: cannot create file" C_RESET "\r\n");
        return;
    }
    if (!g_vfs.write_file(f, text, 0, strlen(text))) {
        sh_out(C_RED "write: failed" C_RESET "\r\n");
        return;
    }
    char b[16];
    sh_out(C_GREEN "wrote " C_RESET);
    u32_to_dec((uint32_t)strlen(text), b);
    sh_out(b);
    sh_out(" bytes\r\n");
}

static void cmd_rm(char* args) {
    while (*args == ' ') args++;
    if (!*args) {
        sh_out(C_RED "usage: rm <path>" C_RESET "\r\n");
        return;
    }
    if (g_vfs.delete_file(args))
        sh_out(C_GREEN "removed" C_RESET "\r\n");
    else
        sh_out(C_RED "rm: not found" C_RESET "\r\n");
}

static void cmd_mount(void) {
    if (g_vfs.mount("ata0", FS_FAT32))
        sh_out(C_GREEN "mounted FAT32 at /ata" C_RESET "\r\n");
    else
        sh_out(C_RED "mount failed (no disk?)" C_RESET "\r\n");
}

static void cmd_fatinfo(void) {
    char buf[128];
    if (!fat32_get_info(buf, sizeof(buf))) {
        sh_out(C_RED "FAT32 not mounted (try 'mount')" C_RESET "\r\n");
        return;
    }
    sh_out(buf);
    sh_out("\r\n");
}

static void cmd_disk(void) {
    char model[64];
    if (!ata_disk_present()) {
        sh_out(C_RED "no ATA disk present" C_RESET "\r\n");
        return;
    }
    sh_out(C_GREEN "ATA disk: " C_RESET);
    if (ata_identify(model, sizeof(model)))
        sh_out(model);
    else
        sh_out("(identify failed)");
    sh_out("\r\n");
}

// ---- 命令分发 ----
static void shell_exec(const char* line) {
    static char arg[256];
    const char* p = line;
    while (*p == ' ') p++;
    const char* cmd = p;
    while (*p && *p != ' ') p++;
    size_t clen = (size_t)(p - cmd);
    while (*p == ' ') p++;
    size_t alen = 0;
    while (p[alen] && alen < sizeof(arg) - 1) { arg[alen] = p[alen]; alen++; }
    arg[alen] = 0;

    if (clen == 4 && (cmd[0]=='h')&&(cmd[1]=='e')&&(cmd[2]=='l')&&(cmd[3]=='p')) cmd_help();
    else if (clen == 4 && (cmd[0]=='e')&&(cmd[1]=='c')&&(cmd[2]=='h')&&(cmd[3]=='o')) cmd_echo(arg);
    else if (clen == 2 && (cmd[0]=='p')&&(cmd[1]=='s')) cmd_ps();
    else if (clen == 3 && (cmd[0]=='m')&&(cmd[1]=='e')&&(cmd[2]=='m')) cmd_mem();
    else if (clen == 5 && (cmd[0]=='c')&&(cmd[1]=='l')&&(cmd[2]=='e')&&(cmd[3]=='a')&&(cmd[4]=='r')) console_clear();
    else if (clen == 3 && (cmd[0]=='v')&&(cmd[1]=='e')&&(cmd[2]=='r')) cmd_ver();
    else if (clen == 3 && (cmd[0]=='p')&&(cmd[1]=='c')&&(cmd[2]=='i')) cmd_pci();
    else if (clen == 2 && (cmd[0]=='l')&&(cmd[1]=='s')) cmd_ls(arg);
    else if (clen == 3 && (cmd[0]=='c')&&(cmd[1]=='a')&&(cmd[2]=='t')) cmd_cat(arg);
    else if (clen == 5 && (cmd[0]=='w')&&(cmd[1]=='r')&&(cmd[2]=='i')&&(cmd[3]=='t')&&(cmd[4]=='e')) cmd_write(arg);
    else if (clen == 2 && (cmd[0]=='r')&&(cmd[1]=='m')) cmd_rm(arg);
    else if (clen == 5 && (cmd[0]=='m')&&(cmd[1]=='o')&&(cmd[2]=='u')&&(cmd[3]=='n')&&(cmd[4]=='t')) cmd_mount();
    else if (clen == 7 && (cmd[0]=='f')&&(cmd[1]=='a')&&(cmd[2]=='t')&&(cmd[3]=='i')&&(cmd[4]=='n')&&(cmd[5]=='f')&&(cmd[6]=='o')) cmd_fatinfo();
    else if (clen == 4 && (cmd[0]=='d')&&(cmd[1]=='i')&&(cmd[2]=='s')&&(cmd[3]=='k')) cmd_disk();
    else if (clen == 6 && (cmd[0]=='r')&&(cmd[1]=='e')&&(cmd[2]=='b')&&(cmd[3]=='o')&&(cmd[4]=='o')&&(cmd[5]=='t')) {
        sh_out("rebooting...\r\n");
        outb(0x64, 0xFE);  // 键盘控制器复位
        while (1) asm volatile("hlt");
    } else if (clen == 4 && (cmd[0]=='h')&&(cmd[1]=='a')&&(cmd[2]=='l')&&(cmd[3]=='t')) {
        sh_out("halting...\r\n");
        while (1) asm volatile("hlt");
    } else {
        sh_out("unknown command (type 'help')\r\n");
    }
}

// ---- 主循环：提示符 + 行编辑（串口 + PS/2 键盘双输入） ----
void shell_main(void) {
    static char line[256];
    kbd_init();
    sh_out(C_CYAN "APinux OS x86_64 kernel shell" C_RESET "\r\n");
    for (;;) {
        sh_out(C_GREEN "apinux$ " C_RESET);
        int n = 0;
        for (;;) {
            while (!serial_rx_ready() && !kbd_ready()) { asm volatile("pause"); }
            char c = serial_rx_ready() ? serial_getc() : kbd_readchar();
            if (c == '\r' || c == '\n') {
                sh_out("\r\n");
                break;
            }
            if (c == '\b' || c == 0x7F) {
                if (n > 0) { n--; sh_out("\b \b"); }
                continue;
            }
            if (c == 3) {  // Ctrl+C
                n = 0;
                sh_out("^C\r\n");
                continue;
            }
            if (c >= 32 && n < 255) {
                line[n++] = c;
                sh_outc(c);
            }
        }
        line[n] = 0;
        if (n > 0)
            shell_exec(line);
    }
}