/* ============================================================
 *  Systrix OS — kernel/ahci.c
 *
 *  AHCI (Advanced Host Controller Interface) SATA driver.
 *
 *  Responsibilities:
 *    - Initialize the HBA (Host Bus Adapter) controller
 *    - Detect and spin up attached SATA drives
 *    - Issue ATA commands via DMA using command lists and FISes
 *    - Read and write sectors (single and multi-sector)
 *    - Issue cache-flush commands
 *    - Recover from errors via COMRESET
 *
 *  Overview of the AHCI command flow:
 *    1. Fill in an H2D Register FIS describing the ATA command.
 *    2. Set up one or more PRDT entries pointing at the data buffer.
 *    3. Write to the Port Command Issue register (P_CI) to start DMA.
 *    4. Poll until the HBA clears the command-active bit or reports an error.
 * ============================================================ */

#include "../include/kernel.h"


/* ============================================================
 *  HBA Global Register Offsets
 *
 *  These are offsets from the HBA base address (BAR5).
 *  See AHCI spec §3.1.
 * ============================================================ */

#define HBA_CAP     0x00    /* Host Capabilities */
#define HBA_GHC     0x04    /* Global Host Control */
#define HBA_IS      0x08    /* Interrupt Status (one bit per port) */
#define HBA_PI      0x0C    /* Ports Implemented (bitmask) */
#define HBA_VS      0x10    /* AHCI Version */

/* Bits inside HBA_GHC */
#define HBA_GHC_AHCI_ENABLE  (1u << 31)  /* must be set to use AHCI mode */
#define HBA_GHC_HBA_RESET    (1u << 0)   /* write 1 to reset; HBA clears it when done */


/* ============================================================
 *  Port Register Offsets
 *
 *  Each port has its own 128-byte register block starting at
 *  HBA_base + 0x100 + (port_index * 0x80).
 *  See AHCI spec §3.3.
 * ============================================================ */

#define P_CLB    0x00   /* Command List Base Address (low 32 bits) */
#define P_CLBU   0x04   /* Command List Base Address (high 32 bits) */
#define P_FB     0x08   /* FIS Base Address (low 32 bits) */
#define P_FBU    0x0C   /* FIS Base Address (high 32 bits) */
#define P_IS     0x10   /* Interrupt Status */
#define P_IE     0x14   /* Interrupt Enable */
#define P_CMD    0x18   /* Command and Status */
#define P_TFD    0x20   /* Task File Data (ATA status and error registers) */
#define P_SIG    0x24   /* Signature (device type) */
#define P_SSTS   0x28   /* SATA Status (DET, SPD, IPM fields) */
#define P_SCTL   0x2C   /* SATA Control */
#define P_SERR   0x30   /* SATA Error */
#define P_SACT   0x34   /* SATA Active (native command queuing) */
#define P_CI     0x38   /* Command Issue (one bit per command slot) */

/* Bits inside P_CMD */
#define PCMD_START            (1u << 0)   /* start processing the command list */
#define PCMD_SPIN_UP_DEVICE   (1u << 1)   /* request the device to spin up */
#define PCMD_POWER_ON_DEVICE  (1u << 2)   /* request cold-presence power on */
#define PCMD_FIS_RX_ENABLE    (1u << 4)   /* allow the port to receive FISes */
#define PCMD_FIS_RX_RUNNING   (1u << 14)  /* FIS receive DMA is active (read-only) */
#define PCMD_CMD_LIST_RUNNING (1u << 15)  /* command list DMA is active (read-only) */

/* Bits inside P_TFD (mirrors the ATA Status register) */
#define TFD_ERROR (1u << 0)   /* ATA ERR bit — previous command failed */
#define TFD_DRQ   (1u << 3)   /* ATA DRQ bit — device wants data transfer */
#define TFD_BUSY  (1u << 7)   /* ATA BSY bit — device is busy */

/* P_SSTS DET field values (bits [3:0]) */
#define SSTS_DET_DEVICE_PRESENT  0x3u   /* device present and PHY communication established */

