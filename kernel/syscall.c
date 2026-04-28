/* ================================================================
 *  SHADOW OS — kernel/syscall.c  (PATCHED)
 *  Fix Bug 6: sys_exit_handler — wrap the entire exit sequence in
 *  cli/sti to prevent the timer ISR from firing between
 *  scheduler_exit() and the rsp restore, which would clobber rsp
 *  with a preempted-process context and cause a wild jump on ret.
 * ================================================================ */
#include "../include/kernel.h"

#define MSR_EFER   0xC0000080
#define MSR_STAR   0xC0000081
#define MSR_LSTAR  0xC0000082
#define MSR_SFMASK 0xC0000084

#define MAP_ANONYMOUS 0x20
#define MAP_FIXED     0x10

extern void syscall_entry(void);

void syscall_init(void) {
    u64 efer = rdmsr(MSR_EFER);
    wrmsr(MSR_EFER, efer | 1);
    wrmsr(MSR_STAR, ((u64)0x10 << 32) | ((u64)0x18 << 48));
    wrmsr(MSR_LSTAR, (u64)syscall_entry);
    wrmsr(MSR_SFMASK, 1UL << 9);
}

/* ── 0: sys_read ─────────────────────────────────────────────── */
/* Forward declaration — defined in kernel.c */
u8 read_key_raw(void);

i64 sys_read(u64 fd, void *buf, usize n) {
    if (fd == 0) {
        /* stdin: read one line from keyboard with echo.
         * Must STI before polling — SYSCALL clears IF, but polled
         * keyboard needs the i8042 to be responsive. */
        __asm__ volatile("sti");
        u8 *out = (u8*)buf;
        usize i = 0;
        while (i < n) {
            u8 c = read_key_raw();
            if (c == '\r' || c == '\n') {
                out[i++] = '\n';
                vga_putchar('\n');
                break;
            }
            if (c == '\b' && i > 0) {
                i--;
                vga_putchar('\b'); vga_putchar(' '); vga_putchar('\b');
                continue;
            }
            if (c < 0x20) continue;
            out[i++] = c;
            vga_putchar(c);
        }
        __asm__ volatile("cli");
        return (i64)i;
    }
    if (fd == 1 || fd == 2) return 0; /* stdout/stderr not readable */
    return vfs_read(fd, buf, n);
}

/* ── 1: sys_write ────────────────────────────────────────────── */
i64 sys_write(u64 fd, const void *buf, usize n) {
    if (fd == 1 || fd == 2) {
        /* DEBUG: bright-yellow 'W' at row 0, col 78.
         * Visible = sys_write(fd=1) was called. */
        ((u16*)0xB8000)[78] = (u16)(0x3F << 8) | 'W';
        const u8 *p = (const u8*)buf;
        for (usize i = 0; i < n; i++) vga_putchar(p[i]);
        return (i64)n;
    }
    return vfs_write(fd, buf, n);
}

/* ── 2: sys_open ─────────────────────────────────────────────── */
i64 sys_open(const char *path, u64 flags) {
    (void)flags;
    while (*path == '/') path++;
    char name83[12];
    format_83_name(path, name83);
    return vfs_open(name83);
}

/* ── 3: sys_close ────────────────────────────────────────────── */
i64 sys_close(u64 fd) {
    return vfs_close(fd);
}

/* ── 4/60/231: sys_exit ──────────────────────────────────────── */
extern void scheduler_exit(void);

i64 sys_exit_handler(i64 code) {
    (void)code;
    /* Print "Process <name> exited." using the running PCB's name */
    kprintf("\r\nProcess ");
    u8 *p = (u8*)PROC_TABLE;
    for (int i = 0; i < PROC_MAX; i++, p += PROC_PCB_SIZE) {
        PCB *t = (PCB*)p;
        if (t->state == PSTATE_RUNNING) {
            kprintf("%s", t->name[0] ? t->name : "(unknown)");
            break;
        }
    }
    kprintf(" exited.\r\n");

    /* FIX (Bug 6): disable interrupts for the entire exit sequence.
     * scheduler_exit() marks the process DEAD and frees its kstack.
     * If the timer ISR fires between scheduler_exit() and the `ret`
     * below, it will try to restore an rsp from a freed/dead PCB,
     * then iretq into garbage.  Hold cli until we are back on the
     * kernel stack and can safely sti. */
    cli();
    scheduler_exit();
    vmm_switch(kernel_cr3);

    /* Restore the kernel stack saved by process_run() and return to
     * the shell loop.  sti() is called after rsp is stable. */
    __asm__ volatile(
        "mov %0, %%rsp\n\t"
        "sti\n\t"
        "ret\n\t"
        :: "m"(kernel_return_rsp)
    );
    __builtin_unreachable();
}

