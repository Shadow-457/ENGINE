/*
 * shc — Shadow Language Compiler
 *
 * Self-contained: no nasm, no ld, no system().
 * Reads a .shadow source file, emits a native ELF64 binary directly.
 * Runs on Linux host OR inside ShadowOS (uses only raw syscalls).
 *
 * Shadow syntax:
 *   set x to 10
 *   set y to x + 5
 *   if x greater y then
 *       print x
 *   else
 *       print y
 *   end
 *   repeat while x greater 0
 *       print x
 *       set x to x - 1
 *   end
 *   return 0
 *
 * Build (host):    gcc -O2 -o shc user/shc.c
 * Build (ShadowOS): compiled with crt0.S, no libc
 *   gcc -m64 -O2 -ffreestanding -fno-stack-protector -mno-red-zone \
 *       -nostdlib -nostdinc -c -o user/shc.o user/shc.c
 *   ld -m elf_x86_64 -static -nostdlib -Ttext=0x400000 \
 *      -o SHC user/crt0.o user/shc.o
 * Then: make addprog PROG=SHC
 * In ShadowOS: elf SHC  (then type filename at prompt)
 */

/* ================================================================
 *  MINIMAL LIBC  (raw Linux/ShadowOS syscalls — no hosted libc)
 * ================================================================ */

typedef unsigned long  size_t;
typedef long           ssize_t;
typedef long           off_t;
typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef unsigned long  u64;
typedef long           i64;

#define NULL  ((void*)0)

/* ── syscall wrappers ─────────────────────────────────────────── */

static ssize_t sys_read(int fd, void *buf, size_t n) {
    ssize_t r;
    __asm__ volatile("syscall"
        : "=a"(r)
        : "0"(0L), "D"((long)fd), "S"(buf), "d"(n)
        : "rcx","r11","memory");
    return r;
}

static ssize_t sys_write(int fd, const void *buf, size_t n) {
    ssize_t r;
    __asm__ volatile("syscall"
        : "=a"(r)
        : "0"(1L), "D"((long)fd), "S"(buf), "d"(n)
        : "rcx","r11","memory");
    return r;
}

/* open: flags O_RDONLY=0, O_WRONLY|O_CREAT|O_TRUNC = 0x241 */
static int sys_open(const char *path, int flags, int mode) {
    long r;
    __asm__ volatile("syscall"
        : "=a"(r)
        : "0"(2L), "D"(path), "S"((long)flags), "d"((long)mode)
        : "rcx","r11","memory");
    return (int)r;
}

static int sys_close(int fd) {
    long r;
    __asm__ volatile("syscall"
        : "=a"(r)
        : "0"(3L), "D"((long)fd)
        : "rcx","r11","memory");
    return (int)r;
}

static void sys_exit(int code) {
    __asm__ volatile("syscall"
        :: "a"(60L), "D"((long)code)
        : "rcx","r11");
    __builtin_unreachable();
}

/* brk for a tiny bump allocator */
static void *sys_brk(void *addr) {
    void *r;
    __asm__ volatile("syscall"
        : "=a"(r)
        : "0"(12L), "D"(addr)
        : "rcx","r11","memory");
    return r;
}

/* ── bump allocator ───────────────────────────────────────────── */
static char  *heap_cur  = NULL;
static char  *heap_end  = NULL;

static void *sh_malloc(size_t n) {
    /* align to 8 */
    n = (n + 7) & ~(size_t)7;
    if (!heap_cur) {
        heap_cur = (char*)sys_brk(NULL);
        heap_end = heap_cur;
    }
    if (heap_cur + n > heap_end) {
        /* extend by at least 64 KB at a time */
        size_t ext = n < 65536 ? 65536 : n;
        heap_end = (char*)sys_brk(heap_end + ext);
        if ((long)heap_end < 0) return NULL;
    }
    void *p = heap_cur;
    heap_cur += n;
    return p;
}

/* ── string helpers ───────────────────────────────────────────── */
static size_t sh_strlen(const char *s) {
    size_t n = 0; while (s[n]) n++; return n;
}
static void sh_memcpy(void *d, const void *s, size_t n) {
    u8 *dd = (u8*)d; const u8 *ss = (const u8*)s;
    for (size_t i = 0; i < n; i++) dd[i] = ss[i];
}
static void sh_memset(void *d, int c, size_t n) {
    u8 *p = (u8*)d; for (size_t i = 0; i < n; i++) p[i] = (u8)c;
}
static int sh_strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (u8)*a - (u8)*b;
}
static int sh_strncmp(const char *a, const char *b, size_t n) {
    while (n-- && *a && *a == *b) { a++; b++; }
    return n == (size_t)-1 ? 0 : (u8)*a - (u8)*b;
}
static void sh_strncpy(char *d, const char *s, size_t n) {
    size_t i = 0;
    while (i < n - 1 && s[i]) { d[i] = s[i]; i++; }
    d[i] = 0;
}
static char *sh_strrchr(const char *s, int c) {
    const char *r = NULL;
    while (*s) { if (*s == (char)c) r = s; s++; }
    return (char*)r;
}
static int sh_isdigit(int c)  { return c >= '0' && c <= '9'; }
static int sh_isalpha(int c)  { return (c>='a'&&c<='z')||(c>='A'&&c<='Z'); }
static int sh_isalnum(int c)  { return sh_isalpha(c) || sh_isdigit(c); }
static int sh_isspace(int c)  { return c==' '||c=='\t'||c=='\n'||c=='\r'; }

/* ── print helpers ────────────────────────────────────────────── */
static void sh_puts(const char *s) {
    sys_write(1, s, sh_strlen(s));
}
static void sh_putc(char c) {
    sys_write(1, &c, 1);
}
static void sh_print_long(long v) {
    if (v < 0) { sh_putc('-'); v = -v; }
    char buf[20]; int i = 0;
    if (v == 0) { sh_putc('0'); return; }
    while (v) { buf[i++] = '0' + (int)(v % 10); v /= 10; }
    while (i--) sh_putc(buf[i]);
}

/* ── file helpers ─────────────────────────────────────────────── */
static char *read_file(const char *path, size_t *out_size) {
    int fd = sys_open(path, 0, 0);  /* O_RDONLY */
    if (fd < 0) return NULL;

    /* read in 4 KB chunks */
    size_t cap = 4096, used = 0;
    char *buf = (char*)sh_malloc(cap);
    if (!buf) { sys_close(fd); return NULL; }

    for (;;) {
        if (used + 512 > cap) {
            char *nb = (char*)sh_malloc(cap * 2);
            if (!nb) { sys_close(fd); return NULL; }
            sh_memcpy(nb, buf, used);
            buf = nb; cap *= 2;
        }
        ssize_t n = sys_read(fd, buf + used, 512);
        if (n <= 0) break;
        used += (size_t)n;
    }
    sys_close(fd);
    buf[used] = 0;
    if (out_size) *out_size = used;
    return buf;
}

