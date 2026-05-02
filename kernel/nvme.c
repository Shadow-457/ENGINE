/* ================================================================
 *  Systrix OS — kernel/nvme.c
 *  NVMe (Non-Volatile Memory Express) over PCIe driver
 *
 *  NVMe spec 1.4 subset:
 *   - Controller init (CC, CSTS, AQA, ASQ, ACQ)
 *   - Admin queue: Identify controller + namespace
 *   - I/O queues: 1 submission + 1 completion queue pair
 *   - Read / Write commands via submission/completion queues
 *
 *  Caveman say: "NVMe fast mammoth — no wait for slow SATA snail!"
 *
 *  PCI class 0x01 subclass 0x08 prog-if 0x02 = NVMe
 * ================================================================ */
#include "../include/kernel.h"

/* ── NVMe BAR0 register offsets ──────────────────────────────── */
#define NVME_CAP        0x00   /* Controller Capabilities (64-bit) */
#define NVME_VS         0x08   /* Version */
#define NVME_INTMS      0x0C   /* Interrupt Mask Set */
#define NVME_INTMC      0x10   /* Interrupt Mask Clear */
#define NVME_CC         0x14   /* Controller Configuration */
#define NVME_CSTS       0x1C   /* Controller Status */
#define NVME_AQA        0x24   /* Admin Queue Attributes */
#define NVME_ASQ        0x28   /* Admin Submission Queue Base (64-bit) */
#define NVME_ACQ        0x30   /* Admin Completion Queue Base (64-bit) */

/* CC bits */
#define CC_EN           (1u<<0)
#define CC_CSS_NVM      (0u<<4)  /* NVM command set */
#define CC_MPS_4K       (0u<<7)  /* host memory page size = 4K (MPS=0 → 2^(12+0)) */
#define CC_AMS_RR       (0u<<11) /* round-robin arbitration */
#define CC_IOSQES       (6u<<16) /* I/O SQ entry size = 2^6 = 64 bytes */
#define CC_IOCQES       (4u<<20) /* I/O CQ entry size = 2^4 = 16 bytes */

/* CSTS bits */
#define CSTS_RDY        (1u<<0)
#define CSTS_CFS        (1u<<1)  /* Controller Fatal Status */

/* ── Queue sizes ─────────────────────────────────────────────── */
#define NVME_ADMIN_Q_DEPTH   8   /* admin SQ/CQ depth */
#define NVME_IO_Q_DEPTH     16   /* I/O SQ/CQ depth   */
#define NVME_MAX_NS          8   /* max namespaces tracked */

/* ── Submission queue entry (64 bytes) ───────────────────────── */
typedef struct __attribute__((packed)){
    u8  opc;        /* opcode */
    u8  fuse_psdt;
    u16 cid;        /* command ID */
    u32 nsid;       /* namespace ID */
    u64 rsvd;
    u64 mptr;       /* metadata pointer */
    u64 prp1;       /* PRP1 (physical region page) */
    u64 prp2;       /* PRP2 */
    u32 cdw10;
    u32 cdw11;
    u32 cdw12;
    u32 cdw13;
    u32 cdw14;
    u32 cdw15;
} NvmeSqe;          /* 64 bytes */

/* ── Completion queue entry (16 bytes) ───────────────────────── */
typedef struct __attribute__((packed)){
    u32 dw0;        /* command-specific result */
    u32 dw1;
    u16 sqhd;       /* SQ head pointer */
    u16 sqid;       /* SQ identifier */
    u16 cid;        /* command ID echoed */
    u16 status;     /* [0]=phase, [15:1]=status field */
} NvmeCqe;          /* 16 bytes */

/* ── NVMe opcodes ────────────────────────────────────────────── */
/* Admin */
#define NVME_ADM_DELETE_IOQ  0x00
#define NVME_ADM_CREATE_IOQ  0x01  /* create both SQ & CQ via this + cdw10 */
#define NVME_ADM_IDENTIFY    0x06
#define NVME_ADM_CREATE_IOCQ 0x05
#define NVME_ADM_CREATE_IOSQ 0x01

/* I/O */
#define NVME_IO_FLUSH   0x00
#define NVME_IO_WRITE   0x01
#define NVME_IO_READ    0x02

