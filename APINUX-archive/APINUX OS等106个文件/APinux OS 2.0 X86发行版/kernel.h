// ================================================================
// kernel.h — APinux OS 微内核 (内存管理, 进程调度, IPC)
// 架构：ARMv8-A / x86_64 双模式
// 无 Linux 依赖，独立构建
// ================================================================
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <new>

// ---------- 硬件抽象 ----------
#ifdef __aarch64__
    #define APINUX_ARCH_ARM 1
    #define APINUX_CACHE_LINE 64
#elif defined(__x86_64__)
    #define APINUX_ARCH_x86 1
    #define APINUX_CACHE_LINE 64
#else
    #error "Unsupported architecture"
#endif

// ---------- 内存管理 ----------
constexpr size_t PAGE_SIZE = 4096;
constexpr size_t KERNEL_HEAP_MB = 512;  // 512MB 内核堆

// 流动内存池（与 OPEN PUFF 兼容）
class FlowMemPool {
    struct Block { Block* next; size_t size; bool free; };
    Block* head = nullptr;
    char* pool_base;
    size_t pool_size;
public:
    size_t get_pool_size() const { return pool_size; }
    void init(void* base, size_t size);
    void* alloc(size_t bytes);
    void free(void* ptr);
    size_t available() const;
};

// 物理页分配器
class PageAllocator {
    static constexpr size_t MAX_ORDER = 10; // 4KB - 4MB
    struct FreeList { void* pages; FreeList* next; };
    FreeList* free_lists[MAX_ORDER + 1];
public:
    void init(void* mem_base, size_t mem_size);
    void* alloc_pages(size_t order);       // 2^order 页
    void free_pages(void* ptr, size_t order);
};

// ---------- 进程管理 ----------
struct Process {
    uint32_t pid;
    uint32_t parent_pid;
    char name[32];
    void* page_table;
    void* stack_top;
    uint32_t state;         // 0=ready, 1=running, 2=blocked
    uint32_t priority;
    Process* next;
};

class ProcessManager {
    Process* proc_list = nullptr;
    uint32_t next_pid = 1;
    Process* current = nullptr;
public:
    Process* create_process(const char* name, void (*entry)());
    void schedule();           // 简单轮转调度
    void yield();
    Process* get_current() { return current; }
    Process* first_process() { return proc_list; }
};

// ---------- 进程间通信 (IPC) ----------
struct Message {
    uint32_t sender_pid;
    uint32_t receiver_pid;
    uint32_t type;
    uint32_t size;
    char data[256];
};

// 构建修复：全局进程管理器（kernel.cpp 定义）
extern ProcessManager g_proc_mgr;

class IPCRouter {
    static constexpr size_t MAX_QUEUE = 128;
    Message queue[MAX_QUEUE];
    size_t head = 0, tail = 0;
public:
    bool send(uint32_t dest, uint32_t type, const void* data, size_t size);
    bool recv(uint32_t sender, Message* out);
};

// ---------- 系统调用接口 ----------
enum Syscall : uint32_t {
    SYS_YIELD = 0,
    SYS_ALLOC = 1,
    SYS_FREE  = 2,
    SYS_IPC_SEND = 3,
    SYS_IPC_RECV = 4,
    SYS_EXIT  = 5,
};

extern "C" uint64_t syscall(uint64_t num, uint64_t arg1, uint64_t arg2, uint64_t arg3);

// ---------- 内核入口 ----------
extern "C" void kernel_main(uint32_t mb_magic, uint32_t mb_info);  // 由启动汇编调用

// ---------- 构建修复补充的声明 ----------
void* alloc_page_table();           // 为进程分配页表（单进程演示返回固定地址）
void switch_context(void** old_sp); // 协程式上下文切换（单进程为空操作）