# ENGINE OS

> A 64-bit operating system built from scratch in C and x86-64 Assembly.

ENGINE is a hobby OS that boots on bare metal (and QEMU), featuring a custom bootloader, preemptive multitasking kernel, virtual memory, FAT32 filesystem, ELF binary loader, PS/2 drivers, and a full framebuffer GUI desktop environment — all written from the ground up.

---

## Features

**Boot & Kernel**
- Custom 512-byte BIOS bootloader — handles real mode → protected mode → long mode transition manually, with hand-rolled GDT/IDT setup
- Preemptive round-robin scheduler with full process control blocks (PCBs)
- Physical memory manager (PMM) using a bitmap allocator
- Virtual memory manager (VMM) with per-process page tables and isolated address spaces
- Custom heap allocator (`malloc`/`free`) living at 0x200000

**Userland**
- ELF64 binary loader — loads and executes 64-bit ELF programs
- Minimal `libc` + `crt0` written from scratch for user programs
- `syscall` interface (`write`, `exit`, etc.) via the `syscall` instruction
- Shadow Compiler (`SHC`) — compiles `.shadow` source files from within the OS

**Drivers & I/O**
- PS/2 keyboard and mouse — both polled and interrupt-driven
- ATA PIO hard disk driver (read/write)
- VGA text mode terminal — 80×25 with 200-row scrollback buffer (Shift+PgUp/PgDn)
- Framebuffer driver — 1024×768 @ 32bpp
- Intel e1000 NIC driver (basic, MAC hardcoded to `52:54:00:12:34:56`)

**Filesystem**
- FAT32 filesystem mounted at LBA 256 — read/write file support

**GUI Desktop Environment**
- Full windowed desktop at 1024×768
- Window management — drag, 8-direction resize, minimize/maximize/close (macOS-style traffic lights)
- Application launcher (top-left Menu button)
- System tray — clock, volume, and network indicators (top-right)
- Desktop icons — double-click to launch apps
- Right-click context menu on desktop
- Bottom pill-style dock with active app indicators
- System monitor — CPU, memory, and disk usage with progress bars
- Color scheme: deep navy `#0a0e1a` with cyan `#00e5ff` and violet `#7c3aed` accents

---

## Project Structure

```
ENGINE/
├── boot/
│   └── boot.S          # 16-bit bootloader (real → protected → long mode)
├── kernel/
│   ├── entry.S         # kernel entry point, stack setup
│   ├── isr.S           # interrupt handlers + syscall stub
│   ├── kernel.c        # core kernel (VGA, PS/2, ATA, FAT32, shell, GUI event loop)
│   ├── heap.c          # malloc/free
│   ├── pmm.c           # physical memory manager
│   ├── vmm.c           # virtual memory manager + page tables
│   ├── tss.c           # task state segment
│   ├── process.c       # process management (PCBs, fork, exec)
│   ├── elf.c           # ELF64 loader
│   ├── scheduler.c     # round-robin preemptive scheduler
│   ├── syscall.c       # syscall handler
│   ├── input.c         # keyboard input
│   ├── net.c           # e1000 NIC driver
│   ├── fbdev.c         # framebuffer driver
│   └── gui.c           # full desktop environment
├── user/
│   ├── crt0.S          # user program startup
│   ├── libc.c / libc.h # minimal libc
│   ├── hello.c         # hello world example
│   ├── myprogram.c     # example user program
│   └── shc.c           # Shadow Compiler
├── include/
│   ├── kernel.h        # types and declarations
│   ├── font8x8.h       # 8x8 bitmap font for framebuffer
│   └── input.h         # keyboard scancodes
├── linker.ld           # kernel linker script
└── Makefile
```

---

## Building

**Dependencies:**
- `gcc` (64-bit target)
- `binutils` (`as`, `ld`, `objcopy`)
- `mtools` (FAT32 image manipulation)
- `qemu-system-x86_64`
- `make`
- `python3` (boot sector size validation)

**Build:**
```bash
make clean
make all
```

This produces `engine.img` — a bootable disk image.

---

## Running

```bash
make run
```

Launches QEMU with the disk image. At the shell prompt inside ENGINE, type `gui` to start the desktop environment.

**Other targets:**
```bash
make run-quiet    # GTK display, less verbose
make run-sdl      # SDL display backend
make run-nographic  # serial console only
```

---

## User Programs

Example programs live in `user/`. Build and add them to the disk image:

```bash
make hello        # builds HELLO_C
make myprog       # builds MYPROGRAM
make shc          # builds the Shadow Compiler
make programs     # adds HELLO_C + MYPROGRAM to disk
make compiler     # adds SHC + sample .shadow files
```

Run them inside ENGINE's shell:
```
elf HELLO_C
elf MYPROGRAM
elf SHC
```

To add a custom ELF binary:
```bash
make addprog PROG=./MYPROG
```

---

## Memory Layout

| Region | Address |
|---|---|
| Bootloader | `0x7C00` |
| Kernel load | physical low memory |
| Heap | `0x200000` (2 MB base, 2 MB size) |
| User programs | `0x400000` |
| Stack | grows down from `0x700000` |
| Framebuffer | `0xA0000000` (kernel virtual) |

---

## Known Limitations

- Max 8 windows open simultaneously (`GUI_MAX_WINDOWS`)
- Max 128 widgets total (`GUI_MAX_WIDGETS`)
- No hardware GPU acceleration — all rendering is software into the framebuffer
- System tray clock displays `00:00:00` (RTC not yet hooked up)
- Network and volume indicators are visual only
- FAT32 support is functional but basic

---

## Notable Implementation Notes

**The `-fno-pic` flag is critical.** Without it, GCC emits GOT-indirect relocations. Calling functions through function pointers (e.g. `null_isr`) goes through a non-existent GOT, causing the IDT gates to load raw code bytes as addresses instead — triggering an immediate GPF cascade on the first timer tick. Took a week to track down.

**binutils `.note.gnu.property` injection.** By default, binutils injects a `.note.gnu.property` section into the boot binary, inflating it from 512 bytes to 1064 bytes — causing the BIOS to triple fault. Workaround: link to a temp ELF first, then strip to raw binary with `objcopy`.

---

## Why?

To understand how operating systems actually work at the lowest level. Turns out it's a lot of edge cases, magic numbers, and late nights — but extremely satisfying when it all clicks.

---

## License

GPL-3.0 — see [LICENSE](LICENSE)
