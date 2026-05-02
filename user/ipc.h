/* ================================================================
 *  Systrix OS — user/ipc.h
 *  Userland helper for microkernel IPC.
 *
 *  Usage in a server:
 *      ipc_register("myserver");
 *      IpcMsg msg;
 *      while (1) {
 *          ipc_recv(&msg);
 *          switch (msg.type) { ... }
 *      }
 *
 *  Usage in a client:
 *      long srv = ipc_lookup("myserver");
 *      IpcMsg req = { .type = MY_REQUEST, .data[0] = 42 };
 *      ipc_send(srv, &req);
 * ================================================================ */

#pragma once

/* ── syscall numbers (must match kernel/isr.S table) ─────────── */
#define SYS_IPC_SEND    329
#define SYS_IPC_RECV    330
#define SYS_IPC_REG     331
#define SYS_IPC_LOOKUP  332

/* ── message structure (64 bytes) ────────────────────────────── */
typedef struct {
    unsigned long type;
    unsigned long from;
    unsigned long data[6];
} IpcMsg;

/* ── well-known message types ────────────────────────────────── */

/* filesystem server ("fs") */
#define MSG_FS_OPEN      0x0100
#define MSG_FS_READ      0x0101
#define MSG_FS_WRITE     0x0102
#define MSG_FS_CLOSE     0x0103
#define MSG_FS_REPLY     0x01FF

/* GUI server ("gui") */
#define MSG_GUI_DRAW_RECT   0x0200
#define MSG_GUI_DRAW_TEXT   0x0201
#define MSG_GUI_FLIP        0x0202
#define MSG_GUI_EVENT       0x0203   /* kernel → client: key/mouse event */
#define MSG_GUI_REPLY       0x02FF

/* network server ("net") */
#define MSG_NET_CONNECT  0x0300
#define MSG_NET_SEND     0x0301
#define MSG_NET_RECV     0x0302
#define MSG_NET_CLOSE    0x0303
#define MSG_NET_REPLY    0x03FF

/* generic reply */
#define MSG_OK           0x0000
#define MSG_ERR          0xFFFF

/* ── inline syscall wrappers ─────────────────────────────────── */

static inline long ipc_send(long dest_pid, IpcMsg *msg) {
    long ret;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"((long)SYS_IPC_SEND), "D"(dest_pid), "S"(msg)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline long ipc_recv(IpcMsg *out) {
    long ret;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"((long)SYS_IPC_RECV), "D"(out)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline long ipc_register(const char *name) {
    long ret;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"((long)SYS_IPC_REG), "D"(name)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline long ipc_lookup(const char *name) {
    long ret;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"((long)SYS_IPC_LOOKUP), "D"(name)
        : "rcx", "r11", "memory"
    );
    return ret;
}
