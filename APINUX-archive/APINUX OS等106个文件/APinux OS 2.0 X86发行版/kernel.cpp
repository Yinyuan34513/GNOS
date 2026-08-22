// ================================================================
// kernel.cpp — APinux 微内核实现 (内存/进程/IPC)
// ================================================================
#include "kernel.h"
#include "drivers.h"
#include "console.h"
#include "shell.h"

// 构建修复：串口输出（userlib.cpp 提供，x86_64 走 COM1）
extern void serial_write(const char* s);

// 构建修复：全局对象前向声明（原文件把定义放在成员函数实现之后，
// create_process 在 g_kernel_pool 声明前就引用它，编译报未声明）
extern FlowMemPool g_kernel_pool;
extern PageAllocator g_page_alloc;

// ----- FlowMemPool -----
void FlowMemPool::init(void* base, size_t size) {
    pool_base = (char*)base;
    pool_size = size;
    head = (Block*)base;
    head->size = size - sizeof(Block);
    head->free = true;
    head->next = nullptr;
}

void* FlowMemPool::alloc(size_t bytes) {
    bytes = (bytes + 63) & ~63; // 64字节对齐
    Block* prev = nullptr;
    Block* curr = head;
    while (curr) {
        if (curr->free && curr->size >= bytes) {
            // 分割大块
            if (curr->size > bytes + sizeof(Block) + 64) {
                Block* new_block = (Block*)((char*)curr + sizeof(Block) + bytes);
                new_block->size = curr->size - bytes - sizeof(Block);
                new_block->free = true;
                new_block->next = curr->next;
                curr->size = bytes;
                curr->next = new_block;
            }
            curr->free = false;
            return (char*)curr + sizeof(Block);
        }
        prev = curr;
        curr = curr->next;
    }
    return nullptr; // OOM
}

void FlowMemPool::free(void* ptr) {
    if (!ptr) return;
    Block* blk = (Block*)((char*)ptr - sizeof(Block));
    blk->free = true;
    // 合并相邻空闲块 (简化)
}

// ----- PageAllocator -----
void PageAllocator::init(void* mem_base, size_t mem_size) {
    size_t num_pages = mem_size / PAGE_SIZE;
    // 将所有页初始化为最大阶
    for (int i = 0; i <= MAX_ORDER; ++i) free_lists[i] = nullptr;
    void* ptr = mem_base;
    while (num_pages > 0) {
        int order = 0;
        while ((1ULL << order) <= num_pages) ++order;
        --order;
        free_pages(ptr, order);
        num_pages -= (1 << order);
        ptr = (char*)ptr + (1 << order) * PAGE_SIZE;
    }
}

void* PageAllocator::alloc_pages(size_t order) {
    for (int i = order; i <= MAX_ORDER; ++i) {
        if (free_lists[i]) {
            void* pages = free_lists[i]->pages;
            free_lists[i] = free_lists[i]->next;
            // 若需要，分割大块
            while (i > (int)order) {
                --i;
                void* buddy = (char*)pages + (1 << i) * PAGE_SIZE;
                FreeList* fl = (FreeList*)buddy;
                fl->pages = buddy;
                fl->next = free_lists[i];
                free_lists[i] = fl;
            }
            return pages;
        }
    }
    return nullptr;
}

void PageAllocator::free_pages(void* ptr, size_t order) {
    FreeList* fl = (FreeList*)ptr;
    fl->pages = ptr;
    fl->next = free_lists[order];
    free_lists[order] = fl;
}

// ----- ProcessManager -----
Process* ProcessManager::create_process(const char* name, void (*entry)()) {
    Process* p = (Process*)g_kernel_pool.alloc(sizeof(Process));
    p->pid = next_pid++;
    strncpy(p->name, name, 31);
    p->state = 0;
    p->priority = 1;
    p->page_table = alloc_page_table();
    p->stack_top = (char*)g_page_alloc.alloc_pages(0) + PAGE_SIZE; // 4KB 栈
    p->next = proc_list;
    proc_list = p;
    return p;
}

void ProcessManager::schedule() {
    if (!proc_list) return;
    Process* next = current ? current->next : proc_list;
    if (!next) next = proc_list;
    if (current) current->state = 0;
    next->state = 1;
    current = next;
    // 上下文切换 (汇编实现)
    switch_context(&current->stack_top);
}

// ----- IPCRouter -----
bool IPCRouter::send(uint32_t dest, uint32_t type, const void* data, size_t size) {
    if ((tail + 1) % MAX_QUEUE == head) return false; // 队列满
    Message& msg = queue[tail];
    msg.sender_pid = 0;
    msg.receiver_pid = dest;
    msg.type = type;
    msg.size = size > 256 ? 256 : size;
    memcpy(msg.data, data, msg.size);
    tail = (tail + 1) % MAX_QUEUE;
    return true;
}

bool IPCRouter::recv(uint32_t sender, Message* out) {
    if (head == tail) return false;
    *out = queue[head];
    head = (head + 1) % MAX_QUEUE;
    return true;
}

// ----- 全局对象 -----
FlowMemPool g_kernel_pool;
PageAllocator g_page_alloc;
ProcessManager g_proc_mgr;
IPCRouter g_ipc;

