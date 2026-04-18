# ================================================================
#  SHADOW OS — Stage 1 Bootloader (512 bytes, MBR)
#
#  Uses INT 13h AH=42h (LBA extensions) — no 127-sector limit.
#  Falls back to CHS if LBA not available.
#  Loads 512 sectors (256KB) in four 128-sector reads.
#  Kernel sits at physical 0x8000.
# ================================================================
.code16
.intel_syntax noprefix

.globl _start
_start:
    mov  [drive_id], dl
    xor  ax, ax
    mov  ds, ax
    mov  es, ax
    mov  ss, ax
    mov  sp, 0x7C00

    mov  si, offset msg_boot
    call print16

    # Check LBA extensions (AH=41h)
    mov  ah, 0x41
    mov  bx, 0x55AA
    mov  dl, [drive_id]
    int  0x13
    jc   .chs_read

    # LBA read #1: LBA 1..128 -> 0x0000:0x8000 (phys 0x8000..0x17FFF)
    mov  word ptr [dap_count],  128
    mov  word ptr [dap_buf_off], 0x8000
    mov  word ptr [dap_buf_seg], 0x0000
    mov  dword ptr [dap_lba_lo], 1
    mov  dword ptr [dap_lba_hi], 0
    lea  si, [dap]
    mov  ah, 0x42
    mov  dl, [drive_id]
    int  0x13
    jc   .disk_err

    # LBA read #2: LBA 129..256 -> 0x1800:0x0000 (phys 0x18000..0x27FFF)
    mov  word ptr [dap_count],  128
    mov  word ptr [dap_buf_off], 0x0000
    mov  word ptr [dap_buf_seg], 0x1800
    mov  dword ptr [dap_lba_lo], 129
    mov  dword ptr [dap_lba_hi], 0
    lea  si, [dap]
    mov  ah, 0x42
    mov  dl, [drive_id]
    int  0x13
    jc   .disk_err

    # LBA read #3: LBA 257..384 -> 0x2800:0x0000 (phys 0x28000..0x37FFF)
    mov  word ptr [dap_count],  128
    mov  word ptr [dap_buf_off], 0x0000
    mov  word ptr [dap_buf_seg], 0x2800
    mov  dword ptr [dap_lba_lo], 257
    mov  dword ptr [dap_lba_hi], 0
    lea  si, [dap]
    mov  ah, 0x42
    mov  dl, [drive_id]
    int  0x13
    jc   .disk_err

    # LBA read #4: LBA 385..512 -> 0x3800:0x0000 (phys 0x38000..0x47FFF)
    mov  word ptr [dap_count],  128
    mov  word ptr [dap_buf_off], 0x0000
    mov  word ptr [dap_buf_seg], 0x3800
    mov  dword ptr [dap_lba_lo], 385
    mov  dword ptr [dap_lba_hi], 0
    lea  si, [dap]
    mov  ah, 0x42
    mov  dl, [drive_id]
    int  0x13
    jc   .disk_err
    jmp  .done

.chs_read:
    mov  ax, 0x0280
    mov  bx, 0x8000
    mov  cx, 0x0002
    xor  dh, dh
    mov  dl, [drive_id]
    int  0x13
    jc   .disk_err

    mov  ax, 0x1800
    mov  es, ax
    mov  ax, 0x0280
    xor  bx, bx
    mov  cx, 0x0003
    mov  dh, 0x01
    mov  dl, [drive_id]
    int  0x13
    jc   .disk_err

    mov  ax, 0x2800
    mov  es, ax
    mov  ax, 0x0280
    xor  bx, bx
    mov  cx, 0x0004
    mov  dh, 0x02
    mov  dl, [drive_id]
    int  0x13
    jc   .disk_err

    mov  ax, 0x3800
    mov  es, ax
    mov  ax, 0x0280
    xor  bx, bx
    mov  cx, 0x0005
    mov  dh, 0x03
    mov  dl, [drive_id]
    int  0x13
    jc   .disk_err
    xor  ax, ax
    mov  es, ax

.done:
    mov  si, offset msg_ok
    call print16
    jmp  0x0000:0x8000

.disk_err:
    mov  si, offset msg_err
    call print16
    cli
    hlt

print16:
    push ax
    push bx
    mov  ah, 0x0E
.ploop:
    lodsb
    test al, al
    jz   .pdone
    mov  bx, 0x0007
    int  0x10
    jmp  .ploop
.pdone:
    pop  bx
    pop  ax
    ret

.align 2
dap:
    .byte 0x10, 0x00
dap_count:   .word 0
dap_buf_off: .word 0
dap_buf_seg: .word 0
dap_lba_lo:  .long 0
dap_lba_hi:  .long 0

msg_boot: .asciz "ENGINE loading...\r\n"
msg_ok:   .asciz "OK\r\n"
msg_err:  .asciz "Disk Error!\r\n"
drive_id: .byte 0

.org 446
.byte 0x80
.byte 0x00, 0x01, 0x01
.byte 0x83
.byte 0x00, 0x00, 0x00
.long 1
.long 63

.byte 0x00
.byte 0x00, 0x00, 0x00
.byte 0x0C
.byte 0x00, 0x00, 0x00
.long 512
.long 67456

.space 16, 0
.space 16, 0
.word 0xAA55
