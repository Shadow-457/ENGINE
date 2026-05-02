#pragma once
/* ================================================================
 *  SHADOW OS — include/kernel.h
 *  All shared types, constants, and function declarations.
 *  Every .c file includes only this header.
 * ================================================================ */

/* ── Basic types ─────────────────────────────────────────────── */
#define _SYSTRIX_KERNEL_TYPES_DEFINED
typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;
typedef signed   long long i64;
typedef u64                usize;

/* syslibc stdint aliases — needed before systrix_libc.h is pulled in */
typedef u8   uint8_t;
typedef u16  uint16_t;
typedef u32  uint32_t;
typedef u64  uint64_t;
typedef signed char       int8_t;
typedef signed short      int16_t;
typedef signed int        int32_t;
typedef signed long long  int64_t;
typedef u64               size_t;
typedef i64               ssize_t;
typedef u64               uintptr_t;
typedef i64               intptr_t;
typedef i64               ptrdiff_t;

#define NULL  ((void*)0)
#define true  1
#define false 0

/* ── Memory layout constants ─────────────────────────────────── */
#define HEAP_BASE     0x200000UL   /* 2 MB */
#define HEAP_SIZE     0x200000UL   /* 2 MB */
#define PMM_BITMAP    0x1000000UL   /* 16 MB */
#define PMM_BMP_SZ    8192
#define PAGE_SIZE     4096
#define RAM_START     0x400000UL

/* RAM_END is set at runtime from E820 — use ram_end_actual.
 * RAM_END_MAX is the compile-time ceiling for static array sizing.
 * Raised to 64 GB so the buddy bitmap and refcount arrays cover the
 * full 64-bit physical address space seen in practice on x86-64. */
#define RAM_END_MAX   0x40000000UL  /* 1 GB ceiling */
extern u64 ram_end_actual;            /* set by pmm_init()           */
#define RAM_END       ram_end_actual
/* TOTAL_PAGES: compile-time upper bound for static sizing only.
 * Use (ram_end_actual - RAM_START) / PAGE_SIZE for runtime counts. */
#define TOTAL_PAGES   ((RAM_END_MAX - RAM_START) / PAGE_SIZE)

/* ── E820 BIOS memory map ────────────────────────────────────── */
/* Written by boot.S at 0x0500 before entering protected mode.   */
#define E820_MAP_ADDR  0x0500UL
#define E820_MAX       32
typedef struct {
    u64 base;
    u64 len;
    u32 type;    /* 1=usable, 2=reserved, 3=ACPI reclaimable         */
    u32 acpi;
} __attribute__((packed)) E820Entry;
#define E820_USABLE  1

#define IDT_BASE      0x500000UL
#define IDT_ENTRIES   256
#define PROC_TABLE    0x300000UL

/* ── VGA ─────────────────────────────────────────────────────── */
#define VGA_BASE      0xB8000UL
#define VGA_COLS      80
#define VGA_ROWS      25
#define VGA_ATTR      0x0F        /* white on black */

/* ── Framebuffer / GUI ──────────────────────────────────────── */
#define FB_KERNEL_VA  0xA0000000UL  /* kernel virtual addr for framebuffer */

/* ── ATA PIO ports ───────────────────────────────────────────── */
#define ATA_DATA      0x1F0
#define ATA_COUNT     0x1F2
#define ATA_LBA_LO    0x1F3
#define ATA_LBA_MID   0x1F4
#define ATA_LBA_HI    0x1F5
#define ATA_DRIVE     0x1F6
#define ATA_CMD       0x1F7
#define ATA_STATUS    0x1F7
#define ATA_CMD_READ  0x20
#define ATA_CMD_WRITE 0x30
#define ATA_CMD_FLUSH 0xE7

/* ── FAT32 partition start (LBA 512 = 256KB offset in disk) ─── */
#define PART_START    512
#define MAX_FILES     8

/* ── Process constants ───────────────────────────────────────── */
#define PROC_PCB_SIZE  128
#define PROC_MAX       64
#define PROC_KSTACK_SZ 4096
#define PROC_STACK_TOP 0x2000000UL
#define BRK_BASE       0x4000000UL     /* 64 MB */
#define BRK_MAX        0x8000000UL     /* 128 MB */
#define MMAP_BASE      0x8000000UL     /* 128 MB */

#define PSTATE_EMPTY   0
#define PSTATE_READY   1
#define PSTATE_RUNNING 2
#define PSTATE_DEAD    3
#define PSTATE_BLOCKED 4   /* sleeping on futex — scheduler skips */

/* PCB field offsets (must match isr.S exactly) */
#define PCB_STATE   0
#define PCB_KSTACK  8
#define PCB_URSP    16
#define PCB_ENTRY   24
#define PCB_PID     32
#define PCB_CR3     40
#define PCB_BRK     48
#define PCB_NAME    56
#define PCB_KBASE   72

/* Forward declarations for VFS */
typedef struct vfs_inode vfs_inode_t;
typedef struct vfs_mount vfs_mount_t;
typedef struct vfs_fd vfs_fd_t;

/* ── VFS core types (shared by vfs.c, kernel.c, jfs.c) ─────── */

typedef struct {
    u32 uid;
    u32 gid;
    u16 mode;
    u16 type;
    u64 size;
    u64 ino;
    u64 nlink;
    u64 atime;
    u64 mtime;
    u64 ctime;
    u32 dev;
    u32 rdev;
    u64 blocks;
    u64 blksize;
} inode_attr_t;

typedef struct inode_ops {
    i64 (*read)(struct vfs_inode *inode, void *buf, usize count, usize offset);
    i64 (*write)(struct vfs_inode *inode, const void *buf, usize count, usize offset);
    i64 (*readdir)(struct vfs_inode *inode, void *buf, usize count, usize *offset);
    i64 (*lookup)(struct vfs_inode *dir, const char *name, struct vfs_inode *out);
    i64 (*create)(struct vfs_inode *dir, const char *name, u16 mode, struct vfs_inode *out);
    i64 (*mkdir)(struct vfs_inode *dir, const char *name, u16 mode);
    i64 (*unlink)(struct vfs_inode *dir, const char *name);
    i64 (*rmdir)(struct vfs_inode *dir, const char *name);
    i64 (*setattr)(struct vfs_inode *inode, inode_attr_t *attr);
    i64 (*getsize)(struct vfs_inode *inode);
} inode_ops_t;

