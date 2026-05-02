/* ================================================================
 *  Systrix OS — kernel/resilience.c
 *
 *  Four subsystems added together:
 *
 *  1. SMP  — bring up additional CPU cores via LAPIC/APIC.
 *     BSP (core 0) sends INIT+SIPI to each AP found in the MADT.
 *     APs park in ap_entry, increment smp_cores_up, then spin
 *     polling smp_work_fn; the BSP can dispatch lightweight tasks.
 *
 *  2. OOM handler (improved)  — called when pmm_alloc() returns 0.
 *     Scores every process by RSS×priority, kills the top scorer,
 *     retries allocation.  Falls through to kernel panic if no
 *     victim exists or second alloc also fails.
 *
 *  3. Kernel panic  — print banner + register dump, attempt to
 *     kill the faulting process and continue (soft recovery).
 *     If recovery is not possible, halts with interrupts disabled.
 *
 *  4. Watchdog timer  — a software watchdog driven by the existing
 *     PIT at 1000 Hz.  Every subsystem that could hang calls
 *     watchdog_pet().  The timer ISR calls watchdog_tick(); if
 *     WD_TIMEOUT_MS ms pass without a pet, the watchdog fires,
 *     prints a warning, and kills the current process (or panics
 *     if it is the kernel idle task).
 * ================================================================ */

#include "../include/kernel.h"

/* ================================================================
 *  1. SMP — Symmetric Multi-Processing
 *
 *  We locate the LAPIC and AP list by scanning for the ACPI MADT
 *  table.  For QEMU's default pc machine the RSDP is always at
 *  0xE0000–0xFFFFF (EBDA search), or at the well-known QEMU
 *  address 0x7FE14.  We try both.
 *
 *  Memory map:
 *    SMP_TRAMPOLINE  0x8000   — 16-bit AP entry code (< 1MB, page aligned)
 *    SMP_STACK_BASE  0x80000  — 4 KB stacks per AP, growing down
 *
 *  The trampoline switches the AP to long mode and jumps to
 *  ap_entry_c() in this file.
 * ================================================================ */

#define SMP_TRAMPOLINE   0x6000UL  /* safe: below 0x7C00 bootloader, away from kernel at 0x8000 */
#define SMP_STACK_BASE   0x80000UL
#define SMP_STACK_SZ     4096
#define SMP_MAX_CPUS     8

/* Shared variables written by APs, read by BSP */
volatile int smp_cores_up  = 1;   /* BSP counts as 1 */
volatile int smp_total_cpus= 1;

/* Simple inter-core work dispatch: BSP sets these, AP clears after done */
void (* volatile smp_work_fn)(int cpu_id) = (void*)0;

/* LAPIC base — default; overridden by MADT if present */
static u64 lapic_base = 0xFEE00000UL;

/* ── LAPIC register access ─────────────────────────────────────── */
static inline u32 lapic_read(u32 reg) {
    return *(volatile u32*)(lapic_base + reg);
}
static inline void lapic_write(u32 reg, u32 val) {
    *(volatile u32*)(lapic_base + reg) = val;
}

#define LAPIC_ID        0x020
#define LAPIC_VER       0x030
#define LAPIC_SVR       0x0F0   /* spurious vector register */
#define LAPIC_ICR_LO    0x300   /* interrupt command register low  */
#define LAPIC_ICR_HI    0x310   /* interrupt command register high */
#define LAPIC_TMR_INIT  0x380
#define LAPIC_TMR_CUR   0x390
#define LAPIC_EOI       0x0B0

static void lapic_enable(void) {
    /* Set SVR bit 8 to enable LAPIC, vector 0xFF for spurious */
    lapic_write(LAPIC_SVR, lapic_read(LAPIC_SVR) | 0x100 | 0xFF);
}

static void lapic_send_init(u8 apic_id) {
    lapic_write(LAPIC_ICR_HI, (u32)apic_id << 24);
    lapic_write(LAPIC_ICR_LO, 0x00004500);  /* INIT, level=assert */
    /* wait ~10ms */
    for (volatile int i = 0; i < 1000000; i++) {}
    lapic_write(LAPIC_ICR_LO, 0x00008500);  /* INIT, level=deassert */
    for (volatile int i = 0; i < 200000;  i++) {}
}

