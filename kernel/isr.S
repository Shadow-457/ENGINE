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
.globl isr_errcode
isr_errcode:
    cli
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    # After 6 pushes (48 bytes), exception frame is at:
    #   error code @ [rsp+48], RIP @ [rsp+56]
    mov  rbx, [rsp + 48]      # error code
    mov  rcx, [rsp + 56]      # faulting RIP
    mov  rdx, cr2             # fault address (#PF)

    mov  rdi, 0xB8000         # VGA framebuffer base

    # Print "FAULT ec=" (9 chars)
    lea  rsi, [rip + .fe_label]
    mov  rax, 9
.fe_lp: movzx ebx, byte ptr [rsi]
    mov  [rdi], bl;  mov  byte ptr [rdi+1], 0x4F
    add  rsi, 1;  add  rdi, 2;  dec  rax;  jnz  .fe_lp

    # Print error code — 4 hex digits
    mov  rax, [rsp + 48]
    mov  rax, rbx             # restore rbx = error code
    mov  rbx, [rsp + 48]
    call .print_hex4

    # Print " rip=" (5 chars)
    lea  rsi, [rip + .rip_lbl]
    mov  rax, 5
.rl_lp: movzx ebx, byte ptr [rsi]
    mov  [rdi], bl;  mov  byte ptr [rdi+1], 0x4F
    add  rsi, 1;  add  rdi, 2;  dec  rax;  jnz  .rl_lp

    # Print RIP — 16 hex digits
    mov  rbx, rcx             # rcx = faulting RIP
    call .print_hex16

    # Print " cr2=" (5 chars)
    lea  rsi, [rip + .cr2_lbl]
    mov  rax, 5
.cl_lp: movzx ebx, byte ptr [rsi]
    mov  [rdi], bl;  mov  byte ptr [rdi+1], 0x4F
    add  rsi, 1;  add  rdi, 2;  dec  rax;  jnz  .cl_lp

    # Print CR2 — 16 hex digits
    mov  rbx, rdx             # rdx = cr2
    call .print_hex16

.isr_hlt:
    hlt
    jmp  .isr_hlt

# ── helpers (local, called with call — rdi = VGA cursor) ─────────
.print_hex4:                  # print low 16 bits of rbx as 4 hex digits
    mov  rax, rbx
    shl  rax, 48              # shift interesting nibbles to top
    mov  rcx, 4
    jmp  .hex_loop
.print_hex16:                 # print rbx as 16 hex digits
    mov  rax, rbx
    mov  rcx, 16
.hex_loop:
    rol  rax, 4
    mov  rbx, rax
    and  rbx, 0xF
    lea  rsi, [rip + .hx]
    movzx ebx, byte ptr [rsi + rbx]
    mov  [rdi], bl;  mov  byte ptr [rdi+1], 0x4F
    add  rdi, 2;  dec  rcx;  jnz  .hex_loop
    ret