/* Fatal error bits in P_IS — any of these requires a port reset */
#define P_IS_TASK_FILE_ERROR     (1u << 30)  /* device reported an error */
#define P_IS_HOST_BUS_FATAL      (1u << 29)  /* host bus data error (uncorrectable) */
#define P_IS_HOST_BUS_DATA       (1u << 28)  /* host bus data error */
#define P_IS_INTERFACE_FATAL     (1u << 27)  /* interface fatal error */

#define P_IS_ANY_FATAL_ERROR  (P_IS_TASK_FILE_ERROR | P_IS_HOST_BUS_FATAL | \
                               P_IS_HOST_BUS_DATA   | P_IS_INTERFACE_FATAL)


/* ============================================================
 *  FIS Types and ATA Commands
 * ============================================================ */

#define FIS_TYPE_H2D  0x27   /* Host-to-Device Register FIS */

#define ATA_READ_DMA_EXT   0x25   /* 48-bit LBA DMA read */
#define ATA_WRITE_DMA_EXT  0x35   /* 48-bit LBA DMA write */
#define ATA_IDENTIFY       0xEC   /* identify device (returns 512 bytes of info) */
#define ATA_FLUSH_EXT      0xEA   /* flush write cache to persistent storage */


/* ============================================================
 *  Limits
 * ============================================================ */

#define MAX_PRDT_ENTRIES      8    /* PRDT entries per command table */
#define MAX_SECTORS_PER_CMD   8    /* maximum sectors in a single DMA command */
#define MAX_PORTS             8    /* we track at most 8 drives */


/* ============================================================
 *  AHCI Data Structures
 *
 *  All structures are packed and must meet specific alignment
 *  requirements imposed by the AHCI spec.
 * ============================================================ */

/*
 * Command Header (32 bytes, must be 32-byte aligned)
 *
 * Each port maintains a list of up to 32 of these.  We only
 * ever use slot 0 (no native command queuing).
 */
typedef struct __attribute__((packed, aligned(32))) {
    u16 flags;          /* bits[4:0] = FIS length in DWORDs; bit 6 = write direction */
    u16 prdt_length;    /* number of PRDT entries in the command table */
    u32 bytes_transferred; /* filled by HBA after command completes */
    u32 cmd_table_addr_low;
    u32 cmd_table_addr_high;
    u32 reserved[4];
} AhciCmdHeader;

/*
 * Physical Region Descriptor Table entry (16 bytes)
 *
 * Describes one contiguous buffer in physical memory that the
 * HBA should DMA to or from.
 */
typedef struct __attribute__((packed)) {
    u32 data_addr_low;
    u32 data_addr_high;
    u32 reserved;
    u32 byte_count_and_irq;  /* bits[21:0] = (byte_count - 1); bit 31 = interrupt on completion */
} AhciPrdtEntry;

/*
 * Command Table (must be 128-byte aligned)
 *
 * Holds the FIS (the actual ATA command bytes) plus the PRDT
 * (the list of data buffer descriptors).
 */
typedef struct __attribute__((packed, aligned(128))) {
    u8          command_fis[64];     /* Command FIS — we fill in the first 20 bytes */
    u8          atapi_command[16];   /* ATAPI command (unused for plain SATA) */
    u8          reserved[48];
    AhciPrdtEntry prdt[MAX_PRDT_ENTRIES];
} AhciCmdTable;

/*
 * Host-to-Device Register FIS (Frame Information Structure)
 *
 * This is the structure we write into command_fis[] to send
 * an ATA command to the drive.  See SATA spec §10.3.1.
 */
