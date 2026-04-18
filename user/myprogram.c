/* ================================================================
 *  ShadowOS — myprogram.c
 *  A simple C program to run natively on Shadow OS.
 *
 *  Build & run steps:
 *    1. Copy this file into the shadowos_fixed/user/ folder
 *    2. as  --64 -o user/crt0.o user/crt0.S
 *       gcc -m64 -O2 -ffreestanding -fno-stack-protector -mno-red-zone \
 *           -nostdlib -nostdinc -c -o user/myprogram.o user/myprogram.c
 *       ld  -m elf_x86_64 -static -nostdlib \
 *           -Ttext=0x400000 -o MYPROGRAM user/crt0.o user/myprogram.o
 *    3. make addprog PROG=MYPROGRAM
 *    4. make run
 *    5. Inside ShadowOS shell: elf MYPROGRAM
 * ================================================================ */

typedef unsigned long size_t;
typedef long          ssize_t;

/* ── Syscall wrappers ─────────────────────────────────────────── */

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

/* ── Helper functions ─────────────────────────────────────────── */

static size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

static void print(const char *s) {
    write(1, s, strlen(s));
}

/* Print a single integer (positive numbers only) */
static void print_int(long n) {
    if (n == 0) { write(1, "0", 1); return; }
    char buf[20];
    int i = 0;
    while (n > 0) { buf[i++] = '0' + (n % 10); n /= 10; }
    /* reverse */
    for (int a = 0, b = i - 1; a < b; a++, b--) {
        char tmp = buf[a]; buf[a] = buf[b]; buf[b] = tmp;
    }
    write(1, buf, i);
}

/* ── Main program ─────────────────────────────────────────────── */

int main(void) {
    print("===========================\r\n");
    print("  Hello from ShadowOS!\r\n");
    print("===========================\r\n\r\n");

    /* Counting loop */
    print("Counting from 1 to 10:\r\n");
    for (int i = 1; i <= 10; i++) {
        print("  i = ");
        print_int(i);
        print("\r\n");
    }

    /* Simple addition */
    print("\r\nSimple math:\r\n");
    long a = 42, b = 58;
    print("  42 + 58 = ");
    print_int(a + b);
    print("\r\n");

    /* Multiplication table */
    print("\r\nMultiplication table (1-5):\r\n");
    for (int i = 1; i <= 5; i++) {
        for (int j = 1; j <= 5; j++) {
            print_int(i * j);
            write(1, "\t", 1);
        }
        print("\r\n");
    }

    print("\r\nDone! Exiting...\r\n");
    exit(0);
    return 0;
}
