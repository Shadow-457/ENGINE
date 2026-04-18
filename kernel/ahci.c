/* ================================================================
 *  ENGINE OS — kernel/ahci.c
 *  AHCI SATA driver: HBA init, port start/stop, ATA identify,
 *  48-bit LBA DMA read/write using proper command-list + FIS + PRDT.
 *
 *  Follows AHCI 1.3.1 spec.
 * ================================================================ */
#include "../include/kernel.h"

/* ── HBA register offsets ────────────────────────────────────── */
#define HBA_CAP     0x00
#define HBA_GHC     0x04
#define HBA_IS      0x08
#define HBA_PI      0x0C
#define HBA_VS      0x10
#define HBA_GHC_AE  (1u<<31)  /* AHCI Enable */
#define HBA_GHC_HR  (1u<<0)   /* HBA Reset   */

/* Port register offsets (base = HBA_base + 0x100 + port*0x80) */
#define P_CLB   0x00  /* Command List Base (phys lo) */
#define P_CLBU  0x04  /* Command List Base (phys hi) */
#define P_FB    0x08  /* FIS Base (phys lo) */
#define P_FBU   0x0C
#define P_IS    0x10  /* Interrupt Status */
#define P_IE    0x14
#define P_CMD   0x18
#define P_TFD   0x20  /* Task File Data */
#define P_SIG   0x24
#define P_SSTS  0x28
#define P_SCTL  0x2C
#define P_SERR  0x30
#define P_SACT  0x34
#define P_CI    0x38  /* Command Issue */

#define PCMD_ST   (1u<<0)  /* Start */
#define PCMD_FRE  (1u<<4)  /* FIS Receive Enable */
#define PCMD_FR   (1u<<14) /* FIS Receive Running */
#define PCMD_CR   (1u<<15) /* Command List Running */

#define SSTS_DET_PRESENT 0x3u

/* ── FIS types ───────────────────────────────────────────────── */
#define FIS_TYPE_H2D 0x27

/* ── ATA commands ────────────────────────────────────────────── */
#define ATA_READ_DMA_EXT  0x25
#define ATA_WRITE_DMA_EXT 0x35
#define ATA_IDENTIFY      0xEC
#define ATA_FLUSH_EXT     0xEA

/* ── DMA structures (must be in physically-accessible memory) ── */
/* Command Header (32 bytes) */
typedef struct __attribute__((packed,aligned(32))){
    u16 cfl_flags; /* [4:0]=CFL, [6]=write, [7]=atapi, [8]=prefetch, [10]=clr_busy */
    u16 prdtl;     /* PRDT entry count */
    u32 prdbc;     /* PRDT byte count (filled by HBA on completion) */
    u32 ctba;      /* Command Table Base Address lo */
    u32 ctbau;     /* Command Table Base Address hi */
    u32 rsvd[4];
} AhciCmdHdr;

/* PRDT entry (16 bytes) */
typedef struct __attribute__((packed)){
    u32 dba;        /* Data Base Address lo */
    u32 dbau;       /* Data Base Address hi */
    u32 rsvd;
    u32 dbc_i;      /* [21:0]=byte count-1, [31]=interrupt on completion */
} AhciPrdt;

/* Command Table (variable; we use 1 PRDT entry = 128 bytes total) */
typedef struct __attribute__((packed,aligned(128))){
    u8      cfis[64];     /* Command FIS */
    u8      acmd[16];     /* ATAPI command */
    u8      rsvd[48];
    AhciPrdt prdt[1];
} AhciCmdTbl;

/* H2D Register FIS */
typedef struct __attribute__((packed)){
    u8  fis_type;   /* 0x27 */
    u8  pmport_c;   /* [7]=1 means command register update */
    u8  command;
    u8  featurel;
    u8  lba0,lba1,lba2;
    u8  device;
    u8  lba3,lba4,lba5;
    u8  featureh;
    u16 count;
    u8  icc;
    u8  control;
    u32 rsvd;
} FisH2D;

/* ── Per-port DMA memory (statically allocated below 4GB) ─────── */
#define MAX_PORTS 8

/* We allocate each port's structures from the kernel heap.
 * The heap lives in identity-mapped low memory, so virt==phys. */
typedef struct {
    AhciCmdHdr *cmd_list;  /* 32 entries × 32 bytes = 1KB aligned to 1KB */
    u8         *fis_buf;   /* 256 bytes, aligned to 256 bytes */
    AhciCmdTbl *cmd_tbl;   /* 128 bytes, aligned to 128 bytes */
    u8         *data_buf;  /* 512-byte sector DMA buffer */
    u32         port_off;  /* port register offset from HBA base */
    int         present;
} Port;

static u8   *g_hba    = 0;   /* virtual (= physical in identity map) HBA base */
static Port  g_ports[MAX_PORTS];
static int   g_nports = 0;

