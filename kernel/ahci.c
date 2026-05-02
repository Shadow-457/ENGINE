/* ================================================================
 *  Systrix OS — kernel/ahci.c   (enhanced v2)
 *  AHCI SATA driver: HBA init, port start/stop, ATA identify,
 *  48-bit LBA DMA read/write (multi-sector), error recovery,
 *  cache flush.
 *
 *  Caveman say: "Me talk to disk with DMA — fast like mammoth!"
 * ================================================================ */
#include "../include/kernel.h"

/* ── HBA register offsets ─────────────────────────────────────── */
#define HBA_CAP     0x00
#define HBA_GHC     0x04
#define HBA_IS      0x08
#define HBA_PI      0x0C
#define HBA_VS      0x10
#define HBA_GHC_AE  (1u<<31)
#define HBA_GHC_HR  (1u<<0)

/* Port register offsets */
#define P_CLB   0x00
#define P_CLBU  0x04
#define P_FB    0x08
#define P_FBU   0x0C
#define P_IS    0x10
#define P_IE    0x14
#define P_CMD   0x18
#define P_TFD   0x20
#define P_SIG   0x24
#define P_SSTS  0x28
#define P_SCTL  0x2C
#define P_SERR  0x30
#define P_SACT  0x34
#define P_CI    0x38

#define PCMD_ST   (1u<<0)
#define PCMD_SUD  (1u<<1)
#define PCMD_POD  (1u<<2)
#define PCMD_FRE  (1u<<4)
#define PCMD_FR   (1u<<14)
#define PCMD_CR   (1u<<15)

#define TFD_ERR   (1u<<0)
#define TFD_BSY   (1u<<7)
#define TFD_DRQ   (1u<<3)

#define SSTS_DET_PRESENT 0x3u

/* Port IS fatal error bits */
#define P_IS_TFES  (1u<<30)
#define P_IS_HBFS  (1u<<29)
#define P_IS_HBDS  (1u<<28)
#define P_IS_IFS   (1u<<27)

/* ── FIS types ────────────────────────────────────────────────── */
#define FIS_TYPE_H2D 0x27

/* ── ATA commands ─────────────────────────────────────────────── */
#define ATA_READ_DMA_EXT   0x25
#define ATA_WRITE_DMA_EXT  0x35
#define ATA_IDENTIFY       0xEC
#define ATA_FLUSH_EXT      0xEA

/* ── Max PRDT entries and sectors per command ─────────────────── */
#define MAX_PRDT            8
#define MAX_SECTORS_PER_CMD 8

/* Command Header (32 bytes) */
typedef struct __attribute__((packed,aligned(32))){
    u16 cfl_flags;
    u16 prdtl;
    u32 prdbc;
    u32 ctba;
    u32 ctbau;
    u32 rsvd[4];
} AhciCmdHdr;

/* PRDT entry (16 bytes) */
typedef struct __attribute__((packed)){
    u32 dba;
    u32 dbau;
    u32 rsvd;
    u32 dbc_i;
} AhciPrdt;

/* Command Table */
typedef struct __attribute__((packed,aligned(128))){
    u8       cfis[64];
    u8       acmd[16];
    u8       rsvd[48];
    AhciPrdt prdt[MAX_PRDT];
} AhciCmdTbl;

