/* ============================================================
 *  Systrix OS — kernel/acpi.c
 *
 *  ACPI (Advanced Configuration and Power Interface) driver.
 *
 *  Responsibilities:
 *    - Find the RSDP (Root System Description Pointer)
 *    - Walk the XSDT or RSDT to discover hardware tables
 *    - Parse the FADT (power management ports)
 *    - Parse the MADT (find the I/O APIC and Local APIC)
 *    - Parse the MCFG (PCIe ECAM base address)
 *    - Provide shutdown and reboot routines
 *    - Provide I/O APIC register read/write helpers
 * ============================================================ */

#include "../include/kernel.h"


/* ============================================================
 *  Table signature helpers
 *
 *  ACPI tables are identified by 4- or 8-byte ASCII signatures
 *  stored at the start of each structure.  We cast a string
 *  literal to an integer type so we can compare in one shot
 *  instead of calling memcmp.
 * ============================================================ */

#define SIG8(str)  (*(u64 *)(str))   /* read 8 bytes as a u64 */
#define SIG4(str)  (*(u32 *)(str))   /* read 4 bytes as a u32 */

#define RSDP_SIG   SIG8("RSD PTR ")  /* RSDP — 8-byte signature */
#define XSDT_SIG   SIG4("XSDT")
#define RSDT_SIG   SIG4("RSDT")
#define FADT_SIG   SIG4("FACP")      /* FADT is stored under the tag "FACP" */
#define MADT_SIG   SIG4("APIC")      /* MADT is stored under the tag "APIC" */
#define MCFG_SIG   SIG4("MCFG")
#define HPET_SIG   SIG4("HPET")


/* ============================================================
 *  ACPI data structures
 *
 *  All of these are defined by the ACPI specification and must
 *  be packed — the firmware writes them directly into memory
 *  with no padding between fields.
 * ============================================================ */

/*
 * Root System Description Pointer (RSDP)
 * Found by scanning the EBDA and BIOS ROM regions.
 * Points to either the XSDT (ACPI 2.0+) or the older RSDT.
 */
typedef struct __attribute__((packed)) {
    u64 signature;          /* "RSD PTR " */
    u8  checksum;           /* checksum covering the first 20 bytes */
    u8  oem_id[6];
    u8  revision;           /* 0 = ACPI 1.0,  >=2 = ACPI 2.0+ */
    u32 rsdt_phys_addr;     /* physical address of the RSDT */

    /* The fields below are only valid when revision >= 2 */
    u32 length;
    u64 xsdt_phys_addr;     /* physical address of the XSDT */
    u8  extended_checksum;  /* checksum covering the full structure */
    u8  reserved[3];
} AcpiRsdp;

/*
 * Generic SDT header — every ACPI table starts with this.
 */
typedef struct __attribute__((packed)) {
    u32 signature;          /* 4-byte ASCII table ID */
    u32 length;             /* total length of the table in bytes */
    u8  revision;
    u8  checksum;           /* all bytes of the table must sum to 0 */
    u8  oem_id[6];
    u8  oem_table_id[8];
    u32 oem_revision;
    u32 creator_id;
    u32 creator_revision;
} AcpiHdr;

/*
 * Fixed ACPI Description Table (FADT / "FACP")
 * Contains I/O port addresses for the PM1 control registers,
 * which we use for ACPI-controlled shutdown and reboot.
 */
typedef struct __attribute__((packed)) {
    AcpiHdr header;

    u32 firmware_ctrl;      /* physical address of the FACS */
    u32 dsdt;               /* physical address of the DSDT */
    u8  reserved_1;
    u8  pm_profile;         /* preferred power-management profile */
    u16 sci_interrupt;      /* IRQ used for SCI */
    u32 smi_command_port;   /* I/O port for sending SMI commands */
    u8  acpi_enable;        /* value to write to smi_command_port to enable ACPI */
    u8  acpi_disable;       /* value to write to smi_command_port to disable ACPI */
    u8  s4bios_request;
    u8  pstate_control;
    u32 pm1a_event_block;
    u32 pm1b_event_block;
    u32 pm1a_control_block; /* I/O port we write to for S-state transitions */
    u32 pm1b_control_block;
    u32 pm2_control_block;
    u32 pm_timer_block;
    u32 gpe0_block;
    u32 gpe1_block;
    u8  pm1_event_length;
    u8  pm1_control_length;
    u8  pm2_control_length;
    u8  pm_timer_length;
    u8  gpe0_length;
    u8  gpe1_length;
    u8  gpe1_base;
    u8  cst_control;
    u16 c2_latency;
    u16 c3_latency;
    u16 flush_size;
    u16 flush_stride;
    u8  duty_offset;
    u8  duty_width;
    u8  rtc_day_alarm;
    u8  rtc_month_alarm;
    u8  rtc_century;
    u16 boot_arch_flags;
    u8  reserved_2;
    u32 flags;
} AcpiFadt;