typedef struct __attribute__((packed)) {
    u8  fis_type;       /* always FIS_TYPE_H2D (0x27) */
    u8  port_and_c_bit; /* bits[3:0] = PM port; bit 7 = 1 means this is a command */
    u8  command;        /* ATA command register */
    u8  feature_low;    /* ATA feature register (low byte) */

    u8  lba_byte0;      /* LBA bits  [7:0]  */
    u8  lba_byte1;      /* LBA bits [15:8]  */
    u8  lba_byte2;      /* LBA bits [23:16] */
    u8  device;         /* device register — set bit 6 for LBA mode */

    u8  lba_byte3;      /* LBA bits [31:24] — "extended" bytes for 48-bit LBA */
    u8  lba_byte4;      /* LBA bits [39:32] */
    u8  lba_byte5;      /* LBA bits [47:40] */
    u8  feature_high;   /* ATA feature register (high byte, for 48-bit commands) */

    u16 sector_count;   /* number of sectors to transfer */
    u8  icc;            /* isochronous command completion (unused) */
    u8  control;        /* device control register */

    u32 reserved;
} FisH2D;


/* ============================================================
 *  Per-Port State
 * ============================================================ */

typedef struct {
    AhciCmdHeader *cmd_list;    /* 32-entry command list (we use slot 0 only) */
    u8            *fis_buf;     /* buffer for FISes received from the drive */
    AhciCmdTable  *cmd_table;   /* command table for slot 0 */
    u8            *data_buf;    /* DMA data buffer (MAX_SECTORS_PER_CMD * 512 bytes) */

    u32  port_reg_offset;       /* byte offset of this port's registers from g_hba_base */
    int  present;               /* 1 = drive detected and initialized */

    u64  sector_count;          /* total number of LBA sectors reported by IDENTIFY */
    char model[41];             /* model string from IDENTIFY (null-terminated) */
} AhciPort;


/* ============================================================
 *  Module Globals
 * ============================================================ */

static u8      *g_hba_base = NULL;              /* virtual address of HBA MMIO region */
static AhciPort g_ports[MAX_PORTS];
static int      g_num_ports = 0;                /* how many drives were successfully initialized */


/* ============================================================
 *  MMIO Helpers
 *
 *  All HBA and port registers must be accessed as 32-bit
 *  volatile reads and writes to prevent the compiler from
 *  reordering or caching the accesses.
 * ============================================================ */

static u32 hba_read(u32 offset)
{
    return *(volatile u32 *)(g_hba_base + offset);
}

static void hba_write(u32 offset, u32 value)
{
    *(volatile u32 *)(g_hba_base + offset) = value;
}

static u32 port_read(int port_index, u32 offset)
{
    return *(volatile u32 *)(g_hba_base + g_ports[port_index].port_reg_offset + offset);
}

static void port_write(int port_index, u32 offset, u32 value)
{
    *(volatile u32 *)(g_hba_base + g_ports[port_index].port_reg_offset + offset) = value;
}


/* ============================================================
 *  Aligned Memory Allocation
 *
 *  AHCI structures need specific power-of-two alignments.
 *  heap_malloc() gives us unaligned memory, so we over-allocate
 *  by (align - 1) bytes and round the pointer up.
 *
 *  Note: this leaks the bytes before the aligned address, which
 *  is acceptable for a simple kernel that never frees AHCI buffers.
 * ============================================================ */
static void *alloc_aligned(usize size, usize alignment)
{
    usize raw = (usize)heap_malloc(size + alignment - 1);

    if (raw == 0) {
        return NULL;
    }

    return (void *)((raw + alignment - 1) & ~(alignment - 1));
}


/* ============================================================
 *  Port Stop
 *
 *  Clears the START and FIS_RX_ENABLE bits, then waits for
 *  the HBA to confirm that DMA has actually stopped.  Must be
 *  called before reprogramming the command list or FIS buffer
 *  base addresses.
 * ============================================================ */
static void port_stop(int port_index)
{
    u32 cmd = port_read(port_index, P_CMD);
    cmd &= ~(PCMD_START | PCMD_FIS_RX_ENABLE);
    port_write(port_index, P_CMD, cmd);

    /* Wait for the HBA to drain in-flight DMA */
    for (int timeout = 50000; timeout-- > 0; ) {
        u32 running = port_read(port_index, P_CMD) & (PCMD_CMD_LIST_RUNNING | PCMD_FIS_RX_RUNNING);
        if (running == 0) {
            break;
        }
        io_wait();
    }
}


