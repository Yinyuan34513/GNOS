# Makefile — build the GNOS 64-bit kernel and boot it with Limine in QEMU.
#
# Pipeline:
#   kernel/*.c    --(gcc -m64) --> GNOSKr.elf      (64-bit kernel, Limine entry)
#   user/*.c      --(gcc -m64) --> *.elf          (user programs, all @0x400000)
#   user programs --(mke2fs)   -> initrd.img      (ext2 image)
#   +limine bins  --(xorriso)  -> gnos.iso       (hybrid BIOS+UEFI CD)
#   gnos.iso     --(qemu)     runs the OS
#
# Every user program is linked at the same address on purpose: each process
# has its own page tables, so they never collide.

BUILD := build
OVMF  := /usr/share/ovmf/OVMF.fd

CC      := gcc
AS      := nasm
LD      := ld
OBJCOPY := objcopy

# Common freestanding flags.  -mgeneral-regs-only keeps gcc away from
# SSE/MMX/x87 registers: the CPU arrives from Limine with CR4.OSFXSR clear,
# so any xmm instruction would raise #UD (and, with no IDT yet, triple-fault).
# -mno-red-zone is mandatory once interrupts can land on the kernel stack.
BASEFLAGS := -m64 -ffreestanding -nostdlib -fno-stack-protector -fno-builtin \
             -nostdinc -std=gnu11 -mno-red-zone -mgeneral-regs-only \
             -mno-sse -mno-sse2 -mno-mmx -mno-80387 -fvisibility=hidden \
             -Wall -Wextra -O2 -g -Isrc/include -Isrc/shared -Isrc/kernel

# kernel: PIE so Limine can relocate it into the higher half
KCFLAGS := $(BASEFLAGS) -fpie
# user programs: linked at a fixed address by src/user/user.ld
UCFLAGS := $(BASEFLAGS) -Isrc/user -fno-pie -fno-pic

# Limine binaries (copied from a local Limine install)
LIMINE_BIOS := limine/limine-bios-cd.bin
LIMINE_UEFI := limine/limine-uefi-cd.bin

# ---- artifacts ----
KRNL    := $(BUILD)/GNOSKr.elf
INITRD  := $(BUILD)/initrd.img
ISO     := $(BUILD)/gnos.iso
ISO_ROOT := $(BUILD)/iso

KOBJS := $(BUILD)/kernel.o $(BUILD)/loader.o $(BUILD)/fbcon.o $(BUILD)/gfx.o \
         $(BUILD)/fbdev.o $(BUILD)/subsys.o $(BUILD)/acpi.o \
         $(BUILD)/debugcon.o $(BUILD)/ext2.o $(BUILD)/panic.o \
         $(BUILD)/gdt.o $(BUILD)/idt.o $(BUILD)/isr.o \
         $(BUILD)/kstring.o $(BUILD)/vfs.o $(BUILD)/procfs.o $(BUILD)/tmpfs.o $(BUILD)/tty.o $(BUILD)/heap.o \
         $(BUILD)/pmm.o $(BUILD)/vmm.o $(BUILD)/proc.o \
         $(BUILD)/signal.o $(BUILD)/switch.o $(BUILD)/timer.o \
         $(BUILD)/syscall.o \
         $(BUILD)/net.o $(BUILD)/tcp.o $(BUILD)/sock.o \
         $(BUILD)/pci.o $(BUILD)/e1000.o $(BUILD)/audio.o \
         $(BUILD)/hda.o \
         $(BUILD)/limine_requests.o

# User programs: name -> build/<name>.elf, all linked from crt0 + ulib.
UPROGS  := init shell count ls cat tail tac rm mkdir touch
UELFS   := $(addprefix $(BUILD)/,$(addsuffix .elf,$(UPROGS)))
UCRT    := $(BUILD)/user/crt0.o $(BUILD)/user/ulib.o

# musl (built by hand into build/muslsrc/musl) provides the C runtime for
# programs that want a real libc.  They are linked non-PIE at 0x400000 like
# every other user program, but from musl's crt1.o + libc.a instead of ulib.
MUSL_SRC    := $(BUILD)/muslsrc/musl-1.2.5
MUSL_PREFIX := $(BUILD)/muslsrc/musl
MUSL_LIB  := $(MUSL_PREFIX)/lib
MUSL_CRT  := $(MUSL_PREFIX)/lib
MUSL_INC  := $(MUSL_PREFIX)/include
MUSL_GCC  := $(MUSL_PREFIX)/bin/musl-gcc

