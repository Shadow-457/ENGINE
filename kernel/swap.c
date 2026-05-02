#include "../include/kernel.h"

#define SWAP_MAGIC 0x57415053ULL
#define SWAP_MAX_PAGES 4096
#define SWAP_DISK_SECTORS 8192
#define SWAP_START_LBA 130000

typedef struct {
    u64 magic;
    u32 total_sectors;
    u32 used_sectors;
    u32 bitmap[SWAP_DISK_SECTORS / 32];
} swap_header_t;

typedef struct {
    u64 virt;
    u64 swap_sector;
    u64 pid;
} swap_entry_t;

static swap_entry_t swap_table[SWAP_MAX_PAGES];
static u32 swap_next_free = 0;
static u8 swap_buf[512];

void swap_init(void) {
    memset(swap_table, 0, sizeof(swap_table));
    ata_read_sector(SWAP_START_LBA, swap_buf);
    swap_header_t *hdr = (swap_header_t*)swap_buf;
    if (hdr->magic != SWAP_MAGIC) {
        memset(swap_buf, 0, 512);
        hdr = (swap_header_t*)swap_buf;
        hdr->magic = SWAP_MAGIC;
        hdr->total_sectors = SWAP_DISK_SECTORS;
        hdr->used_sectors = 0;
        ata_write_sector(SWAP_START_LBA, swap_buf);
    }
}

static u32 swap_alloc_sector(void) {
    ata_read_sector(SWAP_START_LBA, swap_buf);
    swap_header_t *hdr = (swap_header_t*)swap_buf;
    for (u32 i = 0; i < SWAP_DISK_SECTORS; i++) {
        u32 idx = (swap_next_free + i) % SWAP_DISK_SECTORS;
        if (!(hdr->bitmap[idx / 32] & (1 << (idx % 32)))) {
            hdr->bitmap[idx / 32] |= (1 << (idx % 32));
            hdr->used_sectors++;
            ata_write_sector(SWAP_START_LBA, swap_buf);
            swap_next_free = (idx + 1) % SWAP_DISK_SECTORS;
            return idx;
        }
    }
    return (u32)-1;
}

static void swap_free_sector(u32 sector) {
    if (sector >= SWAP_DISK_SECTORS) return;
    ata_read_sector(SWAP_START_LBA, swap_buf);
    swap_header_t *hdr = (swap_header_t*)swap_buf;
    hdr->bitmap[sector / 32] &= ~(1 << (sector % 32));
    hdr->used_sectors--;
    ata_write_sector(SWAP_START_LBA, swap_buf);
}

i64 swap_out(u64 virt, u64 pid) {
    u64 phys = vmm_virt_to_phys(read_cr3(), virt & ~(u64)(PAGE_SIZE - 1));
    if (!phys) return (i64)EFAULT;
    phys &= ~(u64)(PAGE_SIZE - 1);
    u32 sector = swap_alloc_sector();
    if (sector == (u32)-1) return (i64)ENOMEM;
    for (int i = 0; i < 8; i++) {
        ata_write_sector(SWAP_START_LBA + sector * 8 + i, (void*)(usize)(phys + i * 512));
    }
    for (int i = 0; i < SWAP_MAX_PAGES; i++) {
        if (!swap_table[i].virt) {
            swap_table[i].virt = virt & ~(u64)(PAGE_SIZE - 1);
            swap_table[i].swap_sector = sector;
            swap_table[i].pid = pid;
            return 0;
        }
    }
    swap_free_sector(sector);
    return (i64)ENOMEM;
}

i64 swap_in(u64 virt, u64 pid) {
    u64 page = virt & ~(u64)(PAGE_SIZE - 1);
    for (int i = 0; i < SWAP_MAX_PAGES; i++) {
        if (swap_table[i].virt == page && swap_table[i].pid == pid) {
            u64 phys = pmm_alloc();
            if (!phys) return (i64)ENOMEM;
            for (int j = 0; j < 8; j++) {
                ata_read_sector(SWAP_START_LBA + swap_table[i].swap_sector * 8 + j, (void*)(usize)(phys + j * 512));
            }
            vmm_map(read_cr3(), page, phys, PTE_USER_RW);
            swap_free_sector(swap_table[i].swap_sector);
            swap_table[i].virt = 0;
            swap_table[i].swap_sector = 0;
            swap_table[i].pid = 0;
            return 0;
        }
    }
    return (i64)ENOENT;
}

void swap_invalidate_pid(u64 pid) {
    for (int i = 0; i < SWAP_MAX_PAGES; i++) {
        if (swap_table[i].pid == pid) {
            swap_free_sector(swap_table[i].swap_sector);
            swap_table[i].virt = 0;
            swap_table[i].swap_sector = 0;
            swap_table[i].pid = 0;
        }
    }
}
