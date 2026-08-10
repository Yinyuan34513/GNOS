# GNOS 深入分析报告（v2）

> 分析对象：`/home/elaina/gnos`（约 11,600 行项目代码，不含 limine.h）
> 生成日期：2026-08-10（v2，对照 2026-08-10 12:56 构建快照）
> 说明：v1 发布于旧快照（9,000 行、43 syscall、无 termios）。本次更新覆盖 tty 重写（termios 行规程）、syscall 面扩张至 70+、BusyBox 接入等变化，并更新了遗留问题状态。

## 目录

1. [项目总览](#1-项目总览)
2. [构建系统](#2-构建系统)
3. [引导流程与架构层](#3-引导流程与架构层)
4. [内存管理](#4-内存管理)
5. [进程管理与调度](#5-进程管理与调度)
6. [系统调用](#6-系统调用)
7. [文件系统](#7-文件系统)
8. [用户态与 shell](#8-用户态与-shell)
9. [双轨用户库：ulib、musl 与 BusyBox](#9-双轨用户库ulibmusl-与-busybox)
10. [突出问题与安全风险](#10-突出问题与安全风险)
11. [设计亮点](#11-设计亮点)
12. [改进建议路线图](#12-改进建议路线图)

---

## 1. 项目总览

GNOS 是一个 x86-64 **单核教学型操作系统**，使用 Limine 引导协议，目标平台为 QEMU（BIOS 与 UEFI 双模式）。整体形态：

- **内核**：`-static-pie` 64 位内核（GNOSKr.elf），被 Limine 重定位进高半区（`0xFFFFFFFF80000000` 附近）。
- **根文件系统**：64 MiB ext2 内存镜像（initrd），由 `mke2fs` 构建，经 Limine module 协议传递，内核直接在内存中挂载读写。
- **用户程序**：10 个 ulib 程序 + 2 个 musl 静态程序（hello、ttytest）+ **BusyBox 1.38.0**（12 个 applet），全部固定加载地址 `0x400000`；每个进程独立页表，地址永不冲突。
- **哲学**：单核、无锁、协作式内核 + 抢占式用户态；所有动态数据结构都是静态数组（无内核堆）。

### 源码规模（v2）

| 子系统 | 文件 | 行数 |
|---|---|---|
| 引导/架构 | kernel.c, loader.c, gdt/idt/isr/switch, timer, panic, debugcon | ~1,100 |
| 内存管理 | pmm.c, vmm.c, paging.c | ~700 |
| 进程/调度 | proc.c, syscall.c | ~2,240 |
| 文件系统 | vfs.c, ext2.c, fat.c | ~2,600 |
| 终端 | tty.c + tty.h | ~650 |
| 用户态 | shell.c, 13 个程序, ulib/crt0 | ~2,400 |
| 共享 | sysnum.h, bootinfo.h | ~460 |

### v2 变化总览

- **tty 重写**：从"糊出来的行读取"升级为**完整 termios 行规程**（tty.c 295→598 行）：键盘中断驱动、ECHO/ECHOCTL、行编辑（VERASE/VKILL）、ISIG（^C/^Z 发前台进程组）、VMIN/VTIME 定时读、SIGTTIN/SIGTTOU/TOSTOP 作业控制。
- **系统调用 43 → 70+**（syscall.c 777→1335 行、sysnum.h 182→411 行）：新增 openat/newfstatat/getdents64/fcntl/chdir/cwd/ioctl/144 字节 Linux stat 等，为 musl/BusyBox 铺平 libc 面。
- **BusyBox 1.38.0 接入**：12 个 applet（cat/cp/echo/ls/mv/rm/uname/wc…，无 shell），`make test` 冒烟全绿。
- **新增 `ttytest`**：142 行 termios/作业控制回归测试（21 项断言）。
- **每进程 cwd**：`chdir/fchdir/getcwd` + 路径规范化（`path_norm/path_abs`）。
- **无 git 仓库**：版本演进只能靠文件对比与 build 日志推断。

**依然存在的死代码**（v1 已标记，未动）：`bootloader.c`（遗留 UEFI 引导器，未接线）、`fat.c`（745 行，完全未编译）、`paging.c`（无调用者）。

---

## 2. 构建系统

### 2.1 编译管线（Makefile，258 行）

```
kernel/*.c  --(gcc -m64 -fpie)--> GNOSKr.elf      (Limine 入口, static-pie)
user/*.c    --(gcc -m64 -fno-pie)--> *.elf        (全部 @0x400000)
musl: crt1.o + crti/crtn.o + libc.a 静态链接     (hello, ttytest)
busybox 1.38.0 独立构建 (.config 12 applets)     (busybox.elf + 12 个复制 applet)
+limine bins --(xorriso)--> gnos.iso             (混合 BIOS+UEFI CD)
```

关键编译选项（Makefile:25-33）：

- **`-mgeneral-regs-only`**：CPU 从 Limine 收到时 CR4.OSFXSR 清零，任何 SSE 指令都会 #UD，且当时没有 IDT 会直接三重故障——启动早期的真实踩坑记录。
- **`-mno-red-zone`**：中断可能落在内核栈上。
- 内核 `-fpie` + `-static-pie` 链接；用户程序 `-fno-pie -fno-pic`。
- **musl 特殊处理**（Makefile:90-96）：`MUSLCFLAGS` 从 BASEFLAGS 中 filter-out SSE 禁令——libc.a 用 SSE 编译、调用者禁 SSE 会导致 double 传参约定撕裂（注释详述）。
- **BusyBox 构建**（Makefile:109-116）：binutils 2.41 `ar` 的 LTO 插件崩溃，用 `AR=gcc-ar` 绕开——踩坑记录。

### 2.2 用户程序轨道（v2）

- **ulib 轨道**（UCRT = crt0.o + ulib.o）：10 个原生程序 `init shell count ls cat tail tac rm mkdir touch` + `dir.elf` 别名。
- **musl 轨道**（MUSLPROGS，Makefile:69-71）：`hello` + 新增 `ttytest`，static pattern 规则统一编译/链接。
- **BusyBox 轨道**（THIRD_PARTY，Makefile:73-80）：12 个 applet 开启（`CONFIG_SH_IS_NONE=y` 无 shell）；initrd 中 `/bin/busybox.elf` + `/usr/bin/{cat,cp,...}` **12 个复制**（非硬链接——注释明确：ext2 驱动没在 nlink>1 的 inode 上跑过）。
- **`distclean` 新增**（Makefile:247-258）：`clean` 只清可再生物，保住手拉的 musl/busybox 源树。

### 2.3 initrd 与 ISO

- initrd 构建沿用 mke2fs：`-t ext2 -b 1024 -I 256 -O ^resize_inode,^dir_index,^ext_attr`（`^dir_index` 是刻意的——内核驱动只认识线性目录）。
- FHS 骨架 + `/bin` 程序 + `/etc/rc`。
- ISO：xorriso 双引导（El Torito BIOS + EFI）。
- `make test`：无头 QEMU 20s + isa-debugcon 抓 `build/dbg.log`。

---

## 3. 引导流程与架构层

### 3.1 内核初始化顺序（kernel.c:58-141）

```
GDT/IDT（最先装，坏内存访问变可读 panic 而非三重故障）
→ fbcon 控制台
→ pmm_init / vmm_init（开 EFER.NXE、CR4.OSFXSR）/ vmm_map_kernel_bss
→ vfs_init（挂载 initrd）
→ tty_init（PS/2 键盘 IRQ1 + termios 行规程） / syscall_init（int 0x80 门 + syscall 指令 MSR）
→ proc_init / timer_init(100Hz)
→ proc_spawn_init("/init.elf")（PID 1）
→ sched_start()（boot 线程变 idle 循环，永不返回）
```

Limine 协议请求全部经 `__attribute__((used, section(".limine_requests")))` 放入专用段（limine_requests.c），通过 `LIMINE_ENTRY_POINT_REQUEST`（revision 3）以 `call` 方式进入 `kernel_entry`。

### 3.2 双系统调用入口

| 入口 | 机制 | 细节 |
|---|---|---|
| `int 0x80` | 中断门（DPL3） | CPU 经 TSS.RSP0 切栈、硬件压 5 字帧 |
| `syscall` 指令 | MSR（IA32_STAR/LSTAR） | CPU **不压帧不切栈**；isr.asm 手工把用户 RSP 存入 `syscall_user_rsp`（单核下安全），切换 `g_kernel_rsp0`，然后**按降序手工压出与 int 0x80 完全同构的 regs_t 帧**（isr.asm:107-167） |

选 `iretq` 而非 `sysret` 返回的原因：可以放心使用真实用户选择子（0x23/0x1B），不必做 IA32_STAR 的 sysret CS 算术（isr.asm:92-95）。用户程序（musl/BusyBox）走 `syscall` 指令，TSS.RSP0 与 `g_kernel_rsp0` 由 `tss_set_rsp0` 同步维护（gdt.c:51-55），两条路径落栈一致。

### 3.3 中断帧与分发

- 全部 256 个 IDT 向量指向 `isr_stub_base + v*16`——**固定 16 字节步长**的汇编 stub（`%rep 256` + `align 16`），PIE 内核下免去 256 项重定位指针表（isr.asm:3-6）。
- 分发逻辑（idt.c:143-204）：#DF → `fault_halt`；ring-3 异常 → SIGSEGV 只杀进程；ring-0 异常 → `fault_halt`（系统级致命）；0x80 → syscall；IRQ → **先 EOI 后 handler**（handler 可能切任务，不先应答会挂死中断线）。
- 信号检查挂在 `isr_dispatch` 的三个返回点（idt.c:174,186,199），只在返回用户态时执行。
- **v2 变化**：IRQ1 键盘中断接入（tty 驱动）；timer_irq 增加 `sched_expire_timeouts`（VTIME 定时读兜底，ring0 也检查——定时睡眠者常是机器上唯一进程，timer.c:27-30）。

### 3.4 上下文切换

`switch_context`（switch.asm:22-39）只有 7 条指令：压 6 个 callee-saved → 存 RSP → 换 RSP → 弹 → `ret`。新进程从未被切走过，所以 `build_startup_stack`（proc.c:151-169）伪造一个"刚被切走"的栈：`[完整 regs_t 帧][ret_to_user 返回地址][6 个零槽]`，`switch_context` 的 `ret` 恰好"返回"进 `ret_to_user`（与 isr 尾部逐字节相同的弹帧逻辑），`iretq` 直落用户态——这是全系统最精巧的一段代码。

### 3.5 遗留 UEFI bootloader（bootloader.c，371 行）

用 gnu-efi 写的早期引导器，**未被 Makefile 引用、与当前 PIE 内核不兼容、内核不读它的 bootinfo_t**——v1 已详述，v2 未动。结论不变：一段未接回主线的备用引导器（未来真机路径），`bootinfo.h` magic 是 ASCII "BOOFUNCG"。
---

## 4. 内存管理

### 4.1 物理内存（pmm.c，168 行）

v1 分析不变：单一位图 + first-fit（`g_next_hint` 游标，两遍扫描回绕）；Limine memmap 两遍扫描（Pass 1 求最高地址定长，Pass 2 位图停放在单个连续 usable 区域头部）；启动全 1（全占）后裁剪；强制占用 `[0, 1MiB)` 与位图自身；`pmm_free` 防双释放（pmm.c:159）；失败返回 0 与"帧 0 永不可分配"自洽。无 buddy、无连续分配、无 ACPI reclaim。

### 4.2 虚拟内存（vmm.c，391 行）

v1 分析不变，要点重申：

- **无内核堆**：内核动态内存 = PMM 帧 + 静态数组（`g_spaces[32]`、BSS 内核栈等）。
- **地址空间模型**：每进程独立 PML4；`vmm_create` 把内核 PML4 的 **256..511 项（上半部）整体浅拷贝**进新 PML4（vmm.c:152-155）——上半部**指针共享**，下半部私有。这是进程隔离的唯一机制。
- `vmm_init` 开 EFER.NXE（否则 PTE bit 63 是保留位）、CR4.OSFXSR。
- `vmm_map_kernel_bss` 自愈式补齐 BSS 映射。
- `walk()` 遇 `PTE_PS`（大页）直接 **panic**（vmm.c:199-200）——对 Limine 映射粒度的隐含依赖（脆弱点）。
- **fork = `vmm_clone` = eager 深拷贝**（无 COW）；`vmm_destroy` 递归只释放**下半部**（limit=256）。

### 4.3 地址布局（不变）

| 区域 | 地址 |
|---|---|
| 内核镜像 | `0xFFFFFFFF80000000` 附近（Limine 重定位） |
| HHDM 直接映射 | 物理内存 + `0xFFFF800000000000` |
| 用户程序基址 | `0x400000`（ulib） / `0x600000`（init） |
| mmap arena | `0x500000000000` 起 bump，上限 `USER_STACK_TOP` |
| brk 堆底 | `0x600000000000` |
| 用户栈 | `0x700000000000` 向下固定 256 KiB（无栈自动增长） |
| 用户地址上限 | `0x800000000000`（USER_VA_LIMIT） |

### 4.4 paging.c（126 行）——死代码（未变）

"不重建页表、只翻转 U/S 给 Limine 那块地址空间开用户窗口"的**两代前设计**。`paging_make_user` 全仓库无调用者。

### 4.5 隔离状态（未变）

内核页表在用户 CR3 中完整存在，靠 U 位隔离；**未启用** SMEP/SMAP/KPTI/KASLR。内核直接解引用用户指针（无 copy_from_user）——单核无 SMAP 下成立，但信任边界问题依旧（见 §10）。

---

## 5. 进程管理与调度

### 5.1 进程结构（proc.h:51-98）

固定数组 `g_procs[16]`、`g_kstacks[16][16KiB]`（BSS，上半部共享所以任何页表下都可达）。**v2 新增字段**：每进程 `cwd` 字符串（proc.h:91-98，fork 继承，proc.c:404）。其余关键字段不变：`state`、`as`、`kstack_top`、`saved_rsp`、`fds[16]`、`sig_pending/sig_ignored/sig_mask`、`brk`、`fs_base`、`clear_child_tid`、`mmaps[8]`、`fpu[512]`。

### 5.2 生命周期（未变）

- **创建**：`proc_spawn_init`（PID 1，带 /dev/tty 三个 fd 引用）与 `proc_fork`。内核启动后唯一新进程来源就是 fork。
- **阻塞**：`sched_block_irqoff` 在 `cli` 下完成"测试-睡眠"原子序列（tty/pipe 用）；**v2 新增 `sched_block_timeout`**（有界睡眠，tty.c:442 VTIME 用）；`sched_wake_reason` 广播式按原因唤醒。
- **退出**：`proc_exit`（proc.c:472-517）关全部 fd → 按 `clear_child_tid` 写 0 → **先 `vmm_switch_kernel` 再 `vmm_destroy(p->as)`** → 孤儿改挂 init → ZOMBIE → 给父发 SIGCHLD → `sched_yield` 永不复返。
- **回收**：`proc_waitpid` 发现 ZOMBIE 即置 UNUSED。

### 5.3 调度器（未变）

- **无优先级轮转**；`pick_next` 从当前槽位之后**环形扫描**第一个 READY。
- **时间片**：PIT 100Hz；`timer_irq` 只在 `(r->cs & 3)==3`（用户态）时 `sched_tick()`——**内核态绝不抢占**，无锁正确性根基。**v2 变化**：timer_irq 同时跑 `sched_expire_timeouts`（VTIME 定时读超时唤醒）。
- `schedule()` 四连操作：`tss_set_rsp0` → `proc_set_fs`（TLS MSR）→ `vmm_switch`（CR3）→ `fpu_load`（eager，无 lazy）。
- idle：`sti; hlt` 紧邻关唤醒竞态。

### 5.4 fork 与 exec（基本未变）

- **fork 是真 fork**：`vmm_clone` 深拷贝下半区；继承 pgid/sig_ignored/**sig_mask（v2 修复）**/brk/cwd（v2 新增）/fs_base/mmaps 等；fd 表照抄并逐个 ref；子进程返回帧 = 父进程陷入帧拷贝、只把 rax 改 0。**FPU 仍不拷贝**（v2 未修，见 §10）。
- **execve 就地换像**：argv/path 先拷进内核 argbuf → `build_image`（512KiB 上限）→ 换 as、销毁旧 as → 重置 brk/fs/clear_child_tid/**cwd（v2）** → 原地改写陷入帧。失败可恢复。
- **loader.c 纯解析器**：ELF64 与 a.out；校验链完整；所有段统一可写（无 W^X），NX 位区分可执行；内容经 `vmm_copy_to_user`（HHDM 直拷）写进指定地址空间。
- **push_args**：从用户栈顶向下铺 SysV 启动块（argc/argv/envp/auxv AT_PHDR/PHENT/PHNUM/PAGESZ/ENTRY/AT_NULL）。

### 5.5 信号（v2 部分修复）

`proc_signal`（发送）与 `proc_check_signals`（只在返回用户态时检查）：
- SIGCONT 即时恢复、SIG_IGN 丢弃、stop 类 → STOPPED + 通知父、其余 → 杀死。
- **v2 修复**：`proc_signal_blocked` 使用 sig_mask（proc.c:612）、fork 继承 sig_mask（proc.c:394）、rt_sigprocmask 回读（syscall.c:888-889）。
- **v2 未修**：投递仍不看 sig_mask——proc.c:709 `deliverable = sig_pending & ~sig_ignored` 缺 `& ~sig_mask`。
- **仍无用户态信号处理函数**（rt_sigaction 空壳，无 rt_sigreturn）。
- **v2 新增信号投递目标**：前台进程组（`proc_signal_group`，ISIG ^C/^Z 用）。

---

## 6. 系统调用（v2 重大扩张：43 → 70+）

### 6.1 接口约定（不变）

- 编号完全对齐 **Linux x86-64**：RAX=编号，RDI/RSI/RDX/R10=参数，返回 RAX（成功为值，失败为**负 errno**）。私有调用推至 400+：`signal(400)/tcsetpgrp(401)/tcgetpgrp(402)`。
- sysnum.h 扩到 411 行，新增 errno 集合（E_SRCH/E_CHILD/E_AGAIN/E_NOMEM/E_XDEV/E_ISDIR/E_MFILE/E_NOTTY/E_RANGE/E_NAMETOOLONG/E_NOSYS/E_NOTEMPTY，vfs.h:37-58）。

### 6.2 v2 新增实现（核心变化）

| 调用 | 行为 |
|---|---|
| openat/newfstatat/faccessat | 基于 dirfd 的路径 + AT_FDCWD/AT_EMPTY_PATH——musl 的"所有 open/fstat 都走 *at"依赖（syscall.c:155-168, 223-230, 365-378, 488-497） |
| chdir/fchdir/getcwd | **每进程 cwd** + `path_norm/path_abs` 路径规范化（压平 ./..、连续斜杠，E_NAMETOOLONG）（syscall.c:68-146, 382-435） |
| getdents64 | 变长 dirent、8 字节对齐、fd pos 作游标（vfs.c:375-424） |
| fcntl | F_DUPFD/F_DUPFD_CLOEXEC 真实现，FD_CLOEXEC 记账空转（syscall.c:237-266） |
| dup | 新增（v1 缺失） |
| chmod/umask/access/chown | 有接口且记账，**不强制**（权限模型仍未实现） |
| uname | 新增（BusyBox 依赖） |
| ioctl | **真实现**：TCGETS/TCSETS*/TIOCGWINSZ/TIOCSWINSZ/TIOCGPGRP/TIOCSPGRP/TIOCSCTTY；非 tty fd 一律 ENOTTY（v1 是恒 0 空转）（syscall.c:653-719） |
| readlink/lstat | 恒 -EINVAL / ==stat（无符号链接） |
| rename | 恒 -E_XDEV 占位（靠 mv 的 cp+unlink 兜底，日志证实可用） |
| rt_sigprocmask | 回读 sig_mask（v2 部分修复） |

### 6.3 完整实现清单（70+，精要）

| 调用 | 行为 |
|---|---|
| read/write/readv/writev | 范围检查后直调 VFS；musl stdio 依赖 readv/writev |
| open/close/dup2 | 双重间接 fd；O_CREAT 失败回滚；**无 O_EXCL**（v2 未修） |
| stat（旧 gstat_t）/newfstatat | 16 字节 gstat_t（v1 兼容）+ **144 字节 Linux struct stat**（lstat_t，sysnum.h:317-336，musl/BusyBox 解码用） |
| mmap/munmap/mprotect/madvise/brk | mmap：8 槽 bump arena；mprotect/madvise 恒 0 空转 |
| rt_sigaction | 空壳：用户 handler 不支持 |
| ioctl/sched_yield/getuid/gid/… | 见上表 / 直通（isatty 恒真不再成立——ioctl 现在真区分 ENOTTY） |
| fork/execve/exit/exit_group | 见 §5 |
| wait4 | 支持 pid>0 与任意子；不带负 pid 组等待 |
| kill/setpgid/getpgid | **kill(-1) 语义仍错**（syscall.c:594）；setpgid 无权限检查 |
| mkdir/unlink | 转 VFS |
| gettimeofday/clock_gettime | `ticks/100` 秒（硬编码 100Hz，与 SCHED_HZ 仍是两处魔法数字） |
| futex | 空操作：WAIT 恒成功、WAKE 恒 0 |
| arch_prctl | 仅 ARCH_SET_FS/GET_FS |
| set_tid_address | 记 `clear_child_tid`，exit 时清零 |
| gettid | 返回 pid |

**BusyBox 实际链接验证**（objdump + musl stub 解码）：3,4,8,9,12,16,19,21,63,72,79,82,83,87,89,90,92,94,95,158,217,218,228,231,262 全部命中实现；**缺失**：`mremap(25)`（musl 大块 realloc）、`rmdir(84)`（`rm -r` 目录 → ENOSYS）、`link(86)/symlink(88)`（cp -a）、`mknod(133)`、`futimesat(261)`（cp -p）。

### 6.4 syscall 指令配置（未变）

IA32_STAR（内核 CS=0x08 槽位）、IA32_LSTAR、EFER.SCE|NXE；不用 sysret，统一 iretq。IA32_FMASK 留 0，靠帧里的 R11 恢复 IF。

---

## 7. 文件系统

### 7.1 架构（未变，但接口大幅扩）

VFS 三元设计：ext2 根挂载 + 合成 `/dev` 字符设备表 + 匿名管道。没有块层/页缓存/锁，整个 FS 是**内存镜像直写驱动**。操作表仍是 `vfs_ops_t{read, write}` 两个回调（vfs.h:56-59），node 内嵌 `ext2_dirent_t`（与 ext2 强耦合是 FAT 无法接入的原因）。

### 7.2 v2 新增

| 接口 | 行为 |
|---|---|
| `vfs_stat_linux`/`vfs_fstat` | **144 字节 Linux struct stat**；权限位来自 inode mode（`n->e2.mode & 0x0FFF`，vfs.c:334-335），设备/管道默认 0600；st_nlink 目录=2/文件=1（诚实报，不追真实 link count）（vfs.c:323-347） |
| `vfs_dir_getdents64` | getdents64(217) 变长 dirent、8 字节对齐、fd pos 作游标（vfs.c:375-424）；与旧定长 gdirent_t read **同一 fd 二选一**（vfs.c:369-374 注释明示，API 语义脆） |
| `vfs_file_path` | fd→记录在案路径（fchdir/openat 复用，vfs.c:501） |
| `vfs_file_ops` | ioctl 判定"是不是终端"（tty.c:547-550，isatty 依据） |
| `vfs_chmod` | 无 inode 节点假成功（vfs.c:528-529）；否则 `ext2_chmod` 写回 + ctime（vfs.c:518-532） |
| `ext2_chmod` | 掩 S_IFMT 只改低 12 位、写 I_CTIME（fs_now，ext2.c:853-869） |

### 7.3 ext2 驱动（ext2.c，1144 行）

v1 分析基本不变（balloc/ialloc/bmap 12+间接、rec_len 空隙删除、dir_add 追加块、dtime 防孤儿判误、全路径回滚）。**v2 唯一新增**：`ext2_chmod`。dir_index/HTree 依旧不支持（ext2.h:15-19；Makefile:212 `-O ^dir_index` 构建侧配合）。

**v2 验证进展**：BusyBox 的写侧路径（cp/mv/rm/ls -l）是 ext2 首次被第三方程序压测——日志证实全绿（cp 1228→1519 字节、ls 显示 `-rw-r--r--` 权限位与 nlink=1、rm 后目录空）。

### 7.4 依然缺失（v2 未动）

- **O_EXCL**：vfs_file_open 无 EXCL 分支（vfs.c:479-484 对已存在路径静默复用）；目录可被 O_WRONLY|O_TRUNC "成功"打开（无 EISDIR，vfs.c:488）。
- **硬链接**（无 link syscall；nlink 最多 2）；**符号链接**（readlink 恒 -EINVAL）；**fsync**；**rename**（E_XDEV 占位）。
- **权限强制**：chmod/umask/access/chown 有接口且记账但不强制。
- **unlink 打开文件立刻回收 inode/块**（ext2.c:1139-1142）——临时文件模式仍不成立。
- O_APPEND 只取一次 pos；管道 seek 返回 -E_INVAL 非 ESPIPE。

### 7.5 initrd 与根文件系统

`Limine module[0]` → HHDM 直映射虚拟地址 → `ext2_mount` 内存直写。v2 布局：`/init.elf`、`/bin/{init,shell,count,ls,dir,cat,tail,tac,rm,mkdir,touch,hello,ttytest,busybox}.elf`、`/usr/bin/{cat,cp,echo,false,head,ls,mkdir,mv,pwd,rm,true,uname,wc}`（BusyBox applet 复制）、`/etc/rc` + 空壳目录。可写但断电即失。

---

## 8. 用户态与 shell

### 8.1 shell（shell.c，906 行）

- **内建**：help/echo/jobs/fg/bg/kill/pid/exit（未新增）。
- **外部程序**：先解析再 fork（存在性预检避免管道半启动）；setpgid 双保险；重定向 `< > >>`；管道最多 4 段、一条管道一个进程组。
- **路径解析**：裸名依次试 `{/bin,/usr/bin,/sbin,/usr/sbin,""}`，名字无 `.` 再补 `.elf`。
- **作业表**：8 槽；`fg`/`bg` 支持 `%n` 作业号。
- **v2 变化**：
  - 私有 tty fd `shell_tty`（shell.c:66-72, 817-819）：脚本模式把脚本接上 fd 0 后 tcsetpgrp 不再误判 ENOTTY。
  - 启动横幅改打印 pid/pgid（shell.c:841-846）。
  - `interactive` 标志：脚本模式静默、不打 prompt。
  - `reap_jobs` 每轮循环先收割。
- **限制不变**：无引号、无变量展开、无通配符（rc 里 `$?` 仍字面打印）；`#` 开头行当注释跳过。

### 8.2 各程序一览（v2）

| 程序 | 要点 |
|---|---|
| init（161 行） | 监督者：run_rc（/etc/rc，脚本模式，不交终端）→ spawn_shell（独立 pgrp + tcsetpgrp）→ 永久 waitpid 收割；重启风暴保护 |
| count（47 行） | 作业控制测试靶子 |
| ls/dir（95 行） | 旧 gdirent_t 记录流路径 |
| cat/tail/tac | 未变 |
| touch/mkdir/rm | 未变（错误文案仍有 FAT 时代遗留） |
| **hello（新增行为）** | musl printf；启动冒烟第一项 |
| **ttytest（v2 新增，142 行）** | termios/作业控制回归测试：21 项断言——/dev/tty 可开、isatty 区分、文件上 tcgetattr→ENOTTY、TIOCGWINSZ、termios 默认值、cfmakeraw 往返、VMIN/VTIME 有界读（轮询即时 + 0.3s 超时）、TCAFLUSH 恢复、**后台 tcsetattr 被 SIGTTOU 停止且信号编号正确** |

### 8.3 crt0 与启动块（未变）

`_start` 四件事：取 argc/argv、清 rbp、`call main`、返回值交给 `call exit`。内核 `push_args` 保证 SysV 栈布局（16 对齐 + 8 偏移）。

---

## 9. 双轨用户库：ulib、musl 与 BusyBox

### 9.1 三轨道并存（v2 核心变化）

- **ulib**（243 行）：手写最小 libc——字符串/内存函数 + syscall 封装（全走 `int 0x80`），无 printf。`abspath` 补 `/`。
- **musl**（`--disable-shared` 静态 libc.a）：`hello` + 新增 `ttytest`。链接次序 crt1→crti→prog→libc.a→crtn。
- **BusyBox 1.38.0**：12 applet、无 shell（`CONFIG_SH_IS_NONE=y`）。**意义**：可写文件系统侧首个第三方压测 + "为 ash 进场铺路"（Makefile:75-76 自注：ash wants job control features the kernel is only now growing——SIGTTIN/SIGTTOU/WUNTRACED/TOSTOP 整条链就是为迎 ash 进门）。

### 9.2 内核为第三方 libc 补齐的 syscall 面（v2）

- **readv/writev**（musl stdio 刚需）
- **mmap/brk/munmap/mprotect/madvise**（mallocng）
- **openat/newfstatat/faccessat**（musl 所有 open/fstat 都走 *at）
- **144 字节 Linux stat**（lstat_t）
- **getdents64**（BusyBox ls 刚需）
- **chdir/fchdir/getcwd + cwd**（musl/pwd）
- **fcntl/dup**（dup2 外的新姿势）
- **ioctl 真实现**（isatty 区分 ENOTTY）
- **rt_sigaction/rt_sigprocmask**（假装成功，pthread 机制不中止）
- **futex 空操作**（单线程假进展）
- **arch_prctl**（TLS）、**set_tid_address**、**clock_gettime**、**uname**、**syscall 指令入口**

---

## 10. 突出问题与安全风险

按严重度排序（含 v1 遗留状态跟踪）。

### 10.1 严重：信任边界（v1 列、v2 **全部未修**）

1. **用户指针只查上下界、不查映射 → 任意用户程序可挂死内核（DoS）**。`user_ptr_ok`（syscall.c:46-53）只验证 `p < USER_LIMIT && p+len <= USER_LIMIT`；而 read/write/stat/open/mkdir/unlink/execve/pipe/clock_gettime/readv/writev/**ioctl arg（v2 新面，syscall.c:663/695 也只做范围检查）** 等直接解引用用户地址。传未映射但合法范围地址 → ring-0 #PF → `fault_halt` 死机（idt.c:165-178 只把 ring-3 故障转 SIGSEGV）。
2. **sys_mmap hint 路径缺半区校验 → 可卸掉共享内核映射**（syscall.c:792-793）。vmm_alloc_range/unmap 不校验 vaddr 半区，hint 指向上半区已映射地址可导致 munmap 释放共享页表里的内核叶子。
3. **set_tid_address 不校验地址 → 任意内核地址清零 8 字节**（vmm_copy_to_user 只查 present 不查 U 位）。
4. **路径字符串无界读取**（sys_open 只查路径指针首字节，ext2_lookup 在用户内存上无界扫描）。

### 10.2 中等：正确性与资源

5. **fork 丢 NX**（clone_level 无条件 VM_EXEC，vmm.c:350-352）——未修。
6. **mmap 第 9 槽满泄漏页**（vmm_alloc_range 中途 OOM 不回滚）——未修。
7. **fork 不拷贝 FPU 状态**（proc.c:373-423 无 fpu 处理）——未修。
8. **execve 不清 sig_pending**（POSIX 要求丢弃）——未修；子进程首跑绕过 proc_check_signals（伪造栈路径）。
9. **kill(-1, sig) 语义错误**（syscall.c:594，`pid==-1` 被算 pgid=1）；setpgid 无权限检查——未修。
10. **sig_mask 半修复**：`proc_signal_blocked`/fork 继承/rt_sigprocmask 回读已用，**投递仍不看**（proc.c:709 缺 `& ~sig_mask`）。
11. **unlink 打开文件立刻回收 inode/块**（ext2.c:1139-1142）；ialloc 可能复用 inode——未修。
12. **getdents64 与 gdirent_t read 共用一个 fd 的 pos**（两接口二选一，切换调用污染游标，vfs.c:369-374）——新标记。
13. **ISR stub 16 字节假设在 vector≥0x80 失效**（push imm32 膨胀为 19 字节）；无 IST——未修。
14. **O_EXCL/ESPIPE/EISDIR 语义缺**、目录可 O_WRONLY|O_TRUNC 打开——未修。
15. **size_t/ptrdiff_t/uintmax_t 32 位**（stddef.h:4-5、stdint.h:16-17）——未修。
16. **陈旧文案**：kernel.c:119-122 仍打印 "mounting FAT filesystem"/"mountable FAT volume"（实际 ext2）；ulib.h:73-77 仍写 "flat FAT volume、没有当前目录"（**已完全过时**，cwd/openat 早加了）；rc:4 仍写 "shell 没有注释语法"（shell.c:869-874 已支持 `#`）；sysnum.h:85 与 syscall.c:312 仍写 "24-byte gstat_t"（实际 16 字节）。

### 10.3 低危 / 已知取舍（如实记录）

- 无 SMEP/SMAP/KPTI/KASLR；无 W^X；getuid 全 0。
- futex/rt_sigaction/mprotect/madvise 空转；多线程 musl 会假进展。
- 用户态可 `cli` 饿死其它进程（无 NMI/APIC 兜底）。
- vmm_unmap 不释放中间表、对非当前 as 不刷 TLB（单核依赖）。
- walk() 遇大页 panic（Limine 映射粒度隐含依赖）。
- exec 映像 512 KiB 上限；a.out 超长静默截断。
- waitpid 不带负 pid 组等待、无 WCONTINUED。
- tty_get_winsize 像素恒 0；时钟用 tick/100 魔法数。
- 交互模式下 tty 回显双写 0xE9 debugcon（make test 模拟键盘时 dbg.log 混入回声/^X 序列）。

---

## 11. 设计亮点

1. **先装 GDT/IDT 再碰内存**：把"无声三重故障"变成"可读的红色 panic"。
2. **TSS.RSP0 与 g_kernel_rsp0 双镜像**：syscall 指令不看 TSS，两条 syscall 路径落栈一致。
3. **`syscall` 路径手工构造与 int 0x80 完全同构的 regs_t 帧**。
4. **先 EOI 后 handler**——避免 handler 阻塞饿死其他中断。
5. **用户态可抢占、内核态永不抢占 + 无锁单核**，正确性论证可推理。
6. **stub 固定 16 字节步长**免去 256 项指针表重定位。
7. **`sti;hlt` 紧邻**关唤醒竞态；调度游标前移防饿死。
8. **fork=深拷贝、exec 先建好地址空间**（失败可恢复）。
9. **`proc_exit` 先 `vmm_switch_kernel()` 再销毁地址空间**。
10. **注释质量极高**：每条关键决策都有"为什么"的踩坑记录（EFER.NXE、FXSAVE、KERNEL_GS_BASE 弄崩 %fs、EOI 次序、musl syscall 依赖、ar LTO、SSE 传参约定）。
11. **push_args 精确复刻 SysV 启动块**（含 auxv PT_TLS 发现链），musl 静态程序端到端跑通。
12. **v2 新亮点**：
    - termios 线规一次到位：RING_EOF 带外标记区分字面 ^D 与输入结束（tty.c:52-57）；
    - **有界睡眠 `sched_block_timeout` + timer 到期兜底**（VTIME 睡眠者常是唯一进程，ring0 也检查）；
    - `tty_check_ttou`：后台进程对终端变更先遭 SIGTTOU，ignoring/blocking 者放行——shell 后台 fork 后交还终端的前提；
    - termios_t 36 字节是 musl 60 字节结构的前缀（双向拷贝安全，sysnum.h:168-178 论证）；
    - **BusyBox 冒烟纳入 make test 作为第三方回归**——12:56 构建 dbg.log 全绿是整条链路的实证。
    - `ttytest`：把 termios/作业控制行为固化成 21 项可重复断言。

---

## 12. 改进建议路线图

**P0（安全，先修——v2 未动的老账）**
1. `user_ptr_ok` 升级为页级校验（先 `vmm_resolve` 确认映射，或借 Linux access_ok 语义）；所有解引用用户指针的路径统一走 `vmm_copy_to_user/from_user`；ioctl 的 arg 校验同步补。
2. `vmm_map/vmm_unmap/vmm_alloc_range` 内部断言 `vaddr < USER_VA_LIMIT`；sys_mmap hint 路径补半区校验（syscall.c:792-793）。
3. 路径字符串限长拷贝；`set_tid_address` 校验映射；`vmm_copy_to_user` 加 U 位检查（vmm.c:274-282）。

**P1（正确性）**
4. `clone_level` 保留 NX；fork 拷贝 FPU（proc.c:373-423）；execve 清 sig_pending（proc.c:469-473）；kill(-1) 广播（syscall.c:594-595）；sig_mask 进投递（proc.c:709）。
5. unlink 打开文件延迟回收（等最后引用关闭）；O_EXCL/ESPIPE/EISDIR 语义；getdents64 与 gdirent_t 的 pos 隔离。
6. 魔法数字收敛：SCHED_HZ 单一来源；uintmax_t/size_t/ptrdiff_t 修 64 位；清陈旧文案群（FAT 字样、24-byte gstat_t、rc 注释）。

**P2（能力扩展）**
7. 三个几行就能消灭的 ENOSYS 缺口：`rmdir(84)`（rm -r）、`futimesat(261)`（cp -p）、`mremap(25)`（musl 大 realloc）。
8. 用户信号处理函数 + rt_sigreturn；COW fork；ash（BusyBox shell）进场——termios/作业控制已就位，只差 O_EXCL/rename/fsync 等边角。
9. 权限强制（mode 真校验）；mkdir 的 8.3 文案修正。
10. 清理死代码三件套：接回或删除 legacy bootloader、编译或删除 fat.c、删 paging.c。
11. 多线程/SMP 方向同 v1：真实 futex → clone；`syscall_user_rsp`/`g_argv` 全局态收进 per-CPU 结构。

**一句话总结（v2）**：本轮的灵魂是 **tty 从"糊出来的行读取"升级为完整 termios 线规 + 作业控制内核侧**，并以此为 musl/BusyBox 铺平了 openat/fcntl/getdents64/newfstatat/ioctl/144 字节 stat/chdir-cwd 的 libc 面；新增 ttytest 自检并把 BusyBox 12 个 applet 纳入启动冒烟——12:56 构建 dbg.log 全绿（hello ✓、BusyBox 写路径 ✓、ttytest 21/21 ✓）。遗留问题集中在权限强制、O_EXCL/rename/fsync、信号阻塞投递与用户指针边界，多为 v1 已标记、v2 未动的 P0/P1 项。