/* ── Doorbell register offset helper ─────────────────────────── */
/* Doorbell stride = 4 << CAP.DSTRD (bits 35:32 of CAP) */
/* SQ tail doorbell for queue n = BAR0 + 0x1000 + 2n * (4 << DSTRD) */
/* CQ head doorbell for queue n = BAR0 + 0x1000 + (2n+1) * (4 << DSTRD) */

/* ── Per-queue state ─────────────────────────────────────────── */
typedef struct {
    NvmeSqe *sq;   /* submission queue ring */
    NvmeCqe *cq;   /* completion queue ring */
    u32 sq_tail;
    u32 cq_head;
    u8  cq_phase;  /* expected phase bit */
    u32 depth;
    u32 sq_db_off; /* doorbell MMIO offset from BAR0 */
    u32 cq_db_off;
    u16 next_cid;
} NvmeQueue;

/* ── Controller state ────────────────────────────────────────── */
typedef struct {
    u8        *bar;           /* BAR0 virtual = physical */
    u32        dstrd;         /* doorbell stride in bytes */
    NvmeQueue  admin;
    NvmeQueue  ioq;
    u32        nsid;          /* primary namespace ID */
    u64        ns_sectors;    /* sector count of namespace */
    u32        lba_size;      /* bytes per logical block */
    u8         data_buf[4096] __attribute__((aligned(4096)));
    int        ready;
} NvmeCtrl;

static NvmeCtrl g_nvme;

/* ── MMIO helpers ────────────────────────────────────────────── */
static u32  nvme_r32(u32 off){ return *(volatile u32*)(g_nvme.bar+off); }
static void nvme_w32(u32 off,u32 v){ *(volatile u32*)(g_nvme.bar+off)=v; }
static u64  nvme_r64(u32 off){
    return (u64)*(volatile u32*)(g_nvme.bar+off) |
           ((u64)*(volatile u32*)(g_nvme.bar+off+4)<<32);
}
static void nvme_w64(u32 off,u64 v){
    *(volatile u32*)(g_nvme.bar+off)  =(u32)(v&0xFFFFFFFFu);
    *(volatile u32*)(g_nvme.bar+off+4)=(u32)(v>>32);
}

/* ── Ring SQ tail doorbell ───────────────────────────────────── */
static void sq_ring(NvmeQueue *q){
    q->sq_tail=(q->sq_tail+1)%q->depth;
    *(volatile u32*)(g_nvme.bar+q->sq_db_off)=q->sq_tail;
}

/* ── Poll CQ for completion, return status ───────────────────── */
static int cq_poll(NvmeQueue *q, u16 cid){
    for(int t=200000;t--;){
        NvmeCqe *e=&q->cq[q->cq_head];
        /* Phase bit must match expected phase */
        if((e->status&1)==q->cq_phase && e->cid==cid){
            u16 sc=(e->status>>1)&0x7FF;
            /* Advance CQ head */
            q->cq_head=(q->cq_head+1)%q->depth;
            if(q->cq_head==0) q->cq_phase^=1; /* phase flip on wrap */
            /* Ring CQ head doorbell */
            *(volatile u32*)(g_nvme.bar+q->cq_db_off)=q->cq_head;
            return (sc==0)?0:-1;
        }
        io_wait();
    }
    return -1; /* timeout */
}

/* ── Submit admin command ────────────────────────────────────── */
static int admin_submit(NvmeSqe *cmd){
    u16 cid=g_nvme.admin.next_cid++;
    cmd->cid=cid;
    g_nvme.admin.sq[g_nvme.admin.sq_tail]=*cmd;
    sq_ring(&g_nvme.admin);
    return cq_poll(&g_nvme.admin,cid);
}

/* ── Aligned heap alloc ──────────────────────────────────────── */
static void *alloc_aligned(usize sz, usize align){
    usize raw=(usize)heap_malloc(sz+align-1);
    if(!raw) return 0;
    return (void*)((raw+align-1)&~(align-1));
}