/* ============================================================
 *  Port Start
 *
 *  Enables FIS receive, waits for the device to be not-busy,
 *  then sets the START bit so the HBA begins processing commands.
 * ============================================================ */
static void port_start(int port_index)
{
    /* Wait for the drive to finish any internal activity */
    for (int timeout = 100000; timeout-- > 0; ) {
        u32 tfd = port_read(port_index, P_TFD);
        if (!(tfd & (TFD_BUSY | TFD_DRQ))) {
            break;
        }
        io_wait();
    }

    u32 cmd = port_read(port_index, P_CMD);
    cmd |= PCMD_FIS_RX_ENABLE;
    port_write(port_index, P_CMD, cmd);

    cmd |= PCMD_START;
    port_write(port_index, P_CMD, cmd);
}


/* ============================================================
 *  Port Reset (COMRESET)
 *
 *  Used for error recovery.  Sends a COMRESET on the SATA link
 *  by briefly writing DET=1 to P_SCTL, then waits for the drive
 *  to re-establish the link before restarting the port.
 * ============================================================ */
static void port_reset(int port_index)
{
    port_stop(port_index);

    /* Assert COMRESET (DET=1 in SCTL) */
    u32 sctl = port_read(port_index, P_SCTL);
    port_write(port_index, P_SCTL, (sctl & ~0xFu) | 1u);

    for (volatile int t = 10000; t-- > 0; ) {
        io_wait();      /* hold reset for ~10 ms */
    }

    /* De-assert COMRESET (DET=0) */
    port_write(port_index, P_SCTL, sctl & ~0xFu);

    /* Wait for the drive to re-establish the link */
    for (int timeout = 100000; timeout-- > 0; ) {
        u32 det = port_read(port_index, P_SSTS) & 0xFu;
        if (det == SSTS_DET_DEVICE_PRESENT) {
            break;
        }
        io_wait();
    }

    /* Clear any sticky error and interrupt status bits */
    port_write(port_index, P_SERR, 0xFFFFFFFF);
    port_write(port_index, P_IS,   0xFFFFFFFF);

    port_start(port_index);
}


/* ============================================================
 *  Issue Command Slot 0 and Poll for Completion
 *
 *  Clears stale status, writes to P_CI to start the command,
 *  then spins until the HBA either completes the command or
 *  reports a fatal error (in which case we do a COMRESET).
 *
 *  Returns 0 on success, -1 on error or timeout.
 * ============================================================ */
static int port_issue_and_wait(int port_index)
{
    /* Clear stale error and interrupt bits before issuing */
    port_write(port_index, P_SERR, 0xFFFFFFFF);
    port_write(port_index, P_IS,   0xFFFFFFFF);

    /* Set bit 0 of P_CI to issue command slot 0 */
    port_write(port_index, P_CI, 1u);

    for (int timeout = 400000; timeout-- > 0; ) {
        u32 int_status = port_read(port_index, P_IS);

        /* Check for any fatal hardware error */
        if (int_status & P_IS_ANY_FATAL_ERROR) {
            port_reset(port_index);
            return -1;
        }

        /* Check for an ATA-level error */
        if (port_read(port_index, P_TFD) & TFD_ERROR) {
            port_reset(port_index);
            return -1;
        }

        /* Command complete when the HBA clears our slot bit */
        if (!(port_read(port_index, P_CI) & 1u)) {
            return 0;
        }

        io_wait();
    }

    /* Timed out — reset and report failure */
    port_reset(port_index);
    return -1;
}


/* ============================================================
 *  Build an H2D Register FIS
 *
 *  Fills in the command FIS for a 48-bit LBA ATA command.
 *  The FIS sits at the start of the command table's cfis[] array.
 * ============================================================ */