struct vfs_inode {
    u8 valid;
    u64 ino;
    u32 dev;
    u32 rdev;
    u32 refcount;
    inode_attr_t attr;
    inode_ops_t *ops;
    void *fs_data;
    struct vfs_mount *mount;
};

struct vfs_mount {
    u8 valid;
    char path[256];
    u32 dev;
    vfs_inode_t root;
    const char *fstype;
};

struct vfs_fd {
    u8 in_use;
    vfs_inode_t *inode;
    usize offset;
    u32 flags;
    int type;
    void *pipe_data;
};

/* Signal types */
#define SIG_DFL ((void*)0)
#define SIG_IGN ((void*)1)
#define SIG_ERR ((void*)-1)

#define SIGHUP    1
#define SIGINT    2
#define SIGQUIT   3
#define SIGILL    4
#define SIGTRAP   5
#define SIGABRT   6
#define SIGBUS    7
#define SIGFPE    8
#define SIGKILL   9
#define SIGUSR1   10
#define SIGSEGV   11
#define SIGUSR2   12
#define SIGPIPE   13
#define SIGALRM   14
#define SIGTERM   15
#define SIGCHLD   17
#define SIGCONT   18
#define SIGSTOP   19
#define SIGTSTP   20

typedef struct {
    void *handler;
    u64 mask;
    int flags;
} sigaction_t;

typedef struct {
    u64 pending;
    u64 blocked;
    sigaction_t actions[32];
    u64 siginfo[32];
} signal_state_t;

/* ── PTE flags ───────────────────────────────────────────────── */
#define PTE_PRESENT  (1UL << 0)
#define PTE_WRITE    (1UL << 1)
#define PTE_USER     (1UL << 2)
#define PTE_NX       (1UL << 63)
#define PTE_PHYS_MASK 0x000FFFFFFFFFF000UL
#define PTE_KERNEL_RW (PTE_PRESENT | PTE_WRITE)
#define PTE_USER_RW   (PTE_PRESENT | PTE_WRITE | PTE_USER)
/* Executable user page: USER + PRESENT, no WRITE, no NX */
#define PTE_USER_RX   (PTE_PRESENT | PTE_USER)

/* ── Segment selectors ───────────────────────────────────────── */
#define SEL_KERNEL_CS  0x10   /* GDT[2] ring-0 code64  */
#define SEL_KERNEL_SS  0x18   /* GDT[3] ring-0 data64  */
#define SEL_USER_SS    0x23   /* GDT[4] ring-3 data64 | RPL=3 */
#define SEL_USER_CS    0x2b   /* GDT[5] ring-3 code64 | RPL=3 */
#define SEL_TSS        0x30   /* GDT[6] TSS descriptor (16-byte system desc) */

/* ── ELF64 ───────────────────────────────────────────────────── */
#define ELF_MAGIC    0x464C457FUL
#define ET_EXEC      2
#define EM_X86_64    62
#define ELFCLASS64   2
#define PT_LOAD      1
/* ELF program header p_flags bits */
#define PF_X  1   /* segment is executable */
#define PF_W  2   /* segment is writable   */
#define PF_R  4   /* segment is readable   */

/* ── Linux-compatible error codes ───────────────────────────── */
/* Set the guard so systrix_libc.h's errno block is skipped when
 * this kernel header is included first (it already provides these). */
#define _SYSTRIX_ERRNO_DEFINED
#define EPERM    (-1LL)
#define ENOENT   (-2LL)
#define EBADF    (-9LL)
#define ENOMEM   (-12LL)
#define EFAULT   (-14LL)
#define EINVAL   (-22LL)
#define ENOSYS   (-38LL)
#define ERANGE   (-34LL)

/* ── arch_prctl codes ─────────────────────────────────────────── */
#define ARCH_SET_FS  0x1002
#define ARCH_GET_FS  0x1003
#define MSR_FS_BASE  0xC0000100UL

/* ── PIC / PIT ───────────────────────────────────────────────── */
#define PIC1_CMD    0x20
#define PIC1_DATA   0x21
#define PIC2_CMD    0xA0
#define PIC2_DATA   0xA1
#define PIC_EOI     0x20
#define PIT_CMD     0x43
#define PIT_CH0     0x40
#define PIT_DIV     1193    /* 1193182 / 1193 ≈ 1000 Hz → 1ms per tick */
#define KERNEL_CS   0x10

/* ── PCB struct (mirrors layout in isr.S) ───────────────────── */
typedef struct {
    u64 state;       /* 0  */
    u64 kstack;      /* 8  */
    u64 ursp;        /* 16 */
    u64 entry;       /* 24 */
    u64 pid;         /* 32 */
    u64 cr3;         /* 40 */
    u64 brk;         /* 48 */
    char name[16];   /* 56 */
    u64 kbase;       /* 72 */
    u64 vma_table;   /* 80: pointer to VMA array (heap-allocated) */
    u8  _pad[40];    /* 88..127 */
} __attribute__((packed)) PCB;

/* ── fd_table slot (32 bytes per entry) ─────────────────────── */
typedef struct {
    u8  in_use;      /* 0  */
    u8  _pad1[3];
    u32 size;        /* 4  */
    u32 start_clus;  /* 8  */
    u32 cur_clus;    /* 12 */
    u32 pos;         /* 16 */
    u8  name83[11];  /* 20..30  — 8.3 name for patch lookups */
    u8  _pad2[1];    /* 31 */
    u32 dir_clus;    /* 32 — cluster of parent directory */
} __attribute__((packed)) FD;

/* ── ELF64 header ────────────────────────────────────────────── */
typedef struct {
    u32 magic;
    u8  cls, data, version, os_abi;
    u8  pad[8];
    u16 type, machine;
    u32 version2;
    u64 entry;
    u64 phoff;
    u64 shoff;
    u32 flags;
    u16 ehsize, phentsize, phnum;
    u16 shentsize, shnum, shstrndx;
} __attribute__((packed)) Elf64Hdr;

typedef struct {
    u32 type, flags;
    u64 offset, vaddr, paddr, filesz, memsz, align;
} __attribute__((packed)) Elf64Phdr;

/* == Function declarations == */

/* heap.c */
void  heap_init(void);
void *heap_malloc(usize n);
void  heap_free(void *p);
void *heap_realloc(void *ptr, usize n);