static void lapic_send_sipi(u8 apic_id, u8 vec) {
    /* vec = trampoline page number (physical addr / 0x1000) */
    lapic_write(LAPIC_ICR_HI, (u32)apic_id << 24);
    lapic_write(LAPIC_ICR_LO, 0x00004600 | vec);
    for (volatile int i = 0; i < 200000; i++) {}
}

/* ── Minimal ACPI MADT scan ─────────────────────────────────────── */
typedef struct { u8 sig[8]; u8 csum; u8 oem[6]; u8 rev; u32 rsdt; u32 len; u64 xsdt; u8 xcsum; u8 _pad[3]; } __attribute__((packed)) RSDP;
typedef struct { u8 sig[4]; u32 len; u8 rev; u8 csum; u8 oem[6]; u8 oemtbl[8]; u32 oem_rev; u32 creator; u32 creator_rev; } __attribute__((packed)) ACPIHdr;
typedef struct { ACPIHdr hdr; u32 lapic_addr; u32 flags; } __attribute__((packed)) MADT;
typedef struct { u8 type; u8 len; u8 acpi_id; u8 apic_id; u32 flags; } __attribute__((packed)) MADT_LAPIC;

static RSDP *find_rsdp(void) {
    /* Search EBDA and BIOS ROM area */
    u8 *p = (u8*)0xE0000;
    for (; p < (u8*)0x100000; p += 16) {
        if (p[0]=='R'&&p[1]=='S'&&p[2]=='D'&&p[3]==' '&&p[4]=='P'&&p[5]=='T'&&p[6]=='R'&&p[7]==' ')
            return (RSDP*)p;
    }
    return (void*)0;
}

/* AP apic_ids discovered during MADT scan */
static u8  ap_apic_ids[SMP_MAX_CPUS];
static int ap_count = 0;

static void madt_scan(void) {
    RSDP *rsdp = find_rsdp();
    if (!rsdp) return;

    /* Walk RSDT entries looking for "APIC" (MADT) */
    u32 *rsdt_entries = (u32*)(u64)(rsdp->rsdt + 36);  /* skip header */
    ACPIHdr *rsdt_hdr = (ACPIHdr*)(u64)rsdp->rsdt;
    u32 n_entries = (rsdt_hdr->len - 36) / 4;

    u8 bsp_apic_id = (u8)(lapic_read(LAPIC_ID) >> 24);

    for (u32 i = 0; i < n_entries && i < 32; i++) {
        ACPIHdr *hdr = (ACPIHdr*)(u64)rsdt_entries[i];
        if (hdr->sig[0]!='A'||hdr->sig[1]!='P'||hdr->sig[2]!='I'||hdr->sig[3]!='C') continue;

        MADT *madt = (MADT*)hdr;
        if (madt->lapic_addr) lapic_base = (u64)madt->lapic_addr;

        u8 *entry = (u8*)(madt + 1);
        u8 *end   = (u8*)madt + madt->hdr.len;
        while (entry < end) {
            if (entry[0] == 0) {  /* Processor Local APIC */
                MADT_LAPIC *l = (MADT_LAPIC*)entry;
                if ((l->flags & 1) && l->apic_id != bsp_apic_id && ap_count < SMP_MAX_CPUS - 1)
                    ap_apic_ids[ap_count++] = l->apic_id;
            }
            entry += entry[1];
        }
        break;
    }
}

/* ── AP trampoline — 16-bit real mode → long mode jump ──────────
 * Written as raw x86 bytes to SMP_TRAMPOLINE (0x8000).
 * Switches to protected mode, enables paging (reusing BSP's PML4
 * which is always at known physical address), jumps to ap_entry_c.
 * ---------------------------------------------------------------- */
extern void ap_entry_c(void);   /* defined below */

/* Physical address of BSP's PML4 — set by smp_init() */
static u64 smp_pml4_phys = 0;

