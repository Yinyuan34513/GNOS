/*
 * vmm.c — per-process virtual address spaces. (GPLv2)
 */
#include <stddef.h>
#include <stdint.h>

#include "vmm.h"
#include "pmm.h"
#include "panic.h"
#include "kstring.h"
#include "debugcon.h"
#include "sysnum.h"              /* PROT_READ/WRITE/EXEC for vmm_protect */

#define PTE_P    0x001
#define PTE_RW   0x002
#define PTE_U    0x004
#define PTE_PWT  0x008   /* write-through */
#define PTE_PCD  0x010   /* cache-disable (set => uncacheable) */
#define PTE_PS   0x080
#define PTE_NX   (1ULL << 63)
#define PTE_ADDR 0x000FFFFFFFFFF000ULL

/* One static address space object per possible process; the kernel has no
 * heap, and a fixed table keeps allocation honest. */
#define MAX_ADDRSPACES 32
static addrspace_t g_spaces[MAX_ADDRSPACES];
static int         g_used[MAX_ADDRSPACES];

static uint64_t g_kernel_pml4_phys;

static uint64_t *table(uint64_t phys)
{
    return (uint64_t *)pmm_virt(phys);
}

void vmm_init(void)
{
    uint64_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    g_kernel_pml4_phys = cr3 & PTE_ADDR;

    /* Enable the No-Execute bit (EFER.NXE).  Without it the CPU treats PTE
     * bit 63 as a reserved bit, so every non-executable mapping -- the kernel
     * BSS, every user stack and anonymous mmap -- faults the moment it is
     * touched.  This must happen before any such mapping is created. */
    uint32_t e_lo, e_hi;
    asm volatile("rdmsr" : "=a"(e_lo), "=d"(e_hi) : "c"(0xC0000080));
    e_lo |= (1u << 11);                    /* EFER.NXE */
    asm volatile("wrmsr" : : "c"(0xC0000080), "a"(e_lo), "d"(e_hi));

    /* Enable FPU/SSE state management.  CR4.OSFXSR lets user programs execute
     * SSE/SSE2/SSE3 instructions (without it every one of them raises #UD,
     * vector 6 -- this is exactly what was killing musl's libc) and lets the
     * kernel use FXSAVE/FXRSTOR to preserve each process's XMM registers
     * across a context switch.  OSXSAVE (guarded by CPUID) is harmless here
     * and leaves the door open for AVX later; writing it on a CPU that lacks
     * it is simply ignored, so we only set it when supported. */
    uint64_t cr4;
    asm volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1ULL << 9);                    /* CR4.OSFXSR */
    {
        uint32_t a, b, c, d;
        asm volatile("cpuid"
                     : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                     : "a"(1));
        if (c & (1u << 27))               /* CPUID.01H:ECX.OSXSAVE */
            cr4 |= (1ULL << 18);          /* CR4.OSXSAVE */
    }
    asm volatile("mov %0, %%cr4" :: "r"(cr4));

    dbg_puts("VMM: kernel PML4 @");
    dbg_puts_hex(g_kernel_pml4_phys);
    dbg_puts("\r\n");
}

/*
 * Some bootloaders map only the file-backed portion of the kernel image and
 * leave the zero-initialised BSS unmapped.  The first time the kernel writes
 * a global in that region it therefore takes a page fault.  Rather than trust
 * the loader, map whatever BSS pages are not already present ourselves, reusing
 * the live kernel PML4 (g_kernel_pml4_phys) so the change is visible at once.
 *
 * The BSS bounds come straight from the linker symbols, whose addresses are
 * already relocated to their runtime (higher-half) values by the loader, so no
 * load-bias arithmetic is needed here.
 */