/* ── 5: sys_malloc ───────────────────────────────────────────── */
/* Allocate n bytes in the calling process's address space by
 * bumping pcb->brk and backing each new page with a physical
 * frame mapped PTE_USER_RW | PTE_NX into the process's CR3.
 * Returns a user virtual address (not a kernel heap pointer). */
void *sys_malloc(usize n) {
    if (!n) return (void*)0;

    /* Align to 16 bytes */
    n = (n + 15) & ~(usize)15;

    PCB *pcb = process_current_pcb();
    if (!pcb) return (void*)0;

    /* Initialise brk on first call */
    if (pcb->brk < BRK_BASE) pcb->brk = BRK_BASE;

    u64 start = pcb->brk;
    u64 end   = start + n;

    if (end > BRK_MAX) return (void*)0;

    /* Map any new pages needed */
    u64 page_start = start & ~(u64)(PAGE_SIZE - 1);
    u64 page_end   = (end + PAGE_SIZE - 1) & ~(u64)(PAGE_SIZE - 1);

    for (u64 va = page_start; va < page_end; va += PAGE_SIZE) {
        /* Only map pages that aren't already present */
        if (!vmm_virt_to_phys(pcb->cr3, va)) {
            u64 phys = pmm_alloc();
            if (!phys) return (void*)0;
            memset((void*)phys, 0, PAGE_SIZE);
            vmm_map(pcb->cr3, va, phys, PTE_USER_RW | PTE_NX);
        }
    }

    pcb->brk = end;
    return (void*)(usize)start;
}

/* ── 6: sys_free ─────────────────────────────────────────────── */
/* Bump allocator: free is a no-op (memory reclaimed on exit). */
i64 sys_free(void *p) {
    (void)p;
    return 0;
}

/* ── 7: sys_yield ────────────────────────────────────────────── */
/* Pet the watchdog so that well-behaved event-loop processes that
 * call sys_yield() every iteration are not killed for "hanging".
 * Without this, the BROWSER (and any other yield-based userland
 * program) is terminated after WD_TIMEOUT_MS (5 s) even though it
 * is actively running its event loop. */
i64 sys_yield(void) { watchdog_pet(); return 0; }

/* ── 9: sys_mmap ─────────────────────────────────────────────── */
/* Demand-paging mmap: register a VMA, do NOT allocate physical pages.
 * Pages are faulted in lazily by vmm_page_fault() on first access.
 * Supports both anonymous (MAP_ANONYMOUS) and file-backed mappings.
 * File-backed: VMA stores fd + offset; page fault reads file data. */
static u64 mmap_bump = MMAP_BASE;

void *sys_mmap(u64 addr, usize len, u64 prot, u64 flags, u64 fd, u64 off) {
    int is_anon = (flags & MAP_ANONYMOUS) || (i64)fd < 0;

    /* Validate file fd for file-backed mappings */
    if (!is_anon) {
        extern FD fd_table[];
        if (fd >= MAX_FILES || !fd_table[fd].in_use)
            return (void*)(usize)EBADF;
    }

    len = (len + PAGE_SIZE - 1) & ~(usize)(PAGE_SIZE - 1);
    if (!len) return (void*)(usize)ENOMEM;

    u64 virt = (addr && (flags & MAP_FIXED)) ? (addr & ~(u64)(PAGE_SIZE-1))
                                              : mmap_bump;

    PCB *pcb = process_current_pcb();
    if (!pcb) return (void*)(usize)ENOMEM;

    /* Register VMA for demand paging */
    VMA *vmas = (VMA*)(usize)pcb->vma_table;
    if (vmas) {
        u32 vma_flags = is_anon ? VMA_ANON : VMA_FILE;
        if (prot & 1) vma_flags |= VMA_READ;
        if (prot & 2) vma_flags |= VMA_WRITE;
        if (prot & 4) vma_flags |= VMA_EXEC;

        /* Find free VMA slot and fill in fd/offset for file-backed */
        for (int i = 0; i < VMA_MAX; i++) {
            if (vmas[i].start == vmas[i].end) {
                vmas[i].start       = virt;
                vmas[i].end         = virt + len;
                vmas[i].flags       = vma_flags;
                vmas[i].fd          = is_anon ? -1 : (i64)fd;
                vmas[i].file_offset = is_anon ? 0  : off;
                break;
            }
        }
    }

    if (!(flags & MAP_FIXED)) mmap_bump += len;
    return (void*)virt;
}