static int write_file(const char *path, const void *data, size_t size) {
    /* O_WRONLY|O_CREAT|O_TRUNC = 1|64|512 = 577, mode 0755 = 0x1ED */
    int fd = sys_open(path, 577, 0x1ED);
    if (fd < 0) return -1;
    const u8 *p = (const u8*)data;
    size_t written = 0;
    while (written < size) {
        ssize_t n = sys_write(fd, p + written, size - written);
        if (n <= 0) break;
        written += (size_t)n;
    }
    sys_close(fd);
    return (written == size) ? 0 : -1;
}

/* ── die ──────────────────────────────────────────────────────── */
static void die(const char *msg) {
    sh_puts("[shc] error: ");
    sh_puts(msg);
    sh_putc('\n');
    sys_exit(1);
}

/* ================================================================
 *  CODE BUFFER  — growable byte array for emitted machine code
 * ================================================================ */

typedef struct {
    u8    *data;
    size_t size;
    size_t cap;
} Buf;

static void buf_init(Buf *b) {
    b->cap  = 4096;
    b->data = (u8*)sh_malloc(b->cap);
    b->size = 0;
}

static void buf_reserve(Buf *b, size_t extra) {
    if (b->size + extra <= b->cap) return;
    size_t nc = b->cap * 2 + extra;
    u8 *nb = (u8*)sh_malloc(nc);
    sh_memcpy(nb, b->data, b->size);
    b->data = nb;
    b->cap  = nc;
}

static void emit1(Buf *b, u8 byte) {
    buf_reserve(b, 1);
    b->data[b->size++] = byte;
}
static void emit2(Buf *b, u8 a, u8 b2) { emit1(b,a); emit1(b,b2); }
static void emit3(Buf *b, u8 a, u8 b2, u8 c) { emit1(b,a); emit1(b,b2); emit1(b,c); }

static void emit_i32(Buf *b, i64 v) {
    u32 u = (u32)(long)v;
    emit1(b, (u8)(u));
    emit1(b, (u8)(u >> 8));
    emit1(b, (u8)(u >> 16));
    emit1(b, (u8)(u >> 24));
}
static void emit_i64(Buf *b, u64 v) {
    for (int i = 0; i < 8; i++) { emit1(b, (u8)(v & 0xFF)); v >>= 8; }
}

/* ================================================================
 *  x86-64 INSTRUCTION EMITTER
 *
 *  Registers used by shc codegen:
 *    rax — accumulator / result
 *    rcx — second operand
 *    rdx — idiv remainder / scratch
 *    rdi — arg to syscall / print_int
 *    rbp — frame pointer
 *    rsp — stack pointer
 *    r8  — print_int: absolute value temp
 *    r9  — print_int: digit pointer
 *    al  — low byte of rax (for setcc)
 * ================================================================ */

/* push rbp        55 */
static void emit_push_rbp(Buf *b) { emit1(b, 0x55); }
/* push rax        50 */
static void emit_push_rax(Buf *b) { emit1(b, 0x50); }
/* pop rbp         5D */
static void emit_pop_rbp(Buf *b)  { emit1(b, 0x5D); }
/* pop rax         58 */
static void emit_pop_rax(Buf *b)  { emit1(b, 0x58); }

/* mov rbp, rsp    48 89 E5 */
static void emit_mov_rbp_rsp(Buf *b) { emit3(b, 0x48, 0x89, 0xE5); }
/* mov rsp, rbp    48 89 EC */
static void emit_mov_rsp_rbp(Buf *b) { emit3(b, 0x48, 0x89, 0xEC); }

/* sub rsp, imm32  48 81 EC <imm32> */
static void emit_sub_rsp_imm(Buf *b, int imm) {
    emit3(b, 0x48, 0x81, 0xEC);
    emit_i32(b, imm);
}

/* add rsp, imm32  48 81 C4 <imm32> */
static void emit_add_rsp_imm(Buf *b, int imm) {
    emit3(b, 0x48, 0x81, 0xC4);
    emit_i32(b, imm);
}

/* mov rax, imm64  48 B8 <imm64> */
static void emit_mov_rax_imm(Buf *b, i64 imm) {
    emit2(b, 0x48, 0xB8);
    emit_i64(b, (u64)imm);
}

/* mov rdi, imm64  48 BF <imm64> */
static void emit_mov_rdi_imm(Buf *b, i64 imm) {
    emit2(b, 0x48, 0xBF);
    emit_i64(b, (u64)imm);
}

/* mov rcx, imm64  48 B9 <imm64> */
static void emit_mov_rcx_imm(Buf *b, i64 imm) {
    emit2(b, 0x48, 0xB9);
    emit_i64(b, (u64)imm);
}

/* mov rdx, imm64  48 BA <imm64> */
static void emit_mov_rdx_imm(Buf *b, i64 imm) {
    emit2(b, 0x48, 0xBA);
    emit_i64(b, (u64)imm);
}

/* mov rsi, imm64  48 BE <imm64> */
static void emit_mov_rsi_imm(Buf *b, i64 imm) {
    emit2(b, 0x48, 0xBE);
    emit_i64(b, (u64)imm);
}

/* mov rcx, rax    48 89 C1 */
static void emit_mov_rcx_rax(Buf *b) { emit3(b, 0x48, 0x89, 0xC1); }
/* mov rdi, rax    48 89 C7 */
static void emit_mov_rdi_rax(Buf *b) { emit3(b, 0x48, 0x89, 0xC7); }
/* mov rax, rdx    48 89 D0 */
static void emit_mov_rax_rdx(Buf *b) { emit3(b, 0x48, 0x89, 0xD0); }
/* mov rax, rdi    48 89 F8 */
static void emit_mov_rax_rdi(Buf *b) { emit3(b, 0x48, 0x89, 0xF8); }
/* mov rsi, r9    4C 89 CE */
static void emit_mov_rsi_r9(Buf *b)  { emit3(b, 0x4C, 0x89, 0xCE); }
/* mov r8, rdi    49 89 F8 */
static void emit_mov_r8_rdi(Buf *b)  { emit3(b, 0x49, 0x89, 0xF8); }
/* mov rax, r8    4C 89 C0 */
static void emit_mov_rax_r8(Buf *b)  { emit3(b, 0x4C, 0x89, 0xC0); }

