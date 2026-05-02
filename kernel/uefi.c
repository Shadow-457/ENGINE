#include "../include/kernel.h"

#define EFI_SYSTEM_TABLE_SIGNATURE 0x5453595320494249ULL
#define EFI_RUNTIME_SERVICES_SIGNATURE 0x5652545320494249ULL
#define EFI_BOOT_SERVICES_SIGNATURE 0x5652545320494249ULL

#define EFI_PAGE_SIZE 4096

typedef struct {
    u64 signature;
    u32 revision;
    u32 header_size;
    u32 crc32;
    u32 reserved;
} efi_table_hdr_t;

typedef struct {
    efi_table_hdr_t hdr;
    u64 fw_vendor;
    u32 fw_revision;
    u64 con_in_handle;
    u64 con_in;
    u64 con_out_handle;
    u64 con_out;
    u64 stderr_handle;
    u64 stderr;
    u64 runtime;
    u64 boot;
    u64 num_configs;
    u64 config;
} efi_system_table_t;

typedef struct {
    efi_table_hdr_t hdr;
    u64 raise_tpl;
    u64 restore_tpl;
    u64 allocate_pages;
    u64 free_pages;
    u64 get_memory_map;
    u64 allocate_pool;
    u64 free_pool;
    u64 create_event;
    u64 set_timer;
    u64 wait_for_event;
    u64 signal_event;
    u64 close_event;
    u64 check_event;
    u64 install_protocol_interface;
    u64 reinstall_protocol_interface;
    u64 uninstall_protocol_interface;
    u64 handle_protocol;
    u64 reserved;
    u64 register_protocol_notify;
    u64 locate_handle;
    u64 locate_device_path;
    u64 install_configuration_table;
} efi_boot_services_t;

typedef struct {
    u32 type;
    u64 phys_start;
    u64 virt_start;
    u64 num_pages;
    u64 attribute;
} __attribute__((packed)) efi_mem_desc_t;

#define EFI_LOADER_CODE 1
#define EFI_LOADER_DATA 2
#define EFI_BOOT_SERVICES_CODE 3
#define EFI_BOOT_SERVICES_DATA 4
#define EFI_RUNTIME_SERVICES_CODE 5
#define EFI_RUNTIME_SERVICES_DATA 6
#define EFI_CONVENTIONAL_MEMORY 7
#define EFI_UNUSABLE_MEMORY 8
#define EFI_ACPI_RECLAIM_MEMORY 9
#define EFI_ACPI_MEMORY_NVS 10
#define EFI_MEMORY_MAPPED_IO 11
#define EFI_MEMORY_MAPPED_IO_PORT_SPACE 12
#define EFI_PAL_CODE 13
#define EFI_PERSISTENT_MEMORY 14

typedef struct {
    u32 signature;
    u32 revision;
    u32 header_size;
    u32 crc32;
    u32 reserved;
    u32 num_entries;
    u32 desc_size;
    u32 desc_version;
} efi_memory_map_t;

typedef struct {
    u64 signature;
    u32 revision;
    u32 header_size;
    u32 crc32;
    u32 reserved;
    u64 num_table_entries;
    u64 vendor_guid;
    u64 vendor_table;
} efi_runtime_services_t;

typedef struct {
    u64 signature;
    u32 revision;
    u32 header_size;
    u32 crc32;
    u32 reserved;
    u32 attributes;
    u64 num_entries;
    u64 entry_size;
    u64 entry_version;
    efi_mem_desc_t entry[1];
} efi_config_table_t;

static efi_system_table_t *efi_systab = NULL;
static efi_boot_services_t *efi_boot_svc = NULL;
static efi_mem_desc_t *efi_mem_map = NULL;
static usize efi_mem_map_size = 0;

void uefi_init(void *systab_ptr) {
    if (!systab_ptr) return;
    efi_systab = (efi_system_table_t*)systab_ptr;
    if (efi_systab->hdr.signature != EFI_SYSTEM_TABLE_SIGNATURE) {
        efi_systab = NULL;
        return;
    }
    efi_boot_svc = (efi_boot_services_t*)efi_systab->boot;
    kprintf("[UEFI] System table found at: ");
    print_hex_byte((u8)((u64)efi_systab >> 56));
    print_hex_byte((u8)((u64)efi_systab >> 48));
    print_hex_byte((u8)((u64)efi_systab >> 40));
    print_hex_byte((u8)((u64)efi_systab >> 32));
    print_hex_byte((u8)((u64)efi_systab >> 24));
    print_hex_byte((u8)((u64)efi_systab >> 16));
    print_hex_byte((u8)((u64)efi_systab >> 8));
    print_hex_byte((u8)(u64)efi_systab);
    kprintf("\r\n");
}

void uefi_get_memory_map(void) {
    if (!efi_boot_svc) return;
    kprintf("[UEFI] Getting memory map...\r\n");
}

void uefi_map_kernel(void) {
    if (!efi_mem_map || !efi_mem_map_size) return;
    usize num_entries = efi_mem_map_size / sizeof(efi_mem_desc_t);
    for (usize i = 0; i < num_entries; i++) {
        efi_mem_desc_t *desc = &efi_mem_map[i];
        if (desc->type == EFI_CONVENTIONAL_MEMORY) {
            u64 pages = desc->num_pages;
            u64 start = desc->phys_start;
            for (u64 p = 0; p < pages; p++) {
                u64 page = start + p * EFI_PAGE_SIZE;
                if (page >= RAM_START && page < RAM_END) {
                    vmm_map(read_cr3(), page, page, PTE_PRESENT | PTE_WRITE);
                }
            }
        } else if (desc->type == EFI_RUNTIME_SERVICES_DATA ||
                   desc->type == EFI_RUNTIME_SERVICES_CODE) {
            u64 pages = desc->num_pages;
            u64 start = desc->phys_start;
            for (u64 p = 0; p < pages; p++) {
                u64 page = start + p * EFI_PAGE_SIZE;
                vmm_map(read_cr3(), page, page, PTE_PRESENT | PTE_WRITE);
            }
        }
    }
    kprintf("[UEFI] Memory map applied\r\n");
}

void uefi_exit_boot_services(void) {
    if (!efi_boot_svc) return;
    kprintf("[UEFI] Exiting boot services...\r\n");
}

u64 uefi_allocate_pages(usize pages) {
    if (!efi_boot_svc) return 0;
    u64 addr = 0;
    typedef u64 (*alloc_pages_fn)(u32 type, u64 memory_type, usize pages, u64 *memory);
    alloc_pages_fn alloc = (alloc_pages_fn)efi_boot_svc->allocate_pages;
    u64 status = alloc(1, 2, pages, &addr);
    if (status != 0) return 0;
    return addr;
}

void uefi_free_pages(u64 addr, usize pages) {
    if (!efi_boot_svc) return;
    typedef u64 (*free_pages_fn)(u64 memory, usize pages);
    free_pages_fn free = (free_pages_fn)efi_boot_svc->free_pages;
    free(addr, pages);
}