/* ── 10: sys_mprotect ────────────────────────────────────────── */
/* Walk the page tables and update PTE permissions to match prot,
 * then update the VMA flags so future demand-paged faults use the
 * correct permissions.  Like Linux mprotect(2). */
i64 sys_mprotect(u64 addr, usize len, u64 prot) {
    if (addr & (PAGE_SIZE - 1)) return (i64)(usize)EINVAL;
    len = (len + PAGE_SIZE - 1) & ~(usize)(PAGE_SIZE - 1);
    if (!len) return 0;

    PCB *pcb = process_current_pcb();
    if (!pcb) return (i64)(usize)EINVAL;

    u64 end = addr + len;

    /* Update VMA flags */
    VMA *vmas = (VMA*)(usize)pcb->vma_table;
    if (vmas) {
        for (int i = 0; i < VMA_MAX; i++) {
            if (vmas[i].start >= vmas[i].end) continue;
            if (vmas[i].end <= addr || vmas[i].start >= end) continue;
            vmas[i].flags &= ~(u32)(VMA_READ | VMA_WRITE | VMA_EXEC);
            if (prot & 1) vmas[i].flags |= VMA_READ;
            if (prot & 2) vmas[i].flags |= VMA_WRITE;
            if (prot & 4) vmas[i].flags |= VMA_EXEC;
        }
    }

    /* Update PTEs for already-resident pages */
    for (u64 va = addr; va < end; va += PAGE_SIZE) {
        u64 *pte = vmm_pte_get(pcb->cr3, va, 0);
        if (!pte || !(*pte & PTE_PRESENT)) continue;
        /* Rewrite permission bits: keep physical address and USER */
        u64 phys = *pte & PTE_PHYS_MASK;
        u64 flags = PTE_PRESENT | PTE_USER;
        if (prot & 2) flags |= PTE_WRITE;
        *pte = phys | flags;
        vmm_invlpg(va);
    }
    return 0;
}

/* ── 11: sys_munmap ──────────────────────────────────────────── */
i64 sys_munmap(u64 addr, usize len) {
    len = (len + PAGE_SIZE - 1) & ~(usize)(PAGE_SIZE - 1);
    usize pages = len / PAGE_SIZE;
    PCB *pcb = process_current_pcb();
    if (!pcb) return -1;

    /* Remove VMA so further faults in this range fault to death */
    VMA *vmas = (VMA*)(usize)pcb->vma_table;
    if (vmas) vmm_vma_remove(vmas, addr, len);

    /* Unmap any already-faulted-in pages and release their physical frames */
    for (usize i = 0; i < pages; i++) {
        u64 va   = addr + i * PAGE_SIZE;
        u64 phys = vmm_virt_to_phys(pcb->cr3, va);
        if (phys) pmm_unref(phys & ~(u64)(PAGE_SIZE-1));
        vmm_unmap(pcb->cr3, va);
    }
    return 0;
}

/* ── 12: sys_brk ─────────────────────────────────────────────── */
/* Demand-paging brk: expand the heap VMA without allocating pages.
 * Pages are faulted in lazily on first access (like Linux sbrk). */
u64 sys_brk(u64 new_brk) {
    PCB *pcb = process_current_pcb();
    if (!pcb) return (u64)(usize)ENOMEM;

    if (new_brk == 0)                              return pcb->brk;
    if (new_brk < BRK_BASE || new_brk > BRK_MAX)  return (u64)(usize)ENOMEM;
    if (new_brk == pcb->brk)                       return pcb->brk;

    u64 old_brk_pg = (pcb->brk  + PAGE_SIZE - 1) & ~(u64)(PAGE_SIZE - 1);
    u64 new_brk_pg = (new_brk   + PAGE_SIZE - 1) & ~(u64)(PAGE_SIZE - 1);

    VMA *vmas = (VMA*)(usize)pcb->vma_table;

    if (new_brk > pcb->brk) {
        /* Grow: extend or add a heap VMA covering the new range.
         * No physical pages allocated here -- demand paging handles it. */
        if (vmas)
            vmm_vma_add(vmas, old_brk_pg, new_brk_pg,
                        VMA_ANON | VMA_READ | VMA_WRITE);
    } else {
        /* Shrink: unmap already-resident pages and trim the VMA */
        u64 cur = old_brk_pg;
        while (cur > new_brk_pg) {
            cur -= PAGE_SIZE;
            u64 phys = vmm_virt_to_phys(pcb->cr3, cur);
            if (phys) pmm_unref(phys & ~(u64)(PAGE_SIZE - 1));
            vmm_unmap(pcb->cr3, cur);
        }
        if (vmas) vmm_vma_remove(vmas, new_brk_pg, old_brk_pg - new_brk_pg);
    }
    pcb->brk = new_brk;
    return new_brk;
}

