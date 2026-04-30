# ================================================================
#  SHADOW OS — kernel/isr.S  (PATCHED)
#  Fixes: Bug 1 re-fix (post-GP-push RSP in PCB_KSTACK), Bug 3 (hardcoded syscall stack)
#         Bug A (process state race: EMPTY not READY until process_run)
# ================================================================

.equ PROC_TABLE,    0x300000
.equ PROC_PCB_SZ,   128
.equ PROC_MAX,      64
.equ PCB_STATE,     0
.equ PCB_KSTACK,    8
.equ PCB_URSP,      16
.equ PCB_ENTRY,     24
.equ PCB_PID,       32
.equ PCB_CR3,       40
.equ PCB_BRK,       48
.equ PCB_NAME,      56
.equ PCB_KBASE,     72

.equ PSTATE_EMPTY,  0
.equ PSTATE_READY,  1
.equ PSTATE_RUNNING,2
.equ PSTATE_DEAD,   3

.equ PIC1_CMD,      0x20
.equ PIC_EOI,       0x20
.equ KERNEL_CS,     0x10

.code64
.intel_syntax noprefix

.globl null_isr, timer_isr, scheduler_exit, isr_errcode, isr_page_fault
.globl syscall_entry
.globl kernel_return_rsp, current_pid, pit_ticks

# ================================================================
#  null_isr — for IRQs and CPU exceptions WITHOUT an error code
# ================================================================
null_isr:
    push rax
    mov  al, PIC_EOI
    out  PIC1_CMD, al
    pop  rax
    iretq

# ================================================================
#  isr_errcode — CPU exceptions that push an error code.
#  Prints  "FAULT ec=XXXX rip=XXXXXXXXXXXXXXXX cr2=XXXXXXXXXXXXXXXX"
#  to VGA row 0 (white on red, attr=0x4F) then halts permanently.
#
#  Stack on entry (RSP points at ERROR_CODE):
#    [RSP+ 0]  error code
#    [RSP+ 8]  RIP
#    [RSP+16]  CS
#    [RSP+24]  RFLAGS
#    [RSP+32]  RSP_user  (only present if CPL changed)
#    [RSP+40]  SS_user   (only present if CPL changed)
# ================================================================
# ================================================================
#  isr_errcode — now just an alias for panic_stub with vec=0xFF
#  (Should not be called directly anymore; use per-vector stubs.)
#  Kept for backward-compat in case something jumps here.
#  We push ec=0 then vec=0xFF and fall through.
# ================================================================
.globl isr_errcode
isr_errcode:
    push 0          # dummy error code (caller didn't push one)
    push 0xFF       # sentinel vector
    jmp  .panic_common

# ================================================================
#  Per-vector panic stubs.
#  Vectors with CPU-pushed error code: 8,10,11,12,13,14,17,21,29,30
#  All others: we push 0 as error code first.
# ================================================================
.globl panic_stub_0,  panic_stub_6,  panic_stub_7
.globl panic_stub_8,  panic_stub_10, panic_stub_11, panic_stub_12
.globl panic_stub_13, panic_stub_14, panic_stub_17, panic_stub_21
.globl panic_stub_29, panic_stub_30

panic_stub_0:   push 0;   push 0;  jmp  .panic_common   # #DE (no ec)
panic_stub_6:   push 0;   push 6;  jmp  .panic_common   # #UD (no ec)
panic_stub_7:   push 0;   push 7;  jmp  .panic_common   # #NM (no ec)
panic_stub_8:             push 8;  jmp  .panic_common   # #DF (has ec)
panic_stub_10:            push 10; jmp  .panic_common   # #TS
panic_stub_11:            push 11; jmp  .panic_common   # #NP
panic_stub_12:            push 12; jmp  .panic_common   # #SS
panic_stub_13:            push 13; jmp  .panic_common   # #GP
panic_stub_14:            push 14; jmp  .panic_common   # #PF
panic_stub_17:            push 17; jmp  .panic_common   # #AC
panic_stub_21:            push 21; jmp  .panic_common   # #CP
panic_stub_29:            push 29; jmp  .panic_common   # #VC
panic_stub_30:            push 30; jmp  .panic_common   # #SX