# Programs built against musl rather than ulib.
MUSLPROGS := hello ttytest sigtest readlinetest fstest mounttest mount fbtest coldplug
MUSL_OBJS := $(addprefix $(BUILD)/user/,$(addsuffix .o,$(MUSLPROGS)))
MUSL_ELFS := $(addprefix $(BUILD)/,$(addsuffix .elf,$(MUSLPROGS)))

# BusyBox: the first piece of real third-party userland.  Its source tree was
# fetched by hand into build/bbsrc and configured from allnoconfig with only
# the applets below turned on -- no shell yet: the toy shell still drives
# /etc/rc, and ash wants job control features the kernel is only now growing.
# Statically linked against musl, so the kernel only has to load ET_EXEC.
BB_SRC     := $(BUILD)/bbsrc/busybox-1.38.0
BB_BIN     := $(BB_SRC)/busybox
# Network applets (ifconfig/ping/wget/nc/route/udhcpc/hostname/getent) are
# turned on in $(BB_SRC)/.config and copied into /usr/bin below, so they show
# up under their own names and /bin/busybox.elf still works as the multicall
# dispatcher.  ping needs a raw socket and runs as root here, so it works
# without any setuid shimming.
BB_APPLETS := cat cp echo false head ls mkdir mv pwd rm true uname wc sh ash \
              ifconfig ping wget nc route hostname \
              env expr sleep test sort tr sed grep dirname basename \
              mktemp seq stat chmod ln readlink find date id kill ps printf

# GNU Bash — the shell this whole userland effort is aimed at.
#
# Configured by hand in the tree (it takes seconds; the recipe below only
# relinks) with:
#
#   ./configure CC=<musl-gcc> CFLAGS="-O2 -g -static -no-pie -fno-pie" \
#               LDFLAGS="-static -no-pie" --without-bash-malloc \
#               --disable-nls --enable-readline bash_cv_termcap_lib=gnutermcap
#
# Each of those matters.  -static because there is no dynamic loader and
# -no-pie because loader.c only accepts ET_EXEC.  --without-bash-malloc
# drops bash's own sbrk-based allocator in favour of musl's, which is the
# one this kernel's brk/mmap behaviour has actually been tested against.
# bash_cv_termcap_lib=gnutermcap picks the termcap bundled in lib/termcap
# rather than the host's ncurses, which musl-gcc cannot link against.
#
# Note there is no --host: musl-gcc produces binaries that run natively on
# the build machine, so configure's AC_TRY_RUN probes execute for real
# instead of falling back to cross-compilation guesses.
BASH_SRC := $(BUILD)/bashsrc/bash-5.3
BASH_BIN := $(BASH_SRC)/bash

# musl programs see musl's headers and nothing else.  Two things matter here:
#   -nostdinc stays (BASEFLAGS already has it) so glibc's /usr/include cannot
#   leak in, and -Isrc/include / -Isrc/kernel are dropped because they shadow
#   musl's <stdint.h>, <stddef.h> and <syscall.h> -- plain -I beats -isystem.
# -Isrc/shared survives: sysnum.h and bootinfo.h collide with nothing.
#
# The SSE bans in BASEFLAGS come off too.  They exist so the *kernel* never
# touches xmm registers (it does not save its own FPU state across interrupts),
# but libc.a is compiled with SSE enabled: a caller built with -mno-sse hands
# doubles over under a different convention than printf expects and the value
# silently reads back as zero.  User mode is safe here -- vmm.c sets
# CR4.OSFXSR and proc.c fxsaves/fxrstors per process.
MUSLCFLAGS := $(filter-out -Isrc/include -Isrc/kernel -mgeneral-regs-only \
                           -mno-sse -mno-sse2 -mno-mmx -mno-80387,$(BASEFLAGS)) \
              -isystem $(MUSL_INC) -Isrc/user -fno-pie -fno-pic