/*
 * Multiple APIC Description Table (MADT / "APIC")
 * Lists all interrupt controllers on the system.
 */
typedef struct __attribute__((packed)) {
    AcpiHdr header;
    u32     local_apic_phys_addr;   /* default physical address of the Local APIC */
    u32     flags;                  /* bit 0: dual-8259 PICs present */
} AcpiMadt;

/* Every entry inside the MADT starts with this two-byte header */
typedef struct __attribute__((packed)) {
    u8 type;    /* entry type — see constants below */
    u8 length;  /* total length of this entry */
} MadtEntry;

/* MADT entry type 0 — Processor Local APIC */
typedef struct __attribute__((packed)) {
    MadtEntry header;
    u8  processor_id;
    u8  apic_id;
    u32 flags;          /* bit 0: processor is usable */
} MadtLocalApic;

/* MADT entry type 1 — I/O APIC */
typedef struct __attribute__((packed)) {
    MadtEntry header;
    u8  ioapic_id;
    u8  reserved;
    u32 ioapic_phys_addr;   /* physical address of the I/O APIC registers */
    u32 gsi_base;           /* first global system interrupt this I/O APIC handles */
} MadtIoapic;

/* MADT entry type 2 — Interrupt Source Override */
typedef struct __attribute__((packed)) {
    MadtEntry header;
    u8  bus;        /* always 0 (ISA) */
    u8  source_irq;
    u32 global_irq;
    u16 flags;
} MadtIsovr;

/*
 * PCI Express Memory-mapped Configuration (MCFG)
 * Describes the ECAM (Enhanced Configuration Access Mechanism) windows
 * that let us access PCI config space via MMIO instead of port I/O.
 */
typedef struct __attribute__((packed)) {
    AcpiHdr header;
    u64     reserved;

    /* One entry per PCI segment group */
    struct __attribute__((packed)) {
        u64 base_address;   /* physical base of the ECAM window */
        u16 pci_segment;    /* PCI segment group number */
        u8  start_bus;
        u8  end_bus;
        u32 reserved;
    } entries[1];           /* variable length — may contain more entries */
} AcpiMcfg;


/* ============================================================
 *  Module-private globals
 * ============================================================ */

static AcpiRsdp *g_rsdp = NULL;
static AcpiFadt *g_fadt = NULL;
static AcpiMadt *g_madt = NULL;
static AcpiMcfg *g_mcfg = NULL;

/* I/O APIC info extracted from the MADT */
static u32 g_ioapic_phys_addr = 0;
static u32 g_ioapic_gsi_base  = 0;

/* Local APIC physical address from the MADT header */
static u32 g_lapic_phys_addr  = 0;


/* ============================================================
 *  Checksum validation
 *
 *  ACPI requires that all bytes of a table sum to 0 (mod 256).
 *  Returns 1 if valid, 0 if corrupt.
 * ============================================================ */
static int acpi_checksum_valid(void *table, u32 length)
{
    u8 *bytes = (u8 *)table;
    u8  sum   = 0;

    for (u32 i = 0; i < length; i++) {
        sum += bytes[i];
    }

    return sum == 0;
}


/* ============================================================
 *  Find the RSDP
 *
 *  The ACPI spec says to search two regions:
 *    1. The first kilobyte of the Extended BIOS Data Area (EBDA),
 *       whose segment address is stored at physical address 0x40E.
 *    2. The BIOS ROM region from 0xE0000 to 0xFFFFF.
 *
 *  In both regions we scan on 16-byte boundaries.
 * ============================================================ */
static AcpiRsdp *find_rsdp(void)
{
    /* --- Search the EBDA ------------------------------------ */
    u16 ebda_segment    = *(volatile u16 *)0x40E;
    u64 ebda_base_addr  = (u64)ebda_segment << 4;

    for (u64 addr = ebda_base_addr; addr < ebda_base_addr + 0x400; addr += 16) {
        AcpiRsdp *candidate = (AcpiRsdp *)addr;

        if (candidate->signature == RSDP_SIG && acpi_checksum_valid(candidate, 20)) {
            return candidate;
        }
    }

    /* --- Search the BIOS ROM region ------------------------- */
    for (u64 addr = 0xE0000; addr < 0x100000; addr += 16) {
        AcpiRsdp *candidate = (AcpiRsdp *)addr;

        if (candidate->signature == RSDP_SIG && acpi_checksum_valid(candidate, 20)) {
            return candidate;
        }
    }

    return NULL;
}