# ================================================================
#  .panic_common — full panic dump
#
#  Stack on entry:
#    [RSP+ 0]  vector number  (we just pushed)
#    [RSP+ 8]  error code     (CPU or we pushed)
#    [RSP+16]  RIP
#    [RSP+24]  CS
#    [RSP+32]  RFLAGS
#    [RSP+40]  RSP_user
#    [RSP+48]  SS_user
# ================================================================
.panic_common:
    cli

    # Save all 15 GP regs (120 bytes)
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    # GP regs on stack (rsp-relative, after 15 pushes):
    #  +0=r15 +8=r14 +16=r13 +24=r12 +32=r11 +40=r10 +48=r9  +56=r8
    #  +64=rbp +72=rdi +80=rsi +88=rdx +96=rcx +104=rbx +112=rax
    # Exception frame:
    #  +120=vec +128=ec +136=RIP +144=CS +152=RFLAGS +160=RSP +168=SS

    # Stash everything we need in .bss scratch area before we touch r8-r15
    mov  rax, [rsp + 120]; mov [rip + .pv_vec],    rax   # vector
    mov  rax, [rsp + 128]; mov [rip + .pv_ec],     rax   # error code
    mov  rax, [rsp + 136]; mov [rip + .pv_rip],    rax   # RIP
    mov  rax, [rsp + 144]; mov [rip + .pv_cs],     rax   # CS
    mov  rax, [rsp + 152]; mov [rip + .pv_rflags], rax   # RFLAGS
    mov  rax, [rsp + 160]; mov [rip + .pv_rsp],    rax   # user RSP
    mov  rax, [rsp + 168]; mov [rip + .pv_ss],     rax   # SS
    mov  rax, cr2;         mov [rip + .pv_cr2],    rax
    mov  rax, cr3;         mov [rip + .pv_cr3],    rax
    mov  rax, [rip + current_pid];  mov [rip + .pv_pid],   rax
    mov  rax, [rip + pit_ticks];    mov [rip + .pv_ticks], rax

    # ── Clear screen ─────────────────────────────────────────────
    mov  rdi, 0xB8000
    mov  rcx, 80*25
    mov  eax, 0x0720        # space, grey on black
.pv_clr:
    mov  [rdi], ax; add rdi, 2; dec rcx; jnz .pv_clr

    # ── Row 0: banner ─────────────────────────────────────────────
    # "KERNEL PANIC  vec=XX (NAME)"  attr 0x4F
    mov  rdi, 0xB8000
    mov  r15b, 0x4F
    lea  rsi, [rip + .pv_str_banner]; call .pv_puts
    mov  rbx, [rip + .pv_vec];        call .pv_dec8
    mov  al, ' '; call .pv_putc
    mov  al, '('; call .pv_putc
    # look up vector name (16-byte slots)
    mov  rax, [rip + .pv_vec]
    cmp  rax, 31; jle .pv_goodvec
    mov  rax, 31   # clamp
.pv_goodvec:
    imul rax, rax, 16
    lea  rsi, [rip + .pv_vec_names]
    add  rsi, rax
    call .pv_puts
    mov  al, ')'; call .pv_putc

    # ── Row 1: RIP CS RFLAGS [decoded] ───────────────────────────
    mov  rdi, 0xB8000 + (1*80*2)
    mov  r15b, 0x1F
    lea  rsi,[rip+.pv_s_rip];    call .pv_puts
    mov  rbx,[rip+.pv_rip];      call .pv_hex16
    lea  rsi,[rip+.pv_s_cs];     call .pv_puts
    mov  rbx,[rip+.pv_cs];       call .pv_hex4
    lea  rsi,[rip+.pv_s_fl];     call .pv_puts
    mov  rbx,[rip+.pv_rflags];   call .pv_hex8
    # decode flags
    mov  al,' '; call .pv_putc; mov al,'['; call .pv_putc
    mov  rax, [rip + .pv_rflags]
    # CF PF AF ZF SF IF DF OF
    lea  rsi, [rip + .pv_flagbits]
    lea  rcx, [rip + .pv_flagchrs]
    mov  r8, 8
.pv_fl_lp:
    movzx rbx, byte ptr [rsi]
    bt   rax, rbx
    jnc  .pv_fl_skip
    movzx rbx, byte ptr [rcx]
    mov  [rdi], bl; mov byte ptr [rdi+1], r15b; add rdi, 2
.pv_fl_skip:
    inc rsi; inc rcx; dec r8; jnz .pv_fl_lp
    mov al,']'; call .pv_putc

    # ── Row 2: SS RSP EC [decode] CR2 ────────────────────────────
    mov  rdi, 0xB8000 + (2*80*2)
    mov  r15b, 0x1F
    lea  rsi,[rip+.pv_s_ss];   call .pv_puts
    mov  rbx,[rip+.pv_ss];     call .pv_hex4
    lea  rsi,[rip+.pv_s_rsp];  call .pv_puts
    mov  rbx,[rip+.pv_rsp];    call .pv_hex16
    lea  rsi,[rip+.pv_s_ec];   call .pv_puts
    mov  rbx,[rip+.pv_ec];     call .pv_hex4
    # #PF error code decode
    mov  r9, [rip + .pv_vec]
    cmp  r9, 14; jne .pv_no_pf_decode
    mov  rax,[rip+.pv_ec]
    mov  al,' '; call .pv_putc; mov al,'['; call .pv_putc
    bt   rax,0; jnc .pv_br_np;  lea rsi,[rip+.pv_ec_p];  jmp .pv_ec_p0
.pv_br_np:                       lea rsi,[rip+.pv_ec_np]
.pv_ec_p0: call .pv_puts
    bt   rax,1; jnc .pv_br_r;  lea rsi,[rip+.pv_ec_w];  jmp .pv_ec_w0
.pv_br_r:                       lea rsi,[rip+.pv_ec_r]
.pv_ec_w0: call .pv_puts
    bt   rax,2; jnc .pv_br_k;  lea rsi,[rip+.pv_ec_u];  jmp .pv_ec_u0