.fe_label:  .ascii "FAULT ec="
.rip_lbl:   .ascii " rip="
.cr2_lbl:   .ascii " cr2="
.hx:        .ascii "0123456789abcdef"

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
    jmp  isr_errcode

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

    cmp  rax, 332
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
    .quad sys_fstat             # 5
    .quad sys_free              # 6
    .quad sys_yield             # 7
    .quad sys_lseek             # 8
    .quad sys_mmap              # 9
    .quad sys_mprotect          # 10
    .quad sys_munmap            # 11
    .quad sys_brk               # 12
    .quad sys_stub              # 13 rt_sigaction
    .quad sys_stub              # 14 rt_sigprocmask
    .quad sys_pipe              # 22 pipe
    .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0   # 23-30
    .quad sys_dup               # 32
    .quad sys_dup2              # 33 dup2
    .quad 0; .quad 0; .quad 0; .quad 0; .quad 0   # 34-38
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
    .quad sys_stub              # 72 fcntl
    .quad 0; .quad 0; .quad 0; .quad 0   # 73-76
    .quad sys_getdents64        # 77 getdents
    .quad sys_getdents64        # 78 getdents64
    .quad sys_getcwd            # 79
    .quad sys_stub              # 80 chdir
    .quad 0                     # 81
    .quad sys_rename            # 82
    .quad sys_mkdir             # 83
    .quad sys_rmdir             # 84
    .quad 0; .quad 0            # 85-86
    .quad sys_unlink            # 87
    .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0   # 88-95
    .quad sys_gettimeofday      # 96
    .quad 0; .quad 0; .quad 0; .quad 0; .quad 0   # 97-101
    .quad sys_getuid            # 102
    .quad 0                     # 103
    .quad sys_getuid            # 104 getgid
    .quad 0; .quad 0            # 105-106
    .quad sys_getuid            # 107 geteuid
    .quad sys_getuid            # 108 getegid
    .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0   # 109-116
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
    .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0   # 199-206
    .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0   # 207-214
    .quad 0; .quad 0; .quad 0            # 215-217
    .quad sys_stub              # 218 set_tid_address
    .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0   # 219-226
    .quad 0; .quad 0; .quad 0; .quad 0   # 227-230
    .quad sys_exit_group        # 231 exit_group
    .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0   # 232-239
    .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0   # 240-247
    .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0   # 248-255
    .quad 0                     # 256
    .quad sys_openat            # 257
    .quad 0; .quad 0; .quad 0; .quad 0   # 258-261
    .quad sys_fstatat           # 262
    # 263-299: reserved
    .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0   # 263-270
    .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0   # 271-278
    .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0   # 279-286
    .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0   # 287-294
    .quad 0; .quad 0; .quad 0; .quad 0; .quad 0                               # 295-299
    .quad sys_poll_keys         # 300
    .quad sys_poll_mouse        # 301
    .quad sys_poll_pad          # 302
    .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0            # 303-309
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
    .quad 0; .quad 0; .quad 0; .quad 0                           # 333-336
    .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0; .quad 0   # 337-344
    .quad 0; .quad 0; .quad 0; .quad 0; .quad 0                               # 345-349
    .quad sys_pkg_install       # 350
    .quad sys_pkg_remove        # 351
    .quad sys_pkg_list          # 352
.align 8
current_pid:        .quad 0
pit_ticks:          .quad 0
kernel_return_rsp:  .quad 0

.extern sys_read, sys_write, sys_open, sys_close
.extern sys_exit_handler, sys_malloc, sys_free, sys_yield
.extern sys_mmap, sys_mprotect, sys_munmap, sys_brk
.extern sys_writev, sys_stub, sys_stub_noent, sys_stub_enosys
.extern sys_getpid, sys_uname, sys_getdents64, sys_getcwd
.extern sys_gettimeofday, sys_getuid, sys_getgid, sys_arch_prctl
.extern sys_openat, sys_fstatat
.extern sys_fstat, sys_lseek, sys_lseek62, sys_dup, sys_dup2
.extern sys_rename, sys_mkdir, sys_rmdir, sys_unlink
.extern sys_gettime_ms, sys_mouse_setmode
.extern sys_ipc_send, sys_ipc_recv, sys_ipc_register, sys_ipc_lookup
.extern sys_poll_keys, sys_poll_mouse, sys_poll_pad
.extern sys_gfx_flip, sys_gfx_clear, sys_gfx_blit, sys_gfx_set_colorkey
.extern sys_gfx_draw_tile, sys_gfx_set_tilemap, sys_gfx_render_layer
.extern sys_snd_opl_write, sys_snd_opl_note, sys_snd_opl_reset
.extern sys_snd_mix_play, sys_snd_mix_stop, sys_snd_mix_volume, sys_snd_mix_tick
.extern watchdog_tick
.extern vmm_destroy, heap_free
.extern sys_pipe, sys_socket, sys_bind, sys_connect, sys_listen, sys_accept
.extern sys_send, sys_recv, sys_close_socket
.extern sys_fork, sys_execve, sys_clone, sys_wait4, sys_exit_group
.extern sys_pkg_install, sys_pkg_remove, sys_pkg_list