# Hardware handed to the guest beyond the PC platform minimum.  The e1000 is
# the NIC src/kernel/e1000.c drives; the AC97 is the codec src/kernel/audio.c
# drives.  `audiodev none` gives the codec a backend that consumes samples in
# real time without needing a sound card on the host -- which is exactly what
# the headless self-test wants: the DMA engine really runs, nobody hears it.
# `-nic none` is not redundant: without it QEMU also creates its *default*
# NIC, the guest sees two e1000s, and the driver binds to whichever it met
# first -- which is not the one attached to our netdev.
#
# Both sound cards are plugged in at once on purpose: they are two completely
# different programming models (see hda.h) and the kernel drives both, so
# leaving one out would mean half the audio code never runs in `make test`.
QEMU_NET   := -nic none -netdev user,id=net0 -device e1000,netdev=net0
QEMU_AUDIO := -audiodev none,id=snd0 \
              -device AC97,audiodev=snd0 \
              -device intel-hda -device hda-duplex,audiodev=snd0
QEMU_DEVICES := $(QEMU_NET) $(QEMU_AUDIO)

# The same hardware, but with a backend you can actually hear.  Override on
# the command line if PulseAudio is not what your desktop runs, e.g.
#   make guistart AUDIO_BACKEND=pipewire
#   make guistart AUDIO_BACKEND=alsa
AUDIO_BACKEND ?= pa
GUI_AUDIO := -audiodev $(AUDIO_BACKEND),id=snd0 \
             -device AC97,audiodev=snd0 \
             -device intel-hda -device hda-duplex,audiodev=snd0

.PHONY: all run run-uefi guistart test clean distclean
all: $(ISO)

# musl's headers only become usable after `make install` assembles them into a
# sysroot: bits/alltypes.h is generated by configure and lives in obj/include,
# so the raw source tree's include/ directory is incomplete on its own.  This
# rule has to sit below `all` -- make takes the first target in the file as the
# default goal.
$(MUSL_LIB)/libc.a $(MUSL_GCC):
	$(MAKE) -C $(MUSL_SRC) install

# BusyBox builds itself; this rule only fires when the binary is missing, so
# reconfiguring the tree by hand (make menuconfig in $(BB_SRC)) still works.
# AR=gcc-ar is not cosmetic: binutils 2.41's plain `ar` segfaults while
# auto-loading the LTO plugin on this host, and BusyBox archives every
# subdirectory into a .a before the final link.
$(BB_BIN): $(BB_SRC)/.config | $(MUSL_GCC)
	$(MAKE) -C $(BB_SRC) CC=$(abspath $(MUSL_GCC)) HOSTCC=gcc AR=gcc-ar \
	  SKIP_STRIP=y

# GNU Bash.  Like BusyBox, the tree is fetched and configured by hand (see
# the BASH_SRC comment above) and this rule only relinks it when the binary
# is missing, so a hand-run `make` inside the tree is never undone.
$(BASH_BIN): $(BASH_SRC)/Makefile | $(MUSL_GCC)
	$(MAKE) -C $(BASH_SRC)

# Both directories are listed separately: `clean` leaves $(BUILD) standing (the
# third-party trees live there), so a rule keyed only on $(BUILD) would never
# fire again and $(BUILD)/user would stay missing.
$(BUILD) $(BUILD)/user:
	mkdir -p $@

# ---------- header dependencies ----------
# Without this every .o depends only on its .c, so editing a header rebuilds
# nothing that includes it.  That is not merely a stale-build annoyance here:
# proc.h defines proc_t, and half the kernel indexes into it, so a field added
# to that struct and recompiled into only one object gives two translation
# units two different memory layouts.  The result is a kernel that links
# cleanly and then corrupts user processes -- which is exactly how the last
# `-MMD`-less afternoon was spent.  -MP adds a phony target per header so a
# deleted or renamed header does not wedge make with "no rule to make target".
DEPFLAGS = -MMD -MP
DEPS := $(KOBJS:.o=.d) $(UOBJS:.o=.d) $(MUSL_OBJS:.o=.d) $(UCRT:.o=.d) \
        $(addprefix $(BUILD)/user/,$(addsuffix .d,$(UPROGS)))