static void build_command_fis(AhciPort *port, u8 ata_command, u64 lba, u16 sector_count)
{
    FisH2D *fis = (FisH2D *)port->cmd_table->command_fis;
    memset(fis, 0, sizeof(FisH2D));

    fis->fis_type       = FIS_TYPE_H2D;
    fis->port_and_c_bit = 0x80;         /* bit 7 = 1: this is a command, not a control FIS */
    fis->command        = ata_command;
    fis->device         = 0x40;         /* bit 6 = 1: LBA mode */

    /* Split the 48-bit LBA across six bytes */
    fis->lba_byte0 = (u8) lba;
    fis->lba_byte1 = (u8)(lba >>  8);
    fis->lba_byte2 = (u8)(lba >> 16);
    fis->lba_byte3 = (u8)(lba >> 24);
    fis->lba_byte4 = (u8)(lba >> 32);
    fis->lba_byte5 = (u8)(lba >> 40);

    fis->sector_count = sector_count;
}


/* ============================================================
 *  Parse the IDENTIFY DEVICE Response
 *
 *  The drive returns 512 bytes of identification data.
 *  We extract the model string (words 27–46) and the total
 *  sector count (words 100–103 for 48-bit LBA, or 60–61 as
 *  a fallback for older 28-bit drives).
 * ============================================================ */
static void parse_identify_response(AhciPort *port, u16 *identify_data)
{
    /* Words 27–46 contain the model string in big-endian byte order */
    for (int word_index = 0; word_index < 20; word_index++) {
        u16 word = identify_data[27 + word_index];
        port->model[word_index * 2]     = (char)(word >> 8);
        port->model[word_index * 2 + 1] = (char)(word & 0xFF);
    }
    port->model[40] = '\0';

    /* Trim trailing spaces */
    for (int i = 39; i >= 0 && port->model[i] == ' '; i--) {
        port->model[i] = '\0';
    }

    /* Words 100–103: total sectors for 48-bit LBA */
    port->sector_count = (u64)identify_data[100]
                       | ((u64)identify_data[101] << 16)
                       | ((u64)identify_data[102] << 32)
                       | ((u64)identify_data[103] << 48);

    /* Fall back to the 28-bit count in words 60–61 if the 48-bit field is zero */
    if (port->sector_count == 0) {
        port->sector_count = (u32)identify_data[60]
                           | ((u32)identify_data[61] << 16);
    }
}


/* ============================================================
 *  ahci_init — main entry point
 *
 *  Called once during early boot with the physical address of
 *  the HBA's MMIO BAR (BAR5 from PCI config space).
 *
 *  Steps:
 *    1. Identity-map the 16KB HBA MMIO region into the page tables.
 *    2. Enable AHCI mode and reset the HBA.
 *    3. For each implemented port with a device present:
 *       a. Spin up the drive.
 *       b. Allocate and configure command list, FIS buffer, and data buffer.
 *       c. Start the port.
 *       d. Issue ATA IDENTIFY to read the model string and sector count.
 * ============================================================ */
