# ================================================================
# Makefile — APinux OS 内核构建系统
# 目标：ARMv8-A (aarch64) / x86_64
# ================================================================
ARCH ?= aarch64
TARGET = apinux.bin

CROSS_COMPILE = aarch64-linux-gnu-
CC = $(CROSS_COMPILE)g++
AS = $(CROSS_COMPILE)as
LD = $(CROSS_COMPILE)ld
OBJCOPY = $(CROSS_COMPILE)objcopy

CFLAGS = -std=c++17 -O3 -ffreestanding -nostdlib -fno-exceptions -fno-rtti \
         -march=armv8-a -Wall -Wextra
LDFLAGS = -T linker.ld -nostdlib

OBJS = boot.o kernel.o drivers.o vfs.o ai_bridge.o ckr.o  browser.o\
       userlib.o config.o netstack.o theme.o widgets.o desktop.o wifi_app.o bluetooth_app.o \
       settings_app.o disk_manager.o file_manager.o usb_mouse_final.o main_gui.o
OBJS = ... rollback.o readonly.o

all: $(TARGET)

$(TARGET): $(OBJS)
	$(LD) $(LDFLAGS) -o $(TARGET) $(OBJS)
	$(OBJCOPY) -O binary $(TARGET) $(TARGET)

boot.o: boot.S
	$(CC) $(CFLAGS) -c -o $@ $<

kernel.o: kernel.cpp kernel.h
	$(CC) $(CFLAGS) -c -o $@ $<

drivers.o: drivers.cpp drivers.h
	$(CC) $(CFLAGS) -c -o $@ $<

vfs.o: vfs.cpp vfs.h
	$(CC) $(CFLAGS) -c -o $@ $<

gui.o: gui.cpp gui.h
	$(CC) $(CFLAGS) -c -o $@ $<

ai_bridge.o: ai_bridge.cpp ai_bridge.h
	$(CC) $(CFLAGS) -c -o $@ $<

ckr.o: ckr.cpp ckr.h
	$(CC) $(CFLAGS) -c -o $@ $<

userlib.o: userlib.cpp userlib.h
	$(CC) $(CFLAGS) -c -o $@ $<

config.o: config.cpp config.h
	$(CC) $(CFLAGS) -c -o $@ $<

netstack.o: netstack.cpp netstack.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)

run: $(TARGET)
	qemu-system-aarch64 -M virt -cpu cortex-a57 -m 512M -kernel $(TARGET) \
	    -device VGA,vgamem_mb=16 -serial stdio