/* ================================================================
 *  Systrix OS — kernel/acpi.c
 *  ACPI: RSDP→XSDT/RSDT, FADT (PM1a shutdown/reboot), MADT (IOAPIC),
 *        MCFG (PCIe ECAM), checksum validation.
 * ================================================================ */
#include "../include/kernel.h"

/* ── ACPI table signatures ───────────────────────────────────── */
#define SIG8(s)  (*(u64*)(s))
#define SIG4(s)  (*(u32*)(s))
#define RSDP_SIG SIG8("RSD PTR ")
#define XSDT_SIG SIG4("XSDT")
#define RSDT_SIG SIG4("RSDT")
#define FADT_SIG SIG4("FACP")
#define MADT_SIG SIG4("APIC")
#define MCFG_SIG SIG4("MCFG")
#define HPET_SIG SIG4("HPET")

/* ── Packed structures ───────────────────────────────────────── */
typedef struct __attribute__((packed)){
    u64 sig; u8 csum; u8 oem[6]; u8 rev;
    u32 rsdt_pa; u32 len; u64 xsdt_pa; u8 xcsum; u8 rsvd[3];
} AcpiRsdp;

typedef struct __attribute__((packed)){
    u32 sig; u32 len; u8 rev; u8 csum;
    u8 oem[6]; u8 oem_tbl[8]; u32 oem_rev; u32 cid; u32 crev;
} AcpiHdr;

typedef struct __attribute__((packed)){
    AcpiHdr hdr;
    u32 fw_ctrl; u32 dsdt; u8 rsvd; u8 pm_profile; u16 sci_int;
    u32 smi_cmd; u8 acpi_en; u8 acpi_dis; u8 s4bios; u8 pstate;
    u32 pm1a_evt; u32 pm1b_evt; u32 pm1a_cnt; u32 pm1b_cnt;
    u32 pm2_cnt; u32 pm_tmr; u32 gpe0; u32 gpe1;
    u8 pm1_el; u8 pm1_cl; u8 pm2_cl; u8 pm_tl;
    u8 gpe0l; u8 gpe1l; u8 gpe1base; u8 cst;
    u16 plvl2; u16 plvl3; u16 flushsz; u16 flushstr;
    u8 duty_off; u8 duty_wid; u8 day; u8 mon; u8 cent;
    u16 boot_arch; u8 rsvd2; u32 flags;
} AcpiFadt;

typedef struct __attribute__((packed)){
    AcpiHdr hdr; u32 lapic_pa; u32 flags;
} AcpiMadt;

typedef struct __attribute__((packed)){u8 type; u8 len;} MadtEntry;
typedef struct __attribute__((packed)){MadtEntry e; u8 ioapic_id; u8 rsvd; u32 ioapic_pa; u32 gsi_base;} MadtIoapic;
typedef struct __attribute__((packed)){MadtEntry e; u8 bus; u8 src; u32 gsi; u16 flags;} MadtIsovr;
typedef struct __attribute__((packed)){MadtEntry e; u8 proc_id; u8 apic_id; u32 flags;} MadtLapic;

typedef struct __attribute__((packed)){
    AcpiHdr hdr; u64 rsvd;
    struct{u64 base; u16 seg; u8 s_bus; u8 e_bus; u32 rsvd2;} entries[1];
} AcpiMcfg;

/* ── Global pointers ─────────────────────────────────────────── */
static AcpiRsdp *g_rsdp=0;
static AcpiFadt *g_fadt=0;
static AcpiMadt *g_madt=0;
static AcpiMcfg *g_mcfg=0;

static u32 g_ioapic_pa  = 0;
static u32 g_ioapic_gsi = 0;
static u32 g_lapic_pa   = 0;

/* ── Checksum validation ─────────────────────────────────────── */
static int acpi_csum_ok(void *p, u32 len){
    u8 *b=(u8*)p; u8 s=0;
    for(u32 i=0;i<len;i++) s+=b[i];
    return s==0;
}

/* ── RSDP search: EBDA and BIOS ROM ─────────────────────────── */
static AcpiRsdp *find_rsdp(void){
    /* EBDA: first 1 KB starting at (u16 at 0x40E)<<4 */
    u16 ebda_seg=*(volatile u16*)0x40Eu;
    u64 ebda=(u64)ebda_seg<<4;
    for(u64 a=ebda;a<ebda+0x400;a+=16){
        AcpiRsdp *r=(AcpiRsdp*)a;
        if(r->sig==RSDP_SIG && acpi_csum_ok(r,20)) return r;
    }
    /* BIOS ROM 0xE0000–0xFFFFF */
    for(u64 a=0xE0000;a<0x100000;a+=16){
        AcpiRsdp *r=(AcpiRsdp*)a;
        if(r->sig==RSDP_SIG && acpi_csum_ok(r,20)) return r;
    }
    return 0;
}

/* ── Parse a single SDT ──────────────────────────────────────── */
static void parse_sdt(AcpiHdr *h){
    if(!h||!acpi_csum_ok(h,h->len)) return;
    if(h->sig==FADT_SIG) g_fadt=(AcpiFadt*)h;
    else if(h->sig==MADT_SIG) g_madt=(AcpiMadt*)h;
    else if(h->sig==MCFG_SIG) g_mcfg=(AcpiMcfg*)h;
}