/* ── nvme_init ───────────────────────────────────────────────── */
void nvme_init(u64 bar){
    if(!bar){ kprintf("[NVMe] no BAR0\r\n"); return; }
    g_nvme.bar=(u8*)(usize)bar;
    g_nvme.ready=0;

    /* ── 1. Read capabilities ── */
    u64 cap=nvme_r64(NVME_CAP);
    g_nvme.dstrd = (u32)(((cap>>32)&0xFu))*4 + 4; /* 4 << DSTRD */
    u32 to_ms    = (u32)(((cap>>24)&0xFFu)*500);   /* TO in 500ms units */
    (void)to_ms;

    /* ── 2. Disable controller ── */
    nvme_w32(NVME_CC, 0);
    for(int t=100000;t--&&(nvme_r32(NVME_CSTS)&CSTS_RDY);) io_wait();

    /* ── 3. Allocate admin queues ── */
    NvmeQueue *aq=&g_nvme.admin;
    aq->depth = NVME_ADMIN_Q_DEPTH;
    aq->sq=(NvmeSqe*)alloc_aligned(sizeof(NvmeSqe)*NVME_ADMIN_Q_DEPTH, 4096);
    aq->cq=(NvmeCqe*)alloc_aligned(sizeof(NvmeCqe)*NVME_ADMIN_Q_DEPTH, 4096);
    if(!aq->sq||!aq->cq){ kprintf("[NVMe] admin alloc fail\r\n"); return; }
    memset(aq->sq,0,sizeof(NvmeSqe)*NVME_ADMIN_Q_DEPTH);
    memset(aq->cq,0,sizeof(NvmeCqe)*NVME_ADMIN_Q_DEPTH);
    aq->sq_tail=0; aq->cq_head=0; aq->cq_phase=1; aq->next_cid=0;
    aq->sq_db_off = 0x1000 + 0*g_nvme.dstrd*2;
    aq->cq_db_off = 0x1000 + 0*g_nvme.dstrd*2 + g_nvme.dstrd;

    /* AQA: admin SQ size - 1 | admin CQ size - 1 */
    nvme_w32(NVME_AQA, ((NVME_ADMIN_Q_DEPTH-1)<<16)|(NVME_ADMIN_Q_DEPTH-1));
    nvme_w64(NVME_ASQ, (u64)(usize)aq->sq);
    nvme_w64(NVME_ACQ, (u64)(usize)aq->cq);

    /* ── 4. Configure + enable controller ── */
    u32 cc = CC_EN | CC_CSS_NVM | CC_MPS_4K | CC_AMS_RR |
             CC_IOSQES | CC_IOCQES;
    nvme_w32(NVME_CC, cc);

    /* Wait for CSTS.RDY */
    for(int t=200000;t--;){
        u32 csts=nvme_r32(NVME_CSTS);
        if(csts&CSTS_CFS){ kprintf("[NVMe] controller fatal\r\n"); return; }
        if(csts&CSTS_RDY) break;
        io_wait();
    }
    if(!(nvme_r32(NVME_CSTS)&CSTS_RDY)){ kprintf("[NVMe] RDY timeout\r\n"); return; }

    /* ── 5. Identify controller ── */
    {
        NvmeSqe cmd; memset(&cmd,0,sizeof(cmd));
        cmd.opc  = NVME_ADM_IDENTIFY;
        cmd.nsid = 0;
        cmd.prp1 = (u64)(usize)g_nvme.data_buf;
        cmd.prp2 = 0;
        cmd.cdw10= 1; /* CNS=1 → identify controller */
        if(admin_submit(&cmd)){ kprintf("[NVMe] identify ctrl fail\r\n"); return; }
        /* data_buf[24..63] = model number (ASCII, 40 bytes) */
        char model[41]; memcpy(model, g_nvme.data_buf+24, 40); model[40]='\0';
        /* Trim trailing spaces */
        for(int i=39;i>=0&&model[i]==' ';i--) model[i]='\0';
        kprintf("[NVMe] ctrl: "); kprintf("%s", model); kprintf("\r\n");
    }

    /* ── 6. Identify namespace 1 ── */
    {
        NvmeSqe cmd; memset(&cmd,0,sizeof(cmd));
        cmd.opc  = NVME_ADM_IDENTIFY;
        cmd.nsid = 1;
        cmd.prp1 = (u64)(usize)g_nvme.data_buf;
        cmd.prp2 = 0;
        cmd.cdw10= 0; /* CNS=0 → identify namespace */
        if(admin_submit(&cmd)){ kprintf("[NVMe] identify ns fail\r\n"); return; }
        /* NSZE: bytes 0-7 */
        g_nvme.ns_sectors=*(u64*)(g_nvme.data_buf+0);
        /* LBAF: bytes 128+ — current format in FLBAS[3:0] */
        u8 flbas=g_nvme.data_buf[26]&0xF;
        u32 lbaf=*(u32*)(g_nvme.data_buf+128+flbas*4);
        u8  lbads=(u8)((lbaf>>16)&0xFF); /* log2 of block size */
        g_nvme.lba_size=(lbads?1u<<lbads:512u);
        g_nvme.nsid=1;
        kprintf("[NVMe] ns1: ");
        print_hex_byte((u8)(g_nvme.ns_sectors>>56)); print_hex_byte((u8)(g_nvme.ns_sectors>>48));
        print_hex_byte((u8)(g_nvme.ns_sectors>>40)); print_hex_byte((u8)(g_nvme.ns_sectors>>32));
        print_hex_byte((u8)(g_nvme.ns_sectors>>24)); print_hex_byte((u8)(g_nvme.ns_sectors>>16));
        print_hex_byte((u8)(g_nvme.ns_sectors>>8));  print_hex_byte((u8)g_nvme.ns_sectors);
        kprintf(" sectors, lba=");
        print_hex_byte((u8)(g_nvme.lba_size>>24)); print_hex_byte((u8)(g_nvme.lba_size>>16));
        print_hex_byte((u8)(g_nvme.lba_size>>8));  print_hex_byte((u8)g_nvme.lba_size);
        kprintf("\r\n");
    }

    /* ── 7. Create I/O completion queue (admin cmd 0x05) ── */
    NvmeQueue *ioq=&g_nvme.ioq;
    ioq->depth=NVME_IO_Q_DEPTH;
    ioq->sq=(NvmeSqe*)alloc_aligned(sizeof(NvmeSqe)*NVME_IO_Q_DEPTH,4096);
    ioq->cq=(NvmeCqe*)alloc_aligned(sizeof(NvmeCqe)*NVME_IO_Q_DEPTH,4096);
    if(!ioq->sq||!ioq->cq){ kprintf("[NVMe] ioq alloc fail\r\n"); return; }
    memset(ioq->sq,0,sizeof(NvmeSqe)*NVME_IO_Q_DEPTH);
    memset(ioq->cq,0,sizeof(NvmeCqe)*NVME_IO_Q_DEPTH);
    ioq->sq_tail=0; ioq->cq_head=0; ioq->cq_phase=1; ioq->next_cid=0;
    /* Queue ID 1: SQ doorbell = 0x1000 + 2*1*dstrd, CQ = +dstrd */
    ioq->sq_db_off = 0x1000 + 2*g_nvme.dstrd;
    ioq->cq_db_off = 0x1000 + 2*g_nvme.dstrd + g_nvme.dstrd;

    {   /* Create I/O CQ */
        NvmeSqe cmd; memset(&cmd,0,sizeof(cmd));
        cmd.opc  = NVME_ADM_CREATE_IOCQ;
        cmd.prp1 = (u64)(usize)ioq->cq;
        cmd.cdw10= ((u32)(NVME_IO_Q_DEPTH-1)<<16)|1; /* QSIZE | QID=1 */
        cmd.cdw11= 1; /* PC=1 physically contiguous */
        if(admin_submit(&cmd)){ kprintf("[NVMe] create IOCQ fail\r\n"); return; }
    }
    {   /* Create I/O SQ */
        NvmeSqe cmd; memset(&cmd,0,sizeof(cmd));
        cmd.opc  = NVME_ADM_CREATE_IOSQ;
        cmd.prp1 = (u64)(usize)ioq->sq;
        cmd.cdw10= ((u32)(NVME_IO_Q_DEPTH-1)<<16)|1; /* QSIZE | QID=1 */
        cmd.cdw11= (1u<<16)|1; /* CQID=1 | PC=1 */
        if(admin_submit(&cmd)){ kprintf("[NVMe] create IOSQ fail\r\n"); return; }
    }

    g_nvme.ready=1;
    kprintf("[NVMe] ready\r\n");
}

