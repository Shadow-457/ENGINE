/* ================================================================
 *  ShadowOS — myprog.c  (safe, minimal version)
 *
 *  Build:
 *    as  --64 -o user/crt0.o user/crt0.S
 *    gcc -m64 -O2 -ffreestanding -fno-stack-protector -mno-red-zone \
 *        -nostdlib -nostdinc -c -o user/myprog.o user/myprog.c
 *    ld  -m elf_x86_64 -static -nostdlib \
 *        -Ttext=0x400000 -o MYPROG user/crt0.o user/myprog.o
 *    make addprog PROG=MYPROG
 *    make run
 *    # then inside ShadowOS: elf MYPROG
 * ================================================================ */

typedef unsigned long size_t;
typedef long          ssize_t;

static ssize_t write(int fd, const void *buf, size_t count) {
    ssize_t ret;
    __asm__ volatile("syscall"
        : "=a"(ret)
        : "0"(1L), "D"((long)fd), "S"(buf), "d"(count)
        : "rcx", "r11", "memory");
    return ret;
}

static void exit(int code) {
    __asm__ volatile("syscall"
        :: "a"(60L), "D"((long)code) : "rcx", "r11");
    __builtin_unreachable();
}

static void print(const char *s) {
    const char *p = s;
    while (*p) p++;
    write(1, s, (size_t)(p - s));
}

/* Print a digit 0-9 */
static void print_digit(int n) {
    char c = '0' + (char)n;
    write(1, &c, 1);
}

int main(void) {
    print("=== Hello from ShadowOS! ===\r\n");
    print("A C program running on a custom OS.\r\n\r\n");

    print("Counting 1 to 10:\r\n");
    for (int i = 1; i <= 10; i++) {
        print("  ");
        if (i >= 10) print_digit(i / 10);
        print_digit(i % 10);
        print("\r\n");
    }

    print("\r\nMultiplication (2x table):\r\n");
    for (int i = 1; i <= 10; i++) {
        print("  2 x ");
        print_digit(i);
        print(" = ");
        int result = 2 * i;
        if (result >= 10) print_digit(result / 10);
        print_digit(result % 10);
        print("\r\n");
    }

    print("\r\nDone!\r\n");
    exit(0);
    return 0;
}
