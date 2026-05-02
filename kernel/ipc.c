/* ================================================================
 *  Systrix OS — kernel/ipc.c
 *  Inter-Process Communication (IPC) — microkernel message passing
 *
 *  This is the ONLY thing a microkernel really needs beyond memory
 *  and scheduling.  Everything else (GUI, drivers, filesystem) can
 *  live in userland servers that talk through these four syscalls:
 *
 *    sys_ipc_send     (329) — send a message to a pid, block until delivered
 *    sys_ipc_recv     (330) — block until a message arrives, return it
 *    sys_ipc_register (331) — register this process as a named server
 *    sys_ipc_lookup   (332) — find the pid of a named server
 *
 *  Message format: fixed 64-byte IpcMsg.
 *    word[0]  = type  (caller-defined, e.g. MSG_FS_READ, MSG_GUI_DRAW)
 *    word[1]  = from  (sender pid, filled by kernel)
 *    word[2..7] = payload (six 64-bit words = 48 bytes of data)
 *
 *  Each process has a small inbox queue (IPC_QUEUE_DEPTH messages).
 *  send blocks if inbox full; recv blocks if inbox empty.
 *  No copying through kernel heap — message is copied directly from
 *  sender's stack/buffer into receiver's inbox slot.
 * ================================================================ */

#include "../include/kernel.h"

/* ── constants ─────────────────────────────────────────────────── */
#define IPC_QUEUE_DEPTH  16        /* messages buffered per process */
#define IPC_NAME_MAX     16        /* max length of server name     */
#define IPC_SERVER_MAX   32        /* max named servers             */
#define IPC_PAYLOAD_WORDS 6        /* 48 bytes of payload           */

/* ── message structure (64 bytes, cache-line friendly) ─────────── */
typedef struct {
    u64 type;                          /* message type (caller-defined) */
    u64 from;                          /* sender pid (filled by kernel) */
    u64 data[IPC_PAYLOAD_WORDS];       /* payload: 6 × 8 = 48 bytes    */
} IpcMsg;                              /* total: 64 bytes               */

/* ── per-process inbox ─────────────────────────────────────────── */
typedef struct {
    IpcMsg  msgs[IPC_QUEUE_DEPTH];
    int     head;      /* next slot to read  */
    int     tail;      /* next slot to write */
    int     count;     /* messages in queue  */
    int     waiting;   /* 1 if process blocked on recv */
} IpcQueue;

/* ── named server registry ─────────────────────────────────────── */
typedef struct {
    char name[IPC_NAME_MAX];
    u64  pid;
    int  active;
} IpcServer;

/* ── global state ──────────────────────────────────────────────── */
static IpcQueue  g_queues[PROC_MAX];
static IpcServer g_servers[IPC_SERVER_MAX];

/* ── helpers ───────────────────────────────────────────────────── */

/* find PCB by pid — walk the process table */
static PCB *ipc_find_pcb(u64 pid) {
    PCB *t = (PCB *)PROC_TABLE;
    for (int i = 0; i < PROC_MAX; i++, t++) {
        if (t->state != 0 && t->pid == pid)
            return t;
    }
    return 0;
}

/* ── sys_ipc_send (329) ────────────────────────────────────────── */
/*  Send msg to process with given pid.
 *  Blocks (busy-wait yield loop) if receiver inbox is full.
 *  Returns 0 on success, -1 if pid not found.                      */
i64 sys_ipc_send(u64 dest_pid, void *msg_raw) {
    IpcMsg *msg = (IpcMsg *)msg_raw;
    if (!msg) return -1;

    /* verify destination exists */
    PCB *dest = ipc_find_pcb(dest_pid);
    if (!dest) return -1;

    IpcQueue *q = &g_queues[dest_pid % PROC_MAX];

    /* spin-yield if inbox full */
    int spins = 0;
    while (q->count >= IPC_QUEUE_DEPTH) {
        sys_yield();
        if (++spins > 1000) return -1;   /* timeout — receiver dead? */
    }

    /* copy message into inbox, stamp sender pid */
    IpcMsg *slot = &q->msgs[q->tail];
    *slot = *msg;
    slot->from = current_pid;

    q->tail = (q->tail + 1) % IPC_QUEUE_DEPTH;
    q->count++;

    /* wake receiver if it was blocked */
    if (q->waiting) {
        q->waiting = 0;
        dest->state = 1;   /* RUNNING */
    }

    return 0;
}