/* ── 20: sys_writev ──────────────────────────────────────────── */
typedef struct { void *base; usize len; } IOVec;
i64 sys_writev(u64 fd, const void *iov_ptr, u64 cnt) {
    const IOVec *iov = (const IOVec*)iov_ptr;
    i64 total = 0;
    for (u64 i = 0; i < cnt; i++) {
        i64 r = sys_write(fd, iov[i].base, iov[i].len);
        if (r < 0) return r;
        total += r;
    }
    return total;
}

/* ── 21: sys_stub_noent ──────────────────────────────────────── */
i64 sys_stub_noent(void) { return (i64)ENOENT; }

/* ── 39: sys_getpid ──────────────────────────────────────────── */
i64 sys_getpid(void) { return (i64)current_pid; }

/* ── 56/57: stubs ────────────────────────────────────────────── */
i64 sys_stub_enosys(void) { return (i64)ENOSYS; }

/* ── 63: sys_uname ───────────────────────────────────────────── */
typedef struct { char f[65]; } UTS;
i64 sys_uname(void *buf) {
    UTS *u = (UTS*)buf;
    const char *fields[] = {"Systrix","systrix","0.1.0","#1 SMP","x86_64",""};
    for (int i = 0; i < 6; i++) {
        memset(u[i].f, 0, 65);
        strlcpy(u[i].f, fields[i], 65);
    }
    return 0;
}

/* ── 72/80/218: generic stub returning 0 ────────────────────── */
i64 sys_stub(void) { return 0; }

/* ── 78: sys_getdents64 ──────────────────────────────────────── */
typedef struct {
    u64 ino, off;
    u16 reclen;
    u8  type;
    char name[12];
} __attribute__((packed)) Dirent64;

static u8 dents_buf[512];

i64 sys_getdents64(u64 fd, void *buf, usize count) {
    i64 nr = vfs_read(fd, dents_buf, 512);
    if (nr <= 0) return nr;

    u8 *out = (u8*)buf;
    usize written = 0;
    u64 off = 0;
    for (int i = 0; i < 16; i++) {
        u8 *e = dents_buf + i * 32;
        if (e[0] == 0) break;
        if (e[0] == 0xE5) continue;
        if (e[11] & 0x08) continue;
        usize reclen = sizeof(Dirent64);
        if (written + reclen > count) break;
        Dirent64 *d = (Dirent64*)(out + written);
        u32 clus = ((u32)e[20]<<16)|((u32)e[21]<<24)|((u32)e[26])|((u32)e[27]<<8);
        d->ino    = clus ? clus : (u64)(i+1);
        d->off    = off++;
        d->reclen = (u16)reclen;
        d->type   = (e[11] & 0x10) ? 4 : 8;
        for (int j = 0; j < 11; j++) d->name[j] = e[j];
        d->name[11] = 0;
        written += reclen;
    }
    return (i64)written;
}

/* ── 79: sys_getcwd ──────────────────────────────────────────── */
i64 sys_getcwd(char *buf, usize sz) {
    if (sz < 2) return (i64)EINVAL;
    buf[0] = '/'; buf[1] = 0;
    return 2;
}

/* ── 96: sys_gettimeofday ────────────────────────────────────── */
typedef struct { i64 sec; i64 usec; } TimeVal;
i64 sys_gettimeofday(void *tv_ptr, void *tz) {
    (void)tz;
    if (!tv_ptr) return 0;
    TimeVal *tv = (TimeVal*)tv_ptr;
    tv->sec  = (i64)(pit_ticks / 1000);
    tv->usec = (i64)((pit_ticks % 1000) * 1000);
    return 0;
}

/* ── 228: sys_clock_gettime ──────────────────────────────────── */
typedef struct { i64 tv_sec; i64 tv_nsec; } TimeSpec;
i64 sys_clock_gettime(u64 clkid, void *ts_ptr) {
    if (!ts_ptr) return (i64)EINVAL;
    TimeSpec *ts = (TimeSpec*)ts_ptr;
    /* all clocks backed by pit_ticks (ms resolution) */
    switch (clkid) {
    case 0: /* CLOCK_REALTIME          */
    case 1: /* CLOCK_MONOTONIC         */
    case 4: /* CLOCK_MONOTONIC_RAW     */
    case 5: /* CLOCK_REALTIME_COARSE   */
    case 6: /* CLOCK_MONOTONIC_COARSE  */
    case 7: /* CLOCK_BOOTTIME          */
        ts->tv_sec  = (i64)(pit_ticks / 1000);
        ts->tv_nsec = (i64)((pit_ticks % 1000) * 1000000LL);
        return 0;
    case 2: /* CLOCK_PROCESS_CPUTIME_ID */
    case 3: /* CLOCK_THREAD_CPUTIME_ID  */
        ts->tv_sec  = (i64)(pit_ticks / 1000);
        ts->tv_nsec = (i64)((pit_ticks % 1000) * 1000000LL);
        return 0;
    default:
        return (i64)EINVAL;
    }
}

