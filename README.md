# SoftTail OS — Deep Reference Manual

> x86-64 microkernel written in C and AT&T assembly.  
> Boots from a raw MBR disk image, runs ELF64 user-space binaries, has a GUI, networking, audio, a browser, and its own scripting language.

---

## Table of Contents

1. [What Is SoftTail OS?](#1-what-is-engine-os)
2. [Repository Layout](#2-repository-layout)
3. [Building from Source](#3-building-from-source)
4. [Disk Image Layout](#4-disk-image-layout)
5. [Boot Process — Step by Step](#5-boot-process--step-by-step)
6. [Memory Map](#6-memory-map)
7. [Kernel Subsystems](#7-kernel-subsystems)
   - 7.1 [Physical Memory Manager (PMM)](#71-physical-memory-manager-pmm)
   - 7.2 [Virtual Memory Manager (VMM)](#72-virtual-memory-manager-vmm)
   - 7.3 [Heap Allocator](#73-heap-allocator)
   - 7.4 [vmalloc / MMIO Mapper](#74-vmalloc--mmio-mapper)
   - 7.5 [Memory Safety Layer](#75-memory-safety-layer)
   - 7.6 [Process & Scheduler](#76-process--scheduler)
   - 7.7 [Syscall Interface](#77-syscall-interface)
   - 7.8 [Virtual File System (VFS)](#78-virtual-file-system-vfs)
   - 7.9 [FAT32 Driver](#79-fat32-driver)
   - 7.10 [JFS (Journaling File System)](#710-jfs-journaling-file-system)
   - 7.11 [Signals & Pipes](#711-signals--pipes)
   - 7.12 [Futex & pthreads](#712-futex--pthreads)
   - 7.13 [IPC (Inter-Process Communication)](#713-ipc-inter-process-communication)
   - 7.14 [Networking (E1000 + TCP/IP)](#714-networking-e1000--tcpip)
   - 7.15 [USB (EHCI + XHCI + HID + MSC)](#715-usb-ehci--xhci--hid--msc)
   - 7.16 [PCI / PCIe Enumeration](#716-pci--pcie-enumeration)
   - 7.17 [AHCI SATA Driver](#717-ahci-sata-driver)
   - 7.18 [NVMe Driver](#718-nvme-driver)
   - 7.19 [Audio (OPL2 FM + SB16 PCM Mixer)](#719-audio-opl2-fm--sb16-pcm-mixer)
   - 7.20 [Framebuffer / Display (fbdev)](#720-framebuffer--display-fbdev)
   - 7.21 [GUI Desktop](#721-gui-desktop)
   - 7.22 [PS/2 Keyboard & Mouse](#722-ps2-keyboard--mouse)
   - 7.23 [Input Subsystem](#723-input-subsystem)
   - 7.24 [ACPI](#724-acpi)
   - 7.25 [TSS & Privilege Switching](#725-tss--privilege-switching)
   - 7.26 [Security (KASLR, SMAP/SMEP, Stack Canaries)](#726-security-kaslr-smapsmep-stack-canaries)
   - 7.27 [Resilience (SMP, OOM, Watchdog, Panic)](#727-resilience-smp-oom-watchdog-panic)
   - 7.28 [Swap](#728-swap)
   - 7.29 [Package Manager](#729-package-manager)
8. [Shell Reference](#8-shell-reference)
9. [User-Space ABI](#9-user-space-abi)
   - 9.1 [ELF64 Loader](#91-elf64-loader)
   - 9.2 [crt0 & Program Startup](#92-crt0--program-startup)
   - 9.3 [libc.h / libc.c](#93-libch--libcc)
   - 9.4 [libm.h — Math Library](#94-libmh--math-library)
   - 9.5 [pthread.h — Threading](#95-pthreadh--threading)
   - 9.6 [tls.h — TLS 1.2 Client](#96-tlsh--tls-12-client)
   - 9.7 [ipc.h — IPC Messaging](#97-ipch--ipc-messaging)
   - 9.8 [gfx.h — Graphics API](#98-gfxh--graphics-api)
   - 9.9 [sound.h — Audio API](#99-soundh--audio-api)
10. [Writing a User Program](#10-writing-a-user-program)
11. [SHC — The Shadow Scripting Compiler](#11-shc--the-shadow-scripting-compiler)
12. [Browser](#12-browser)
13. [Syscall Table](#13-syscall-table)
14. [FAQ](#14-faq)
15. [Known Bugs & Limitations](#15-known-bugs--limitations)
16. [Roadmap](#16-roadmap)

---

## 1. What Is SoftTail OS?

SoftTail OS is a from-scratch, x86-64 microkernel written entirely in C (kernel) and GNU assembly (boot + ISR stubs). It uses a microkernel architecture with IPC message-passing between components, allowing drivers and services to communicate via well-defined 64-byte messages. It targets the `qemu-system-x86_64` emulator and any real PC that supports legacy BIOS boot with LBA disk access.

**What it has:**

- MBR bootloader → 32-bit protected mode → 64-bit long mode in a single stage
- E820 memory map parsed at boot — uses **all available RAM** given to QEMU
- Preemptive multitasking with a round-robin scheduler
- Full x86-64 syscall ABI (Linux-compatible numbering for ~60 calls)
- Demand-paging VMM with Copy-on-Write fork, ASLR, huge-page support
- FAT32 read/write filesystem + experimental JFS (Journaling FS)
- VFS layer with pluggable `inode_ops_t` backends
- E1000 NIC driver with a hand-written TCP/IP + DNS + DHCP stack
- USB (EHCI + XHCI) with HID keyboard/mouse and Mass Storage (USB flash drives)
- PCI/PCIe device enumeration with BAR sizing and ECAM support
- AHCI SATA driver (DMA, multi-sector, error recovery)
- NVMe PCIe SSD driver (submission/completion queues)
- SB16 PCM audio mixer + OPL2 FM synthesis
- 1024×768 / 1080p framebuffer GUI desktop (bochs-display)
- Built-in web browser (TCP, TLS 1.2, HTML/CSS renderer)
- IPC message-passing microkernel layer
- Signals (POSIX-compatible), pipes, futex, pthreads
- KASLR, SMAP, SMEP, stack canaries, red-zone guards
- Kernel watchdog, OOM killer, basic SMP bringup
- A self-hosted scripting language compiler (SHC / Shadow)

**What it does NOT have (yet):**

- Multi-core scheduling (SMP cores are brought up but only core 0 runs the scheduler)
- GPU acceleration (framebuffer only)
- HD Audio driver (currently SB16 + OPL2 only)
- NTFS / ext4 filesystem support
- Real POSIX process groups, sessions, or TTYs

---

## 2. Repository Layout

```
SoftTail-0.1/
├── boot/
│   └── boot.S          # 512-byte MBR bootloader (16-bit real mode)
├── kernel/
│   ├── entry.S         # Long-mode entry, GDT, paging, BSS clear, jumps to kernel_main()
│   ├── isr.S           # IDT stubs for 256 vectors + syscall dispatch table
│   ├── kernel.c        # VGA, ATA PIO, FAT32, VFS, shell, kernel_main()
│   ├── heap.c          # Simple bump-style kernel heap
│   ├── heap_enhanced.c # dlmalloc-style allocator with coalescing
│   ├── pmm.c           # Physical memory manager (buddy allocator, E820)
│   ├── pmm_enhanced.c  # PMM zones, watermarks, poison pages, stats
│   ├── vmm.c           # Page table management, VMA tracking, CoW fork
│   ├── vmm_enhanced.c  # ASLR, huge pages, mprotect, guard pages
│   ├── vmalloc.c       # Kernel virtual address space allocator (vmalloc/vfree)
│   ├── mem_safety.c    # Red-zone guards, quarantine, leak tracking
│   ├── tss.c           # Task State Segment for ring-3 → ring-0 stack switch
│   ├── process.c       # PCB table, process create/destroy
│   ├── elf.c           # ELF64 loader for user binaries
│   ├── scheduler.c     # Preemptive round-robin scheduler
│   ├── syscall.c       # All syscall implementations
│   ├── ipc.c           # Message-passing IPC (64-byte messages)
│   ├── signal.c        # POSIX signals (kill, sigaction, sigprocmask)
│   ├── pipe.c          # Anonymous kernel pipes
│   ├── futex.c         # Linux-compatible futex (FUTEX_WAIT / FUTEX_WAKE)
│   ├── fork_exec.c     # fork(), execve(), clone(), wait4()
│   ├── input.c         # Unified input ring (keyboard, mouse, gamepad)
│   ├── ps2.c           # PS/2 i8042 controller (keyboard + mouse)
│   ├── usb.c           # EHCI + XHCI USB host controller driver
│   ├── usb_hid.c       # USB HID (keyboard/mouse) class driver
│   ├── pci.c           # PCI/PCIe enumeration + BAR sizing
│   ├── ahci.c          # AHCI SATA controller driver (DMA)
│   ├── nvme.c          # NVMe PCIe SSD driver
│   ├── e1000.c         # Intel E1000 NIC driver
│   ├── net.c           # TCP/IP stack (ARP, IP, ICMP, TCP, UDP, DHCP, DNS)
│   ├── fbdev.c         # Framebuffer device (bochs-display)
│   ├── gui.c           # Desktop GUI, window manager, widgets
│   ├── sound.c         # OPL2 FM synth + SB16 PCM mixer
│   ├── acpi.c          # ACPI table parser, I/O APIC, reboot/shutdown
│   ├── uefi.c          # UEFI stub (not used in current BIOS boot path)
│   ├── vfs.c           # VFS inode layer (mount, open, read, write, stat)
│   ├── jfs.c           # Journaling filesystem (second partition)
│   ├── swap.c          # Swap-to-disk (demand eviction)
│   ├── security.c      # KASLR, SMAP/SMEP, copy_from/to_user, canaries
│   ├── shell.c         # Extended shell (sourced into kernel_main loop)
│   ├── resilience.c    # SMP init, watchdog, OOM killer, kernel_panic
│   └── pkgmgr.c        # In-memory package registry
├── include/
│   └── kernel.h        # Single mega-header: all types, structs, prototypes
├── user/
│   ├── crt0.S          # ELF entry point _start, calls main(), then sys_exit
│   ├── libc.h          # Full C library header (types, syscall wrappers, stdio, …)
│   ├── libc.c          # Implementations: printf, malloc, string funcs, sockets, …
│   ├── malloc.c        # dlmalloc port for user space
│   ├── libm.h          # Header-only math library (sin, cos, sqrt, pow, …)
│   ├── pthread.h       # Header-only pthreads on top of clone + futex
│   ├── tls.h           # Header-only TLS 1.2 client (AES-GCM, SHA-256, RSA)
│   ├── ipc.h           # IPC message structs + send/recv wrappers
│   ├── gfx.h           # Graphics syscall wrappers (blit, flip, tilemap)
│   ├── sound.h         # Audio syscall wrappers (OPL2 + PCM mixer)
│   ├── hello.c         # Minimal "Hello, world!" example
│   ├── myprogram.c     # Demo program
│   ├── posix_test.c    # POSIX compliance test suite
│   ├── echo_server.c   # IPC echo server demo
│   ├── echo_client.c   # IPC echo client demo
│   ├── shc.c           # SHC (Shadow) scripting language compiler (~43 KB)
│   ├── fib.shadow      # Fibonacci example in the Shadow language
│   └── hello.shadow    # Hello-world in Shadow
├── browser/
│   ├── browser.c       # Main browser: HTTP/HTTPS fetch, HTML parse, render loop
│   ├── html.h          # HTML tokenizer + DOM builder
│   ├── css.h           # CSS property parser
│   ├── layout.h        # Block/inline box layout engine
│   ├── render.h        # Framebuffer renderer (text + boxes)
│   └── net.h           # Browser-side socket helpers
├── linker.ld           # Kernel linker script (loads at 0x8000, binary output)
├── Makefile            # Build system
└── work.md             # Dev log of completed features
```

---

## 3. Building from Source

### Prerequisites

```bash
# Arch Linux
sudo pacman -S gcc binutils mtools qemu

# Ubuntu / Debian
sudo apt install gcc binutils mtools qemu-system-x86

# Fedora
sudo dnf install gcc binutils mtools qemu-system-x86
```

### Build Targets

| Command | What it does |
|---|---|
| `make` | Build `softtail.img` (bootable disk image) |
| `make run` | Build + launch in QEMU (SDL display) |
| `make run-quiet` | Run with GTK display |
| `make run-sdl` | Run with SDL display explicitly |
| `make run-nographic` | Run with serial console only |
| `make browser` | Build the BROWSER user binary |
| `make addbrowser` | Inject BROWSER into the FAT32 partition |
| `make compiler` | Build SHC + inject SHC + sample .shadow files |
| `make programs` | Build HELLO_C, MYPROGRAM, POSIX_TEST and inject all |
| `make hello` | Build HELLO_C binary only |
| `make addprog PROG=./mybinary` | Inject any ELF binary into the disk |
| `make clean` | Remove all build artifacts |

### Full Workflow (first time)

```bash
git clone <repo>
cd SoftTail-0.1

# Build kernel + disk image
make

# Optionally inject the browser and compiler
make browser addbrowser
make compiler

# Launch
make run
```

### QEMU Flags Used

```
-drive format=raw,file=softtail.img,if=ide   # raw disk image via IDE
-m 128M                                     # 128 MB RAM (increase freely: -m 512M, -m 2G)
-machine pc,accel=tcg                       # PC machine, software emulation
-device bochs-display,xres=1024,yres=768   # 1024×768 framebuffer
-netdev user,id=net0                        # user-mode NAT networking
-device e1000,netdev=net0,mac=52:54:00:12:34:56
-device sb16,audiodev=snd0                 # SB16 sound card
-audiodev sdl,id=snd0
```

> **Tip:** To give SoftTail OS more RAM, add `-m 256M` (or any value) to the QEMU run line in the Makefile. The E820 PMM will discover and use all of it automatically.

### Compiler Flags Explained

```makefile
CFLAGS = -m64                   # 64-bit output
         -ffreestanding         # no host libc
         -fno-stack-protector   # no __stack_chk_guard (kernel handles its own)
         -mno-red-zone          # no red zone (ISRs write below RSP)
         -fno-pic               # no GOT — prevents R_X86_64_REX_GOTPCRELX faults
         -nostdlib -nostdinc    # no host headers or runtime
         -O2
         -mno-mmx -mno-sse -mno-sse2 -mno-avx  # no SIMD (kernel doesn't save XMM)
         -Wall -Wextra
```

The `-fno-pic` flag is critical. Without it, GCC emits `R_X86_64_REX_GOTPCRELX` relocations for function pointers. Since the kernel has no GOT, this causes IDT gates to be loaded with raw code bytes instead of addresses, producing a triple fault on the first timer tick.

---

## 4. Disk Image Layout

```
softtail.img (64 MB raw)
│
├── LBA 0           boot.bin   (512 bytes — MBR bootloader + partition table)
├── LBA 1–511       kernel.bin (256 KB — entire kernel, loaded to 0x8000)
└── LBA 512–131071  fat32.img  (64 MB — FAT32 user partition)

Partition Table (embedded in boot.S at offset 446):
  Partition 1: type 0x83 (Linux), LBA start=1,   size=63 sectors  → kernel
  Partition 2: type 0x0C (FAT32), LBA start=512, size=67456 sectors → user data
```

The kernel binary is loaded directly to physical address `0x8000` by the bootloader (four 128-sector LBA reads). The FAT32 partition starts at LBA 512 (sector offset 0x40000 bytes = 256 KB into the image).

---

## 5. Boot Process — Step by Step

### Stage 1: boot.S (16-bit real mode, runs at 0x7C00)

1. Saves BIOS drive ID (`dl`) into `[drive_save]`
2. Checks for INT 13h LBA extensions (`AH=0x41`) — halts if not supported
3. Reads LBA sectors 1–512 (the kernel) into physical memory starting at `0x8000` using four 128-sector `INT 13h / AH=0x42` calls
4. Queries the E820 memory map (`INT 15h / EAX=0xE820`) and stores up to 32 entries at physical `0x0500`
5. Jumps to `0x0000:0x8000` — transfers control to `entry.S`

### Stage 2: entry.S (transition to 64-bit long mode)

1. Sets up a minimal flat GDT (null, 64-bit code, 64-bit data)
2. Enables A20 line
3. Sets up identity-mapped page tables (first 4 GB, 2 MB huge pages)
4. Sets `CR4.PAE`, loads `CR3`, sets `EFER.LME`, sets `CR0.PG` → enters long mode
5. Performs a far jump to reload `CS` with the 64-bit code segment
6. Zeroes the BSS section (`_bss_start` to `_bss_end`)
7. Sets up a temporary kernel stack
8. Calls `kernel_main()`

### Stage 3: kernel_main() (C code, full 64-bit)

Initialization order:
```
heap_init()          → bump allocator at 0x200000
pmm_init()           → reads E820, initializes buddy allocator
vmm_init_kernel()    → sets up kernel page tables
tss_init()           → sets RSP0 for ring-0 re-entry on syscall/interrupt
syscall_init()       → loads LSTAR/SFMASK for SYSCALL instruction
ipc_init()           → zeroes IPC server table
process_init()       → zeroes PCB table at 0x300000
scheduler_init()     → sets up round-robin queue
vfs_init()           → zeroes VFS inode/mount/fd tables
fat32_init()         → reads FAT32 BPB, finds root cluster
jfs_init()           → initializes journaling FS on second partition
net_start()          → initializes E1000 NIC, runs DHCP
pci_scan_all()       → enumerates all PCI devices
ahci_init()          → finds SATA controller (class 01:06:01), starts ports
nvme_init()          → finds NVMe controller (class 01:08:02), sets up queues
ps2_init()           → i8042 full init, enables keyboard + mouse
scheduler_start()    → enables interrupts (STI), PIT fires at ~100 Hz
usb_full_init()      → EHCI/XHCI enumeration, HID, Mass Storage
smp_init()           → brings up additional CPU cores (they spin-wait)
watchdog_init()      → starts watchdog timer
gui_init()           → sets up desktop, taskbar, icons
→ shell loop         → interactive command prompt
```

---

## 6. Memory Map

```
Physical Address         Contents
─────────────────────────────────────────────────────────
0x0000_0000              Real-mode IVT (preserved, not used after boot)
0x0000_0500              E820 memory map (up to 32 × 24-byte entries)
0x0000_7C00              MBR bootloader load address (not resident after jump)
0x0000_8000              Kernel binary (entry.S, then kernel.c, all drivers)
0x0020_0000  (2 MB)      HEAP_BASE — kernel heap (2 MB bump region)
0x0030_0000  (3 MB)      PROC_TABLE — PCB table (64 × 128-byte PCBs)
0x0040_0000  (4 MB)      RAM_START — PMM buddy allocator begins here
0x0050_0000  (5 MB)      IDT — 256 interrupt descriptor entries
0x0060_0000  (6 MB)      PMM_BITMAP — physical page bitmap
0x0070_0000  (7 MB)      PROC_STACK_TOP — user-space stack base
...
0x0080_0000+             PMM-managed free pages (given out to VMM, heap, etc.)
...
RAM_END_MAX (64 GB)      Compile-time ceiling for static array sizing
```

**Virtual Address Space (per-process):**

```
0x0040_0000              User ELF load address (Ttext=0x400000)
0x0070_0000              User stack (grows down from PROC_STACK_TOP)
ASLR mmap base           mmap() / shared libraries (randomized)
...
0xFFFF_8000_0000_0000+   Kernel virtual space (identity-mapped first 4 GB)
```

---

## 7. Kernel Subsystems

### 7.1 Physical Memory Manager (PMM)

**Files:** `kernel/pmm.c`, `kernel/pmm_enhanced.c`

The PMM is a **buddy allocator** initialized from the E820 memory map.

At boot, `pmm_init()` reads all `E820_USABLE` entries from `0x0500` and registers each page into the free list, skipping the kernel binary region (below `RAM_START = 4 MB`).

**Key functions:**

```c
u64  pmm_alloc(void);                     // alloc one 4 KB page → physical addr
u64  pmm_alloc_order(u32 order);          // alloc 2^order contiguous pages
void pmm_free(u64 phys);                  // free one page
void pmm_free_order(u64 phys, u32 order); // free 2^order pages
u32  pmm_free_pages(void);                // query free page count
u64  pmm_alloc_n(usize n);                // alloc n contiguous pages
void pmm_ref(u64 phys);                   // increment refcount (for CoW)
```

**Enhanced PMM** (`pmm_enhanced.c`) adds:
- Memory zones (DMA, Normal, High)
- Watermark-based allocation pressure
- Page poisoning (write `0xDEAD` on free, check on alloc)
- Fragmentation stats and defragmentation passes
- Per-zone allocation: `pmm_alloc_zone(zone_id, order)`

### 7.2 Virtual Memory Manager (VMM)

**Files:** `kernel/vmm.c`, `kernel/vmm_enhanced.c`

4-level x86-64 page tables (PML4 → PDPT → PD → PT). The kernel is identity-mapped at physical addresses; user processes each have their own CR3.

**VMA (Virtual Memory Area) tracking:**

Each process has a `VMA[32]` table. Flags:

| Flag | Meaning |
|---|---|
| `VMA_READ` | Page is readable |
| `VMA_WRITE` | Page is writable |
| `VMA_EXEC` | Page is executable |
| `VMA_ANON` | Anonymous mapping (zero-filled) |
| `VMA_STACK` | Stack region |
| `VMA_FILE` | File-backed mapping (demand-loaded from fd) |

**Copy-on-Write fork:** `vmm_cow_fork()` clones the parent's page tables marking all writable pages read-only. On the first write, a page fault fires, `vmm_page_fault()` detects the CoW condition, allocates a new page, copies data, and re-maps as writable.

**File-backed mmap:** When a page fault hits a `VMA_FILE` region, the fault handler reads the appropriate file offset into the new page and maps it.

**ASLR:** `vmm_aslr_mmap_base()`, `vmm_aslr_stack_base()`, `vmm_aslr_brk_base()` randomize the base addresses for each process.

**Huge pages:** `vmm_alloc_huge_page()` maps 2 MB pages directly in the PDPT; `vmm_thp_promote()` can promote a 4 KB region to a huge page transparently.

### 7.3 Heap Allocator

**Files:** `kernel/heap.c`, `kernel/heap_enhanced.c`

Two allocators exist:

**Simple heap** (`heap.c`): Bump allocator starting at `HEAP_BASE = 0x200000`. Used early in boot before PMM is fully up. `heap_malloc()` / `heap_free()` / `heap_realloc()`.

**Enhanced heap** (`heap_enhanced.c`): dlmalloc-style with:
- Free-list coalescing (merges adjacent free blocks)
- `heap_defragment()` — active compaction pass
- `heap_check_integrity()` — walks the heap checking header magic values
- `heap_print_stats()` — prints used/free bytes, largest free block

> **Rule:** The kernel uses `heap_malloc()` for everything. The enhanced heap is the default after `heap_enhanced_init()` is called.

### 7.4 vmalloc / MMIO Mapper

**File:** `kernel/vmalloc.c`

Manages the kernel's virtual address space above the identity map for:
- Non-contiguous kernel allocations (`vmalloc()` / `vfree()`)
- MMIO mappings (`vmalloc_map_mmio()` / `vmalloc_unmap_mmio()`)
- Leak tracking with tags: every allocation carries a `const char *tag` string

```c
void *vmalloc(u64 size, const char *tag);
void  vfree(void *addr);
void *vmalloc_map_mmio(u64 phys, u64 size, const char *tag);
void  vmalloc_dump_leaks(void);   // print un-freed allocations
```

### 7.5 Memory Safety Layer

**File:** `kernel/mem_safety.c`

Sits on top of the allocator to catch bugs:
- **Red zones**: `mem_safety_redzone_fill()` writes a known pattern before/after allocations; `mem_safety_redzone_check()` verifies it on free
- **Quarantine**: Freed memory is held in a quarantine list and scanned before reuse to catch use-after-free
- **Leak tracking**: `mem_safety_track()` / `mem_safety_untrack()` record all allocations with address, size, and tag
- **Pointer validation**: `mem_safety_valid_kptr()` / `mem_safety_valid_uptr()` check that a pointer lies in the expected range before a syscall uses it

### 7.6 Process & Scheduler

**Files:** `kernel/process.c`, `kernel/scheduler.c`, `kernel/tss.c`

**PCB (Process Control Block)** — 128 bytes, stored in a flat table at physical `0x300000`:

```c
typedef struct {
    u64  state;       // PSTATE_EMPTY/READY/RUNNING/DEAD/BLOCKED
    u64  kstack;      // kernel stack pointer (RSP saved on context switch)
    u64  ursp;        // user RSP (saved on syscall entry)
    u64  entry;       // entry point address
    u64  pid;         // process ID (index into PCB table)
    u64  cr3;         // page table root (physical address)
    u64  brk;         // current program break (for sbrk/brk)
    char name[16];    // process name
    u64  kbase;       // kernel stack base
    u64  vma_table;   // pointer to VMA[32] array
} PCB;
```

**Process states:**

| State | Value | Meaning |
|---|---|---|
| `PSTATE_EMPTY` | 0 | PCB slot unused |
| `PSTATE_READY` | 1 | Ready to run |
| `PSTATE_RUNNING` | 2 | Currently on CPU |
| `PSTATE_DEAD` | 3 | Exited, waiting for `wait4()` |
| `PSTATE_BLOCKED` | 4 | Sleeping on futex — scheduler skips |

**Scheduler:** Round-robin. The PIT fires at ~100 Hz (IRQ 0). The timer ISR calls `schedule()`, which saves the current process's registers into its PCB and loads the next `PSTATE_READY` process's register state, then `iretq`.

**Limits:** `PROC_MAX = 64` processes. Each gets a 4 KB kernel stack (`PROC_KSTACK_SZ`).

### 7.7 Syscall Interface

**Files:** `kernel/syscall.c`, `kernel/isr.S`

Syscalls use the x86-64 `SYSCALL` instruction (not `INT 0x80`). On `SYSCALL`:
- CPU saves user `RIP` → `RCX`, `RFLAGS` → `R11`
- Jumps to `LSTAR` (set in `syscall_init()` via `wrmsr`)
- The stub in `isr.S` saves all registers, switches to the kernel stack (RSP0 from TSS), and dispatches through a function pointer table indexed by `RAX`

**Argument passing:** follows Linux x86-64 ABI:
```
rax = syscall number
rdi, rsi, rdx, r10, r8, r9 = args 1–6
return value in rax
```

**Validation:** Before dispatching, `syscall_validate_args()` checks that pointer arguments fall within valid user-space ranges.

### 7.8 Virtual File System (VFS)

**File:** `kernel/vfs.c`, plus `vfs_*` functions in `kernel/kernel.c`

The VFS provides a uniform interface over multiple filesystem backends via `inode_ops_t`:

```c
typedef struct inode_ops {
    i64 (*open)   (vfs_inode_t *, u64 flags);
    i64 (*read)   (vfs_inode_t *, void *buf, usize n, usize off);
    i64 (*write)  (vfs_inode_t *, const void *buf, usize n, usize off);
    i64 (*stat)   (vfs_inode_t *, void *statbuf);
    i64 (*readdir)(vfs_inode_t *, void *buf, usize n);
    i64 (*create) (vfs_inode_t *, const char *name, u16 mode);
    i64 (*unlink) (vfs_inode_t *, const char *name);
    i64 (*mkdir)  (vfs_inode_t *, const char *name, u16 mode);
} inode_ops_t;
```

Mount points:
- `/` → FAT32 (registered by `vfs_register_fat32()`)
- `/jfs` → JFS (registered by `vfs_register_jfs()`)

File descriptors are stored per-process in `fd_table[32]` (max 32 open files per process).

### 7.9 FAT32 Driver

**File:** `kernel/kernel.c` (embedded, ~600 lines)

Full FAT32 read/write implementation:
- Reads BPB (BIOS Parameter Block) at boot to find root cluster, sectors-per-cluster, FAT offset
- Cluster chain traversal via `fat32_next_cluster()`
- Supports subdirectories (cd, mkdir, rmdir, ls in subdirs)
- 8.3 filename format only (`format_83_name()` converts to uppercase padded names)
- `fat32_create_file()`, `fat32_delete_file()`, `fat32_rename()`
- Dirty cluster writeback via ATA PIO (legacy) or AHCI (preferred)

> **Note:** Long filename (LFN) entries are ignored — all filenames must be 8.3 compatible.

### 7.10 JFS (Journaling File System)

**File:** `kernel/jfs.c`

A simple append-log journaling filesystem on the second disk partition. Supports:
- `jfs_create()`, `jfs_read()`, `jfs_write()`, `jfs_unlink()`, `jfs_mkdir()`
- `jfs_journal_flush()` — commits pending journal entries to stable storage

Mounted at `/jfs` by `vfs_register_jfs()`.

### 7.11 Signals & Pipes

**Files:** `kernel/signal.c`, `kernel/pipe.c`

**Signals:** POSIX-compatible subset.
- `sys_signal()` — legacy BSD-style signal handler registration
- `sys_sigaction()` — full `sigaction_t` with `sa_flags`
- `sys_sigprocmask()` — block/unblock signal sets
- `sys_kill()` — send signal to any process
- `signal_deliver()` — called on return from syscall/interrupt to inject pending signals into user stack

**Pipes:** Anonymous bidirectional byte streams via a kernel ring buffer. Created by `sys_pipe()`, read/written through normal `read()`/`write()` fd calls.

### 7.12 Futex & pthreads

**Files:** `kernel/futex.c`, `user/pthread.h`

**Futex** (Fast Userspace Mutex): implements `FUTEX_WAIT` and `FUTEX_WAKE`.
- `FUTEX_WAIT`: if `*addr == val`, sets process state to `PSTATE_BLOCKED` and yields
- `FUTEX_WAKE`: sets up to `val` blocked waiters back to `PSTATE_READY`

**pthread.h** (header-only userspace library) builds on top of `clone()` + `futex()`:
- `pthread_create()` / `pthread_join()` / `pthread_exit()`
- `pthread_mutex_init()` / `pthread_mutex_lock()` / `pthread_mutex_trylock()` / `pthread_mutex_unlock()`
- `pthread_cond_wait()` / `pthread_cond_signal()` / `pthread_cond_broadcast()`

### 7.13 IPC (Inter-Process Communication)

**Files:** `kernel/ipc.c`, `user/ipc.h`

64-byte fixed-size messages between processes:

```c
typedef struct {
    unsigned long type;    // message type (MSG_FS_*, MSG_GUI_*, MSG_NET_*, …)
    unsigned long from;    // sender PID
    unsigned long data[6]; // payload (48 bytes)
} IpcMsg;
```

**Kernel API:**
```c
i64 sys_ipc_register(const char *name);   // register as a named server
i64 sys_ipc_lookup(const char *name);     // find a server's PID by name
i64 sys_ipc_send(u64 dest_pid, void *msg);
i64 sys_ipc_recv(void *out_msg);          // blocks until a message arrives
```

**Syscall numbers:** `SYS_IPC_REG=331`, `SYS_IPC_LOOKUP=332`, `SYS_IPC_SEND=329`, `SYS_IPC_RECV=330`

**Well-known message types:**

| Prefix | Service |
|---|---|
| `MSG_FS_*` (0x01xx) | Filesystem server |
| `MSG_GUI_*` (0x02xx) | GUI server |
| `MSG_NET_*` (0x03xx) | Network server |
| `MSG_OK` / `MSG_ERR` | Generic reply |

### 7.14 Networking (E1000 + TCP/IP)

**Files:** `kernel/e1000.c`, `kernel/net.c`

**E1000 NIC driver** (`e1000.c`):
- Finds the Intel E1000 via PCI (vendor 0x8086, device 0x100E)
- Sets up TX/RX descriptor rings (16 entries each)
- Reads MAC from RAL0/RAH0 registers
- `nic_send()` / `nic_poll()` for transmit and receive

**TCP/IP stack** (`net.c`):
- **ARP**: request/reply, cache
- **IPv4**: fragment-free, TTL=64
- **ICMP**: echo (ping) request/reply
- **UDP**: send/receive
- **DHCP**: discover → offer → request → ack (gets IP, gateway, DNS)
- **DNS**: A-record lookup via `net_dns_resolve(hostname)`
- **TCP**: 3-way handshake, send/recv buffers, connection tracking (`TcpConn`)
- **HTTP**: raw GET requests via `net_http_get()`, plus a built-in file server via `net_http_serve(port)`

**Key functions:**
```c
void net_start(void);                             // init + DHCP
int  net_ping(u32 ip);                            // ICMP echo
u32  net_dns_resolve(const char *hostname);       // DNS A lookup
int  net_http_get(u32 ip, u16 port, ...);         // HTTP GET
int  net_tcp_listen(u16 port);                    // listen for TCP
TcpConn *net_tcp_accept(u16 port);               // blocking accept
int  net_tcp_send(TcpConn *c, const void *data, u16 len);
int  net_tcp_recv(TcpConn *c, void *buf, usize bufsz);
void net_tcp_close(TcpConn *c);
```

> **QEMU:** The fixed MAC `52:54:00:12:34:56` is hardcoded so `net_init()` can read it from E1000 registers before reset.

### 7.15 USB (EHCI + XHCI + HID + MSC)

**Files:** `kernel/usb.c`, `kernel/usb_hid.c`

`usb_full_init()` (called after `pci_scan_all()`):
1. Finds all EHCI (USB 2.0) and XHCI (USB 3.x) host controllers via PCI class 0x0C/0x03
2. Enumerates devices on each port (GET_DESCRIPTOR → SET_ADDRESS → SET_CONFIGURATION)
3. For HID class devices: sets up interrupt-IN polling for keyboards and mice
4. For Mass Storage class devices: registers a block device accessible via `usb_msc_read()` / `usb_msc_write()`

**MSC API:**
```c
int usb_msc_count(void);
u64 usb_msc_block_count(int idx);
u32 usb_msc_block_size(int idx);
int usb_msc_read(int dev_idx, u64 lba, u32 count, void *buf);
int usb_msc_write(int dev_idx, u64 lba, u32 count, void *buf);
```

### 7.16 PCI / PCIe Enumeration

**File:** `kernel/pci.c`

Scans all 256 buses × 32 slots × 8 functions using the legacy I/O port method (`0xCF8`/`0xCFC`) with optional ECAM (PCIe Enhanced Configuration Access Mechanism) for extended config space.

**Key functions:**
```c
void  pci_scan_all(void);                                   // must be called first
void *pci_find_device(u16 vendor, u16 device_id);           // by VID:DID
void *pci_find_class(u8 class_code, u8 subclass);
void *pci_find_class_progif(u8 class, u8 sub, u8 prog_if);  // exact match
u64   pci_bar_base(void *dev, int bar);                     // get BAR physical addr
u64   pci_bar_size(void *dev, int bar);
void  pci_enable_bus_master(void *dev);
void  pci_list_devices(void);                               // dump all found devices
```

**PCI class codes used internally:**

| Class | Sub | ProgIF | Device |
|---|---|---|---|
| 0x02 | 0x00 | — | E1000 NIC |
| 0x01 | 0x06 | 0x01 | AHCI SATA |
| 0x01 | 0x08 | 0x02 | NVMe |
| 0x0C | 0x03 | 0x20 | EHCI USB |
| 0x0C | 0x03 | 0x30 | XHCI USB |

### 7.17 AHCI SATA Driver

**File:** `kernel/ahci.c`

Follows AHCI 1.3.1 spec. Supports up to 8 ports.

**Init sequence:**
1. Enable AHCI mode (`HBA_GHC_AE`)
2. Global HBA reset (`HBA_GHC_HR`) — wait for reset to clear
3. For each implemented port (bit set in `HBA_PI`): check `SSTS.DET == 3` (device present), spin up, allocate DMA structures, set CLB/FB, wire command table, start port, run IDENTIFY
4. Prints model string and sector count for each drive

**DMA structures per port:**
- Command List: 32 × 32-byte `AhciCmdHdr` entries (1 KB aligned)
- FIS buffer: 256 bytes (256-byte aligned)
- Command Table: 64-byte FIS + 8 × 16-byte PRDT entries (128-byte aligned)
- Data buffer: 8 × 512 bytes = 4 KB (for multi-sector I/O)

**API:**
```c
void        ahci_init(u32 bar5);
i64         ahci_read_sector(int port, u32 lba, void *buf);       // 1 sector
i64         ahci_read_sectors(int port, u64 lba, u16 n, void *buf); // up to 8
i64         ahci_write_sector(int port, u32 lba, const void *buf);
i64         ahci_write_sectors(int port, u64 lba, u16 n, const void *buf);
i64         ahci_flush(int port);    // ATA FLUSH EXT — writeback cache
i64         ahci_identify(int port, void *buf);  // 512-byte IDENTIFY response
int         ahci_get_port_count(void);
u64         ahci_get_sector_count(int port);
const char *ahci_get_model(int port);
```

**Error recovery:** On any fatal error (TFES/HBFS/HBDS/IFS bits in Port IS, or TFD.ERR set), the driver issues a COMRESET (`SCTL.DET=1` for 10K cycles, then clear) and restarts the port.

### 7.18 NVMe Driver

**File:** `kernel/nvme.c`

Implements NVMe 1.4 over PCIe (PCI class 01:08:02, BAR0).

**Init sequence:**
1. Read `CAP` register — extract doorbell stride (`DSTRD`) and timeout
2. Disable controller (`CC.EN=0`), wait for `CSTS.RDY=0`
3. Allocate Admin SQ (depth 8) and Admin CQ (depth 8) — 4KB aligned
4. Set `AQA`, `ASQ`, `ACQ` registers
5. Enable controller with `CC = EN | CSS_NVM | MPS_4K | IOSQES=6 | IOCQES=4`
6. Wait for `CSTS.RDY=1`
7. Admin command: **Identify Controller** — prints model string
8. Admin command: **Identify Namespace 1** — reads sector count and LBA size
9. Admin command: **Create I/O CQ** (QID=1, depth=16)
10. Admin command: **Create I/O SQ** (QID=1, CQID=1, depth=16)

**Queue mechanism:** Submission queue entries (64 bytes) are written to the ring, then the tail doorbell register is written. Completion queue entries (16 bytes) use a **phase bit** to distinguish new entries from stale ones — no interrupts needed (polling).

**API:**
```c
void nvme_init(u64 bar0);
i64  nvme_read_sector(u64 lba, void *buf);    // one 512-byte sector
i64  nvme_write_sector(u64 lba, const void *buf);
i64  nvme_read_4k(u64 lba, void *buf);         // 8 sectors = 4 KB
i64  nvme_write_4k(u64 lba, const void *buf);
i64  nvme_flush(void);                         // NVMe FLUSH command
int  nvme_ready(void);
u64  nvme_sector_count(void);
u32  nvme_lba_size(void);
```

Non-512-byte native blocks (e.g., 4096-byte) are handled with a read-modify-write cycle for `nvme_write_sector()`.

### 7.19 Audio (OPL2 FM + SB16 PCM Mixer)

**File:** `kernel/sound.c`, `user/sound.h`

**OPL2 FM synthesis** (ports `0x388`/`0x389`):
- 9 channels, 2 operators each
- `sys_snd_opl_write(reg, val)` — direct register access
- `sys_snd_opl_note(ch, freq_num, block, vol, key_on)` — play/stop a note
- `sys_snd_opl_reset()` — silence all channels

**SB16 PCM mixer** (I/O port `0x220`):
- 8 software mixer channels
- 8-bit unsigned PCM samples at 22050 Hz
- Direct DAC output (DSP command `0x10`) — no DMA
- PIT fires at ~100 Hz; each tick pushes ~220 samples
- `sys_snd_mix_play(ch, samples, len, loop)` — queue audio on a channel
- `sys_snd_mix_stop(ch)` — stop channel
- `sys_snd_mix_volume(ch, vol)` — set channel volume (0–255)

Detected via the standard OPL2 timer test (sets Timer 1, reads status byte).

### 7.20 Framebuffer / Display (fbdev)

**File:** `kernel/fbdev.c`

QEMU `bochs-display` device provides a linear framebuffer at a BAR address. SoftTail probes it via PCI, maps the framebuffer, and exposes:

```c
void fb_enable(void);
int  fb_set_resolution(int w, int h);   // switch between 720p / 1080p
void fb_put_pixel(int x, int y, u32 color);
void fb_fill_rect(int x, int y, int w, int h, u32 color);
void fb_draw_rounded_rect(...);
void fb_draw_circle(...);
void fb_draw_line(int x0, int y0, int x1, int y1, u32 color);
void fb_fill_gradient_h(...);
void fb_fill_gradient_v(...);
void fb_draw_shadow(int x, int y, int w, int h);
void fb_blit(int dx, int dy, int w, int h, const u32 *src, int src_stride);
int  fb_get_width(void);
int  fb_get_height(void);
```

Default resolution: **1024 × 768**. The `720p` shell command switches to 1280×720; `1080p` switches to 1920×1080.

### 7.21 GUI Desktop

**File:** `kernel/gui.c`

A retro-style windowed desktop environment:
- Taskbar at bottom with a clock
- Desktop icons (file manager, terminal, network monitor)
- Draggable, resizable windows
- Widget types: button, label, checkbox, progress bar, text input, list row, separator, icon

**Window lifecycle:**
```c
int gui_window_create(int x, int y, int w, int h, const char *title);
void gui_window_close(int id);
void gui_window_set_active(int id);
```

**Built-in windows:**
- `gui_open_shell_window()` — embedded terminal window
- `gui_open_system_monitor()` — CPU/memory stats

Launch from the shell with `gui`. Type `720p` or `1080p` while GUI is active to change resolution.

### 7.22 PS/2 Keyboard & Mouse

**File:** `kernel/ps2.c`

Full i8042 controller init:
1. Disable both PS/2 ports
2. Flush output buffer
3. Set controller command byte (enable interrupts, disable translation)
4. Self-test controller and both ports
5. Enable both ports
6. Reset keyboard (`0xFF`) and mouse (`0xD4 0xFF`)
7. Set keyboard scan code set 2, enable mouse data reporting (`0xF4`)

Polled (no IRQ): the shell loop calls `ps2_poll()` to read scancodes. Mouse: 3-byte packets, delta X/Y decoded with sign extension from packet byte 0 flags.

### 7.23 Input Subsystem

**File:** `kernel/input.c`

A unified ring buffer for all input events — keyboard, mouse, and gamepad. User programs read events via syscalls:

```c
i64 sys_poll_keys(void *buf, usize max_events);    // SYS_POLL_KEYS = 300
i64 sys_poll_mouse(void *buf, usize max_events);   // SYS_POLL_MOUSE = 301
i64 sys_poll_pad(void *buf);                       // SYS_POLL_PAD = 302
```

Mouse grab mode (`sys_mouse_setmode()`) locks the pointer for first-person games.

### 7.24 ACPI

**File:** `kernel/acpi.c`

Parses RSDP → RSDT/XSDT → MADT to find:
- I/O APIC base address and GSI base
- Local APIC base address (for SMP)

Functions:
```c
void acpi_init(void);
void acpi_ioapic_init(void);
void acpi_reboot(void);      // writes 0x06 to ACPI reset register
void acpi_shutdown(void);    // writes ACPI S5 sleep type to PM1a_CNT
```

### 7.25 TSS & Privilege Switching

**File:** `kernel/tss.c`

The Task State Segment stores `RSP0` — the kernel stack pointer loaded on ring-3 → ring-0 transition. `tss_init()` installs the TSS descriptor in the GDT and executes `ltr` to load it. On every context switch, RSP0 is updated to the new process's kernel stack top.

### 7.26 Security (KASLR, SMAP/SMEP, Stack Canaries)

**File:** `kernel/security.c`

- **KASLR**: `kaslr_init()` randomizes base addresses for mmap, stack, and brk regions using a seed from the PIT counter
- **SMAP** (Supervisor Mode Access Prevention) and **SMEP** (Supervisor Mode Execution Prevention): enabled in CR4 if CPUID reports support — prevents kernel from accidentally reading/executing user memory
- **Stack canaries**: `stack_canary_generate()` produces a random 64-bit value placed before return addresses; checked on function return
- **User pointer validation**: `copy_from_user()` / `copy_to_user()` validate address ranges before any kernel memcpy from/to user buffers
- **Syscall argument validation**: `syscall_validate_args()` checks all pointer arguments for each syscall before dispatch

### 7.27 Resilience (SMP, OOM, Watchdog, Panic)

**File:** `kernel/resilience.c`

**SMP:** `smp_init()` sends INIT-SIPI-SIPI to all Application Processors (APs) found in the MADT. APs enter 64-bit mode and spin in a wait loop (`smp_cores_up` tracks how many came up). Only BSP (core 0) runs the scheduler.

**OOM killer:** `oom_kill()` is called when `pmm_alloc()` fails. It finds the process with the largest VMA footprint and sends it `SIGKILL`.

**Watchdog:** `watchdog_init()` arms a software counter. `watchdog_pet()` must be called from the shell loop. If the watchdog fires (called from the PIT ISR after N ticks without a pet), it prints a warning and calls `kernel_panic()`.

**Panic:** `kernel_panic(reason)` prints the reason, disables interrupts, and halts.

### 7.28 Swap

**File:** `kernel/swap.c`

When a process's working set exceeds available physical memory, `swap_out()` writes a page to the swap area on disk and marks the PTE as not-present with a swap slot encoded in the PTE bits. On the next access, a page fault calls `swap_in()` to reload the page. `swap_invalidate_pid()` frees all swap slots for an exiting process.

### 7.29 Package Manager

**File:** `kernel/pkgmgr.c`

An in-memory package registry (no actual download infrastructure yet). `pkg_add()` registers a package name, description, URL, and size. `pkg_list()` prints the registry. `pkg_install()` is a stub that prints what would happen. Syscall numbers: `SYS_PKG_INSTALL=350`, `SYS_PKG_REMOVE=351`, `SYS_PKG_LIST=352`.

---

## 8. Shell Reference

The SoftTail shell is embedded in `kernel_main()`. It supports a 16-entry command history (Up/Down arrows) and Shift+PageUp/PageDown scrollback (200 rows).

### File System Commands

| Command | Description |
|---|---|
| `ls` | List current directory contents |
| `cd <dir>` | Change directory (`..` goes up) |
| `pwd` | Print working directory |
| `stat <name>` | Show file or directory metadata |
| `cat <file>` | Print file contents |
| `head <file> [n]` | Print first N lines (default 10) |
| `tail <file> [n]` | Print last N lines (default 10) |
| `hexdump <file>` | Hex + ASCII dump of file |
| `wc <file>` | Word, line, and byte count |
| `find [pattern]` | Search filenames recursively |
| `df` | Disk free space on FAT32 partition |
| `touch <file>` | Create an empty file |
| `write <file> <text>` | Overwrite file with text |
| `append <file> <text>` | Append a line to a file |
| `rm <file>` | Delete a file |
| `cp <src> <dst>` | Copy a file |
| `rename <old> <new>` | Rename a file |
| `mkdir <dir>` | Create a directory |
| `rmdir <dir>` | Remove an empty directory |
| `echo <text>` | Print text |

### Process Commands

| Command | Description |
|---|---|
| `elf <file>` | Load and run an ELF64 binary from FAT32 |
| `run <file>` | Alias for `elf` |
| `ps` | List all running processes (PID, state, name) |

### System Commands

| Command | Description |
|---|---|
| `meminfo` | Physical memory stats (total, used, free pages) |
| `uname` | OS name, version, architecture |
| `uptime` | Seconds since boot (PIT tick count) |
| `clear` | Clear the VGA terminal |
| `reboot` | Reboot via 8042 command (port 0x64 ← 0xFE) |
| `halt` | Halt the CPU (CLI + HLT) |
| `shc` | Print SHC compiler info |

### Network Commands

| Command | Description |
|---|---|
| `ifconfig` | Show IP address, MAC, gateway, DNS |
| `ping <host/ip>` | Send ICMP echo request |
| `wget <url>` | Fetch a URL and print/save it |
| `serve [port]` | Start the built-in HTTP file server (default port 80) |
| `netcat [port]` | Listen on a TCP port and print received data |

### GUI Commands

| Command | Description |
|---|---|
| `gui` | Launch the framebuffer GUI desktop |
| `browser` | Launch the built-in web browser (requires BROWSER binary on disk) |
| `720p` | Switch display to 1280×720 |
| `1080p` | Switch display to 1920×1080 |

---

## 9. User-Space ABI

### 9.1 ELF64 Loader

**File:** `kernel/elf.c`

`elf_load(data, size, name)` processes an ELF64 binary:
1. Validates ELF magic and `e_machine == EM_X86_64`
2. For each `PT_LOAD` segment: allocates pages, maps them into the new process's address space, copies data from the ELF
3. Maps a 4 KB user stack below `PROC_STACK_TOP = 0x700000`
4. Creates a new PCB with `entry = e_entry`, name from the filename
5. Returns the new PID

**No 64 KB limit:** The loader uses `vfs_seek()` to get the real file size and allocates accordingly.

### 9.2 crt0 & Program Startup

**File:** `user/crt0.S`

The ELF entry point `_start`:
1. Aligns RSP to 16 bytes (System V AMD64 ABI requirement)
2. Calls `env_init_defaults()` — seeds PATH, HOME, TMPDIR, LANG, TERM, USER, etc.
3. Zero-fills `rdi` (argc), `rsi` (argv), `rdx` (envp) — no shell argument passing yet
4. Calls `main()`
5. Moves return value to `rdi`, executes `syscall` with `rax=60` (exit)

### 9.3 libc.h / libc.c

A near-complete C standard library for SoftTail user programs. Highlights:

**Syscall wrappers:** `read`, `write`, `open`, `close`, `exit`, `fork`, `execve`, `waitpid`, `pipe`, `dup`, `dup2`, `kill`, `signal`, `mmap`, `munmap`, `brk`, `socket`, `bind`, `connect`, `listen`, `accept`, `send`, `recv`, `clone`

**stdio:** `printf`, `fprintf`, `sprintf`, `snprintf`, `vprintf`, `vfprintf`, `vsprintf`, `vsnprintf`, `putchar`, `puts`, `fopen`, `fclose`, `fread`, `fwrite`, `fseek`, `ftell`, `fflush`, `feof`, `ferror`, `getchar`, `fgetc`, `fputc`, `fputs`, `gets`, `fgets`, `sscanf`, `fscanf`, `scanf`

**string:** `strlen`, `strcmp`, `strncmp`, `strcpy`, `strncpy`, `strcat`, `strncat`, `strchr`, `strrchr`, `strstr`, `strdup`, `strtok`, `strtol`, `strtoul`, `strtod`, `atoi`, `atof`

**memory:** `memcpy`, `memmove`, `memset`, `memcmp`, `memchr`, `malloc`, `calloc`, `realloc`, `free`

**stdlib:** `abs`, `rand`, `srand`, `qsort`, `bsearch`, `exit`, `abort`, `getenv`, `setenv`, `unsetenv`

**time:** `time`, `clock`, `gettimeofday`, `clock_gettime`

**locale:** `setlocale` (always returns "C"), `localeconv`

**env:** `environ` array, `env_init_defaults()` seeds 10 standard variables

### 9.4 libm.h — Math Library

**File:** `user/libm.h` (header-only)

Full math library in a single header. All functions available in both `double` and `float` (`f`-suffixed) forms:

`sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2`, `exp`, `exp2`, `log`, `log2`, `log10`, `pow`, `sqrt`, `cbrt`, `hypot`, `floor`, `ceil`, `round`, `trunc`, `fabs`, `fmod`, `modf`, `frexp`, `ldexp`, `sinh`, `cosh`, `tanh`, `asinh`, `acosh`, `atanh`, `isinf`, `isnan`, `isfinite`

### 9.5 pthread.h — Threading

**File:** `user/pthread.h` (header-only)

```c
int pthread_create(pthread_t *t, const pthread_attr_t *attr, void *(*fn)(void*), void *arg);
int pthread_join(pthread_t t, void **retval);
void pthread_exit(void *retval);

int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *attr);
int pthread_mutex_lock(pthread_mutex_t *m);
int pthread_mutex_trylock(pthread_mutex_t *m);
int pthread_mutex_unlock(pthread_mutex_t *m);
int pthread_mutex_destroy(pthread_mutex_t *m);

int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m);
int pthread_cond_signal(pthread_cond_t *c);
int pthread_cond_broadcast(pthread_cond_t *c);
int pthread_cond_destroy(pthread_cond_t *c);
```

Mutexes are implemented using `futex()` — no busy-waiting.

### 9.6 tls.h — TLS 1.2 Client

**File:** `user/tls.h` (header-only)

A full TLS 1.2 client requiring zero external dependencies:

- **AES-128-GCM** (full key schedule, GHASH, CTR mode, tag verify)
- **SHA-256** and **HMAC-SHA-256**
- **TLS 1.2 PRF** (RFC 5246 §5)
- **RSA PKCS#1 v1.5** up to 2048-bit (for key exchange)
- **ASN.1/DER** certificate parser (extracts server public key)
- **SNI** extension
- Cipher suite: `TLS_RSA_WITH_AES_128_GCM_SHA256 (0x009C)`
- PRNG: xorshift64 seeded from `gettime_ms()`

```c
TlsConn *tls_connect(int sockfd, const char *hostname);
int      tls_write(TlsConn *c, const void *buf, size_t len);
int      tls_read(TlsConn *c, void *buf, size_t len);
void     tls_close(TlsConn *c);
```

### 9.7 ipc.h — IPC Messaging

**File:** `user/ipc.h`

```c
// Register this process as a named server
long ipc_register(const char *name);

// Find a server's PID
long ipc_lookup(const char *name);

// Send a message (blocks until delivered)
long ipc_send(long dest_pid, IpcMsg *msg);

// Receive a message (blocks until one arrives)
long ipc_recv(IpcMsg *msg);
```

### 9.8 gfx.h — Graphics API

**File:** `user/gfx.h`

Double-buffered rendering API:

```c
void clear_screen(uint32_t color);                         // fill back buffer
void blit(int dx, int dy, int w, int h,
          const uint32_t *pixels, uint32_t colorkey);      // copy sprite to back buf
void flip(void);                                           // swap buffers → screen

void set_tilemap(uint64_t layer, const GfxTilemap *tm);
void draw_tile(int x, int y, uint32_t tile_id, uint64_t layer);
void render_layer(uint64_t layer, int scroll_x, int scroll_y);
void set_colorkey(uint32_t color);
```

`GfxTilemap` holds a pointer to the tile pixel data, the tile map array, tile dimensions, and map dimensions. Colorkey `0xFFFFFFFF` disables transparency.

### 9.9 sound.h — Audio API

**File:** `user/sound.h`

```c
// OPL2 FM synthesis
void opl_write(uint8_t reg, uint8_t val);
void opl_note(uint64_t ch, uint32_t fnum, uint32_t block, uint32_t vol, uint32_t key_on);
void opl_reset(void);

// PCM software mixer
void mix_play(uint64_t ch, const uint8_t *samples, uint32_t len, uint32_t loop);
void mix_stop(uint64_t ch);
void mix_volume(uint64_t ch, uint32_t vol);
void mix_tick(void);  // call every frame to advance mixer
```

---

## 10. Writing a User Program

### Minimal example

```c
// hello.c
#include "libc.h"

int main(void) {
    write(1, "Hello, SoftTail!\n", 15);
    return 0;
}
```

### Build and inject

```bash
# Compile
gcc -m64 -ffreestanding -fno-stack-protector -mno-red-zone \
    -nostdlib -nostdinc -O2 -Iuser -Wall -c -o hello.o hello.c

# Link (load address 0x400000)
ld -m elf_x86_64 -static -nostdlib -Ttext=0x400000 \
   -o HELLO user/crt0.o user/libc.o hello.o

# Inject into disk image
make addprog PROG=HELLO

# Boot and run
make run
# At SoftTail shell:
# softtail:/$ elf HELLO
```

### Using threads

```c
#include "libc.h"
#include "pthread.h"

pthread_mutex_t mu;

void *worker(void *arg) {
    pthread_mutex_lock(&mu);
    printf("thread %d\n", (int)(long)arg);
    pthread_mutex_unlock(&mu);
    return NULL;
}

int main(void) {
    pthread_mutex_init(&mu, NULL);
    pthread_t t1, t2;
    pthread_create(&t1, NULL, worker, (void*)1);
    pthread_create(&t2, NULL, worker, (void*)2);
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    return 0;
}
```

### Using graphics

```c
#include "libc.h"
#include "gfx.h"

static uint32_t sprite[16*16]; // 16×16 RGBA pixels

int main(void) {
    // fill sprite with red
    for (int i = 0; i < 256; i++) sprite[i] = 0xFF0000;

    int x = 100, y = 100;
    while (1) {
        clear_screen(0x000020);              // dark blue background
        blit(x, y, 16, 16, sprite, 0);      // draw sprite
        flip();                              // show frame
        x++;
        if (x > 800) x = 0;
    }
}
```

Link with `user/gfx.h` — no extra `.o` needed (header-only inline wrappers).

---

## 11. SHC — The Shadow Scripting Compiler

**File:** `user/shc.c` (~43 KB)

SHC is a self-hosted scripting language compiler that runs inside SoftTail OS. It compiles `.shadow` source files into native ELF64 binaries.

### Building and installing

```bash
make compiler    # builds SHC + injects it + sample .shadow files
make run
# At the shell:
elf SHC          # launch the SHC compiler REPL
```

### Sample: hello.shadow

```
print "Hello from Shadow!"
```

### Sample: fib.shadow

```
fn fib(n)
  if n < 2 then return n end
  return fib(n-1) + fib(n-2)
end
print fib(10)
```

### Usage inside SoftTail

```
softtail:/$ elf SHC
SHC> (enter source file name, e.g. FIB.SHA)
FIB.SHA
(compiles to FIB)
softtail:/$ elf FIB
55
```

---

## 12. Browser

**File:** `browser/browser.c` + `browser/{html,css,layout,render}.h`

A graphical web browser that runs as a normal user-space ELF binary.

**Features:**
- HTTP and HTTPS (via `tls.h` — TLS 1.2 with AES-128-GCM)
- HTML tokenizer and DOM builder
- CSS property parser (color, font-size, display, margin, padding, background)
- Block/inline layout engine
- Framebuffer renderer (text glyphs from `font8x8.h`, colored boxes)
- URL bar navigation

**Build and run:**
```bash
make browser addbrowser
make run
# At shell:
elf BROWSER
```

---

## 13. Syscall Table

All syscalls use the x86-64 `SYSCALL` instruction. `rax` = syscall number, return value in `rax`, args in `rdi rsi rdx r10 r8 r9`.

| # | Name | Args |
|---|---|---|
| 0 | `read` | fd, buf, count |
| 1 | `write` | fd, buf, count |
| 2 | `open` | path, flags |
| 3 | `close` | fd |
| 5 | `fstat` | fd, statbuf |
| 7 | `poll` | fds, nfds, timeout_ms |
| 9 | `mmap` | addr, len, prot, flags, fd, off |
| 10 | `mprotect` | addr, len, prot |
| 11 | `munmap` | addr, len |
| 12 | `brk` | new_brk |
| 13 | `signal` / `sigaction` | signum, handler/act, oldact |
| 14 | `sigprocmask` | how, set, oldset |
| 20 | `writev` | fd, iov, cnt |
| 22 | `pipe` | pipefd[2] |
| 23 | `select` | nfds, rfds, wfds, efds, tv |
| 32 | `dup` | oldfd |
| 33 | `dup2` | oldfd, newfd |
| 39 | `getpid` | — |
| 41 | `socket` | domain, type, proto |
| 42 | `connect` | sockfd, addr, addrlen |
| 43 | `accept` | sockfd, addr, addrlen |
| 44 | `send` | sockfd, buf, len, flags |
| 45 | `recv` | sockfd, buf, len, flags |
| 49 | `bind` | sockfd, addr, addrlen |
| 50 | `listen` | sockfd, backlog |
| 56 | `clone` | flags, stack, ptid, ctid, newtls |
| 57 | `fork` | — |
| 59 | `execve` | path, argv, envp |
| 60 | `exit` | code |
| 61 | `wait4` | pid, wstatus, options, ru |
| 62 | `kill` / `lseek` | pid, sig / fd, off, whence |
| 72 | `fcntl` | fd, cmd, arg |
| 82 | `rename` | old, new |
| 83 | `mkdir` | path, mode |
| 84 | `rmdir` | path |
| 87 | `unlink` | path |
| 96 | `gettimeofday` | tv, tz |
| 158 | `arch_prctl` | code, addr |
| 202 | `futex` | addr, op, val, timeout, addr2, val3 |
| 213 | `epoll_create` | flags |
| 217 | `getdents64` | fd, buf, count |
| 228 | `clock_gettime` | clk_id, timespec |
| 229/263 | `clock_getres` | clk_id, timespec |
| 231 | `exit_group` | code |
| 257 | `openat` | dirfd, path, flags, mode |
| 262 | `fstatat` | dirfd, path, statbuf, flags |
| 300 | `poll_keys` | buf, max_events |
| 301 | `poll_mouse` | buf, max_events |
| 302 | `poll_pad` | buf |
| 310 | `gfx_flip` | — |
| 311 | `gfx_clear` | color |
| 312 | `gfx_blit` | dx, dy, w, h, pixels, colorkey |
| 313 | `gfx_set_colorkey` | color |
| 314 | `gfx_draw_tile` | x, y, tile_id, layer |
| 315 | `gfx_set_tilemap` | layer, GfxTilemap* |
| 316 | `gfx_render_layer` | layer, scroll_x, scroll_y |
| 320 | `snd_opl_write` | reg, val |
| 321 | `snd_opl_note` | ch, fnum, block, vol, key_on |
| 322 | `snd_opl_reset` | — |
| 323 | `snd_mix_play` | ch, samples, len, loop |
| 324 | `snd_mix_stop` | ch |
| 325 | `snd_mix_volume` | ch, vol |
| 326 | `snd_mix_tick` | — |
| 327 | `gettime_ms` | — |
| 328 | `mouse_setmode` | mode |
| 329 | `ipc_send` | dest_pid, IpcMsg* |
| 330 | `ipc_recv` | IpcMsg* |
| 331 | `ipc_register` | name |
| 332 | `ipc_lookup` | name |
| 350 | `pkg_install` | name |
| 351 | `pkg_remove` | name |
| 352 | `pkg_list` | — |

---

## 14. FAQ

**Q: How do I add my own program to SoftTail OS?**

A: Compile it as a static ELF64 binary linked at `0x400000`, link with `crt0.o` and `libc.o`, then run `make addprog PROG=./MYBINARY`. At the SoftTail shell, run it with `elf MYBINARY`.

---

**Q: Why does the kernel use `-fno-pic`?**

A: Without it, GCC emits `R_X86_64_REX_GOTPCRELX` relocations for extern function pointers. The kernel has no Global Offset Table. When the IDT handler addresses are resolved, the linker writes the GOT slot address instead of the function address. Every interrupt then jumps to garbage, causing an immediate triple fault.

---

**Q: Can I run Linux ELF binaries in SoftTail OS?**

A: Not directly. SoftTail has a Linux-compatible syscall numbering, but it does not implement dynamic linking (`ld-linux.so`), `procfs`, or the full kernel ABI. Static ELF binaries that use only the implemented syscalls (most things in the table above) will generally work if compiled with the SoftTail libc or a compatible minimal libc.

---

**Q: How much RAM can SoftTail OS use?**

A: Any amount QEMU is given (via `-m`). The bootloader runs `INT 15h/E820` to query the full BIOS memory map, and the PMM feeds every usable page into the buddy allocator. There is a compile-time ceiling of `RAM_END_MAX = 64 GB` for static bitmap sizing, but runtime can go much higher with a recompile.

> **Known bug:** `meminfo` may report ~4 GB total even with a smaller QEMU `-m` value, due to a `end_pfn` calculation issue in `pmm_init()`.

---

**Q: What is the maximum number of processes?**

A: `PROC_MAX = 64`. Each PCB is 128 bytes; the table occupies 8 KB at physical `0x300000`. Each process also gets a 4 KB kernel stack. To increase the limit, change `PROC_MAX` in `kernel.h` and recompile.

---

**Q: How does the scheduler work? Is it preemptive?**

A: Yes, fully preemptive. The PIT (Programmable Interval Timer) fires IRQ 0 at approximately 100 Hz. The timer ISR saves all registers to the current PCB's kernel stack and calls `schedule()`, which selects the next `PSTATE_READY` process by round-robin and loads its saved state. The timer ISR then `iretq`s into the new process.

---

**Q: How do I do networking from a user program?**

A: Use the POSIX socket API from `libc.h`:
```c
int fd = socket(AF_INET, SOCK_STREAM, 0);
struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(80), ... };
connect(fd, &addr, sizeof(addr));
send(fd, request, len, 0);
recv(fd, buf, sizeof(buf), 0);
close(fd);
```
For HTTPS, wrap the socket with `tls_connect()` from `tls.h`.

---

**Q: How does the IPC system work?**

A: A process calls `ipc_register("myserver")` to publish its name. Clients call `ipc_lookup("myserver")` to get the server's PID, then `ipc_send(pid, &msg)`. The kernel delivers the message and the server receives it via `ipc_recv(&msg)`. Messages are 64 bytes fixed. The kernel blocks `ipc_recv()` callers until a message arrives (process state → `PSTATE_BLOCKED`).

---

**Q: How do I write to a file from a user program?**

A: Use standard POSIX file I/O:
```c
int fd = open("MYFILE.TXT", O_WRONLY | O_CREAT);
write(fd, "hello\n", 6);
close(fd);
```
Note: FAT32 requires 8.3 filenames (8 chars + 3 char extension, uppercase). The VFS layer formats names automatically but if the name doesn't fit 8.3, it will be truncated.

---

**Q: Can I use floating-point in user programs?**

A: Yes. The kernel disables SSE/MMX in its own compile flags (`-mno-sse`), but user programs can use FP normally since the hardware FP unit is always available in ring 3. The ISR does not save/restore XMM registers (which would break FP across context switches in the kernel), but user programs each have their own register context which is saved/restored on each context switch at the kernel boundary. For complex math, include `libm.h`.

---

**Q: Why does `meminfo` show ~4 GB total even with 128M QEMU?**

A: This is a known bug in `pmm_init()`. The E820 parsing code miscalculates `end_pfn` from the last memory entry, making the PMM think there are far more pages than actually available. The buddy allocator still works correctly for the pages that are actually present — it just reports an inflated total. It is not a blocking issue.

---

**Q: How do I use the AHCI driver from kernel code?**

A: After `pci_scan_all()`, call `ahci_init(bar5)` where `bar5` is the AHCI BAR (obtained via `pci_bar_base(dev, 5)`). Reads and writes:
```c
u8 sector_buf[512];
ahci_read_sector(0, 512, sector_buf);   // port 0, LBA 512
ahci_write_sector(0, 512, sector_buf);
ahci_flush(0);                          // write-back cache
```
Or multi-sector:
```c
u8 buf[4096];
ahci_read_sectors(0, 1024, 8, buf);   // 8 sectors starting at LBA 1024
```

---

**Q: How do I use the NVMe driver?**

A: After `pci_scan_all()`, call `nvme_init(bar0)` where `bar0` is obtained via `pci_bar_base(dev, 0)`. The driver auto-identifies the primary namespace. Then:
```c
u8 buf[512];
nvme_read_sector(0, buf);          // LBA 0
nvme_write_sector(0, buf);
nvme_flush();

// Fast 4KB reads:
u8 buf4k[4096] __attribute__((aligned(4096)));
nvme_read_4k(0, buf4k);           // reads LBAs 0–7
```

---

**Q: What display resolution does SoftTail OS support?**

A: Default is **1024×768** (set in QEMU with `-device bochs-display,xres=1024,yres=768`). From the shell, `720p` switches to 1280×720 and `1080p` switches to 1920×1080. The framebuffer is a 32-bit linear RGBA/RGBX surface.

---

**Q: How do I build the SHC scripting compiler?**

A: `make compiler` builds SHC and injects it along with sample `.shadow` files into the FAT32 partition. Boot SoftTail and run `elf SHC` at the shell.

---

**Q: Does SoftTail OS support SMP (multiple CPU cores)?**

A: Partially. `smp_init()` sends INIT+SIPI signals to all Application Processors found in the ACPI MADT, and they enter 64-bit mode. However, only core 0 (the Bootstrap Processor) runs the scheduler and handles interrupts. The APs spin-wait. Full SMP scheduling is on the roadmap.

---

**Q: How does CoW (Copy-on-Write) fork work?**

A: When `fork()` is called, `vmm_cow_fork()` walks the parent's page tables. Every writable page is remapped as read-only in both the parent and child, and the PMM refcount for each shared page is incremented. When either process writes to a shared page, a page fault fires. `vmm_page_fault()` sees the page is writable in the VMA but read-only in the PTE, allocates a fresh page, copies the contents, decrements the old page's refcount, and maps the new page as writable. The other process continues sharing the original read-only page.

---

**Q: Can SoftTail OS boot on real hardware?**

A: Theoretically yes, for machines with a legacy BIOS and an IDE/SATA controller that BIOS presents as an INT 13h LBA drive. In practice:
- The E1000 NIC driver targets QEMU's specific device ID (`0x8086:0x100E`) — real E1000 cards use different IDs
- The `bochs-display` framebuffer driver is QEMU-specific — real hardware needs VBE/VESA or a real GPU driver
- The SB16 audio driver targets port `0x220` — real hardware may not have an SB16

---

## 15. Known Bugs & Limitations

| Issue | Detail |
|---|---|
| `meminfo` reports ~4 GB total | PMM `end_pfn` calculation bug in E820 parsing; non-blocking |
| 8.3 filenames only | FAT32 LFN entries are ignored; all names must be short |
| No shell argument passing | `argv`/`argc` are always null/zero in user programs; `crt0.S` does not parse a command line yet |
| SMP scheduling incomplete | AP cores are brought up but only BSP runs the scheduler |
| USB mass storage: no FAT32 | USB flash drives are accessible via `usb_msc_read/write` but not mounted into VFS |
| AHCI: single command slot | Only slot 0 is used; no native command queuing (NCQ) |
| NVMe: single namespace | Only namespace ID 1 is enumerated |
| No dynamic linking | All user programs must be statically linked |
| No TTY / terminal emulation | The shell is a bare read-line loop; no job control, no signals from Ctrl+C in GUI |
| HD Audio not implemented | Only SB16 PCM + OPL2 FM are supported |
| Browser TLS: xorshift64 PRNG | Not cryptographically strong; fine for a hobby OS, not for real use |
| Stack canary not injected at compile time | `stack_canary_generate()` exists but the kernel does not use `-fstack-protector` in `CFLAGS` |

---

## 16. Roadmap

Based on the `work.md` dev log, all items from the browser porting checklist are complete. Planned next steps:

- **HD Audio driver** — Intel HDA (PCI class 04:03) to replace the SB16 PCM path
- **Full SMP scheduling** — per-CPU run queues, IPI-based preemption, spinlock audit
- **Shell argument passing** — parse command-line arguments in `crt0.S`, propagate `argc`/`argv` to `main()`
- **LFN (Long File Name) FAT32** — support filenames longer than 8.3
- **USB mass storage VFS mount** — auto-mount USB drives at `/usb0`, `/usb1`, …
- **NVMe multi-namespace** — enumerate all namespaces, expose as separate block devices
- **AHCI NCQ** — native command queuing for SATA SSDs (up to 32 in-flight commands)
- **VGA text-mode → full framebuffer console** — replace VGA `0xB8000` terminal with a GPU-rendered font
- **Dynamic linker** — `ld.so` stub to support shared libraries
- **procfs / devfs** — virtual filesystems at `/proc` and `/dev`
- **Network stack improvements** — UDP multicast, TCP retransmit, window scaling, IPv6

---

*SoftTail OS — built from scratch, one page fault at a time.*