/* ── HBA MMIO helpers ────────────────────────────────────────── */
static u32 hba_r(u32 off){ return *(volatile u32*)(g_hba+off); }
static void hba_w(u32 off,u32 v){ *(volatile u32*)(g_hba+off)=v; }
static u32 port_r(int p,u32 off){ return *(volatile u32*)(g_hba+g_ports[p].port_off+off); }
static void port_w(int p,u32 off,u32 v){ *(volatile u32*)(g_hba+g_ports[p].port_off+off)=v; }

/* ── Aligned heap allocation (virt == phys in identity map) ──── */
static void *alloc_aligned(usize sz, usize align){
    usize raw=(usize)heap_malloc(sz+align-1);
    if(!raw) return 0;
    return (void*)((raw+align-1)&~(align-1));
}

/* ── Port stop/start ─────────────────────────────────────────── */
static void port_stop(int p){
    u32 cmd=port_r(p,P_CMD);
    cmd&=~(PCMD_ST|PCMD_FRE);
    port_w(p,P_CMD,cmd);
    for(int t=5000;t--;){
        if(!(port_r(p,P_CMD)&(PCMD_CR|PCMD_FR))) break;
        io_wait();
    }
}
static void port_start(int p){
    /* Wait until not busy */
    for(int t=10000;t--&&(port_r(p,P_TFD)&0x88);) io_wait();
    u32 cmd=port_r(p,P_CMD);
    cmd|=PCMD_FRE; port_w(p,P_CMD,cmd);
    cmd|=PCMD_ST;  port_w(p,P_CMD,cmd);
}

/* ── Issue one command and poll CI ──────────────────────────── */
static int port_issue(int p){
    /* Clear error bits */
    port_w(p,P_SERR,0xFFFFFFFF);
    port_w(p,P_IS,  0xFFFFFFFF);
    /* Issue slot 0 */
    port_w(p,P_CI, 1);
    /* Poll until CI clears or error */
    for(int t=200000;t--;){
        u32 tfd=port_r(p,P_TFD);
        if(tfd&1) return -1; /* error */
        if(!(port_r(p,P_CI)&1)) return 0;
        io_wait();
    }
    return -1; /* timeout */
}

/* ── Build H2D command FIS ───────────────────────────────────── */
static void build_fis(Port *pt, u8 cmd, u64 lba, u16 count){
    FisH2D *fis=(FisH2D*)pt->cmd_tbl->cfis;
    memset(fis,0,sizeof(FisH2D));
    fis->fis_type = FIS_TYPE_H2D;
    fis->pmport_c = 0x80; /* command register */
    fis->command  = cmd;
    fis->device   = 0x40; /* LBA mode */
    fis->lba0=(u8)lba; fis->lba1=(u8)(lba>>8); fis->lba2=(u8)(lba>>16);
    fis->lba3=(u8)(lba>>24); fis->lba4=(u8)(lba>>32); fis->lba5=(u8)(lba>>40);
    fis->count=count;
}

/* ── ahci_init ───────────────────────────────────────────────── */
void ahci_init(u32 bar){
    if(!bar) return;
    g_hba = (u8*)(usize)(bar & ~0xFu);

    /* Enable AHCI mode */
    hba_w(HBA_GHC, hba_r(HBA_GHC)|HBA_GHC_AE);

    /* Reset HBA */
    hba_w(HBA_GHC, hba_r(HBA_GHC)|HBA_GHC_HR);
    for(int t=10000;t--&&(hba_r(HBA_GHC)&HBA_GHC_HR);) io_wait();
    hba_w(HBA_GHC, hba_r(HBA_GHC)|HBA_GHC_AE);

    u32 pi=hba_r(HBA_PI); /* ports implemented bitmask */
    g_nports=0;

    for(int i=0;i<32&&g_nports<MAX_PORTS;i++){
        if(!(pi&(1u<<i))) continue;
        u32 poff=0x100u+(u32)i*0x80u;

        /* Check drive present */
        u32 ssts=*(volatile u32*)(g_hba+poff+P_SSTS);
        if((ssts&0xFu)!=SSTS_DET_PRESENT) continue;

        Port *pt=&g_ports[g_nports];
        pt->port_off=poff;
        pt->present =1;

        port_stop(g_nports);

        /* Allocate DMA memory (identity-mapped; virt==phys) */
        pt->cmd_list=(AhciCmdHdr*)alloc_aligned(sizeof(AhciCmdHdr)*32, 1024);
        pt->fis_buf =(u8*)            alloc_aligned(256, 256);
        pt->cmd_tbl =(AhciCmdTbl*)   alloc_aligned(sizeof(AhciCmdTbl), 128);
        pt->data_buf=(u8*)            alloc_aligned(512, 2);
        if(!pt->cmd_list||!pt->fis_buf||!pt->cmd_tbl||!pt->data_buf){
            print_str("[AHCI] alloc fail\r\n"); continue;
        }
        memset(pt->cmd_list,0,sizeof(AhciCmdHdr)*32);
        memset(pt->fis_buf, 0,256);
        memset(pt->cmd_tbl, 0,sizeof(AhciCmdTbl));

        /* Set CLB and FB */
        u64 clb=(u64)(usize)pt->cmd_list;
        u64 fb =(u64)(usize)pt->fis_buf;
        *(volatile u32*)(g_hba+poff+P_CLB) =(u32)(clb&0xFFFFFFFFu);
        *(volatile u32*)(g_hba+poff+P_CLBU)=(u32)(clb>>32);
        *(volatile u32*)(g_hba+poff+P_FB)  =(u32)(fb &0xFFFFFFFFu);
        *(volatile u32*)(g_hba+poff+P_FBU) =(u32)(fb >>32);

        /* Wire slot 0 command header to command table */
        u64 ctba=(u64)(usize)pt->cmd_tbl;
        pt->cmd_list[0].ctba =(u32)(ctba&0xFFFFFFFFu);
        pt->cmd_list[0].ctbau=(u32)(ctba>>32);

        port_start(g_nports);
        g_nports++;
    }

    print_str("[AHCI] "); print_hex_byte((u8)g_nports); print_str(" drive(s)\r\n");
}