/* heap_enhanced.c — Production-level heap with slab defrag, best-fit, poisoning */
void  heap_enhanced_init(void);
void *heap_enhanced_malloc(usize n);
void  heap_enhanced_free(void *p);
void *heap_enhanced_realloc(void *ptr, usize n);
void  heap_print_stats(void);
void  heap_defragment(void);
int   heap_check_integrity(void);

/* pmm.c */
void  pmm_init(void);
u64   pmm_alloc(void);
u64   pmm_alloc_order(u32 order);
void  pmm_free(u64 phys);
void  pmm_free_order(u64 phys, u32 order);
u32   pmm_free_pages(void);
u64   pmm_alloc_n(usize n);
void  pmm_clear_region(u64 base, usize size);
void  pmm_ref(u64 phys);
u8    pmm_unref(u64 phys);
u8    pmm_refcount(u64 phys);

/* pmm_enhanced.c — Production-level PMM with stats, defrag, zones */
typedef struct PmmStats {
    u64 total_allocations;
    u64 total_frees;
    u64 total_pages_allocated;
    u64 total_pages_freed;
    u64 peak_allocated;
    u64 current_allocated;
    u64 current_free;
    u64 fragmentation_index;
    u32 alloc_per_order[11];
    u32 free_per_order[11];
    u64 failed_allocations;
    u64 compaction_runs;
    u64 pages_compacted;
} PmmStats;
typedef struct MemoryZone {
    u64  start_pfn;
    u64  end_pfn;
    u64  free_pages;
    u64  managed_pages;
    u32  watermark_min;
    u32  watermark_high;
    char name[16];
} MemoryZone;
void  pmm_enhanced_init(void);
void  pmm_get_stats(PmmStats *out);
void  pmm_print_stats(void);
u64   pmm_defragment(void);
int   pmm_watermark_ok(u32 order);
u64   pmm_alloc_aligned(u32 n, u32 align_pages);
u64   pmm_alloc_zone(u32 zone_id, u32 order);
void  pmm_free_zone(u64 phys, u32 zone_id, u32 order);
void  pmm_poison_page(u64 phys);
int   pmm_check_poison(u64 phys);
void  pmm_get_zone_info(u32 zone_id, MemoryZone *out);
int   pmm_is_managed(u64 phys);
u32   pmm_max_contiguous_order(void);
void  pmm_dump_freelist(void);

/* VMA type — must be before vmm_enhanced declarations */
typedef struct {
    u64 start;
    u64 end;
    u32 flags;
    i64 fd;          /* -1 for anonymous, >=0 for file-backed */
    u64 file_offset; /* byte offset into file for first page   */
} VMA;
#define VMA_READ   (1<<0)
#define VMA_WRITE  (1<<1)
#define VMA_EXEC   (1<<2)
#define VMA_ANON   (1<<3)
#define VMA_STACK  (1<<4)
#define VMA_FILE   (1<<5)   /* file-backed mapping */
#define VMA_MAX    32

/* vmm.c */
u64  vmm_create_space(void);
void vmm_map(u64 cr3, u64 virt, u64 phys, u64 flags);
void vmm_unmap(u64 cr3, u64 virt);
void vmm_switch(u64 cr3);
void vmm_destroy(u64 cr3);
void vmm_map_kernel(u64 cr3);
void vmm_init_kernel(void);
void vmm_invlpg(u64 virt);
u64  vmm_virt_to_phys(u64 cr3, u64 virt);
int  vmm_page_fault(u64 fault_addr, u64 error_code);
void vmm_cow_fork(u64 parent_cr3, u64 child_cr3);
void vmm_oom_kill(void);

/* vmm_enhanced.c — Production-level VMM with ASLR, huge pages, enhanced VMA */
u64  vmm_aslr_mmap_base(void);
u64  vmm_aslr_stack_base(void);
u64  vmm_aslr_brk_base(void);
int  vmm_vma_split(VMA *table, u64 addr, u32 new_flags);
int  vmm_vma_add_enhanced(VMA *table, u64 start, u64 end, u32 flags);
u64  vmm_vma_fragmentation(VMA *table);
void vmm_print_vmas(VMA *table);
u64  vmm_alloc_huge_page(u64 cr3, u64 virt, u64 flags);
void vmm_free_huge_page(u64 cr3, u64 virt);
int  vmm_thp_promote(u64 cr3, u64 virt_start, u64 virt_end);
int  vmm_is_huge_page(u64 cr3, u64 virt);
void vmm_huge_page_stats(void);
int  vmm_page_fault_enhanced(u64 fault_addr, u64 error_code);
int  vmm_mprotect_enhanced(PCB *pcb, u64 addr, u64 len, u64 prot);
int  vmm_add_guard_page(PCB *pcb);
void vmm_enhanced_init(void);

/* vmalloc.c — Kernel virtual memory allocator (like Linux vmalloc) */
void  vmalloc_init(void);
void *vmalloc(u64 size, const char *tag);
void  vfree(void *addr);
void *vzalloc(u64 size, const char *tag);
void *vrealloc(void *old, u64 new_size, const char *tag);
u64   vmalloc_to_phys(void *addr);
void  vmalloc_print_stats(void);
void  vmalloc_dump_leaks(void);
void *vmalloc_map_mmio(u64 phys, u64 size, const char *tag);
void  vmalloc_unmap_mmio(void *addr);

/* mem_safety.c — Memory safety features */
void mem_safety_init(void);
void mem_safety_track(u64 addr, u64 size, const char *tag);
void mem_safety_untrack(u64 addr);
void mem_safety_dump_leaks(void);
void mem_safety_tick(void);
void mem_safety_redzone_fill(void *ptr, u64 size);
int  mem_safety_redzone_check(void *ptr, u64 size);
void mem_safety_quarantine_add(void *ptr, u64 size);
int  mem_safety_quarantine_process(void);
int  mem_safety_in_quarantine(u64 addr);
int  mem_safety_is_freed(u64 addr);
int  mem_safety_validate_free(void *ptr);
int  mem_safety_valid_kptr(const void *ptr, usize size);
int  mem_safety_valid_uptr(const void *ptr, usize size);
i64  mem_safety_memcpy(void *dst, const void *src, usize n);
void mem_safety_print_stats(void);

/* ── resilience.c — SMP, OOM, panic, watchdog ───────────────── */
void smp_init(void);
void smp_dispatch(void (*fn)(int cpu_id));
extern volatile int smp_cores_up;
extern volatile int smp_total_cpus;
void oom_kill(void);
void kernel_panic(const char *reason);
void watchdog_init(void);
void watchdog_pet(void);
void watchdog_suspend(void);
void watchdog_resume(void);
void watchdog_tick(void);
u64 *vmm_pte_get(u64 cr3, u64 virt, int alloc);
extern u64 kernel_cr3;