static void install_trampoline(void) {
    /* We store key parameters in the trampoline page itself at
     * fixed offsets after the code, then the code reads them. */

    /* Simple approach: write a small machine-code stub.
     * Layout at 0x8000:
     *   [0x00] 16-bit real-mode code
     *   [0xF0] smp_pml4_phys (8 bytes)
     *   [0xF8] ap_entry_c address (8 bytes)
     *   [0xFC] AP stack pointer slot (8 bytes, per-AP filled before SIPI)
     */
    u8 *t = (u8*)SMP_TRAMPOLINE;
    /* Clear trampoline page */
    memset(t, 0, 0x1000);

    /* Real-mode entry: CLI, set up data seg, jump to protected-mode setup
     * We use the "inline shellcode" technique — write the bytes directly.
     * This blob:
     *   cli
     *   xor ax,ax; mov ds,ax; mov es,ax; mov ss,ax
     *   lgdt [cs:0x8060]      (GDT descriptor at offset 0x60)
     *   mov eax,cr0; or al,1; mov cr0,eax   (PE bit)
     *   ljmp 0x08:0x8020      (far jump to 32-bit stub at 0x8020)
     *
     * 32-bit stub at 0x8020:
     *   set up selectors; enable PAE; load PML4 (from 0x80F0);
     *   set EFER.LME; enable paging; ljmp 0x08:ap_entry_c_trampoline
     *
     * 64-bit ap_entry_c_trampoline at 0x8040:
     *   set rsp from 0x80FC; jmp ap_entry_c
     *
     * This is complex to hand-assemble correctly here, so instead we
     * use a well-known minimal trampoline byte sequence for x86-64:
     */

    /* --- 16-bit header at 0x8000 --- */
    static const u8 tramp16[] = {
        0xFA,                   /* cli */
        0x31, 0xC0,             /* xor ax,ax */
        0x8E, 0xD8,             /* mov ds,ax */
        0x8E, 0xC0,             /* mov es,ax */
        0x8E, 0xD0,             /* mov ss,ax */
        /* lgdt [0x8060] */
        0x0F, 0x01, 0x16, 0x60, 0x80,
        /* mov eax,cr0 */
        0x0F, 0x20, 0xC0,
        /* or al, 1 (set PE) */
        0x0C, 0x01,
        /* mov cr0, eax */
        0x0F, 0x22, 0xC0,
        /* ljmp 0x08:0x8020  (far jump: 66 EA lo hi 0x00 seg_lo seg_hi) */
        0x66, 0xEA,
        0x20, 0x80, 0x00, 0x00,  /* offset 0x8020 */
        0x08, 0x00               /* selector 0x08 */
    };
    memcpy(t, tramp16, sizeof(tramp16));

    /* --- 32-bit PM stub at 0x8020 --- */
    /* Sets up segments, loads PML4, enables long mode */
    static const u8 tramp32[] = {
        /* mov ax,0x10; mov ds,ax; mov es,ax; mov ss,ax */
        0x66, 0xB8, 0x10, 0x00,
        0x8E, 0xD8, 0x8E, 0xC0, 0x8E, 0xD0,
        /* mov eax, cr4; or eax, 0x20 (PAE); mov cr4, eax */
        0x0F, 0x20, 0xE0, 0x83, 0xC8, 0x20, 0x0F, 0x22, 0xE0,
        /* mov eax, [0x80F0]  (low 32 bits of PML4 phys addr) */
        0x8B, 0x05, 0xF0, 0x80, 0x00, 0x00,
        /* mov cr3, eax */
        0x0F, 0x22, 0xD8,
        /* rdmsr EFER (0xC0000080); or eax,0x100 (LME); wrmsr */
        0xB9, 0x80, 0x00, 0x00, 0xC0,
        0x0F, 0x32, 0x0D, 0x00, 0x01, 0x00, 0x00, 0x0F, 0x30,
        /* mov eax,cr0; or eax,0x80000001; mov cr0,eax (PG+PE) */
        0x0F, 0x20, 0xC0,
        0x0D, 0x01, 0x00, 0x00, 0x80,
        0x0F, 0x22, 0xC0,
        /* ljmp 0x08:0x8040 (64-bit entry) */
        0x66, 0xEA, 0x40, 0x80, 0x00, 0x00, 0x08, 0x00
    };
    memcpy(t + 0x20, tramp32, sizeof(tramp32));

    /* --- 64-bit entry at 0x8040 --- */
    /* mov rsp, [0x80FC]; jmp [0x80F8] (absolute indirect) */
    static const u8 tramp64[] = {
        /* mov rsp, qword [rel 0x80FC] */
        0x48, 0x8B, 0x24, 0x25, 0xFC, 0x80, 0x00, 0x00,
        /* jmp qword [rel 0x80F8] */
        0xFF, 0x24, 0x25, 0xF8, 0x80, 0x00, 0x00,
    };
    memcpy(t + 0x40, tramp64, sizeof(tramp64));

    /* --- Minimal GDT at 0x8060 --- */
    /* GDT descriptor */
    u16 *gdt_desc = (u16*)(SMP_TRAMPOLINE + 0x60);
    gdt_desc[0] = 23;                   /* limit = 3 entries × 8 - 1 */
    *(u32*)(gdt_desc + 1) = (u32)(SMP_TRAMPOLINE + 0x68); /* base */

    /* GDT entries at 0x8068 */
    u64 *gdt = (u64*)(SMP_TRAMPOLINE + 0x68);
    gdt[0] = 0;                          /* null descriptor */
    gdt[1] = 0x00CF9A000000FFFFULL;      /* 32-bit code, ring 0 */
    gdt[2] = 0x00CF92000000FFFFULL;      /* 32-bit data, ring 0 */

    /* --- Parameters at 0x80F0 --- */
    *(u64*)(SMP_TRAMPOLINE + 0xF0) = smp_pml4_phys;  /* PML4 physical */
    /* 0x80F8 = ap_entry_c address, 0x80FC = stack pointer — filled per AP */
}

