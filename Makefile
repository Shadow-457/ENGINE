export PATH := $(PATH):/usr/sbin
# ================================================================
#  Systrix OS v0.1 -- Makefile  (C kernel edition)
#
#  Targets:
#    make            -> build systrix.img (bootable disk image)
#    make run        -> launch in QEMU with debug log
#    make run-quiet  -> launch in QEMU silently
#    make hello      -> build user/HELLO_C example program
#    make addprog PROG=path/to/ELF -> add binary to FAT32 partition
#    make clean      -> remove all build artifacts
#
#  Prerequisites:
#    binutils (as, ld, objcopy), gcc, mtools, qemu-system-x86_64
# ================================================================

AS   = as
LD   = ld
CC   = gcc
QEMU = qemu-system-x86_64

# -fno-pic: prevent compiler emitting GOT-indirect relocations
# (R_X86_64_REX_GOTPCRELX) for extern function pointers.  Without
# this flag, calling null_isr/timer_isr via a C function pointer
# goes through a non-existent GOT and the IDT gates get loaded with
# the raw code bytes of the handler instead of its address, causing
# an immediate GPF cascade on the first timer tick.
CFLAGS = -m64 -ffreestanding -fno-stack-protector -mno-red-zone -fno-pic \
         -nostdlib -nostdinc -O2 -Iinclude -DSYSTRIX_KERNEL \
         -mno-mmx -mno-sse -mno-sse2 -mno-avx -Wall -Wextra -Wno-unused-parameter

KERNEL_ASM_OBJS = \
    kernel/entry.o  \
    kernel/isr.o

KERNEL_C_OBJS = \
    libc/systrix_libc.o \
    kernel/heap.o      \
    kernel/pmm.o       \
    kernel/vmm.o       \
    kernel/pmm_enhanced.o    \
    kernel/heap_enhanced.o   \
    kernel/vmm_enhanced.o    \
    kernel/vmalloc.o         \
    kernel/mem_safety.o      \
    kernel/tss.o       \
    kernel/process.o   \
    kernel/elf.o       \
    kernel/scheduler.o \
    kernel/syscall.o   \
    kernel/ipc.o       \
    kernel/input.o     \
    kernel/usb_hid.o   \
    kernel/usb.o       \
    kernel/gfx.o       \
    kernel/sound.o     \
    kernel/resilience.o \
    kernel/net.o       \
    kernel/fbdev.o     \
    kernel/gui.o       \
    kernel/kernel.o    \
    kernel/vfs.o       \
    kernel/pipe.o      \
    kernel/signal.o    \
    kernel/fork_exec.o \
    kernel/futex.o     \
    kernel/security.o  \
    kernel/swap.o      \
    kernel/jfs.o       \
    kernel/tcpip.o     \
    kernel/uefi.o      \
    kernel/acpi.o      \
    kernel/pci.o       \
    kernel/ahci.o      \
    kernel/nvme.o      \
    kernel/ps2.o       \
    kernel/e1000.o     \
    kernel/shell.o     \
    kernel/pkgmgr.o \
    kernel/pngview.o

KERNEL_OBJS = $(KERNEL_ASM_OBJS) $(KERNEL_C_OBJS)

# -- Syslibc (shared kernel+user C library) -----------------------
libc/systrix_libc.o: libc/systrix_libc.c libc/systrix_libc.h
	$(CC) $(CFLAGS) -Ilibc -c -o $@ $<

# -- Compilation rules ---------------------------------------------
boot/boot.o: boot/boot.S
	$(AS) --32 -o $@ $<

kernel/entry.o: kernel/entry.S
	$(AS) --64 -o $@ $<

kernel/isr.o: kernel/isr.S
	$(AS) --64 -o $@ $<

kernel/%.o: kernel/%.c include/kernel.h
	$(CC) $(CFLAGS) -c -o $@ $<

