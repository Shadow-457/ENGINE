#include "../include/kernel.h"

#define MAX_SIGNAL 32

signal_state_t proc_signals[PROC_MAX];

void signal_init(void) {
    memset(proc_signals, 0, sizeof(proc_signals));
}

i64 sys_signal(int signum, void *handler) {
    if (signum < 1 || signum >= MAX_SIGNAL) return (i64)SIG_ERR;
    if (signum == SIGKILL || signum == SIGSTOP) return (i64)SIG_ERR;
    PCB *pcb = process_current_pcb();
    if (!pcb) return (i64)SIG_ERR;
    int idx = (int)(pcb->pid - 1);
    if (idx < 0 || idx >= PROC_MAX) return (i64)SIG_ERR;
    void *old = proc_signals[idx].actions[signum].handler;
    proc_signals[idx].actions[signum].handler = handler;
    return (i64)old;
}

i64 sys_sigaction(int signum, const sigaction_t *act, sigaction_t *oldact) {
    if (signum < 1 || signum >= MAX_SIGNAL) return (i64)EINVAL;
    if (signum == SIGKILL || signum == SIGSTOP) return (i64)EINVAL;
    PCB *pcb = process_current_pcb();
    if (!pcb) return (i64)EINVAL;
    int idx = (int)(pcb->pid - 1);
    if (idx < 0 || idx >= PROC_MAX) return (i64)EINVAL;
    if (oldact) *oldact = proc_signals[idx].actions[signum];
    if (act) proc_signals[idx].actions[signum] = *act;
    return 0;
}

i64 sys_sigprocmask(int how, const u64 *set, u64 *oldset) {
    PCB *pcb = process_current_pcb();
    if (!pcb) return (i64)EINVAL;
    int idx = (int)(pcb->pid - 1);
    if (idx < 0 || idx >= PROC_MAX) return (i64)EINVAL;
    if (oldset) *oldset = proc_signals[idx].blocked;
    if (set) {
        switch (how) {
            case 0: proc_signals[idx].blocked = *set; break;
            case 1: proc_signals[idx].blocked |= *set; break;
            case 2: proc_signals[idx].blocked &= ~*set; break;
            default: return (i64)EINVAL;
        }
    }
    return 0;
}

i64 sys_kill(u64 pid, int signum) {
    if (signum < 0 || signum >= MAX_SIGNAL) return (i64)EINVAL;
    if (pid == 0) return (i64)EPERM;
    u8 *p = (u8*)PROC_TABLE;
    for (int i = 0; i < PROC_MAX; i++, p += PROC_PCB_SIZE) {
        PCB *t = (PCB*)p;
        if (t->state == PSTATE_EMPTY || t->state == PSTATE_DEAD) continue;
        if (t->pid == pid || pid == (u64)-1) {
            int idx = i;
            if (signum == 0) return 0;
            if (signum == SIGKILL) {
                t->state = PSTATE_DEAD;
                return 0;
            }
            proc_signals[idx].pending |= (1ULL << signum);
            return 0;
        }
    }
    return (i64)ESRCH;
}

void signal_deliver(PCB *pcb) {
    if (!pcb) return;
    int idx = (int)(pcb->pid - 1);
    if (idx < 0 || idx >= PROC_MAX) return;
    signal_state_t *ss = &proc_signals[idx];
    u64 pending = ss->pending & ~ss->blocked;
    if (!pending) return;
    for (int sig = 1; sig < MAX_SIGNAL; sig++) {
        if (!(pending & (1ULL << sig))) continue;
        ss->pending &= ~(1ULL << sig);
        void *handler = ss->actions[sig].handler;
        if (handler == SIG_DFL) {
            if (sig == SIGCHLD || sig == SIGCONT) continue;
            pcb->state = PSTATE_DEAD;
            return;
        }
        if (handler == SIG_IGN) continue;
        u64 ursp = pcb->ursp;
        ursp -= 8;
        *(u64*)(usize)ursp = sig;
        ursp -= 8;
        *(u64*)(usize)ursp = pcb->entry;
        pcb->ursp = ursp;
        pcb->entry = (u64)handler;
        return;
    }
}

u64 signal_pending(PCB *pcb) {
    if (!pcb) return 0;
    int idx = (int)(pcb->pid - 1);
    if (idx < 0 || idx >= PROC_MAX) return 0;
    return proc_signals[idx].pending & ~proc_signals[idx].blocked;
}