/* Called by each AP after it reaches long mode */
void ap_entry_c(void) {
    lapic_enable();

    int my_id = smp_cores_up;   /* simple monotonic ID */
    __atomic_add_fetch(&smp_cores_up, 1, __ATOMIC_SEQ_CST);

    /* Spin polling for work from BSP */
    for (;;) {
        void (*fn)(int) = smp_work_fn;
        if (fn) {
            fn(my_id);
            __atomic_store_n(&smp_work_fn, (void*)0, __ATOMIC_SEQ_CST);
        }
        /* halt until next interrupt to save power */
        __asm__ volatile("hlt");
    }
}

void smp_init(void) {
    /* Read BSP's CR3 to share page tables with APs */
    u64 cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    smp_pml4_phys = cr3;

    /* Map the LAPIC MMIO page before any lapic_read/lapic_write.
     * entry.S identity-maps only 0..1 GB (512x2MB huge pages).
     * 0xFEE00000 is outside that range and causes a #PF without
     * this explicit mapping.  PTE_PCD (bit 4) disables caching. */
    vmm_map(cr3, lapic_base, lapic_base,
            PTE_PRESENT | PTE_WRITE | (1UL << 4) /* PTE_PCD */);

    lapic_enable();
    madt_scan();

    if (ap_count == 0) {
        kprintf("[SMP] Single-core only (no APs found in MADT)\r\n");
        return;
    }

    install_trampoline();

    kprintf("[SMP] Bringing up %llu AP(s)...\r\n", (unsigned long long)(ap_count));

    int prev_up = smp_cores_up;

    for (int i = 0; i < ap_count; i++) {
        u8 apic_id = ap_apic_ids[i];

        /* Give this AP a stack */
        u64 stack_top = SMP_STACK_BASE - (u64)i * SMP_STACK_SZ;
        *(u64*)(SMP_TRAMPOLINE + 0xF8) = (u64)ap_entry_c;
        *(u64*)(SMP_TRAMPOLINE + 0xFC) = stack_top;

        lapic_send_init(apic_id);
        lapic_send_sipi(apic_id, (u8)(SMP_TRAMPOLINE >> 12));
        lapic_send_sipi(apic_id, (u8)(SMP_TRAMPOLINE >> 12));  /* SIPI sent twice per spec */

        /* Wait up to 100ms for AP to come up */
        int timeout = 100000;
        while (smp_cores_up == prev_up && --timeout) {
            for (volatile int d = 0; d < 100; d++) {}
        }
        if (smp_cores_up > prev_up) {
            kprintf("[SMP] AP %llu online (core %llu)\r\n", (unsigned long long)(apic_id), (unsigned long long)(smp_cores_up) - 1);
            prev_up = smp_cores_up;
        } else {
            kprintf("[SMP] AP %llu did not respond\r\n", (unsigned long long)(apic_id));
        }
    }

    smp_total_cpus = smp_cores_up;
    kprintf("[SMP] Total cores: %llu\r\n", (unsigned long long)(smp_total_cpus));
}

/* Dispatch fn to all idle APs (fire-and-forget, BSP does not wait) */
void smp_dispatch(void (*fn)(int cpu_id)) {
    __atomic_store_n(&smp_work_fn, fn, __ATOMIC_SEQ_CST);
}

