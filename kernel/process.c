/* ================================================================
 *  Systrix OS — kernel/process.c  (PATCHED)
 *  Fix Bug 2: CR3 must be switched INSIDE the iretq asm sequence,
 *  not before it.  If vmm_switch() runs while we are still using
 *  the kernel stack, and the new CR3 does not map that stack page,
 *  the very next memory access causes a triple fault.
 *
 *  The fix: pass cr3 as a register input to the inline asm and do
 *  `mov cr3, <reg>` as the last instruction before `iretq`.  That
 *  way the stack we just built the iretq frame on is still mapped
 *  by the OLD (kernel) CR3 until iretq consumes it.
 * ================================================================ */
#include "../include/kernel.h"

void process_init(void) {
    memset((void*)PROC_TABLE, 0, PROC_PCB_SIZE * PROC_MAX);
}

PCB *process_current_pcb(void) {
    u8 *p = (u8*)PROC_TABLE;
    for (int i = 0; i < PROC_MAX; i++, p += PROC_PCB_SIZE) {
        PCB *t = (PCB*)p;
        if (t->state == PSTATE_RUNNING) return t;
    }
    return NULL;
}

i64 process_create(u64 entry, const char *name) {
    u8 *p = (u8*)PROC_TABLE;
    int slot_idx = -1;
    PCB *slot = NULL;
    for (int i = 0; i < PROC_MAX; i++, p += PROC_PCB_SIZE) {
        PCB *t = (PCB*)p;
        if (t->state == PSTATE_EMPTY) { slot = t; slot_idx = i; break; }
    }
    if (!slot) return -1;

    u64 cr3 = vmm_create_space();
    if (!cr3) return -1;
    vmm_map_kernel(cr3);

    void *kstack_base = heap_malloc(PROC_KSTACK_SZ);
    if (!kstack_base) { vmm_destroy(cr3); return -1; }

    memset(slot, 0, PROC_PCB_SIZE);
    slot->pid    = (u64)(slot_idx + 1);
    slot->entry  = entry;
    slot->cr3    = cr3;
    slot->kbase  = (u64)kstack_base;
    /* PCB_KSTACK holds the pre-GP-push top — for a freshly created
       process that hasn't been scheduled yet, timer_isr will never
       load this value (state=READY, not RUNNING during scan).
       process_run() uses kstack directly for the iretq frame. */
    slot->kstack = (u64)kstack_base + PROC_KSTACK_SZ;
    slot->ursp   = PROC_STACK_TOP;
    slot->brk      = BRK_BASE;
    /* Allocate VMA table for demand paging / CoW tracking */
    VMA *vmas = vmm_vma_alloc();
    slot->vma_table = vmas ? (u64)(usize)vmas : 0;
    /* Stay EMPTY until process_run() is called.  The timer ISR only
     * picks PSTATE_READY processes; a fresh process with a zeroed
     * kernel stack must not be scheduled before process_run() sets
     * up the iretq frame.  process_run() transitions EMPTY → RUNNING
     * directly inside the iretq asm, so no window exists for the ISR
     * to steal the slot. */
    slot->state  = PSTATE_EMPTY;

    if (name) strlcpy(slot->name, name, 16);
    return (i64)slot->pid;
}

void process_run(u64 pid) {
    /* Save kernel RSP so sys_exit can restore it cleanly */
    __asm__ volatile("mov %%rsp, %0" : "=m"(kernel_return_rsp));

    u8 *p = (u8*)PROC_TABLE;
    for (int i = 0; i < PROC_MAX; i++, p += PROC_PCB_SIZE) {
        PCB *t = (PCB*)p;
        if (t->pid != pid) continue;

        kprintf("\r\nRunning: "); kprintf("%s", t->name); kprintf("\r\n");
        /* Transition EMPTY → RUNNING (skipping READY to avoid the
         * timer ISR window).  Any previously-run process that was
         * preempted would be READY; a fresh process starts EMPTY and
         * goes directly here so the ISR never sees it with a zeroed
         * kernel stack. */
        t->state = PSTATE_RUNNING;

        u64 entry  = t->entry;
        u64 ursp   = t->ursp;
        u64 kstack = t->kstack;
        u64 cr3    = t->cr3;

        /* FIX (Bug 2): switch CR3 INSIDE the asm, as the very last
         * step before iretq.  All stack writes happen while the
         * kernel's own CR3 is still active; only iretq itself runs
         * with the new CR3.  This prevents a triple fault when the
         * new address space does not yet map the kernel stack page.
         *
         * FIX (Bug 6): iretq must use ring-3 selectors so the CPU
         * actually performs a privilege-level switch to CPL=3.
         *   SS = 0x23 = GDT[4] (user data64, DPL=3) | RPL=3
         *   CS = 0x2b = GDT[5] (user code64, DPL=3) | RPL=3
         * These match the selectors that SYSRET synthesises from the
         * STAR MSR (STAR[63:48]=0x18 → CS=0x28|3=0x2b, SS=0x20|3=0x23).
         * Previously 0x10/0x18 (ring-0) were pushed, so iretq did a
         * same-privilege return → the process ran in ring 0 with the
         * kernel stack, and after the first SYSRET it landed in ring 3
         * with a stale/invalid RSP → page fault → triple fault. */
        __asm__ volatile(
            "mov  %0, %%rsp       \n\t"  /* switch to process kernel stack   */
            "push $0x23           \n\t"  /* SS  = user data64 selector       */
            "push %1              \n\t"  /* RSP = user stack top              */
            "pushfq               \n\t"  /* RFLAGS                            */
            "pop  %%rax           \n\t"
            "or   $0x200, %%rax   \n\t"  /* set IF=1                          */
            "push %%rax           \n\t"
            "push $0x2b           \n\t"  /* CS  = user code64 selector        */
            "push %2              \n\t"  /* RIP = entry point                 */
            /* switch CR3 here, after the iretq frame is fully built */
            "mov  %3, %%cr3       \n\t"
            "iretq                \n\t"
            :: "r"(kstack), "r"(ursp), "r"(entry), "r"(cr3)
            : "rax", "memory"
        );
        __builtin_unreachable();
    }
}

/* Free all resources owned by a PCB slot (called from sys_exit / scheduler) */
void process_destroy(PCB *t) {
    if (!t) return;
    if (t->vma_table) {
        vmm_vma_free((VMA*)(usize)t->vma_table);
        t->vma_table = 0;
    }
    if (t->cr3) {
        vmm_destroy(t->cr3);
        t->cr3 = 0;
    }
    if (t->kbase) {
        heap_free((void*)(usize)t->kbase);
        t->kbase = 0;
    }
    t->state = PSTATE_EMPTY;
}

void ps_list(void) {
    kprintf("PID  STATE    NAME\r\n");
    u8 *p = (u8*)PROC_TABLE;
    for (int i = 0; i < PROC_MAX; i++, p += PROC_PCB_SIZE) {
        PCB *t = (PCB*)p;
        if (t->state == PSTATE_EMPTY) continue;
        vga_putchar((u8)('0' + (t->pid / 10) % 10));
        vga_putchar((u8)('0' + t->pid % 10));
        kprintf("   ");
        const char *states[] = {"EMPTY  ","READY  ","RUNNING","DEAD   "};
        kprintf("%s", states[t->state < 4 ? t->state : 0]);
        vga_putchar(' ');
        kprintf("%s", t->name[0] ? t->name : "(none)");
        kprintf("\r\n");
    }
}