/* VMA management */
VMA *vmm_vma_alloc(void);
void vmm_vma_free(VMA *table);
int  vmm_vma_add(VMA *table, u64 start, u64 end, u32 flags);
void vmm_vma_remove(VMA *table, u64 addr, u64 len);
VMA *vmm_find_vma(VMA *table, u64 addr);

/* process.c */
void process_init(void);
i64  process_create(u64 entry, const char *name);
void process_run(u64 pid);
PCB *process_current_pcb(void);
void ps_list(void);
void process_destroy(PCB *t);
extern u64 kernel_return_rsp;

/* elf.c */
i64  elf_load(void *data, usize size, const char *name);

/* scheduler.c */
void scheduler_init(void);
void scheduler_start(void);
extern u64 current_pid;
extern volatile u64 pit_ticks;

/* kernel.c  – VGA */
void vga_clear(void);
void vga_putchar(u8 c);
void vga_backspace(void);
void vga_scroll_up(void);
void vga_scroll_down(void);
void print_str(const char *s);
void print_hex_byte(u8 v);
void kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* kernel.c  – ATA */
void ata_read_sector(u32 lba, void *buf);
void ata_write_sector(u32 lba, const void *buf);

/* kernel.c  – FAT32 / VFS */
void vfs_init(void);
void fat32_init(void);
i64  vfs_open(const char *path);
i64  vfs_open_in(const char *name83, u32 dir_clus);
i64  vfs_read(u64 fd, void *buf, usize n);
i64  vfs_write(u64 fd, const void *buf, usize n);
i64  vfs_close(u64 fd);
i64  vfs_seek(u64 fd, i64 offset, u64 whence);
u32  fat32_alloc_cluster(void);
void fat32_create_file(const char *name83);
void fat32_create_file_in(const char *name83, u32 dir_clus);
void fat32_create_dir_entry(const char *name83, u32 cluster);
void fat32_create_dir_entry_in(const char *name83, u32 cluster, u32 dir_clus);
i64  fat32_delete_file(const char *name83);
i64  fat32_rename(const char *old83, const char *new83);
i64  fat32_delete_file_in(const char *name83, u32 dir_clus);
u32  fat32_next_cluster(u32 clus);
u32  cluster_to_lba(u32 clus);
i64  fat32_rename_in(const char *old83, const char *new83, u32 dir_clus);
i64  fat32_find_file(const char *name83, u32 *out_size);
void format_83_name(const char *src, char *dst);

i64  fat32_find_file_in(const char *name83, u32 *out_size, u32 dir_clus);
/* kernel.c  – Shell / I/O */
void kernel_main(void);
u8   read_key_raw(void);  /* polled PS/2 keyboard — used by sys_read stdin */

/* net.c — e1000 driver + TCP/IP stack */
void net_init(void);
void net_start(void);             /* init + DHCP, called from kernel_main */
void net_poll(void);              /* call from shell to process incoming packets */
void net_ip_send(u32 dst_ip, u8 proto, const void *payload, u16 plen);
void net_udp_send(u32 dst_ip, u16 src_port, u16 dst_port, const void *payload, u16 plen);
int  net_ping(u32 ip);           /* send ICMP echo, returns 1 if reply received */
u32  net_dns_resolve(const char *hostname); /* DNS A lookup, returns 0 on fail */
int  net_http_get(u32 ip, u16 port, const char *path, void *buf, usize bufsz);

/* fbdev.c — VESA framebuffer */
void  fb_enable(void);
int   fb_set_resolution(int w, int h);   /* switch display res at runtime */
void  fb_disable(void);
int   fb_is_enabled(void);
void  fb_put_pixel(int x, int y, u32 color);
u32   fb_get_pixel(int x, int y);
void  fb_fill_rect(int x, int y, int w, int h, u32 color);
void  fb_draw_rect(int x, int y, int w, int h, u32 color);
void  fb_draw_hline(int x, int y, int w, u32 color);
void  fb_fill_gradient_h(int x, int y, int w, int h, u32 ca, u32 cb);
void  fb_fill_gradient_v(int x, int y, int w, int h, u32 ca, u32 cb);
void  fb_draw_shadow(int x, int y, int w, int h);
void  fb_draw_vline(int x, int y, int h, u32 color);
void  fb_draw_line(int x0, int y0, int x1, int y1, u32 color);
void  fb_draw_rounded_rect(int x, int y, int w, int h, u32 color);
void  fb_fill_rounded_rect(int x, int y, int w, int h, u32 color);
void  fb_draw_circle(int cx, int cy, int r, u32 color);
void  fb_blit(int dx, int dy, int w, int h, const u32 *src, int src_stride);
int   fb_get_width(void);
int   fb_get_height(void);

/* USB HID keyboard/mouse (kernel/usb_hid.c) */
void  usb_hid_init(void);     /* legacy stub — superseded by usb_full_init */
void  usb_hid_poll(void);     /* legacy stub */

/* kernel/usb.c — full USB stack (EHCI + XHCI + mass storage) */
void  usb_full_init(void);    /* call after pci_scan_all */
void  usb_full_poll(void);    /* call from event/idle loop */
void  usb_list_devices(void); /* lsusb shell command helper */
int   usb_msc_read(int dev_idx, u64 lba, u32 count, void *buf);
int   usb_msc_write(int dev_idx, u64 lba, u32 count, void *buf);
int   usb_msc_count(void);
u64   usb_msc_block_count(int idx);
u32   usb_msc_block_size(int idx);
int   fb_get_bpp(void);
u8   *fb_get_ptr(void);
u64   fb_get_phys(void);

/* gui.c — Enhanced Framebuffer GUI windowing system */
void  gui_init(void);
void  gui_shutdown(void);
int   gui_is_active(void);
void  gui_redraw(void);
void  gui_cursor_move(int dx, int dy);
void  gui_cursor_get(int *out_x, int *out_y);
void  gui_handle_click(int px, int py);
void  gui_handle_mouse_down(int px, int py);
void  gui_handle_mouse_up(int px, int py);
void  gui_handle_right_click(int px, int py);
void  gui_handle_drag(int px, int py);
void  gui_stop_drag(void);
void  gui_stop_resize(void);

