# ================================================================
# Makefile — APinux OS x86_64 内核构建系统（构建修复版）
#
# 修复内容：
#   1. 删除 `OBJS = ... rollback.o readonly.o` 覆盖 bug（原文件第二行
#      把真实对象列表覆盖成字面量 "..."，导致 make 报
#      "没有规则可制作目标 '...'"）
#   2. 工具链改为宿主 x86_64（g++/as/ld/objcopy），不再需要
#      aarch64-linux-gnu- 交叉编译器（本机未安装）
#   3. OBJS 只包含仓库里真实存在的源文件；AI 引擎（ACPU/ACPNN/OPEN PUFF/
#      puff_cpu_optimized/A2 PPS）是宿主库（iostream/thread/OpenMP），
#      不属内核代码，已排除；GUI（gui_fb/main_gui/desktop/theme/widgets/
#      wifi_app/bluetooth_app/settings_app/disk_manager/file_manager/
#      usb_mouse_final/browser）已整体移除，内核走纯控制台模式
#   4. run 目标用 apinux.bin（Multiboot v1 raw 镜像；boot_x86.S 的
#      flags 置 HAS_ADDR(bit16)，QEMU 走 raw 地址字段加载路径）
# ================================================================
TARGET = apinux.bin
ELF    = apinux.elf

CC      := g++
AS      := as
LD      := ld
OBJCOPY := objcopy

CFLAGS  := -std=c++17 -O2 -ffreestanding -nostdlib -fno-exceptions -fno-rtti \
           -fno-stack-protector -mno-red-zone -mno-sse -mno-sse2 -mno-mmx \
           -Wall -Wextra -I.
ASFLAGS := --64
LDFLAGS := -T linker.ld -nostdlib -static

OBJS := boot_x86.o kernel.o userlib.o libc_support.o drivers.o vfs.o \
        console.o shell.o kbd.o ata.o

all: $(TARGET)

$(TARGET): $(ELF)
	$(OBJCOPY) -O binary $< $@

$(ELF): $(OBJS)
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

%.o: %.S
	$(AS) $(ASFLAGS) -o $@ $<

%.o: %.cpp
	$(CC) $(CFLAGS) -c -o $@ $<

run: $(TARGET)
	qemu-system-x86_64 -kernel $(TARGET) -m 128M -serial stdio \
	    -vga std -display gtk -no-reboot

clean:
	rm -f $(OBJS) $(ELF) $(TARGET)

.PHONY: all run clean
