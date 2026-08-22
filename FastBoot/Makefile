# ================================================================
# FastBoot — UEFI/BIOS 双协议引导管理器（VBoot 抽象层）
#   make efi    → esp/EFI/BOOT/BOOTX64.EFI（UEFI 变体）
#   make bin    → build/fastboot.img（BIOS 变体：stage1+stage2+tkernel）
#   make run-efi / run-bin
# ================================================================
CC      := clang
LINK    := lld-link
NASM    := nasm
OVMF    := /usr/share/ovmf/OVMF.fd

# 构建修复：Deepin clang 17 在 x86_64-unknown-uefi 目标上崩溃（段错误，
# 连平凡文件都不行）；改用 x86_64-unknown-windows-msvc（PE/COFF + MS ABI，
# TinyOS 验证过的路径），再以 lld-link 链接成 EFI 应用
EFI_TARGET := x86_64-unknown-windows-msvc
EFI_FLAGS  := --target=$(EFI_TARGET) -ffreestanding -fno-stack-protector \
              -fno-strict-aliasing -mno-red-zone -fshort-wchar -Wall -Wextra -O2 \
              -I vboot -I manager
ESP_BIN := esp/EFI/BOOT/BOOTX64.EFI

all: efi bin

# ---- UEFI 变体 ----
$(ESP_BIN): vboot/vboot_uefi.c manager/menu.c vboot/vboot.h
	@mkdir -p esp/EFI/BOOT
	$(CC) $(EFI_FLAGS) -c vboot/vboot_uefi.c -o esp/vboot_uefi.o
	$(CC) $(EFI_FLAGS) -c manager/menu.c -o esp/menu.o
	$(LINK) /subsystem:efi_application /entry:efi_main /out:$@ \
	    esp/vboot_uefi.o esp/menu.o

efi: $(ESP_BIN)

# ---- BIOS 变体 ----
build/stage1.bin: boot/stage1.asm
	@mkdir -p build
	$(NASM) -f bin boot/stage1.asm -o $@
build/stage2.bin: boot/stage2.asm
	$(NASM) -f bin boot/stage2.asm -o $@
build/tkernel32.bin: boot/tkernel32.asm
	$(NASM) -f bin boot/tkernel32.asm -o $@
build/fastboot.img: build/stage1.bin build/stage2.bin build/tkernel32.bin
	cat build/stage1.bin build/stage2.bin build/tkernel32.bin > build/fastboot.raw
	dd if=build/fastboot.raw of=$@ bs=512 conv=sync status=none

bin: build/fastboot.img

# ---- 运行 ----
run-efi: efi
	qemu-system-x86_64 -bios $(OVMF) -drive file=fat:rw:esp,format=raw \
	    -display gtk -no-reboot

run-bin: bin
	qemu-system-i386 -fda build/fastboot.img -display gtk -no-reboot

# 无头验证（screendump 截屏）
test-efi: efi
	qemu-system-x86_64 -bios $(OVMF) -drive file=fat:rw:esp,format=raw \
	    -display none -monitor stdio -no-reboot
test-bin: bin
	qemu-system-i386 -fda build/fastboot.img -display none -monitor stdio -no-reboot

clean:
	rm -rf build esp/*.o esp/EFI

.PHONY: all efi bin run-efi run-bin test-efi test-bin clean