/* pngview.c — PNG image viewer command */
void  cmd_photo(const char *filename);
int   gui_window_create(int x, int y, int w, int h, const char *title);
void  gui_window_close(int id);
void  gui_window_set_active(int id);
int   gui_window_get_active(void);
int   gui_widget_add_button(int win_id, int x, int y, int w, int h, const char *text, int widget_id);
int   gui_widget_add_label(int win_id, int x, int y, const char *text, int widget_id);
int   gui_widget_add_checkbox(int win_id, int x, int y, const char *text, int checked, int widget_id);
int   gui_widget_add_progress(int win_id, int x, int y, int w, int progress, int widget_id);
int   gui_widget_add_separator(int win_id, int y, int widget_id);
int   gui_widget_add_listrow(int win_id, int x, int y, int w, const char *name, const char *size, int widget_id);
int   gui_widget_add_textinput(int win_id, int x, int y, int w, const char *placeholder, int widget_id);
int   gui_widget_add_icon(int win_id, int x, int y, int icon_type, int widget_id);
void  gui_open_shell_window(void);
void  gui_open_system_monitor(void);
void  gui_shell_append(const char *text);
void  gui_shell_keypress(u8 scancode);

/* TCP server API — TcpConn is defined in net.c; use as opaque pointer */
struct TcpConn;
typedef struct TcpConn TcpConn;
int       net_tcp_listen(u16 port);
TcpConn  *net_tcp_accept(u16 port);      /* blocking */
TcpConn  *net_tcp_accept_nb(u16 port);   /* non-blocking, returns NULL if none ready */
void      net_tcp_unlisten(u16 port);
int       net_tcp_send(TcpConn *c, const void *data, u16 len);
int       net_tcp_recv(TcpConn *c, void *buf, usize bufsz);
void      net_tcp_close(TcpConn *c);
void      net_http_serve(u16 port);   /* built-in HTTP file server */

/* helpers */
u32  net_make_ip(u8 a, u8 b, u8 c, u8 d);
void net_print_ip(u32 ip);
extern u8  net_mac[6];
extern u32 net_ip;               /* our IP (set by net_start) */
extern u32 net_gateway;          /* default gateway */
extern int net_ready;            /* 1 once e1000 is initialised */

/* tss.c */
void tss_init(void);

/* ── Phase 3 extras: Timer / Mouse-grab / POSIX files ────────── */

/* syscall.c — new implementations */
i64  sys_fstat(u64 fd, void *statbuf);
i64  sys_lseek(u64 fd, i64 offset, u64 whence);
i64  sys_lseek62(u64 fd, i64 offset, u64 whence);
i64  sys_dup(u64 oldfd);
i64  sys_rename(const char *oldpath, const char *newpath);
i64  sys_mkdir(const char *path, u64 mode);
i64  sys_rmdir(const char *path);
i64  sys_unlink(const char *path);
i64  sys_gettime_ms(void);
i64  sys_mouse_setmode(u64 mode);

/* ipc.c — microkernel message passing */
i64  sys_ipc_send(u64 dest_pid, void *msg);
i64  sys_ipc_recv(void *out_msg);
i64  sys_ipc_register(const char *name);
i64  sys_ipc_lookup(const char *name);
void ipc_init(void);
void ipc_dump_servers(void);

/* input.c — mouse grab */
int  input_is_grabbed(void);
void input_set_mouse_grab(int grabbed);
void input_push_mouse_ex(int dx, int dy, u8 buttons, int from_gui);

/* Syscall numbers */
#define SYS_FSTAT       5    /* also used for sys_malloc — we alias */
#define SYS_LSEEK       62
#define SYS_DUP         32
#define SYS_RENAME      82
#define SYS_MKDIR       83
#define SYS_RMDIR       84
#define SYS_UNLINK      87
#define SYS_GETTIME_MS  327
#define SYS_MOUSE_MODE  328
#define SYS_IPC_SEND    329
#define SYS_IPC_RECV    330
#define SYS_IPC_REG     331
#define SYS_IPC_LOOKUP  332

/* ── Phase 3: Audio ──────────────────────────────────────────── */

/* sound.c — syscall implementations */
i64 sys_snd_opl_write(u8 reg, u8 val);
i64 sys_snd_opl_note(u64 ch, u32 fnum, u32 block, u32 vol, u32 key_on);
i64 sys_snd_opl_reset(void);
i64 sys_snd_mix_play(u64 ch, const u8 *samples, u32 len, u32 loop);
i64 sys_snd_mix_stop(u64 ch);
i64 sys_snd_mix_volume(u64 ch, u32 vol);
i64 sys_snd_mix_tick(void);

/* Syscall numbers */
#define SYS_SND_OPL_WRITE   320
#define SYS_SND_OPL_NOTE    321
#define SYS_SND_OPL_RESET   322
#define SYS_SND_MIX_PLAY    323
#define SYS_SND_MIX_STOP    324
#define SYS_SND_MIX_VOLUME  325
#define SYS_SND_MIX_TICK    326

/* ── Phase 2: Graphics & Rendering ──────────────────────────── */

/* GfxTilemap — passed to sys_gfx_set_tilemap() */
typedef struct {
    const u32 *tileset;   /* flat pixel array: tile_count × tile_w × tile_h */
    const u32 *map;       /* 2-D tile index array, row-major, map_w × map_h  */
    u32  tile_w;          /* tile width  in pixels                            */
    u32  tile_h;          /* tile height in pixels                            */
    u32  tile_count;      /* number of tiles in the tileset strip             */
    u32  map_w;           /* map width  in tiles                              */
    u32  map_h;           /* map height in tiles                              */
    u32  _pad;
} GfxTilemap;

#define GFX_COLORKEY_NONE  0xFFFFFFFFUL  /* disables colorkey transparency   */
#define GFX_TILE_EMPTY     0xFFFFFFFFUL  /* map entry treated as transparent  */
#define GFX_MAX_LAYERS     2

/* gfx.c — syscall implementations */
i64 sys_gfx_flip(void);
i64 sys_gfx_clear(u32 color);
i64 sys_gfx_blit(int dx, int dy, int w, int h, const u32 *pixels, u32 colorkey);
i64 sys_gfx_set_colorkey(u32 color);
i64 sys_gfx_set_tilemap(u64 layer_id, const GfxTilemap *tm);
i64 sys_gfx_draw_tile(int x, int y, u32 tile_id, u64 layer_id);
i64 sys_gfx_render_layer(u64 layer_id, int scroll_x, int scroll_y);

