/* ================================================================
 *  SHADOW OS — user/hello.c
 *  Example C program that runs natively on Shadow OS.
 *
 *  Compile (statically linked, no libc):
 *    as  --64 -o crt0.o crt0.S
 *    gcc -O2 -ffreestanding -fno-stack-protector -nostdlib \
 *        -nostdinc -m64 -c -o hello.o hello.c
 *    ld  -m elf_x86_64 -static -nostdlib \
 *        -Ttext=0x400000 -o HELLO_C crt0.o hello.o
 *
 *  Then add to disk:  make addprog PROG=HELLO_C
 *  Run from shell:    elf HELLO_C
 * ================================================================ */

/* Syscall wrappers (no libc — hand-roll what we need) */
typedef unsigned long size_t;
typedef long          ssize_t;

static ssize_t write(int fd, const void *buf, size_t count) {
    ssize_t ret;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "0"(1L), "D"((long)fd), "S"(buf), "d"(count)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static void exit(int code) {
    __asm__ volatile(
        "syscall"
        :: "a"(60L), "D"((long)code)
        : "rcx", "r11"
    );
    __builtin_unreachable();
}

static size_t strlen(const char *s) {
    size_t n = 0; while (s[n]) n++; return n;
}

static void print(const char *s) {
    write(1, s, strlen(s));
}

/* ── Main program ─────────────────────────────────────────────── */
int main(void) {
    print("Hello from C on Shadow OS!\r\n");
    print("This program was compiled with GCC and runs natively.\r\n");

    /* Simple counting demo */
    print("Counting: ");
    for (int i = 1; i <= 5; i++) {
        char c = '0' + (char)i;
        write(1, &c, 1);
        write(1, " ", 1);
    }
    print("\r\nDone!\r\n");

    return 0;
}