/* ── sys_ipc_recv (330) ────────────────────────────────────────── */
/*  Block until a message arrives.  Copies it into *out_msg.
 *  Returns sender pid on success, -1 on error.                     */
i64 sys_ipc_recv(void *out_raw) {
    IpcMsg *out_msg = (IpcMsg *)out_raw;
    if (!out_msg) return -1;

    u64 my_pid = current_pid;
    IpcQueue *q = &g_queues[my_pid % PROC_MAX];

    /* block (yield loop) until something arrives */
    while (q->count == 0) {
        q->waiting = 1;
        sys_yield();
    }

    /* dequeue oldest message */
    IpcMsg *slot = &q->msgs[q->head];
    *out_msg = *slot;
    q->head = (q->head + 1) % IPC_QUEUE_DEPTH;
    q->count--;

    return (i64)out_msg->from;
}

/* ── sys_ipc_register (331) ────────────────────────────────────── */
/*  Register current process as a named server (e.g. "gui", "fs").
 *  Returns 0 on success, -1 if table full or name already taken.   */
i64 sys_ipc_register(const char *name) {
    if (!name) return -1;

    /* check for duplicate */
    for (int i = 0; i < IPC_SERVER_MAX; i++) {
        if (g_servers[i].active) {
            int same = 1;
            for (int j = 0; j < IPC_NAME_MAX - 1; j++) {
                if (g_servers[i].name[j] != name[j]) { same = 0; break; }
                if (!name[j]) break;
            }
            if (same) {
                /* update pid — server restarted */
                g_servers[i].pid = current_pid;
                return 0;
            }
        }
    }

    /* find free slot */
    for (int i = 0; i < IPC_SERVER_MAX; i++) {
        if (!g_servers[i].active) {
            int j;
            for (j = 0; j < IPC_NAME_MAX - 1 && name[j]; j++)
                g_servers[i].name[j] = name[j];
            g_servers[i].name[j] = 0;
            g_servers[i].pid    = current_pid;
            g_servers[i].active = 1;
            return 0;
        }
    }

    return -1;   /* table full */
}

/* ── sys_ipc_lookup (332) ──────────────────────────────────────── */
/*  Find pid of a registered server by name.
 *  Returns pid on success, -1 if not found.                        */
i64 sys_ipc_lookup(const char *name) {
    if (!name) return -1;

    for (int i = 0; i < IPC_SERVER_MAX; i++) {
        if (!g_servers[i].active) continue;
        int match = 1;
        for (int j = 0; j < IPC_NAME_MAX; j++) {
            if (g_servers[i].name[j] != name[j]) { match = 0; break; }
            if (!name[j]) break;
        }
        if (match) return (i64)g_servers[i].pid;
    }

    return -1;
}

/* ── init (called from kernel_main) ───────────────────────────── */
void ipc_init(void) {
    for (int i = 0; i < PROC_MAX; i++) {
        g_queues[i].head    = 0;
        g_queues[i].tail    = 0;
        g_queues[i].count   = 0;
        g_queues[i].waiting = 0;
    }
    for (int i = 0; i < IPC_SERVER_MAX; i++) {
        g_servers[i].active = 0;
    }
}

/* ── debug helper: dump server registry (callable from shell) ──── */
void ipc_dump_servers(void) {
    kprintf("[IPC] registered servers:\n");
    int found = 0;
    for (int i = 0; i < IPC_SERVER_MAX; i++) {
        if (g_servers[i].active) {
            kprintf("  ");
            kprintf("%s", g_servers[i].name);
            kprintf("\n");
            found++;
        }
    }
    if (!found) kprintf("  (none)\n");
}
