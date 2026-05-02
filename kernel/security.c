#include "../include/kernel.h"

#define STACK_CANARY_MAGIC 0xDEADBEEFCAFEBABEULL
#define SMEP_BIT (1ULL << 20)
#define SMAP_BIT (1ULL << 21)

static u64 kaslr_offset = 0;
static u64 kernel_base = 0xFFFFFFFF80000000UL;

u64 kaslr_get_offset(void) { return kaslr_offset; }

void kaslr_init(void) {
    u64 entropy = 0;
    __asm__ volatile("rdtsc" : "=a"(entropy));
    entropy ^= (u64)(usize)&kaslr_init;
    entropy ^= pit_ticks;
    kaslr_offset = (entropy & 0x3FFFFF) << 21;
    if (kaslr_offset == 0) kaslr_offset = 1ULL << 30;
}

u64 stack_canary_generate(void) {
    u32 lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    u64 canary = ((u64)hi << 32) | lo;
    canary |= STACK_CANARY_MAGIC;
    return canary;
}

void smap_smep_init(void) {
    u64 cr4 = 0;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= SMEP_BIT;
    cr4 |= SMAP_BIT;
    __asm__ volatile("mov %0, %%cr4" :: "r"(cr4) : "memory");
}

static inline void set_cr4_smap(void) {
    u64 cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= SMAP_BIT;
    __asm__ volatile("mov %0, %%cr4" :: "r"(cr4) : "memory");
}

static inline void clear_cr4_smap(void) {
    u64 cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 &= ~SMAP_BIT;
    __asm__ volatile("mov %0, %%cr4" :: "r"(cr4) : "memory");
}

int is_user_addr(const void *addr, usize size) {
    u64 va = (u64)addr;
    if (va >= 0xFFFFFFFF80000000UL) return 0;
    if (va + size < va) return 0;
    if (va + size >= 0xFFFFFFFF80000000UL) return 0;
    return 1;
}

i64 copy_from_user(void *dst, const void *src, usize n) {
    if (!is_user_addr(src, n)) return (i64)EFAULT;
    u8 *d = (u8*)dst;
    const u8 *s = (const u8*)src;
    for (usize i = 0; i < n; i++) {
        volatile u8 v = s[i];
        d[i] = v;
    }
    return 0;
}

i64 copy_to_user(void *dst, const void *src, usize n) {
    if (!is_user_addr(dst, n)) return (i64)EFAULT;
    u8 *d = (u8*)dst;
    const u8 *s = (const u8*)src;
    for (usize i = 0; i < n; i++) {
        d[i] = s[i];
    }
    return 0;
}

int syscall_validate_args(u64 num, u64 arg1, u64 arg2, u64 arg3, u64 arg4, u64 arg5, u64 arg6) {
    (void)arg6;
    switch (num) {
        case 0: case 1: case 3: case 4: case 7:
            return is_user_addr((void*)arg2, arg3);
        case 2:
            return is_user_addr((void*)arg1, 256);
        case 9:
            return 1;
        case 10:
            return is_user_addr((void*)arg1, arg2);
        case 12:
            return 1;
        case 57:
            return is_user_addr((void*)arg1, 256);
        case 59:
            return is_user_addr((void*)arg1, 256) &&
                   (arg2 == 0 || is_user_addr((void*)arg2, 256)) &&
                   (arg3 == 0 || is_user_addr((void*)arg3, 256));
        default:
            return 1;
    }
}

void syscall_log_fault(u64 num, u64 arg1, u64 arg2, u64 arg3) {
    kprintf("\n[SYSCALL FAULT] pid=");
    PCB *pcb = process_current_pcb();
    if (pcb) {
        print_hex_byte((u8)(pcb->pid >> 8));
        print_hex_byte((u8)pcb->pid);
    }
    kprintf(" num=");
    print_hex_byte((u8)(num >> 8));
    print_hex_byte((u8)num);
    kprintf(" args=[");
    print_hex_byte((u8)(arg1 >> 8));
    print_hex_byte((u8)arg1);
    kprintf(", ");
    print_hex_byte((u8)(arg2 >> 8));
    print_hex_byte((u8)arg2);
    kprintf(", ");
    print_hex_byte((u8)(arg3 >> 8));
    print_hex_byte((u8)arg3);
    kprintf("]\n");
}
