# Building SystrixOS

---

## Prerequisites

| Tool | Purpose |
|------|---------|
| `gcc` | Compile kernel and user C code |
| `as` (binutils) | Assemble `.S` files |
| `ld` (binutils) | Link ELF objects |
| `objcopy` (binutils) | Strip binary from ELF (boot sector) |
| `mtools` (`mcopy`, `mdir`, `mdel`) | Manipulate FAT32 image without root |
| `qemu-system-x86_64` | Run the OS |
| `python3` | Validate boot sector size (Makefile check) |

Install on Arch Linux:

```bash
sudo pacman -S gcc binutils mtools qemu-desktop python
```

Install on Ubuntu/Debian:

```bash
sudo apt install gcc binutils mtools qemu-system-x86 python3
```

---

## Build Targets

| Target | What it does |
|--------|-------------|
| `make` | Build `systrix.img` (default) |
| `make run` | Build + launch QEMU (SDL display) |
| `make run-quiet` | Launch QEMU with GTK window |
| `make run-sdl` | Launch QEMU with SDL display |
| `make run-nographic` | Launch QEMU in terminal (serial only) |
| `make hello` | Build `examples/c/hello.c` → `HELLO_C` |
| `make myprog` | Build `examples/c/myprogram.c` → `MYPROGRAM` |
| `make posix_test` | Build `examples/c/posix_test.c` → `POSIX_TEST` |
| `make programs` | Build all example programs and embed in disk |
| `make ipc_demo` | Build IPC echo server/client and embed |
| `make lynx` | Build the SystrixLynx browser → `LYNX` |
| `make addprog PROG=./myapp` | Add any ELF binary to the disk image |
| `make synchome` | Upload `home/` folder contents to FAT32 root |
| `make run-home` | `synchome` + `run` |
| `make clean` | Remove all build artifacts |

---

## How the Disk Image is Built

```
boot.bin   (512 bytes)   = boot/boot.S compiled with as --32
kernel.bin (raw binary)  = all kernel/*.o + libc/*.o linked with linker.ld
fat32.img  (64 MB)       = mkfs.fat -F 32, holds user programs + home/ files

systrix.img layout:
  sector 0         boot.bin        (MBR)
  sectors 1..N     kernel.bin      (loaded by bootloader)
  sector 512+      fat32.img       (mounted at / in the OS)
```

The bootloader reads `kernel.bin` from sectors 1..N using BIOS INT 13h, switches to 64-bit long mode, and jumps to the kernel entry point in `kernel/entry.S`.

---

## Compiler Flags

**Kernel (`CFLAGS`):**
```
-m64                    x86-64 output
-ffreestanding          no host libc
-fno-stack-protector    no canary (kernel manages its own stack)
-mno-red-zone           no red zone (used by ISRs)
-fno-pic                no GOT/PLT (bare-metal, fixed addresses)
-nostdlib -nostdinc     no host headers or startup files
-O2                     optimise
-Iinclude               find kernel.h
-DSYSTRIX_KERNEL        gates kernel-only code in systrix_libc.c
-mno-mmx -mno-sse ...   no SIMD (kernel doesn't save/restore XMM regs)
```

**User programs (`UCFLAGS`):**
```
-m64 -O2 -ffreestanding -fno-stack-protector -mno-red-zone
-nostdlib -nostdinc
-Iuser                  find libc.h
```

---

## Adding a New Kernel File

1. Create `kernel/mymodule.c` (include `../include/kernel.h`)
2. Add `kernel/mymodule.o` to `KERNEL_C_OBJS` in `Makefile`
3. Declare any public functions in `include/kernel.h`
4. `make` — it will compile and link automatically

---

## Disk Image FAT32 Notes

- FAT32 names are **8.3 uppercase**. `mcopy` auto-uppercases during `synchome`.
- The FAT32 partition starts at **sector 512** in `systrix.img`.
- `make addprog PROG=./myelf` uses `mcopy` to copy the binary into the partition root, then re-stamps the partition back into `systrix.img`.
- The shell `elf` command searches the FAT32 root for the named file, loads it into memory, and `execve`s it.

---

## QEMU Configuration

From the Makefile:

```makefile
NIC   = -netdev user,id=net0 -device e1000,netdev=net0,mac=52:54:00:12:34:56
DISP  = -device bochs-display,xres=1024,yres=768
AUDIO = -device sb16,audiodev=snd0 -audiodev sdl,id=snd0
```

- **NIC:** Fixed MAC ensures `net_init()` reads the right address from RAL0/RAH0.
- **Display:** `bochs-display` gives a pure linear framebuffer with no legacy VGA quirks.
- **Audio:** SoundBlaster 16 over SDL audio backend.
- **Memory:** `-m 1G` — the kernel identity-maps the first 2 GB.
- **Machine:** `-machine pc,accel=tcg` — TCG (software) acceleration; no KVM required.
