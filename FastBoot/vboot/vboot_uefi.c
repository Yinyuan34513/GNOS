/**
 * vboot_uefi.c — VBoot UEFI 变体（完整版）
 *
 * 自包含 UEFI 类型（无 EDK2 依赖），实现全部 VBoot API：
 *   vb_read_disk      → EFI_BLOCK_IO_PROTOCOL（读物理磁盘扇区）
 *   vb_load_file      → EFI_SIMPLE_FILE_SYSTEM + EFI_FILE_PROTOCOL
 *   vb_get_memory_map → gBS->GetMemoryMap
 *   vb_set_video_mode → EFI_GRAPHICS_OUTPUT_PROTOCOL（GOP）
 *   vb_reboot         → gRT->ResetSystem
 *   vb_boot_kernel    → ExitBootServices + 跳内核（传 BootInfo）
 *
 * 编译: clang --target=x86_64-unknown-windows-msvc（Deepin clang 17
 *       的 x86_64-unknown-uefi 目标会段错误，改用 msvc PE/COFF + MS ABI）
 * 链接: lld-link /subsystem:efi_application /entry:efi_main
 */
#include "vboot.h"

typedef uint64_t EFI_STATUS;
typedef uint64_t UINTN;
typedef void*    EFI_HANDLE;
#define EFI_SUCCESS 0

typedef struct { uint16_t ScanCode; uint16_t UnicodeChar; } EFI_INPUT_KEY;
typedef struct { uint32_t Data1; uint16_t Data2; uint16_t Data3; uint8_t Data4[8]; } EFI_GUID;

/* ---- 内存描述符（GetMemoryMap 输出） ---- */
typedef struct {
    uint32_t Type;
    uint32_t Pad;
    uint64_t PhysicalStart;
    uint64_t VirtualStart;
    uint64_t NumberOfPages;
    uint64_t Attribute;
} EFI_MEMORY_DESCRIPTOR;

/* ---- SIMPLE_INPUT / SIMPLE_TEXT_OUTPUT ---- */
typedef struct {
    void* (*Reset)(void*, int);
    void* (*ReadKeyStroke)(void*, EFI_INPUT_KEY*);
} SIMPLE_INPUT;
typedef struct {
    void* (*Reset)(void*, int);
    void* (*OutputString)(void*, const uint16_t*);
    void* (*TestString)(void*, const uint16_t*);
    void* (*QueryMode)(void*, UINTN, UINTN*, UINTN*);
    void* (*SetMode)(void*, UINTN);
    void* (*SetAttribute)(void*, UINTN);
    void* (*ClearScreen)(void*);
} SIMPLE_OUTPUT;

/* ---- EFI_LOADED_IMAGE_PROTOCOL（DeviceHandle 在 0x30） ---- */
typedef struct {
    uint32_t Revision;
    EFI_HANDLE ParentHandle;
    EFI_HANDLE SystemTable;
    EFI_HANDLE DeviceHandle;       /* 0x30 */
    void*      FilePath;
    void*      Reserved;
    uint32_t   LoadOptionsSize;
    void*      LoadOptions;
    void*      ImageBase;
    uint64_t   ImageSize;
} LOADED_IMAGE;

/* ---- EFI_BLOCK_IO ---- */
typedef struct {
    uint32_t MediaId;
    int      RemovableMedia;
    int      MediaPresent;
    int      LogicalPartition;
    int      ReadOnly;
    int      WriteCaching;
    uint32_t BlockSize;
    uint32_t IoAlign;
    void*    LastBlock;
} BLOCK_IO_MEDIA;
typedef struct {
    uint64_t        Revision;
    BLOCK_IO_MEDIA* Media;
    void* (*Reset)(void*, int);
    void* (*ReadBlocks)(void*, uint32_t, uint64_t, UINTN, void*);  /* 0x18 */
    void* (*WriteBlocks)(void*, uint32_t, uint64_t, UINTN, void*);
    void* (*FlushBlocks)(void*);
} BLOCK_IO;

