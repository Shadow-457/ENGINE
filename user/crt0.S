# ================================================================
#  SHADOW OS — user/crt0.S
#  C runtime startup for user programs running on Shadow OS.
#
#  WHY THIS MUST BE ASM (cannot be written in C):
#   - Must execute BEFORE main() — it IS the ELF entry point _start
#   - Aligns the stack to 16 bytes (AMD64 ABI requirement for SSE)
#   - Calls main() then issues SYSCALL exit — bootstraps C runtime
#   - A C function assumes crt0 already ran; you cannot write crt0 in C
# ================================================================

.code64
.intel_syntax noprefix

.section .note.GNU-stack,"",@progbits   /* mark stack non-executable */

.section .text
.globl _start
_start:
    # ── Align stack to 16 bytes (System V AMD64 ABI) ─────────────
    and  rsp, -16

    # ── Seed default environment variables ───────────────────────
    call env_init_defaults

    # ── Zero argc / argv / envp (no shell arg passing yet) ───────
    xor  rdi, rdi       # argc = 0
    xor  rsi, rsi       # argv = NULL
    xor  rdx, rdx       # envp = NULL

    # ── Call user main() ─────────────────────────────────────────
    call main

    # ── Exit: syscall 60 (exit), code = return value of main ─────
    mov  rdi, rax       # exit code
    mov  rax, 60
    syscall

    # ── Fallback (should never reach here) ───────────────────────
    cli
.spin:
    hlt
    jmp  .spin
