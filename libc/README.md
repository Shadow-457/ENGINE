# libc/

Unified C library that compiles for both the kernel and user space.

| File | What it does |
|------|-------------|
| `systrix_libc.c` | Implementation: memory, string, character, integer, I/O, assert/panic |
| `systrix_libc.h` | Header declaring all functions |

Compile for the kernel with `-DSYSTRIX_KERNEL` — this gates `kputs`-based output and kernel panic behaviour. Compile without the flag for user space (uses `write` syscall for output).

See [`../docs/slibc_reference.md`](../docs/slibc_reference.md) for the full function reference.