-include $(DEPS)

# ---------- kernel (Limine entry point) ----------
$(BUILD)/%.o: src/kernel/%.c | $(BUILD)
	$(CC) $(KCFLAGS) $(DEPFLAGS) -c -o $@ $<

$(BUILD)/%.o: src/kernel/%.asm | $(BUILD)
	$(AS) -f elf64 -o $@ $<

$(KRNL): $(KOBJS) linker.ld
	$(CC) $(KCFLAGS) -static-pie -Wl,-T,linker.ld -Wl,-z,max-page-size=0x1000 \
	  -Wl,--build-id=none -o $@ $(KOBJS)

# ---------- user programs ----------
$(BUILD)/user/%.o: src/user/%.c | $(BUILD)/user
	$(CC) $(UCFLAGS) $(DEPFLAGS) -c -o $@ $<

# musl programs compile against musl's headers (see MUSLCFLAGS).  These are
# static pattern rules, which beat the generic ulib ones above for exactly the
# targets named in MUSL_OBJS / MUSL_ELFS and leave every other program alone.
$(MUSL_OBJS): $(BUILD)/user/%.o: src/user/%.c $(MUSL_LIB)/libc.a | $(BUILD)/user
	$(CC) $(MUSLCFLAGS) $(DEPFLAGS) -c -o $@ $<

$(BUILD)/user/%.o: src/user/%.asm | $(BUILD)/user
	$(AS) -f elf64 -o $@ $<

$(BUILD)/%.elf: $(BUILD)/user/%.o $(UCRT) src/user/user.ld
	$(LD) -m elf_x86_64 -T src/user/user.ld -z max-page-size=0x1000 \
	  --build-id=none -o $@ $(UCRT) $<

# musl-linked program: crt1.o, libc.a, and the .init/.fini glue from crti/crtn.
# Order matters: crt1.o references __libc_start_main (in libc.a, so it comes
# after), and the program object references printf (also in libc.a).  crtn.o
# closes the .init/.fini sections opened by crti.o, so it is last.
$(MUSL_ELFS): $(BUILD)/%.elf: $(BUILD)/user/%.o \
              $(MUSL_CRT)/crt1.o $(MUSL_CRT)/crti.o $(MUSL_CRT)/crtn.o \
              $(MUSL_LIB)/libc.a src/user/user.ld
	$(LD) -m elf_x86_64 -T src/user/user.ld -z max-page-size=0x1000 \
	  --build-id=none -static -nostdlib -o $@ \
	  $(MUSL_CRT)/crt1.o $(MUSL_CRT)/crti.o $< \
	  $(MUSL_LIB)/libc.a $(MUSL_CRT)/crtn.o