/* ── Submit I/O command (read or write) ──────────────────────── */
static int nvme_io(u8 opc, u64 slba, u32 nlb, void *buf){
    if(!g_nvme.ready) return ENODEV;
    NvmeQueue *q=&g_nvme.ioq;
    u16 cid=q->next_cid++;
    NvmeSqe *e=&q->sq[q->sq_tail];
    memset(e,0,sizeof(NvmeSqe));
    e->opc  = opc;
    e->cid  = cid;
    e->nsid = g_nvme.nsid;
    e->prp1 = (u64)(usize)buf;
    /* PRP2 only needed if transfer > page size; we cap at 4KB */
    e->prp2 = 0;
    e->cdw10= (u32)(slba&0xFFFFFFFFu);
    e->cdw11= (u32)(slba>>32);
    e->cdw12= nlb-1; /* 0-based count */
    sq_ring(q);
    return cq_poll(q,cid);
}

/* ── Public API ──────────────────────────────────────────────── */

/* Read one 512-byte sector (adapts to native LBA size internally) */
i64 nvme_read_sector(u64 lba, void *buf){
    if(!g_nvme.ready) return ENODEV;
    /* If native LBA size == 512, direct 1:1 mapping */
    if(g_nvme.lba_size==512){
        u8 tmp[512] __attribute__((aligned(4096)));
        if(nvme_io(NVME_IO_READ,lba,1,tmp)) return ETIMEDOUT;
        memcpy(buf,tmp,512);
        return 0;
    }
    /* Otherwise read one native block, return first 512 bytes */
    u8 *tmp=(u8*)alloc_aligned(g_nvme.lba_size,4096);
    if(!tmp) return ENOMEM;
    u64 native_lba=lba/(g_nvme.lba_size/512);
    if(nvme_io(NVME_IO_READ,native_lba,1,tmp)){ heap_free(tmp); return ETIMEDOUT; }
    memcpy(buf,tmp,512);
    heap_free(tmp);
    return 0;
}