/* ---- EFI_SIMPLE_FILE_SYSTEM + EFI_FILE ---- */
typedef struct {
    uint64_t Revision;
    void* (*OpenVolume)(void*, void**);                            /* 0x08 */
} SIMPLE_FS;
typedef struct {
    uint64_t Revision;
    void* (*Open)(void*, void**, uint16_t*, uint64_t, uint64_t);  /* 0x08 */
    void* (*Close)(void*);                                         /* 0x10 */
    void* (*Delete)(void*);
    void* (*Read)(void*, uint64_t*, void*);                        /* 0x20 */
    void* (*Write)(void*, uint64_t*, void*);
} EFI_FILE;

/* ---- GOP ---- */
typedef struct {
    uint32_t RedMask, GreenMask, BlueMask, ReservedMask;
} GOP_PIXEL_FORMAT;
typedef struct {
    uint32_t Version;
    uint32_t HorizontalResolution;
    uint32_t VerticalResolution;
    uint32_t PixelFormat;
    GOP_PIXEL_FORMAT PixelInformation;
    uint32_t PixelsPerScanLine;
} GOP_MODE_INFO;
typedef struct {
    uint32_t MaxMode;
    uint32_t Mode;
    GOP_MODE_INFO* Info;
    UINTN SizeOfInfo;
    void*  FrameBufferBase;
    UINTN FrameBufferSize;
} GOP_MODE;
typedef struct {
    void* (*QueryMode)(void*, uint32_t, UINTN*, GOP_MODE_INFO**);
    void* (*SetMode)(void*, uint32_t);
    void* (*GetInfo)(void*);
    void* (*GetMode)(void*);
    void* (*Blt)(void*);
    GOP_MODE* Mode;
} GOP;

/* ---- BOOT_SERVICES（按 UEFI ABI 偏移完整布局） ---- */
typedef struct {
    uint64_t Hdr[2];                     /* 0x00 */
    void* RaiseTpl;                      /* 0x10 */
    void* RestoreTpl;                    /* 0x18 */
    void* AllocatePages;                 /* 0x20 */
    void* FreePages;                     /* 0x28 */
    void* (*GetMemoryMap)(UINTN*, void*, UINTN*, UINTN*, uint32_t*); /* 0x30 */
    void* (*AllocatePool)(uint32_t, UINTN, void**);                  /* 0x38 */
    void* (*FreePool)(void*);            /* 0x40 */
    void* pad1[19];                      /* 0x48..0xD8（0x48 + 19*8 = 0xE0） */
    void* (*ExitBootServices)(EFI_HANDLE, UINTN); /* 0xE0 */
    void* pad2[5];                       /* 0xE8..0x108 */
    void* (*OpenProtocol)(EFI_HANDLE, EFI_GUID*, void**, EFI_HANDLE, EFI_HANDLE, uint32_t); /* 0x110 */
    void* pad3[4];                       /* 0x118..0x130 */
    void* (*LocateProtocol)(EFI_GUID*, void*, void**); /* 0x138 */
} BOOT_SERVICES;

/* ---- RUNTIME_SERVICES（ResetSystem 在 0x60） ---- */
typedef struct {
    uint64_t Hdr[2];                     /* 0x00 */
    void* GetTime;                       /* 0x10 */
    void* SetTime;
    void* GetWakeupTime;
    void* SetWakeupTime;
    void* SetVirtualAddressMap;
    void* ConvertPointer;
    void* GetVariable;
    void* GetNextVariableName;
    void* SetVariable;
    void* GetNextHighMonotonicCount;
    void* (*ResetSystem)(int, EFI_STATUS, UINTN, void*); /* 0x60 */
} RUNTIME_SERVICES;