static uint64_t *walk(addrspace_t *as, uint64_t vaddr, int create, unsigned flags);
void vmm_map_kernel_bss(void)
{
    extern char _bss_start[];
    extern char _kernel_end[];

    uint64_t start = (uint64_t)(uintptr_t)_bss_start;
    uint64_t end   = (uint64_t)(uintptr_t)_kernel_end;

    dbg_puts("VMM: bss map range [");
    dbg_puts_hex(start);
    dbg_puts(",");
    dbg_puts_hex(end);
    dbg_puts(")\r\n");

    /* Same physical PML4 the running kernel uses; mapping through it means the
     * new PTEs are live immediately. */
    addrspace_t k = { .pml4_phys = g_kernel_pml4_phys };
    uint64_t base = start & ~0xFFFULL;
    for (uint64_t va = base; va < end; va += PAGE_SIZE) {
        uint64_t *pte = walk(&k, va, 0, 0);   /* no allocate, just inspect */
        int present = pte && (*pte & PTE_P);
        int writable = present && (*pte & PTE_RW);

        if (present && writable)
            continue;                       /* loader already gave us a good page */

        if (present) {
            /* Loader mapped it read-only; just flip the write bit. */
            *pte |= PTE_RW;
        } else {
            uint64_t frame = pmm_alloc_zeroed();
            if (!frame) {
                dbg_puts("VMM: bss OOM at ");
                dbg_puts_hex(va);
                dbg_puts("\r\n");
                panic("vmm: failed to map kernel BSS (OOM)");
            }
            if (!vmm_map(&k, va, frame, VM_WRITE)) {
                dbg_puts("VMM: bss map fail at ");
                dbg_puts_hex(va);
                dbg_puts("\r\n");
                panic("vmm: failed to map kernel BSS (map)");
            }
        }
        asm volatile("invlpg (%0)" :: "r"(va) : "memory");
    }

    dbg_puts("VMM: kernel BSS mapped OK\r\n");
}

addrspace_t *vmm_create(void)
{
    int slot = -1;
    for (int i = 0; i < MAX_ADDRSPACES; i++) {
        if (!g_used[i]) {
            slot = i;
            break;
        }
    }
    if (slot < 0)
        return NULL;

    uint64_t pml4 = pmm_alloc_zeroed();
    if (!pml4)
        return NULL;

    /* Share the kernel half; leave the user half empty. */
    uint64_t *dst = table(pml4);
    uint64_t *src = table(g_kernel_pml4_phys);
    for (int i = 256; i < 512; i++)
        dst[i] = src[i];

    g_used[slot] = 1;
    g_spaces[slot].pml4_phys = pml4;
    g_spaces[slot].refs      = 1;
    return &g_spaces[slot];
}

static uint64_t pte_flags(unsigned flags)
{
    uint64_t f = PTE_P;
    if (flags & VM_WRITE)
        f |= PTE_RW;
    if (flags & VM_USER)
        f |= PTE_U;
    if (!(flags & VM_EXEC))
        f |= PTE_NX;
    return f;
}

/* Walk to the PTE for `vaddr`, allocating tables when `create` is set. */
static uint64_t *walk(addrspace_t *as, uint64_t vaddr, int create,
                      unsigned flags)
{
    uint64_t *cur = table(as->pml4_phys);

    for (int level = 4; level > 1; level--) {
        unsigned idx = (unsigned)((vaddr >> (12 + 9 * (level - 1))) & 0x1FF);

        if (!(cur[idx] & PTE_P)) {
            if (!create)
                return NULL;
            uint64_t next = pmm_alloc_zeroed();
            if (!next)
                return NULL;
            cur[idx] = next | PTE_P | PTE_RW;
        }
        /* Intermediate levels must permit whatever the leaf permits. */
        if (create) {
            cur[idx] |= PTE_RW;
            if (flags & VM_USER)
                cur[idx] |= PTE_U;
            if (flags & VM_EXEC)
                cur[idx] &= ~PTE_NX;
        }
        if (cur[idx] & PTE_PS)
            panic("vmm: large page in a process address space");

        cur = table(cur[idx] & PTE_ADDR);
    }

    return &cur[(vaddr >> 12) & 0x1FF];
}

int vmm_map(addrspace_t *as, uint64_t vaddr, uint64_t paddr, unsigned flags)
{
    uint64_t *pte = walk(as, vaddr & ~0xFFFULL, 1, flags);
    if (!pte)
        return 0;
    *pte = (paddr & PTE_ADDR) | pte_flags(flags);
    return 1;
}

/*
 * mprotect(2) for real: rewrite the permission bits of every present page
 * in [vaddr, vaddr+size).  PROT_NONE leaves the page mapped but strips the
 * read/write/execute bits -- the state musl relies on for a thread stack's
 * guard page, and the state its mmap(PROT_NONE)+mprotect(RW) dance needs
 * reversed for the stack itself.  Pages that are not mapped are skipped
 * (Linux tolerates holes in the range); everything mapped gets invlpg'd.
 */