# -- Binary images -------------------------------------------------
#
# FIX 1 (boot.bin): newer binutils inject .note.gnu.property into
# object files.  ld --oformat binary dumps all sections linearly so
# boot.bin grows to 1064 bytes instead of 512; the BIOS only loads
# the first sector, causing an immediate triple fault.
# Fix: link to a temp ELF first, then strip the section with objcopy.
boot.bin: boot/boot.o
	$(LD) -m elf_i386 -Ttext=0x7C00 -o boot_elf.tmp $<
	objcopy --remove-section=.note.gnu.property --output-target=binary boot_elf.tmp $@
	@rm -f boot_elf.tmp
	@python3 -c "d=open('$@','rb').read(); assert len(d)==512, f'boot.bin is {len(d)} bytes, not 512'; print('boot.bin OK: 512 bytes')"

kernel.bin: $(KERNEL_OBJS) linker.ld
	$(LD) -m elf_x86_64 -T linker.ld --oformat binary -o $@ $(KERNEL_OBJS)

# -- FAT32 partition image -----------------------------------------
fat32.img:
	dd if=/dev/zero of=$@ bs=1M count=64 status=none
	mkfs.fat -F 32 -n SYSTRIXOS $@
	echo "Systrix OS v0.1 (C kernel) created by hamza shahbaz (Shadow) just need your support guys and i will make a full ecosystem" > /tmp/README.TXT
	MTOOLS_SKIP_CHECK=1 mcopy -o -i $@ /tmp/README.TXT ::/README.TXT

# -- Combined bootable disk image ----------------------------------
systrix.img: boot.bin kernel.bin fat32.img
	dd if=/dev/zero  of=$@ bs=512 count=131072 status=none
	dd if=boot.bin   of=$@ bs=512 conv=notrunc status=none
	dd if=kernel.bin of=$@ bs=512 seek=1  conv=notrunc status=none
	dd if=fat32.img  of=$@ bs=512 seek=512 conv=notrunc status=none

all: systrix.img

# -- QEMU ----------------------------------------------------------
# Fixed MAC ensures net_init can read it from RAL0/RAH0 before reset.
# All run targets include NIC so networking works everywhere.
NIC  = -netdev user,id=net0 -device e1000,netdev=net0,mac=52:54:00:12:34:56
# HDMI output via bochs-display (no legacy VGA, pure framebuffer)
DISP = -device bochs-display,xres=1024,yres=768
# USB keyboard + mouse (supplements PS/2)
USBHID = # PS/2 keyboard+mouse used — GUI polls port 0x60/0x64 directly
AUDIO = -device sb16,audiodev=snd0 -audiodev sdl,id=snd0

run: systrix.img
	$(QEMU) -drive format=raw,file=systrix.img,if=ide \
	        -m 1G -no-reboot \
	        -machine pc,accel=tcg \
	        $(DISP) $(USBHID) \
	        $(NIC) \
	        $(AUDIO)

run-quiet: systrix.img
	$(QEMU) -drive format=raw,file=systrix.img,if=ide \
	        -m 1G -no-reboot \
	        -machine pc,accel=tcg \
	        $(DISP) $(USBHID) \
	        $(NIC) \
	        $(AUDIO) \
	        -display gtk

run-sdl: systrix.img
	$(QEMU) -drive format=raw,file=systrix.img,if=ide \
	        -m 1G -no-reboot \
	        -machine pc,accel=tcg \
	        $(DISP) $(USBHID) \
	        $(NIC) \
	        $(AUDIO) \
	        -display sdl

run-nographic: systrix.img
	$(QEMU) -drive format=raw,file=systrix.img,if=ide \
	        -m 1G -no-reboot \
	        -machine pc,accel=tcg \
	        $(DISP) $(USBHID) \
	        $(NIC) \
	        $(AUDIO) \
	        -serial mon:stdio \
	        -nographic

# -- Engine Compiler (engc) — runs INSIDE EngineOS -------------------
user/shc.o: user/shc.c
	$(CC) -m64 -O2 -ffreestanding -fno-stack-protector -mno-red-zone \
	      -nostdlib -nostdinc -c -o $@ $<

SHC: user/crt0.o user/libc.o user/shc.o
	$(LD) -m elf_x86_64 -static -nostdlib \
	      -Ttext=0x400000 -o $@ $^

shc: SHC
	@echo "Built SHC -- add to disk with: make addshc"

