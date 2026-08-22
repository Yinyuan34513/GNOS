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

# The system compiler for the userland programs.  Must be gcc-13: the
# stock gcc 12.3 on this machine has a libcpp ICE (_cpp_process_line_notes,
# libcpp/lex.cc:1163) that randomly kills preprocessing of perfectly
# ordinary files (getty.c, ncurses's make_hash.c, ...).
CC      := gcc-13
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
             -Wall -Wextra -O2 -g -Isrc/include -Isrc/shared \
             -Isrc/kernel/core -Isrc/kernel/driver

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
         $(BUILD)/fbdev.o $(BUILD)/drm.o $(BUILD)/subsys.o $(BUILD)/acpi.o \
         $(BUILD)/debugcon.o $(BUILD)/ext2.o $(BUILD)/panic.o \
         $(BUILD)/gdt.o $(BUILD)/idt.o $(BUILD)/isr.o \
         $(BUILD)/kstring.o $(BUILD)/vfs.o $(BUILD)/procfs.o $(BUILD)/tmpfs.o $(BUILD)/tty.o $(BUILD)/heap.o \
         $(BUILD)/pmm.o $(BUILD)/vmm.o $(BUILD)/proc.o $(BUILD)/ptrace.o \
        $(BUILD)/signal.o $(BUILD)/switch.o $(BUILD)/timer.o \
        $(BUILD)/syscall.o \
        $(BUILD)/smp.o $(BUILD)/ap_trampoline.o \
        $(BUILD)/lapic.o \
        $(BUILD)/net.o $(BUILD)/tcp.o $(BUILD)/sock.o \
         $(BUILD)/pci.o $(BUILD)/e1000.o $(BUILD)/audio.o \
        $(BUILD)/hda.o $(BUILD)/ata.o $(BUILD)/cjkfont.o \
        $(BUILD)/cjkfont_data.o \
        $(BUILD)/input.o \
        $(BUILD)/anonfd.o $(BUILD)/epoll.o $(BUILD)/timerfd.o \
        $(BUILD)/unix.o \
        $(BUILD)/module.o $(BUILD)/module_elf.o $(BUILD)/exports.o \
        $(BUILD)/limine_requests.o

# User programs: name -> build/<name>.elf, all linked from crt0 + ulib.
UPROGS  := init shell count ls cat tail tac rm mkdir touch scan dbgcat
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
MUSLPROGS := hello mount coldplug chvt getty login installer ttytest thrtest drmtest ptracetest insmod rmmod evtest eventest socktest
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

# curl — the network client, built against musl and statically linked.
# Fetched into build/curlsrc and configured by hand with every optional
# dependency disabled (no TLS: the kernel has no crypto, and GNOS's network
# is HTTP to the QEMU slirp gateway).  Two things matter at link time:
#   - `-static` in LDFLAGS is swallowed by libtool as one of its own mode
#     flags (the same trap binutils fell into), so the binary comes out
#     dynamically linked against libc and dies on this kernel (no ld-musl).
#     CURL_LDFLAGS_BIN="-all-static" is what actually makes it static.
#   - the kernel's gcc 12.3 ICEs in libcpp on some inputs, so musl-gcc is
#     always pointed at gcc-13 via REALGCC (see dynhello).
CURL_BIN := $(BUILD)/curlsrc/curl-8.9.1/src/curl

# mbedtls 2.28.9 -- the TLS library curl links against, vendored in-tree
# (src/user/third_party/mbedtls, Apache-2.0).  It is compiled against
# musl's headers exactly like the musl programs, with the GNOS
# configuration: include/mbedtls/mbedtls_config.h *is* config-gnos.h (a
# client-only TLS 1.2 with ECDHE + AES-GCM + SHA-256 suites, entropy fed
# by the kernel's getrandom through mbedtls_hardware_poll).  mbedtls's own
# library Makefile builds the three split archives curl's configure probes
# for (libmbedtls/x509/crypto.a), and the result is copied into an
# installed prefix (include/ + lib/) curl's configure understands.
MBEDTLS_SRC    := src/user/third_party/mbedtls
MBEDTLS_INC    := $(MBEDTLS_SRC)/include
MBEDTLS_PREFIX := $(BUILD)/mbedtls-inst
MBEDTLS_LIBS   := $(MBEDTLS_PREFIX)/lib/libmbedtls.a \
                  $(MBEDTLS_PREFIX)/lib/libmbedx509.a \
                  $(MBEDTLS_PREFIX)/lib/libmbedcrypto.a
MBEDTLS_CFLAGS := $(MUSLCFLAGS) -I$(MBEDTLS_SRC) -I$(MBEDTLS_INC)

