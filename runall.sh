#!/bin/bash
# ================================================================
# runall.sh — 一键后台 QEMU GTK 运行 5 个 OS
#   GNOS / 32os / APINUX / FastOS / MWOS（TinyOS 构建成功则一并）
# 每个 OS 后台 nohup 运行，日志 /tmp/qemu-<name>.log，PID 打印。
# ================================================================
set -u
cd "$(dirname "$0")"
pids=()

launch() {   # launch <name> <cmd...>
    local name="$1"; shift
    echo "[$name] launching: $*"
    nohup "$@" >"/tmp/qemu-$name.log" 2>&1 &
    pids+=("$!")
    echo "[$name] PID $!"
}

# 1) GNOS（rc 已禁用 desktest/labwc）
[ -f build/gnos.iso ] && \
    launch gnos qemu-system-x86_64 -cdrom build/gnos.iso -m 512M -display gtk -no-reboot

# 2) 32os（v1.5：VBE+GEM+32API+32FS+ring3+USB鼠标）
[ -f 32os/build/32os.iso ] && \
    launch 32os qemu-system-i386 -cdrom 32os/build/32os.iso -m 32M \
           -usb -device usb-mouse -display gtk -no-reboot

# 3) APINUX 2.0 X86（Multiboot v1 + PVH）
APIBIN="APINUX-archive/APINUX OS等106个文件/APinux OS 2.0 X86发行版/apinux.bin"
[ -f "$APIBIN" ] && \
    launch apinux qemu-system-x86_64 -kernel "$APIBIN" -m 128M -vga std -display gtk -no-reboot

# 4) FastOS（现场汇编 512B 引导扇区）
[ -f /tmp/fastos.bin ] || nasm -f bin FastOS/boot.asm -o /tmp/fastos.bin 2>/dev/null
[ -f /tmp/fastos.bin ] && \
    launch fastos qemu-system-i386 -fda /tmp/fastos.bin -display gtk -no-reboot

# 5) MWOS（构建：llvm-objcopy 缺失时用 binutils objcopy）
[ -f MWOS/build/disk.img ] || (cd MWOS && make clean >/dev/null 2>&1; \
    make OBJCOPY=objcopy >/tmp/mwos-build.log 2>&1)
[ -f MWOS/build/disk.img ] && \
    launch mwos qemu-system-x86_64 -bios /usr/share/ovmf/OVMF.fd \
           -drive file=MWOS/build/disk.img,format=raw -display gtk -no-reboot

# 6) TinyOS（构建尝试；成功则 UEFI 运行）
[ -f TinyOS-V1/esp_temp/EFI/BOOT/BOOTX64.EFI ] || (cd TinyOS-V1 && make >/tmp/tinyos-build.log 2>&1)
[ -f TinyOS-V1/esp_temp/EFI/BOOT/BOOTX64.EFI ] && \
    launch tinyos qemu-system-x86_64 -bios /usr/share/ovmf/OVMF.fd \
           -drive file=fat:rw:TinyOS-V1/esp_temp,format=raw -display gtk -no-reboot

echo "=== 已启动 ${#pids[@]} 个 OS ==="
sleep 2
echo "--- 存活 qemu 进程 ---"
ps aux | grep qemu-system | grep -v grep | awk '{print $2, $11, $12}' || true
