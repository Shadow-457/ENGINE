# kernel/

Kernel source files. Every `.c` file is compiled and linked into `kernel.bin`.

See [`../docs/kernel.md`](../docs/kernel.md) for a full description of each file.

| File | What it does |
|------|-------------|
| `kernel.c` | VGA terminal, ATA/FAT32, PS/2 mouse, `kernel_main()` |
| `entry.S` | 64-bit kernel entry point |
| `isr.S` | IDT stubs for all 256 interrupts |
| `shell.c` | Interactive shell |
| `syscall.c` | Syscall dispatch (Linux-compatible numbers) |
| `process.c` | Process table and PCB |
| `scheduler.c` | Round-robin scheduler |
| `fork_exec.c` | `fork()` / `exec()` |
| `elf.c` | ELF64 loader |
| `signal.c` | POSIX signals |
| `futex.c` | Fast userspace mutexes |
| `pipe.c` | Anonymous pipes |
| `ipc.c` | Named IPC channels |
| `pmm.c` / `pmm_enhanced.c` | Physical memory manager |
| `vmm.c` / `vmm_enhanced.c` | Virtual memory manager (4-level paging) |
| `vmalloc.c` | Kernel virtual address allocator |
| `heap.c` / `heap_enhanced.c` | Kernel heap allocator |
| `mem_safety.c` | Guard pages, canary checks |
| `swap.c` | Swap space |
| `vfs.c` | Virtual Filesystem Switch |
| `jfs.c` | Journaling Filesystem |
| `net.c` | Network stack (e1000 → Ethernet → IP → TCP → HTTP) |
| `tcpip.c` | TCP state machine |
| `e1000.c` | Intel 8254x NIC driver |
| `fbdev.c` | Framebuffer device (bochs-display) |
| `gfx.c` | 2D drawing primitives |
| `gui.c` | Window manager / compositor |
| `input.c` | Unified input event queue |
| `pngview.c` | PNG decoder |
| `sound.c` | SoundBlaster 16 driver |
| `pci.c` | PCI bus enumeration |
| `acpi.c` | ACPI table parser |
| `ps2.c` | PS/2 keyboard + mouse |
| `usb.c` | xHCI USB host controller |
| `usb_hid.c` | USB HID class driver |
| `ahci.c` | AHCI SATA driver |
| `nvme.c` | NVMe SSD driver |
| `uefi.c` | UEFI GOP framebuffer detection |
| `tss.c` | Task State Segment |
| `security.c` | Privilege enforcement |
| `resilience.c` | Watchdog + panic recovery |
| `pkgmgr.c` | Package registry |