/* ── Walk MADT ───────────────────────────────────────────────── */
static void parse_madt(void){
    if(!g_madt) return;
    g_lapic_pa=g_madt->lapic_pa;
    u8 *p=(u8*)(g_madt+1), *end=(u8*)g_madt+g_madt->hdr.len;
    while(p<end){
        MadtEntry *e=(MadtEntry*)p;
        if(e->type==1){ /* I/O APIC */
            MadtIoapic *ia=(MadtIoapic*)p;
            if(!g_ioapic_pa){g_ioapic_pa=ia->ioapic_pa;g_ioapic_gsi=ia->gsi_base;}
        }
        p+=e->len;
    }
}

/* ── acpi_init ───────────────────────────────────────────────── */
void acpi_init(void){
    g_rsdp=find_rsdp();
    if(!g_rsdp){kprintf("[ACPI] no RSDP\r\n");return;}

    /* Prefer XSDT (ACPI 2.0+) */
    if(g_rsdp->rev>=2 && g_rsdp->xsdt_pa){
        AcpiHdr *x=(AcpiHdr*)(usize)g_rsdp->xsdt_pa;
        if(x->sig==XSDT_SIG && acpi_csum_ok(x,x->len)){
            u64 *entries=(u64*)(x+1);
            u32 n=(x->len-sizeof(AcpiHdr))/8;
            for(u32 i=0;i<n;i++) parse_sdt((AcpiHdr*)(usize)entries[i]);
            goto done;
        }
    }
    /* Fallback: RSDT */
    if(g_rsdp->rsdt_pa){
        AcpiHdr *r=(AcpiHdr*)(usize)g_rsdp->rsdt_pa;
        if(r->sig==RSDT_SIG && acpi_csum_ok(r,r->len)){
            u32 *entries=(u32*)(r+1);
            u32 n=(r->len-sizeof(AcpiHdr))/4;
            for(u32 i=0;i<n;i++) parse_sdt((AcpiHdr*)(usize)entries[i]);
        }
    }
done:
    parse_madt();

    /* Hand MCFG base to PCI driver */
    if(g_mcfg){
        pci_set_ecam(g_mcfg->entries[0].base, g_mcfg->entries[0].s_bus);
        kprintf("[ACPI] PCIe ECAM @ ");
        print_hex_byte((u8)(g_mcfg->entries[0].base>>24));
        print_hex_byte((u8)(g_mcfg->entries[0].base>>16));
        print_hex_byte((u8)(g_mcfg->entries[0].base>>8));
        print_hex_byte((u8) g_mcfg->entries[0].base);
        kprintf("\r\n");
    }

    kprintf("[ACPI] OK");
    if(g_fadt) kprintf(" FADT");
    if(g_madt) kprintf(" MADT");
    if(g_ioapic_pa){kprintf(" IOAPIC@");print_hex_byte((u8)(g_ioapic_pa>>24));print_hex_byte((u8)(g_ioapic_pa>>16));print_hex_byte((u8)(g_ioapic_pa>>8));print_hex_byte((u8)g_ioapic_pa);}
    kprintf("\r\n");
}

/* ── ACPI power management ───────────────────────────────────── */
void acpi_shutdown(void){
    /* Try FADT PM1a control block — write SLP_TYPa|SLP_EN */
    if(g_fadt && g_fadt->pm1a_cnt){
        /* SLP_TYP for S5 is firmware-specific; QEMU uses 0 for S5 via DSDT.
         * Standard approach: write 0x2000 (SLP_EN bit) + SLP_TYP from S5 pkg.
         * On QEMU the value is typically 0x2000 with SLP_TYP=5→bits[12:10]=101. */
        outw((u16)g_fadt->pm1a_cnt, 0x3C00u | (1u<<13)); /* try S5 */
        io_wait();
        outw((u16)g_fadt->pm1a_cnt, 0x2000u); /* fallback */
    }
    /* QEMU-specific: write 0x2000 to 0x604 (PIIX4 ACPI PM) */
    outw(0x604, 0x2000);
    /* Last resort: triple fault */
    __asm__ volatile("cli; hlt");
}

void acpi_reboot(void){
    if(g_fadt && g_fadt->smi_cmd && g_fadt->pm1a_cnt){
        /* Reset via FADT reset register (ACPI 3.0+) — offset 0x74 in FADT */
    }
    /* PS/2 controller reset line */
    outb(0x64, 0xFE);
    io_wait();
    /* Triple-fault fallback */
    __asm__ volatile("cli; lidt (0); int $0");
}

/* ── I/O APIC ────────────────────────────────────────────────── */
void acpi_ioapic_init(void){
    if(!g_ioapic_pa){kprintf("[ACPI] no IOAPIC\r\n");return;}
}

static volatile u32 *ioapic_reg(u32 off){return (volatile u32*)(usize)(g_ioapic_pa+off);}
u32 acpi_ioapic_read(u32 reg){
    if(!g_ioapic_pa) return 0;
    *ioapic_reg(0x00)=reg; return *ioapic_reg(0x10);
}
void acpi_ioapic_write(u32 reg,u32 val){
    if(!g_ioapic_pa) return;
    *ioapic_reg(0x00)=reg; *ioapic_reg(0x10)=val;
}

u32 acpi_get_ioapic_addr(void){return g_ioapic_pa;}
u32 acpi_get_ioapic_gsi_base(void){return g_ioapic_gsi;}