.pv_br_k:                       lea rsi,[rip+.pv_ec_k]
.pv_ec_u0: call .pv_puts
    bt   rax,3; jnc .pv_no_rsvd; lea rsi,[rip+.pv_ec_rsvd]; call .pv_puts
.pv_no_rsvd:
    bt   rax,4; jnc .pv_no_id;   lea rsi,[rip+.pv_ec_id];   call .pv_puts
.pv_no_id:
    mov  al,']'; call .pv_putc
.pv_no_pf_decode:
    lea  rsi,[rip+.pv_s_cr2];  call .pv_puts
    mov  rbx,[rip+.pv_cr2];    call .pv_hex16

    # ── Row 3: CR3 PID TICKS ─────────────────────────────────────
    mov  rdi, 0xB8000 + (3*80*2)
    mov  r15b, 0x1F
    lea  rsi,[rip+.pv_s_cr3];   call .pv_puts
    mov  rbx,[rip+.pv_cr3];     call .pv_hex16
    lea  rsi,[rip+.pv_s_pid];   call .pv_puts
    mov  rbx,[rip+.pv_pid];     call .pv_hex4
    lea  rsi,[rip+.pv_s_ticks]; call .pv_puts
    mov  rbx,[rip+.pv_ticks];   call .pv_hex16

    # ── Rows 4-7: GP registers ───────────────────────────────────
    # Stack: +112=rax +104=rbx +96=rcx +88=rdx +80=rsi +72=rdi
    #        +64=rbp  +56=r8   +48=r9  +40=r10 +32=r11 +24=r12
    #        +16=r13  +8=r14   +0=r15
    mov  rdi, 0xB8000 + (4*80*2); mov r15b, 0x1F
    lea  rsi,[rip+.pv_s_rax]; call .pv_puts; mov rbx,[rsp+112]; call .pv_hex16
    lea  rsi,[rip+.pv_s_rbx]; call .pv_puts; mov rbx,[rsp+104]; call .pv_hex16
    lea  rsi,[rip+.pv_s_rcx]; call .pv_puts; mov rbx,[rsp+ 96]; call .pv_hex16
    lea  rsi,[rip+.pv_s_rdx]; call .pv_puts; mov rbx,[rsp+ 88]; call .pv_hex16

    mov  rdi, 0xB8000 + (5*80*2)
    lea  rsi,[rip+.pv_s_rsi]; call .pv_puts; mov rbx,[rsp+ 80]; call .pv_hex16
    lea  rsi,[rip+.pv_s_rdi]; call .pv_puts; mov rbx,[rsp+ 72]; call .pv_hex16
    lea  rsi,[rip+.pv_s_rbp]; call .pv_puts; mov rbx,[rsp+ 64]; call .pv_hex16
    lea  rsi,[rip+.pv_s_r8];  call .pv_puts; mov rbx,[rsp+ 56]; call .pv_hex16

    mov  rdi, 0xB8000 + (6*80*2)
    lea  rsi,[rip+.pv_s_r9];  call .pv_puts; mov rbx,[rsp+ 48]; call .pv_hex16
    lea  rsi,[rip+.pv_s_r10]; call .pv_puts; mov rbx,[rsp+ 40]; call .pv_hex16
    lea  rsi,[rip+.pv_s_r11]; call .pv_puts; mov rbx,[rsp+ 32]; call .pv_hex16
    lea  rsi,[rip+.pv_s_r12]; call .pv_puts; mov rbx,[rsp+ 24]; call .pv_hex16

    mov  rdi, 0xB8000 + (7*80*2)
    lea  rsi,[rip+.pv_s_r13]; call .pv_puts; mov rbx,[rsp+ 16]; call .pv_hex16
    lea  rsi,[rip+.pv_s_r14]; call .pv_puts; mov rbx,[rsp+  8]; call .pv_hex16
    lea  rsi,[rip+.pv_s_r15]; call .pv_puts; mov rbx,[rsp+  0]; call .pv_hex16

    # ── Row 8: stack dump header ──────────────────────────────────
    mov  rdi, 0xB8000 + (8*80*2); mov r15b, 0x4F
    lea  rsi,[rip+.pv_s_stk]; call .pv_puts

    # ── Rows 9-12: 4 rows × 4 qwords from RSP_user ───────────────
    mov  rbp, [rip + .pv_rsp]   # start address
    mov  r13, 9                  # VGA row counter
    mov  r14, 4                  # row count
.pv_sdump:
    mov  rax, r13; imul rax, rax, 160; add rax, 0xB8000
    mov  rdi, rax; mov r15b, 0x2F

    lea  rsi,[rip+.pv_s_at]; call .pv_puts
    mov  rbx, rbp;            call .pv_hex16
    mov  al,':'; call .pv_putc; mov al,' '; call .pv_putc

    mov  r12, 4
.pv_sdcell:
    cmp  rbp, 0x1000
    jl   .pv_sdbad
    mov  rbx, [rbp]; jmp .pv_sdok
.pv_sdbad: mov rbx, 0xBADBADBADBADBADB
.pv_sdok:
    call .pv_hex16; mov al,' '; call .pv_putc
    add  rbp, 8; dec r12; jnz .pv_sdcell
    inc  r13; dec r14; jnz .pv_sdump

.pv_halt:
    hlt; jmp .pv_halt