/* mov [rbp + disp8], rax    48 89 45 <disp8>  (disp must be -128..127) */
/* mov [rbp + disp32], rax   48 89 85 <disp32> */
static void emit_mov_mem_rax(Buf *b, int disp) {
    if (disp >= -128 && disp <= 127) {
        emit3(b, 0x48, 0x89, 0x45);
        emit1(b, (u8)(char)disp);
    } else {
        emit3(b, 0x48, 0x89, 0x85);
        emit_i32(b, disp);
    }
}

/* mov rax, [rbp + disp]    48 8B 45 <d8> or 48 8B 85 <d32> */
static void emit_mov_rax_mem(Buf *b, int disp) {
    if (disp >= -128 && disp <= 127) {
        emit3(b, 0x48, 0x8B, 0x45);
        emit1(b, (u8)(char)disp);
    } else {
        emit3(b, 0x48, 0x8B, 0x85);
        emit_i32(b, disp);
    }
}

/* add rax, rcx   48 01 C8 */
static void emit_add_rax_rcx(Buf *b) { emit3(b, 0x48, 0x01, 0xC8); }
/* sub rax, rcx   48 29 C8 */
static void emit_sub_rax_rcx(Buf *b) { emit3(b, 0x48, 0x29, 0xC8); }
/* imul rax, rcx  48 0F AF C1 */
static void emit_imul_rax_rcx(Buf *b){ emit1(b,0x48);emit1(b,0x0F);emit1(b,0xAF);emit1(b,0xC1); }
/* cqo             48 99 */
static void emit_cqo(Buf *b)         { emit2(b, 0x48, 0x99); }
/* idiv rcx        48 F7 F9 */
static void emit_idiv_rcx(Buf *b)    { emit3(b, 0x48, 0xF7, 0xF9); }
/* neg rax         48 F7 D8 */
static void emit_neg_rax(Buf *b)     { emit3(b, 0x48, 0xF7, 0xD8); }
/* test rax, rax   48 85 C0 */
static void emit_test_rax_rax(Buf *b){ emit3(b, 0x48, 0x85, 0xC0); }
/* test rdi, rdi   48 85 FF */
static void emit_test_rdi_rdi(Buf *b){ emit3(b, 0x48, 0x85, 0xFF); }
/* cmp rax, rcx    48 39 C8 */
static void emit_cmp_rax_rcx(Buf *b) { emit3(b, 0x48, 0x39, 0xC8); }
/* movzx rax, al   48 0F B6 C0 */
static void emit_movzx_rax_al(Buf *b){ emit1(b,0x48);emit1(b,0x0F);emit1(b,0xB6);emit1(b,0xC0); }
/* syscall         0F 05 */
static void emit_syscall(Buf *b)     { emit2(b, 0x0F, 0x05); }
/* ret             C3 */
static void emit_ret(Buf *b)         { emit1(b, 0xC3); }

/* setcc al:   0F 9x C0 */
static void emit_setg(Buf *b)  { emit1(b,0x0F);emit1(b,0x9F);emit1(b,0xC0); }
static void emit_setl(Buf *b)  { emit1(b,0x0F);emit1(b,0x9C);emit1(b,0xC0); }
static void emit_sete(Buf *b)  { emit1(b,0x0F);emit1(b,0x94);emit1(b,0xC0); }
static void emit_setne(Buf *b) { emit1(b,0x0F);emit1(b,0x95);emit1(b,0xC0); }
static void emit_setge(Buf *b) { emit1(b,0x0F);emit1(b,0x9D);emit1(b,0xC0); }
static void emit_setle(Buf *b) { emit1(b,0x0F);emit1(b,0x9E);emit1(b,0xC0); }
static void emit_setz(Buf *b)  { emit_sete(b); }
static void emit_setnz(Buf *b) { emit_setne(b); }

/*
 * Jumps: emit a near rel32 jump placeholder, return offset of imm32
 * so the caller can patch it later.
 *
 * jmp rel32   E9 <rel32>   — 5 bytes total
 * jz  rel32   0F 84 <rel32>— 6 bytes total
 * jnz rel32   0F 85 <rel32>— 6 bytes total
 * jge rel32   0F 8D <rel32>— 6 bytes total
 */
static size_t emit_jmp(Buf *b) {
    emit1(b, 0xE9);
    size_t off = b->size;
    emit_i32(b, 0); /* placeholder */
    return off;
}
static size_t emit_jz(Buf *b) {
    emit1(b, 0x0F); emit1(b, 0x84);
    size_t off = b->size;
    emit_i32(b, 0);
    return off;
}
static size_t emit_jnz(Buf *b) {
    emit1(b, 0x0F); emit1(b, 0x85);
    size_t off = b->size;
    emit_i32(b, 0);
    return off;
}

/* Patch a previously emitted rel32 jump to jump to current position */
static void patch_jmp(Buf *b, size_t imm_off) {
    /* rel32 is relative to the instruction AFTER the imm32 field */
    i64 rel = (i64)b->size - (i64)(imm_off + 4);
    u8 *p = b->data + imm_off;
    p[0] = (u8)(rel);
    p[1] = (u8)(rel >> 8);
    p[2] = (u8)(rel >> 16);
    p[3] = (u8)(rel >> 24);
}

/* call rel32   E8 <rel32> */
static size_t emit_call(Buf *b) {
    emit1(b, 0xE8);
    size_t off = b->size;
    emit_i32(b, 0);
    return off;
}
static void patch_call(Buf *b, size_t imm_off, size_t target) {
    i64 rel = (i64)target - (i64)(imm_off + 4);
    u8 *p = b->data + imm_off;
    p[0] = (u8)(rel);
    p[1] = (u8)(rel >> 8);
    p[2] = (u8)(rel >> 16);
    p[3] = (u8)(rel >> 24);
}

/* and rsp, -16   48 83 E4 F0 */
static void emit_and_rsp_m16(Buf *b){ emit1(b,0x48);emit1(b,0x83);emit1(b,0xE4);emit1(b,0xF0); }

/* xor rdx, rdx   48 31 D2 */
static void emit_xor_rdx_rdx(Buf *b){ emit3(b, 0x48, 0x31, 0xD2); }

/* dec r9        49 FF C9 */
static void emit_dec_r9(Buf *b)     { emit3(b, 0x49, 0xFF, 0xC9); }

/* mov [r9], dl   41 88 11 */
static void emit_mov_r9mem_dl(Buf *b){ emit3(b, 0x41, 0x88, 0x11); }

/* div rcx (unsigned)  48 F7 F1 */
static void emit_div_rcx(Buf *b)    { emit3(b, 0x48, 0xF7, 0xF1); }

/* add dl, '0'   80 C2 30 */
static void emit_add_dl_30(Buf *b)  { emit3(b, 0x80, 0xC2, 0x30); }