addshc: SHC fat32.img
	MTOOLS_SKIP_CHECK=1 mcopy -i fat32.img SHC ::/SHC
	dd if=fat32.img of=systrix.img bs=512 seek=512 conv=notrunc status=none
	@echo "SHC added to disk. Boot EngineOS and run: elf SHC"

# -- libc (basic C library for user programs) ----------------------
# Link user programs with: crt0.o libc.o yourprog.o
# Then #include "libc.h" in your source.
UCFLAGS = -m64 -O2 -ffreestanding -fno-stack-protector -mno-red-zone \
          -nostdlib -nostdinc -Iuser -Wall -Wextra -Wno-unused-parameter

user/libc.o: user/libc.c user/libc.h
	$(CC) $(UCFLAGS) -c -o $@ $<

user/malloc.o: user/malloc.c user/libc.h
	$(CC) $(UCFLAGS) -c -o $@ $<

# -- Example hello.c user program ----------------------------------
user/crt0.o: user/crt0.S
	$(AS) --64 -o $@ $<

user/hello.o: user/hello.c
	$(CC) $(UCFLAGS) -c -o $@ $<

user/myprogram.o: user/myprogram.c
	$(CC) $(UCFLAGS) -c -o $@ $<

HELLO_C: user/crt0.o user/libc.o user/hello.o
	$(LD) -m elf_x86_64 -static -nostdlib \
	      -Ttext=0x400000 -o $@ $^

MYPROGRAM: user/crt0.o user/libc.o user/myprogram.o
	$(LD) -m elf_x86_64 -static -nostdlib \
	      -Ttext=0x400000 -o $@ $^

hello: HELLO_C
	@echo "Built HELLO_C -- add to disk with: make addprog PROG=HELLO_C"

myprog: MYPROGRAM
	@echo "Built MYPROGRAM -- add to disk with: make addprog PROG=MYPROGRAM"

# -- Build all user programs and embed them in the disk image ------
user/posix_test.o: user/posix_test.c
	$(CC) $(UCFLAGS) -c -o $@ $<

POSIX_TEST: user/crt0.o user/libc.o user/posix_test.o
	$(LD) -m elf_x86_64 -static -nostdlib \
	      -Ttext=0x400000 -o $@ $^

posix_test: POSIX_TEST
	@echo "Built POSIX_TEST -- add to disk with: make addprog PROG=POSIX_TEST"

programs: systrix.img HELLO_C MYPROGRAM POSIX_TEST
	MTOOLS_SKIP_CHECK=1 mcopy -o -i fat32.img HELLO_C   ::/HELLO_C
	MTOOLS_SKIP_CHECK=1 mcopy -o -i fat32.img MYPROGRAM ::/MYPROGRAM
	MTOOLS_SKIP_CHECK=1 mcopy -o -i fat32.img POSIX_TEST ::/POSIX_TEST
	dd if=fat32.img of=systrix.img bs=512 seek=512 conv=notrunc status=none
	@echo "Added HELLO_C, MYPROGRAM and POSIX_TEST to systrix.img"
	@echo "  Boot QEMU then run:  elf POSIX_TEST"

# -- Add a program to the FAT32 partition --------------------------
# Usage: make addprog PROG=./myapp
addprog: fat32.img
	MTOOLS_SKIP_CHECK=1 mcopy -o -i fat32.img $(PROG) ::/$(notdir $(PROG))
	dd if=fat32.img of=systrix.img bs=512 seek=512 conv=notrunc status=none
	@echo "Added $(notdir $(PROG)) to systrix.img -- run with: elf $(notdir $(PROG))"