# ── Helpers ─────────────────────────────────────────────────────
.pv_puts:
    push rax
.pv_puts_l:
    movzx eax, byte ptr [rsi]; test al,al; jz .pv_puts_e
    mov [rdi],al; mov byte ptr [rdi+1],r15b; add rdi,2; inc rsi; jmp .pv_puts_l
.pv_puts_e: pop rax; ret

.pv_putc:
    mov [rdi],al; mov byte ptr [rdi+1],r15b; add rdi,2; ret

.pv_hex16:
    push rax; push rcx
    mov rax,rbx; mov rcx,16
.pv_h16l: rol rax,4; push rax; and rax,0xF
    lea rbx,[rip+.pv_hx]; movzx eax,byte ptr [rbx+rax]
    mov [rdi],al; mov byte ptr [rdi+1],r15b; add rdi,2
    pop rax; dec rcx; jnz .pv_h16l
    pop rcx; pop rax; ret

.pv_hex8:
    push rax; push rcx
    mov rax,rbx; shl rax,32; mov rcx,8; jmp .pv_h16l

.pv_hex4:
    push rax; push rcx
    mov rax,rbx; shl rax,48; mov rcx,4; jmp .pv_h16l

.pv_dec8:   # print rbx as decimal (up to 3 digits)
    push rax; push rbx; push rcx; push rdx
    mov rax,rbx; xor rdx,rdx; mov rcx,100; div rcx
    test rax,rax; jz .pv_d_skip_h
    add al,'0'; mov [rdi],al; mov byte ptr [rdi+1],r15b; add rdi,2
.pv_d_skip_h:
    mov rax,rdx; xor rdx,rdx; mov rcx,10; div rcx
    test rax,rax; jz .pv_d_skip_t
    add al,'0'; mov [rdi],al; mov byte ptr [rdi+1],r15b; add rdi,2
.pv_d_skip_t:
    add dl,'0'; mov [rdi],dl; mov byte ptr [rdi+1],r15b; add rdi,2
    pop rdx; pop rcx; pop rbx; pop rax; ret

# ── Data ────────────────────────────────────────────────────────
.align 8
.pv_vec:    .quad 0
.pv_ec:     .quad 0
.pv_rip:    .quad 0
.pv_cs:     .quad 0
.pv_rflags: .quad 0
.pv_rsp:    .quad 0
.pv_ss:     .quad 0
.pv_cr2:    .quad 0
.pv_cr3:    .quad 0
.pv_pid:    .quad 0
.pv_ticks:  .quad 0

.pv_hx:     .ascii "0123456789abcdef"

.pv_str_banner: .asciz "KERNEL PANIC  vec="
.pv_s_rip:      .asciz "RIP="
.pv_s_cs:       .asciz " CS="
.pv_s_fl:       .asciz " FL="
.pv_s_ss:       .asciz "SS="
.pv_s_rsp:      .asciz " RSP="
.pv_s_ec:       .asciz " EC="
.pv_s_cr2:      .asciz " CR2="
.pv_s_cr3:      .asciz "CR3="
.pv_s_pid:      .asciz " PID="
.pv_s_ticks:    .asciz " TICKS="
.pv_s_rax:      .asciz "RAX="
.pv_s_rbx:      .asciz " RBX="
.pv_s_rcx:      .asciz " RCX="
.pv_s_rdx:      .asciz " RDX="
.pv_s_rsi:      .asciz "RSI="
.pv_s_rdi:      .asciz " RDI="
.pv_s_rbp:      .asciz " RBP="
.pv_s_r8:       .asciz " R8="
.pv_s_r9:       .asciz "R9="
.pv_s_r10:      .asciz "  R10="
.pv_s_r11:      .asciz " R11="
.pv_s_r12:      .asciz " R12="
.pv_s_r13:      .asciz "R13="
.pv_s_r14:      .asciz " R14="
.pv_s_r15:      .asciz " R15="
.pv_s_stk:      .asciz "Stack dump (RSP_user):"
.pv_s_at:       .asciz "  "

.pv_flagbits: .byte 0,2,4,6,7,9,10,11   # CF PF AF ZF SF IF DF OF
.pv_flagchrs: .ascii "CPAZSIDo"

.pv_ec_p:    .asciz "P"
.pv_ec_np:   .asciz "NP"
.pv_ec_w:    .asciz "W"
.pv_ec_r:    .asciz "R"
.pv_ec_u:    .asciz "U"
.pv_ec_k:    .asciz "K"
.pv_ec_rsvd: .asciz "RSVD"
.pv_ec_id:   .asciz "ID"

