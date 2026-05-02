# user/

User-space runtime — everything needed to write programs that run on SystrixOS.

See [`../docs/userspace.md`](../docs/userspace.md) for full documentation.

| File | What it does |
|------|-------------|
| `crt0.S` | C runtime entry point (`_start` → `main()` → `exit()`) |
| `libc.c` | User-space C library (string, memory, I/O, process, socket syscalls) |
| `libc.h` | Header for `libc.c` |
| `malloc.c` | User-space heap allocator (`malloc`/`free`/`realloc` via `brk`) |
| `libm.h` | Math helpers (header-only) |
| `pthread.h` | POSIX thread stubs |
| `tls.h` | Thread-local storage |
| `ipc.h` | IPC channel API |
| `gfx.h` | Graphics syscall wrappers (not usable — GUI not working yet) |
| `sound.h` | Sound syscall wrappers |
| `i_sys.c` | Low-level syscall wrappers |
| `i_sound.c` | Sound interface |
| `i_video.c` | Video/framebuffer interface (not usable — GUI not working yet) |