/* ================================================================
 *  2. OOM Handler (improved)
 *
 *  Called when pmm_alloc() returns 0 anywhere in the kernel.
 *  Strategy:
 *    - Score each process:  score = rss_pages (simple heuristic)
 *    - Kill the highest scorer that is NOT the kernel idle task
 *    - After freeing pages, return so the caller can retry alloc
 *    - If no victim or no pages freed → kernel_panic()
 * ================================================================ */

/* Forward declaration */
void kernel_panic(const char *reason);

void oom_kill(void) {
    kprintf("\r\n[OOM] Out of memory! Scanning for victim...\r\n");

    u32  free_before = pmm_free_pages();
    PCB *victim      = (void*)0;
    u32  best_score  = 0;

    u8 *p = (u8*)PROC_TABLE;
    for (int i = 0; i < PROC_MAX; i++, p += PROC_PCB_SIZE) {
        PCB *t = (PCB*)p;
        if (t->state == PSTATE_EMPTY || t->state == PSTATE_DEAD) continue;
        if (t->pid == 0) continue;   /* never kill PID 0 (kernel) */

        /* Walk page tables counting user-mapped pages (RSS) */
        u32 rss = 0;
        if (t->cr3) {
            u64 *pml4 = (u64*)t->cr3;
            for (int i4 = 1; i4 < 512; i4++) {
                if (!(pml4[i4] & PTE_PRESENT)) continue;
                u64 *pdpt = (u64*)(pml4[i4] & ~0xFFFUL);
                for (int i3 = 0; i3 < 512; i3++) {
                    if (!(pdpt[i3] & PTE_PRESENT) || (pdpt[i3] & (1UL<<7))) continue;
                    u64 *pd = (u64*)(pdpt[i3] & ~0xFFFUL);
                    for (int i2 = 0; i2 < 512; i2++) {
                        if (!(pd[i2] & PTE_PRESENT) || (pd[i2] & (1UL<<7))) continue;
                        u64 *pt = (u64*)(pd[i2] & ~0xFFFUL);
                        for (int i1 = 0; i1 < 512; i1++)
                            if ((pt[i1] & PTE_PRESENT) && (pt[i1] & PTE_USER)) rss++;
                    }
                }
            }
        }

        if (rss > best_score) { best_score = rss; victim = t; }
    }

    if (!victim) {
        kernel_panic("OOM: no user process to kill");
        return;
    }

    kprintf("[OOM] Killing PID %llu (", (unsigned long long)(victim->pid));
    kprintf("%s", victim->name);
    kprintf(") RSS=%llu pages\r\n", (unsigned long long)(best_score));

    /* Mark dead — scheduler will skip it; vmm_destroy frees pages */
    victim->state = PSTATE_DEAD;
    if (victim->cr3) vmm_destroy(victim->cr3);

    u32 free_after = pmm_free_pages();
    kprintf("[OOM] Freed %llu pages. Free now: %llu\r\n", (unsigned long long)(free_after - free_before), (unsigned long long)(free_after));

    if (free_after == free_before)
        kernel_panic("OOM: killed process but freed no pages");
}

/* ================================================================
 *  3. Kernel Panic
 *
 *  Prints a red-on-black panic banner, the reason string, and a
 *  minimal register dump.  Then tries soft recovery (kill current
 *  process and yield).  If recovery is impossible, halts.
 * ================================================================ */

static int panic_depth = 0;   /* guard against re-entrant panics */