void ahci_init(u64 bar5_address)
{
    if (bar5_address == 0) {
        return;
    }

    /* BAR5 may have attribute bits in the low nibble; mask them off */
    u64 hba_phys = bar5_address & ~0xFULL;

    /*
     * Map the 16KB HBA MMIO region.  This is necessary because the HBA
     * typically lives above 4GB on QEMU, outside the early boot identity map.
     * Without this mapping the first register read would page-fault.
     */
    for (u64 offset = 0; offset < 0x4000; offset += PAGE_SIZE) {
        vmm_map(read_cr3(), hba_phys + offset, hba_phys + offset,
                PTE_PRESENT | PTE_WRITE | PTE_NX);
    }

    g_hba_base = (u8 *)(usize)hba_phys;

    /* Enable AHCI mode */
    hba_write(HBA_GHC, hba_read(HBA_GHC) | HBA_GHC_AHCI_ENABLE);

    /* Reset the HBA — it clears the reset bit when it is ready */
    hba_write(HBA_GHC, hba_read(HBA_GHC) | HBA_GHC_HBA_RESET);
    for (int timeout = 50000; timeout-- > 0 && (hba_read(HBA_GHC) & HBA_GHC_HBA_RESET); ) {
        io_wait();
    }

    /* Re-enable AHCI mode (reset cleared it) */
    hba_write(HBA_GHC, hba_read(HBA_GHC) | HBA_GHC_AHCI_ENABLE);

    /* Iterate over all 32 possible port slots */
    u32 ports_implemented = hba_read(HBA_PI);
    g_num_ports = 0;

    for (int i = 0; i < 32 && g_num_ports < MAX_PORTS; i++) {
        /* Skip ports the HBA says are not implemented */
        if (!(ports_implemented & (1u << i))) {
            continue;
        }

        u32 port_base = 0x100u + (u32)i * 0x80u;

        /* Skip ports with no device attached (DET field must be 3) */
        u32 ssts = *(volatile u32 *)(g_hba_base + port_base + P_SSTS);
        if ((ssts & 0xFu) != SSTS_DET_DEVICE_PRESENT) {
            continue;
        }

        /* Request spin-up if the drive is in power-management standby */
        u32 port_cmd = *(volatile u32 *)(g_hba_base + port_base + P_CMD);
        if (!(port_cmd & PCMD_SPIN_UP_DEVICE)) {
            *(volatile u32 *)(g_hba_base + port_base + P_CMD)
                = port_cmd | PCMD_SPIN_UP_DEVICE | PCMD_POWER_ON_DEVICE;

            for (volatile int t = 10000; t-- > 0; ) {
                io_wait();
            }
        }

        /* Initialize the port state structure */
        AhciPort *port       = &g_ports[g_num_ports];
        port->port_reg_offset = port_base;
        port->present         = 1;
        port->sector_count    = 0;

        port_stop(g_num_ports);

        /* Allocate DMA-accessible buffers with required alignments */
        port->cmd_list  = (AhciCmdHeader *)alloc_aligned(sizeof(AhciCmdHeader) * 32, 1024);
        port->fis_buf   = (u8 *)           alloc_aligned(256, 256);
        port->cmd_table = (AhciCmdTable *) alloc_aligned(sizeof(AhciCmdTable), 128);
        port->data_buf  = (u8 *)           alloc_aligned(512 * MAX_SECTORS_PER_CMD, 512);

        if (!port->cmd_list || !port->fis_buf || !port->cmd_table || !port->data_buf) {
            print_str("[AHCI] memory allocation failed\r\n");
            continue;
        }

        memset(port->cmd_list,  0, sizeof(AhciCmdHeader) * 32);
        memset(port->fis_buf,   0, 256);
        memset(port->cmd_table, 0, sizeof(AhciCmdTable));

        /* Tell the HBA where the command list and FIS buffer live */
        u64 cmd_list_phys = (u64)(usize)port->cmd_list;
        u64 fis_buf_phys  = (u64)(usize)port->fis_buf;

        *(volatile u32 *)(g_hba_base + port_base + P_CLB)  = (u32)(cmd_list_phys & 0xFFFFFFFF);
        *(volatile u32 *)(g_hba_base + port_base + P_CLBU) = (u32)(cmd_list_phys >> 32);
        *(volatile u32 *)(g_hba_base + port_base + P_FB)   = (u32)(fis_buf_phys  & 0xFFFFFFFF);
        *(volatile u32 *)(g_hba_base + port_base + P_FBU)  = (u32)(fis_buf_phys  >> 32);

        /* Point command slot 0 at the command table */
        u64 cmd_table_phys = (u64)(usize)port->cmd_table;
        port->cmd_list[0].cmd_table_addr_low  = (u32)(cmd_table_phys & 0xFFFFFFFF);
        port->cmd_list[0].cmd_table_addr_high = (u32)(cmd_table_phys >> 32);

        port_start(g_num_ports);

        /* Identify the drive to get its model name and capacity */
        u8 identify_buf[512];
        if (ahci_identify(g_num_ports, identify_buf) == 0) {
            parse_identify_response(port, (u16 *)identify_buf);
        }

        print_str("[AHCI] Port ");
        print_hex_byte((u8)g_num_ports);
        print_str(": ");
        print_str(port->model);
        print_str("\r\n");

        g_num_ports++;
    }

    print_str("[AHCI] ");
    print_hex_byte((u8)g_num_ports);
    print_str(" drive(s) found\r\n");
}