$(MBEDTLS_LIBS): $(MBEDTLS_SRC)/library/Makefile $(wildcard $(MBEDTLS_SRC)/library/*.c) $(MBEDTLS_INC)/mbedtls/mbedtls_config.h $(MBEDTLS_INC)/mbedtls/config.h
	mkdir -p $(MBEDTLS_PREFIX)/lib
	$(MAKE) -C $(MBEDTLS_SRC)/library clean
	$(MAKE) -C $(MBEDTLS_SRC)/library -j"$(nproc)" \
	  CC=$(CC) AR=ar CFLAGS="$(MBEDTLS_CFLAGS) -O2 -g" \
	  libmbedtls.a libmbedx509.a libmbedcrypto.a
	cp -r $(MBEDTLS_INC) $(MBEDTLS_PREFIX)/
	cp $(MBEDTLS_SRC)/library/libmbedtls.a \
	   $(MBEDTLS_SRC)/library/libmbedx509.a \
	   $(MBEDTLS_SRC)/library/libmbedcrypto.a $(MBEDTLS_PREFIX)/lib/

# GNU nano — the editor, against a wide-char ncurses built by hand into
# build/nanosrc/ncstage (also musl, also static).  ncurses 6.4 needs its
# generated lib_gen.c rebuilt with gcc-13's cpp (the gcc 12 ICE strikes the
# MKlib_gen.sh pipeline too), and the initrd carries the xterm + linux
# terminfo entries it was installed with so TERM=xterm nano has a terminal
# description to talk to.
NANO_SRC := $(BUILD)/nanosrc
NANO_BIN := $(NANO_SRC)/nano-7.2/src/nano
NC_STAGE := $(NANO_SRC)/ncstage

# The desktop stack (wayland/wlroots 0.19/labwc 0.9 + friends), cross-built
# by hand into build/desk/stage with musl-gcc (see build/desk/env.sh).  The
# initrd's labwc section copies the compositor, xkb data and config out of
# this stage.
DESK_STAGE := $(BUILD)/desk/stage

# GNU coreutils 9.9 — the other half of what makes a shell prompt feel like a
# system.  Fetched into build/ccsrc and configured by hand with:
#
#   ./configure --host=x86_64-linux-musl --prefix=/usr --disable-nls \
#       --disable-libcap --without-selinux \
#       --enable-no-install-program=stdbuf HELP2MAN=: MAKEINFO=: \
#       CC=<musl-gcc> CFLAGS="-O2 -g -static -no-pie -fno-pie -isystem <stub>"
#
# Three of those are not obvious:
#
#   --enable-no-install-program=stdbuf drops libstdbuf.so.  It is the only
#   shared object coreutils builds, and musl-gcc's static-only setup cannot
#   produce one (`relocation R_X86_64_32 can not be used when making a shared
#   object`).  stdbuf is useless here anyway: it works by LD_PRELOAD.
#
#   HELP2MAN=: MAKEINFO=: because the man pages are generated by *running* each
#   freshly built binary with --help, and the texinfo manual needs makeinfo.
#   Neither ships in the image, so both are stubbed out to `true`.
#
#   -isystem $(CC_STUB) puts a fake <linux/version.h> reporting kernel 0.0.0 on
#   the include path.  gnulib probes it to decide whether copy_file_range(2),
#   renameat2(2) and friends are worth calling; claiming to be older than all
#   of them makes coreutils take its portable fallback paths, which is exactly
#   what this kernel implements.
#
# gnulib-tests is excluded from the build (SUBDIRS below) rather than fixed:
# it wants <linux/fs.h>, the test suite is not installed into the image, and
# building it would double the compile for nothing.
CC_SRC  := $(BUILD)/ccsrc/coreutils-9.9
CC_STUB := $(BUILD)/ccsrc/linux-stub/include
CC_BIN  := $(CC_SRC)/src/ls

# GNU binutils 2.47 — assembler, linker and the ELF inspection tools, so the
# guest can build and dissect its own binaries.  Fetched by hand into
# build/busrcc and configured with:
#
#   ./configure --host=x86_64-linux-musl CC=<musl-gcc> AR=gcc-ar \
#               RANLIB=gcc-ranlib CFLAGS="-O2 -static -no-pie -fno-pie" \
#               LDFLAGS="-static" --disable-nls --disable-werror \
#               --disable-gdb --disable-gprofng --without-debuginfod
#
# AR/RANLIB are gcc-ar/gcc-ranlib rather than bare ar because 2.47's
# configure probe for the libsframe archiver interface trips on plain ar.
#
# Two more hand edits live in the tree and must be re-applied after any
# re-configure:
#   - the libtool script in each subdirectory swallows `-static` as one of
#     its own mode flags and never passes it to the compiler driver, so the
#     tools come out dynamically linked against libc and die instantly on
#     this kernel (no ld-musl).  The `-static | -static-libtool-libs)`
#     branch in func_mode_link is patched to append " -static" to the
#     compile/finalize commands instead of `continue`.
#   - `disable-shared` in configure makes the bfd/opcodes/ctf libraries
#     static archives, otherwise libtool links the executables against
#     uninstalled .so files.
BU_SRC := $(BUILD)/busrcc/binutils-2.47

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
MUSLCFLAGS := $(filter-out -Isrc/include -Isrc/kernel/core -Isrc/kernel/driver \
                           -mgeneral-regs-only \
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

# A hard disk for the guest to install onto.  `-cdrom` already occupies the
# secondary master (that is where the boot ISO is, and why ata.c has to detect
# and skip ATAPI devices), so this goes on the primary channel and comes up as
# /dev/sda.  `if=ide` and not virtio deliberately: the point of src/kernel/ata.c
# is to drive the 1986 interface every PC still emulates, so the guest sees a
# disk that needs no driver it does not already have.
#
# The image is sparse -- 256 MiB of address space costs a few kilobytes on the
# host until something writes to it -- and is *not* removed by `clean`: once
# the installer has put a system on it, that system is the interesting artifact.
DISK    := $(BUILD)/disk.img
DISK_MB ?= 256
QEMU_DISK := -drive file=$(DISK),format=raw,if=ide,index=0,media=disk

# The initrd's explicit size.  mke2fs -d auto-sizes to the content, which is
# exactly what must not happen: the kernel mounts this read-write in RAM and
# every byte the filesystem grows at runtime comes out of this headroom.
# fastfetch alone added ~4 MiB of binaries, so 96M.
INITRD_MB ?= 96

# Number of virtual cores QEMU exposes.  The SMP bring-up path brings up
# every core Limine reports, so changing this also changes what smpinfo.elf
# (run from /etc/rc) must assert -- keep the two in sync.
SMP_CPUS ?= 4
QEMU_SMP := -smp $(SMP_CPUS)

QEMU_DEVICES := $(QEMU_NET) $(QEMU_AUDIO) $(QEMU_DISK) $(QEMU_SMP)

# The same hardware, but with a backend you can actually hear.  Override on
# the command line if PulseAudio is not what your desktop runs, e.g.
#   make guistart AUDIO_BACKEND=pipewire
#   make guistart AUDIO_BACKEND=alsa
AUDIO_BACKEND ?= pa
GUI_AUDIO := -audiodev $(AUDIO_BACKEND),id=snd0 \
             -device AC97,audiodev=snd0 \
             -device intel-hda -device hda-duplex,audiodev=snd0

.PHONY: all run run-uefi guistart headless clean distclean
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
#
# CONFIG_STATIC must be y in $(BB_SRC)/.config or the result is a dynamic
# PIE with an ld-musl interpreter -- which the kernel's loader cannot run
# (ET_EXEC only, no dynamic linker).  -no-pie -fno-pie on top because the
# host gcc defaults to PIE, and "-static -pie" is still an ET_DYN.
$(BB_BIN): $(BB_SRC)/.config | $(MUSL_GCC)
	$(MAKE) -C $(BB_SRC) CC=$(abspath $(MUSL_GCC)) HOSTCC=gcc AR=gcc-ar \
	  SKIP_STRIP=y CFLAGS_EXTRA="-fno-pie -no-pie" LDFLAGS_EXTRA="-static -no-pie"

# GNU Bash.  Like BusyBox, the tree is fetched and configured by hand (see
# the BASH_SRC comment above) and this rule only relinks it when the binary
# is missing, so a hand-run `make` inside the tree is never undone.
$(BASH_BIN): $(BASH_SRC)/Makefile | $(MUSL_GCC)
	$(MAKE) -C $(BASH_SRC)

# curl.  Configured by hand (see CURL_BIN above); this rule only relinks it
# when the binary is missing.  The link flags are forced on the command line
# because libtool eats -static from LDFLAGS (see the CURL_BIN comment).
$(CURL_BIN): $(BUILD)/curlsrc/curl-8.9.1/Makefile $(MBEDTLS_LIBS) | $(MUSL_GCC)
	REALGCC=gcc-13 $(MAKE) -C $(BUILD)/curlsrc/curl-8.9.1 CURL_LDFLAGS_BIN="-all-static"

# GNU nano against the ncursesw build in build/nanosrc/ncstage.  Same
# hand-configured-tree pattern; CFLAGS is forced on the command line so the
# stage's headers (and the linux uapi copy that musl's sys/vt.h needs) are
# always on the include path.
$(NANO_BIN): $(NANO_SRC)/nano-7.2/Makefile | $(MUSL_GCC)
	REALGCC=gcc-13 $(MAKE) -C $(NANO_SRC)/nano-7.2 CFLAGS="-O2 -g -static -fno-pie \
	  -isystem $(BUILD)/desk/stage/include"

# GNU coreutils.  Same deal: the tree is configured by hand (see CC_SRC above)
# and this only rebuilds when the binaries are gone.  SUBDIRS is overridden to
# skip gnulib-tests; "po ." is the pair that actually produces src/*.
$(CC_BIN): $(CC_SRC)/Makefile | $(MUSL_GCC)
	$(MAKE) -C $(CC_SRC) SUBDIRS="po ." HELP2MAN=: MAKEINFO=:

# GNU binutils.  Configured by hand (see BU_SRC above); this only relinks it
# when the tool binaries are gone.
$(BU_SRC)/binutils/readelf: $(BU_SRC)/Makefile | $(MUSL_GCC)
	$(MAKE) -C $(BU_SRC)

# fastfetch — the system-info tool GNOS exists to run.  Fetched into
# build/ffsrc (https://github.com/fastfetch-cli/fastfetch.git) and built by
# tools/build-fastfetch.sh with the locally built clang 24 (fastfetch needs
# C23, host gcc 12 cannot do it) against musl, statically, with every
# optional dependency disabled.  See that script for the full flag list.
#
# This rule must live below `all`: make takes the first target in the file
# as its default goal.
FF_SRC := $(BUILD)/ffsrc
FF_BIN := $(BUILD)/ffbuild/fastfetch
FFLASH  := $(BUILD)/ffbuild/flashfetch

$(FF_BIN): $(FF_SRC)/CMakeLists.txt | $(MUSL_GCC)
	chmod +x tools/build-fastfetch.sh
	tools/build-fastfetch.sh

# Both directories are listed separately: `clean` leaves $(BUILD) standing (the
# third-party trees live there), so a rule keyed only on $(BUILD) would never
# fire again and $(BUILD)/user would stay missing.
$(BUILD) $(BUILD)/user $(BUILD)/modules:
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
# Sources live in three trees -- core (arch/mem/fs/proc) and driver
# (hardware-facing) subdirectories plus the root for the two entry-point
# files -- while every .o lands flat in $(BUILD).  vpath lets the %.o rules
# below find a source by bare name no matter which directory it is in.
vpath %.c src/kernel src/kernel/core src/kernel/driver
vpath %.asm src/kernel src/kernel/core src/kernel/driver
vpath %.S src/kernel src/kernel/core src/kernel/driver

$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(KCFLAGS) $(DEPFLAGS) -c -o $@ $<

$(BUILD)/%.o: %.asm | $(BUILD)
	$(AS) -f elf64 -o $@ $<

# GNU-as sources, for the one thing nasm cannot do here: .incbin of a file the
# C compiler must not see.  Run through the C driver so the preprocessor and
# the kernel's flags apply.
$(BUILD)/%.o: %.S | $(BUILD)
	$(CC) $(KCFLAGS) $(DEPFLAGS) -c -o $@ $<

# The font blob is checked in, so this is a dependency rather than a rule that
# fires: `make cjkfont` regenerates it deliberately, a normal build never does.
$(BUILD)/cjkfont_data.o: src/kernel/driver/cjkfont.bin

# The CJK bitmap is stapled into the kernel image by GNU-as (.incbin of a 1 MiB
# binary the C compiler must never see).  It is assembled into its own object,
# cjkfont_data.o; cjkfont.c compiles to cjkfont.o.  Keeping them separate is
# what stops the two competing for the same cjkfont.o target.
$(BUILD)/cjkfont_data.o: src/kernel/driver/cjkfont_data.S | $(BUILD)
	$(CC) $(KCFLAGS) $(DEPFLAGS) -c -o $@ $<

.PHONY: cjkfont
cjkfont:
	python3 tools/mkcjkfont.py

$(KRNL): $(KOBJS) linker.ld
	$(CC) $(KCFLAGS) -static-pie -Wl,-T,linker.ld -Wl,-z,max-page-size=0x1000 \
	  -Wl,--build-id=none -o $@ $(KOBJS)

# ---------- loadable kernel modules (.ko) ----------
# src/kernel/modules/*.c -> build/modules/*.ko.  These are plain ET_REL
# objects (no -fpie, no -fpic): the loader maps and relocates them itself.
# The module toolchain is the kernel's minus the PIE codegen; -fno-pie /
# -fno-pic undo what BASEFLAGS -fpie would otherwise add.
KMSRC := $(wildcard src/kernel/modules/*.c)
KMODS := $(patsubst src/kernel/modules/%.c,$(BUILD)/modules/%.ko,$(KMSRC))

$(BUILD)/modules/%.o: src/kernel/modules/%.c \
	  src/kernel/modules/module_info.h src/kernel/core/module.h \
	  src/kernel/core/kstring.h | $(BUILD)/modules
	$(CC) $(BASEFLAGS) -fno-pie -fno-pic -fno-stack-protector \
	  -fno-asynchronous-unwind-tables -fno-omit-frame-pointer \
	  $(DEPFLAGS) -c -o $@ $<

$(BUILD)/modules/%.ko: $(BUILD)/modules/%.o
	$(LD) -m elf_x86_64 -r --build-id=none -o $@ $<

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

# dynhello — the one *dynamically linked* program on the system.  musl-gcc
# links it as a PIE (ET_DYN) with a PT_INTERP pointing at
# /lib/ld-musl-x86_64.so.1; the kernel loader maps the PIE, reads the
# interpreter path, loads the linker, and hands the entry point to it, the
# way a Linux loader would.  Everything else stays -static: this file exists
# to prove the ET_DYN + PT_INTERP path, not to be a production choice.
$(BUILD)/dynhello.elf: src/user/dynhello.c $(MUSL_GCC)
	REALGCC=gcc-13 $(MUSL_GCC) -O2 -g -o $@ $<
	file $@

# ---------- initrd (ext2 image holding the user programs) ----------
# mke2fs -d fills the image straight from a staging directory, so this needs
# neither a loopback mount nor root.  The feature set is trimmed on purpose:
# ^dir_index keeps every directory a plain linear list, which is all the
# kernel driver knows how to rewrite.
$(INITRD): $(UELFS) $(MUSL_ELFS) $(BUILD)/dynhello.elf $(BB_BIN) $(BASH_BIN) \
           $(CC_BIN) $(KRNL) $(FF_BIN) $(KMODS) $(CURL_BIN) $(NANO_BIN) \
           src/user/rc | $(BUILD)
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
	# ---- /sys: the static sysfs tree the DRM client tooling reads ---------
	# There is no sysfs backend, so the handful of nodes fastfetch's GPU
	# detection consults are laid down here, by hand, exactly as the Linux
	# sysfs would lay them out for a QEMU bochs VGA behind bochsdrm.  They
	# describe the real hardware: 1234:1111 stdvga, class 03/00, on
	# 0000:00:02.0 -- the device GNOS's drm.c actually drives.
	mkdir -p $(BUILD)/initrd-root/sys/class/drm/card0
	mkdir -p $(BUILD)/initrd-root/sys/class/drm/renderD128
	mkdir -p $(BUILD)/initrd-root/sys/devices/0000:00:02.0/drm/renderD128
	# /sys/dev/char/<maj>:<min> is where libdrm maps an fd back to its
	# /dev node: drmGetDeviceNameFromFd2() fstats the fd and reads
	# DEVNAME= out of /sys/dev/char/N/uevent.  drmNodeIsDRM() additionally
	# stats /sys/dev/char/N/device/drm, so the char node needs its 'device'
	# symlink and the PCI device needs a drm/ subdir, exactly as Linux lays
	# it out.  Without all three wlroots refuses the whole DRM backend.
	mkdir -p $(BUILD)/initrd-root/sys/dev/char/226:0
	mkdir -p $(BUILD)/initrd-root/sys/dev/char/226:128
	echo 'DEVNAME=dri/card0' > $(BUILD)/initrd-root/sys/dev/char/226:0/uevent
	echo 'DEVNAME=dri/renderD128' > $(BUILD)/initrd-root/sys/dev/char/226:128/uevent
	ln -sfn ../../../devices/0000:00:02.0 \
	  $(BUILD)/initrd-root/sys/dev/char/226:0/device
	ln -sfn ../../../devices/0000:00:02.0 \
	  $(BUILD)/initrd-root/sys/dev/char/226:128/device
	mkdir -p $(BUILD)/initrd-root/sys/devices/0000:00:02.0/drm/card0
	echo 'pci:v00001234d00001111sv00001AF4sd00001100bc03sc00' \
	  > $(BUILD)/initrd-root/sys/devices/0000:00:02.0/modalias
	ln -sfn ../../../bus/pci/drivers/bochsdrm \
	  $(BUILD)/initrd-root/sys/devices/0000:00:02.0/driver
	ln -sfn ../../../devices/0000:00:02.0 \
	  $(BUILD)/initrd-root/sys/class/drm/card0/device
	ln -sfn ../../../devices/0000:00:02.0 \
	  $(BUILD)/initrd-root/sys/class/drm/renderD128/device
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
	# ---- the dynamically linked hello, and the loader it needs ----
	# dynhello.elf is an ET_DYN with PT_INTERP=/lib/ld-musl-x86_64.so.1, so
	# the initrd must carry the interpreter at exactly that path.  musl's
	# libc.so *is* the dynamic linker (the two names are the same file in a
	# musl install), so a plain copy provides both the interpreter and the
	# libc.so it is asked to load.
	cp $(BUILD)/dynhello.elf $(BUILD)/initrd-root/bin/dynhello.elf
	cp $(MUSL_LIB)/libc.so $(BUILD)/initrd-root/lib/ld-musl-x86_64.so.1
	cp $(MUSL_LIB)/libc.so $(BUILD)/initrd-root/lib/libc.so
	# `mount` is invoked by its bare name from OpenRC's init.sh and service
	# scripts, so it must sit on PATH as /bin/mount (not /bin/mount.elf).  The
	# rest of the musl programs are only ever called by absolute path.
	cp $(BUILD)/mount.elf $(BUILD)/initrd-root/bin/mount
	# getty, login and chvt are named without the .elf suffix: /etc/inittab
	# style callers, the shell and /etc/issue all refer to them by the names
	# every other Unix uses, and `login` in particular is what getty execs by
	# a compiled-in absolute path.
	cp $(BUILD)/getty.elf $(BUILD)/initrd-root/sbin/getty
	cp $(BUILD)/login.elf $(BUILD)/initrd-root/bin/login
	cp $(BUILD)/chvt.elf  $(BUILD)/initrd-root/usr/bin/chvt
	# ---- loadable kernel modules: /lib/modules, like every Linux ---------
	mkdir -p $(BUILD)/initrd-root/lib/modules
	for k in $(KMODS); do \
	  cp $$k $(BUILD)/initrd-root/lib/modules/; \
	done
	# insmod/rmmod are typed by name at the shell, no .elf suffix (and
	# /sbin, like the Linux kmod tools they stand in for).
	cp $(BUILD)/insmod.elf $(BUILD)/initrd-root/sbin/insmod
	cp $(BUILD)/rmmod.elf  $(BUILD)/initrd-root/sbin/rmmod
	# The installer is typed by name at the shell, exactly like the rest of
	# a Unix tool set -- no .elf suffix.
	cp $(BUILD)/installer.elf $(BUILD)/initrd-root/bin/installer
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
	# ---- GNU coreutils ----
	# Every program coreutils built, into /usr/bin.  This lands *after* the
	# BusyBox loop above, so for the ~30 names both provide (cat, cp, ls, rm,
	# sort, ...) the GNU one wins and BusyBox's applet stays reachable through
	# /bin/busybox.elf's multi-call dispatcher.  That is the intended order:
	# BusyBox was scaffolding to get a userland booting at all, GNU coreutils
	# is the thing GNOS is supposed to run.
	#
	# Stripped like bash, and for the same reason -- 33 MB of binaries becomes
	# 12 MB, all of it DWARF that nothing in the image can read.  `ginstall` is
	# coreutils' build-time name for install(1) (autoconf renames it to dodge
	# the host's install script); it goes in under its real name.  `getlimits`
	# is a helper for coreutils' own test suite and has no business shipping.
	for f in $(CC_SRC)/src/*; do \
	  [ -f "$$f" ] && [ -x "$$f" ] || continue; \
	  n=$${f##*/}; \
	  case "$$n" in *.o|*.sh|*.pl|getlimits) continue;; esac; \
	  head -c4 "$$f" | grep -q ELF || continue; \
	  [ "$$n" = ginstall ] && n=install; \
	  cp "$$f" $(BUILD)/initrd-root/usr/bin/$$n; \
	  strip $(BUILD)/initrd-root/usr/bin/$$n; \
	done
	# ---- GNU binutils ----
	# The ELF toolchain the guest ships with.  `as`/`ld` come from their
	# build-time names (as-new/ld-new); ld is copied twice so that the
	# ld.bfd spelling that many build scripts probe for works too.
	cp $(BU_SRC)/binutils/ar        $(BUILD)/initrd-root/usr/bin/ar
	cp $(BU_SRC)/binutils/addr2line $(BUILD)/initrd-root/usr/bin/addr2line
	cp $(BU_SRC)/binutils/cxxfilt   $(BUILD)/initrd-root/usr/bin/c++filt
	cp $(BU_SRC)/binutils/elfedit   $(BUILD)/initrd-root/usr/bin/elfedit
	cp $(BU_SRC)/binutils/nm-new    $(BUILD)/initrd-root/usr/bin/nm
	cp $(BU_SRC)/binutils/objcopy   $(BUILD)/initrd-root/usr/bin/objcopy
	cp $(BU_SRC)/binutils/objdump   $(BUILD)/initrd-root/usr/bin/objdump
	cp $(BU_SRC)/binutils/ranlib    $(BUILD)/initrd-root/usr/bin/ranlib
	cp $(BU_SRC)/binutils/readelf   $(BUILD)/initrd-root/usr/bin/readelf
	cp $(BU_SRC)/binutils/size      $(BUILD)/initrd-root/usr/bin/size
	cp $(BU_SRC)/binutils/strings   $(BUILD)/initrd-root/usr/bin/strings
	cp $(BU_SRC)/binutils/strip-new $(BUILD)/initrd-root/usr/bin/strip
	cp $(BU_SRC)/gas/as-new         $(BUILD)/initrd-root/usr/bin/as
	cp $(BU_SRC)/ld/ld-new          $(BUILD)/initrd-root/usr/bin/ld
	cp $(BU_SRC)/ld/ld-new          $(BUILD)/initrd-root/usr/bin/ld.bfd
	cp $(BU_SRC)/gprof/gprof        $(BUILD)/initrd-root/usr/bin/gprof
	strip $(BUILD)/initrd-root/usr/bin/ar \
	      $(BUILD)/initrd-root/usr/bin/addr2line \
	      $(BUILD)/initrd-root/usr/bin/c++filt \
	      $(BUILD)/initrd-root/usr/bin/elfedit \
	      $(BUILD)/initrd-root/usr/bin/nm \
	      $(BUILD)/initrd-root/usr/bin/objcopy \
	      $(BUILD)/initrd-root/usr/bin/objdump \
	      $(BUILD)/initrd-root/usr/bin/ranlib \
	      $(BUILD)/initrd-root/usr/bin/readelf \
	      $(BUILD)/initrd-root/usr/bin/size \
	      $(BUILD)/initrd-root/usr/bin/strings \
	      $(BUILD)/initrd-root/usr/bin/strip \
	      $(BUILD)/initrd-root/usr/bin/as \
	      $(BUILD)/initrd-root/usr/bin/ld \
	      $(BUILD)/initrd-root/usr/bin/ld.bfd \
	      $(BUILD)/initrd-root/usr/bin/gprof
	# ---- fastfetch ----
	# The system-info tool itself, plus its single-threaded flashfetch
	# sibling.  Stripped like everything else: the unstripped pair is
	# mostly DWARF.
	cp $(FF_BIN) $(BUILD)/initrd-root/usr/bin/fastfetch
	cp $(FFLASH) $(BUILD)/initrd-root/usr/bin/flashfetch
	strip $(BUILD)/initrd-root/usr/bin/fastfetch \
	      $(BUILD)/initrd-root/usr/bin/flashfetch
	# ---- curl ----
	# The network client that proves the TCP/UDP stack end to end.  Stripped
	# on the way in like everything else.
	cp $(CURL_BIN) $(BUILD)/initrd-root/usr/bin/curl
	strip $(BUILD)/initrd-root/usr/bin/curl
	# curl's CA store: the host's root bundle, at the path curl was
	# configured with --with-ca-bundle.  Without it https fails loudly;
	# `curl -k` remains the escape hatch.
	mkdir -p $(BUILD)/initrd-root/etc/ssl/certs
	cp /etc/ssl/certs/ca-certificates.crt \
	   $(BUILD)/initrd-root/etc/ssl/certs/ca-bundle.pem
	# ---- nano + terminfo ----
	# The editor, plus the xterm and linux terminfo entries it needs when
	# TERM=xterm (getty sets TERM=linux by default, so nano must have that
	# entry too).  The whole ncurses terminfo tree is 6.7 MB; x/ and linux/
	# are the only entries this machine will ever ask for.
	cp $(NANO_BIN) $(BUILD)/initrd-root/usr/bin/nano
	strip $(BUILD)/initrd-root/usr/bin/nano
	mkdir -p $(BUILD)/initrd-root/usr/share/terminfo
	cp -a $(NC_STAGE)/share/terminfo/x $(BUILD)/initrd-root/usr/share/terminfo/
	mkdir -p $(BUILD)/initrd-root/usr/share/terminfo/l
	cp -a $(NC_STAGE)/share/terminfo/l/linux $(BUILD)/initrd-root/usr/share/terminfo/l/
	# v/ is ncurses's fallback terminal (vt220) when TERM is unset, so it
	# rides along too.
	mkdir -p $(BUILD)/initrd-root/usr/share/terminfo/v
	cp -a $(NC_STAGE)/share/terminfo/v $(BUILD)/initrd-root/usr/share/terminfo/
	# ---- labwc: the wayland compositor -----------------------------------
	# The labwc stack (wlroots 0.19 + labwc 0.9, built by hand into
	# build/desk/stage) is the desktop itself.  It needs three things the
	# initrd must carry: the binary, the xkb keyboard-layout data that
	# libxkbcommon reads (rules/keycodes/symbols), and its rc.xml/menu.xml
	# config.  xkbcommon looks at XKB_CONFIG_ROOT, which rc exports, so the
	# data lands at the standard /usr/share/X11/xkb path.
	cp $(DESK_STAGE)/bin/labwc $(BUILD)/initrd-root/usr/bin/labwc
	strip $(BUILD)/initrd-root/usr/bin/labwc
	mkdir -p $(BUILD)/initrd-root/usr/share/X11/xkb
	cp -a /usr/share/X11/xkb/. $(BUILD)/initrd-root/usr/share/X11/xkb/
	mkdir -p $(BUILD)/initrd-root/etc/xdg/labwc
	cp $(DESK_STAGE)/../labwc-etc/rc.xml $(BUILD)/initrd-root/etc/xdg/labwc/rc.xml
	cp $(DESK_STAGE)/../labwc-etc/menu.xml $(BUILD)/initrd-root/etc/xdg/labwc/menu.xml
	# labwc draws text through pango/cairo/fontconfig: the initrd must carry
	# fontconfig's config tree (fonts.conf + conf.d) and at least one
	# TrueType face, or pango fails font resolution and the compositor
	# refuses to start.  Two DejaVu faces (sans + mono) are plenty.
	mkdir -p $(BUILD)/initrd-root/etc/fonts
	cp -a /etc/fonts/. $(BUILD)/initrd-root/etc/fonts/
	mkdir -p $(BUILD)/initrd-root/usr/share/fonts/truetype/dejavu
	cp /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf \
	   /usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf \
	   $(BUILD)/initrd-root/usr/share/fonts/truetype/dejavu/
	cp src/user/rc $(BUILD)/initrd-root/etc/rc            # run once at boot by init
	# Static system config (hosts, resolv.conf, nsswitch, services, protocols,
	# passwd/group, hostname).  These make the BusyBox network tools and the
	# C resolver actually work: ping/wget do DNS via /etc/resolv.conf, getent
	# reads /etc/passwd, and `hostname` uses /etc/hostname.
	cp -a src/rootfs/etc/. $(BUILD)/initrd-root/etc/
	# ---- root home: ~/.bashrc is sourced by the interactive login shell ----
	cp -a src/rootfs/root/. $(BUILD)/initrd-root/root/
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
	# ---- the ordinary user's home ----------------------------------------
	mkdir -p $(BUILD)/initrd-root/home/elaina
	cp -a src/rootfs/home/elaina/. $(BUILD)/initrd-root/home/elaina/ 2>/dev/null || true
	# ---- boot payload (what an installed machine boots from) ------------
	# Every system image carries the whole boot chain inside it, so a root
	# partition dumped onto a disk by the installer is already bootable: the
	# disk's Limine reads /boot/limine/limine.sys (stage 2) and
	# /boot/limine/limine.conf at boot, and the kernel_image file it points
	# at is /GNOSKr.elf at the volume root.  Stage 1 (limine-bios.sys)
	# handled the MBR when the installer ran.  The kernel's own modules are
	# not copied -- installed boots have no initrd, and the root filesystem
	# itself is the module, read off partition 1 by the kernel at boot.
	mkdir -p $(BUILD)/initrd-root/boot/limine
	cp limine/limine-bios.sys $(BUILD)/initrd-root/boot/limine/limine.sys
	cp $(KRNL) $(BUILD)/initrd-root/GNOSKr.elf
	printf 'timeout: 1\n\n/GNOS\n    protocol: limine\n    kernel_path: boot():/GNOSKr.elf\n' > $(BUILD)/initrd-root/boot/limine/limine.conf
	dd if=/dev/zero of=$@ bs=1M count=64 2>/dev/null
	# ---- ownership and modes ---------------------------------------------
	# mke2fs -d copies the *build user's* uid/gid onto every inode, which on
	# a machine whose developer is uid 1000 means shipping an image where
	# /etc/shadow and /bin/bash belong to an ordinary user.  That was
	# harmless while the kernel reported st_uid = 0 for everything; now that
	# it reads i_uid for real, it would hand the whole system away.
	#
	# fakeroot is what fixes it: chown(2) inside it is remembered in a side
	# table that mke2fs's stat(2) then sees, so the image comes out
	# root-owned without this build needing to be root.  Everything that has
	# to differ from root:root -- the user's home, the shadow file's mode --
	# is set in the same shell, after the blanket chown.
	fakeroot -- sh -c '\
	  chown -R 0:0 $(BUILD)/initrd-root; \
	  chmod 0700 $(BUILD)/initrd-root/root; \
	  chmod 0600 $(BUILD)/initrd-root/etc/shadow; \
	  chmod 0644 $(BUILD)/initrd-root/etc/passwd $(BUILD)/initrd-root/etc/group; \
	  chmod 1777 $(BUILD)/initrd-root/tmp; \
	  chown -R 1000:1000 $(BUILD)/initrd-root/home/elaina; \
	  chmod 0755 $(BUILD)/initrd-root/home/elaina; \
	  mke2fs -q -t ext2 -b 1024 -I 256 \
	         -O ^resize_inode,^dir_index,^ext_attr \
	         -d $(BUILD)/initrd-root -F $@ $(INITRD_MB)M'

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