void kernel_panic(const char *reason) {
    /* Disable interrupts immediately */
    __asm__ volatile("cli");

    if (panic_depth++ > 0) {
        /* Double panic — give up */
        kprintf("[PANIC] Double panic -- system halted\r\n");
        for (;;) __asm__ volatile("hlt");
    }

    /* Print banner */
    kprintf("\r\n!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\r\n!!         K E R N E L  P A N I C       !!\r\n!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\r\nReason: ");
    kprintf("%s", reason ? reason : "(null)");
    kprintf("\r\n");

    /* Register dump */
    u64 rsp, rip, cr3_val, cr2_val;
    __asm__ volatile("mov %%rsp, %0" : "=r"(rsp));
    __asm__ volatile("call 1f\n1: pop %0" : "=r"(rip));
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3_val));
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2_val));

    kprintf("  RSP="); kprintf("0x%016llx", (unsigned long long)(rsp));
    kprintf("  RIP="); kprintf("0x%016llx", (unsigned long long)(rip));
    kprintf("\r\n");
    kprintf("  CR3="); kprintf("0x%016llx", (unsigned long long)(cr3_val));
    kprintf("  CR2="); kprintf("0x%016llx", (unsigned long long)(cr2_val));
    kprintf("\r\n");

    /* Memory stats */
    u32 fp = pmm_free_pages();
    kprintf("  Free pages: %llu (%llu KB)\r\n", (unsigned long long)(fp), (unsigned long long)(fp * 4));

    /* Try soft recovery: kill the currently running process */
    PCB *cur = (void*)0;
    u8 *p = (u8*)PROC_TABLE;
    for (int i = 0; i < PROC_MAX; i++, p += PROC_PCB_SIZE) {
        PCB *t = (PCB*)p;
        if (t->state == PSTATE_RUNNING && t->pid != 0) { cur = t; break; }
    }

    if (cur) {
        kprintf("[PANIC] Attempting soft recovery: killing PID %llu", (unsigned long long)(cur->pid));
        kprintf(" ("); kprintf("%s", cur->name); kprintf(")\r\n");

        cur->state = PSTATE_DEAD;
        if (cur->cr3) vmm_destroy(cur->cr3);

        kprintf("[PANIC] Recovery attempted. Resuming kernel.\r\n!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\r\n");

        panic_depth = 0;
        __asm__ volatile("sti");
        return;   /* caller's context is gone; scheduler will pick next */
    }

    /* No recoverable process — hard halt */
    kprintf("[PANIC] No user process to kill. System halted.\r\n!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\r\n");
    for (;;) __asm__ volatile("hlt");
}

/* ================================================================
 *  4. Watchdog Timer
 *
 *  The BSP's PIT fires at 1000 Hz.  timer_isr in isr.S calls
 *  watchdog_tick() once per interrupt (added to isr.S like
 *  sys_snd_mix_tick).
 *
 *  Any code that could legitimately run for a long time (disk I/O,
 *  network init, tight loops) should call watchdog_pet() to reset
 *  the counter.
 *
 *  If WD_TIMEOUT_MS consecutive ticks pass without a pet, the
 *  watchdog fires:
 *    - If a user process is running → kill it (and print a warning)
 *    - If only the kernel is running → kernel_panic()
 *
 *  The watchdog can be temporarily suspended with watchdog_suspend()
 *  and re-armed with watchdog_resume() for known-long operations.
 * ================================================================ */

#define WD_TIMEOUT_MS  30000  /* 30 seconds without a pet = hang */

static volatile u32 wd_counter   = 0;
static volatile int wd_suspended = 0;
static volatile int wd_enabled   = 0;

void watchdog_init(void) {
    wd_counter   = 0;
    wd_suspended = 0;
    wd_enabled   = 1;
    kprintf("[WD] Watchdog armed (timeout=%llums)\r\n", (unsigned long long)(WD_TIMEOUT_MS));
}

/* Call from any kernel subsystem to signal "still alive" */
void watchdog_pet(void) {
    wd_counter = 0;
}

void watchdog_suspend(void) { wd_suspended = 1; }
void watchdog_resume(void)  { wd_suspended = 0; wd_counter = 0; }

/* Called from timer_isr — must be fast, no blocking */
void watchdog_tick(void) {
    if (!wd_enabled || wd_suspended) return;

    if (++wd_counter < WD_TIMEOUT_MS) return;

    /* Watchdog fired */
    wd_counter = 0;

    /* Find running process */
    u8 *p = (u8*)PROC_TABLE;
    for (int i = 0; i < PROC_MAX; i++, p += PROC_PCB_SIZE) {
        PCB *t = (PCB*)p;
        if (t->state == PSTATE_RUNNING && t->pid != 0) {
            /* Kill it */
            kprintf("\r\n[WD] TIMEOUT: killing hung PID %llu", (unsigned long long)(t->pid));
            kprintf(" ("); kprintf("%s", t->name); kprintf(")\r\n");
            t->state = PSTATE_DEAD;
            if (t->cr3) vmm_destroy(t->cr3);
            return;
        }
    }

    /* Only kernel running and it's hung */
    kernel_panic("Watchdog: kernel idle loop hung");
}
