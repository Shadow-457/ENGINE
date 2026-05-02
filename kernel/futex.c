/* ================================================================
 *  Systrix OS — kernel/futex.c
 *  Fast userspace mutex primitives (syscall #202)
 *
 *  Implements the minimum futex ops needed for pthreads:
 *    FUTEX_WAIT  — sleep until *addr != val (or timeout)
 *    FUTEX_WAKE  — wake up to N waiters on addr
 *
 *  Each waiter entry stores the virtual address and the PCB of
 *  the sleeping process.  When FUTEX_WAKE fires, it marks those
 *  PCBs READY so the scheduler picks them up next tick.
 *
 *  Max simultaneous waiters: FUTEX_MAX_WAITERS (64).
 *  This is enough for browser-engine thread pools in early porting.
 * ================================================================ */

#include "../include/kernel.h"

#define FUTEX_WAIT  0
#define FUTEX_WAKE  1

/* one slot per sleeping thread */
#define FUTEX_MAX_WAITERS 64

typedef struct {
    u64  addr;   /* virtual address of the futex word (0 = free slot) */
    PCB *pcb;    /* sleeping process control block                     */
} FutexWaiter;

static FutexWaiter futex_table[FUTEX_MAX_WAITERS];

/* ── helpers ─────────────────────────────────────────────────── */

static FutexWaiter *futex_alloc_slot(void) {
    for (int i = 0; i < FUTEX_MAX_WAITERS; i++)
        if (futex_table[i].addr == 0)
            return &futex_table[i];
    return NULL;
}

/* ── sys_futex ───────────────────────────────────────────────── */
/*
 * sys_futex(addr, op, val, timeout, addr2, val3)
 *
 * FUTEX_WAIT:
 *   If *addr == val → put caller to sleep until woken or timeout.
 *   Returns 0 on wake, -ETIMEDOUT on timeout, -EAGAIN if *addr!=val.
 *
 * FUTEX_WAKE:
 *   Wake up to val waiters sleeping on addr.
 *   Returns the number of threads actually woken.
 */
i64 sys_futex(u64 *addr, u64 op, u64 val,
              u64 timeout_ms, u64 *addr2, u64 val3) {
    (void)addr2; (void)val3;

    if (!addr) return (i64)EINVAL;

    /* ── FUTEX_WAIT ─────────────────────────────────────────── */
    if (op == FUTEX_WAIT) {
        /* Atomically check: if word changed already, bail out */
        if (*addr != val) return (i64)EAGAIN;

        PCB *pcb = process_current_pcb();
        if (!pcb) return (i64)EINVAL;

        FutexWaiter *slot = futex_alloc_slot();
        if (!slot) return (i64)ENOMEM;

        slot->addr = (u64)addr;
        slot->pcb  = pcb;

        /* Calculate wake deadline (0 = wait forever) */
        u64 deadline = timeout_ms ? pit_ticks + timeout_ms : 0;

        /* Put this process to sleep — scheduler skips PSTATE_BLOCKED */
        pcb->state = PSTATE_BLOCKED;

        /* Spin-yield until woken or timed out.
         * STI so timer ISR keeps running (needed for pit_ticks and
         * scheduler to function). The scheduler will skip BLOCKED pcbs
         * and run other threads.  We busy-check here but yield each
         * iteration so other threads get CPU time. */
        __asm__ volatile("sti");
        while (pcb->state == PSTATE_BLOCKED) {
            if (deadline && pit_ticks >= deadline) {
                /* Timed out — reclaim our slot and return */
                slot->addr = 0;
                slot->pcb  = NULL;
                pcb->state = PSTATE_RUNNING;
                __asm__ volatile("cli");
                return (i64)ETIMEDOUT;
            }
            /* Yield to let other processes run and potentially wake us */
            __asm__ volatile("pause");
        }
        __asm__ volatile("cli");
        return 0;   /* woken successfully */
    }

    /* ── FUTEX_WAKE ─────────────────────────────────────────── */
    if (op == FUTEX_WAKE) {
        u64 woken = 0;
        for (int i = 0; i < FUTEX_MAX_WAITERS && woken < val; i++) {
            if (futex_table[i].addr == (u64)addr && futex_table[i].pcb) {
                /* Mark the sleeper ready and free the slot */
                futex_table[i].pcb->state = PSTATE_READY;
                futex_table[i].addr = 0;
                futex_table[i].pcb  = NULL;
                woken++;
            }
        }
        return (i64)woken;
    }

    return (i64)EINVAL;
}