/* ============================================================
 *  Parse a single SDT
 *
 *  Validates the checksum then stores a pointer in the
 *  appropriate global if it is a table we care about.
 * ============================================================ */
static void parse_sdt(AcpiHdr *table)
{
    if (table == NULL) {
        return;
    }

    if (!acpi_checksum_valid(table, table->length)) {
        return;     /* corrupt table — skip it */
    }

    if (table->signature == FADT_SIG) {
        g_fadt = (AcpiFadt *)table;
    } else if (table->signature == MADT_SIG) {
        g_madt = (AcpiMadt *)table;
    } else if (table->signature == MCFG_SIG) {
        g_mcfg = (AcpiMcfg *)table;
    }
    /* Unknown signatures are silently ignored */
}


/* ============================================================
 *  Walk the MADT
 *
 *  Iterates over every variable-length entry and picks out:
 *    - The first I/O APIC (type 1) — gives us the register
 *      base address and the GSI base.
 *    - The Local APIC address is taken from the MADT header.
 *
 *  We deliberately ignore additional I/O APICs; a simple
 *  kernel only needs the first one.
 * ============================================================ */
static void parse_madt(void)
{
    if (g_madt == NULL) {
        return;
    }

    g_lapic_phys_addr = g_madt->local_apic_phys_addr;

    u8 *entry_ptr  = (u8 *)(g_madt + 1);          /* first entry */
    u8 *table_end  = (u8 *)g_madt + g_madt->header.length;

    while (entry_ptr < table_end) {
        MadtEntry *entry = (MadtEntry *)entry_ptr;

        if (entry->type == 1) {
            /* I/O APIC entry */
            MadtIoapic *ioapic = (MadtIoapic *)entry_ptr;

            /* Only record the first I/O APIC we find */
            if (g_ioapic_phys_addr == 0) {
                g_ioapic_phys_addr = ioapic->ioapic_phys_addr;
                g_ioapic_gsi_base  = ioapic->gsi_base;
            }
        }

        entry_ptr += entry->length;     /* advance to next entry */
    }
}


/* ============================================================
 *  acpi_init — main entry point
 *
 *  Called once during early kernel boot.  Finds the RSDP,
 *  walks the SDT, parses the MADT, and hands the PCIe ECAM
 *  base to the PCI driver.
 * ============================================================ */
void acpi_init(void)
{
    g_rsdp = find_rsdp();

    if (g_rsdp == NULL) {
        print_str("[ACPI] RSDP not found\r\n");
        return;
    }

    /* Prefer the XSDT (ACPI 2.0+) because it uses 64-bit pointers */
    if (g_rsdp->revision >= 2 && g_rsdp->xsdt_phys_addr != 0) {
        AcpiHdr *xsdt = (AcpiHdr *)(usize)g_rsdp->xsdt_phys_addr;

        if (xsdt->signature == XSDT_SIG && acpi_checksum_valid(xsdt, xsdt->length)) {
            u64 *entries    = (u64 *)(xsdt + 1);
            u32  num_tables = (xsdt->length - sizeof(AcpiHdr)) / 8;

            for (u32 i = 0; i < num_tables; i++) {
                parse_sdt((AcpiHdr *)(usize)entries[i]);
            }
            goto tables_done;
        }
    }

    /* Fall back to the RSDT (32-bit pointers) */
    if (g_rsdp->rsdt_phys_addr != 0) {
        AcpiHdr *rsdt = (AcpiHdr *)(usize)g_rsdp->rsdt_phys_addr;

        if (rsdt->signature == RSDT_SIG && acpi_checksum_valid(rsdt, rsdt->length)) {
            u32 *entries    = (u32 *)(rsdt + 1);
            u32  num_tables = (rsdt->length - sizeof(AcpiHdr)) / 4;

            for (u32 i = 0; i < num_tables; i++) {
                parse_sdt((AcpiHdr *)(usize)entries[i]);
            }
        }
    }

tables_done:
    parse_madt();

    /* Give the PCIe ECAM base address to the PCI subsystem */
    if (g_mcfg != NULL) {
        u64 ecam_base = g_mcfg->entries[0].base_address;
        u8  start_bus = g_mcfg->entries[0].start_bus;

        pci_set_ecam(ecam_base, start_bus);

        print_str("[ACPI] PCIe ECAM base @ ");
        print_hex_byte((u8)(ecam_base >> 24));
        print_hex_byte((u8)(ecam_base >> 16));
        print_hex_byte((u8)(ecam_base >>  8));
        print_hex_byte((u8) ecam_base       );
        print_str("\r\n");
    }

    /* Report which tables were found */
    print_str("[ACPI] OK");
    if (g_fadt) { print_str(" FADT"); }
    if (g_madt) { print_str(" MADT"); }

    if (g_ioapic_phys_addr != 0) {
        print_str(" IOAPIC@");
        print_hex_byte((u8)(g_ioapic_phys_addr >> 24));
        print_hex_byte((u8)(g_ioapic_phys_addr >> 16));
        print_hex_byte((u8)(g_ioapic_phys_addr >>  8));
        print_hex_byte((u8) g_ioapic_phys_addr       );
    }

    print_str("\r\n");
}


