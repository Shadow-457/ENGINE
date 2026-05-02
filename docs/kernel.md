# Kernel Subsystems

The SystrixOS kernel is a monolithic x86-64 kernel written in C with assembly stubs for low-level entry points. All source lives in `kernel/`.

---

## Entry & Interrupts

| File | Purpose |
|------|---------|
| `entry.S` | 64-bit kernel entry point; sets up GDT, stack, and calls `kernel_main()` |
| `isr.S` | Interrupt Service Routines — 256 IDT stubs, context save/restore, dispatches to C handlers |
| `tss.c` | Task State Segment setup; sets RSP0 so the CPU switches to the kernel stack on ring-3 → ring-0 transitions |

---

## Core Kernel

### `kernel.c`
The largest file (~110 KB). Contains:
- VGA text-mode terminal (80×25, scrollback buffer of 200 rows, Shift+PgUp/PgDn)
- PS/2 mouse driver (polled, text-mode cursor)
- ATA PIO disk driver
- FAT32 filesystem driver
- `kernel_main()` — initialises every subsystem in order

### `shell.c`
Interactive shell. Features: command history, I/O redirection (`<`, `>`), shell variables (`$VAR`), pipelines. Built-in commands are registered as a table of `{name, handler}` pairs.

---

## Memory Management

| File | Purpose |
|------|---------|
| `pmm.c` | Physical Memory Manager — bitmap allocator over all RAM |
| `pmm_enhanced.c` | Extended PMM with zone awareness and NUMA hints |
| `vmm.c` | Virtual Memory Manager — 4-level page tables (PML4→PDPT→PD→PT) |
| `vmm_enhanced.c` | Enhanced VMM: CoW pages, demand paging, huge pages |
| `vmalloc.c` | Kernel virtual address space allocator (like Linux `vmalloc`) |
| `heap.c` | Kernel heap — simple slab/buddy allocator |
| `heap_enhanced.c` | Enhanced heap with per-size-class free lists |
| `mem_safety.c` | Guard pages, canary checks, use-after-free detection |
| `swap.c` | Swap space management — evict cold pages to disk |

**Address layout (virtual):**

```
0x0000_0000_0000_0000  user space  (0 – 128 TB)
0xFFFF_8000_0000_0000  kernel direct map
0xFFFF_C000_0000_0000  vmalloc range
0xFFFF_FFFF_8000_0000  kernel image
```

---

## Process Management

| File | Purpose |
|------|---------|
| `process.c` | Process table, PCB (Process Control Block), PID allocator |
| `scheduler.c` | Round-robin scheduler; `schedule()` picks the next runnable process |
| `fork_exec.c` | `fork()` (copy-on-write) and `exec()` (ELF loader wrapper) |
| `elf.c` | ELF64 binary loader — parses PT_LOAD segments, sets up user stack |
| `signal.c` | POSIX-compatible signals: `kill()`, `sigaction()`, signal delivery on return from kernel |
| `futex.c` | Fast userspace mutexes — `futex_wait()` / `futex_wake()` |
| `pipe.c` | Anonymous pipes — circular ring buffer, blocking read/write |
| `ipc.c` | Named IPC channels — `ipc_register()`, `ipc_send()`, `ipc_recv()` |
| `syscall.c` | Syscall dispatch table; handles `syscall` instruction from userspace |

---

## Filesystem

| File | Purpose |
|------|---------|
| `vfs.c` | Virtual Filesystem Switch — mount table, `open`/`read`/`write`/`readdir` abstraction |
| `jfs.c` | Journaling Filesystem — write-ahead log, atomic transactions on top of FAT32 |

The FAT32 driver lives inside `kernel.c`. JFS adds journaling on top for crash consistency.

---

## Networking

See [`network.md`](network.md) for a full breakdown.

| File | Purpose |
|------|---------|
| `net.c` | e1000 NIC driver + full network stack (Ethernet → TCP/IP → HTTP) |
| `tcpip.c` | TCP state machine (SYN/SYN-ACK/ACK, FIN, retransmit) |
| `e1000.c` | Intel 8254x (e1000) register-level driver — TX/RX descriptor rings |

---

## GUI & Graphics

> ⚠️ **Not working** — see [`gui.md`](gui.md) for details and current status.

| File | Purpose |
|------|---------|
| `fbdev.c` | Framebuffer device (bochs-display MMIO) — **not working** |
| `gfx.c` | 2D drawing primitives — **not working** |
| `gui.c` | Window manager / compositor — **not working** |
| `input.c` | Unified input event queue (works in text mode; GUI path not working) |
| `pngview.c` | Minimal PNG decoder — **not working** (depends on framebuffer) |

---

## Hardware Drivers

| File | Purpose |
|------|---------|
| `pci.c` | PCI bus enumeration — scans 256 buses × 32 devices × 8 functions |
| `acpi.c` | ACPI table parser — finds RSDT/XSDT, MADT, FADT; powers off via PM1a_CNT |
| `ps2.c` | PS/2 controller — keyboard (scan-code set 2) and mouse (3-byte packets) |
| `usb.c` | xHCI USB host controller driver — ring management, port reset, device enumeration |
| `usb_hid.c` | USB HID class driver — translates HID reports to keyboard/mouse events |
| `sound.c` | SoundBlaster 16 driver — DSP init, DMA buffer, PCM playback |
| `ahci.c` | AHCI SATA driver — port init, command list, FIS-based transfers |
| `nvme.c` | NVMe SSD driver — admin/IO queues, namespace discovery, read/write |
| `uefi.c` | UEFI GOP framebuffer detection (for non-legacy boot paths) |

---

## Security & Resilience

| File | Purpose |
|------|---------|
| `security.c` | Capability checks, ring-3 privilege enforcement, basic syscall filtering |
| `resilience.c` | Watchdog timer, panic recovery, kernel self-test hooks |
| `pkgmgr.c` | In-kernel package registry — tracks installed "packages" (named ELF binaries) |