# 32 vector names, 16 bytes each (null-padded to exactly 16)
.pv_vec_names:
    .ascii "#DE div0\0       "   # 0  (pad to 16)
    .ascii "#DB debug\0      "   # 1
    .ascii "#NMI\0           "   # 2
    .ascii "#BP breakpt\0    "   # 3
    .ascii "#OF overflow\0   "   # 4
    .ascii "#BR bound\0      "   # 5
    .ascii "#UD bad opcode\0 "   # 6
    .ascii "#NM no FPU\0     "   # 7
    .ascii "#DF dbl fault\0  "   # 8
    .ascii "#coproc seg\0    "   # 9
    .ascii "#TS bad TSS\0    "   # 10
    .ascii "#NP !present\0   "   # 11
    .ascii "#SS stack\0      "   # 12
    .ascii "#GP gen prot\0   "   # 13
    .ascii "#PF page fault\0 "   # 14
    .ascii "#reserved\0      "   # 15
    .ascii "#MF x87\0        "   # 16
    .ascii "#AC align\0      "   # 17
    .ascii "#MC machine\0    "   # 18
    .ascii "#XF SIMD\0       "   # 19
    .ascii "#VE virt\0       "   # 20
    .ascii "#CP ctrl prot\0  "   # 21
    .ascii "#reserved\0      "   # 22
    .ascii "#reserved\0      "   # 23
    .ascii "#reserved\0      "   # 24
    .ascii "#reserved\0      "   # 25
    .ascii "#reserved\0      "   # 26
    .ascii "#reserved\0      "   # 27
    .ascii "#HV hypervisor\0 "   # 28
    .ascii "#VC VMM comm\0   "   # 29
    .ascii "#SX security\0   "   # 30
    .ascii "#reserved\0      "   # 31


# ================================================================
#  isr_page_fault — vector 14 (#PF) handler with demand-paging/CoW
#
#  Stack on entry (CPU already pushed error code + iretq frame):
#    [RSP+ 0]  error code
#    [RSP+ 8]  RIP
#    [RSP+16]  CS
#    [RSP+24]  RFLAGS
#    [RSP+32]  RSP_user
#    [RSP+40]  SS_user
#
#  Calls vmm_page_fault(fault_addr=CR2, error_code).
#  If it returns 1 (handled) -> iretq to resume.
#  If it returns 0 (unhandled) -> fall through to isr_errcode panic.
# ================================================================
.globl isr_page_fault
isr_page_fault:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11

    # rdi = CR2 (faulting address), rsi = error code
    # After 10 pushes (80 bytes), error code is at [rsp+80]
    mov  rdi, cr2
    mov  rsi, [rsp + 80]

    # Call vmm_page_fault(fault_addr, error_code) -- C calling convention
    # We need a 16-byte aligned stack for the call.
    # Currently RSP may not be aligned; sub 8 to align then call.
    sub  rsp, 8
    call vmm_page_fault
    add  rsp, 8

    # rax = return value: 1 = handled, 0 = panic
    test rax, rax
    jz   .pf_panic

    # Handled: restore registers and resume
    pop  r11
    pop  r10
    pop  r9
    pop  r8
    pop  rdi
    pop  rsi
    pop  rdx
    pop  rcx
    pop  rbx
    pop  rax
    add  rsp, 8        # skip error code
    iretq

.pf_panic:
    # Restore scratch regs then fall into the generic panic display
    pop  r11
    pop  r10
    pop  r9
    pop  r8
    pop  rdi
    pop  rsi
    pop  rdx
    pop  rcx
    pop  rbx
    pop  rax
    # Stack now: [error_code][RIP][CS][RFLAGS][RSP][SS]
    # Jump into isr_errcode which expects exactly this layout
    push 14           # vector #PF
    jmp  .panic_common

# ================================================================
#  timer_isr — Bug 1 re-fix
#
#  The previous "Bug 1 Fix" stashed rsp BEFORE the 15 GP pushes and
#  stored THAT value into PCB_KSTACK.  But the restore path does:
#    mov rsp, PCB_KSTACK
#    pop r15; pop r14; ... pop rax   (15 pops × 8 = 120 bytes)
#    iretq
#  so PCB_KSTACK must be the POST-push rsp (top of GP-save area).
#  Storing the pre-push value put rsp 120 bytes too high on restore,
#  causing it to pop garbage and iretq with RIP=0 → #GP → triple fault.
#
#  Fix: store rsp AFTER all 15 pushes into PCB_KSTACK.
# ================================================================
timer_isr:
    inc  qword ptr [rip + pit_ticks]

    push rax;  push rbx;  push rcx;  push rdx
    push rsi;  push rdi;  push rbp
    push r8;   push r9;   push r10;  push r11
    push r12;  push r13;  push r14;  push r15

    # ── Advance PCM mixer every tick ─────────────────────────────
    call sys_snd_mix_tick

    # ── Watchdog tick ─────────────────────────────────────────────
    call watchdog_tick

    # ── Software cursor blink ─────────────────────────────────────
    call cursor_blink_tick

    # ── Find RUNNING process ──────────────────────────────────────
    mov  rbx, PROC_TABLE
    xor  r12, r12
    xor  r13, r13
    mov  rcx, PROC_MAX
.ti_scan:
    cmp  qword ptr [rbx + PCB_STATE], PSTATE_RUNNING
    je   .ti_found_cur
    add  rbx, PROC_PCB_SZ
    inc  r13
    dec  rcx
    jnz  .ti_scan
    xor  r13, r13
    jmp  .ti_pick