/* ============================================================
 *  Sector Read (multi-sector)
 *
 *  Reads `sector_count` consecutive 512-byte sectors starting
 *  at `lba` into the caller's buffer using ATA READ DMA EXT.
 *
 *  Returns 0 on success, or a negative error code.
 * ============================================================ */
i64 ahci_read_sectors(int port_index, u64 lba, u16 sector_count, void *out_buf)
{
    if (port_index < 0 || port_index >= g_num_ports || !g_ports[port_index].present) {
        return ENODEV;
    }
    if (sector_count == 0 || sector_count > MAX_SECTORS_PER_CMD) {
        return EINVAL;
    }

    AhciPort *port = &g_ports[port_index];

    /* Command header: FIS length = 5 DWORDs, direction = read */
    port->cmd_list[0].flags            = 5;    /* FIS is 5 DWORDs = 20 bytes */
    port->cmd_list[0].prdt_length      = sector_count;
    port->cmd_list[0].bytes_transferred = 0;

    /* One PRDT entry per sector, each pointing to a 512-byte sub-buffer */
    for (u16 s = 0; s < sector_count; s++) {
        u64 buf_phys = (u64)(usize)(port->data_buf + (usize)s * 512);

        port->cmd_table->prdt[s].data_addr_low       = (u32)(buf_phys & 0xFFFFFFFF);
        port->cmd_table->prdt[s].data_addr_high      = (u32)(buf_phys >> 32);
        port->cmd_table->prdt[s].reserved            = 0;
        port->cmd_table->prdt[s].byte_count_and_irq  = 511;  /* 512 bytes - 1 */
    }

    build_command_fis(port, ATA_READ_DMA_EXT, lba, sector_count);

    if (port_issue_and_wait(port_index) != 0) {
        return ETIMEDOUT;
    }

    memcpy(out_buf, port->data_buf, (usize)sector_count * 512);
    return 0;
}

/* Convenience wrapper: read exactly one sector using a 32-bit LBA */
i64 ahci_read_sector(int port_index, u32 lba, void *out_buf)
{
    return ahci_read_sectors(port_index, (u64)lba, 1, out_buf);
}


/* ============================================================
 *  Sector Write (multi-sector)
 *
 *  Writes `sector_count` consecutive 512-byte sectors starting
 *  at `lba` from the caller's buffer using ATA WRITE DMA EXT.
 *
 *  Returns 0 on success, or a negative error code.
 * ============================================================ */
i64 ahci_write_sectors(int port_index, u64 lba, u16 sector_count, const void *in_buf)
{
    if (port_index < 0 || port_index >= g_num_ports || !g_ports[port_index].present) {
        return ENODEV;
    }
    if (sector_count == 0 || sector_count > MAX_SECTORS_PER_CMD) {
        return EINVAL;
    }

    AhciPort *port = &g_ports[port_index];

    /* Copy caller data into the DMA buffer before issuing the command */
    memcpy(port->data_buf, in_buf, (usize)sector_count * 512);

    /* Command header: FIS length = 5 DWORDs, bit 6 set = write direction */
    port->cmd_list[0].flags             = 5 | (1 << 6);
    port->cmd_list[0].prdt_length       = sector_count;
    port->cmd_list[0].bytes_transferred  = 0;

    /* One PRDT entry per sector */
    for (u16 s = 0; s < sector_count; s++) {
        u64 buf_phys = (u64)(usize)(port->data_buf + (usize)s * 512);

        port->cmd_table->prdt[s].data_addr_low      = (u32)(buf_phys & 0xFFFFFFFF);
        port->cmd_table->prdt[s].data_addr_high     = (u32)(buf_phys >> 32);
        port->cmd_table->prdt[s].reserved           = 0;
        port->cmd_table->prdt[s].byte_count_and_irq = 511;  /* 512 bytes - 1 */
    }

    build_command_fis(port, ATA_WRITE_DMA_EXT, lba, sector_count);

    if (port_issue_and_wait(port_index) != 0) {
        return ETIMEDOUT;
    }

    return 0;
}

