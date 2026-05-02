/* ================================================================
 *  Systrix OS — kernel/tss.c
 *
 *  Sets up the x86-64 Task State Segment (TSS) and installs it in
 *  the GDT so the CPU has a valid kernel stack (RSP0) to switch to
 *  when an interrupt or exception fires in ring 3.
 *
 *  Without this, ANY fault in user mode (page fault, GPF, etc.)
 *  tries to push the exception frame onto RSP0 = 0, immediately
 *  faults again (#DF), cannot deliver that either → triple fault.
 *
 *  The GDT slot at offset 0x30 is a 16-byte "system descriptor"
 *  pre-zeroed in entry.S with the base/limit/type filled in here
 *  at runtime.  We patch it in place so entry.S stays as the single
 *  source of truth for the GDT layout.
 * ================================================================ */
#include "../include/kernel.h"

/* ── TSS layout (AMD64 vol.2 §12.2.5) ─────────────────────────── */
typedef struct {
    u32 reserved0;
    u64 rsp0;           /* kernel stack for ring-0 entry (CPL 3→0) */
    u64 rsp1;
    u64 rsp2;
    u64 reserved1;
    u64 ist[7];         /* IST stacks — unused, keep zero           */
    u64 reserved2;
    u16 reserved3;
    u16 iopb_offset;    /* offset to IOPB; set to sizeof(TSS) → no IOPB */
} __attribute__((packed)) TSS64;

/* ── 16-byte system descriptor (GDT entry for TSS) ─────────────── */
typedef struct {
    u16 limit_lo;
    u16 base_lo;
    u8  base_mid;
    u8  type_attr;   /* 0x89 = Present, DPL=0, 64-bit TSS Available */
    u8  limit_hi_flags;
    u8  base_hi;
    u32 base_upper;
    u32 reserved;
} __attribute__((packed)) SysDesc64;

/* ── Static storage ─────────────────────────────────────────────── */

/* 4 KB kernel stack used exclusively for exception delivery from ring 3 */
static u8 exc_stack[4096] __attribute__((aligned(16)));

static TSS64 tss __attribute__((aligned(16)));

/* ── Helpers ────────────────────────────────────────────────────── */

static inline void ltr(u16 sel) {
    __asm__ volatile("ltr %0" :: "r"(sel));
}

/* ── tss_init ───────────────────────────────────────────────────── */

void tss_init(void) {
    /* Zero TSS, then fill required fields */
    memset(&tss, 0, sizeof(tss));
    tss.rsp0        = (u64)(exc_stack + sizeof(exc_stack)); /* top of stack */
    tss.iopb_offset = (u16)sizeof(TSS64);                   /* no IOPB */

    /* Locate the 16-byte TSS system descriptor at GDT offset 0x30.
     * The GDT base is stored in GDTR; read it rather than hard-coding
     * the address so this stays correct even if entry.S moves it. */
    struct { u16 limit; u64 base; } __attribute__((packed)) gdtr;
    __asm__ volatile("sgdt %0" : "=m"(gdtr));

    SysDesc64 *desc = (SysDesc64 *)(gdtr.base + SEL_TSS);

    u64 base  = (u64)&tss;
    u32 limit = (u32)(sizeof(TSS64) - 1);

    desc->limit_lo       = (u16)(limit & 0xFFFF);
    desc->base_lo        = (u16)(base  & 0xFFFF);
    desc->base_mid       = (u8)((base  >> 16) & 0xFF);
    desc->type_attr      = 0x89;   /* P=1 DPL=0 Type=9 (64-bit TSS Available) */
    desc->limit_hi_flags = (u8)(limit >> 16) & 0x0F;  /* G=0, limit[19:16] */
    desc->base_hi        = (u8)((base  >> 24) & 0xFF);
    desc->base_upper     = (u32)(base  >> 32);
    desc->reserved       = 0;

    /* Load the TSS selector into TR */
    ltr((u16)SEL_TSS);
}