.ti_found_cur:
    mov  r12, rbx
    # FIX (Bug 1 re-fix): store the POST-GP-push RSP into PCB_KSTACK.
    # The restore path does:
    #   mov rsp, PCB_KSTACK
    #   pop r15; pop r14; ... pop rax   ← 15 pops = 120 bytes
    #   iretq                           ← consumes CPU frame below
    # So PCB_KSTACK must point at the TOP of the GP-register save area
    # (i.e. the current RSP right here, after all 15 pushes).
    # Storing the PRE-push RSP (the old Bug 1 "fix") means restore
    # loads an RSP that is 120 bytes too HIGH, pops garbage into the
    # GP regs, and iretqs with RIP/CS/RFLAGS = 0 → #GP → triple fault.
    mov  qword ptr [r12 + PCB_KSTACK], rsp
    mov  rax, cr3
    mov  qword ptr [r12 + PCB_CR3], rax
    mov  qword ptr [r12 + PCB_STATE], PSTATE_READY
    inc  r13

.ti_pick:
    mov  rcx, PROC_MAX
    mov  r14, r13
.ti_search:
    cmp  r14, PROC_MAX
    jl   .ti_no_wrap
    xor  r14, r14
.ti_no_wrap:
    imul rax, r14, PROC_PCB_SZ
    add  rax, PROC_TABLE
    cmp  qword ptr [rax + PCB_STATE], PSTATE_READY
    je   .ti_found_next
    inc  r14
    dec  rcx
    jnz  .ti_search

    test r12, r12
    jz   .ti_idle

    mov  qword ptr [r12 + PCB_STATE], PSTATE_RUNNING
    mov  rsp, qword ptr [r12 + PCB_KSTACK]
    jmp  .ti_restore

.ti_idle:
    mov  al, PIC_EOI; out PIC1_CMD, al
    pop  r15; pop r14; pop r13; pop r12
    pop  r11; pop r10; pop r9;  pop r8
    pop  rbp; pop rdi; pop rsi; pop rdx
    pop  rcx; pop rbx; pop rax
    iretq

.ti_found_next:
    mov  rbx, rax
    mov  qword ptr [rbx + PCB_STATE], PSTATE_RUNNING

    mov  rax, qword ptr [rbx + PCB_CR3]
    test rax, rax
    jz   .ti_skip_cr3
    mov  cr3, rax
.ti_skip_cr3:

    # Restore stack to the pre-GP-push top for this process
    mov  rsp, qword ptr [rbx + PCB_KSTACK]

    mov  rax, qword ptr [rbx + PCB_PID]
    mov  [rip + current_pid], rax

.ti_restore:
    mov  al, PIC_EOI; out PIC1_CMD, al
    pop  r15; pop r14; pop r13; pop r12
    pop  r11; pop r10; pop r9;  pop r8
    pop  rbp; pop rdi; pop rsi; pop rdx
    pop  rcx; pop rbx; pop rax
    iretq

.align 8
# (no .ti_saved_rsp — PCB_KSTACK is stored post-GP-push directly)

# ================================================================
#  scheduler_exit — mark RUNNING process DEAD, free resources.
#  Caller must already have interrupts disabled.
# ================================================================
scheduler_exit:
    cli
    mov  rbx, PROC_TABLE
    mov  rcx, PROC_MAX
.se_find:
    cmp  qword ptr [rbx + PCB_STATE], PSTATE_RUNNING
    je   .se_found
    add  rbx, PROC_PCB_SZ
    dec  rcx
    jnz  .se_find
    sti
    ret

.se_found:
    mov  qword ptr [rbx + PCB_STATE], PSTATE_DEAD

    mov  rax, qword ptr [rbx + PCB_CR3]
    test rax, rax
    jz   .se_no_vmm
    push rbx
    mov  rdi, rax
    call vmm_destroy
    pop  rbx
.se_no_vmm:

    mov  rdi, qword ptr [rbx + PCB_KBASE]
    call heap_free
    ret

# ================================================================
#  syscall_entry — BUG 3 FIX + RSP-scan fix
#
#  BUG 3 FIX: use the RUNNING process's PCB_KSTACK as kernel stack,
#  not a hardcoded address.
#
#  ADDITIONAL FIX: the original scan used RSP as the walk pointer
#  (mov rsp, PROC_TABLE; add rsp, PROC_PCB_SZ each iteration).
#  Fix: use RBX as the walk pointer, keep RSP on the user stack
#  until we have the kstack address. Cleaner and interrupt-safe.
# ================================================================
syscall_entry:
    # Preserve user RSP — RSP still points at user stack here
    mov  [rip + .sc_user_rsp], rsp

    # Save RCX immediately: SYSCALL deposits user return RIP into RCX.
    # The PCB scan below clobbers RCX as a loop counter.
    mov  [rip + .sc_user_rcx], rcx

    # Find RUNNING process using RBX as walk pointer (not RSP)
    mov  rbx, PROC_TABLE
    mov  rcx, PROC_MAX
.sc_find_pcb:
    cmp  qword ptr [rbx + PCB_STATE], PSTATE_RUNNING
    je   .sc_got_pcb
    add  rbx, PROC_PCB_SZ
    dec  rcx
    jnz  .sc_find_pcb
    # Fallback: no running process — use private emergency stack
    lea  rsp, [rip + .sc_emergency_stack_top]
    jmp  .sc_on_kstack