/* jnz short rel8  75 <rel8> */
static size_t emit_jnz8(Buf *b) {
    emit1(b, 0x75);
    size_t off = b->size;
    emit1(b, 0x00);
    return off;
}
static void patch_jnz8(Buf *b, size_t imm_off) {
    i64 rel = (i64)b->size - (i64)(imm_off + 1);
    b->data[imm_off] = (u8)(char)rel;
}

/* jge short rel8  7D <rel8> */
static size_t emit_jge8(Buf *b) {
    emit1(b, 0x7D);
    size_t off = b->size;
    emit1(b, 0x00);
    return off;
}
static void patch_jge8(Buf *b, size_t imm_off) {
    i64 rel = (i64)b->size - (i64)(imm_off + 1);
    b->data[imm_off] = (u8)(char)rel;
}

/* jmp short rel8  EB <rel8> */
static size_t emit_jmp8(Buf *b) {
    emit1(b, 0xEB);
    size_t off = b->size;
    emit1(b, 0x00);
    return off;
}
static void patch_jmp8(Buf *b, size_t imm_off) {
    i64 rel = (i64)b->size - (i64)(imm_off + 1);
    b->data[imm_off] = (u8)(char)rel;
}

/* lea r9, [rip + disp32]   4C 8D 0D <disp32> */
static size_t emit_lea_r9_rip(Buf *b) {
    emit1(b,0x4C); emit1(b,0x8D); emit1(b,0x0D);
    size_t off = b->size;
    emit_i32(b, 0);
    return off;
}
/* patch rip-relative lea: target is absolute address of data */
static void patch_rip_lea(Buf *b, size_t imm_off, size_t target_offset_in_code) {
    i64 rel = (i64)target_offset_in_code - (i64)(imm_off + 4);
    u8 *p = b->data + imm_off;
    p[0]=(u8)rel; p[1]=(u8)(rel>>8); p[2]=(u8)(rel>>16); p[3]=(u8)(rel>>24);
}

/* lea rax, [rip + disp32]  48 8D 05 <disp32> */
static size_t emit_lea_rax_rip(Buf *b) {
    emit1(b,0x48); emit1(b,0x8D); emit1(b,0x05);
    size_t off = b->size;
    emit_i32(b, 0);
    return off;
}
static void patch_rip_lea_rax(Buf *b, size_t imm_off, size_t target_offset_in_code) {
    i64 rel = (i64)target_offset_in_code - (i64)(imm_off + 4);
    u8 *p = b->data + imm_off;
    p[0]=(u8)rel; p[1]=(u8)(rel>>8); p[2]=(u8)(rel>>16); p[3]=(u8)(rel>>24);
}

/* lea rsi, [rip + disp32]  48 8D 35 <disp32> */
static size_t emit_lea_rsi_rip(Buf *b) {
    emit1(b,0x48); emit1(b,0x8D); emit1(b,0x35);
    size_t off = b->size;
    emit_i32(b, 0);
    return off;
}
static void patch_rip_lea_rsi(Buf *b, size_t imm_off, size_t target_offset_in_code) {
    i64 rel = (i64)target_offset_in_code - (i64)(imm_off + 4);
    u8 *p = b->data + imm_off;
    p[0]=(u8)rel; p[1]=(u8)(rel>>8); p[2]=(u8)(rel>>16); p[3]=(u8)(rel>>24);
}

/* mov byte [r9], '-'   41 C6 01 2D */
static void emit_mov_r9mem_minus(Buf *b){ emit1(b,0x41);emit1(b,0xC6);emit1(b,0x01);emit1(b,0x2D); }

/* neg r8   4D F7 D8 */
static void emit_neg_r8(Buf *b)    { emit3(b, 0x4D, 0xF7, 0xD8); }

/* mov rcx, 10   48 B9 0A 00... */
static void emit_mov_rcx_10(Buf *b) { emit_mov_rcx_imm(b, 10); }

/* sub rax, r9  (for digit count)  4C 29 C8 */
static void emit_sub_rax_r9(Buf *b){ emit3(b, 0x4C, 0x29, 0xC8); }

/* mov rdx, rax  48 89 C2 */
static void emit_mov_rdx_rax(Buf *b){ emit3(b, 0x48, 0x89, 0xC2); }

/* ================================================================
 *  _shadow_print_int  — emit the runtime print_int function
 *
 *  Equivalent to the nasm version in the original shc.
 *  Calling convention: rdi = integer to print.
 *
 *  Uses a local buffer on the stack (32 bytes below rsp).
 *  Writes digits right-to-left, then sys_write.
 *
 *  Returns offset of the function start in the code buffer.
 * ================================================================ */

/*
 * Emitted logic — stack-based itoa, no static writable buffer needed:
 *
 * print_int(rdi):
 *   push rbp; mov rbp, rsp; sub rsp, 48
 *   ; buf[32] lives at rbp-48 .. rbp-17  (32 bytes on stack)
 *   ; r8  = abs(rdi)
 *   ; r9  = rbp - 17  (one past end of buf: buf[31] address)
 *   r8 = rdi
 *   if rdi < 0: neg r8
 *   lea r9, [rbp-17]
 *   rax = r8; rcx = 10
 * .digit_loop:
 *   xor rdx,rdx; div rcx; add dl,'0'; dec r9; [r9]=dl
 *   if rax!=0: jmp .digit_loop
 *   if rdi < 0: dec r9; [r9] = '-'
 *   rsi = r9
 *   lea rax, [rbp-17]
 *   rdx = rax - rsi   (length)
 *   sys_write(1, rsi, rdx)
 *   mov byte [rbp-17], 0x0a   ; newline on stack
 *   lea rsi, [rbp-17]
 *   mov rdx, 1
 *   sys_write(1, rsi, 1)
 *   mov rsp, rbp; pop rbp; ret
 */