int vmm_protect(addrspace_t *as, uint64_t vaddr, uint64_t size, unsigned prot)
{
    if (!as || (vaddr & 0xFFF) || (size & 0xFFF))
        return 0;

    unsigned flags = VM_USER;
    if (prot & PROT_WRITE)
        flags |= VM_WRITE;
    if (prot & PROT_EXEC)
        flags |= VM_EXEC;
    if (prot & PROT_READ)
        flags |= VM_READ;
    uint64_t bits = pte_flags(flags);

    dbg_puts("VMM: protect [");
    dbg_puts_hex(vaddr);
    dbg_puts(",");
    dbg_puts_hex(vaddr + size);
    dbg_puts(") prot=");
    dbg_puts_dec(prot);
    dbg_puts("\r\n");

    for (uint64_t va = vaddr; va < vaddr + size; va += PAGE_SIZE) {
        uint64_t *pte = walk(as, va, 0, 0);
        if (!pte || !(*pte & PTE_P))
            continue;                   /* hole: nothing to protect */
        if (*pte & PTE_PS)
            panic("vmm_protect: large page in a process address space");
        *pte = (*pte & ~(PTE_RW | PTE_NX)) | (bits & (PTE_RW | PTE_NX));
        asm volatile("invlpg (%0)" : : "r"(va) : "memory");
    }
    return 1;
}

uint64_t vmm_resolve(addrspace_t *as, uint64_t vaddr)
{
    uint64_t *pte = walk(as, vaddr & ~0xFFFULL, 0, 0);
    if (!pte || !(*pte & PTE_P))
        return 0;
    return (*pte & PTE_ADDR) + (vaddr & 0xFFF);
}

/* ---- kernel-address-space helpers (used by the module loader) ---------- */
/* The kernel runs on a page table that is not one of the proc address
 * spaces, so all three wrap the walker with a space anchored at the
 * kernel PML4.  vmm_map_kernel is what maps module pages into the kernel
 * image's own higher-half. */

int vmm_map_kernel(uint64_t vaddr, uint64_t paddr, unsigned flags)
{
    addrspace_t k = { .pml4_phys = g_kernel_pml4_phys };
    return vmm_map(&k, vaddr, paddr, flags);
}

int vmm_unmap_kernel(uint64_t vaddr, uint64_t size)
{
    addrspace_t k = { .pml4_phys = g_kernel_pml4_phys };
    return vmm_unmap(&k, vaddr, size);
}

int vmm_kernel_present(uint64_t vaddr)
{
    addrspace_t k = { .pml4_phys = g_kernel_pml4_phys };
    return vmm_resolve(&k, vaddr) != 0;
}

/* Page-fault forensics: print the four page-table entries for `va` in the
 * *current* address space, so a protection fault can be told apart from a
 * missing page without a debugger. */
void vmm_pte_dump(uint64_t va)
{
    uint64_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));

    int i4 = (int)((va >> 39) & 0x1FF);
    int i3 = (int)((va >> 30) & 0x1FF);
    int i2 = (int)((va >> 21) & 0x1FF);
    int i1 = (int)((va >> 12) & 0x1FF);

    dbg_puts("VMM: va=");
    dbg_puts_hex(va);
    uint64_t *p4 = table(cr3 & PTE_ADDR);
    dbg_puts(" p4=");
    dbg_puts_hexn(p4[i4], 16);
    if (!(p4[i4] & PTE_P))
        goto out;
    uint64_t *p3 = table(p4[i4] & PTE_ADDR);
    dbg_puts(" p3=");
    dbg_puts_hexn(p3[i3], 16);
    if (!(p3[i3] & PTE_P))
        goto out;
    uint64_t *p2 = table(p3[i3] & PTE_ADDR);
    dbg_puts(" p2=");
    dbg_puts_hexn(p2[i2], 16);
    if (!(p2[i2] & PTE_P))
        goto out;
    uint64_t *p1 = table(p2[i2] & PTE_ADDR);
    dbg_puts(" pte=");
    dbg_puts_hexn(p1[i1], 16);
out:
    dbg_puts("\r\n");
}