.sc_got_pcb:
    mov  rsp, qword ptr [rbx + PCB_KSTACK]
.sc_on_kstack:

    # Push the saved user RCX (return address), NOT the loop-clobbered value
    push qword ptr [rip + .sc_user_rcx]
    push r11
    push rdi;  push rsi;  push rdx
    push r8;   push r9;   push r10

    cmp  rax, 352
    ja   .sc_bad

    lea  rbx, [rip + syscall_table]
    mov  rbx, qword ptr [rbx + rax * 8]
    test rbx, rbx
    jz   .sc_bad

    call rbx

.sc_ret:
    pop  r10;  pop r9;   pop r8
    pop  rdx;  pop rsi;  pop rdi
    pop  r11
    pop  rcx
    mov  rsp, [rip + .sc_user_rsp]
    sysretq

.sc_bad:
    mov  rax, -38
    jmp  .sc_ret

.align 8
.sc_user_rsp: .quad 0
.sc_user_rcx: .quad 0

.align 16
.sc_emergency_stack: .space 4096
.sc_emergency_stack_top:

# ================================================================
#  Syscall dispatch table
# ================================================================
.align 8
.globl syscall_table
syscall_table:
    .quad sys_read              # 0
    .quad sys_write             # 1
    .quad sys_open              # 2
    .quad sys_close             # 3
    .quad sys_exit_handler      # 4
    .quad sys_malloc            # 5  malloc (libc calls syscall 5 for heap alloc)
    .quad sys_lstat             # 6  lstat
    .quad sys_poll              # 7  poll
    .quad sys_lseek             # 8
    .quad sys_mmap              # 9
    .quad sys_mprotect          # 10
    .quad sys_munmap            # 11
    .quad sys_brk               # 12
    .quad sys_sigaction         # 13 rt_sigaction
    .quad sys_sigprocmask       # 14 rt_sigprocmask
    .quad 0                     # 15 rt_sigreturn (stub)
    .quad sys_ioctl             # 16 ioctl
    .quad 0                     # 17 pread64 (stub)
    .quad 0                     # 18 pwrite64 (stub)
    .quad 0                     # 19 readv (stub)
    .quad sys_writev            # 20 writev
    .quad sys_access            # 21 access
    .quad sys_pipe              # 22 pipe
    .quad sys_select; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0   # 23-30
    .quad 0                     # 31 tgkill (stub)
    .quad sys_dup               # 32
    .quad sys_dup2              # 33 dup2
    .quad 0                     # 34
    .quad sys_nanosleep         # 35 nanosleep
    .quad 0; .quad 0; .quad 0   # 36-38
    .quad sys_getpid            # 39
    .quad 0                     # 40
    .quad sys_socket            # 41 socket
    .quad sys_connect           # 42 connect
    .quad sys_accept            # 43 accept
    .quad sys_send              # 44 send
    .quad sys_recv              # 45 recv
    .quad 0; .quad 0; .quad 0   # 46-48
    .quad sys_bind              # 49 bind
    .quad sys_listen            # 50 listen
    .quad 0; .quad 0; .quad 0; .quad 0; .quad 0   # 51-55
    .quad sys_clone             # 56 clone
    .quad sys_fork              # 57 fork
    .quad 0                     # 58
    .quad sys_execve            # 59 execve
    .quad sys_exit_handler      # 60 exit
    .quad sys_wait4             # 61 wait4
    .quad sys_lseek62           # 62 lseek
    .quad sys_uname             # 63
    .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0   # 64-71
    .quad sys_fcntl             # 72 fcntl
    .quad 0; .quad 0; .quad 0; .quad 0   # 73-76
    .quad sys_getdents64        # 77 getdents
    .quad sys_getdents64        # 78 getdents64
    .quad sys_getcwd_real       # 79 getcwd (real cwd)
    .quad sys_chdir             # 80 chdir
    .quad 0                     # 81
    .quad sys_rename            # 82
    .quad sys_mkdir             # 83
    .quad sys_rmdir             # 84
    .quad 0; .quad 0            # 85-86
    .quad sys_unlink            # 87
    .quad 0                     # 88
    .quad sys_readlink          # 89 readlink
    .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0   # 90-95
    .quad sys_gettimeofday      # 96
    .quad 0; .quad 0             # 97-98
    .quad sys_sysinfo           # 99 sysinfo
    .quad 0; .quad 0             # 100-101
    .quad sys_getuid            # 102
    .quad 0                     # 103
    .quad sys_getuid            # 104 getgid
    .quad 0; .quad 0            # 105-106
    .quad sys_getuid            # 107 geteuid
    .quad sys_getuid            # 108 getegid
    .quad sys_getppid           # 110 (getppid)
    .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0   # 110-116
    .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0   # 117-124
    .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0   # 125-132
    .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0   # 133-140
    .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0   # 141-148
    .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0   # 149-156
    .quad 0                                                                     # 157
    .quad sys_arch_prctl        # 158
    .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0   # 159-166
    .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0   # 167-174
    .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0   # 175-182
    .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0   # 183-190
    .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0   # 191-198
    .quad 0; .quad 0; .quad 0           # 199-201
    .quad sys_futex                     # 202 futex
    .quad 0; .quad 0; .quad 0; .quad 0; .quad 0   # 203-207
    .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad sys_epoll_create; .quad 0   # 207-214
    .quad 0; .quad 0; .quad 0            # 215-217
    .quad sys_stub              # 218 set_tid_address
    .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0   # 219-226
    .quad 0; .quad sys_clock_gettime; .quad sys_clock_getres; .quad 0   # 227-230
    .quad sys_exit_group        # 231 exit_group
    .quad sys_epoll_wait; .quad sys_epoll_ctl; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0   # 232-239
    .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0   # 240-247
    .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0   # 248-255
    .quad 0                     # 256
    .quad sys_openat            # 257
    .quad 0; .quad 0; .quad 0; .quad 0   # 258-261
    .quad sys_fstatat           # 262
    # 263-299: reserved
    .quad sys_clock_getres; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0   # 263-270
    .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0   # 271-278
    .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0   # 279-286
    .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0   # 287-294
    .quad 0; .quad 0; .quad 0; .quad 0; .quad 0                               # 295-299
    .quad sys_poll_keys         # 300
    .quad sys_poll_mouse        # 301
    .quad sys_poll_pad          # 302
    .quad sys_yield; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0            # 303-309
    .quad sys_gfx_flip          # 310
    .quad sys_gfx_clear         # 311
    .quad sys_gfx_blit          # 312
    .quad sys_gfx_set_colorkey  # 313
    .quad sys_gfx_draw_tile     # 314
    .quad sys_gfx_set_tilemap   # 315
    .quad sys_gfx_render_layer  # 316
    .quad 0; .quad 0; .quad 0   # 317-319
    .quad sys_snd_opl_write     # 320
    .quad sys_snd_opl_note      # 321
    .quad sys_snd_opl_reset     # 322
    .quad sys_snd_mix_play      # 323
    .quad sys_snd_mix_stop      # 324
    .quad sys_snd_mix_volume    # 325
    .quad sys_snd_mix_tick      # 326
    .quad sys_gettime_ms        # 327
    .quad sys_mouse_setmode     # 328
    .quad sys_ipc_send      # 329
    .quad sys_ipc_recv      # 330
    .quad sys_ipc_register  # 331
    .quad sys_ipc_lookup    # 332
    .quad sys_net_dns               # 333
    .quad sys_vga_clear; .quad sys_read_char; .quad 0             # 334-336
    .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0   # 337-344
    .quad 0; .quad 0; .quad 0; .quad 0; .quad 0                               # 345-349
    .quad sys_pkg_install       # 350
    .quad sys_pkg_remove        # 351
    .quad sys_pkg_list          # 352
    .quad sys_watchdog_pet      # 353