/* Syscall numbers */
#define SYS_GFX_FLIP          310
#define SYS_GFX_CLEAR         311
#define SYS_GFX_BLIT          312
#define SYS_GFX_SET_COLORKEY  313
#define SYS_GFX_DRAW_TILE     314
#define SYS_GFX_SET_TILEMAP   315
#define SYS_GFX_RENDER_LAYER  316

/* ── Phase 1: Input & Controls ───────────────────────────────── */

/* KeyEvent — one entry in sys_poll_keys() ring */
typedef struct {
    u8  scancode;   /* raw PS/2 make-code                         */
    u8  ascii;      /* translated ASCII, or 0 for non-printable   */
    u8  mods;       /* INPUT_MOD_* bitmask                        */
    u8  _pad;
} KeyEvent;

#define INPUT_MOD_SHIFT  (1 << 0)
#define INPUT_MOD_CTRL   (1 << 1)
#define INPUT_MOD_ALT    (1 << 2)
#define INPUT_MOD_CAPS   (1 << 3)

/* MouseEvent — one entry in sys_poll_mouse() ring */
typedef struct {
    i64 dx;         /* relative X (positive = right)              */
    i64 dy;         /* relative Y (positive = up)                 */
    u8  buttons;    /* INPUT_BTN_* bitmask                        */
    u8  _pad[7];
} MouseEvent;

#define INPUT_BTN_LEFT   (1 << 0)
#define INPUT_BTN_RIGHT  (1 << 1)
#define INPUT_BTN_MIDDLE (1 << 2)

/* PadState — snapshot returned by sys_poll_pad() */
typedef struct {
    i64 axis_x;     /* -128 .. 127                                */
    i64 axis_y;
    u16 buttons;    /* bitmask of up to 16 buttons                */
    u8  connected;  /* 1 if pad detected                          */
    u8  _pad[5];
} PadState;

/* input.c — internal hooks called by kernel.c event loops */
void input_push_key(u8 scancode, u8 ascii);
void input_key_release(void);
void input_mod_update(u8 shift, u8 ctrl, u8 caps);
void input_key_repeat_tick(void);
void input_push_mouse(int dx, int dy, u8 buttons);
void input_pad_update(int axis_x, int axis_y, u16 buttons, u8 connected);

/* input.c — syscall implementations */
i64  sys_poll_keys(void *buf, usize max_events);
i64  sys_poll_mouse(void *buf, usize max_events);
i64  sys_poll_pad(void *buf);

/* syscall numbers for user-space reference */
#define SYS_POLL_KEYS   300
#define SYS_POLL_MOUSE  301
#define SYS_POLL_PAD    302

/* syscall.c */
void syscall_init(void);
i64  sys_read(u64 fd, void *buf, usize n);
i64  sys_write(u64 fd, const void *buf, usize n);
i64  sys_open(const char *path, u64 flags);
i64  sys_close(u64 fd);
i64  sys_exit_handler(i64 code);
void*sys_malloc(usize n);
i64  sys_free(void *p);
i64  sys_yield(void);
void*sys_mmap(u64 addr, usize len, u64 prot, u64 flags, u64 fd, u64 off);
i64  sys_mprotect(u64 addr, usize len, u64 prot);
i64  sys_munmap(u64 addr, usize len);
u64  sys_brk(u64 new_brk);
i64  sys_writev(u64 fd, const void *iov, u64 cnt);
i64  sys_getpid(void);
i64  sys_uname(void *buf);
i64  sys_getcwd(char *buf, usize sz);
i64  sys_getcwd_real(char *buf, usize sz);
i64  sys_chdir(const char *path);
i64  sys_lstat(const char *path, void *statbuf);
i64  sys_stat(const char *path, void *statbuf);
i64  sys_access(const char *path, int mode);
i64  sys_readlink(const char *path, char *buf, usize bufsz);
i64  sys_ioctl(u64 fd, u64 req, void *arg);
i64  sys_nanosleep(const void *req, void *rem);
i64  sys_sysinfo(void *info);
i64  sys_getppid(void);
i64  sys_gettimeofday(void *tv, void *tz);
i64  sys_getuid(void);
i64  sys_arch_prctl(u64 code, u64 addr);
i64  sys_openat(u64 dirfd, const char *path, u64 flags, u64 mode);
i64  sys_fstatat(u64 dirfd, const char *path, void *statbuf, u64 flags);
i64  sys_getdents64(u64 fd, void *buf, usize count);
i64  sys_stub(void);
i64  sys_stub_noent(void);
i64  sys_stub_enosys(void);

/* VFS core */
void vfs_core_init(void);
i64  vfs_mount_fs(const char *path, const char *fstype, u32 dev, vfs_inode_t *root);
i64  vfs_open(const char *path);
i64  vfs_read(u64 fd, void *buf, usize n);
i64  vfs_write(u64 fd, const void *buf, usize n);
i64  vfs_close(u64 fd);
i64  vfs_seek(u64 fd, i64 offset, u64 whence);
i64  vfs_stat(const char *path, void *statbuf);
i64  vfs_fstat(u64 fd, void *statbuf);
i64  vfs_mkdir(const char *path, u16 mode);
i64  vfs_unlink(const char *path);
i64  vfs_create(const char *path, u16 mode);
i64  vfs_readdir(u64 fd, void *buf, usize count);
i64  vfs_dup(u64 oldfd);
i64  vfs_dup2(u64 oldfd, u64 newfd);
void inode_ref(vfs_inode_t *inode);
void inode_deref(vfs_inode_t *inode);
vfs_fd_t *fd_get(u64 fd);

/* Pipe */
void *pipe_create(void);
i64  pipe_read(void *pipe, void *buf, usize count);
i64  pipe_write(void *pipe, const void *buf, usize count);
void pipe_destroy(void *pipe);
i64  sys_pipe(int *pipefd);

/* Signal */
void signal_init(void);
i64  sys_signal(int signum, void *handler);
i64  sys_sigaction(int signum, const sigaction_t *act, sigaction_t *oldact);
i64  sys_sigprocmask(int how, const u64 *set, u64 *oldset);
i64  sys_kill(u64 pid, int signum);
void signal_deliver(PCB *pcb);
u64  signal_pending(PCB *pcb);
extern signal_state_t proc_signals[PROC_MAX];