/*
 * Extend the stack by the one page that was touched.
 *
 * Deliberately not clever: any unmapped address inside the reserved window
 * grows the stack, with no check that it is anywhere near the current RSP.
 * Linux needs that check because a user mapping can legitimately sit just
 * below the stack; here the whole window is reserved for the stack and
 * nothing else is ever placed in it, so there is nothing to protect
 * against.  Everything below the window stays unmapped forever and is the
 * guard: a runaway recursion walks off the end and takes SIGSEGV.
 */
int vmm_grow_stack(addrspace_t *as, uint64_t addr)
{
    if (!as)
        return 0;

    if (addr >= USER_STACK_TOP || addr < USER_STACK_TOP - USER_STACK_MAX)
        return 0;

    uint64_t page = addr & ~0xFFFULL;
    if (vmm_resolve(as, page))
        return 0;               /* already mapped: a protection fault, not growth */

    if (!vmm_alloc_range(as, page, PAGE_SIZE, VM_USER | VM_WRITE))
        return 0;

    /* The fault loaded a not-present entry into the TLB; drop it so the
     * retried instruction sees the mapping we just made. */
    asm volatile("invlpg (%0)" :: "r"(page) : "memory");
    return 1;
}

/*
 * Map a physical MMIO region into the kernel's own address space with the
 * cache-disable (uncacheable) attribute device registers need, and return
 * its virtual base.  Device registers must not be cached or writes can be
 * coalesced/lost and reads can return stale values -- the HHDM direct map is
 * write-back, so it is wrong for this.  We carve a private virtual arena
 * above the HHDM and page the region in one page at a time.
 */
#define MMIO_BASE  0xFFFFA00000000000ULL
uint64_t vmm_map_mmio(uint64_t phys, uint64_t size)
{
    static uint64_t mmio_next = MMIO_BASE;

    /* A bad BAR-sizing calculation produces an enormous "size", and mapping
     * it would eat every free frame on page tables before failing.  No device
     * we drive needs more than a few hundred KiB of registers. */
    if (size == 0 || size > (16ULL << 20))
        return 0;

    uint64_t base = mmio_next;
    mmio_next += (size + 0xFFF) & ~0xFFFULL;

    addrspace_t k = { .pml4_phys = g_kernel_pml4_phys };
    for (uint64_t off = 0; off < size; off += PAGE_SIZE) {
        uint64_t *pte = walk(&k, (base + off) & ~0xFFFULL, 1, 0);
        if (!pte)
            return 0;
        *pte = ((phys + off) & PTE_ADDR) | PTE_P | PTE_RW | PTE_PCD |
               PTE_PWT | PTE_NX;
        asm volatile("invlpg (%0)" :: "r"(base + off) : "memory");
    }
    return base;
}

int vmm_unmap(addrspace_t *as, uint64_t vaddr, uint64_t size)
{
    uint64_t start = vaddr & ~0xFFFULL;
    uint64_t end   = (vaddr + size + 0xFFF) & ~0xFFFULL;

    for (uint64_t va = start; va < end; va += PAGE_SIZE) {
        uint64_t *pte = walk(as, va, 0, 0);
        if (!pte || !(*pte & PTE_P))
            continue;                   /* nothing mapped here */
        pmm_free(*pte & PTE_ADDR);
        *pte = 0;
    }
    /* Drop stale TLB entries for the range, but only if this is the address
     * space we are actually running on; reloading CR3 for another process
     * would switch the page tables out from under the caller. */
    uint64_t cur;
    asm volatile("mov %%cr3, %0" : "=r"(cur));
    if ((cur & PTE_ADDR) == as->pml4_phys) {
        for (uint64_t va = start; va < end; va += PAGE_SIZE)
            asm volatile("invlpg (%0)" :: "r"(va) : "memory");
    }
    return 1;
}

int vmm_alloc_range(addrspace_t *as, uint64_t vaddr, uint64_t size,
                    unsigned flags)
{
    uint64_t start = vaddr & ~0xFFFULL;
    uint64_t end   = (vaddr + size + 0xFFF) & ~0xFFFULL;

    for (uint64_t va = start; va < end; va += PAGE_SIZE) {
        if (vmm_resolve(as, va))
            continue;                       /* already backed */
        uint64_t frame = pmm_alloc_zeroed();
        if (!frame)
            return 0;
        if (!vmm_map(as, va, frame, flags)) {
            pmm_free(frame);
            return 0;
        }
    }
    return 1;
}