/* ── 263: sys_clock_getres ───────────────────────────────────── */
i64 sys_clock_getres(u64 clkid, void *ts_ptr) {
    (void)clkid;
    if (!ts_ptr) return 0;
    TimeSpec *ts = (TimeSpec*)ts_ptr;
    ts->tv_sec  = 0;
    ts->tv_nsec = 1000000LL; /* 1 ms resolution */
    return 0;
}

/* ── 102: sys_getuid ─────────────────────────────────────────── */
i64 sys_getuid(void) { return 0; }

/* ── 104: sys_getgid ─────────────────────────────────────────── */
i64 sys_getgid(void) { return 0; }

/* ── 158: sys_arch_prctl ─────────────────────────────────────── */
i64 sys_arch_prctl(u64 code, u64 addr) {
    if (code == ARCH_SET_FS) {
        wrmsr((u32)MSR_FS_BASE, addr);
        return 0;
    }
    if (code == ARCH_GET_FS) {
        *(u64*)addr = rdmsr((u32)MSR_FS_BASE);
        return 0;
    }
    return (i64)EINVAL;
}

/* ── 257: sys_openat ─────────────────────────────────────────── */
i64 sys_openat(u64 dirfd, const char *path, u64 flags, u64 mode) {
    (void)dirfd; (void)mode;
    return sys_open(path, flags);
}

/* ── 262: sys_fstatat ────────────────────────────────────────── */
typedef struct {
    u64 dev, ino, nlink;
    u32 mode, uid, gid, _pad0;
    u64 rdev, size, blksize, blocks;
    u64 atime, atimensec, mtime, mtimensec, ctime, ctimensec;
} StatBuf;

i64 sys_fstatat(u64 dirfd, const char *path, void *statbuf, u64 flags) {
    (void)dirfd; (void)flags;
    const char *p = path;
    while (*p == '/') p++;
    char name83[12];
    format_83_name(p, name83);
    u32 fsize;
    if (fat32_find_file(name83, &fsize) < 0) return (i64)ENOENT;
    StatBuf *sb = (StatBuf*)statbuf;
    memset(sb, 0, sizeof(*sb));
    sb->dev    = 1;
    sb->ino    = 1;
    sb->nlink  = 1;
    sb->mode   = 0x81A4;
    sb->size   = fsize;
    sb->blksize= 512;
    return 0;
}

/* ================================================================
 *  POSIX file API extensions + Timer + Mouse-grab
 * ================================================================ */

/* ── sys_fstat ──────────────────────────────────────────────── */
i64 sys_fstat(u64 fd, void *statbuf) {
    if (fd < 3) {                  /* stdin/stdout/stderr */
        StatBuf *sb = (StatBuf*)statbuf;
        memset(sb, 0, sizeof(*sb));
        sb->mode = 0x2180;         /* S_IFCHR | 0600 */
        return 0;
    }
    /* Walk the open FD table to find the filename, then stat */
    extern FD fd_table[];          /* defined in kernel.c */
    if (fd >= MAX_FILES) return (i64)EBADF;
    FD *f = &fd_table[fd];
    if (!f->in_use) return (i64)EBADF;
    StatBuf *sb = (StatBuf*)statbuf;
    memset(sb, 0, sizeof(*sb));
    sb->dev     = 1;
    sb->ino     = f->start_clus ? f->start_clus : 1;
    sb->nlink   = 1;
    sb->mode    = 0x81A4;          /* S_IFREG | 0644 */
    sb->size    = f->size;
    sb->blksize = 512;
    return 0;
}

