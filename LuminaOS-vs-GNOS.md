# LuminaOS vs GNOS 全方位激烈吐槽

## 基本盘

| | LuminaOS | GNOS |
|---|---|---|
| 语言 | C + NASM | C + GAS asm |
| 架构 | i386（32 位保护模式） | x86_64（64 位 + SMP） |
| 代码量 | **4,696 行**（不含 linux-src 参考树） | **56k 自研行**（另有 159k third_party 移植） |
| 启动 | 自写 multiboot boot.asm（173 行） | Limine 协议 |
| 构建 | Windows PowerShell（build.ps1）+ i686-elf 工具链 | bash + musl-gcc 交叉链 + meson/autotools 混战 |
| 定位 | 教科书级 hobby OS：从零到"有窗口的桌面" | 从零到"跑 XFCE 的桌面系统" |

## 完成度对比

| 能力 | LuminaOS | GNOS |
|---|---|---|
| 多任务 | ✅ sched.c —— **58 行** | ✅ proc.c 2127 行，SMP 唤醒 |
| 用户空间 | ✅ usermode.asm + syscall.asm，LSP 自定义格式 | ✅ 完整 ELF + musl 动态/静态链接 |
| 文件系统 | FAT16（654 行，IDE 盘） | ext2(2164 行) + FAT + procfs + tmpfs 挂载表 |
| 显示 | VGA 直接画窗口，desktop.c **272 行**桌面环境 | fbdev/DRM 双通道，CJK 字体渲染 |
| 输入 | PS/2 键盘鼠标 | evdev（event0/event1）+ libinput 移植 |
| 网络 | ❌ | e1000 + TCP/IP 栈 |
| 音频 | ❌ | AC97 + HDA |
| 应用 | 记事本、文件管理器、关于、系统信息（内置内核） | shell(906 行) + XFCE4 全家桶静态移植 |
| 内核模块 | ❌ | ✅ ELF 内核模块动态加载 |

## LuminaOS 的亮点与暴击点

**亮点：**
- 麻雀虽小五脏俱全：bootloader → pmm → paging → GDT/IDT/IRQ → 调度器
  → FAT16 → syscall → 窗口桌面，一条完整的教学闭环
- 不依赖任何现成组件，连应用格式（LSP）都是自己定的——纯粹的 osdev 精神
- 272 行写出一个带窗口的桌面，性价比之王；GNOS 光 libudev stub 都快 500 行了

**暴击点：**
- 58 行的调度器和 2127 行的 proc.c 放在一起，像纸飞机停在机库旁边
- 所有应用（记事本/文件管理器）编译进**内核镜像**——"用户态程序"
  和内核一个地址空间，syscall 形同虚设，微内核看了会晕、宏内核看了会沉默
- README 要求 Windows + PowerShell 构建，2026 年了还在用软盘镜像思维
- 目录里躺着一份 linux-src/linux-7.1.8……**在旁边放整个 Linux 源码树**
  当参考，4.7k 行对 4000 万行，勇气可嘉

## GNOS 的亮点与暴击点

**亮点：**
- 56k 行全是真家伙：SMP、epoll、双文件系统、DRM、网卡、声卡、ELF 模块
- 用户态是完整的 musl 静态世界：wayland/libinput/XFCE/labwc/dbus 都搬上来了
- vfs.c 的 /dev 设备注册与真实驱动一一对应，不是摆设

**暴击点：**
- 为了跑桌面手写假 libudev，设备表硬编码三个节点，热插拔是根死管道
- 构建血泪史：`-isystem /usr/include/linux` 遮蔽 musl 头调一下午、
  meson 把 `-L dir` 拆参数、wayland-scanner.pc 版本号要手动伪造
- 4.7k 行能画出窗口桌面 vs GNOS 搬运 159k 第三方代码才见到 labwc——
  重型火炮打蚊子，蚊子还经常没打着

## 总结陈词

- **LuminaOS** 是 osdev 教程的满分毕业作品：
  小而完整，但天花板就是"内核里画窗口"，58 行调度器撑不起更多野心。
- **GNOS** 是不讲武德的工程怪兽：
  单挑所有现代软件栈，把 2010 年代的 Linux 桌面硬塞进自己的内核，
  过程惨烈（见构建血泪史），但确实塞进去了。

一句话：LuminaOS 用 4.7k 行证明了"我懂操作系统原理"，
GNOS 用 56k 行证明了"我不光懂，我还真把它造出来了"。

## 公平裁决

- 论**教学价值 / 可读性**：LuminaOS 胜——一个周末能读完
- 论**工程完成度 / 生态兼容**：GNOS 碾压——POSIX 子系统 + 真实驱动
- 论**单位代码生产力**：LuminaOS 每行代码的功能密度更高，
  但那是因为它根本没碰 GNOS 在踩的那些坑（musl、meson、XFCE）

（数据来源：/persistent/home/elaina/LuminaOS 本地统计，2026-08-21）