/* ── Read sectors (48-bit LBA DMA) ──────────────────────────── */
i64 ahci_read_sector(int port,u32 lba,void *buf){
    if(port<0||port>=g_nports||!g_ports[port].present) return ENODEV;
    Port *pt=&g_ports[port];

    /* Command header: slot 0, read (write=0), CFL=5 dwords */
    pt->cmd_list[0].cfl_flags = 5; /* CFL=5, no ATAPI, read */
    pt->cmd_list[0].prdtl     = 1;
    pt->cmd_list[0].prdbc     = 0;

    /* PRDT: one 512-byte entry */
    u64 dba=(u64)(usize)pt->data_buf;
    pt->cmd_tbl->prdt[0].dba  =(u32)(dba&0xFFFFFFFFu);
    pt->cmd_tbl->prdt[0].dbau =(u32)(dba>>32);
    pt->cmd_tbl->prdt[0].rsvd =0;
    pt->cmd_tbl->prdt[0].dbc_i=511; /* 512 bytes - 1 */

    build_fis(pt, ATA_READ_DMA_EXT, (u64)lba, 1);

    if(port_issue(port)) return ETIMEDOUT;
    memcpy(buf, pt->data_buf, 512);
    return 0;
}

/* ── Write sectors (48-bit LBA DMA) ─────────────────────────── */
i64 ahci_write_sector(int port,u32 lba,const void *buf){
    if(port<0||port>=g_nports||!g_ports[port].present) return ENODEV;
    Port *pt=&g_ports[port];

    memcpy(pt->data_buf, buf, 512);

    pt->cmd_list[0].cfl_flags = 5|(1<<6); /* write bit */
    pt->cmd_list[0].prdtl     = 1;
    pt->cmd_list[0].prdbc     = 0;

    u64 dba=(u64)(usize)pt->data_buf;
    pt->cmd_tbl->prdt[0].dba  =(u32)(dba&0xFFFFFFFFu);
    pt->cmd_tbl->prdt[0].dbau =(u32)(dba>>32);
    pt->cmd_tbl->prdt[0].rsvd =0;
    pt->cmd_tbl->prdt[0].dbc_i=511;

    build_fis(pt, ATA_WRITE_DMA_EXT, (u64)lba, 1);

    if(port_issue(port)) return ETIMEDOUT;
    return 0;
}

/* ── Identify drive (fills 512-byte buf) ─────────────────────── */
i64 ahci_identify(int port, void *buf){
    if(port<0||port>=g_nports||!g_ports[port].present) return ENODEV;
    Port *pt=&g_ports[port];

    pt->cmd_list[0].cfl_flags = 5;
    pt->cmd_list[0].prdtl     = 1;
    pt->cmd_list[0].prdbc     = 0;

    u64 dba=(u64)(usize)pt->data_buf;
    pt->cmd_tbl->prdt[0].dba  =(u32)(dba&0xFFFFFFFFu);
    pt->cmd_tbl->prdt[0].dbau =(u32)(dba>>32);
    pt->cmd_tbl->prdt[0].rsvd =0;
    pt->cmd_tbl->prdt[0].dbc_i=511;

    FisH2D *fis=(FisH2D*)pt->cmd_tbl->cfis;
    memset(fis,0,sizeof(FisH2D));
    fis->fis_type=FIS_TYPE_H2D; fis->pmport_c=0x80;
    fis->command =ATA_IDENTIFY;  fis->device=0;

    if(port_issue(port)) return ETIMEDOUT;
    memcpy(buf, pt->data_buf, 512);
    return 0;
}

int ahci_get_port_count(void){return g_nports;}