/* ── 8: sys_lseek  (Linux x86-64 = 62, we also alias to 8's gap) */
i64 sys_lseek(u64 fd, i64 offset, u64 whence) {
    if (fd < 3) return (i64)EINVAL;
    extern FD fd_table[];
    if (fd >= MAX_FILES) return (i64)EBADF;
    FD *f = &fd_table[fd];
    if (!f->in_use) return (i64)EBADF;
    u32 new_pos;
    if (whence == 0)       new_pos = (u32)offset;                 /* SEEK_SET */
    else if (whence == 1)  new_pos = f->pos + (u32)offset;        /* SEEK_CUR */
    else if (whence == 2)  new_pos = f->size + (u32)offset;       /* SEEK_END */
    else return (i64)EINVAL;
    if (new_pos > f->size) new_pos = f->size;
    vfs_seek(fd, new_pos, 0);
    return (i64)new_pos;
}

/* ── 32: sys_dup ──────────────────────────────────────────────── */
i64 sys_dup(u64 oldfd) {
    if (oldfd < 3) return (i64)oldfd;    /* stdin/stdout/stderr dup = same */
    extern FD fd_table[];
    if (oldfd >= MAX_FILES) return (i64)EBADF;
    FD *src = &fd_table[oldfd];
    if (!src->in_use) return (i64)EBADF;
    /* Find a free slot */
    for (u64 newfd = 3; newfd < MAX_FILES; newfd++) {
        if (!fd_table[newfd].in_use) {
            fd_table[newfd] = *src;      /* copy the entire FD struct */
            return (i64)newfd;
        }
    }
    return (i64)ENOMEM;
}

/* ── 62: sys_lseek (Linux canonical number) ──────────────────── */
/* Alias — same implementation, different slot number */
i64 sys_lseek62(u64 fd, i64 offset, u64 whence) {
    return sys_lseek(fd, offset, whence);
}

/* ── 82: sys_rename ───────────────────────────────────────────── */
i64 sys_rename(const char *oldpath, const char *newpath) {
    const char *op = oldpath; while (*op == '/') op++;
    const char *np = newpath; while (*np == '/') np++;
    char old83[12], new83[12];
    format_83_name(op, old83);
    format_83_name(np, new83);
    return fat32_rename(old83, new83);
}

/* ── 83: sys_mkdir ────────────────────────────────────────────── */
i64 sys_mkdir(const char *path, u64 mode) {
    (void)mode;
    const char *p = path; while (*p == '/') p++;
    char name83[12];
    format_83_name(p, name83);
    u32 clus = fat32_alloc_cluster();
    if (!clus) return (i64)ENOMEM;
    fat32_create_dir_entry(name83, clus);
    return 0;
}

/* ── 84: sys_rmdir ────────────────────────────────────────────── */
i64 sys_rmdir(const char *path) {
    const char *p = path; while (*p == '/') p++;
    char name83[12];
    format_83_name(p, name83);
    return fat32_delete_file(name83);   /* FAT32: dir deletion = file deletion */
}

/* ── 87: sys_unlink ───────────────────────────────────────────── */
i64 sys_unlink(const char *path) {
    const char *p = path; while (*p == '/') p++;
    char name83[12];
    format_83_name(p, name83);
    return fat32_delete_file(name83);
}

/* ── 327: sys_gettime_ms ──────────────────────────────────────── */
/* Returns milliseconds since boot.  With PIT at 1000 Hz,
 * pit_ticks increments once per millisecond — direct mapping.    */
i64 sys_gettime_ms(void) {
    return (i64)pit_ticks;
}

/* ── 328: sys_mouse_setmode ───────────────────────────────────── */
/* mode 0 = absolute (default GUI cursor)
 * mode 1 = relative / grabbed (FPS mouse-look)
 * In relative mode the kernel resets accumulated delta each read
 * and the GUI cursor is hidden.                                   */
void input_set_mouse_grab(int grabbed);   /* defined in input.c   */

i64 sys_mouse_setmode(u64 mode) {
    input_set_mouse_grab((int)(mode & 1));
    return 0;
}

/* ── 333: sys_net_dns — resolve hostname → IPv4 address ──────── */
/* Wraps kernel net_dns_resolve() for userspace browsers/apps.
 * arg0 (rdi): pointer to null-terminated hostname string
 * Returns: IPv4 address in network byte order, or 0 on failure.  */
u32 net_dns_resolve(const char *hostname);   /* defined in net.c   */

i64 sys_net_dns(const char *hostname) {
    if (!hostname) return 0;
    return (i64)(u64)net_dns_resolve(hostname);
}

i64 sys_watchdog_pet(void) {
    watchdog_pet();
    return 0;
}

/* ================================================================
 *  POSIX syscalls needed for Lynx / browser support
 * ================================================================ */

/* ── 6: sys_lstat ─────────────────────────────────────────────── */
/* lstat = stat for us — no symlinks on FAT32                     */
i64 sys_lstat(const char *path, void *statbuf) {
    if (!path || !statbuf) return (i64)EINVAL;
    return vfs_stat(path, statbuf);
}

