#include "../include/kernel.h"

#define CLONE_VM      0x00000100
#define CLONE_FS      0x00000200
#define CLONE_FILES   0x00000400
#define CLONE_SIGHAND 0x00000800
#define CLONE_THREAD  0x00010000
#define CLONE_CHILD_CLEARTID  0x00200000
#define CLONE_CHILD_SETTID    0x01000000

#define FD_MAX 256

extern u64 kernel_return_rsp;
extern void process_destroy(PCB *t);
extern signal_state_t proc_signals[PROC_MAX];

i64 sys_fork(void) {
    PCB *parent = process_current_pcb();
    if (!parent) return (i64)ENOMEM;
    i64 pid = process_create(parent->entry, parent->name);
    if (pid < 0) return pid;
    u8 *p = (u8*)PROC_TABLE;
    PCB *child = NULL;
    for (int i = 0; i < PROC_MAX; i++, p += PROC_PCB_SIZE) {
        PCB *t = (PCB*)p;
        if (t->pid == (u64)pid) { child = t; break; }
    }
    if (!child) return (i64)ENOMEM;
    child->cr3 = vmm_create_space();
    if (!child->cr3) { child->state = PSTATE_DEAD; return (i64)ENOMEM; }
    vmm_map_kernel(child->cr3);
    vmm_cow_fork(parent->cr3, child->cr3);
    void *kstack = heap_malloc(PROC_KSTACK_SZ);
    if (!kstack) { vmm_destroy(child->cr3); child->state = PSTATE_DEAD; return (i64)ENOMEM; }
    child->kbase = (u64)kstack;
    child->kstack = (u64)kstack + PROC_KSTACK_SZ;
    child->ursp = parent->ursp;
    child->brk = parent->brk;
    VMA *vmas = vmm_vma_alloc();
    child->vma_table = vmas ? (u64)(usize)vmas : 0;
    if (parent->vma_table) {
        VMA *pv = (VMA*)(usize)parent->vma_table;
        if (vmas) {
            for (int i = 0; i < VMA_MAX; i++) {
                if (pv[i].start >= pv[i].end) break;
                vmas[i] = pv[i];
            }
        }
    }
    int pidx = (int)(parent->pid - 1);
    int cidx = (int)(child->pid - 1);
    if (pidx >= 0 && pidx < PROC_MAX && cidx >= 0 && cidx < PROC_MAX) {
        proc_signals[cidx] = proc_signals[pidx];
        proc_signals[cidx].pending = 0;
    }
    child->state = PSTATE_READY;
    return pid;
}

i64 sys_execve(const char *path, char **argv, char **envp) {
    (void)argv; (void)envp;
    PCB *pcb = process_current_pcb();
    if (!pcb) return (i64)EFAULT;
    const char *fname = path;
    while (*fname == '/') fname++;
    char name83[12];
    format_83_name(fname, name83);
    i64 fd = vfs_open(name83);
    if (fd < 0) return fd;
    /* get real file size via seek-to-end */
    i64 file_sz = vfs_seek((u64)fd, 0, 2);
    if (file_sz <= 0) { vfs_close(fd); return (i64)ENOENT; }
    vfs_seek((u64)fd, 0, 0); /* rewind */
    void *elf_data = sys_malloc((usize)file_sz);
    if (!elf_data) { vfs_close(fd); return (i64)ENOMEM; }
    i64 sz = vfs_read(fd, elf_data, (usize)file_sz);
    vfs_close(fd);
    if (sz <= 0) { sys_free(elf_data); return (i64)ENOENT; }
    Elf64Hdr *eh = (Elf64Hdr*)elf_data;
    if (eh->magic != ELF_MAGIC || eh->type != ET_EXEC || eh->machine != EM_X86_64) {
        sys_free(elf_data);
        return (i64)EINVAL;
    }
    vmm_destroy(pcb->cr3);
    pcb->cr3 = vmm_create_space();
    if (!pcb->cr3) { sys_free(elf_data); return (i64)ENOMEM; }
    vmm_map_kernel(pcb->cr3);
    if (pcb->vma_table) {
        vmm_vma_free((VMA*)(usize)pcb->vma_table);
        pcb->vma_table = (u64)(usize)vmm_vma_alloc();
    }
    Elf64Phdr *phdr = (Elf64Phdr*)(elf_data + eh->phoff);
    for (u16 i = 0; i < eh->phnum; i++) {
        if (phdr[i].type != PT_LOAD) continue;
        u64 vaddr = phdr[i].vaddr & ~(u64)(PAGE_SIZE - 1);
        u64 end = (phdr[i].vaddr + phdr[i].memsz + PAGE_SIZE - 1) & ~(u64)(PAGE_SIZE - 1);
        u64 flags = PTE_PRESENT | PTE_USER;
        if (phdr[i].flags & PF_W) flags |= PTE_WRITE;
        if (!(phdr[i].flags & PF_X)) flags |= PTE_NX;
        for (u64 va = vaddr; va < end; va += PAGE_SIZE) {
            u64 phys = pmm_alloc();
            if (!phys) { sys_free(elf_data); return (i64)ENOMEM; }
            memset((void*)(usize)phys, 0, PAGE_SIZE);
            u64 off = va - (phdr[i].vaddr & ~(u64)(PAGE_SIZE - 1));
            if (off < phdr[i].filesz) {
                usize copy = phdr[i].filesz - off;
                if (copy > PAGE_SIZE) copy = PAGE_SIZE;
                memcpy((void*)(usize)phys, elf_data + phdr[i].offset + off, copy);
            }
            vmm_map(pcb->cr3, va, phys, flags);
        }
    }
    u64 stack_top = 0x700000UL;
    for (u64 va = stack_top - PAGE_SIZE; va < stack_top; va += PAGE_SIZE) {
        u64 phys = pmm_alloc();
        if (!phys) { sys_free(elf_data); return (i64)ENOMEM; }
        memset((void*)(usize)phys, 0, PAGE_SIZE);
        vmm_map(pcb->cr3, va, phys, PTE_USER_RW);
    }
    if (pcb->vma_table) {
        VMA *vmas = (VMA*)(usize)pcb->vma_table;
        vmm_vma_add(vmas, 0x400000UL, 0x400000UL + 0x1000000UL, VMA_ANON | VMA_READ | VMA_EXEC);
        vmm_vma_add(vmas, stack_top - PAGE_SIZE, stack_top, VMA_ANON | VMA_READ | VMA_WRITE | VMA_STACK);
    }
    pcb->entry = eh->entry;
    pcb->ursp = stack_top;
    pcb->brk = BRK_BASE;
    sys_free(elf_data);
    return 0;
}