/* ============================================================
 *  ACPI power management
 * ============================================================ */

/*
 * acpi_shutdown — attempt a clean ACPI power-off.
 *
 * Strategy (in order of preference):
 *   1. Write the S5 sleep state to the PM1a control block.
 *   2. Write the QEMU-specific shutdown port 0x604.
 *   3. Halt the CPU (should never be reached on real hardware).
 *
 * The SLP_TYP value for S5 is firmware-specific and should be
 * read from the DSDT's _S5 package.  On QEMU the correct value
 * happens to be 5 (bits [12:10] = 101), giving 0x3C00 ORed
 * with SLP_EN (bit 13) = 0x3C00 | 0x2000 = 0x5C00.
 * A simpler write of 0x2000 (only SLP_EN) is used as a fallback.
 */
void acpi_shutdown(void)
{
    if (g_fadt != NULL && g_fadt->pm1a_control_block != 0) {
        u16 pm1a_port = (u16)g_fadt->pm1a_control_block;

        /* Attempt S5 with SLP_TYP=5 and SLP_EN=1 */
        outw(pm1a_port, 0x3C00u | (1u << 13));
        io_wait();

        /* Fallback: just set SLP_EN with no SLP_TYP */
        outw(pm1a_port, 0x2000u);
    }

    /* QEMU PIIX4 ACPI power management port */
    outw(0x604, 0x2000);

    /* Should never reach here — halt the CPU */
    __asm__ volatile("cli; hlt");
}

/*
 * acpi_reboot — reset the system.
 *
 * Strategy (in order of preference):
 *   1. Pulse the PS/2 controller reset line (port 0x64, command 0xFE).
 *      This works on virtually all PC-compatible hardware.
 *   2. Triple-fault the CPU as a last resort.
 *
 * Note: ACPI 3.0 added a "reset register" field to the FADT (offset 0x74).
 * That path is not yet implemented here.
 */
void acpi_reboot(void)
{
    /* Pulse the PS/2 keyboard controller reset line */
    outb(0x64, 0xFE);
    io_wait();

    /* Triple-fault fallback: load a null IDT and trigger an interrupt */
    __asm__ volatile("cli; lidt (0); int $0");
}


/* ============================================================
 *  I/O APIC interface
 *
 *  The I/O APIC has two MMIO registers:
 *    offset 0x00 — IOREGSEL (index register, write the register number here)
 *    offset 0x10 — IOWIN    (data window, read or write the selected register)
 * ============================================================ */

void acpi_ioapic_init(void)
{
    if (g_ioapic_phys_addr == 0) {
        print_str("[ACPI] no I/O APIC found\r\n");
        return;
    }

    /*
     * Nothing to do yet — the I/O APIC is already memory-mapped
     * at the physical address we recorded from the MADT.
     * IRQ routing setup happens in the interrupt subsystem.
     */
}

/* Return a pointer to the MMIO register at the given byte offset */
static volatile u32 *ioapic_mmio(u32 offset)
{
    return (volatile u32 *)(usize)(g_ioapic_phys_addr + offset);
}

/* Read an I/O APIC internal register by index */
u32 acpi_ioapic_read(u32 reg_index)
{
    if (g_ioapic_phys_addr == 0) {
        return 0;
    }

    *ioapic_mmio(0x00) = reg_index;     /* select the register */
    return *ioapic_mmio(0x10);          /* read the value */
}

/* Write an I/O APIC internal register by index */
void acpi_ioapic_write(u32 reg_index, u32 value)
{
    if (g_ioapic_phys_addr == 0) {
        return;
    }

    *ioapic_mmio(0x00) = reg_index;     /* select the register */
    *ioapic_mmio(0x10) = value;         /* write the value */
}

/* Accessors for other subsystems that need the I/O APIC location */
u32 acpi_get_ioapic_addr(void)     { return g_ioapic_phys_addr; }
u32 acpi_get_ioapic_gsi_base(void) { return g_ioapic_gsi_base;  }