.align 8
current_pid:        .quad 0
pit_ticks:          .quad 0
kernel_return_rsp:  .quad 0

.extern sys_read, sys_write, sys_open, sys_close
.extern sys_exit_handler, sys_malloc, sys_free, sys_yield
.extern sys_mmap, sys_mprotect, sys_munmap, sys_brk
.extern sys_writev, sys_stub, sys_stub_noent, sys_stub_enosys
.extern sys_futex
.extern sys_getpid, sys_uname, sys_getdents64, sys_getcwd
.extern sys_gettimeofday, sys_getuid, sys_getgid, sys_arch_prctl
.extern sys_openat, sys_fstatat
.extern sys_clock_gettime, sys_clock_getres
.extern sys_fstat, sys_lseek, sys_lseek62, sys_dup, sys_dup2
.extern sys_rename, sys_mkdir, sys_rmdir, sys_unlink
.extern sys_gettime_ms, sys_mouse_setmode
.extern sys_ipc_send, sys_ipc_recv, sys_ipc_register, sys_ipc_lookup
.extern sys_poll_keys, sys_poll_mouse, sys_poll_pad
.extern sys_vga_clear, sys_read_char
.extern sys_poll, sys_select, sys_fcntl
.extern sys_epoll_create, sys_epoll_ctl, sys_epoll_wait
.extern sys_gfx_flip, sys_gfx_clear, sys_gfx_blit, sys_gfx_set_colorkey
.extern sys_gfx_draw_tile, sys_gfx_set_tilemap, sys_gfx_render_layer
.extern sys_snd_opl_write, sys_snd_opl_note, sys_snd_opl_reset
.extern sys_snd_mix_play, sys_snd_mix_stop, sys_snd_mix_volume, sys_snd_mix_tick
.extern watchdog_tick
.extern cursor_blink_tick
.extern vmm_destroy, heap_free
.extern sys_pipe, sys_socket, sys_bind, sys_connect, sys_listen, sys_accept
.extern sys_send, sys_recv, sys_close_socket
.extern sys_fork, sys_execve, sys_clone, sys_wait4, sys_exit_group
.extern sys_pkg_install, sys_pkg_remove, sys_pkg_list
.extern sys_net_dns, sys_watchdog_pet
.extern sys_lstat, sys_ioctl, sys_access, sys_nanosleep
.extern sys_chdir, sys_readlink, sys_sysinfo, sys_getppid
.extern sys_sigaction, sys_sigprocmask, sys_stat
.extern sys_getcwd_real