static size_t emit_print_int_fn(Buf *b,
                                 size_t *out_newline_off,
                                 size_t *out_itoa_buf_off)
{
    /* these out params are no longer used for patching but kept for compat */
    *out_newline_off  = 0;
    *out_itoa_buf_off = 0;

    size_t fn_start = b->size;

    /* prologue — 48 bytes of local space: 32 for digit buf + 16 spare */
    emit_push_rbp(b);
    emit_mov_rbp_rsp(b);
    emit_sub_rsp_imm(b, 48);

    /* r8 = rdi */
    emit_mov_r8_rdi(b);

    /* test rdi, rdi; jge .pos */
    emit_test_rdi_rdi(b);
    size_t jge_pos = emit_jge8(b);
    /* neg r8 */
    emit_neg_r8(b);
    /* .pos: */
    patch_jge8(b, jge_pos);

    /* lea r9, [rbp-17]  →  4C 8D 4D EF
     * REX.WR=0x4C (R extends reg→r9), ModRM=0x4D (mod=01,reg=001,rm=101=rbp) */
    emit1(b,0x4C); emit1(b,0x8D); emit1(b,0x4D); emit1(b,0xEF);

    /* rax = r8 */
    emit_mov_rax_r8(b);
    /* rcx = 10 */
    emit_mov_rcx_10(b);

    /* .digit_loop: */
    size_t digit_loop = b->size;
    emit_xor_rdx_rdx(b);
    emit_div_rcx(b);
    emit_add_dl_30(b);
    emit_dec_r9(b);
    emit_mov_r9mem_dl(b);
    emit_test_rax_rax(b);
    /* jnz back to digit_loop */
    {
        emit1(b, 0x0F); emit1(b, 0x85);
        size_t off = b->size;
        emit_i32(b, 0);
        i64 rel = (i64)digit_loop - (i64)(b->size);
        u8 *p = b->data + off;
        p[0]=(u8)rel; p[1]=(u8)(rel>>8); p[2]=(u8)(rel>>16); p[3]=(u8)(rel>>24);
    }

    /* if negative: dec r9; [r9] = '-' */
    emit_test_rdi_rdi(b);
    size_t jge_nominus = emit_jge8(b);
    emit_dec_r9(b);
    emit_mov_r9mem_minus(b);
    patch_jge8(b, jge_nominus);

    /* rsi = r9  (start of digit string) */
    emit_mov_rsi_r9(b);

    /* lea rax, [rbp-17]  →  48 8D 45 EF */
    emit1(b,0x48); emit1(b,0x8D); emit1(b,0x45); emit1(b,0xEF);

    /* rdx = rax - rsi  (length) */
    emit3(b, 0x48, 0x29, 0xF0); /* sub rax, rsi */
    emit_mov_rdx_rax(b);

    /* sys_write(1, rsi, rdx) */
    emit_mov_rax_imm(b, 1);
    emit_mov_rdi_imm(b, 1);
    emit_syscall(b);

    /* write newline: store '\n' on stack, write 1 byte */
    /* mov byte [rbp-48], 0x0a  →  C6 45 D0 0A */
    emit1(b,0xC6); emit1(b,0x45); emit1(b,0xD0); emit1(b,0x0A);
    /* lea rsi, [rbp-48]  →  48 8D 75 D0 */
    emit1(b,0x48); emit1(b,0x8D); emit1(b,0x75); emit1(b,0xD0);
    emit_mov_rdx_imm(b, 1);
    emit_mov_rax_imm(b, 1);
    emit_mov_rdi_imm(b, 1);
    emit_syscall(b);

    /* epilogue */
    emit_mov_rsp_rbp(b);
    emit_pop_rbp(b);
    emit_ret(b);

    return fn_start;
}

/* ================================================================
 *  LEXER
 * ================================================================ */

#define MAX_TOKENS 4096
#define MAX_IDENT   64

typedef enum {
    TK_EOF=0, TK_INT, TK_IDENT,
    TK_SET, TK_TO, TK_IF, TK_THEN, TK_ELSE, TK_END,
    TK_REPEAT, TK_WHILE, TK_PRINT, TK_RETURN,
    TK_AND, TK_OR, TK_NOT,
    TK_GREATER, TK_LESS, TK_EQUALS, TK_NOTEQUALS,
    TK_GREATEREQUALS, TK_LESSEQUALS,
    TK_PLUS, TK_MINUS, TK_STAR, TK_SLASH, TK_PERCENT,
    TK_LPAREN, TK_RPAREN,
} TokenKind;

typedef struct {
    TokenKind kind;
    long      ival;
    char      sval[MAX_IDENT];
    int       line;
} Token;

typedef struct {
    const char *src;
    int         line;
    Token      *tokens;
    int         count;
} Lexer;

static void add_tok(Lexer *lx, TokenKind k, long iv, const char *sv) {
    if (lx->count >= MAX_TOKENS) die("too many tokens");
    Token *t = &lx->tokens[lx->count++];
    t->kind = k; t->ival = iv; t->line = lx->line;
    if (sv) sh_strncpy(t->sval, sv, MAX_IDENT); else t->sval[0] = 0;
}

static void lex(Lexer *lx) {
    const char *s = lx->src;
    int i = 0; lx->line = 1; lx->count = 0;

    while (s[i]) {
        if (s[i]=='\n') { lx->line++; i++; continue; }
        if (sh_isspace((u8)s[i])) { i++; continue; }
        if (s[i]=='#' || (s[i]=='/' && s[i+1]=='/')) {
            while (s[i] && s[i]!='\n') i++; continue;
        }
        if (sh_isdigit((u8)s[i])) {
            long v = 0;
            while (sh_isdigit((u8)s[i])) v = v*10 + (s[i++]-'0');
            add_tok(lx, TK_INT, v, NULL); continue;
        }
        if (sh_isalpha((u8)s[i]) || s[i]=='_') {
            char buf[MAX_IDENT]; int j = 0;
            while (sh_isalnum((u8)s[i]) || s[i]=='_') {
                if (j < MAX_IDENT-1) buf[j++] = s[i]; i++;
            }
            buf[j] = 0;
            struct { const char *w; TokenKind k; } kw[] = {
                {"set",TK_SET},{"to",TK_TO},{"if",TK_IF},{"then",TK_THEN},
                {"else",TK_ELSE},{"end",TK_END},{"repeat",TK_REPEAT},
                {"while",TK_WHILE},{"print",TK_PRINT},{"return",TK_RETURN},
                {"and",TK_AND},{"or",TK_OR},{"not",TK_NOT},
                {"greater",TK_GREATER},{"less",TK_LESS},
                {"equals",TK_EQUALS},{"notequals",TK_NOTEQUALS},
                {"greaterequals",TK_GREATEREQUALS},{"lessequals",TK_LESSEQUALS},
                {NULL,0}
            };
            int m = 0;
            for (int k=0; kw[k].w; k++)
                if (!sh_strcmp(buf, kw[k].w)) { add_tok(lx,kw[k].k,0,NULL); m=1; break; }
            if (!m) add_tok(lx, TK_IDENT, 0, buf);
            continue;
        }
        switch(s[i]) {
            case '+': add_tok(lx,TK_PLUS,0,NULL);    break;
            case '-': add_tok(lx,TK_MINUS,0,NULL);   break;
            case '*': add_tok(lx,TK_STAR,0,NULL);    break;
            case '/': add_tok(lx,TK_SLASH,0,NULL);   break;
            case '%': add_tok(lx,TK_PERCENT,0,NULL); break;
            case '(': add_tok(lx,TK_LPAREN,0,NULL);  break;
            case ')': add_tok(lx,TK_RPAREN,0,NULL);  break;
            default: {
                sh_puts("[shc] unknown char on line ");
                sh_print_long(lx->line);
                sh_putc('\n');
                sys_exit(1);
            }
        }
        i++;
    }
    add_tok(lx, TK_EOF, 0, NULL);
}