# -- Sync host home/ folder → OS filesystem root -------------------
# Place any files you want inside SystrixOS into the host folder ./home/
# then run:  make synchome
# They will appear in the OS root ( / ) when you boot.
# Sub-folders are copied recursively (mcopy -s).
#
# FAT32 filename rules (enforced by the filesystem):
#   - Max 8 chars name + 3 chars extension, uppercase
#   - e.g. MYFILE.TXT, MYPROG, SHOT.PNG
#   - mtools will warn/mangle names that are too long or lowercase
synchome: fat32.img
	@if [ ! -d home ]; then \
	    echo "home/ folder not found — creating it for you."; \
	    mkdir -p home; \
	    echo "Put files into home/ then run make synchome again."; \
	    exit 0; \
	fi
	@if [ -z "$$(ls -A home 2>/dev/null)" ]; then \
	    echo "home/ is empty — nothing to sync."; \
	    exit 0; \
	fi
	@echo "Syncing home/* → FAT32 filesystem root..."
	MTOOLS_SKIP_CHECK=1 mcopy -s -o -i fat32.img home/* ::/
	dd if=fat32.img of=systrix.img bs=512 seek=512 conv=notrunc status=none
	@echo "Done. Contents of OS filesystem root:"
	@MTOOLS_SKIP_CHECK=1 mdir -i fat32.img ::/ 2>/dev/null || true
	@echo "Boot with: make run"

# -- Sync home/ then immediately boot ------------------------------
run-home: synchome
	$(QEMU) -drive format=raw,file=systrix.img,if=ide \
	        -m 1G -no-reboot \
	        -machine pc,accel=tcg \
	        $(DISP) $(USBHID) \
	        $(NIC) \
	        $(AUDIO)

.PHONY: synchome run-home

# -- Add sample .shadow source files to disk -----------------------
addshadow: fat32.img
	MTOOLS_SKIP_CHECK=1 mcopy -i fat32.img user/fib.shadow ::/FIB.SHA
	MTOOLS_SKIP_CHECK=1 mcopy -i fat32.img user/hello.shadow ::/HELLO.SHA
	dd if=fat32.img of=shadow.img bs=512 seek=512 conv=notrunc status=none
	@echo "Added FIB.SHA and HELLO.SHA to disk"

# -- Build compiler + add everything to disk in one shot -----------
# This is the main workflow: build SHC, add it + sample files to disk
compiler: systrix.img SHC
	MTOOLS_SKIP_CHECK=1 mcopy -i fat32.img SHC ::/SHC
	MTOOLS_SKIP_CHECK=1 mcopy -i fat32.img user/fib.shadow ::/FIB.SHA
	MTOOLS_SKIP_CHECK=1 mcopy -i fat32.img user/hello.shadow ::/HELLO.SHA
	dd if=fat32.img of=systrix.img bs=512 seek=512 conv=notrunc status=none
	@echo ""
	@echo "  SHC compiler + sample .shadow files added to disk."
	@echo "  Boot EngineOS, then:"
	@echo "    elf SHC          <- launches the compiler"
	@echo "    (enter) FIB.SHA  <- compiles fibonacci"
	@echo "    elf FIB          <- runs it"
	@echo ""

# -- Clean ---------------------------------------------------------
clean:
	rm -f boot/boot.o kernel/*.o kernel/entry.o kernel/isr.o boot_elf.tmp libc/systrix_libc.o
	rm -f boot.bin kernel.bin fat32.img systrix.img
	rm -f user/crt0.o user/hello.o user/myprogram.o user/shc.o user/libc.o user/echo_server.o user/echo_client.o browser/browser.o
	rm -f HELLO_C MYPROGRAM SHC ECHO_SRV ECHO_CLI BROWSER
	rm -f qemu.log /tmp/README.TXT
	rm -f kernel/fbdev.o kernel/gui.o

.PHONY: all run run-quiet hello myprog shc addshc addshadow compiler programs addprog clean

# -- IPC echo server/client (microkernel demo) ---------------------
user/echo_server.o: user/echo_server.c user/ipc.h
	$(CC) $(UCFLAGS) -c -o $@ $<

user/echo_client.o: user/echo_client.c user/ipc.h
	$(CC) $(UCFLAGS) -c -o $@ $<

ECHO_SRV: user/crt0.o user/libc.o user/echo_server.o
	$(LD) -m elf_x86_64 -static -nostdlib \
	      -Ttext=0x400000 -o $@ $^

ECHO_CLI: user/crt0.o user/libc.o user/echo_client.o
	$(LD) -m elf_x86_64 -static -nostdlib \
	      -Ttext=0x400000 -o $@ $^

ipc_demo: systrix.img ECHO_SRV ECHO_CLI
	MTOOLS_SKIP_CHECK=1 mcopy -o -i fat32.img ECHO_SRV ::/ECHO_SRV
	MTOOLS_SKIP_CHECK=1 mcopy -o -i fat32.img ECHO_CLI ::/ECHO_CLI
	dd if=fat32.img of=systrix.img bs=512 seek=512 conv=notrunc status=none
	@echo "Added ECHO_SRV and ECHO_CLI to disk"
	@echo "  In Systrix shell:"
	@echo "    elf ECHO_SRV   <- start server in background"
	@echo "    elf ECHO_CLI   <- run client, should print: IPC round-trip OK!"
	@echo "    ipc            <- list registered servers"

.PHONY: ipc_demo

# ── Browser ────────────────────────────────────────────────────────
# Build flags for browser (needs -Ibrowser -Iuser -Iinclude, no SSE)
BCFLAGS = -m64 -O2 -ffreestanding -fno-stack-protector -mno-red-zone \
          -fno-pic -nostdlib -nostdinc \
          -Ibrowser -Iuser -Iinclude \
          -mno-mmx -mno-sse -mno-sse2 -mno-avx \
          -Wall -Wextra -Wno-unused-parameter -Wno-unused-function

browser/browser.o: browser/browser.c browser/net.h browser/html.h \
                   browser/css.h browser/layout.h browser/render.h \
                   user/libc.h user/tls.h include/font8x8.h
	$(CC) $(BCFLAGS) -c -o $@ $<

browser/png.o: browser/png.c browser/png.h
	$(CC) $(BCFLAGS) -c -o $@ $<

BROWSER_OBJ = user/crt0.o user/libc.o browser/browser.o browser/png.o
BROWSER: $(BROWSER_OBJ)
	$(LD) -m elf_x86_64 -static -nostdlib \
	      -Ttext=0x400000 -o $@ $^
	@echo "Built BROWSER ($(shell wc -c < BROWSER) bytes)"

browser: BROWSER
	@echo "Browser binary ready. Run: make addbrowser"

addbrowser: BROWSER fat32.img
	MTOOLS_SKIP_CHECK=1 mcopy -o -i fat32.img BROWSER ::/BROWSER
	dd if=fat32.img of=systrix.img bs=512 seek=512 conv=notrunc status=none
	@echo ""
	@echo "  BROWSER added to systrix.img"
	@echo "  Boot Systrix OS, then at the shell:"
	@echo "    elf BROWSER"
	@echo ""

# One-shot: build kernel + browser + write to disk
browser-all: systrix.img BROWSER
	MTOOLS_SKIP_CHECK=1 mcopy -i fat32.img BROWSER ::/BROWSER
	dd if=fat32.img of=systrix.img bs=512 seek=512 conv=notrunc status=none
	@echo "systrix.img ready with BROWSER. Run: make run"

.PHONY: browser addbrowser browser-all

# ── SystrixLynx — text-mode browser ──────────────────────────────
browser/lynx.o: browser/lynx.c browser/net.h user/libc.h
	$(CC) $(BCFLAGS) -c -o $@ $<

LYNX_OBJ = user/crt0.o user/libc.o browser/lynx.o
LYNX: $(LYNX_OBJ)
	$(LD) -m elf_x86_64 -static -nostdlib \
	      -Ttext=0x400000 -o $@ $^
	@echo "Built LYNX ($$(wc -c < LYNX) bytes)"

lynx: LYNX
	@echo "SystrixLynx ready."

addlynx: LYNX fat32.img
	MTOOLS_SKIP_CHECK=1 mcopy -o -i fat32.img LYNX ::/LYNX
	dd if=fat32.img of=systrix.img bs=512 seek=512 conv=notrunc status=none
	@echo "LYNX added to systrix.img  ->  boot and run:  elf LYNX"

lynx-all: systrix.img LYNX
	MTOOLS_SKIP_CHECK=1 mcopy -o -i fat32.img LYNX ::/LYNX
	dd if=fat32.img of=systrix.img bs=512 seek=512 conv=notrunc status=none
	@echo "systrix.img ready with LYNX. Run: make run"

.PHONY: lynx addlynx lynx-all
