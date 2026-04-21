/* ================================================================
 *  SoftTail OS — kernel/scheduler.c
 *  Preemptive round-robin scheduler.
 *
 *  Sets up IDT, remaps PIC, programs PIT to 100 Hz.
 *  The actual context-switch ISR (timer_isr) lives in isr.S because
 *  it needs manual push/pop of all 15 GP registers + iretq.
 *  The globals current_pid and pit_ticks are DEFINED in isr.S
 *  (.globl) and declared extern here.
 * ================================================================ */
#include "../include/kernel.h"

/* Defined in isr.S — do NOT redefine here */
/* extern u64 current_pid;  (declared in kernel.h) */
/* extern u64 pit_ticks;    (declared in kernel.h) */

/* IDT gate descriptor (16 bytes) */
typedef struct {
    u16 off_lo;
    u16 cs;
    u8  ist;
    u8  attr;
    u16 off_mid;
    u32 off_hi;
    u32 reserved;
} __attribute__((packed)) IDTGate;

typedef struct {
    u16 limit;
    u64 base;
} __attribute__((packed)) IDTPtr;

extern void null_isr(void);
extern void timer_isr(void);
extern void isr_errcode(void);
extern void isr_page_fault(void);

static void idt_set(IDTGate *idt, int vec, void (*handler)(void)) {
    u64 addr = (u64)handler;
    idt[vec].off_lo   = (u16)(addr & 0xFFFF);
    idt[vec].cs       = KERNEL_CS;
    idt[vec].ist      = 0;
    idt[vec].attr     = 0x8E;
    idt[vec].off_mid  = (u16)((addr >> 16) & 0xFFFF);
    idt[vec].off_hi   = (u32)(addr >> 32);
    idt[vec].reserved = 0;
}

static void idt_init(void) {
    IDTGate *idt = (IDTGate*)IDT_BASE;
    for (int i = 0; i < IDT_ENTRIES; i++) idt_set(idt, i, null_isr);
    idt_set(idt, 0x20, timer_isr);

    idt_set(idt, 8,  isr_errcode);
    idt_set(idt, 10, isr_errcode);
    idt_set(idt, 11, isr_errcode);
    idt_set(idt, 12, isr_errcode);
    idt_set(idt, 13, isr_errcode);
    idt_set(idt, 14, isr_page_fault); /* #PF: demand paging + CoW */
    idt_set(idt, 17, isr_errcode);
    idt_set(idt, 21, isr_errcode);
    idt_set(idt, 29, isr_errcode);
    idt_set(idt, 30, isr_errcode);
}

static void pic_init(void) {
    outb(PIC1_CMD,  0x11); io_wait();
    outb(PIC2_CMD,  0x11); io_wait();
    outb(PIC1_DATA, 0x20); io_wait();
    outb(PIC2_DATA, 0x28); io_wait();
    outb(PIC1_DATA, 0x04); io_wait();
    outb(PIC2_DATA, 0x02); io_wait();
    outb(PIC1_DATA, 0x01); io_wait();
    outb(PIC2_DATA, 0x01); io_wait();
    outb(PIC1_DATA, 0xFE); /* unmask IRQ0 (timer) only — keyboard is polled */
    outb(PIC2_DATA, 0xFF);
}

static void pit_init(void) {
    outb(PIT_CMD, 0x36);
    outb(PIT_CH0, (u8)(PIT_DIV & 0xFF));
    outb(PIT_CH0, (u8)(PIT_DIV >> 8));
}

static void lidt(u64 base, u16 limit) {
    IDTPtr p = { limit, base };
    __asm__ volatile("lidt %0" :: "m"(p));
}

void scheduler_init(void) {
    idt_init();
    pic_init();
    pit_init();
    lidt(IDT_BASE, (u16)(IDT_ENTRIES * sizeof(IDTGate) - 1));
}

void scheduler_start(void) {
    sti();
}
