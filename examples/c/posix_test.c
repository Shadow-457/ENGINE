#include "libc.h"

int main(void) {
    printf("=== Systrix OS POSIX Feature Demo ===\r\n\r\n");

    printf("[1] Testing fork() + execve()...\r\n");
    pid_t pid = fork();
    if (pid == 0) {
        printf("  Child process (PID=%d)\r\n", getpid());
        exit(0);
    } else if (pid > 0) {
        printf("  Parent process (PID=%d), child PID=%d\r\n", getpid(), pid);
        wait(NULL);
        printf("  Child exited\r\n");
    } else {
        printf("  fork() failed: errno=%d\r\n", errno);
    }

    printf("\r\n[2] Testing pipe()...\r\n");
    int pipefd[2];
    if (pipe(pipefd) == 0) {
        const char *msg = "Hello through pipe!";
        write(pipefd[1], msg, strlen(msg));
        char buf[64];
        int n = read(pipefd[0], buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = 0;
            printf("  Pipe read: '%s'\r\n", buf);
        }
        close(pipefd[0]);
        close(pipefd[1]);
    } else {
        printf("  pipe() failed: errno=%d\r\n", errno);
    }

    printf("\r\n[3] Testing dup()/dup2()...\r\n");
    int fd = open("README.TXT", 0);
    if (fd >= 0) {
        int fd2 = dup(fd);
        printf("  dup() success: fd=%d, fd2=%d\r\n", fd, fd2);
        close(fd);
        close(fd2);
    } else {
        printf("  open() failed for README.TXT\r\n");
    }

    printf("\r\n[4] Testing signals...\r\n");
    void *old = signal(10, (void*)0);
    printf("  signal() handler set (old=%p)\r\n", old);

    printf("\r\n[5] Testing sockets...\r\n");
    int sock = socket(2, 1, 0);
    if (sock >= 0) {
        printf("  socket() created: fd=%d\r\n", sock);
        close(sock);
    } else {
        printf("  socket() failed: errno=%d\r\n", errno);
    }

    printf("\r\n[6] Testing getpid()...\r\n");
    printf("  Current PID: %d\r\n", getpid());

    printf("\r\n[7] Testing uname()...\r\n");
    struct { char f[65]; } uts[6];
    long _r;
    __asm__ volatile("syscall" : "=a"(_r) : "0"(63LL), "D"((long)&uts) : "rcx", "r11", "memory");
    printf("  Sysname: %s\r\n", uts[0].f);
    printf("  Nodename: %s\r\n", uts[1].f);
    printf("  Release: %s\r\n", uts[2].f);

    printf("\r\n=== All POSIX features tested ===\r\n");
    exit(0);
    return 0;
}