/* ── 16: sys_ioctl ────────────────────────────────────────────── */
/* Handle the terminal ioctls that Lynx and libc probe at startup */
#define TCGETS      0x5401
#define TCSETS      0x5402
#define TCSETSW     0x5403
#define TCSETSF     0x5404
#define TIOCGWINSZ  0x5413
#define TIOCSWINSZ  0x5414
#define TIOCGPGRP   0x540F
#define TIOCSPGRP   0x5410
#define FIONREAD    0x541B
#define FIONBIO     0x5421

typedef struct { u32 c_iflag, c_oflag, c_cflag, c_lflag;
                 u8  c_cc[19]; u8 _pad; u32 c_ispeed, c_ospeed; } Termios;
typedef struct { u16 ws_row, ws_col, ws_xpixel, ws_ypixel; } Winsize;

i64 sys_ioctl(u64 fd, u64 req, void *arg) {
    (void)fd;
    switch (req) {
    case TCGETS:
        if (arg) {
            Termios *t = (Termios*)arg;
            memset(t, 0, sizeof(*t));
            t->c_iflag = 0x500;   /* ICRNL | IXON */
            t->c_oflag = 0x5;     /* OPOST | ONLCR */
            t->c_cflag = 0xBF;    /* CS8 | CREAD | HUPCL */
            t->c_lflag = 0x8A3B;  /* ISIG | ICANON | ECHO | ECHOE | ECHOK | ECHOCTL | ECHOKE */
            t->c_ispeed = 38400;
            t->c_ospeed = 38400;
        }
        return 0;
    case TCSETS: case TCSETSW: case TCSETSF:
        return 0;   /* accept any terminal mode set */
    case TIOCGWINSZ:
        if (arg) {
            Winsize *w = (Winsize*)arg;
            w->ws_row    = 25;
            w->ws_col    = 80;
            w->ws_xpixel = 640;
            w->ws_ypixel = 400;
        }
        return 0;
    case TIOCSWINSZ:
        return 0;
    case TIOCGPGRP:
        if (arg) *(int*)arg = 1;
        return 0;
    case TIOCSPGRP:
        return 0;
    case FIONREAD:
        if (arg) *(int*)arg = 0;
        return 0;
    case FIONBIO:
        /* set non-blocking on socket fd if arg != 0 */
        if (fd < 64) {
            extern i64 sys_fcntl(u64, u64, u64);
            sys_fcntl(fd, 4, arg ? (*(int*)arg ? 0x800 : 0) : 0);
        }
        return 0;
    default:
        return (i64)EINVAL;
    }
}

/* ── 21: sys_access ───────────────────────────────────────────── */
i64 sys_access(const char *path, int mode) {
    (void)mode;  /* FAT32 has no permission bits — if file exists, allow */
    if (!path) return (i64)EINVAL;
    /* Special always-present paths */
    if (strcmp(path, "/dev/tty") == 0) return 0;
    if (strcmp(path, "/dev/null") == 0) return 0;
    if (strcmp(path, "/dev/zero") == 0) return 0;
    if (strcmp(path, "/proc/self/exe") == 0) return 0;
    StatBuf sb;
    return vfs_stat(path, &sb);   /* 0 = exists, negative = not found */
}

/* ── 35: sys_nanosleep ────────────────────────────────────────── */
i64 sys_nanosleep(const void *req_ptr, void *rem_ptr) {
    if (!req_ptr) return (i64)EINVAL;
    const TimeSpec *req = (const TimeSpec*)req_ptr;
    TimeSpec       *rem = (TimeSpec*)rem_ptr;
    /* Convert to milliseconds — PIT runs at 1000 Hz */
    u64 ms = (u64)(req->tv_sec) * 1000 + (u64)(req->tv_nsec) / 1000000;
    if (ms == 0) ms = 1;   /* always yield at least once */
    u64 deadline = pit_ticks + ms;
    while (pit_ticks < deadline) {
        extern void watchdog_pet(void);
        watchdog_pet();
        __asm__ volatile("pause");
    }
    if (rem) { rem->tv_sec = 0; rem->tv_nsec = 0; }
    return 0;
}

/* ── 80: sys_chdir ────────────────────────────────────────────── */
/* kernel.c owns cwd_path / cwd_clus — expose them via externs    */
extern char cwd_path[128];
extern u32  cwd_clus;
extern u32  fat32_root_clus;