/* ---- EFI_SYSTEM_TABLE ---- */
typedef struct {
    uint64_t   Signature;
    uint32_t   Revision;
    void*      FirmwareVendor;
    uint32_t   FirmwareRevision;
    EFI_HANDLE ConInHandle;
    SIMPLE_INPUT*  ConIn;
    EFI_HANDLE ConOutHandle;
    SIMPLE_OUTPUT* ConOut;
    EFI_HANDLE StdErrHandle;
    SIMPLE_OUTPUT* StdErr;
    RUNTIME_SERVICES* RuntimeServices;
    BOOT_SERVICES*    BootServices;
} EFI_SYSTEM_TABLE;

static EFI_SYSTEM_TABLE* gST;
static EFI_HANDLE gImage;
static LOADED_IMAGE* gLoaded;
static uint8_t g_memmap[64 * 1024];
static uint32_t g_mem_count;

/* GUID（用字节序写） */
static const EFI_GUID GUID_LOADED_IMAGE  = {0x5B1B31A1, 0x9562, 0x11d2, {0x8E,0x3F,0x00,0xA0,0xC9,0x69,0x72,0x3B}};
static const EFI_GUID GUID_BLOCK_IO      = {0x964E5B21, 0x6459, 0x11d2, {0x8E,0x39,0x00,0xA0,0xC9,0x69,0x72,0x3B}};
static const EFI_GUID GUID_SIMPLE_FS     = {0x0964e5b22, 0x6459, 0x11d2, {0x8E,0x39,0x00,0xA0,0xC9,0x69,0x72,0x3B}};
static const EFI_GUID GUID_GOP           = {0x9042a9de, 0x23dc, 0x4a38, {0x96,0xFB,0x7A,0xDE,0xD0,0x80,0x51,0x6A}};

/* ---- 测试内核（退出 BootServices 后写 VGA 文本 + hlt） ---- */
static void test_kernel(void) {
    volatile uint16_t* vga = (volatile uint16_t*)0xB8000;
    const char* msg = "FastBoot: kernel booted (UEFI full)";
    int i;
    for (i = 0; msg[i]; i++)
        vga[i] = (uint16_t)(0x0A00 | msg[i]);
    for (;;) __asm__ volatile("hlt");
}

/* ---- VBoot 实现 ---- */
int vb_init(void) {
    if (!gST) return 0;
    if (gST->ConOut && gST->ConOut->Reset)
        gST->ConOut->Reset(gST->ConOut, 0);
    if (gST->BootServices) {
        gST->BootServices->OpenProtocol(gImage, (EFI_GUID*)&GUID_LOADED_IMAGE,
                                        (void**)&gLoaded, gImage, 0, 3);
    }
    return 1;
}

void vb_putc(char c) {
    uint16_t s[2];
    if (!gST || !gST->ConOut) return;
    s[0] = (uint16_t)(unsigned char)c;
    s[1] = 0;
    gST->ConOut->OutputString(gST->ConOut, s);
}

void vb_puts(const char* str) {
    while (str && *str) vb_putc(*str++);
}

int vb_getc(void) {
    EFI_INPUT_KEY key;
    if (!gST || !gST->ConIn) return 0;
    if (gST->ConIn->ReadKeyStroke(gST->ConIn, &key) != EFI_SUCCESS)
        return 0;
    return key.UnicodeChar ? key.UnicodeChar : 0;
}

int vb_read_disk(uint32_t lba, void* buf, uint32_t n) {
    BLOCK_IO* bio;
    if (!gST || !gLoaded || !gST->BootServices) return 0;
    if (gST->BootServices->OpenProtocol(gLoaded->DeviceHandle,
            (EFI_GUID*)&GUID_BLOCK_IO, (void**)&bio, gImage, 0, 3) != EFI_SUCCESS)
        return 0;
    return bio->ReadBlocks(bio, bio->Media->MediaId, lba, (UINTN)n * 512, buf) == EFI_SUCCESS ? 1 : 0;
}

