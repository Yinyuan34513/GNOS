# ================================================================
# UEFIToolbox — UEFI GUI 工具箱（中文点阵字体）
#   gnu-efi 工具链：gcc -mabi=ms + gnu-efi crt0 + 链接脚本
#   → BOOTX64.EFI（带 .reloc 重定位，修复 lld-link 版启动崩溃）
#   make       → esp/EFI/BOOT/BOOTX64.EFI
#   make run   → QEMU OVMF GUI 窗口
#   make test  → 无头 + monitor（screendump 验证）
# ================================================================
CC     := gcc
LD     := ld
OBJCOPY := objcopy
EFI_LDSCRIPT := /usr/lib/elf_x86_64_efi.lds
EFI_CRT0    := /usr/lib/crt0-efi-x86_64.o
FLAGS  := -mabi=ms -ffreestanding -fno-stack-protector -fno-builtin \
          -fno-strict-aliasing -mno-red-zone -Wall -O2 -I include
ESP_BIN := esp/EFI/BOOT/BOOTX64.EFI

all: $(ESP_BIN)

$(ESP_BIN): toolbox.c include/ascii_font.h include/chinese_font.h
	@mkdir -p esp/EFI/BOOT build
	$(CC) $(FLAGS) -c toolbox.c -o build/toolbox.o
	$(LD) -nostdlib -shared -Bsymbolic -T $(EFI_LDSCRIPT) \
	    build/toolbox.o $(EFI_CRT0) -o build/toolbox.so
	$(OBJCOPY) -j .text -j .sdata -j .data -j .dynamic -j .dynsym \
	    -j .rel* -j .rela* -j .reloc --target efi-app-x86_64 \
	    build/toolbox.so $@

run: all
	qemu-system-x86_64 -bios /usr/share/ovmf/OVMF.fd \
	    -drive file=fat:rw:esp,format=raw -m 256M -display gtk -no-reboot

test: all
	qemu-system-x86_64 -bios /usr/share/ovmf/OVMF.fd \
	    -drive file=fat:rw:esp,format=raw -m 256M -display none -monitor stdio -no-reboot

clean:
	rm -rf build esp/EFI

.PHONY: all run test clean