# ---------- initrd (ext2 image holding the user programs) ----------
# mke2fs -d fills the image straight from a staging directory, so this needs
# neither a loopback mount nor root.  The feature set is trimmed on purpose:
# ^dir_index keeps every directory a plain linear list, which is all the
# kernel driver knows how to rewrite.
$(INITRD): $(UELFS) $(MUSL_ELFS) $(BB_BIN) $(BASH_BIN) src/user/rc | $(BUILD)
	rm -rf $(BUILD)/initrd-root
	mkdir -p $(BUILD)/initrd-root
	# ---- FHS skeleton (empty dirs are harmless placeholders for now) ----
	mkdir -p $(BUILD)/initrd-root/bin
	mkdir -p $(BUILD)/initrd-root/sbin
	mkdir -p $(BUILD)/initrd-root/etc
	mkdir -p $(BUILD)/initrd-root/dev
	mkdir -p $(BUILD)/initrd-root/proc
	mkdir -p $(BUILD)/initrd-root/sys
	mkdir -p $(BUILD)/initrd-root/tmp
	mkdir -p $(BUILD)/initrd-root/var/run
	mkdir -p $(BUILD)/initrd-root/usr/bin
	mkdir -p $(BUILD)/initrd-root/usr/sbin
	mkdir -p $(BUILD)/initrd-root/usr/lib
	mkdir -p $(BUILD)/initrd-root/lib
	mkdir -p $(BUILD)/initrd-root/root
	mkdir -p $(BUILD)/initrd-root/home
	mkdir -p $(BUILD)/initrd-root/mnt
	mkdir -p $(BUILD)/initrd-root/run
	# ---- user programs live in /bin ----
	for p in $(UPROGS); do \
	  cp $(BUILD)/$$p.elf $(BUILD)/initrd-root/bin/$$p.elf; \
	done
	cp $(BUILD)/ls.elf $(BUILD)/initrd-root/bin/dir.elf   # "dir" is another name for ls
	cp $(BUILD)/init.elf $(BUILD)/initrd-root/init.elf    # kernel loads /init.elf at root
	# ---- musl-linked programs also live in /bin ----
	for p in $(MUSLPROGS); do \
	  cp $(BUILD)/$$p.elf $(BUILD)/initrd-root/bin/$$p.elf; \
	done
	# `mount` is invoked by its bare name from OpenRC's init.sh and service
	# scripts, so it must sit on PATH as /bin/mount (not /bin/mount.elf).  The
	# rest of the musl programs are only ever called by absolute path.
	cp $(BUILD)/mount.elf $(BUILD)/initrd-root/bin/mount
	# ---- BusyBox: the multi-call binary, plus one file per applet ----
	# BusyBox picks its applet from basename(argv[0]) -- names that start with
	# "busybox" fall through to the multi-call dispatcher instead -- so every
	# applet needs a file of its own.  They are copies, not hard links: the
	# ext2 driver has never been run against an inode with nlink > 1.
	# /usr/bin keeps them out of the way of the toy ls/cat/rm in /bin, which
	# PATH finds first.
	cp $(BB_BIN) $(BUILD)/initrd-root/bin/busybox.elf
	for a in $(BB_APPLETS); do \
	  cp $(BB_BIN) $(BUILD)/initrd-root/usr/bin/$$a; \
	done
	# /bin/sh and /bin/ash are busybox multi-call names: invoked as "sh"
	# (or "ash") busybox dispatches to its ash applet, which is the system
	# shell.  They live in /bin so #!/bin/sh shebangs and the init PATH find
	# them.
	cp $(BB_BIN) $(BUILD)/initrd-root/bin/sh
	cp $(BB_BIN) $(BUILD)/initrd-root/bin/ash
	# ---- GNU Bash ----
	# Stripped on the way in: the unstripped binary is 4.4 MB of mostly
	# DWARF, and every byte of it would be read off the initrd at exec time.
	cp $(BASH_BIN) $(BUILD)/initrd-root/bin/bash
	strip $(BUILD)/initrd-root/bin/bash
	cp src/user/rc $(BUILD)/initrd-root/etc/rc            # run once at boot by init
	# Static system config (hosts, resolv.conf, nsswitch, services, protocols,
	# passwd/group, hostname).  These make the BusyBox network tools and the
	# C resolver actually work: ping/wget do DNS via /etc/resolv.conf, getent
	# reads /etc/passwd, and `hostname` uses /etc/hostname.
	cp -a src/rootfs/etc/. $(BUILD)/initrd-root/etc/
	# ---- OpenRC 0.56 tree ------------------------------------------------
	# The full install (built separately with meson + musl, see
	# build/orcsrc/) drops in here: /sbin/openrc (+ openrc-run, rc-status,
	# start-stop-daemon, ...), /etc/init.d/*, /etc/runlevels/*, /etc/conf.d/*,
	# /etc/rc.conf, and /usr/libexec/rc/{bin,sbin,sh}.  It is what the kernel's
	# mount/getrandom/symlink/rename support exists to serve, so it rides along
	# in the initrd and /etc/rc brings its runlevels up at boot.
	mkdir -p $(BUILD)/initrd-root/dev/shm $(BUILD)/initrd-root/run/lock
	cp -a build/orcsrc/openrc-install/bin/.    $(BUILD)/initrd-root/bin/    2>/dev/null || true
	cp -a build/orcsrc/openrc-install/sbin/.   $(BUILD)/initrd-root/sbin/
	cp -a build/orcsrc/openrc-install/etc/.    $(BUILD)/initrd-root/etc/
	cp -a build/orcsrc/openrc-install/usr/.    $(BUILD)/initrd-root/usr/
	# devfs would mount a tmpfs over /dev and hide the static character
	# devices the kernel already provides (null, tty, ...); drop it from the
	# sysinit runlevel so the rest of OpenRC can run headless.
	rm -f $(BUILD)/initrd-root/etc/runlevels/sysinit/devfs
	dd if=/dev/zero of=$@ bs=1M count=64 2>/dev/null
	mke2fs -q -t ext2 -b 1024 -I 256 \
	       -O ^resize_inode,^dir_index,^ext_attr \
	       -d $(BUILD)/initrd-root -F $@