/* H2D Register FIS */
typedef struct __attribute__((packed)){
    u8  fis_type;
    u8  pmport_c;
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

/* ── Per-port state ───────────────────────────────────────────── */
#define MAX_PORTS 8

typedef struct {
    AhciCmdHdr *cmd_list;
    u8         *fis_buf;
    AhciCmdTbl *cmd_tbl;
    u8         *data_buf;
    u32         port_off;
    int         present;
    u64         sector_count;
    char        model[41];
} Port;

static u8   *g_hba    = 0;
static Port  g_ports[MAX_PORTS];
static int   g_nports = 0;

/* ── MMIO helpers ─────────────────────────────────────────────── */
static u32  hba_r(u32 off){ return *(volatile u32*)(g_hba+off); }
static void hba_w(u32 off,u32 v){ *(volatile u32*)(g_hba+off)=v; }
static u32  port_r(int p,u32 off){ return *(volatile u32*)(g_hba+g_ports[p].port_off+off); }
static void port_w(int p,u32 off,u32 v){ *(volatile u32*)(g_hba+g_ports[p].port_off+off)=v; }

/* ── Aligned heap alloc ───────────────────────────────────────── */
static void *alloc_aligned(usize sz, usize align){
    usize raw=(usize)heap_malloc(sz+align-1);
    if(!raw) return 0;
    return (void*)((raw+align-1)&~(align-1));
}

/* ── Port stop ────────────────────────────────────────────────── */
static void port_stop(int p){
    u32 cmd=port_r(p,P_CMD);
    cmd&=~(PCMD_ST|PCMD_FRE);
    port_w(p,P_CMD,cmd);
    for(int t=50000;t--;){
        if(!(port_r(p,P_CMD)&(PCMD_CR|PCMD_FR))) break;
        io_wait();
    }
}

/* ── Port start ───────────────────────────────────────────────── */
static void port_start(int p){
    for(int t=100000;t--&&(port_r(p,P_TFD)&(TFD_BSY|TFD_DRQ));) io_wait();
    u32 cmd=port_r(p,P_CMD);
    cmd|=PCMD_FRE; port_w(p,P_CMD,cmd);
    cmd|=PCMD_ST;  port_w(p,P_CMD,cmd);
}

/* ── COMRESET error recovery ──────────────────────────────────── */
static void port_reset(int p){
    port_stop(p);
    u32 sctl=port_r(p,P_SCTL);
    port_w(p,P_SCTL,(sctl&~0xFu)|1u);
    for(volatile int t=10000;t--;) io_wait();
    port_w(p,P_SCTL,sctl&~0xFu);
    for(int t=100000;t--;){
        if((port_r(p,P_SSTS)&0xFu)==SSTS_DET_PRESENT) break;
        io_wait();
    }
    port_w(p,P_SERR,0xFFFFFFFF);
    port_w(p,P_IS,  0xFFFFFFFF);
    port_start(p);
}

/* ── Issue slot 0, poll ───────────────────────────────────────── */
static int port_issue(int p){
    port_w(p,P_SERR,0xFFFFFFFF);
    port_w(p,P_IS,  0xFFFFFFFF);
    port_w(p,P_CI,  1u);
    for(int t=400000;t--;){
        u32 is=port_r(p,P_IS);
        if(is&(P_IS_TFES|P_IS_HBFS|P_IS_HBDS|P_IS_IFS)){
            port_reset(p); return -1;
        }
        if(port_r(p,P_TFD)&TFD_ERR){ port_reset(p); return -1; }
        if(!(port_r(p,P_CI)&1u)) return 0;
        io_wait();
    }
    port_reset(p);
    return -1;
}

/* ── Build H2D FIS ────────────────────────────────────────────── */
static void build_fis(Port *pt, u8 cmd, u64 lba, u16 count){
    FisH2D *fis=(FisH2D*)pt->cmd_tbl->cfis;
    memset(fis,0,sizeof(FisH2D));
    fis->fis_type=FIS_TYPE_H2D; fis->pmport_c=0x80;
    fis->command=cmd; fis->device=0x40;
    fis->lba0=(u8)lba;       fis->lba1=(u8)(lba>>8);  fis->lba2=(u8)(lba>>16);
    fis->lba3=(u8)(lba>>24); fis->lba4=(u8)(lba>>32); fis->lba5=(u8)(lba>>40);
    fis->count=count;
}

/* ── Parse IDENTIFY response ──────────────────────────────────── */
static void parse_identify(Port *pt, u16 *data){
    for(int i=0;i<20;i++){
        u16 w=data[27+i];
        pt->model[i*2]  =(char)(w>>8);
        pt->model[i*2+1]=(char)(w&0xFF);
    }
    pt->model[40]='\0';
    for(int i=39;i>=0&&pt->model[i]==' ';i--) pt->model[i]='\0';
    pt->sector_count=(u64)data[100]|((u64)data[101]<<16)|
                     ((u64)data[102]<<32)|((u64)data[103]<<48);
    if(!pt->sector_count)
        pt->sector_count=(u32)data[60]|((u32)data[61]<<16);
}

/* ── ahci_init ────────────────────────────────────────────────── */
void ahci_init(u32 bar){
    if(!bar) return;
    g_hba=(u8*)(usize)(bar&~0xFu);

    hba_w(HBA_GHC, hba_r(HBA_GHC)|HBA_GHC_AE);
    hba_w(HBA_GHC, hba_r(HBA_GHC)|HBA_GHC_HR);
    for(int t=50000;t--&&(hba_r(HBA_GHC)&HBA_GHC_HR);) io_wait();
    hba_w(HBA_GHC, hba_r(HBA_GHC)|HBA_GHC_AE);

    u32 pi=hba_r(HBA_PI);
    g_nports=0;

    for(int i=0;i<32&&g_nports<MAX_PORTS;i++){
        if(!(pi&(1u<<i))) continue;
        u32 poff=0x100u+(u32)i*0x80u;
        u32 ssts=*(volatile u32*)(g_hba+poff+P_SSTS);
        if((ssts&0xFu)!=SSTS_DET_PRESENT) continue;

        /* Spin-up */
        u32 pcmd=*(volatile u32*)(g_hba+poff+P_CMD);
        if(!(pcmd&PCMD_SUD)){
            *(volatile u32*)(g_hba+poff+P_CMD)=pcmd|PCMD_SUD|PCMD_POD;
            for(volatile int t=10000;t--;) io_wait();
        }

        Port *pt=&g_ports[g_nports];
        pt->port_off=poff; pt->present=1; pt->sector_count=0;

        port_stop(g_nports);

        pt->cmd_list=(AhciCmdHdr*)alloc_aligned(sizeof(AhciCmdHdr)*32,1024);
        pt->fis_buf =(u8*)        alloc_aligned(256,256);
        pt->cmd_tbl =(AhciCmdTbl*)alloc_aligned(sizeof(AhciCmdTbl),128);
        pt->data_buf=(u8*)        alloc_aligned(512*MAX_SECTORS_PER_CMD,512);
        if(!pt->cmd_list||!pt->fis_buf||!pt->cmd_tbl||!pt->data_buf){
            kprintf("[AHCI] alloc fail\r\n"); continue;
        }
        memset(pt->cmd_list,0,sizeof(AhciCmdHdr)*32);
        memset(pt->fis_buf, 0,256);
        memset(pt->cmd_tbl, 0,sizeof(AhciCmdTbl));

        u64 clb=(u64)(usize)pt->cmd_list;
        u64 fb =(u64)(usize)pt->fis_buf;
        *(volatile u32*)(g_hba+poff+P_CLB) =(u32)(clb&0xFFFFFFFFu);
        *(volatile u32*)(g_hba+poff+P_CLBU)=(u32)(clb>>32);
        *(volatile u32*)(g_hba+poff+P_FB)  =(u32)(fb &0xFFFFFFFFu);
        *(volatile u32*)(g_hba+poff+P_FBU) =(u32)(fb >>32);

        u64 ctba=(u64)(usize)pt->cmd_tbl;
        pt->cmd_list[0].ctba =(u32)(ctba&0xFFFFFFFFu);
        pt->cmd_list[0].ctbau=(u32)(ctba>>32);

        port_start(g_nports);

        /* IDENTIFY to get model + size */
        { u8 idbuf[512];
          if(ahci_identify(g_nports,idbuf)==0) parse_identify(pt,(u16*)idbuf); }

        kprintf("[AHCI] Port "); print_hex_byte((u8)g_nports);
        kprintf(": "); kprintf("%s", pt->model); kprintf("\r\n");

        g_nports++;
    }
    kprintf("[AHCI] "); print_hex_byte((u8)g_nports); kprintf(" drive(s)\r\n");
}

/* ── Multi-sector read ────────────────────────────────────────── */
i64 ahci_read_sectors(int port, u64 lba, u16 count, void *buf){
    if(port<0||port>=g_nports||!g_ports[port].present) return ENODEV;
    if(!count||count>MAX_SECTORS_PER_CMD) return EINVAL;
    Port *pt=&g_ports[port];
    pt->cmd_list[0].cfl_flags=5;
    pt->cmd_list[0].prdtl=count;
    pt->cmd_list[0].prdbc=0;
    for(u16 s=0;s<count;s++){
        u64 dba=(u64)(usize)(pt->data_buf+(usize)s*512);
        pt->cmd_tbl->prdt[s].dba  =(u32)(dba&0xFFFFFFFFu);
        pt->cmd_tbl->prdt[s].dbau =(u32)(dba>>32);
        pt->cmd_tbl->prdt[s].rsvd =0;
        pt->cmd_tbl->prdt[s].dbc_i=511;
    }
    build_fis(pt,ATA_READ_DMA_EXT,lba,count);
    if(port_issue(port)) return ETIMEDOUT;
    memcpy(buf,pt->data_buf,(usize)count*512);
    return 0;
}

/* ── Single sector read ───────────────────────────────────────── */
i64 ahci_read_sector(int port, u32 lba, void *buf){
    return ahci_read_sectors(port,(u64)lba,1,buf);
}

/* ── Multi-sector write ───────────────────────────────────────── */
i64 ahci_write_sectors(int port, u64 lba, u16 count, const void *buf){
    if(port<0||port>=g_nports||!g_ports[port].present) return ENODEV;
    if(!count||count>MAX_SECTORS_PER_CMD) return EINVAL;
    Port *pt=&g_ports[port];
    memcpy(pt->data_buf,buf,(usize)count*512);
    pt->cmd_list[0].cfl_flags=5|(1<<6);
    pt->cmd_list[0].prdtl=count;
    pt->cmd_list[0].prdbc=0;
    for(u16 s=0;s<count;s++){
        u64 dba=(u64)(usize)(pt->data_buf+(usize)s*512);
        pt->cmd_tbl->prdt[s].dba  =(u32)(dba&0xFFFFFFFFu);
        pt->cmd_tbl->prdt[s].dbau =(u32)(dba>>32);
        pt->cmd_tbl->prdt[s].rsvd =0;
        pt->cmd_tbl->prdt[s].dbc_i=511;
    }
    build_fis(pt,ATA_WRITE_DMA_EXT,lba,count);
    if(port_issue(port)) return ETIMEDOUT;
    return 0;
}

/* ── Single sector write ──────────────────────────────────────── */
i64 ahci_write_sector(int port, u32 lba, const void *buf){
    return ahci_write_sectors(port,(u64)lba,1,buf);
}

/* ── Cache flush ──────────────────────────────────────────────── */
i64 ahci_flush(int port){
    if(port<0||port>=g_nports||!g_ports[port].present) return ENODEV;
    Port *pt=&g_ports[port];
    pt->cmd_list[0].cfl_flags=5;
    pt->cmd_list[0].prdtl=0;
    pt->cmd_list[0].prdbc=0;
    FisH2D *fis=(FisH2D*)pt->cmd_tbl->cfis;
    memset(fis,0,sizeof(FisH2D));
    fis->fis_type=FIS_TYPE_H2D; fis->pmport_c=0x80;
    fis->command=ATA_FLUSH_EXT; fis->device=0x40;
    if(port_issue(port)) return ETIMEDOUT;
    return 0;
}

/* ── Identify drive ───────────────────────────────────────────── */
i64 ahci_identify(int port, void *buf){
    if(port<0||port>=g_nports||!g_ports[port].present) return ENODEV;
    Port *pt=&g_ports[port];
    pt->cmd_list[0].cfl_flags=5;
    pt->cmd_list[0].prdtl=1;
    pt->cmd_list[0].prdbc=0;
    u64 dba=(u64)(usize)pt->data_buf;
    pt->cmd_tbl->prdt[0].dba  =(u32)(dba&0xFFFFFFFFu);
    pt->cmd_tbl->prdt[0].dbau =(u32)(dba>>32);
    pt->cmd_tbl->prdt[0].rsvd =0;
    pt->cmd_tbl->prdt[0].dbc_i=511;
    FisH2D *fis=(FisH2D*)pt->cmd_tbl->cfis;
    memset(fis,0,sizeof(FisH2D));
    fis->fis_type=FIS_TYPE_H2D; fis->pmport_c=0x80;
    fis->command=ATA_IDENTIFY; fis->device=0;
    if(port_issue(port)) return ETIMEDOUT;
    memcpy(buf,pt->data_buf,512);
    return 0;
}

int        ahci_get_port_count(void){ return g_nports; }
u64        ahci_get_sector_count(int p){ if(p<0||p>=g_nports)return 0; return g_ports[p].sector_count; }
const char *ahci_get_model(int p){ if(p<0||p>=g_nports)return ""; return g_ports[p].model; }