# ---------- run ----------
# The disk is created on demand and never rebuilt once it exists -- `make run`
# twice in a row must find whatever the guest wrote the first time.  The rule
# lives down here rather than beside its variables because make builds the
# *first* target in the file when given no arguments, and that has to stay
# `all`.
$(DISK):
	@mkdir -p $(BUILD)
	qemu-img create -f raw $@ $(DISK_MB)M

run: $(ISO) $(DISK)
	qemu-system-x86_64 -cdrom $(ISO) -m 512M -display gtk $(QEMU_DEVICES)

run-uefi: $(ISO) $(DISK)
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
guistart: $(ISO) $(DISK)
	@echo "GNOS: booting in a window (audio backend: $(AUDIO_BACKEND));"
	@echo "      boot log is also being written to $(BUILD)/dbg.log"
	qemu-system-x86_64 -cdrom $(ISO) -m 512M \
	  $(QEMU_NET) $(GUI_AUDIO) $(QEMU_DISK) \
	  -device isa-debugcon,chardev=dbg -chardev file,id=dbg,path=$(BUILD)/dbg.log \
	  -display gtk -no-reboot

# headless — guistart without the window: the headless self-test target.
# The debug console is teed to build/dbg.log, exactly like guistart, so
# `make headless; grep PASS build/dbg.log` is the whole verification loop.
headless: $(ISO) $(DISK)
	@echo "GNOS: booting headless; log is $(BUILD)/dbg.log"
	qemu-system-x86_64 -cdrom $(ISO) -m 512M \
	  $(QEMU_NET) $(QEMU_DISK) \
	  -device isa-debugcon,chardev=dbg -chardev file,id=dbg,path=$(BUILD)/dbg.log \
	  -display none -no-reboot -smp 4

# `clean` deliberately spares the third-party source trees under $(BUILD).
# musl was fetched and built by hand and there is no rule to get it back, so a
# plain `rm -rf build` would destroy the toolchain irrecoverably.  Everything
# this Makefile knows how to rebuild is listed explicitly instead.
THIRD_PARTY := $(BUILD)/muslsrc $(BUILD)/bbsrc $(BUILD)/bashsrc \
               $(BUILD)/orcsrc $(BUILD)/ccsrc $(BUILD)/busrcc

clean:
	rm -rf $(BUILD)/user $(BUILD)/initrd-root $(BUILD)/modules $(ISO_ROOT)
	rm -f  $(BUILD)/*.o $(BUILD)/*.elf $(BUILD)/*.img $(BUILD)/*.iso \
	       $(BUILD)/*.log

# Nuke everything, third-party trees included.  Only useful if you are prepared
# to re-fetch musl by hand -- see THIRD_PARTY above.
distclean:
	rm -rf $(BUILD)