i64 sys_clone(u64 flags, void *stack, void *ptid, void *ctid, void *newtls) {
    (void)ptid; (void)ctid;
    PCB *parent = process_current_pcb();
    if (!parent) return (i64)ENOMEM;
    i64 pid = process_create(parent->entry, parent->name);
    if (pid < 0) return pid;
    u8 *p = (u8*)PROC_TABLE;
    PCB *child = NULL;
    for (int i = 0; i < PROC_MAX; i++, p += PROC_PCB_SIZE) {
        PCB *t = (PCB*)p;
        if (t->pid == (u64)pid) { child = t; break; }
    }
    if (!child) return (i64)ENOMEM;
    if (flags & CLONE_VM) {
        child->cr3 = parent->cr3;
        inode_ref((vfs_inode_t*)(usize)parent->cr3);
    } else {
        child->cr3 = vmm_create_space();
        if (!child->cr3) { child->state = PSTATE_DEAD; return (i64)ENOMEM; }
        vmm_map_kernel(child->cr3);
        vmm_cow_fork(parent->cr3, child->cr3);
    }
    void *kstack = heap_malloc(PROC_KSTACK_SZ);
    if (!kstack) { if (!(flags & CLONE_VM)) vmm_destroy(child->cr3); child->state = PSTATE_DEAD; return (i64)ENOMEM; }
    child->kbase = (u64)kstack;
    child->kstack = (u64)kstack + PROC_KSTACK_SZ;
    child->ursp = stack ? (u64)stack : parent->ursp;
    child->brk = parent->brk;
    if (newtls) {
        wrmsr((u32)MSR_FS_BASE, (u64)newtls);
    }
    child->state = PSTATE_READY;
    return pid;
}

i64 sys_wait4(u64 pid, int *wstatus, u64 options, void *ru) {
    (void)options; (void)ru;
    u8 *p = (u8*)PROC_TABLE;
    for (int i = 0; i < PROC_MAX; i++, p += PROC_PCB_SIZE) {
        PCB *t = (PCB*)p;
        if (t->state == PSTATE_EMPTY || t->state == PSTATE_DEAD) continue;
        if (t->pid == pid || pid == (u64)-1) {
            if (t->state == PSTATE_DEAD) {
                if (wstatus) *wstatus = 0;
                u64 exited_pid = t->pid;
                process_destroy(t);
                return (i64)exited_pid;
            }
            return (i64)EAGAIN;
        }
    }
    return (i64)ECHILD;
}

i64 sys_exit_group(i64 code) {
    (void)code;
    u8 *p = (u8*)PROC_TABLE;
    for (int i = 0; i < PROC_MAX; i++, p += PROC_PCB_SIZE) {
        PCB *t = (PCB*)p;
        if (t->state != PSTATE_EMPTY && t->state != PSTATE_DEAD) {
            process_destroy(t);
        }
    }
    cli();
    vmm_switch(kernel_cr3);
    __asm__ volatile(
        "mov %0, %%rsp\n\t"
        "sti\n\t"
        "ret\n\t"
        :: "m"(kernel_return_rsp)
    );
    __builtin_unreachable();
}