# ---------- Limine hybrid ISO ----------
$(ISO): $(KRNL) $(INITRD) limine.conf $(LIMINE_BIOS) $(LIMINE_UEFI) | $(BUILD)
	mkdir -p $(ISO_ROOT)
	cp $(KRNL)      $(ISO_ROOT)/GNOSKr.elf
	cp $(INITRD)    $(ISO_ROOT)/initrd.img
	cp limine.conf  $(ISO_ROOT)/limine.conf
	cp $(LIMINE_BIOS) $(ISO_ROOT)/limine-bios-cd.bin
	cp $(LIMINE_UEFI) $(ISO_ROOT)/limine-uefi-cd.bin
	cp limine/limine-bios.sys $(ISO_ROOT)/limine-bios.sys
	xorriso -as mkisofs -b limine-bios-cd.bin -no-emul-boot \
	  -boot-load-size 4 -boot-info-table \
	  --efi-boot limine-uefi-cd.bin -efi-boot-part --efi-boot-image \
	  -r -J -o $@ $(ISO_ROOT)

# ---------- run / test ----------
run: $(ISO)
	qemu-system-x86_64 -cdrom $(ISO) -m 512M -display gtk $(QEMU_DEVICES)

run-uefi: $(ISO)
	qemu-system-x86_64 -cdrom $(ISO) -m 512M -bios $(OVMF) -display gtk \
	  $(QEMU_DEVICES)

# guistart — the "just show me the OS" target.
#
# Differences from `run`, all of them about being in front of a human:
#   - the audio backend is a real one, so both self-test tones (AC97 first,
#     then HDA) are audible;
#   - the debug console is teed to build/dbg.log as well, so the boot messages
#     that scroll past the framebuffer are still there afterwards;
#   - -no-reboot turns a triple fault into a stopped VM you can look at
#     instead of an endless reboot loop.
# The ISO is a prerequisite, so this rebuilds anything stale first.
guistart: $(ISO)
	@echo "GNOS: booting in a window (audio backend: $(AUDIO_BACKEND));"
	@echo "      boot log is also being written to $(BUILD)/dbg.log"
	qemu-system-x86_64 -cdrom $(ISO) -m 512M \
	  $(QEMU_NET) $(GUI_AUDIO) \
	  -device isa-debugcon,chardev=dbg -chardev file,id=dbg,path=$(BUILD)/dbg.log \
	  -display gtk -no-reboot

test: $(ISO)
	timeout 20 qemu-system-x86_64 -cdrom $(ISO) -m 512M $(QEMU_DEVICES) \
	  -device isa-debugcon,chardev=dbg -chardev file,id=dbg,path=$(BUILD)/dbg.log \
	  -serial none -display none -no-reboot || true
	@echo "----- debugcon log -----"
	@cat $(BUILD)/dbg.log

# `clean` deliberately spares the third-party source trees under $(BUILD).
# musl was fetched and built by hand and there is no rule to get it back, so a
# plain `rm -rf build` would destroy the toolchain irrecoverably.  Everything
# this Makefile knows how to rebuild is listed explicitly instead.
THIRD_PARTY := $(BUILD)/muslsrc $(BUILD)/bbsrc

clean:
	rm -rf $(BUILD)/user $(BUILD)/initrd-root $(ISO_ROOT)
	rm -f  $(BUILD)/*.o $(BUILD)/*.elf $(BUILD)/*.img $(BUILD)/*.iso \
	       $(BUILD)/*.log

# Nuke everything, third-party trees included.  Only useful if you are prepared
# to re-fetch musl by hand -- see THIRD_PARTY above.
distclean:
	rm -rf $(BUILD)
