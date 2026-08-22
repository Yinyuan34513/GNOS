// ================================================================
// ckr.cpp — APinux OS C++ 命令行工具 (CKR Shell)
// 功能：文件管理、进程查看、AI 任务提交、系统配置
// ================================================================
#include "kernel.h"
#include "vfs.h"
#include "drivers.h"
#include "ai_bridge.h"
#include <cstdio>
#include <cstring>

extern VirtualFileSystem g_vfs;
extern DeviceManager g_devmgr;
extern AIPlatform g_ai;

// ---------- 命令实现 ----------
int cmd_ls(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : "/";
    VNode* dir = g_vfs.lookup(path);
    if (!dir || dir->type != 1) {
        printf("ls: not a directory\n");
        return 1;
    }
    VNode* child = dir->children;
    while (child) {
        printf("%s  ", child->name);
        child = child->next_sibling;
    }
    printf("\n");
    return 0;
}

int cmd_cat(int argc, char** argv) {
    if (argc < 2) { printf("usage: cat <file>\n"); return 1; }
    VNode* file = g_vfs.lookup(argv[1]);
    if (!file) { printf("cat: %s not found\n", argv[1]); return 1; }
    char buf[256];
    if (g_vfs.read_file(file, buf, 0, sizeof(buf))) {
        printf("%s\n", buf);
    }
    return 0;
}

int cmd_ps(int argc, char** argv) {
    extern ProcessManager g_proc_mgr;
    printf("PID\tNAME\n");
    Process* p = g_proc_mgr.get_current();
    // 遍历进程列表 (略)
    printf("1\tinit\n");
    printf("2\tckr\n");
    return 0;
}

int cmd_ai_infer(int argc, char** argv) {
    if (argc < 3) { printf("usage: ai infer <model.bin> <image.ppm>\n"); return 1; }
    void* model = g_ai.load_model(AIPlatform::ENGINE_ACPNN, argv[1]);
    if (!model) { printf("failed to load model\n"); return 1; }
    // 加载图像并推理 (简化)
    printf("AI inference completed.\n");
    return 0;
}

int cmd_ai_train(int argc, char** argv) {
    if (argc < 4) { printf("usage: ai train <model.bin> <data.txt> <epochs>\n"); return 1; }
    printf("Training started...\n");
    return 0;
}

int cmd_ai_connect(int argc, char** argv) {
    if (argc < 3) { printf("usage: ai connect <ip> <task>\n"); return 1; }
    // 多机互联调度
    ClusterScheduler cluster;
    Device* eth = g_devmgr.find_by_type(4); // 以太网
    if (!eth) { printf("no ethernet device\n"); return 1; }
    EthernetDriver eth_drv;
    eth_drv.init((uint32_t)(uintptr_t)eth->mmio_base);
    cluster.attach(&eth_drv);
    cluster.broadcast_task(0, argv[2], strlen(argv[2]));
    printf("Task broadcasted.\n");
    return 0;
}

// ---------- 命令表 ----------
struct Command {
    const char* name;
    int (*func)(int, char**);
    const char* help;
};

static Command cmds[] = {
    {"ls",     cmd_ls,         "list files"},
    {"cat",    cmd_cat,        "print file content"},
    {"ps",     cmd_ps,         "list processes"},
    {"ai",     nullptr,        "ai [infer|train|connect]"},
    {"help",   nullptr,        "show help"},
};

// ---------- 主入口 ----------
int ckr_main() {
    printf("CKR Shell v0.1 (APinux OS)\n");
    char line[256];
    while (1) {
        printf("ckr> ");
        fgets(line, sizeof(line), stdin);
        // 解析命令
        if (strncmp(line, "exit", 4) == 0) break;
        if (strncmp(line, "ai", 2) == 0) {
            char sub[16], arg1[128], arg2[128];
            if (sscanf(line, "ai %s %s %s", sub, arg1, arg2) >= 2) {
                if (strcmp(sub, "infer") == 0) {
                    char* argv[] = { (char*)"ai", arg1, arg2 };
                    cmd_ai_infer(3, argv);
                } else if (strcmp(sub, "train") == 0) {
                    char* argv[] = { (char*)"ai", arg1, arg2 };
                    cmd_ai_train(3, argv);
                } else if (strcmp(sub, "connect") == 0) {
                    char* argv[] = { (char*)"ai", arg1, arg2 };
                    cmd_ai_connect(3, argv);
                }
            }
        } else if (strncmp(line, "ls", 2) == 0) {
            char* argv[] = { (char*)"ls", line + 3 };
            cmd_ls(2, argv);
        } else if (strncmp(line, "cat", 3) == 0) {
            char* argv[] = { (char*)"cat", line + 4 };
            cmd_cat(2, argv);
        } else if (strncmp(line, "ps", 2) == 0) {
            cmd_ps(0, nullptr);
        } else {
            printf("unknown command\n");
        }
    }
    return 0;
}