// ----- 内核入口 -----
// EDI = Multiboot magic，ESI = Multiboot info 指针（boot_x86.S 传入）
extern "C" void kernel_main(uint32_t mb_magic, uint32_t mb_info) {
#if defined(__x86_64__)
    // 控制台初始化（帧缓冲优先，回退 VGA 文本）
    console_init(mb_magic, mb_info);

    // x86_64 (QEMU -m 128M)：堆池放 16MB 起，页区 32MB 起共 64MB
    g_kernel_pool.init((void*)0x1000000, KERNEL_HEAP_MB * 1024 * 1024);
    g_page_alloc.init((void*)0x2000000, 64 * 1024 * 1024);
    serial_write("APinux OS x86_64: kernel booted\r\n");
    console_write("APinux OS x86_64: kernel booted\r\n");
    serial_write("APinux OS x86_64: scheduler running (1 init process)\r\n");
    console_write("APinux OS x86_64: scheduler running (1 init process)\r\n");
#else
    // 初始化内存池 (假设由Bootloader传递)
    g_kernel_pool.init((void*)0x80000000, KERNEL_HEAP_MB * 1024 * 1024);
    g_page_alloc.init((void*)0x90000000, 256 * 1024 * 1024); // 256MB用户内存
#endif

    // 创建第一个用户进程 (init)
    g_proc_mgr.create_process("init", []() {
        // 用户态入口 (后续文件实现)
        while (1) {
#if defined(__aarch64__)
            asm volatile("wfi");
#else
            asm volatile("hlt");   // x86_64: WFI 是非法指令，会 #UD 三连重置
#endif
        }
    });

    // 调度循环（GUI 已移除，纯控制台模式）
    serial_write("APinux OS x86_64: booted (console mode)\r\n");
    console_write("APinux OS x86_64: booted (console mode)\r\n");

    // 内核 shell（阻塞读 COM1，不返回）
    shell_main();
}

// ---------- 系统调用处理 ----------
extern "C" uint64_t syscall(uint64_t num, uint64_t arg1, uint64_t arg2, uint64_t arg3) {
    switch (num) {
        case SYS_YIELD:
            g_proc_mgr.yield();
            return 0;
        case SYS_ALLOC:
            return (uint64_t)g_kernel_pool.alloc(arg1);
        case SYS_FREE:
            g_kernel_pool.free((void*)arg1);
            return 0;
        case SYS_IPC_SEND:
            return g_ipc.send(arg1, arg2, (void*)arg3, arg3 >> 32) ? 1 : 0;
        case SYS_IPC_RECV:
            return g_ipc.recv(arg1, (Message*)arg2) ? 1 : 0;
        case SYS_EXIT:
#if defined(__aarch64__)
            asm volatile("wfi"); // 简化退出
#else
            asm volatile("hlt"); // 简化退出
#endif
        default: return -1;
    }
}

// ================================================================
// 构建修复补充：原仓库缺失的符号实现
//   alloc_page_table() / switch_context() 被 kernel.cpp 调用但从未定义；
//   ProcessManager::yield() 在 kernel.h 声明但从未定义。
//   单进程演示：页表给固定虚拟地址（不真正用于 CR3 切换），
//   上下文切换为空操作（只有一个 init 进程）。
// ================================================================
void* alloc_page_table() {
    return (void*)0x200000;
}

void switch_context(void** old_sp) {
    (void)old_sp;   // 单进程：无事可切
}

void ProcessManager::yield() {
    schedule();
}

// 构建修复：裸机内核 -nostdlib 无 libc，提供被 kernel/userlib 引用的
// memcpy/strncpy 最小实现（声明来自 <cstring>，extern "C" 匹配）。
extern "C" void* memcpy(void* dst, const void* src, size_t n) {
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    while (n--) *d++ = *s++;
    return dst;
}

extern "C" char* strncpy(char* dst, const char* src, size_t n) {
    size_t i = 0;
    while (i < n && src[i]) { dst[i] = src[i]; ++i; }
    while (i < n) dst[i++] = '\0';
    return dst;
}

// 构建修复：GUI/STL 运行时支撑（-nostdlib 下无 libstdc++/libsupc++）。
//   std::vector（AppWindow::widgets）扩容需要 memmove 与 operator new/delete；
//   纯虚函数 vtable 槽需要 __cxa_pure_virtual；-fno-exceptions 下
//   vector 的越界路径引用 __throw_length_error / __throw_bad_alloc。
extern "C" void* memmove(void* dst, const void* src, size_t n) {
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        d += n;
        s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}

void* operator new(size_t size) { return g_kernel_pool.alloc(size); }
void* operator new[](size_t size) { return g_kernel_pool.alloc(size); }
void operator delete(void* p) noexcept { g_kernel_pool.free(p); }
void operator delete(void* p, size_t) noexcept { g_kernel_pool.free(p); }
void operator delete[](void* p) noexcept { g_kernel_pool.free(p); }
void operator delete[](void* p, size_t) noexcept { g_kernel_pool.free(p); }

extern "C" void __cxa_pure_virtual() { for (;;) asm volatile("hlt"); }

namespace std {
__attribute__((noreturn)) void __throw_length_error(const char*) { for (;;) asm volatile("hlt"); }
__attribute__((noreturn)) void __throw_bad_alloc() { for (;;) asm volatile("hlt"); }
}