/* Write one 512-byte sector */
i64 nvme_write_sector(u64 lba, const void *buf){
    if(!g_nvme.ready) return ENODEV;
    if(g_nvme.lba_size==512){
        u8 tmp[512] __attribute__((aligned(4096)));
        memcpy(tmp,buf,512);
        return nvme_io(NVME_IO_WRITE,lba,1,tmp)?ETIMEDOUT:0;
    }
    /* RMW for non-512 native blocks */
    u8 *tmp=(u8*)alloc_aligned(g_nvme.lba_size,4096);
    if(!tmp) return ENOMEM;
    u64 native_lba=lba/(g_nvme.lba_size/512);
    u32 off=(u32)((lba%(g_nvme.lba_size/512))*512);
    if(nvme_io(NVME_IO_READ,native_lba,1,tmp)){ heap_free(tmp); return ETIMEDOUT; }
    memcpy(tmp+off,buf,512);
    if(nvme_io(NVME_IO_WRITE,native_lba,1,tmp)){ heap_free(tmp); return ETIMEDOUT; }
    heap_free(tmp);
    return 0;
}

/* Read up to 4KB (8 sectors) at once — aligned buffer required */
i64 nvme_read_4k(u64 lba, void *buf){
    if(!g_nvme.ready) return ENODEV;
    return nvme_io(NVME_IO_READ, lba, 8, buf)?ETIMEDOUT:0;
}

/* Write up to 4KB (8 sectors) at once */
i64 nvme_write_4k(u64 lba, const void *buf){
    if(!g_nvme.ready) return ENODEV;
    return nvme_io(NVME_IO_WRITE, lba, 8, (void*)(usize)buf)?ETIMEDOUT:0;
}

/* Flush NVMe write cache */
i64 nvme_flush(void){
    if(!g_nvme.ready) return ENODEV;
    return nvme_io(NVME_IO_FLUSH,0,0,0)?ETIMEDOUT:0;
}

int  nvme_ready(void){ return g_nvme.ready; }
u64  nvme_sector_count(void){ return g_nvme.ns_sectors; }
u32  nvme_lba_size(void){ return g_nvme.lba_size; }