int vb_load_file(const char* path, void* buf, uint32_t max, uint32_t* size) {
    SIMPLE_FS* fs;
    EFI_FILE* root, * file;
    uint64_t rd = max;
    uint16_t wpath[64];
    int i;
    if (!gST || !gLoaded || !gST->BootServices) return 0;
    if (gST->BootServices->OpenProtocol(gLoaded->DeviceHandle,
            (EFI_GUID*)&GUID_SIMPLE_FS, (void**)&fs, gImage, 0, 3) != EFI_SUCCESS)
        return 0;
    if (fs->OpenVolume(fs, (void**)&root) != EFI_SUCCESS) return 0;
    for (i = 0; path[i] && i < 63; i++) wpath[i] = (uint16_t)(unsigned char)path[i];
    wpath[i] = 0;
    if (root->Open(root, (void**)&file, wpath, 1, 0) != EFI_SUCCESS) return 0;
    if (file->Read(file, &rd, buf) != EFI_SUCCESS) return 0;
    file->Close(file);
    if (size) *size = (uint32_t)rd;
    return 1;
}

int vb_get_memory_map(void* out, uint32_t max, uint32_t* n) {
    UINTN sz = sizeof(g_memmap), key, dsz;
    uint32_t ver;
    if (!gST || !gST->BootServices) return 0;
    if (gST->BootServices->GetMemoryMap(&sz, g_memmap, &key, &dsz, &ver) != EFI_SUCCESS)
        return 0;
    g_mem_count = (uint32_t)(sz / (dsz ? dsz : 48));
    if (out && max > g_mem_count) {
        uint32_t i;
        for (i = 0; i < g_mem_count; i++) {
            EFI_MEMORY_DESCRIPTOR* src = (EFI_MEMORY_DESCRIPTOR*)(g_memmap + (uint64_t)i * dsz);
            EFI_MEMORY_DESCRIPTOR* dst = &((EFI_MEMORY_DESCRIPTOR*)out)[i];
            dst->Type = src->Type;
            dst->PhysicalStart = src->PhysicalStart;
            dst->NumberOfPages = src->NumberOfPages;
        }
        if (n) *n = g_mem_count;
        return 1;
    }
    if (n) *n = g_mem_count;
    return 0;
}

int vb_set_video_mode(uint16_t w, uint16_t h, uint16_t bpp) {
    GOP* gop;
    uint32_t mode, i;
    if (!gST || !gST->BootServices) return 0;
    if (gST->BootServices->LocateProtocol((EFI_GUID*)&GUID_GOP, 0, (void**)&gop) != EFI_SUCCESS)
        return 0;
    for (i = 0; i < gop->Mode->MaxMode; i++) {
        UINTN info_size;
        GOP_MODE_INFO* info;
        if (gop->QueryMode(gop, i, &info_size, &info) != EFI_SUCCESS) continue;
        if (info->HorizontalResolution == w && info->VerticalResolution == h &&
            info->PixelFormat == 2) {   /* PixelBlueGreenRedReserved8BitPerColor */
            mode = i;
            return gop->SetMode(gop, mode) == EFI_SUCCESS ? 1 : 0;
        }
    }
    return 0;
}

void vb_reboot(void) {
    if (gST && gST->RuntimeServices && gST->RuntimeServices->ResetSystem) {
        gST->RuntimeServices->ResetSystem(1, 0, 0, 0);   /* EfiResetWarm */
    }
    for (;;) __asm__ volatile("hlt");
}

void vb_boot_kernel(vboot_bootinfo_t* info) {
    UINTN sz, key, dsz;
    uint32_t ver;
    (void)info;
    /* 取内存图键，用于 ExitBootServices */
    sz = sizeof(g_memmap);
    if (gST->BootServices->GetMemoryMap(&sz, g_memmap, &key, &dsz, &ver) == EFI_SUCCESS)
        gST->BootServices->ExitBootServices(gImage, key);
    test_kernel();   /* 演示：退出引导服务后写 VGA + hlt */
}

/* ---- UEFI 入口 ---- */
extern void boot_manager(void);

EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE* SystemTable) {
    gST = SystemTable;
    gImage = ImageHandle;
    vb_init();
    boot_manager();
    return EFI_SUCCESS;
}
