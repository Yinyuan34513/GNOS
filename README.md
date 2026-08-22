# GNOS

> 🚀 **这是你见过迄今为止最强的小学生开发的操作系统** —— 由一位**五年级小学生**独立开发。
> x86_64 真内核：SMP 多核 + EEVDF 现代调度器 + Linux ABI + 网络栈 + Wayland 桌面。

GNOS 是一个面向 x86_64 的教学型操作系统。它用约 2.6 万行自研代码实现了一个
尽可能贴近 Linux 的用户态 ABI，使得 **musl、BusyBox 1.38、GNU Bash 5.3 和
GNU coreutils 9.9 这些真实的第三方用户软件可以直接在其上运行**——不靠兼容层、
不靠模拟，靠的是内核本身。

> 代码中的命名前缀多为 GNU/gnucos；内核镜像为 `GNOSKr.elf`，项目文档见
> `ANALYSIS.md`。

## 设计哲学

- **Linux/x86-64 syscall ABI 是唯一契约**：系统调用编号、寄存器约定、错误码
  （负 errno）均对齐 Linux（见 `src/shared/sysnum.h`，当前 150 个调用）。内核与
  用户态共享这一份头文件，二者不会漂移。
- **真货优先**：与其自造玩具用户态，不如把内核做对，让成熟的第三方用户栈直接
  跑起来。Bash 的 `bash_cv_termcap_lib=gnutermcap`、coreutils 的
  `linux-stub` 都是出于这一原则的手工配置。
- **SMP + EEVDF**：多核支持（Limine SMP 协议，最多 16 核；每核独立 GS 基址/私有 GDT+TSS，
  syscall 入口按每核内核栈切换）。调度器为 **EEVDF**（与 Linux 6.6+ 同款：全局虚拟时钟、
  进程虚拟运行时间、虚拟截止时间 = vtime + slice、二叉最小堆运行队列、timer 中断里的
  截止时间抢占判定）。运行队列由自旋锁保护，其余共享状态走大内核锁（BKL）。
- **一切可复现、可断言**：开机由 `/etc/rc` 自动执行 `bashtest.sh` 与
  `coreutilstest.sh` 的逐条断言，`make test` 无头跑一遍 QEMU 并打印 debugcon
  日志，任何内核回归在出现的那一刻就被抓到。

## 里程碑（按提交顺序）

| 提交 | 内容 |
|------|------|
| `2b034a0` | 初始版本：引导、PMM/VMM、进程、VFS、ext2/fat、ulib 用户态 |
| `abdf1a1` | 网络栈随内核启动；e1000 + 自研 tcp/sock/net |
| `c79795d` | tmpfs、procfs、ext2 符号链接/rename、信号、OpenRC 脚手架 |
| `860a584` | gfx/fbdev 帧缓冲（/dev/fb0 可 mmap）、子系统注册表、内核堆、ACPI、coldplug、fstab、pread/pwrite |
| `53a2495` | coreutils 支持 + 系统调用补齐（约 660 行） |

## 架构

**引导**（`src/bootloader/`）：Limine 协议（BIOS 与 UEFI 双模式）；内核为
`-static-pie` ELF，被 Limine 重定位进高半区（`0xFFFFFFFF80000000` 附近）；
根文件系统是 64 MiB ext2 内存盘（initrd），内核直接在内存中挂载读写。

**内核**（`src/kernel/`）：

| 子系统 | 说明 |
|--------|------|
| 内存 | PMM 物理帧、VMM 4 级页表、HHDM 直接映射、kheap（边界标签分配，启动时定长） |
| 架构 | GDT/IDT/ISR/中断、syscall 入口（Linux ABI）、TSC/定时器 |
| 进程 | fork/exec/wait、每进程独立页表、抢占式用户态调度、信号 |
| TTY | termios 行规程（`src/kernel/tty.c`） |
| 文件系统 | VFS（挂载表 + fstab）+ tmpfs + procfs + ext2（符号链接/rename）+ fat |
| 图形 | fbcon 文本控制台、gfx、fbdev（Linux 风格 `/dev/fb0`，支持 mmap） |
| 网络 | e1000 驱动 + 自研 tcp/sock 协议栈（`tcp.c`/`sock.c`/`net.c`） |
| 声音 | HDA 与 AC97 |
| 其他 | ACPI、PCI、子系统注册表（`subsys.c`）、coldplug |

