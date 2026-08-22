# racaOS vs GNOS 全方位激烈吐槽

## 基本盘

| | racaOS | GNOS |
|---|---|---|
| 语言 | Rust（`#![no_std]`） | C + 少量 asm |
| 架构 | LoongArch64 | x86_64 |
| 代码量 | ~8.5k 行（83 个 .rs） | ~215k 行 |
| 启动 | 自研 builder + Nix flake | Limine |
| 口号 | framekernel：宏内核速度、微内核安全、微内核灵活 | 把整个桌面塞进内核 |

## 完成度对比

| 能力 | racaOS | GNOS |
|---|---|---|
| 多任务 | ❌ roadmap 未勾 | ✅ proc.c 2127 行，含 SMP |
| 用户空间 | ❌ | ✅ musl 静态链接全套用户态 |
| 文件系统 | ❌（FAT/ext2/ext4 全在待办） | ✅ ext2(2164行) + FAT + procfs + tmpfs 挂载表 |
| 驱动 | framebuffer 一个 | ATA、e1000、AC97/HDA 音频、PCI、evdev input、DRM |
| 网络 | ❌ | ✅ net.c + e1000 网卡驱动 |
| 内核模块 | ✅ 动态加载/卸载（招牌菜） | ✅ module_elf.c 同样支持 ELF 内核模块 |
| 内存管理 | ✅ VMO/VMAR（对标 Zircon） | ✅ paging/pmm/heap，朴素但能用 |
| epoll | ❌ | ✅ epoll.c |
| 显示中文 | ❌ | ✅ cjkfont.bin 内置 CJK 字体 |

## racaOS 的亮点与暴击点

**亮点：**
- Asterinas 式组件化 + 运行时模块加载，架构野心是真的
- VMO/VMAR 直接对标 Fuchsia，设计审美在线
- Nix flake、cargo workspace、过程宏（zodiac_macro），工程仪式感拉满

**暴击点：**
- README 路线图自己承认：Multitask `[ ]`、User space `[ ]`
  —— **一个没有进程的操作系统**。它目前是一台"能分配内存的 LoongArch 单体程序"
- DRM/Xorg/Wayland 排在路线图最底部，而 GNOS 的 `build/desk/stage/bin`
  里已经躺着 labwc + XFCE4 全家桶 + dbus（90+ 个静态二进制）
- 8.5k 行里一大块是框架的框架：mostd、mostd-core、mostd-macros、
  stringmap、core-dylib、zodiac-dylib……用户态 std 还没影子，
  先给自己造了六层 crate 套娃
- modules/task/src/lib.rs 只有一个文件——任务模块是个空壳占位符

## GNOS 的亮点与暴击点

**亮点：**
- 215k 行全是能跑的东西：ext2 → epoll → evdev → DRM 一条龙
- 真实战果：musl 交叉链 + wayland/libinput/libxkbcommon/pixman/expat/
  libxml2/libffi 全套静态移植，最后跑起 XFCE4
- vfs.c 里 /dev/input/event0（键盘）、event1（鼠标）、/dev/dri/card0
  注册得明明白白；连假 libudev 的设备表都对着真内核节点写的

**暴击点：**
- 为了跑 XFCE 手写了假 libudev（设备表硬编码 card0/event0/event1），
  热插拔是一个"永远不会可读的 pipe"——自欺欺人指数五颗星
- libffi 编译踩了 `-isystem /usr/include/linux` 遮蔽 musl 头文件的坑，
  调了一下午才发现是 uapi 的 limits.h 抢了 musl 的位置
- meson 对 `-L dir` 分开写会拆参数的坑、wayland-scanner.pc 版本号要
  手动伪造 1.22.0 才让 meson 满意——GNOS 的构建史就是一部血泪史
- 215k 行 vs 8.5k 行：胜利是用"什么都自己写三遍"堆出来的，
  kstring.c 这种手搓字符串库在 Rust 眼里属于行为艺术

## 总结陈词

- **racaOS**：架构论文写得漂亮，操作系统还没出生。
  它赢在设计图，输在没有房子。
- **GNOS**：不讲架构美学，只讲"能不能跑 XFCE"。
  房子已经盖到精装修，只是水电（udev/热插拔）全是模拟器。

一句话：racaOS 是"未来的内核"，GNOS 是"过去的桌面"——
一个还没学会走路就开始设计跑鞋，另一个穿着草鞋已经跑完了马拉松。

## 公平裁决

- 论**工程完成度 / 可玩性**：GNOS 完胜，不在一个次元
- 论**架构前瞻性 / 类型安全**：racaOS 的模块系统与 VMO 设计更现代
- 但 racaOS 路线图上从"多任务"到"XFCE"之间隔着十几步没走完的台阶，
  而 GNOS 已经站在台阶顶上骂物业了

（数据来源：~/racaOS 克隆于 2026-08-21，GNOS src/ 本地统计）
