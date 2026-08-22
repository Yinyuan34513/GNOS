# FastBoot — UEFI/BIOS 双协议引导管理器（VBoot 抽象层）

> FastOS（56 行引导扇区）的进化：从"被引导的键盘回显演示"升级为
> "引导者"——一个同时服务 UEFI 与 BIOS 固件的引导管理器。
> 核心是 **VBoot**：把两种固件的底层能力抽象成同一套接口，
> 上层（菜单/内核加载/引导）零平台代码。

---

## 1. 动机

FastOS 的 56 行证明了"最小的引导闭环"（BIOS → 0x7C00 → 键盘回显），
但它**只能被引导，不能引导别人**。FastBoot 让它反客为主：

- 一个引导管理器，同一套源码编译出两个变体：
  - **EFI 变体**：`BOOTX64.EFI`（UEFI 固件引导，clang --target=x86_64-unknown-uefi）
  - **BIN 变体**：引导扇区 + stage2（传统 BIOS，NASM 实模式）
- 上层（菜单/加载器）只调 **VBoot**，永不接触 `gST->ConOut` 或 `int 0x10`。

## 2. 架构总览

```
                 ┌──────────────────────────────┐
   UEFI 固件 ──▶ │  BOOTX64.EFI                 │
                 │  ├─ vboot_uefi.c  (VBoot 实现)│
                 │  └─ menu.c loader.c (共享上层)│
                 └───────────┬──────────────────┘
                             │ 只调 vboot.h
                 ┌───────────▼──────────────────┐
                 │        vboot.h (唯一契约)     │
                 └───────────┬──────────────────┘
                             │ 只调 vboot.h
                 ┌───────────▼──────────────────┐
   BIOS 固件 ──▶ │  stage1.bin + stage2.bin     │
                 │  ├─ vboot_bios (实模式实现)   │
                 │  └─ 菜单/加载（与 UEFI 同逻辑）│
                 └──────────────────────────────┘
```

## 3. VBoot API（唯一契约，vboot.h）

```c
int  vb_init(void);                          /* 初始化（定位输出/输入/存储） */
void vb_putc(char c);  void vb_puts(const char* s);   /* ConOut / int 0x10 AH=0E */
int  vb_getc(void);                            /* ConIn  / int 0x16 AH=00 */
int  vb_read_disk(uint32_t lba, void* buf, uint32_t n); /* BlockIO / int 0x13 AH=02 */
int  vb_load_file(const char* path, void* buf, uint32_t max, uint32_t* size);
                                               /* SimpleFS / 自研 32FS 扇区协议 */
int  vb_get_memory_map(void* out, uint32_t max, uint32_t* n);  /* GetMemoryMap / E820 */
int  vb_set_video_mode(uint16_t w, uint16_t h, uint16_t bpp);  /* GOP / VBE */
void vb_reboot(void);                         /* ResetSystem / 8042 */
void vb_boot_kernel(vboot_bootinfo_t* info);  /* 跳内核（传统一 BootInfo，不返回） */
```

平台映射表：

| VBoot | UEFI 实现 | BIOS 实现 |
|---|---|---|
| vb_putc | `gST->ConOut->OutputString` | `int 0x10` AH=0x0E |
| vb_getc | `gST->ConIn->ReadKeyStroke` | `int 0x16` AH=0x00 |
| vb_read_disk | BlockIO `ReadBlocks` | `int 0x13` AH=0x02 |
| vb_get_memory_map | `gBS->GetMemoryMap` | `int 0x15` E820 |
| vb_set_video_mode | GOP `SetMode` | VBE `int 0x10` AX=0x4F02 |
| vb_reboot | `gRT->ResetSystem` | 8042 复位（端口 0x64 写 0xFE） |
| vb_boot_kernel | `ExitBootServices` + 跳转 | 实模式→保护/长模式 + 跳转 |

## 4. 统一 BootInfo（跨固件传给内核）

```c
typedef struct {
    uint32_t magic;             /* 'VBOOT' */
    uint32_t mem_base;          /* 内存图数组地址 */
    uint32_t mem_count;         /* 内存图条目数 */
    uint32_t video_width, video_height, video_bpp;
    void*    framebuffer;       /* 图形模式帧缓冲（可选） */
    void*    kernel_entry;
} vboot_bootinfo_t;
```

与 GNOS 的 `bootinfo.h` 语义对齐，32os/GNOS 内核可直接消费。

## 5. 目录结构

```
FastBoot/
├── vboot/
│   ├── vboot.h            # 唯一契约
│   ├── vboot_uefi.c       # UEFI 实现 → BOOTX64.EFI
│   └── vboot_bios.asm     # BIOS 实现（实模式，随 stage2 编译）
├── manager/
│   ├── menu.c             # 引导菜单（只调 VBoot）
│   └── loader.c           # 内核加载 + 跳转（只调 VBoot）
├── boot/
│   ├── stage1.asm         # 512B 引导扇区（加载 stage2）
│   ├── stage2.asm         # 实模式 stage2 引导管理器
│   └── fastboot.conf      # 引导项配置（可选）
└── Makefile               # make efi / make bin
```

## 6. 双目标构建

```bash
make efi   # clang --target=x86_64-unknown-uefi + lld-link → build/BOOTX64.EFI
make bin   # nasm stage1.asm + stage2.asm → build/fastboot.bin（含 stage2）
make run-efi   # qemu-system-x86_64 -bios OVMF.fd -drive fat:rw:esp
make run-bin   # qemu-system-i386 -fda build/fastboot.bin
```

## 7. FastBoot 引导管理器功能

- 引导菜单：列出 `boot/` 下的可引导项（32os/GNOS/测试内核）
- 键盘选择（↑/↓ + Enter），显示说明
- 从磁盘/ESP 加载内核镜像 → 填 BootInfo → `vb_boot_kernel`
- 按 `R` 重启、按 `I` 显示系统信息（内存图/视频模式）

## 8. 里程碑

| 阶段 | 内容 | 验证 |
|---|---|---|
| M1 | vboot.h + DESIGN.md + 骨架 | 文档审阅 |
| M2 | UEFI 变体：菜单 + 加载 + 引导 | OVMF 启动 BOOTX64.EFI，菜单可见可键控 |
| M3 | BIOS 变体：stage1→stage2，菜单 + 磁盘读 | QEMU -fda 启动，菜单可见可键控 |
| M4 | 双目标构建固化 + 测试内核引导 | 两个变体各自引导一个测试内核 |
| M5 | 接入真实内核（32os/GNOS 镜像） | 经 FastBoot 引导 32os |

## 9. 设计原则

1. **上层零平台代码**：menu.c/loader.c 不许出现 `int 0x10` 或 `gST->`。
2. **VBoot 是唯一契约**：新增平台只需实现 vboot.h，上层不动。
3. **BootInfo 统一**：跨固件传给内核的引导信息格式一致。
4. **可编译成 EFI 也可编译成 BIN**：同一份上层源码两个产物。