/* Convenience wrapper: write exactly one sector using a 32-bit LBA */
i64 ahci_write_sector(int port_index, u32 lba, const void *in_buf)
{
    return ahci_write_sectors(port_index, (u64)lba, 1, in_buf);
}


/* ============================================================
 *  Cache Flush
 *
 *  Sends ATA FLUSH CACHE EXT, which forces the drive to write
 *  its internal write cache to persistent storage.  Should be
 *  called before shutdown or unmount.
 *
 *  Returns 0 on success, or a negative error code.
 * ============================================================ */
i64 ahci_flush(int port_index)
{
    if (port_index < 0 || port_index >= g_num_ports || !g_ports[port_index].present) {
        return ENODEV;
    }

    AhciPort *port = &g_ports[port_index];

    /* FLUSH EXT transfers no data, so there are no PRDT entries */
    port->cmd_list[0].flags             = 5;
    port->cmd_list[0].prdt_length       = 0;
    port->cmd_list[0].bytes_transferred  = 0;

    /* Build the FIS manually since build_command_fis sets a device field
     * that is not appropriate for a flush (no LBA needed) */
    FisH2D *fis = (FisH2D *)port->cmd_table->command_fis;
    memset(fis, 0, sizeof(FisH2D));
    fis->fis_type       = FIS_TYPE_H2D;
    fis->port_and_c_bit = 0x80;
    fis->command        = ATA_FLUSH_EXT;
    fis->device         = 0x40;

    if (port_issue_and_wait(port_index) != 0) {
        return ETIMEDOUT;
    }

    return 0;
}


/* ============================================================
 *  Identify Device
 *
 *  Sends ATA IDENTIFY DEVICE and returns the raw 512-byte
 *  response in `out_buf`.  Called during init and also
 *  available to other subsystems.
 *
 *  Returns 0 on success, or a negative error code.
 * ============================================================ */
i64 ahci_identify(int port_index, void *out_buf)
{
    if (port_index < 0 || port_index >= g_num_ports || !g_ports[port_index].present) {
        return ENODEV;
    }

    AhciPort *port = &g_ports[port_index];

    port->cmd_list[0].flags             = 5;
    port->cmd_list[0].prdt_length       = 1;
    port->cmd_list[0].bytes_transferred  = 0;

    /* Single PRDT entry for the 512-byte IDENTIFY response */
    u64 buf_phys = (u64)(usize)port->data_buf;
    port->cmd_table->prdt[0].data_addr_low      = (u32)(buf_phys & 0xFFFFFFFF);
    port->cmd_table->prdt[0].data_addr_high     = (u32)(buf_phys >> 32);
    port->cmd_table->prdt[0].reserved           = 0;
    port->cmd_table->prdt[0].byte_count_and_irq = 511;  /* 512 bytes - 1 */

    /* IDENTIFY uses no LBA and no sector count */
    FisH2D *fis = (FisH2D *)port->cmd_table->command_fis;
    memset(fis, 0, sizeof(FisH2D));
    fis->fis_type       = FIS_TYPE_H2D;
    fis->port_and_c_bit = 0x80;
    fis->command        = ATA_IDENTIFY;
    fis->device         = 0;    /* no LBA bit for IDENTIFY */

    if (port_issue_and_wait(port_index) != 0) {
        return ETIMEDOUT;
    }

    memcpy(out_buf, port->data_buf, 512);
    return 0;
}


/* ============================================================
 *  Public Accessors
 * ============================================================ */

int ahci_get_port_count(void)
{
    return g_num_ports;
}

u64 ahci_get_sector_count(int port_index)
{
    if (port_index < 0 || port_index >= g_num_ports) {
        return 0;
    }
    return g_ports[port_index].sector_count;
}

const char *ahci_get_model(int port_index)
{
    if (port_index < 0 || port_index >= g_num_ports) {
        return "";
    }
    return g_ports[port_index].model;
}