int vmm_copy_to_user(addrspace_t *as, uint64_t dst, const void *src, uint64_t n)
{
    const uint8_t *s = (const uint8_t *)src;

    while (n) {
        uint64_t phys = vmm_resolve(as, dst);
        if (!phys)
            return 0;

        uint64_t chunk = PAGE_SIZE - (dst & 0xFFF);
        if (chunk > n)
            chunk = n;

        memcpy(pmm_virt(phys), s, chunk);
        dst += chunk;
        s   += chunk;
        n   -= chunk;
    }
    return 1;
}

int user_ptr_ok(uint64_t p, uint64_t len)
{
    if (p == 0)
        return 0;
    if (p >= USER_LIMIT || len > USER_LIMIT)
        return 0;
    return p + len <= USER_LIMIT;
}

/* Recursively release the lower half of a page-table tree. */
static void free_level(uint64_t table_phys, int level)
{
    uint64_t *t = table(table_phys);
    int limit = (level == 4) ? 256 : 512;      /* upper half is shared */

    for (int i = 0; i < limit; i++) {
        if (!(t[i] & PTE_P))
            continue;
        uint64_t child = t[i] & PTE_ADDR;
        if (level > 1)
            free_level(child, level - 1);
        else
            pmm_free(child);
        t[i] = 0;
    }
    if (level < 4)
        pmm_free(table_phys);
}

void vmm_destroy(addrspace_t *as)
{
    if (!as)
        return;

    free_level(as->pml4_phys, 4);
    pmm_free(as->pml4_phys);

    for (int i = 0; i < MAX_ADDRSPACES; i++) {
        if (&g_spaces[i] == as) {
            g_used[i] = 0;
            break;
        }
    }
    as->pml4_phys = 0;
    as->refs      = 0;
}

addrspace_t *vmm_share(addrspace_t *as)
{
    if (as)
        as->refs++;
    return as;
}

void vmm_put(addrspace_t *as)
{
    if (!as || as->refs <= 0 || --as->refs > 0)
        return;
    /* Last reference: the caller must not still be running on it. */
    vmm_destroy(as);
}

/* Walk `src`'s lower half and duplicate every mapped page into `dst`.  This
 * is an eager copy: no copy-on-write, because fork() here is almost always
 * followed immediately by execve() and the extra machinery would not pay for
 * itself yet. */
static int clone_level(addrspace_t *dst, uint64_t src_table, int level,
                       uint64_t vbase)
{
    uint64_t *t = table(src_table);
    int limit = (level == 4) ? 256 : 512;
    unsigned shift = (unsigned)(12 + 9 * (level - 1));

    for (int i = 0; i < limit; i++) {
        if (!(t[i] & PTE_P))
            continue;

        uint64_t va = vbase | ((uint64_t)i << shift);

        if (level > 1) {
            if (!clone_level(dst, t[i] & PTE_ADDR, level - 1, va))
                return 0;
            continue;
        }

        unsigned flags = VM_USER | VM_EXEC;
        if (t[i] & PTE_RW)
            flags |= VM_WRITE;

        uint64_t frame = pmm_alloc();
        if (!frame)
            return 0;
        memcpy(pmm_virt(frame), pmm_virt(t[i] & PTE_ADDR), PAGE_SIZE);
        if (!vmm_map(dst, va, frame, flags)) {
            pmm_free(frame);
            return 0;
        }
    }
    return 1;
}

addrspace_t *vmm_clone(addrspace_t *src)
{
    addrspace_t *dst = vmm_create();
    if (!dst)
        return NULL;

    if (!clone_level(dst, src->pml4_phys, 4, 0)) {
        vmm_destroy(dst);
        return NULL;
    }
    return dst;
}

void vmm_switch(addrspace_t *as)
{
    uint64_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    if ((cr3 & PTE_ADDR) == as->pml4_phys)
        return;                     /* already there: skip the TLB flush */
    asm volatile("mov %0, %%cr3" :: "r"(as->pml4_phys) : "memory");
}

void vmm_switch_kernel(void)
{
    asm volatile("mov %0, %%cr3" :: "r"(g_kernel_pml4_phys) : "memory");
}