/* Fork/Exec/Clone */
i64  sys_fork(void);
i64  sys_execve(const char *path, char **argv, char **envp);
i64  sys_clone(u64 flags, void *stack, void *ptid, void *ctid, void *newtls);
i64  sys_wait4(u64 pid, int *wstatus, u64 options, void *ru);
i64  sys_exit_group(i64 code);
i64  sys_futex(u64 *addr, u64 op, u64 val, u64 timeout_ms, u64 *addr2, u64 val3);

/* Security */
void kaslr_init(void);
u64  stack_canary_generate(void);
void smap_smep_init(void);
int  is_user_addr(const void *addr, usize size);
i64  copy_from_user(void *dst, const void *src, usize n);
i64  copy_to_user(void *dst, const void *src, usize n);
int  syscall_validate_args(u64 num, u64 arg1, u64 arg2, u64 arg3, u64 arg4, u64 arg5, u64 arg6);
void syscall_log_fault(u64 num, u64 arg1, u64 arg2, u64 arg3);

/* Swap */
void swap_init(void);
i64  swap_out(u64 virt, u64 pid);
i64  swap_in(u64 virt, u64 pid);
void swap_invalidate_pid(u64 pid);

/* Journaling FS */
void jfs_init(void);
i64  jfs_create(const char *name, u16 mode);
i64  jfs_write(u32 ino, const void *buf, usize count, usize offset);
i64  jfs_read(u32 ino, void *buf, usize count, usize offset);
i64  jfs_unlink(u32 ino);
i64  jfs_mkdir(const char *name, u16 mode);
void jfs_journal_flush(void);
void vfs_register_jfs(void);   /* mount JFS at /jfs via inode_ops_t */
void vfs_register_fat32(void); /* mount FAT32 at /     via inode_ops_t */

/* TCP/IP */
void socket_init(void);
i64  sys_socket(int domain, int type, int protocol);
i64  sys_bind(int sockfd, const void *addr, usize addrlen);
i64  sys_listen(int sockfd, int backlog);
i64  sys_connect(int sockfd, const void *addr, usize addrlen);
i64  sys_accept(int sockfd, void *addr, usize *addrlen);
i64  sys_send(int sockfd, const void *buf, usize len, int flags);
i64  sys_recv(int sockfd, void *buf, usize len, int flags);
i64  sys_close_socket(int sockfd);
void tcpip_handle_packet(u8 *data, usize len);

/* UEFI */
void uefi_init(void *systab_ptr);
void uefi_get_memory_map(void);
void uefi_map_kernel(void);
void uefi_exit_boot_services(void);
u64  uefi_allocate_pages(usize pages);
void uefi_free_pages(u64 addr, usize pages);

/* ACPI */
void acpi_init(void);
void acpi_ioapic_init(void);
u32  acpi_ioapic_read(u32 reg);
void acpi_ioapic_write(u32 reg, u32 val);
void acpi_reboot(void);
void acpi_shutdown(void);
u32  acpi_get_ioapic_addr(void);
u32  acpi_get_ioapic_gsi_base(void);

/* PCI */
u32  pci_read32(u8 bus, u8 slot, u8 fn, u16 off);
void pci_write32(u8 bus, u8 slot, u8 fn, u16 off, u32 v);
u16  pci_read16(u8 bus, u8 slot, u8 fn, u16 off);
void pci_write16(u8 bus, u8 slot, u8 fn, u16 off, u16 v);
u8   pci_read8 (u8 bus, u8 slot, u8 fn, u16 off);
void pci_scan_all(void);
void pci_enable_device(void *dev);
void pci_enable_bus_master(void *dev);
void *pci_find_device(u16 vendor, u16 device);
void *pci_find_class(u8 class_code, u8 subclass);
void *pci_find_class_progif(u8 class_code, u8 subclass, u8 prog_if);
void pci_list_devices(void);
void pci_power_on(void *dev);
u64  pci_bar_base(void *dev, int bar);
u64  pci_bar_size(void *dev, int bar);
u16  pci_bar_io(void *dev, int bar);
u8   pci_irq(void *dev);
void pci_set_ecam(u64 base, u8 start_bus);

/* AHCI */
/* ── AHCI SATA driver (ahci.c) ─────────────────────────────── */
void        ahci_init(u32 bar);
i64         ahci_read_sector(int port, u32 lba, void *buf);
i64         ahci_read_sectors(int port, u64 lba, u16 count, void *buf);
i64         ahci_write_sector(int port, u32 lba, const void *buf);
i64         ahci_write_sectors(int port, u64 lba, u16 count, const void *buf);
i64         ahci_flush(int port);
i64         ahci_identify(int port, void *buf);
int         ahci_get_port_count(void);
u64         ahci_get_sector_count(int port);
const char *ahci_get_model(int port);

/* ── NVMe driver (nvme.c) ──────────────────────────────────── */
void nvme_init(u64 bar);
i64  nvme_read_sector(u64 lba, void *buf);
i64  nvme_write_sector(u64 lba, const void *buf);
i64  nvme_read_4k(u64 lba, void *buf);
i64  nvme_write_4k(u64 lba, const void *buf);
i64  nvme_flush(void);
int  nvme_ready(void);
u64  nvme_sector_count(void);
u32  nvme_lba_size(void);

/* PS/2 keyboard + mouse (i8042) */
void ps2_init(void);
void ps2_poll(void);
int  ps2_kb_ok(void);
int  ps2_mouse_ok(void);
int  ps2_mouse_x(void);
int  ps2_mouse_y(void);
void ps2_mouse_refresh(void);
void ps2_mouse_hide(void);
void ps2_mouse_show_cursor(void);

/* NIC (e1000 + RTL8139 abstraction) */
void  nic_init(void);
void  nic_send(const u8 *buf, usize len);
void  nic_poll(void);
extern u8  nic_mac[6];
extern int nic_ready;
/* Called by nic_poll to deliver received frames to the network stack */
void  net_rx_deliver(const u8 *buf, u16 len);

/* Shell */
void shell_init(void);
void shell_run(void);

/* Package Manager */
void pkg_init(void);
void pkg_add(const char *name, const char *desc, const char *url, u32 size);
void pkg_list(void);
i64  pkg_install(const char *name);
i64  pkg_remove(const char *name);

