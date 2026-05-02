# Boot Process

SystrixOS boots from a custom hand-written 512-byte MBR bootloader, then transitions to 64-bit long mode and starts the C kernel.

---

## Stage 1: MBR Bootloader (`boot/boot.S`)

The BIOS loads sector 0 of the disk to `0x7C00` and jumps to it. The bootloader:

1. **Sets up a minimal real-mode stack** at `0x7C00`
2. **Reads kernel sectors** from disk using BIOS INT 13h (extended read — LBA mode)
   - Kernel starts at sector 1, length determined at build time
   - Loaded to physical address `0x10000`
3. **Enables A20** via the keyboard controller (port 0x64/0x60)
4. **Loads a temporary GDT** (32-bit protected mode descriptor)
5. **Enters 32-bit protected mode** (sets CR0.PE)
6. **Sets up 64-bit paging:**
   - Identity maps the first 2 GB using 2 MB huge pages (PML4 → PDPT → PD)
   - PML4 at `0x1000`, PDPT at `0x2000`, PD at `0x3000`
7. **Loads the 64-bit GDT** and enters long mode (sets EFER.LME, EFER.NXE, then CR0.PG)
8. **Far jumps** to `kernel/entry.S` in 64-bit mode

**Boot sector validation:** The Makefile asserts `boot.bin` is exactly 512 bytes:

```python
d = open('boot.bin','rb').read()
assert len(d) == 512, f'boot.bin is {len(d)} bytes, not 512'
```

Newer binutils inject `.note.gnu.property` into object files which would make the binary 1064 bytes. The Makefile works around this by linking to a temp ELF first, then using `objcopy --remove-section`.

---

## Stage 2: Kernel Entry (`kernel/entry.S`)

Executed in 64-bit long mode at `0x10000 + offset`:

1. **Loads the final GDT** (64-bit code + data segments)
2. **Sets up the kernel stack** (16-byte aligned, 64 KB)
3. **Zeros BSS** (loop over `__bss_start` .. `__bss_end`, defined by `linker.ld`)
4. **Calls `kernel_main()`** in `kernel/kernel.c`

---

## Stage 3: `kernel_main()` (`kernel/kernel.c`)

Initialisation order:

```
1.  VGA terminal         (text output for early boot messages)
2.  PMM                  (physical memory map from BIOS e820)
3.  VMM + heap           (virtual memory, kernel heap)
4.  GDT (final)          (proper segment descriptors)
5.  IDT                  (load all 256 ISR stubs from isr.S)
6.  PIC                  (remap IRQs to 0x20–0x2F, unmask timer + kbd)
7.  PIT                  (timer at ~100 Hz for scheduler)
8.  TSS                  (RSP0 for ring-3 → ring-0 stack switch)
9.  PS/2 keyboard        (IRQ1 handler, scan-code set 2)
10. PCI                  (enumerate all devices)
11. ACPI                 (power management)
12. AHCI / NVMe          (storage drivers)
13. USB                  (xHCI init, device enumeration)
14. e1000 / net          (NIC init, TX/RX ring setup)
15. Framebuffer / GUI    (bochs-display detection, compositor init)
16. Scheduler            (create idle task, enable preemption)
17. Shell                (hand control to interactive shell)
```

---

## Linker Script (`linker.ld`)

Places the kernel at link address `0x10000`:

```ld
ENTRY(kernel_entry)

SECTIONS {
    . = 0x10000;
    .text   : { *(.text*)   }
    .rodata : { *(.rodata*) }
    .data   : { *(.data*)   }
    .bss    : {
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        __bss_end = .;
    }
}
```

The `--oformat binary` flag strips the ELF wrapper so `kernel.bin` is a flat binary that the bootloader can load directly.
