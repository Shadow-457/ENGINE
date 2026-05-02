/* ================================================================
 *  Systrix OS — kernel/elf.c  (PATCHED)
 *
 *  Fix Bug 4: elf_load() previously wrote PT_LOAD segments directly
 *  to ph->vaddr using memcpy, which only works in the kernel's own
 *  identity-mapped CR3.  When the process switches to its own CR3
 *  those physical pages are unmapped → immediate page fault.
 *
 *  Fix: allocate physical pages via pmm_alloc(), copy the segment
 *  data there, then map them into the process's CR3 with vmm_map().
 *  The identity map means phys == virt in the kernel, so the memcpy
 *  destination is still valid during the load.
 *
 *  Fix Bug 5 (here too): map PROC_STACK_TOP pages into the new
 *  CR3 so the user stack exists when process_run() iretqs into the
 *  process.  We allocate PROC_KSTACK_SZ / PAGE_SIZE pages for the
 *  user stack (one 4KB page is enough for typical programs).
 * ================================================================ */
#include "../include/kernel.h"

static int elf_verify(const Elf64Hdr *h) {
    if (h->magic   != (u32)ELF_MAGIC)  return -1;
    if (h->cls     != ELFCLASS64)       return -1;
    if (h->type    != ET_EXEC)          return -1;
    if (h->machine != EM_X86_64)        return -1;
    return 0;
}

i64 elf_load(void *data, usize size, const char *name) {
    (void)size;
    Elf64Hdr *hdr = (Elf64Hdr*)data;
    if (elf_verify(hdr) != 0) return -1;

    /* Create the process first so we have a CR3 to map into */
    i64 pid = process_create(hdr->entry, name);
    if (pid < 0) return -1;

    /* Find the PCB for the new process */
    PCB *pcb = NULL;
    u8 *pp = (u8*)PROC_TABLE;
    for (int i = 0; i < PROC_MAX; i++, pp += PROC_PCB_SIZE) {
        PCB *t = (PCB*)pp;
        if (t->pid == (u64)pid) { pcb = t; break; }
    }
    if (!pcb) return -1;

    u64 cr3 = pcb->cr3;

    /* FIX (Bug 4): walk PT_LOAD segments, allocate physical pages,
     * copy content, and map them into the process address space. */
    u16 phnum     = hdr->phnum;
    u16 phentsize = hdr->phentsize;
    u8 *phdr_base = (u8*)data + hdr->phoff;

    for (u16 i = 0; i < phnum; i++) {
        Elf64Phdr *ph = (Elf64Phdr*)(phdr_base + (u64)i * phentsize);
        if (ph->type != PT_LOAD) continue;

        u64 vaddr  = ph->vaddr;
        u64 memsz  = ph->memsz;
        u64 filesz = ph->filesz;
        u8 *src    = (u8*)data + ph->offset;

        /* Round down/up to page boundaries */
        u64 vbase  = vaddr & ~(u64)(PAGE_SIZE - 1);
        u64 vtop   = (vaddr + memsz + PAGE_SIZE - 1) & ~(u64)(PAGE_SIZE - 1);
        usize npages = (usize)((vtop - vbase) / PAGE_SIZE);

        for (usize pg = 0; pg < npages; pg++) {
            u64 phys = pmm_alloc();
            if (!phys) return -1;
            memset((void*)phys, 0, PAGE_SIZE);

            /* Copy whatever file bytes land in this page */
            u64 page_vaddr = vbase + pg * PAGE_SIZE;
            i64 seg_off    = (i64)page_vaddr - (i64)vaddr;
            if (seg_off < (i64)filesz && seg_off + (i64)PAGE_SIZE > 0) {
                i64 copy_start = seg_off < 0 ? -seg_off : 0;
                i64 src_start  = seg_off < 0 ? 0 : seg_off;
                i64 copy_len   = (i64)PAGE_SIZE - copy_start;
                i64 remaining  = (i64)filesz - src_start;
                if (copy_len > remaining) copy_len = remaining;
                if (copy_len > 0)
                    memcpy((u8*)phys + copy_start, src + src_start, (usize)copy_len);
            }

            /* FIX (Bug 7): derive PTE flags from ELF segment flags.
             * Code segments (PF_X, not PF_W) are mapped read+execute
             * (PTE_USER_RX — no PTE_WRITE, no PTE_NX).  Data/BSS
             * segments (PF_W) are mapped read+write+noexecute
             * (PTE_USER_RW | PTE_NX).  Previously every segment got
             * PTE_USER_RW with no NX, which is overly permissive, but
             * worse: on some code-paths the CPU's NX enforcement
             * triggered because intermediate page-table entries lacked
             * the correct flags, causing the page-fault that led to the
             * triple fault. */
            u64 seg_flags = PTE_PRESENT | PTE_USER;
            if (ph->flags & PF_W) {
                seg_flags |= PTE_WRITE;   /* writable data/BSS   */
                seg_flags |= PTE_NX;      /* data is not executable */
            }
            /* No PTE_NX on code segments → CPU can fetch instructions */
            vmm_map(cr3, page_vaddr, phys, seg_flags);
        }
    }

    /* FIX (Bug 5): map user stack pages into the new CR3.
     * SHC (the Shadow compiler) uses ~97KB of stack, so we map
     * 32 pages (128KB) below PROC_STACK_TOP to be safe. */
    {
        u64 phys;
        for (int sp = 1; sp <= 32; sp++) {
            u64 stack_page = PROC_STACK_TOP - (u64)sp * PAGE_SIZE;
            phys = pmm_alloc();
            if (!phys) return -1;
            memset((void*)phys, 0, PAGE_SIZE);
            vmm_map(cr3, stack_page, phys, PTE_USER_RW | PTE_NX);
        }
    }

    return pid;
}
