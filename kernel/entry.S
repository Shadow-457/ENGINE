# ================================================================
#  Systrix OS — kernel/entry.S
#  Kernel entry: real mode → protected mode → long mode → kernel_main()
# ================================================================

.code16
.intel_syntax noprefix

.section .text.start

.globl kernel_start
kernel_start:
    cli
    xor  ax, ax
    mov  ds, ax; mov es, ax; mov ss, ax
    mov  sp, 0x7000

    in   al, 0x92
    or   al, 0x02
    and  al, 0xFE
    out  0x92, al

    lgdt [gdt_ptr16]

    mov  eax, cr0
    or   eax, 1
    mov  cr0, eax

    .byte 0x66, 0xEA
    .long pm32_entry
    .word 0x08

.align 8
gdt_table:
    .quad 0x0000000000000000   /* 0x00: null */
    .quad 0x00CF9A000000FFFF   /* 0x08: code32 protected-mode (PM32 transition) */
    .quad 0x00AF9A000000FFFF   /* 0x10: code64 kernel, DPL=0 */
    .quad 0x00CF92000000FFFF   /* 0x18: data64 kernel, DPL=0 */
    .quad 0x00CFF2000000FFFF   /* 0x20: data64 user,   DPL=3  (SS for iretq/SYSRET → 0x23) */
    .quad 0x00AFFA000000FFFF   /* 0x28: code64 user,   DPL=3  (CS for iretq/SYSRET → 0x2b) */
    /* 0x30: 64-bit TSS system descriptor (16 bytes).
     * Base/limit filled in at runtime by tss_init(); zeroed here. */
    .quad 0x0000000000000000   /* TSS descriptor low  quad */
    .quad 0x0000000000000000   /* TSS descriptor high quad */
gdt_table_end:

gdt_ptr16:
    .word gdt_table_end - gdt_table - 1
    .long gdt_table

.code32
pm32_entry:
    mov  ax, 0x18
    mov  ds, ax; mov es, ax; mov ss, ax
    mov  esp, 0x7000
    cld

    mov  edi, 0x1000
    xor  eax, eax
    mov  ecx, 3072
    rep  stosd

    mov  dword ptr [0x1000], 0x00002003
    mov  dword ptr [0x1004], 0
    mov  dword ptr [0x2000], 0x00003003
    mov  dword ptr [0x2004], 0

    mov  edi, 0x3000
    mov  eax, 0x00000083
    mov  ecx, 512
.fill_pd:
    mov  dword ptr [edi],   eax
    mov  dword ptr [edi+4], 0
    add  eax, 0x200000
    add  edi, 8
    loop .fill_pd

    mov  eax, cr4
    or   eax, 0x20
    mov  cr4, eax

    mov  eax, 0x1000
    mov  cr3, eax

    mov  ecx, 0xC0000080
    rdmsr
    or   eax, 0x900      /* LME (bit 8) + NXE (bit 11) */
    wrmsr

    mov  eax, cr0
    or   eax, 0x80000000
    mov  cr0, eax

    .byte 0xEA
    .long lm64_entry
    .word 0x10

.code64
lm64_entry:
    mov  ax, 0x18
    mov  ds, ax; mov es, ax; mov ss, ax
    mov  fs, ax; mov gs, ax
    mov  rsp, 0x7E00
    xor  rbp, rbp

    # ── CRITICAL: disable interrupts immediately ──────────────────
    # The BIOS leaves IF=1. The PIT is still ticking at ~18 Hz.
    # We must CLI here before the IDT is set up, or a spurious timer
    # will triple-fault us. We only STI again in scheduler_start().
    cli

    # ── Enable SSE ────────────────────────────────────────────────
    mov  rax, cr0
    and  rax, ~(1 << 2)
    or   rax,  (1 << 1)
    mov  cr0, rax
    mov  rax, cr4
    or   rax, (3 << 9)
    mov  cr4, rax

    # ── Reload GDT with 64-bit pointer ───────────────────────────
    lea  rax, [rip + gdt_ptr64]
    lgdt [rax]

    call kernel_main

    cli
.spin:
    hlt
    jmp .spin

.align 4
gdt_ptr64:
    .word gdt_table_end - gdt_table - 1
    .quad gdt_table