/* New syscall numbers */
#define SYS_FORK        57
#define SYS_EXECVE      59
#define SYS_EXIT_GROUP  231
#define SYS_PIPE        22
#define SYS_SOCKET      41
#define SYS_BIND        49
#define SYS_CONNECT     42
#define SYS_LISTEN      50
#define SYS_ACCEPT      43
#define SYS_SEND        44
#define SYS_RECV        45
#define SYS_KILL        62
#define SYS_SIGNAL      13
#define SYS_WAIT4       61
#define SYS_CLONE       56
#define SYS_SIGACTION   13
#define SYS_SIGPROCMASK 14
#define SYS_DUP2        33
#define SYS_GETPID      39
#define SYS_PKG_INSTALL 350
#define SYS_PKG_REMOVE  351
#define SYS_PKG_LIST    352
#define SYS_WATCHDOG_PET 353

#define ENAMETOOLONG (-36LL)
#define ECHILD       (-10LL)
#define ESRCH        (-3LL)
#define EAGAIN       (-11LL)
#define EWOULDBLOCK  (-11LL)
#define EINPROGRESS  (-115LL)
#define EALREADY     (-114LL)
#define EBUSY        (-16LL)
#define ENOTCONN     (-107LL)

/* fcntl */
#define F_DUPFD      0
#define F_GETFD      1
#define F_SETFD      2
#define F_GETFL      3
#define F_SETFL      4
#define FD_CLOEXEC   1

/* open / socket flags */
#define O_NONBLOCK   0x800
#define SOCK_NONBLOCK 0x800

/* poll */
#define POLLIN       0x0001
#define POLLPRI      0x0002
#define POLLOUT      0x0004
#define POLLERR      0x0008
#define POLLHUP      0x0010
#define POLLNVAL     0x0020

/* epoll */
#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3
#define EPOLLIN       0x00000001u
#define EPOLLPRI      0x00000002u
#define EPOLLOUT      0x00000004u
#define EPOLLERR      0x00000008u
#define EPOLLHUP      0x00000010u
#define EPOLLONESHOT  (1u << 30)
#define EPOLLET       (1u << 31)
#define ENETUNREACH  (-101LL)
#define ENODEV       (-19LL)
#define ETIMEDOUT    (-110LL)
#define EISCONN      (-106LL)
#define ECONNREFUSED (-111LL)

/* clock IDs for clock_gettime (syscall 228) */
#define CLOCK_REALTIME           0
#define CLOCK_MONOTONIC          1
#define CLOCK_PROCESS_CPUTIME_ID 2
#define CLOCK_THREAD_CPUTIME_ID  3
#define CLOCK_MONOTONIC_RAW      4
#define CLOCK_REALTIME_COARSE    5
#define CLOCK_MONOTONIC_COARSE   6
#define CLOCK_BOOTTIME           7
#define ENOTDIR      (-20LL)
#define ENOSPC       (-28LL)
#define EOPNOTSUPP   (-95LL)
#define EACCES       (-13LL)
#define EIO          (-5LL)
#define EINTR        (-4LL)

/* ── inline I/O port helpers ─────────────────────────────────── */
static inline void outb(u16 port, u8 val) {
    __asm__ volatile("outb %0, %1" :: "a"(val), "Nd"(port));
}
static inline u8 inb(u16 port) {
    u8 v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline void outw(u16 port, u16 val) {
    __asm__ volatile("outw %0, %1" :: "a"(val), "Nd"(port));
}
static inline u16 inw(u16 port) {
    u16 v;
    __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline void outl(u16 port, u32 val) {
    __asm__ volatile("outl %0, %1" :: "a"(val), "Nd"(port));
}
static inline u32 inl(u16 port) {
    u32 v;
    __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline u64 read_cr3(void) {
    u64 v;
    __asm__ volatile("mov %%cr3, %0" : "=r"(v));
    return v;
}
static inline void write_cr3(u64 v) {
    __asm__ volatile("mov %0, %%cr3" :: "r"(v) : "memory");
}
static inline void wrmsr(u32 msr, u64 val) {
    u32 lo = (u32)val, hi = (u32)(val >> 32);
    __asm__ volatile("wrmsr" :: "c"(msr), "a"(lo), "d"(hi));
}
static inline u64 rdmsr(u32 msr) {
    u32 lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((u64)hi << 32) | lo;
}
static inline void sti(void)  { __asm__ volatile("sti"); }
static inline void cli(void)  { __asm__ volatile("cli"); }
static inline void hlt(void)  { __asm__ volatile("hlt"); }
static inline void io_wait(void) { outb(0x80, 0); }

/* ================================================================
 *  Syslibc — shared kernel+user C library
 *  Provides: mem*, str*, char class, atoi/strtol, snprintf,
 *            qsort/bsearch, setjmp/longjmp, bit math, and more.
 *  Compiled with the same -ffreestanding -nostdlib flags as kernel.
 * ================================================================ */
#include "../libc/systrix_libc.h"

/* ── slibc integer-to-string short aliases ───────────────────────
 * slibc_u64_to_dec / slibc_u64_to_hex are the canonical names;
 * these short aliases let call sites stay concise.                */
#define u64_to_dec   slibc_u64_to_dec
#define u64_to_hex   slibc_u64_to_hex

/* MIN/MAX/CLAMP/ARRAY_SIZE — guard against syslibc redefinition    */
#ifndef MIN
#define MIN(a, b)         ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b)         ((a) > (b) ? (a) : (b))
#endif
#ifndef CLAMP
#define CLAMP(v, lo, hi)  ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))
#endif
#define ARRAY_SIZE(a)     (sizeof(a) / sizeof((a)[0]))

/* ── poll / epoll structs ──────────────────────────────────────── */
typedef struct {
    int   fd;
    short events;
    short revents;
} pollfd_t;

typedef union {
    u32   u32;
    u64   u64;
    void *ptr;
    int   fd;
} epoll_data_t;

typedef struct {
    u32          events;
    epoll_data_t data;
} __attribute__((packed)) epoll_event_t;

/* forward decls for poll/epoll/fcntl syscalls */
i64 sys_poll(pollfd_t *fds, u64 nfds, int timeout_ms);
i64 sys_select(int nfds, void *rfds, void *wfds, void *efds, void *tv);
i64 sys_fcntl(u64 fd, u64 cmd, u64 arg);
i64 sys_epoll_create(int flags);
i64 sys_epoll_ctl(int epfd, int op, int fd, epoll_event_t *ev);
i64 sys_epoll_wait(int epfd, epoll_event_t *evs, int maxevents, int timeout_ms);