/* ================================================================
 *  PARSER / CODE GENERATOR
 * ================================================================ */

#define MAX_VARS   256
#define MAX_LABELS 1024

typedef struct {
    Token *tokens;
    int    pos;
    Buf   *code;

    /* variables */
    struct { char name[MAX_IDENT]; int offset; } vars[MAX_VARS];
    int var_count;
    int stack_size;

    /* label system: array of code offsets */
    size_t labels[MAX_LABELS];
    int    label_count;

    /* forward-jump patch list: (label_id, imm_off_in_code) */
    struct { int lbl; size_t off; } patches[MAX_LABELS * 4];
    int patch_count;

    /* where call to print_int needs patching */
    struct { size_t off; } print_calls[MAX_LABELS];
    int print_call_count;

    size_t print_int_offset; /* filled after print_int is emitted */
} Parser;

static Token *peek(Parser *p)               { return &p->tokens[p->pos]; }
static Token *advance(Parser *p)            { return &p->tokens[p->pos++]; }
static int    check(Parser *p, TokenKind k) { return peek(p)->kind == k; }
static Token *expect(Parser *p, TokenKind k) {
    if (!check(p,k)) {
        sh_puts("[shc] parse error line ");
        sh_print_long(peek(p)->line);
        sh_putc('\n');
        sys_exit(1);
    }
    return advance(p);
}

static int new_label(Parser *p) {
    if (p->label_count >= MAX_LABELS) die("too many labels");
    p->labels[p->label_count] = (size_t)-1; /* undefined */
    return p->label_count++;
}

/* Define label at current code position */
static void def_label(Parser *p, int lbl) {
    p->labels[lbl] = p->code->size;
}

/* Emit a forward jmp and record for patching */
static void emit_fwd_jmp(Parser *p, int lbl) {
    size_t off = emit_jmp(p->code);
    p->patches[p->patch_count].lbl = lbl;
    p->patches[p->patch_count].off = off;
    p->patch_count++;
}
static void emit_fwd_jz(Parser *p, int lbl) {
    size_t off = emit_jz(p->code);
    p->patches[p->patch_count].lbl = lbl;
    p->patches[p->patch_count].off = off;
    p->patch_count++;
}
static void emit_fwd_jnz(Parser *p, int lbl) {
    size_t off = emit_jnz(p->code);
    p->patches[p->patch_count].lbl = lbl;
    p->patches[p->patch_count].off = off;
    p->patch_count++;
}

/* Back-jump directly to a defined label */
static void emit_back_jmp(Parser *p, int lbl) {
    emit1(p->code, 0xE9);
    size_t here = p->code->size;
    i64 rel = (i64)p->labels[lbl] - (i64)(here + 4);
    emit_i32(p->code, rel);
}
static void emit_back_jmp_if_true(Parser *p, int lbl) {
    /* test rax, rax; jnz lbl */
    emit_test_rax_rax(p->code);
    emit1(p->code, 0x0F); emit1(p->code, 0x85);
    size_t here = p->code->size;
    i64 rel = (i64)p->labels[lbl] - (i64)(here + 4);
    emit_i32(p->code, rel);
}

static void resolve_patches(Parser *p) {
    for (int i = 0; i < p->patch_count; i++) {
        int lbl = p->patches[i].lbl;
        size_t off = p->patches[i].off;
        size_t target = p->labels[lbl];
        if (target == (size_t)-1) die("undefined label");
        patch_jmp(p->code, off);
        /* patch_jmp uses current size to compute rel; but target may differ */
        /* manually patch: */
        i64 rel = (i64)target - (i64)(off + 4);
        u8 *pp = p->code->data + off;
        pp[0]=(u8)rel; pp[1]=(u8)(rel>>8);
        pp[2]=(u8)(rel>>16); pp[3]=(u8)(rel>>24);
    }
}

static void resolve_print_calls(Parser *p) {
    for (int i = 0; i < p->print_call_count; i++) {
        size_t off = p->print_calls[i].off;
        patch_call(p->code, off, p->print_int_offset);
    }
}

static int var_offset(Parser *p, const char *name, int create) {
    for (int i = 0; i < p->var_count; i++)
        if (!sh_strcmp(p->vars[i].name, name)) return p->vars[i].offset;
    if (!create) {
        sh_puts("[shc] unknown variable: "); sh_puts(name); sh_putc('\n');
        sys_exit(1);
    }
    if (p->var_count >= MAX_VARS) die("too many variables");
    p->stack_size += 8;
    int off = -p->stack_size;
    sh_strncpy(p->vars[p->var_count].name, name, MAX_IDENT);
    p->vars[p->var_count].offset = off;
    p->var_count++;
    return off;
}

static void parse_stmt(Parser *p);
static void parse_expr(Parser *p);

static void parse_primary(Parser *p) {
    Token *t = peek(p);
    if (t->kind == TK_INT) {
        advance(p);
        emit_mov_rax_imm(p->code, t->ival);
    } else if (t->kind == TK_IDENT) {
        advance(p);
        int off = var_offset(p, t->sval, 0);
        emit_mov_rax_mem(p->code, off);
    } else if (t->kind == TK_LPAREN) {
        advance(p); parse_expr(p); expect(p, TK_RPAREN);
    } else {
        sh_puts("[shc] expected value or ( on line ");
        sh_print_long(t->line); sh_putc('\n');
        sys_exit(1);
    }
}

static void parse_unary(Parser *p) {
    if (check(p, TK_MINUS)) {
        advance(p); parse_unary(p);
        emit_neg_rax(p->code);
    } else if (check(p, TK_NOT)) {
        advance(p); parse_unary(p);
        emit_test_rax_rax(p->code);
        emit_setz(p->code);
        emit_movzx_rax_al(p->code);
    } else {
        parse_primary(p);
    }
}

static void parse_mul(Parser *p) {
    parse_unary(p);
    while (check(p,TK_STAR) || check(p,TK_SLASH) || check(p,TK_PERCENT)) {
        TokenKind op = advance(p)->kind;
        emit_push_rax(p->code);
        parse_unary(p);
        emit_mov_rcx_rax(p->code);
        emit_pop_rax(p->code);
        if (op == TK_STAR) {
            emit_imul_rax_rcx(p->code);
        } else {
            emit_cqo(p->code);
            emit_idiv_rcx(p->code);
            if (op == TK_PERCENT) emit_mov_rax_rdx(p->code);
        }
    }
}