**用户态**（`src/user/` + 镜像）：

- `ulib`：自研最小库 + 15 个工具（shell、cat、ls、touch、rm、tac、tail、count、mkdir、mount…），固定加载地址 `0x400000`
- musl 1.2.5 静态程序 9 个（hello、ttytest、sigtest、readlinetest、fstest、mounttest、mount、fbtest、coldplug）
- BusyBox 1.38（含 sh/ash 多调用调度）
- **GNU Bash 5.3**（musl 静态、`-no-pie`、禁用 bash-malloc）
- **GNU coreutils 9.9**（配 `linux-stub` 头 + musl-gcc，全量装进 `/usr/bin`）
- OpenRC 体系（`/etc/rc`、启停脚本接线），`/etc` 有 passwd、group、hosts、nsswitch.conf、resolv.conf、fstab、services 等一整套

## 目录结构

```
GNOS/
├── src/
│   ├── bootloader/      # Limine 引导相关
│   ├── init/            # 内核入口与链接脚本
│   ├── kernel/          # 内核本体（约 2 万行）
│   ├── shared/          # 内核/用户共享契约：sysnum.h（syscall 编号）
│   ├── user/            # ulib 程序、crt0、rc 脚本
│   ├── include/         # limine.h 等
│   └── rootfs/          # 打包进 initrd 的 /etc（测试脚本、fstab…）
├── lib/                 # termcap 等
├── limine/              # Limine 引导文件（limine-bios-cd.bin 等）
├── tools/               # 构建辅助脚本
├── Makefile             # 全量构建
├── linker.ld            # 高半区内核链接脚本
├── limine.conf
└── ANALYSIS.md          # 深度分析报告
```

第三方源码树（musl、busybox、bash、coreutils、openrc）放在 `build/` 下，由手工
fetch 并配置；`make clean` **刻意不清除它们**，避免毁掉手工工具链。

## 构建与运行

依赖：`gcc`（支持 `-static-pie`）、`ld`、`mke2fs`、`qemu-system-x86_64`。

```bash
make all        # 构建内核、用户程序、initrd 与 gnos.iso
make run        # QEMU（BIOS 引导，GTK 窗口）
make run-uefi   # QEMU（OVMF/UEFI 引导）
make guistart   # 演示模式：真实音频后端 + debugcon 落盘 + 不无限重启
make test       # 无头自检：跑 20s QEMU，断言测试随开机执行，打印 debugcon 日志
make clean      # 清理构建产物（保留第三方源码树）
```

QEMU 配置：512 MiB 内存、e1000 网卡（user 网络）、音频设备（HDA + AC97）。

## 现状与限制

- 单核：无 SMP；内核协作式、用户态抢占式，无锁。
- 用户程序固定加载地址 `0x400000`，loader 只接受 ET_EXEC（因此 musl 程序一律
  `-no-pie`）；每进程独立页表，地址永不冲突。
- 根文件系统是内存盘（ext2），尚无真实块设备根文件系统支持。
- 无 GUI 窗口系统：图形一侧只有 fbcon/fbdev；`/dev/fb0` 为最后写入者胜。
- 150 个系统调用已覆盖 musl/Bash/coreutils/BusyBox 的实际使用路径，但不是
  完整的 Linux 面（如缺少线程、完整的网络 syscall 深度等）。
- 教学定位：无安全边界、无多用户隔离。

## 许可

GPL-2.0（见 LICENSE）。代码头与文档中的分析报告同遵循该项目精神：自研部分
GPLv2，第三方组件各自遵循上游许可。