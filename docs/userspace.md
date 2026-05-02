# User Space

Everything needed to write and run programs on SystrixOS lives in `user/`.

---

## Runtime Layout

```
user/
├── crt0.S          # C runtime zero — _start, sets up argc/argv, calls main()
├── libc.c          # User-space C library implementation
├── libc.h          # User-space C library header
├── libm.h          # Math helpers (header-only)
├── malloc.c        # User-space heap allocator (sbrk-based)
├── pthread.h       # POSIX thread stubs (for compatibility)
├── tls.h           # Thread-local storage helpers
├── ipc.h           # IPC channel API
├── gfx.h           # Graphics syscall wrappers (framebuffer access)
├── sound.h         # Sound syscall wrappers
├── i_sys.c         # Low-level syscall wrappers
├── i_sound.c       # Sound interface
├── i_video.c       # Video/framebuffer interface
└── shc.c           # Shadow language compiler
```

---

## libc (`user/libc.c` + `user/libc.h`)

A freestanding C library that runs without the host OS. It talks to the kernel via the `syscall` instruction.

Provided functions (grouped by header section):

| Section | Functions |
|---------|-----------|
| Output | `write()`, `puts()`, `putchar()`, `printf()` (basic) |
| Memory | `memcpy()`, `memset()`, `memmove()`, `memcmp()` |
| String | `strlen()`, `strcmp()`, `strncmp()`, `strcpy()`, `strcat()`, `strstr()`, `strtok()` |
| Char | `isdigit()`, `isalpha()`, `isspace()`, `toupper()`, `tolower()` |
| Conversion | `atoi()`, `itoa()`, `strtol()` |
| Heap | `malloc()`, `free()`, `realloc()` (via `malloc.c`) |
| Process | `exit()`, `getpid()`, `fork()`, `execve()`, `wait()` |
| File I/O | `open()`, `close()`, `read()`, `write()`, `lseek()`, `stat()` |
| Network | `socket()`, `connect()`, `send()`, `recv()`, `bind()`, `listen()`, `accept()` |

---

## crt0 (`user/crt0.S`)

The C runtime entry point. The kernel's ELF loader jumps here after loading the binary:

```asm
_start:
    xor rbp, rbp            ; mark end of call chain
    mov rdi, [rsp]          ; argc
    lea rsi, [rsp+8]        ; argv
    call main
    mov rdi, rax
    mov rax, 60             ; SYS_exit
    syscall
```

Always link `crt0.o` first in the link command.

---

## malloc (`user/malloc.c`)

User-space allocator using `brk`/`sbrk` syscalls. Uses a free-list with coalescing. Compatible with the `malloc`/`free`/`realloc` interface in `libc.h`.

---

## Graphics API (`user/gfx.h`)

Thin wrappers over display server syscalls (syscall numbers 300–308):

```c
void gfx_fill_rect(int x, int y, int w, int h, uint32_t color);
void gfx_draw_string(int x, int y, const char *s, uint32_t fg);
void gfx_flip(void);          // push backbuffer to screen
int  gfx_get_width(void);
int  gfx_get_height(void);
```

---

## IPC API (`user/ipc.h`)

```c
int ipc_register(const char *name);     // register this process as a named server
int ipc_connect(const char *name);      // get handle to a server by name
int ipc_send(int handle, void *msg, int len);
int ipc_recv(int handle, void *buf, int max);
```

See `examples/c/echo_server.c` and `examples/c/echo_client.c` for a working demo.

---

## Syscall Numbers

The kernel's `syscall.c` defines a Linux-compatible syscall table. Common ones:

| Number | Name |
|--------|------|
| 0 | `read` |
| 1 | `write` |
| 2 | `open` |
| 3 | `close` |
| 9 | `mmap` |
| 11 | `munmap` |
| 12 | `brk` |
| 39 | `getpid` |
| 57 | `fork` |
| 59 | `execve` |
| 60 | `exit` |
| 61 | `wait4` |
| 62 | `kill` |
| 300–308 | display server (SystrixOS-specific) |

---

## Building a Program

Minimal example:

```c
// examples/c/hello.c
#include "libc.h"

int main(int argc, char **argv) {
    puts("Hello from SystrixOS!\n");
    return 0;
}
```

Build steps:

```bash
as  --64 -o user/crt0.o user/crt0.S
gcc -m64 -O2 -ffreestanding -fno-stack-protector -mno-red-zone \
    -nostdlib -nostdinc -Iuser \
    -c -o user/hello.o examples/c/hello.c
ld  -m elf_x86_64 -static -nostdlib -Ttext=0x400000 \
    -o HELLO_C user/crt0.o user/libc.o user/hello.o
make addprog PROG=HELLO_C
make run
# inside the shell:
elf HELLO_C
```

Or just use the Makefile shortcut:

```bash
make hello      # builds examples/c/hello.c
```