static void parse_add(Parser *p) {
    parse_mul(p);
    while (check(p,TK_PLUS) || check(p,TK_MINUS)) {
        TokenKind op = advance(p)->kind;
        emit_push_rax(p->code);
        parse_mul(p);
        emit_mov_rcx_rax(p->code);
        emit_pop_rax(p->code);
        if (op == TK_PLUS) emit_add_rax_rcx(p->code);
        else               emit_sub_rax_rcx(p->code);
    }
}

static void parse_cmp(Parser *p) {
    parse_add(p);
    TokenKind op = peek(p)->kind;
    if (op==TK_GREATER || op==TK_LESS || op==TK_EQUALS ||
        op==TK_NOTEQUALS || op==TK_GREATEREQUALS || op==TK_LESSEQUALS) {
        advance(p);
        emit_push_rax(p->code);
        parse_add(p);
        emit_mov_rcx_rax(p->code);
        emit_pop_rax(p->code);
        emit_cmp_rax_rcx(p->code);
        switch(op) {
            case TK_GREATER:       emit_setg(p->code);  break;
            case TK_LESS:          emit_setl(p->code);  break;
            case TK_EQUALS:        emit_sete(p->code);  break;
            case TK_NOTEQUALS:     emit_setne(p->code); break;
            case TK_GREATEREQUALS: emit_setge(p->code); break;
            case TK_LESSEQUALS:    emit_setle(p->code); break;
            default: break;
        }
        emit_movzx_rax_al(p->code);
    }
}

static void parse_and_expr(Parser *p) {
    parse_cmp(p);
    while (check(p, TK_AND)) {
        int lend = new_label(p);
        advance(p);
        emit_test_rax_rax(p->code);
        emit_fwd_jz(p, lend);
        parse_cmp(p);
        emit_test_rax_rax(p->code);
        emit_setnz(p->code);
        emit_movzx_rax_al(p->code);
        def_label(p, lend);
    }
}

static void parse_expr(Parser *p) {
    parse_and_expr(p);
    while (check(p, TK_OR)) {
        int lt = new_label(p), le = new_label(p);
        advance(p);
        emit_test_rax_rax(p->code);
        emit_fwd_jnz(p, lt);
        parse_and_expr(p);
        emit_test_rax_rax(p->code);
        emit_setnz(p->code);
        emit_movzx_rax_al(p->code);
        emit_fwd_jmp(p, le);
        def_label(p, lt);
        emit_mov_rax_imm(p->code, 1);
        def_label(p, le);
    }
}

static void parse_block(Parser *p) {
    while (!check(p,TK_END) && !check(p,TK_ELSE) && !check(p,TK_EOF))
        parse_stmt(p);
}

static void parse_stmt(Parser *p) {
    Token *t = peek(p);

    if (t->kind == TK_SET) {
        advance(p);
        Token *name = expect(p, TK_IDENT);
        int off = var_offset(p, name->sval, 1);
        expect(p, TK_TO);
        parse_expr(p);
        emit_mov_mem_rax(p->code, off);
        return;
    }

    if (t->kind == TK_IF) {
        advance(p);
        int lelse = new_label(p), lend = new_label(p);
        parse_expr(p);
        expect(p, TK_THEN);
        emit_test_rax_rax(p->code);
        emit_fwd_jz(p, lelse);
        parse_block(p);
        emit_fwd_jmp(p, lend);
        def_label(p, lelse);
        if (check(p, TK_ELSE)) { advance(p); parse_block(p); }
        expect(p, TK_END);
        def_label(p, lend);
        return;
    }

    if (t->kind == TK_REPEAT) {
        advance(p);
        expect(p, TK_WHILE);
        int ls = new_label(p), le = new_label(p);
        def_label(p, ls);
        parse_expr(p);
        emit_test_rax_rax(p->code);
        emit_fwd_jz(p, le);
        parse_block(p);
        expect(p, TK_END);
        emit_back_jmp(p, ls);
        def_label(p, le);
        return;
    }

    if (t->kind == TK_PRINT) {
        advance(p);
        parse_expr(p);
        emit_mov_rdi_rax(p->code);
        /* sub rsp, 8 to keep 16-byte alignment before call
         * (our frame is 16-aligned at statement boundary;
         *  call pushes 8-byte ret addr, callee push rbp aligns to 16) */
        emit1(p->code, 0x48); emit1(p->code, 0x83);
        emit1(p->code, 0xEC); emit1(p->code, 0x08); /* sub rsp, 8 */
        size_t call_off = emit_call(p->code);
        p->print_calls[p->print_call_count++].off = call_off;
        emit1(p->code, 0x48); emit1(p->code, 0x83);
        emit1(p->code, 0xC4); emit1(p->code, 0x08); /* add rsp, 8 */
        return;
    }

    if (t->kind == TK_RETURN) {
        advance(p);
        parse_expr(p);
        /* exit syscall: rdi=rax, rax=60 */
        emit_mov_rdi_rax(p->code);
        emit_mov_rax_imm(p->code, 60);
        emit_syscall(p->code);
        return;
    }

    sh_puts("[shc] unexpected token on line ");
    sh_print_long(t->line); sh_putc('\n');
    sys_exit(1);
}

/* ================================================================
 *  ELF64 WRITER
 * ================================================================ */

#define LOAD_ADDR   0x400000UL
#define ELF_HDR_SZ  64
#define PHDR_SZ     56
#define HEADERS_SZ  (ELF_HDR_SZ + PHDR_SZ)

typedef struct __attribute__((packed)) {
    /* ELF ident */
    u8  ei_mag[4];
    u8  ei_class;
    u8  ei_data;
    u8  ei_version;
    u8  ei_osabi;
    u8  ei_pad[8];
    /* ELF header */
    u16 e_type;
    u16 e_machine;
    u32 e_version;
    u64 e_entry;
    u64 e_phoff;
    u64 e_shoff;
    u32 e_flags;
    u16 e_ehsize;
    u16 e_phentsize;
    u16 e_phnum;
    u16 e_shentsize;
    u16 e_shnum;
    u16 e_shstrndx;
} Elf64Hdr;

typedef struct __attribute__((packed)) {
    u32 p_type;
    u32 p_flags;
    u64 p_offset;
    u64 p_vaddr;
    u64 p_paddr;
    u64 p_filesz;
    u64 p_memsz;
    u64 p_align;
} Elf64Phdr;

