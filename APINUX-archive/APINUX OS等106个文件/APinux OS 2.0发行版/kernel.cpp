// ================================================================
// kernel.cpp — APinux 微内核实现 (内存/进程/IPC)
// ================================================================
#include "kernel.h"

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
    p->stack_top = (char*)alloc_pages(0) + PAGE_SIZE; // 4KB 栈
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
extern "C" void kernel_main(void* dtb) {
    // 初始化内存池 (假设由Bootloader传递)
    g_kernel_pool.init((void*)0x80000000, KERNEL_HEAP_MB * 1024 * 1024);
    g_page_alloc.init((void*)0x90000000, 256 * 1024 * 1024); // 256MB用户内存

    // 创建第一个用户进程 (init)
    g_proc_mgr.create_process("init", []() {
        // 用户态入口 (后续文件实现)
        while (1) asm volatile("wfi");
    });

    // 调度循环
    while (1) {
        g_proc_mgr.schedule();
    }
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
            asm volatile("wfi"); // 简化退出
        default: return -1;
    }
}