i64 sys_chdir(const char *path) {
    if (!path) return (i64)EINVAL;
    /* Root shortcut */
    if (path[0] == '/' && path[1] == '\0') {
        cwd_clus = 0;
        cwd_path[0] = '/'; cwd_path[1] = '\0';
        return 0;
    }
    /* Check the directory actually exists */
    StatBuf sb;
    i64 r = vfs_stat(path, &sb);
    if (r < 0) return r;
    /* Update cwd_path */
    char new_path[128];
    if (slibc_path_join(new_path, sizeof(new_path), cwd_path, path) < 0)
        return (i64)ENAMETOOLONG;
    slibc_path_normalize(new_path);
    strlcpy(cwd_path, new_path, sizeof(cwd_path));
    /* cwd_clus: let vfs resolve — set to 0 (root) and let fat layer walk */
    if (strcmp(cwd_path, "/") == 0) cwd_clus = 0;
    return 0;
}

/* ── 89: sys_readlink ─────────────────────────────────────────── */
/* FAT32 has no symlinks — return EINVAL except for known virtual ones */
i64 sys_readlink(const char *path, char *buf, usize bufsz) {
    if (!path || !buf || bufsz == 0) return (i64)EINVAL;
    /* /proc/self/exe — return a plausible path */
    if (strcmp(path, "/proc/self/exe") == 0) {
        usize n = strlcpy(buf, "/bin/lynx", bufsz);
        return (i64)(n < bufsz ? n : bufsz - 1);
    }
    return (i64)EINVAL;   /* no symlinks */
}

/* ── 99: sys_sysinfo ──────────────────────────────────────────── */
typedef struct {
    i64  uptime;
    u64  loads[3];
    u64  totalram, freeram, sharedram, bufferram;
    u64  totalswap, freeswap;
    u16  procs;
    u64  totalhigh, freehigh;
    u32  mem_unit;
    u8   _pad[20];
} SysInfo;

i64 sys_sysinfo(void *info_ptr) {
    if (!info_ptr) return (i64)EINVAL;
    SysInfo *info = (SysInfo*)info_ptr;
    memset(info, 0, sizeof(*info));
    info->uptime    = (i64)(pit_ticks / 1000);
    info->totalram  = 64 * 1024 * 1024;   /* report 64 MB */
    info->freeram   = 32 * 1024 * 1024;   /* conservative estimate */
    info->procs     = 1;
    info->mem_unit  = 1;
    return 0;
}

/* ── 110: sys_getppid ─────────────────────────────────────────── */
i64 sys_getppid(void) { return 1; }   /* init is always parent */

/* ── 4: sys_stat (Linux compat — same as fstatat with AT_FDCWD) ─ */
i64 sys_stat(const char *path, void *statbuf) {
    if (!path || !statbuf) return (i64)EINVAL;
    /* Handle virtual /proc and /dev paths */
    if (strcmp(path, "/dev/tty")  == 0 ||
        strcmp(path, "/dev/null") == 0 ||
        strcmp(path, "/dev/zero") == 0) {
        StatBuf *sb = (StatBuf*)statbuf;
        memset(sb, 0, sizeof(*sb));
        sb->mode = 0020666;   /* S_IFCHR | rw-rw-rw- */
        sb->rdev = 5;
        return 0;
    }
    if (slibc_str_starts_with(path, "/proc/")) {
        StatBuf *sb = (StatBuf*)statbuf;
        memset(sb, 0, sizeof(*sb));
        sb->mode = 0040555;   /* S_IFDIR | r-xr-xr-x */
        return 0;
    }
    return vfs_stat(path, statbuf);
}

/* ── Updated sys_getcwd — return real cwd_path ────────────────── */
/* Replaces the stub that always returned "/" */
i64 sys_getcwd_real(char *buf, usize sz) {
    if (!buf || sz < 2) return (i64)EINVAL;
    usize len = strlen(cwd_path);
    if (len + 1 > sz) return (i64)ERANGE;
    strlcpy(buf, cwd_path, sz);
    return (i64)(len + 1);
}

/* ── 334: sys_vga_clear — clear screen and reset kernel cursor ── */
/* Calls the existing vga_clear() which writes blanks to 0xB8000  */
/* and resets cur_row/cur_col to 0, so vga_putchar starts fresh.  */
void vga_clear(void);   /* defined in kernel.c */
i64  sys_vga_clear(void) {
    vga_clear();
    /* DEBUG: bright-green 'C' at top-right (row 0, col 79).
     * Visible = sys_vga_clear ran. Disappears = something clears after. */
    ((u16*)0xB8000)[79] = (u16)(0x2F << 8) | 'C';
    return 0;
}