static void write_elf(const char *out_path, u8 *code, size_t code_size,
                      size_t entry_offset)
{
    size_t file_size = HEADERS_SZ + code_size;
    u8 *file = (u8*)sh_malloc(file_size);
    sh_memset(file, 0, file_size);

    Elf64Hdr *hdr = (Elf64Hdr*)file;
    hdr->ei_mag[0] = 0x7F; hdr->ei_mag[1]='E';
    hdr->ei_mag[2] = 'L';  hdr->ei_mag[3]='F';
    hdr->ei_class   = 2;  /* ELFCLASS64 */
    hdr->ei_data    = 1;  /* little-endian */
    hdr->ei_version = 1;
    hdr->ei_osabi   = 0;
    hdr->e_type     = 2;  /* ET_EXEC */
    hdr->e_machine  = 62; /* EM_X86_64 */
    hdr->e_version  = 1;
    hdr->e_entry    = LOAD_ADDR + HEADERS_SZ + entry_offset;
    hdr->e_phoff    = ELF_HDR_SZ;
    hdr->e_shoff    = 0;
    hdr->e_ehsize   = ELF_HDR_SZ;
    hdr->e_phentsize= PHDR_SZ;
    hdr->e_phnum    = 1;
    hdr->e_shentsize= 64;
    hdr->e_shnum    = 0;
    hdr->e_shstrndx = 0;

    Elf64Phdr *phdr = (Elf64Phdr*)(file + ELF_HDR_SZ);
    phdr->p_type   = 1;          /* PT_LOAD */
    phdr->p_flags  = 5;          /* PF_R | PF_X */
    phdr->p_offset = 0;
    phdr->p_vaddr  = LOAD_ADDR;
    phdr->p_paddr  = LOAD_ADDR;
    phdr->p_filesz = file_size;
    phdr->p_memsz  = file_size + 4096; /* extra for itoa buf */
    phdr->p_align  = 0x200000;

    sh_memcpy(file + HEADERS_SZ, code, code_size);

    if (write_file(out_path, file, file_size) < 0) {
        sh_puts("[shc] cannot write output file\n");
        sys_exit(1);
    }
}

/* ================================================================
 *  ENTRY POINT  (main — called by crt0.S)
 * ================================================================ */

/*
 * Layout of the emitted binary (code buffer):
 *
 *  [0]        _start:   push rbp; mov rbp,rsp; sub rsp,<frame>
 *             ... user code ...
 *             implicit exit(0)
 *  [X]        _shadow_print_int:  (runtime helper)
 *             ... newline byte ...
 *             ... itoa_buf[32] ...
 *
 * The ELF entry point is set to LOAD_ADDR + HEADERS_SZ + 0 (_start).
 * _shadow_print_int is referenced by call rel32 instructions.
 *
 * All rip-relative addresses are computed assuming the code is loaded
 * at LOAD_ADDR + HEADERS_SZ (= 0x400078).
 */

/* read a line from stdin into buf, up to max-1 chars */
static int read_line_stdin(char *buf, int max) {
    int n = 0;
    while (n < max - 1) {
        char c;
        ssize_t r = sys_read(0, &c, 1);
        if (r <= 0 || c == '\n') break;
        if (c == '\r') continue;
        buf[n++] = c;
    }
    buf[n] = 0;
    return n;
}

int main(void) {
    /* ── get source filename ───────────────────────────────────── */
    char src_path[256];

    sh_puts("shc - Shadow Compiler\n");
    sh_puts("Enter .shadow filename: ");
    if (read_line_stdin(src_path, sizeof(src_path)) == 0)
        die("no filename given");

    /* ── compute output filename (strip .shadow → no extension) ── */
    char out_path[256];
    sh_strncpy(out_path, src_path, sizeof(out_path));
    char *dot = sh_strrchr(out_path, '.');
    if (dot && !sh_strcmp(dot, ".shadow")) *dot = 0;

    /* ── read source ─────────────────────────────────────────────  */
    sh_puts("[shc] reading "); sh_puts(src_path); sh_putc('\n');
    char *source = read_file(src_path, NULL);
    if (!source) {
        sh_puts("[shc] cannot open: "); sh_puts(src_path); sh_putc('\n');
        sys_exit(1);
    }

    /* ── lex ─────────────────────────────────────────────────── */
    Token *toks = (Token*)sh_malloc(sizeof(Token) * MAX_TOKENS);
    Lexer lx = { .src = source, .tokens = toks };
    lex(&lx);
    sh_puts("[shc] lexed "); sh_print_long(lx.count);
    sh_puts(" tokens\n");

    /* ── set up code buffer ─────────────────────────────────────── */
    Buf code;
    buf_init(&code);

    /* ── _start prologue ─────────────────────────────────────────  */
    size_t entry_offset = 0; /* _start is at offset 0 in code buf */

    emit_push_rbp(&code);
    emit_mov_rbp_rsp(&code);

    /* Reserve space for sub rsp, <frame> — we'll patch it after parsing */
    size_t frame_patch = code.size;
    emit_sub_rsp_imm(&code, 0); /* placeholder */

    /* ── parse + codegen ─────────────────────────────────────────  */
    Parser par;
    sh_memset(&par, 0, sizeof(par));
    par.tokens = toks;
    par.code   = &code;

    while (!check(&par, TK_EOF))
        parse_stmt(&par);

    /* implicit exit(0) */
    emit_mov_rdi_imm(&code, 0);
    emit_mov_rax_imm(&code, 60);
    emit_syscall(&code);

    /* ── patch frame size ─────────────────────────────────────────  */
    int frame = par.stack_size;
    if (frame % 16) frame += 16 - (frame % 16);
    if (!frame) frame = 16;
    /* patch the sub rsp, imm at frame_patch */
    u8 *fp = code.data + frame_patch + 3; /* skip 48 81 EC */
    fp[0]=(u8)frame; fp[1]=(u8)(frame>>8);
    fp[2]=(u8)(frame>>16); fp[3]=(u8)(frame>>24);

    /* ── emit print_int runtime (after user code) ─────────────────  */
    size_t nl_off, itoa_off;
    /*
     * The rip-relative addresses inside print_int are relative to
     * where the code will be loaded: LOAD_ADDR + HEADERS_SZ.
     * We pass the code buffer offsets; the emitter patches using those
     * buffer offsets (same as final file offsets relative to code start).
     */
    par.print_int_offset = emit_print_int_fn(&code, &nl_off, &itoa_off);

    /* ── resolve forward jumps and call patches ───────────────────  */
    resolve_patches(&par);
    resolve_print_calls(&par);

    sh_puts("[shc] code size: "); sh_print_long((long)code.size);
    sh_puts(" bytes\n");

    /* ── write ELF ───────────────────────────────────────────────  */
    sh_puts("[shc] writing -> "); sh_puts(out_path); sh_putc('\n');
    write_elf(out_path, code.data, code.size, entry_offset);

    sh_puts("[shc] done! run with: elf ");
    /* uppercase the filename for ShadowOS 8.3 convention hint */
    sh_puts(out_path);
    sh_putc('\n');

    return 0;
}
