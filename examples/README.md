# examples/

Example C programs that run on SystrixOS.

| File | What it demonstrates |
|------|---------------------|
| `hello.c` | Minimal "Hello, SystrixOS!" using `puts()` |
| `myprogram.c` | Syscall wrappers, string output, basic I/O |
| `posix_test.c` | POSIX compatibility: `fork`, `exec`, `wait`, file I/O |
| `echo_server.c` | IPC named server — registers itself and waits for messages |
| `echo_client.c` | IPC client — connects to the echo server and sends a ping |

## Building

```bash
make hello         # → HELLO_C
make myprog        # → MYPROGRAM
make posix_test    # → POSIX_TEST
make ipc_demo      # → ECHO_SRV + ECHO_CLI (both embedded in disk)
```

Or manually (see [`../docs/userspace.md`](../docs/userspace.md) for full steps):

```bash
gcc -m64 -O2 -ffreestanding -fno-stack-protector -mno-red-zone \
    -nostdlib -nostdinc -Iuser \
    -c -o myfile.o examples/c/myfile.c
```
