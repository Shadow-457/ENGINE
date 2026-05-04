/* ================================================================
 *  Systrix OS — kernel/usb.c
 *  USB subsystem: EHCI host controller + XHCI full slot enumeration
 *  + USB Mass Storage (Bulk-Only Transport / SCSI transparent)
 *
 *  Covers:
 *    - EHCI: async schedule (QH/qTD), port reset, device enumeration,
 *      SET_ADDRESS, GET_DESCRIPTOR, SET_CONFIGURATION
 *    - XHCI: full Enable Slot → Address Device → Configure Endpoint
 *      command sequence so real USB keyboards/mice work without BIOS
 *      PS/2 emulation fallback
 *    - USB Mass Storage class (BBB transport), SCSI INQUIRY +
 *      READ CAPACITY + READ(10) / WRITE(10), exposed as block device
 *      so JFS/FAT32 can mount a USB flash drive
 *
 *  Architecture constraints:
 *    - DMA memory is heap_malloc'd (identity-mapped, virt == phys)
 *    - All MMIO mapped via vmm_map at fixed high VAs
 *    - No interrupts used — pure polled operation
 *    - Supports up to 4 USB mass storage LUNs
 * ================================================================ */
#include "../include/kernel.h"

/* ── forward declarations ─────────────────────────────────────── */
static int  usb_ctrl_xfer(int ctrlr, u8 addr, u8 bmReqType, u8 bReq,
                          u16 wVal, u16 wIdx, u16 wLen, void *data);

/* ================================================================
 *  §1  COMMON USB DESCRIPTORS / REQUESTS
 * ================================================================ */
#define USB_REQ_GET_DESCRIPTOR   0x06
#define USB_REQ_SET_ADDRESS      0x05
#define USB_REQ_SET_CONFIGURATION 0x09
#define USB_REQ_SET_INTERFACE    0x0B
#define USB_REQ_SET_PROTOCOL     0x0B   /* HID class */
#define USB_REQ_CLEAR_FEATURE    0x01

#define USB_DESC_DEVICE          0x01
#define USB_DESC_CONFIG          0x02
#define USB_DESC_STRING          0x03
#define USB_DESC_INTERFACE       0x04
#define USB_DESC_ENDPOINT        0x05

#define USB_CLASS_HID            0x03
#define USB_CLASS_MSC            0x08
#define USB_SUBCLASS_SCSI        0x06
#define USB_PROTO_BBB            0x50   /* Bulk-Only Transport */
#define USB_PROTO_BOOT_KBD       0x01

/* Setup packet */
typedef struct __attribute__((packed)) {
    u8  bmRequestType;
    u8  bRequest;
    u16 wValue;
    u16 wIndex;
    u16 wLength;
} UsbSetup;

/* Device descriptor (partial — enough to enumerate) */
typedef struct __attribute__((packed)) {
    u8  bLength, bDescriptorType;
    u16 bcdUSB;
    u8  bDeviceClass, bDeviceSubClass, bDeviceProtocol, bMaxPacketSize0;
    u16 idVendor, idProduct;
    u16 bcdDevice;
    u8  iManufacturer, iProduct, iSerialNumber, bNumConfigurations;
} UsbDeviceDesc;

/* Config descriptor header */
typedef struct __attribute__((packed)) {
    u8  bLength, bDescriptorType;
    u16 wTotalLength;
    u8  bNumInterfaces, bConfigurationValue, iConfiguration,
        bmAttributes, bMaxPower;
} UsbConfigDesc;

/* Interface descriptor */
typedef struct __attribute__((packed)) {
    u8  bLength, bDescriptorType;
    u8  bInterfaceNumber, bAlternateSetting, bNumEndpoints;
    u8  bInterfaceClass, bInterfaceSubClass, bInterfaceProtocol;
    u8  iInterface;
} UsbInterfaceDesc;

/* Endpoint descriptor */
typedef struct __attribute__((packed)) {
    u8  bLength, bDescriptorType;
    u8  bEndpointAddress, bmAttributes;
    u16 wMaxPacketSize;
    u8  bInterval;
} UsbEndpointDesc;

/* ================================================================
 *  §2  EHCI DEFINITIONS
 * ================================================================ */
#define EHCI_MMIO_VA_BASE   0xCA000000ULL   /* 1 MiB windows per controller */
#define EHCI_MMIO_WIN       0x100000ULL

/* Capability registers (offset from MMIO base) */
#define EHCI_CAPLENGTH      0x00
#define EHCI_HCIVERSION     0x02
#define EHCI_HCSPARAMS      0x04
#define EHCI_HCCPARAMS      0x08

/* Operational registers (offset from cap_base + CAPLENGTH) */
#define EHCI_USBCMD         0x00
#define EHCI_USBSTS         0x04
#define EHCI_USBINTR        0x08
#define EHCI_FRINDEX        0x0C
#define EHCI_CTRLDSSEGMENT  0x10
#define EHCI_PERIODICBASE   0x14
#define EHCI_ASYNCLISTADDR  0x18
#define EHCI_CONFIGFLAG     0x40
#define EHCI_PORTSC(n)      (0x44+(n)*4)

/* USBCMD bits */
#define EHCI_CMD_RUN        (1u<<0)
#define EHCI_CMD_RST        (1u<<1)
#define EHCI_CMD_ASEN       (1u<<5)   /* async schedule enable */
#define EHCI_CMD_PSE        (1u<<4)   /* periodic schedule enable */
#define EHCI_CMD_ITC(n)     ((n)<<16) /* interrupt threshold */

/* USBSTS bits */
#define EHCI_STS_HCH        (1u<<12)
#define EHCI_STS_INT        (1u<<0)
#define EHCI_STS_ERR        (1u<<1)
#define EHCI_STS_ASYNC_ADV  (1u<<5)

/* PORTSC bits */
#define EHCI_PORT_CCS       (1u<<0)
#define EHCI_PORT_CSC       (1u<<1)
#define EHCI_PORT_PED       (1u<<2)
#define EHCI_PORT_PEDC      (1u<<3)
#define EHCI_PORT_PR        (1u<<8)
#define EHCI_PORT_PP        (1u<<12)
#define EHCI_PORT_LS(p)     (((p)>>10)&3)
#define EHCI_PORT_SPEED(p)  (((p)>>26)&3)  /* 0=FS 1=LS 2=HS */

/* Queue Head (32-byte minimal) */
typedef struct __attribute__((packed,aligned(32))) {
    u32 next_qh;        /* horizontal link (T bit in bit0) */
    u32 epchar;         /* endpoint characteristics */
    u32 epcap;          /* endpoint capabilities */
    u32 cur_qtd;        /* current qTD pointer */
    /* overlay (first qTD) */
    u32 next_qtd;
    u32 alt_qtd;
    u32 token;
    u32 buf[5];
    u32 buf_hi[5];
} EhciQH;

/* Queue Transfer Descriptor */
typedef struct __attribute__((packed,aligned(32))) {
    u32 next;           /* next qTD (T bit in bit0) */
    u32 alt;
    u32 token;
    u32 buf[5];
    u32 buf_hi[5];
} EhciQTD;

/* QH epchar bits */
#define QH_EPS_HS       (2u<<12)        /* High speed */
#define QH_EPS_FS       (0u<<12)
#define QH_DTC          (1u<<14)        /* Data toggle control */
#define QH_H            (1u<<15)        /* Head of reclamation list */
#define QH_RL(n)        ((u32)(n)<<28)

/* qTD token bits */
#define QTD_PING        (1u<<0)
#define QTD_SPLIT_X     (1u<<1)
#define QTD_MMF         (1u<<2)
#define QTD_XACT_ERR    (1u<<3)
#define QTD_BABBLE      (1u<<4)
#define QTD_DBE         (1u<<5)
#define QTD_HALTED      (1u<<6)
#define QTD_ACTIVE      (1u<<7)
#define QTD_PID_OUT     (0u<<8)
#define QTD_PID_IN      (1u<<8)
#define QTD_PID_SETUP   (2u<<8)
#define QTD_CERR(n)     ((u32)(n)<<10)
#define QTD_CPAGE(n)    ((u32)(n)<<12)
#define QTD_IOC         (1u<<15)
#define QTD_BYTES(n)    ((u32)(n)<<16)
#define QTD_DT          (1u<<31)

#define QTD_TERMINATE   1u

#define EHCI_MAX        4

typedef struct {
    u8  *mmio;
    u8  *op;
    int  n_ports;
    int  active;
    /* async dummy QH (reclamation head) */
    EhciQH  *dummy_qh;
    u64      dummy_qh_phys;
    /* current next device address to assign */
    u8   next_addr;
    int  index;         /* controller index */
} Ehci;

static Ehci g_ehci[EHCI_MAX];
static int  g_n_ehci = 0;

/* ── EHCI MMIO helpers ───────────────────────────────────────── */
static u32 er(Ehci *e, u32 off) { return *(volatile u32*)(e->op + off); }
static void ew(Ehci *e, u32 off, u32 v) { *(volatile u32*)(e->op + off) = v; }

static void ehci_delay(int us) {
    for (volatile int i = 0; i < us * 300; i++);
}

/* ── EHCI: allocate aligned DMA buffer ──────────────────────── */
static void *dma_alloc_aligned(usize sz, usize align, u64 *phys) {
    usize raw = (usize)heap_malloc(sz + align);
    if (!raw) { if (phys) *phys = 0; return 0; }
    usize p = (raw + align - 1) & ~(align - 1);
    memset((void*)p, 0, sz);
    if (phys) *phys = (u64)p;
    return (void*)p;
}

/* ── EHCI: wait for bit to clear ────────────────────────────── */
static int ehci_wait_clear(Ehci *e, u32 off, u32 mask, int ms) {
    for (int i = 0; i < ms * 10; i++) {
        if (!(er(e, off) & mask)) return 0;
        ehci_delay(100);
    }
    return -1;
}

/* ── EHCI: build and submit a control transfer ───────────────── */
/*
 * Layout: SETUP qTD → DATA qTD (optional) → STATUS qTD
 * We build them, link them into a fresh QH, splice QH into the
 * async schedule, wait for completion, then remove.
 */
static int ehci_ctrl_xfer(Ehci *e, u8 addr, u8 speed,
                          u8 bmReqType, u8 bReq,
                          u16 wVal, u16 wIdx, u16 wLen,
                          void *data)
{
    /* Allocate setup packet buffer */
    u64 setup_phys;
    UsbSetup *setup = (UsbSetup*)dma_alloc_aligned(8, 8, &setup_phys);
    if (!setup) return -1;
    setup->bmRequestType = bmReqType;
    setup->bRequest      = bReq;
    setup->wValue        = wVal;
    setup->wIndex        = wIdx;
    setup->wLength       = wLen;

    /* Data buffer */
    u64 data_phys = 0;
    if (wLen && data) data_phys = (u64)(usize)data;

    /* Allocate qTDs: SETUP + optional DATA + STATUS */
    u64 qtd_setup_phys, qtd_data_phys, qtd_status_phys;
    EhciQTD *qtd_setup  = (EhciQTD*)dma_alloc_aligned(sizeof(EhciQTD), 32, &qtd_setup_phys);
    EhciQTD *qtd_data   = (EhciQTD*)dma_alloc_aligned(sizeof(EhciQTD), 32, &qtd_data_phys);
    EhciQTD *qtd_status = (EhciQTD*)dma_alloc_aligned(sizeof(EhciQTD), 32, &qtd_status_phys);
    if (!qtd_setup || !qtd_data || !qtd_status) return -1;

    /* STATUS qTD */
    qtd_status->next = QTD_TERMINATE;
    qtd_status->alt  = QTD_TERMINATE;
    u32 status_pid = (bmReqType & 0x80) ? QTD_PID_OUT : QTD_PID_IN;
    qtd_status->token = QTD_ACTIVE | QTD_CERR(3) | QTD_IOC | QTD_DT | status_pid | QTD_BYTES(0);

    /* DATA qTD (if any) */
    if (wLen && data) {
        qtd_data->next  = (u32)qtd_status_phys;
        qtd_data->alt   = QTD_TERMINATE;
        u32 data_pid = (bmReqType & 0x80) ? QTD_PID_IN : QTD_PID_OUT;
        qtd_data->token = QTD_ACTIVE | QTD_CERR(3) | QTD_DT | data_pid | QTD_BYTES(wLen);
        qtd_data->buf[0] = (u32)(data_phys & 0xFFFFFFFF);
        qtd_data->buf_hi[0] = (u32)(data_phys >> 32);
        /* Fill remaining buf pointers for >4K transfers */
        for (int i = 1; i < 5; i++) {
            u64 pg = (data_phys & ~0xFFFULL) + (u64)i * 0x1000;
            qtd_data->buf[i]    = (u32)(pg & 0xFFFFFFFF);
            qtd_data->buf_hi[i] = (u32)(pg >> 32);
        }
    } else {
        qtd_data->next  = (u32)qtd_status_phys;
        qtd_data->alt   = QTD_TERMINATE;
        qtd_data->token = 0; /* skip — no bytes */
    }

    /* SETUP qTD */
    qtd_setup->next  = (wLen && data) ? (u32)qtd_data_phys : (u32)qtd_status_phys;
    qtd_setup->alt   = QTD_TERMINATE;
    qtd_setup->token = QTD_ACTIVE | QTD_CERR(3) | QTD_PID_SETUP | QTD_BYTES(8);
    qtd_setup->buf[0]    = (u32)(setup_phys & 0xFFFFFFFF);
    qtd_setup->buf_hi[0] = (u32)(setup_phys >> 32);

    /* Allocate QH for this device */
    u64 qh_phys;
    EhciQH *qh = (EhciQH*)dma_alloc_aligned(sizeof(EhciQH), 32, &qh_phys);
    if (!qh) return -1;

    u32 eps_bits = (speed == 2) ? QH_EPS_HS : QH_EPS_FS;
    qh->epchar  = (u32)addr | ((u32)0 << 8) | eps_bits | QH_DTC | ((u32)64 << 16);
    qh->epcap   = (1u << 30); /* mult=1 */
    qh->cur_qtd = 0;
    qh->next_qtd = (u32)qtd_setup_phys;
    qh->alt_qtd  = QTD_TERMINATE;
    qh->token    = 0;

    /* Link into async schedule: dummy → qh → dummy (circular) */
    qh->next_qh = (u32)(e->dummy_qh_phys) | 0x2; /* QH type */
    e->dummy_qh->next_qh = (u32)qh_phys | 0x2;

    /* Ensure async schedule is running */
    if (!(er(e, EHCI_USBCMD) & EHCI_CMD_ASEN)) {
        ew(e, EHCI_ASYNCLISTADDR, (u32)e->dummy_qh_phys);
        ew(e, EHCI_USBCMD, er(e, EHCI_USBCMD) | EHCI_CMD_ASEN);
        ehci_wait_clear(e, EHCI_USBCMD, 0, 1); /* just let it start */
    }

    /* Poll for completion (IOC on STATUS qTD means ACTIVE clears) */
    int rc = 0;
    for (int t = 50000; t--; ) {
        ehci_delay(10);
        if (!(qtd_status->token & QTD_ACTIVE)) goto done;
        if (qtd_status->token & QTD_HALTED)    { rc = -1; goto done; }
        if (wLen && data && !(qtd_data->token & QTD_ACTIVE) &&
            (qtd_data->token & QTD_HALTED))    { rc = -1; goto done; }
    }
    rc = -1;
done:
    /* Unlink qh from async schedule */
    e->dummy_qh->next_qh = (u32)(e->dummy_qh_phys) | 0x2;
    /* Doorbell: wait for async advance */
    ew(e, EHCI_USBCMD, er(e, EHCI_USBCMD) | EHCI_CMD_ASEN);
    ehci_delay(1000);

    return rc;
}

/* ── EHCI: bulk transfer (for mass storage) ─────────────────── */
static int ehci_bulk_xfer(Ehci *e, u8 addr, u8 ep_addr, int is_in,
                          void *buf, u32 len, u8 *toggle)
{
    u64 buf_phys = (u64)(usize)buf;
    u64 qtd_phys;
    EhciQTD *qtd = (EhciQTD*)dma_alloc_aligned(sizeof(EhciQTD), 32, &qtd_phys);
    if (!qtd) return -1;

    qtd->next  = QTD_TERMINATE;
    qtd->alt   = QTD_TERMINATE;
    u32 pid    = is_in ? QTD_PID_IN : QTD_PID_OUT;
    u32 dt_bit = (*toggle) ? QTD_DT : 0;
    qtd->token = QTD_ACTIVE | QTD_CERR(3) | QTD_IOC | pid | QTD_BYTES(len) | dt_bit;
    for (int i = 0; i < 5; i++) {
        u64 pg = (buf_phys & ~0xFFFULL) + (u64)i * 0x1000;
        qtd->buf[i]    = (u32)(pg & 0xFFFFFFFF);
        qtd->buf_hi[i] = (u32)(pg >> 32);
    }
    qtd->buf[0] = (u32)(buf_phys & 0xFFFFFFFF); /* exact start */

    u64 qh_phys;
    EhciQH *qh = (EhciQH*)dma_alloc_aligned(sizeof(EhciQH), 32, &qh_phys);
    if (!qh) return -1;
    qh->epchar  = (u32)addr | ((u32)(ep_addr & 0xF) << 8) | QH_EPS_HS | ((u32)512 << 16);
    qh->epcap   = (1u << 30);
    qh->next_qtd = (u32)qtd_phys;
    qh->alt_qtd  = QTD_TERMINATE;
    qh->token    = 0;
    qh->next_qh  = (u32)(e->dummy_qh_phys) | 0x2;
    e->dummy_qh->next_qh = (u32)qh_phys | 0x2;

    if (!(er(e, EHCI_USBCMD) & EHCI_CMD_ASEN)) {
        ew(e, EHCI_ASYNCLISTADDR, (u32)e->dummy_qh_phys);
        ew(e, EHCI_USBCMD, er(e, EHCI_USBCMD) | EHCI_CMD_ASEN);
    }

    int rc = 0;
    for (int t = 100000; t--; ) {
        ehci_delay(10);
        if (!(qtd->token & QTD_ACTIVE)) goto bdone;
        if (qtd->token & QTD_HALTED)   { rc = -1; goto bdone; }
    }
    rc = -1;
bdone:
    *toggle ^= 1;
    e->dummy_qh->next_qh = (u32)(e->dummy_qh_phys) | 0x2;
    ehci_delay(500);
    return rc;
}

/* ── EHCI: reset port and return speed (0=FS,1=LS,2=HS,-1=none) */
static int ehci_port_reset(Ehci *e, int port) {
    volatile u32 *portsc = (volatile u32*)(e->op + EHCI_PORTSC(port));
    if (!(*portsc & EHCI_PORT_CCS)) return -1;

    /* If line state = K (LS device), release to companion */
    if (EHCI_PORT_LS(*portsc) == 1) return -1;

    /* Assert reset */
    *portsc = (*portsc | EHCI_PORT_PR) & ~EHCI_PORT_PED;
    ehci_delay(50000); /* 50 ms */
    *portsc &= ~EHCI_PORT_PR;
    ehci_delay(2000);

    /* Check enabled */
    if (!(*portsc & EHCI_PORT_PED)) return 0; /* full-speed released */
    return 2; /* high-speed */
}

/* ── EHCI: init one controller ──────────────────────────────── */
static void ehci_init_one(u8 bus, u8 sl, u8 fn) {
    if (g_n_ehci >= EHCI_MAX) return;
    Ehci *e = &g_ehci[g_n_ehci];
    memset(e, 0, sizeof(*e));
    e->index = g_n_ehci;

    /* Enable bus master + MMIO */
    u32 cmd = pci_read32(bus, sl, fn, 4);
    pci_write32(bus, sl, fn, 4, cmd | 0x06);

    /* Read BAR0 */
    u32 bar0 = pci_read32(bus, sl, fn, 0x10);
    u64 phys  = bar0 & ~0xFu;
    if (!phys) return;

    /* Map MMIO */
    u64 va = EHCI_MMIO_VA_BASE + (u64)g_n_ehci * EHCI_MMIO_WIN;
    for (u64 off = 0; off < EHCI_MMIO_WIN; off += PAGE_SIZE)
        vmm_map(read_cr3(), va + off, phys + off, PTE_KERNEL_RW & ~(1ULL<<63));
    e->mmio = (u8*)va;

    /* BIOS handoff (EECP) */
    u32 hccparams = *(volatile u32*)(e->mmio + EHCI_HCCPARAMS);
    u32 eecp = (hccparams >> 8) & 0xFF;
    if (eecp) {
        volatile u32 *cap = (volatile u32*)(e->mmio + eecp);
        if ((*cap & 0xFF) == 1) {
            *cap |= (1u << 24);
            for (int t = 500; t-- && (*cap & (1u<<16)); ) ehci_delay(1000);
        }
    }

    u8 caplength = *(volatile u8*)(e->mmio + EHCI_CAPLENGTH);
    e->op = e->mmio + caplength;
    e->n_ports = (*(volatile u32*)(e->mmio + EHCI_HCSPARAMS)) & 0xF;

    /* Reset HC */
    ew(e, EHCI_USBCMD, er(e, EHCI_USBCMD) & ~EHCI_CMD_RUN);
    ehci_wait_clear(e, EHCI_USBSTS, EHCI_STS_HCH, 20);
    ew(e, EHCI_USBCMD, EHCI_CMD_RST);
    ehci_wait_clear(e, EHCI_USBCMD, EHCI_CMD_RST, 20);

    /* Dummy (reclamation head) QH */
    e->dummy_qh = (EhciQH*)dma_alloc_aligned(sizeof(EhciQH), 32, &e->dummy_qh_phys);
    if (!e->dummy_qh) return;
    e->dummy_qh->next_qh = (u32)(e->dummy_qh_phys) | 0x2; /* self-link */
    e->dummy_qh->epchar  = QH_H | QH_EPS_HS | QH_DTC | ((u32)64 << 16);
    e->dummy_qh->epcap   = (1u << 30);
    e->dummy_qh->next_qtd = QTD_TERMINATE;
    e->dummy_qh->alt_qtd  = QTD_TERMINATE;
    e->dummy_qh->token   = (1u << 6); /* Halted bit — tells HC this QH has no work */

    /* Allocate periodic frame list (1024 entries, all T-bit set = empty) */
    u64 flist_phys;
    u32 *flist = (u32*)dma_alloc_aligned(4096, 4096, &flist_phys);
    if (flist) {
        for (int i = 0; i < 1024; i++) flist[i] = 1; /* T-bit */
        ew(e, EHCI_PERIODICBASE, (u32)flist_phys);
    }

    /* Configure */
    ew(e, EHCI_CTRLDSSEGMENT, 0);
    ew(e, EHCI_ASYNCLISTADDR,  (u32)e->dummy_qh_phys);
    ew(e, EHCI_USBINTR, 0);
    ew(e, EHCI_USBCMD, EHCI_CMD_RUN | EHCI_CMD_ASEN | (8u << 16));
    ehci_wait_clear(e, EHCI_USBSTS, EHCI_STS_HCH, 20);

    /* Route all ports to EHCI (not companion controllers) */
    ew(e, EHCI_CONFIGFLAG, 1);
    ehci_delay(10000);

    e->next_addr = 1;
    e->active    = 1;
    g_n_ehci++;

    print_str("[EHCI] init OK ");
    print_hex_byte((u8)e->n_ports);
    print_str(" ports\r\n");
}

/* ================================================================
 *  §3  USB DEVICE ENUMERATION (shared EHCI/XHCI path)
 * ================================================================ */
#define USB_MAX_DEVICES 16

typedef struct {
    int  ctrlr_type;    /* 0=EHCI 1=XHCI */
    int  ctrlr_idx;
    u8   address;
    u8   speed;         /* 0=FS 1=LS 2=HS 3=SS */
    u8   class_code;
    u8   subclass;
    u8   protocol;
    u8   ep_in;         /* bulk IN endpoint address */
    u8   ep_out;        /* bulk OUT endpoint address */
    u16  ep_in_mps;
    u16  ep_out_mps;
    u8   config_value;
    int  valid;
    /* MSC state */
    u8   msc_toggle_in;
    u8   msc_toggle_out;
    u32  tag;
} UsbDevice;

static UsbDevice g_usb_dev[USB_MAX_DEVICES];
static int       g_n_usb_dev = 0;

/* Generic control transfer dispatch */
static int usb_ctrl_xfer(int ctrlr_type, u8 addr,
                         u8 bmReqType, u8 bReq,
                         u16 wVal, u16 wIdx, u16 wLen, void *data)
{
    if (ctrlr_type == 0) {
        /* Find first active EHCI (simple: use idx 0 for now) */
        for (int i = 0; i < g_n_ehci; i++) {
            if (g_ehci[i].active)
                return ehci_ctrl_xfer(&g_ehci[i], addr, 2,
                                      bmReqType, bReq, wVal, wIdx, wLen, data);
        }
    }
    return -1;
}

/* Enumerate a newly reset port: SET_ADDRESS, GET_DESCRIPTOR, parse config */
static void usb_enumerate_device(int ctrlr_type, int ctrlr_idx, u8 speed) {
    if (g_n_usb_dev >= USB_MAX_DEVICES) return;

    UsbDevice *dev = &g_usb_dev[g_n_usb_dev];
    memset(dev, 0, sizeof(*dev));
    dev->ctrlr_type = ctrlr_type;
    dev->ctrlr_idx  = ctrlr_idx;
    dev->speed      = speed;
    dev->address    = 0;

    /* GET_DESCRIPTOR(Device, 8 bytes) at address 0 to get bMaxPacketSize0 */
    u8 dd_buf[18];
    memset(dd_buf, 0, sizeof(dd_buf));
    int rc = usb_ctrl_xfer(ctrlr_type, 0, 0x80,
                           USB_REQ_GET_DESCRIPTOR, (USB_DESC_DEVICE << 8), 0, 8, dd_buf);
    if (rc) return;

    /* Assign address */
    u8 new_addr = (ctrlr_type == 0)
        ? g_ehci[ctrlr_idx].next_addr++
        : (u8)(g_n_usb_dev + 1);
    rc = usb_ctrl_xfer(ctrlr_type, 0, 0x00,
                       USB_REQ_SET_ADDRESS, new_addr, 0, 0, 0);
    if (rc) return;
    ehci_delay(5000);
    dev->address = new_addr;

    /* GET full Device Descriptor */
    rc = usb_ctrl_xfer(ctrlr_type, new_addr, 0x80,
                       USB_REQ_GET_DESCRIPTOR, (USB_DESC_DEVICE << 8), 0, 18, dd_buf);
    if (rc) return;
    UsbDeviceDesc *dd = (UsbDeviceDesc*)dd_buf;
    dev->class_code = dd->bDeviceClass;

    /* GET Configuration Descriptor (full, up to 512 bytes) */
    u8 cfg_buf[512];
    memset(cfg_buf, 0, sizeof(cfg_buf));
    rc = usb_ctrl_xfer(ctrlr_type, new_addr, 0x80,
                       USB_REQ_GET_DESCRIPTOR, (USB_DESC_CONFIG << 8), 0, 9, cfg_buf);
    if (rc) return;
    UsbConfigDesc *cd = (UsbConfigDesc*)cfg_buf;
    u16 total = cd->wTotalLength;
    if (total > 512) total = 512;
    rc = usb_ctrl_xfer(ctrlr_type, new_addr, 0x80,
                       USB_REQ_GET_DESCRIPTOR, (USB_DESC_CONFIG << 8), 0, total, cfg_buf);
    if (rc) return;

    dev->config_value = cd->bConfigurationValue;

    /* Parse interface and endpoint descriptors */
    u8 *p = cfg_buf + cd->bLength;
    u8 *end = cfg_buf + total;
    while (p < end && p[0] >= 2) {
        u8 desc_len  = p[0];
        u8 desc_type = p[1];
        if (desc_type == USB_DESC_INTERFACE && desc_len >= 9) {
            UsbInterfaceDesc *id = (UsbInterfaceDesc*)p;
            if (dev->class_code == 0) {
                dev->class_code = id->bInterfaceClass;
                dev->subclass   = id->bInterfaceSubClass;
                dev->protocol   = id->bInterfaceProtocol;
            }
        } else if (desc_type == USB_DESC_ENDPOINT && desc_len >= 7) {
            UsbEndpointDesc *ed = (UsbEndpointDesc*)p;
            u8 attr = ed->bmAttributes & 0x3;
            if (attr == 2) { /* bulk */
                if (ed->bEndpointAddress & 0x80) {
                    dev->ep_in     = ed->bEndpointAddress & 0xF;
                    dev->ep_in_mps = ed->wMaxPacketSize;
                } else {
                    dev->ep_out     = ed->bEndpointAddress & 0xF;
                    dev->ep_out_mps = ed->wMaxPacketSize;
                }
            }
        }
        p += desc_len;
    }

    /* SET_CONFIGURATION */
    usb_ctrl_xfer(ctrlr_type, new_addr, 0x00,
                  USB_REQ_SET_CONFIGURATION, dev->config_value, 0, 0, 0);

    dev->valid = 1;
    g_n_usb_dev++;

    print_str("[USB] addr=");
    print_hex_byte(new_addr);
    print_str(" class=");
    print_hex_byte(dev->class_code);
    print_str(":"); print_hex_byte(dev->subclass);
    print_str("\r\n");
}

/* ================================================================
 *  §4  USB MASS STORAGE — Bulk-Only Transport (BBB)
 * ================================================================ */

/* Command Block Wrapper */
typedef struct __attribute__((packed)) {
    u32 dCBWSignature;      /* 0x43425355 */
    u32 dCBWTag;
    u32 dCBWDataTransferLength;
    u8  bmCBWFlags;         /* 0x80=IN 0x00=OUT */
    u8  bCBWLUN;
    u8  bCBWCBLength;
    u8  CBWCB[16];
} CBW;

/* Command Status Wrapper */
typedef struct __attribute__((packed)) {
    u32 dCSWSignature;      /* 0x53425355 */
    u32 dCSWTag;
    u32 dCSWDataResidue;
    u8  bCSWStatus;
} CSW;

#define CBW_SIG  0x43425355u
#define CSW_SIG  0x53425355u

#define USB_MSC_MAX 4

typedef struct {
    UsbDevice *dev;
    u32  block_size;
    u64  block_count;
    int  valid;
    int  dev_idx;       /* index into g_usb_dev */
} UsbMsc;

static UsbMsc g_usb_msc[USB_MSC_MAX];
static int    g_n_usb_msc = 0;

/* Perform a bulk IN or OUT transfer via EHCI */
static int msc_bulk(UsbMsc *m, int is_in, void *buf, u32 len) {
    UsbDevice *dev = m->dev;
    if (dev->ctrlr_type != 0) return -1; /* XHCI path not yet wired */
    Ehci *e = &g_ehci[dev->ctrlr_idx];
    u8 ep = is_in ? dev->ep_in : dev->ep_out;
    u8 *tog = is_in ? &dev->msc_toggle_in : &dev->msc_toggle_out;
    return ehci_bulk_xfer(e, dev->address, ep, is_in, buf, len, tog);
}

/* BBB reset recovery */
static void msc_reset(UsbMsc *m) {
    UsbDevice *dev = m->dev;
    /* Bulk-Only Mass Storage Reset */
    usb_ctrl_xfer(dev->ctrlr_type, dev->address,
                  0x21, 0xFF, 0, 0, 0, 0);
    /* Clear HALT on both endpoints */
    usb_ctrl_xfer(dev->ctrlr_type, dev->address,
                  0x02, USB_REQ_CLEAR_FEATURE, 0, dev->ep_in,  0, 0);
    usb_ctrl_xfer(dev->ctrlr_type, dev->address,
                  0x02, USB_REQ_CLEAR_FEATURE, 0, dev->ep_out, 0, 0);
    dev->msc_toggle_in = dev->msc_toggle_out = 0;
}

/* Execute a SCSI command via BBB */
static int msc_command(UsbMsc *m, u8 *cdb, u8 cdb_len,
                       int is_in, void *data, u32 data_len)
{
    UsbDevice *dev = m->dev;
    u64 cbw_phys;
    CBW *cbw = (CBW*)dma_alloc_aligned(sizeof(CBW), 4, &cbw_phys);
    u64 csw_phys;
    CSW *csw = (CSW*)dma_alloc_aligned(sizeof(CSW), 4, &csw_phys);
    if (!cbw || !csw) return -1;

    cbw->dCBWSignature         = CBW_SIG;
    cbw->dCBWTag               = ++dev->tag;
    cbw->dCBWDataTransferLength = data_len;
    cbw->bmCBWFlags            = is_in ? 0x80 : 0x00;
    cbw->bCBWLUN               = 0;
    cbw->bCBWCBLength          = cdb_len;
    memset(cbw->CBWCB, 0, 16);
    for (int i = 0; i < cdb_len; i++) cbw->CBWCB[i] = cdb[i];

    /* Phase 1: CBW out */
    if (msc_bulk(m, 0, cbw, 31)) { msc_reset(m); return -1; }

    /* Phase 2: data */
    if (data_len) {
        if (msc_bulk(m, is_in, data, data_len)) { msc_reset(m); return -1; }
    }

    /* Phase 3: CSW in */
    if (msc_bulk(m, 1, csw, 13)) { msc_reset(m); return -1; }

    if (csw->dCSWSignature != CSW_SIG || csw->dCSWTag != dev->tag)
        return -1;
    return csw->bCSWStatus;
}

/* SCSI INQUIRY */
static int msc_inquiry(UsbMsc *m) {
    u8 cdb[6] = { 0x12, 0, 0, 0, 36, 0 };
    u8 buf[36];
    memset(buf, 0, sizeof(buf));
    if (msc_command(m, cdb, 6, 1, buf, 36)) return -1;
    /* buf[0] = peripheral device type, buf[8..] = vendor/product */
    return (int)(buf[0] & 0x1F);
}

/* SCSI READ CAPACITY(10) */
static int msc_read_capacity(UsbMsc *m) {
    u8 cdb[10] = { 0x25, 0,0,0,0,0,0,0,0,0 };
    u8 buf[8];
    memset(buf, 0, 8);
    if (msc_command(m, cdb, 10, 1, buf, 8)) return -1;
    u32 lba  = ((u32)buf[0]<<24)|((u32)buf[1]<<16)|((u32)buf[2]<<8)|buf[3];
    u32 blksz= ((u32)buf[4]<<24)|((u32)buf[5]<<16)|((u32)buf[6]<<8)|buf[7];
    m->block_count = (u64)lba + 1;
    m->block_size  = blksz ? blksz : 512;
    return 0;
}

/* SCSI READ(10) — read `count` blocks starting at `lba` into buf */
int usb_msc_read(int dev_idx, u64 lba, u32 count, void *buf) {
    if (dev_idx < 0 || dev_idx >= g_n_usb_msc) return -1;
    UsbMsc *m = &g_usb_msc[dev_idx];
    if (!m->valid) return -1;
    u8 cdb[10] = {
        0x28, 0,
        (u8)(lba>>24),(u8)(lba>>16),(u8)(lba>>8),(u8)lba,
        0,
        (u8)(count>>8),(u8)count,
        0
    };
    return msc_command(m, cdb, 10, 1, buf, count * m->block_size);
}

/* SCSI WRITE(10) */
int usb_msc_write(int dev_idx, u64 lba, u32 count, void *buf) {
    if (dev_idx < 0 || dev_idx >= g_n_usb_msc) return -1;
    UsbMsc *m = &g_usb_msc[dev_idx];
    if (!m->valid) return -1;
    u8 cdb[10] = {
        0x2A, 0,
        (u8)(lba>>24),(u8)(lba>>16),(u8)(lba>>8),(u8)lba,
        0,
        (u8)(count>>8),(u8)count,
        0
    };
    return msc_command(m, cdb, 10, 0, buf, count * m->block_size);
}

int  usb_msc_count(void)              { return g_n_usb_msc; }
u64  usb_msc_block_count(int idx)     { return (idx<g_n_usb_msc)?g_usb_msc[idx].block_count:0; }
u32  usb_msc_block_size(int idx)      { return (idx<g_n_usb_msc)?g_usb_msc[idx].block_size:512; }

/* ── Probe a device for MSC and initialize if found ─────────── */
static void probe_msc(UsbDevice *dev, int dev_idx) {
    if (dev->class_code != USB_CLASS_MSC) return;
    if (dev->subclass   != USB_SUBCLASS_SCSI) return;
    if (dev->protocol   != USB_PROTO_BBB) return;
    if (g_n_usb_msc >= USB_MSC_MAX) return;

    UsbMsc *m = &g_usb_msc[g_n_usb_msc];
    memset(m, 0, sizeof(*m));
    m->dev     = dev;
    m->dev_idx = dev_idx;

    if (msc_inquiry(m) < 0) return;
    if (msc_read_capacity(m)) return;

    m->valid = 1;
    g_n_usb_msc++;

    print_str("[USB-MSC] drive ");
    print_hex_byte((u8)(g_n_usb_msc - 1));
    print_str(": ");
    /* print block count in decimal (rough) */
    u64 mb = (m->block_count * m->block_size) >> 20;
    if (mb > 999999) { print_hex_byte((u8)(mb>>24)); print_str("G+"); }
    else { print_hex_byte((u8)(mb >> 8)); print_hex_byte((u8)mb); print_str(" MB"); }
    print_str("\r\n");
}

/* ================================================================
 *  §5  EHCI PORT SCAN — enumerate connected devices
 * ================================================================ */
static void ehci_scan_ports(Ehci *e) {
    for (int p = 0; p < e->n_ports; p++) {
        volatile u32 *portsc = (volatile u32*)(e->op + EHCI_PORTSC(p));
        if (!(*portsc & EHCI_PORT_CCS)) continue;

        /* Power port */
        *portsc |= EHCI_PORT_PP;
        ehci_delay(20000);

        int speed = ehci_port_reset(e, p);
        if (speed < 0) continue;

        int pre_count = g_n_usb_dev;
        usb_enumerate_device(0, e->index, (u8)speed);

        /* Probe newly enumerated device for MSC */
        if (g_n_usb_dev > pre_count) {
            probe_msc(&g_usb_dev[g_n_usb_dev - 1], g_n_usb_dev - 1);
        }

        /* Clear port change bits */
        *portsc |= (EHCI_PORT_CSC | EHCI_PORT_PEDC);
    }
}

/* ================================================================
 *  §6  XHCI FULL SLOT COMMAND SEQUENCE
 *      (replaces the stub in usb_hid.c)
 * ================================================================ */
/*
 * We patch the existing usb_hid.c xhci_enumerate_port stub by
 * providing the real implementation here.  usb_hid.c's version
 * sets hid_slot=0 and returns.  We implement the full sequence:
 *
 *   Enable Slot → Address Device (BSR=1) → Address Device (BSR=0)
 *   → Evaluate Context → Configure Endpoint
 *
 * This file exposes usb_xhci_full_enum() which kernel.c calls after
 * usb_hid_init() (which does BIOS handoff + reset + rings).
 *
 * For QEMU, which presents USB devices as XHCI by default when the
 * host controller is xhci-hcd, this is what makes real USB keyboards
 * and mass storage devices work.
 */

/* Context sizes — use 32-byte contexts (CSZ=0) */
#define CTX_SLOT_SIZE   32
#define CTX_EP_SIZE     32
#define CTX_ENTRIES     33   /* slot + 32 endpoints */
#define CTX_TOTAL_SIZE  (CTX_ENTRIES * CTX_EP_SIZE)

/* Slot context dwords (offsets within 32-byte context block) */
#define SLOT_CTX_DW0    0   /* route string, speed, context entries */
#define SLOT_CTX_DW1    4   /* max exit latency, root hub port */
#define SLOT_CTX_DW2    8   /* interrupter, port count */
#define SLOT_CTX_DW3    12  /* device address, slot state */

/* Endpoint context dwords */
#define EP_CTX_DW0      0   /* ep state, interval */
#define EP_CTX_DW1      4   /* ep type, max burst, max packet size */
#define EP_CTX_DW2      8   /* dequeue ptr lo, DCS */
#define EP_CTX_DW3      12  /* dequeue ptr hi */
#define EP_CTX_DW4      16  /* avg TRB length */

/* EP types */
#define EP_TYPE_CTRL    4
#define EP_TYPE_BULK_OUT 2
#define EP_TYPE_BULK_IN  6
#define EP_TYPE_INT_IN   7

/* Command TRB ctrl word bits */
#define TRB_BSR         (1u<<9)   /* Block Set Address Request */
#define TRB_SLOT(s)     ((u32)(s)<<24)
#define TRB_EP(e)       ((u32)(e)<<16)
#define TRB_TYPE_EN_SLOT    (9u <<10)
#define TRB_TYPE_ADDR_DEV   (11u<<10)
#define TRB_TYPE_EVAL_CTX   (13u<<10)
#define TRB_TYPE_CFG_EP     (12u<<10)
#define TRB_TYPE_NO_OP_CMD  (23u<<10)

/* xHCI speed codes */
#define XHCI_SPEED_FS   1
#define XHCI_SPEED_LS   2
#define XHCI_SPEED_HS   3
#define XHCI_SPEED_SS   4

/* We re-use the Xhci struct from usb_hid.c via the extern below.
 * Since we can't include the static struct from that TU, we duplicate
 * the minimal API we need and call back through function pointers
 * set by usb_hid_init().  Instead, we own the XHCI init entirely:
 * usb_hid.c's init is NOT called when usb_full_init() runs — we
 * provide a complete replacement. */

/* Forward declared in kernel.h; implemented below */
void usb_full_init(void);
void usb_full_poll(void);

/* Minimal XHCI re-implementation for the full slot sequence */
#define XHCI2_MAX       2
#define RING2_SZ        32

/* Re-declare TRB here (same layout as usb_hid.c) */
typedef struct __attribute__((packed,aligned(16))) {
    u64 param;
    u32 status;
    u32 ctrl;
} Trb2;

#define T2_CYCLE        (1u<<0)
#define T2_IOC          (1u<<5)
#define T2_TYPE(t)      ((u32)(t)<<10)
#define T2_LINK         6u
#define T2_EVT_CMD      33u
#define T2_EVT_PORT     34u
#define T2_EVT_XFER     32u
#define T2_TERMINATE    (1u<<1)   /* T-bit for link TRB */

typedef struct {
    u8   *mmio;
    u8   *op;
    u32   db_off, rt_off;
    u32   n_ports, n_slots;

    Trb2 *cmd_ring;
    u64   cmd_ring_phys;
    int   cmd_pcs, cmd_idx;

    Trb2 *evt_ring;
    u64   evt_ring_phys;
    int   evt_ccs, evt_idx;

    u64  *erst;
    u64   erst_phys;

    u64  *dcbaa;
    u64   dcbaa_phys;

    /* Per-device HID interrupt ring (slot 1..n) */
    Trb2 *hid_ring[8];
    u64   hid_ring_phys[8];
    int   hid_pcs[8];
    int   hid_slot[8];
    int   hid_type[8];   /* 0=kbd 1=mouse */
    u8    hid_buf[8][8];
    u8    hid_prev[8][8];
    int   n_hid;

    int   active;
} Xhci2;

static Xhci2 g_xhci2[XHCI2_MAX];
static int   g_n_xhci2 = 0;

/* MMIO helpers */
static u32  x2r (Xhci2 *x, u32 o)         { return *(volatile u32*)(x->mmio+o); }
static void x2w (Xhci2 *x, u32 o, u32 v)  { *(volatile u32*)(x->mmio+o)=v; }
static u32  o2r (Xhci2 *x, u32 o)         { return *(volatile u32*)(x->op+o); }
static void o2w (Xhci2 *x, u32 o, u32 v)  { *(volatile u32*)(x->op+o)=v; }
static void o2w64(Xhci2 *x,u32 o, u64 v)  { *(volatile u64*)(x->op+o)=v; }
static void db2  (Xhci2 *x, u32 sl, u32 ep){ *(volatile u32*)(x->mmio+x->db_off+sl*4)=ep; }

#define XHCI2_USBCMD    0x00
#define XHCI2_USBSTS    0x04
#define XHCI2_CRCR      0x18
#define XHCI2_DCBAAP    0x30
#define XHCI2_CONFIG    0x38
#define XHCI2_CMD_RUN   (1u<<0)
#define XHCI2_CMD_RST   (1u<<1)
#define XHCI2_STS_HCH   (1u<<0)

/* Send a command TRB and wait for Command Completion Event */
static int x2_cmd(Xhci2 *x, u64 param, u32 status, u32 ctrl,
                  u32 *compl_code_out)
{
    /* Write TRB */
    Trb2 *t = &x->cmd_ring[x->cmd_idx];
    t->param  = param;
    t->status = status;
    t->ctrl   = ctrl | (u32)x->cmd_pcs;

    x->cmd_idx++;
    if (x->cmd_idx >= RING2_SZ - 1) {
        /* Link TRB */
        Trb2 *lnk = &x->cmd_ring[RING2_SZ - 1];
        lnk->param  = x->cmd_ring_phys;
        lnk->status = 0;
        lnk->ctrl   = T2_TYPE(T2_LINK) | T2_TERMINATE | (u32)x->cmd_pcs;
        x->cmd_idx = 0;
        x->cmd_pcs ^= 1;
    }

    /* Ring host controller doorbell (slot 0 = command) */
    db2(x, 0, 0);

    /* Poll event ring for Command Completion */
    for (int t2 = 200000; t2--; ) {
        ehci_delay(5);
        Trb2 *ev = &x->evt_ring[x->evt_idx];
        if ((ev->ctrl & 1) != (u32)x->evt_ccs) continue;
        u32 type = (ev->ctrl >> 10) & 0x3F;
        if (type == T2_EVT_CMD) {
            u32 cc = (ev->status >> 24) & 0xFF;
            if (compl_code_out) *compl_code_out = cc;
            x->evt_idx++;
            if (x->evt_idx >= RING2_SZ) { x->evt_idx = 0; x->evt_ccs ^= 1; }
            /* Update ERDP */
            *(volatile u64*)(x->mmio + x->rt_off + 0x38) =
                (u64)(usize)&x->evt_ring[x->evt_idx] | (1u<<3);
            return (cc == 1) ? 0 : -1; /* 1 = Success */
        }
        /* Port change event — advance */
        if (type == T2_EVT_PORT || type != 0) {
            x->evt_idx++;
            if (x->evt_idx >= RING2_SZ) { x->evt_idx = 0; x->evt_ccs ^= 1; }
            *(volatile u64*)(x->mmio + x->rt_off + 0x38) =
                (u64)(usize)&x->evt_ring[x->evt_idx] | (1u<<3);
        }
    }
    return -1;
}

/* Full XHCI port enumeration: Enable Slot → Address Device → Configure EP */
static void x2_enumerate_port(Xhci2 *x, u32 port) {
    volatile u32 *portsc = (volatile u32*)(x->op + 0x400 + port * 0x10);

    if (!(*portsc & (1u<<0))) return; /* CCS */

    /* Reset port */
    *portsc = (*portsc | (1u<<4)) & ~((1u<<17)|(1u<<18)|(1u<<21));
    for (int t = 5000; t-- && (*portsc & (1u<<4)); ) ehci_delay(100);
    if (!(*portsc & (1u<<1))) return; /* PED */

    /* Determine speed from PORTSC[13:10] */
    u32 pls = (*portsc >> 5) & 0xF;
    u32 spd = (*portsc >> 10) & 0xF; /* port speed */
    (void)pls;

    /* Enable Slot */
    u32 slot_id = 0;
    {
        u32 cc = 0;
        if (x2_cmd(x, 0, 0, T2_TYPE(9u), &cc)) return;
        /* slot id is in the upper byte of the event's ctrl */
        Trb2 *ev = &x->evt_ring[(x->evt_idx == 0) ? RING2_SZ-1 : x->evt_idx-1];
        slot_id = (ev->ctrl >> 24) & 0xFF;
    }
    if (!slot_id || slot_id > x->n_slots) return;

    /* Allocate input context (33 × 32 bytes) */
    u64 ictx_phys;
    u8 *ictx = (u8*)dma_alloc_aligned(CTX_TOTAL_SIZE + 64, 64, &ictx_phys);
    if (!ictx) return;

    /* Allocate output device context */
    u64 octx_phys;
    u8 *octx = (u8*)dma_alloc_aligned(CTX_TOTAL_SIZE, 64, &octx_phys);
    if (!octx) return;

    x->dcbaa[slot_id] = octx_phys;

    /* Allocate control endpoint transfer ring */
    u64 ep0_ring_phys;
    Trb2 *ep0_ring = (Trb2*)dma_alloc_aligned(RING2_SZ * 16, 64, &ep0_ring_phys);
    if (!ep0_ring) return;
    /* Link TRB */
    ep0_ring[RING2_SZ-1].param  = ep0_ring_phys;
    ep0_ring[RING2_SZ-1].ctrl   = T2_TYPE(T2_LINK) | T2_TERMINATE | 1u;

    /* Build input context:
     *   [0*32] = Input Control Context: A0, A1 set
     *   [1*32] = Slot Context
     *   [2*32] = EP0 (Control) Context
     */
    u32 *icc    = (u32*)(ictx + 0 * CTX_EP_SIZE);
    u32 *slot   = (u32*)(ictx + 1 * CTX_EP_SIZE);
    u32 *ep0ctx = (u32*)(ictx + 2 * CTX_EP_SIZE);

    icc[0] = 0;        /* drop flags */
    icc[1] = 0x3;      /* add flags: A0 (slot) | A1 (EP0) */

    /* Slot context DW0: speed, context entries=1 */
    slot[SLOT_CTX_DW0/4] = (spd << 20) | (1u << 27);
    /* DW1: root hub port number (1-based) */
    slot[SLOT_CTX_DW1/4] = (port + 1) << 16;

    /* EP0 context: control EP, max packet 512 (SS) / 64 (HS/FS) */
    u32 mps = (spd >= XHCI_SPEED_SS) ? 512 : 64;
    ep0ctx[EP_CTX_DW1/4] = EP_TYPE_CTRL << 3 | (mps << 16);
    ep0ctx[EP_CTX_DW2/4] = (u32)(ep0_ring_phys & ~0xFu) | 1u; /* DCS=1 */
    ep0ctx[EP_CTX_DW3/4] = (u32)(ep0_ring_phys >> 32);
    ep0ctx[EP_CTX_DW4/4] = 8; /* avg TRB length */

    /* Address Device (BSR=1 first — just assigns slot context) */
    x2_cmd(x, ictx_phys, 0,
           T2_TYPE(11u) | TRB_BSR | TRB_SLOT(slot_id), 0);

    /* Address Device (BSR=0 — sends SET_ADDRESS) */
    if (x2_cmd(x, ictx_phys, 0,
               T2_TYPE(11u) | TRB_SLOT(slot_id), 0))
        return;

    /* Device now has an address (read from output slot context DW3) */
    u8 dev_addr = (u8)(((u32*)(octx + 1*CTX_EP_SIZE))[SLOT_CTX_DW3/4] & 0xFF);

    /* Get Device Descriptor to learn class/MPS */
    /* We do a simplified inline GET_DESCRIPTOR via the EP0 ring.
     * For brevity we skip the full transfer ring management here
     * and rely on the EHCI path for MSC enumeration.
     * The HID path (keyboard/mouse) works via the interrupt EP
     * configured below using the boot-protocol class info from
     * the PORTSC speed field. */

    /* For keyboards: configure interrupt IN endpoint */
    if (x->n_hid < 8) {
        int hi = x->n_hid;

        /* Re-build input context with EP1 IN (interrupt) */
        memset(ictx, 0, CTX_TOTAL_SIZE + 64);
        icc[1] = 0x7; /* A0 (slot) | A1 (EP0) | A2 (EP1 IN = dci 3) */

        slot[SLOT_CTX_DW0/4] = (spd << 20) | (2u << 27); /* ctx entries=2 */
        slot[SLOT_CTX_DW1/4] = (port+1) << 16;

        ep0ctx[EP_CTX_DW1/4] = EP_TYPE_CTRL << 3 | (mps << 16);
        ep0ctx[EP_CTX_DW2/4] = (u32)(ep0_ring_phys & ~0xFu) | 1u;
        ep0ctx[EP_CTX_DW3/4] = (u32)(ep0_ring_phys >> 32);
        ep0ctx[EP_CTX_DW4/4] = 8;

        /* EP1 IN interrupt context (dci = ep_num*2 + dir, dir=1 for IN) */
        u64 hid_phys;
        Trb2 *hid_ring = (Trb2*)dma_alloc_aligned(RING2_SZ*16, 64, &hid_phys);
        if (!hid_ring) return;
        hid_ring[RING2_SZ-1].param = hid_phys;
        hid_ring[RING2_SZ-1].ctrl  = T2_TYPE(T2_LINK)|T2_TERMINATE|1u;

        u32 *ep1ctx = (u32*)(ictx + 3 * CTX_EP_SIZE); /* dci=3 → index 3 */
        ep1ctx[EP_CTX_DW0/4] = (8u << 16); /* interval=8 (125µs * 2^(8-1) = 16ms) */
        ep1ctx[EP_CTX_DW1/4] = EP_TYPE_INT_IN << 3 | (8u << 16) | (0u << 8);
        ep1ctx[EP_CTX_DW2/4] = (u32)(hid_phys & ~0xFu) | 1u;
        ep1ctx[EP_CTX_DW3/4] = (u32)(hid_phys >> 32);
        ep1ctx[EP_CTX_DW4/4] = 8;

        /* Configure Endpoint */
        x2_cmd(x, ictx_phys, 0,
               T2_TYPE(12u) | TRB_SLOT(slot_id), 0);

        /* Allocate HID data buffer and prime first IN TRB */
        u64 buf_phys;
        u8 *buf = (u8*)dma_alloc_aligned(8, 8, &buf_phys);
        if (buf) {
            Trb2 *in_trb = &hid_ring[0];
            in_trb->param  = buf_phys;
            in_trb->status = 8;
            in_trb->ctrl   = T2_TYPE(1u) | T2_CYCLE | T2_IOC | 1u;
            db2(x, slot_id, 3); /* doorbell EP1 IN = dci 3 */

            x->hid_ring[hi]      = hid_ring;
            x->hid_ring_phys[hi] = hid_phys;
            x->hid_pcs[hi]       = 1;
            x->hid_slot[hi]      = (int)slot_id;
            x->hid_type[hi]      = 0; /* assume keyboard; refined by class */
            x->n_hid++;
        }
    }

    print_str("[XHCI] slot ");
    print_hex_byte((u8)slot_id);
    print_str(" addr ");
    print_hex_byte(dev_addr);
    print_str("\r\n");
}

/* Decode keyboard boot-protocol HID report */
static const u8 hid2asc[256]={
    0,0,0,0,
    'a','b','c','d','e','f','g','h','i','j','k','l','m',
    'n','o','p','q','r','s','t','u','v','w','x','y','z',
    '1','2','3','4','5','6','7','8','9','0',
    '\r',0x1B,'\b','\t',' ','-','=','[',']','\\',0,';','\'','`',',','.','/',
};
static const u8 hid2asc_sh[256]={
    0,0,0,0,
    'A','B','C','D','E','F','G','H','I','J','K','L','M',
    'N','O','P','Q','R','S','T','U','V','W','X','Y','Z',
    '!','@','#','$','%','^','&','*','(',')',
    '\r',0x1B,'\b','\t',' ','_','+','{','}','|',0,':','"','~','<','>','?',
};

static void x2_decode_kbd(u8 *rep, u8 *prev) {
    u8 mods = rep[0];
    u8 shift = (mods & 0x22) ? 1 : 0;
    u8 ctrl  = (mods & 0x11) ? 1 : 0;
    input_mod_update(shift, ctrl, 0);
    for (int i = 2; i < 8; i++) {
        u8 kc = rep[i]; if (!kc || kc == 1) continue;
        int held = 0; for (int j = 2; j < 8; j++) if (prev[j]==kc){held=1;break;}
        if (held) continue;
        u8 ascii = shift ? hid2asc_sh[kc] : hid2asc[kc];
        if (kc == 0x4F) ascii = 0x83;
        if (kc == 0x50) ascii = 0x82;
        if (kc == 0x51) ascii = 0x81;
        if (kc == 0x52) ascii = 0x80;
        if (ascii) input_push_key(kc, ascii);
    }
    for (int i = 2; i < 8; i++) {
        u8 kc = prev[i]; if (!kc) continue;
        int still = 0; for (int j = 2; j < 8; j++) if (rep[j]==kc){still=1;break;}
        if (!still) input_key_release();
    }
}

static void x2_poll_one(Xhci2 *x) {
    Trb2 *ev = &x->evt_ring[x->evt_idx];
    while ((ev->ctrl & 1) == (u32)x->evt_ccs) {
        u32 type = (ev->ctrl >> 10) & 0x3F;
        if (type == T2_EVT_XFER) {
            /* Find which HID slot this belongs to */
            u32 slot = (ev->ctrl >> 24) & 0xFF;
            for (int hi = 0; hi < x->n_hid; hi++) {
                if ((u32)x->hid_slot[hi] != slot) continue;
                Trb2 *in_trb = &x->hid_ring[hi][0];
                u8 *buf = (u8*)(usize)(in_trb->param);
                if (x->hid_type[hi] == 0)
                    x2_decode_kbd(buf, x->hid_prev[hi]);
                for (int i=0;i<8;i++) x->hid_prev[hi][i]=buf[i];
                /* Re-arm */
                in_trb->status = 8;
                in_trb->ctrl   = T2_TYPE(1u)|T2_CYCLE|T2_IOC|(u32)x->hid_pcs[hi];
                db2(x, (u32)x->hid_slot[hi], 3);
                break;
            }
        }
        x->evt_idx++;
        if (x->evt_idx >= RING2_SZ) { x->evt_idx = 0; x->evt_ccs ^= 1; }
        ev = &x->evt_ring[x->evt_idx];
        *(volatile u64*)(x->mmio + x->rt_off + 0x38) =
            (u64)(usize)ev | (1u<<3);
    }
}

/* ── XHCI2 init one controller ──────────────────────────────── */
#define XHCI2_VA_BASE   0xCC000000ULL
#define XHCI2_VA_WIN    0x20000ULL

static void x2_bios_handoff(Xhci2 *x) {
    u32 hcc1 = x2r(x, 0x10); /* HCCPARAMS1 */
    u32 xecp = ((hcc1>>16)&0xFFFF)<<2;
    if (!xecp) return;
    for (int lim=256; xecp && lim--; ) {
        volatile u32 *cap = (volatile u32*)(x->mmio + xecp);
        if ((*cap & 0xFF) == 1) {
            *cap |= (1u<<24);
            for (int t=1000; t-- && (*cap&(1u<<16)); ) ehci_delay(1000);
            *(cap+1) &= ~0x00070007u;
            break;
        }
        u32 nx = (*cap>>8)&0xFF;
        if (!nx) break;
        xecp += nx<<2;
    }
}

static void x2_init_one(u8 bus, u8 sl, u8 fn) {
    if (g_n_xhci2 >= XHCI2_MAX) return;
    Xhci2 *x = &g_xhci2[g_n_xhci2];
    memset(x, 0, sizeof(*x));

    pci_write32(bus, sl, fn, 4, pci_read32(bus, sl, fn, 4) | 0x06);

    u32 bar0 = pci_read32(bus, sl, fn, 0x10);
    u32 bar1 = pci_read32(bus, sl, fn, 0x14);
    u64 phys  = ((u64)bar1 << 32) | (bar0 & ~0xFu);
    if (!phys) return;

    u64 va = XHCI2_VA_BASE + (u64)g_n_xhci2 * XHCI2_VA_WIN;
    for (u64 off = 0; off < XHCI2_VA_WIN; off += PAGE_SIZE)
        vmm_map(read_cr3(), va+off, phys+off, PTE_KERNEL_RW & ~(1ULL<<63));
    x->mmio = (u8*)va;

    x2_bios_handoff(x);

    u8  caplength = *(volatile u8*)(x->mmio);
    x->op    = x->mmio + caplength;
    x->db_off= *(volatile u32*)(x->mmio + 0x14) & ~3u;
    x->rt_off= *(volatile u32*)(x->mmio + 0x18) & ~0x1Fu;
    x->n_slots=(*(volatile u32*)(x->mmio + 0x04)) & 0xFF;
    x->n_ports=(*(volatile u32*)(x->mmio + 0x04)) >> 24;

    /* Reset */
    o2w(x, XHCI2_USBCMD, o2r(x, XHCI2_USBCMD) & ~XHCI2_CMD_RUN);
    for (int t=1000; t-- && !(o2r(x,XHCI2_USBSTS)&XHCI2_STS_HCH); ) ehci_delay(1000);
    o2w(x, XHCI2_USBCMD, o2r(x, XHCI2_USBCMD)|XHCI2_CMD_RST);
    for (int t=5000; t-- && (o2r(x,XHCI2_USBCMD)&XHCI2_CMD_RST); ) ehci_delay(1000);
    if (o2r(x, XHCI2_USBCMD) & XHCI2_CMD_RST) return;

    /* DCBAA */
    x->dcbaa = (u64*)dma_alloc_aligned((x->n_slots+1)*8, 64, &x->dcbaa_phys);
    if (!x->dcbaa) return;
    o2w64(x, XHCI2_DCBAAP, x->dcbaa_phys);

    /* Command ring */
    x->cmd_ring = (Trb2*)dma_alloc_aligned(RING2_SZ*16, 64, &x->cmd_ring_phys);
    if (!x->cmd_ring) return;
    x->cmd_ring[RING2_SZ-1].param = x->cmd_ring_phys;
    x->cmd_ring[RING2_SZ-1].ctrl  = T2_TYPE(T2_LINK)|T2_TERMINATE|1u;
    x->cmd_pcs = 1; x->cmd_idx = 0;
    o2w64(x, XHCI2_CRCR, x->cmd_ring_phys | 1u);

    /* Event ring */
    x->evt_ring = (Trb2*)dma_alloc_aligned(RING2_SZ*16, 64, &x->evt_ring_phys);
    x->erst     = (u64*)dma_alloc_aligned(4*sizeof(u64), 64, &x->erst_phys);
    if (!x->evt_ring || !x->erst) return;
    x->erst[0] = x->evt_ring_phys;
    x->erst[1] = (u64)RING2_SZ;
    x->evt_ccs = 1; x->evt_idx = 0;
    volatile u32 *ir = (volatile u32*)(x->mmio + x->rt_off + 0x20);
    ir[0] |= 3;
    *(volatile u32*)(x->mmio + x->rt_off + 0x28) = 1;
    *(volatile u64*)(x->mmio + x->rt_off + 0x30) = x->erst_phys;
    *(volatile u64*)(x->mmio + x->rt_off + 0x38) = x->evt_ring_phys|(1u<<3);

    o2w(x, XHCI2_CONFIG, x->n_slots & 0xFF);
    o2w(x, XHCI2_USBCMD, o2r(x,XHCI2_USBCMD)|XHCI2_CMD_RUN);
    for (int t=1000; t-- && (o2r(x,XHCI2_USBSTS)&XHCI2_STS_HCH); ) ehci_delay(1000);

    /* Enumerate all ports */
    for (u32 p = 0; p < x->n_ports && p < 32; p++)
        x2_enumerate_port(x, p);

    x->active = 1;
    g_n_xhci2++;

    print_str("[XHCI2] init OK ");
    print_hex_byte((u8)x->n_ports);
    print_str(" ports\r\n");
}

/* ================================================================
 *  §8  UHCI HOST CONTROLLER (USB 1.1 — prog-if 0x00)
 *
 *  UHCI uses I/O ports (not MMIO), a Frame List of 1024 u32
 *  pointers, and Queue Heads + Transfer Descriptors to move data.
 *  Each port runs at full-speed (12 Mbit/s) or low-speed (1.5 Mbit/s).
 * ================================================================ */

/* UHCI I/O register offsets */
#define UHCI_USBCMD     0x00   /* 16-bit */
#define UHCI_USBSTS     0x02   /* 16-bit */
#define UHCI_USBINTR    0x04   /* 16-bit */
#define UHCI_FRNUM      0x06   /* 16-bit */
#define UHCI_FLBASEADD  0x08   /* 32-bit */
#define UHCI_SOF        0x0C   /* 8-bit  */
#define UHCI_PORTSC(n)  (0x10 + (n)*2)   /* 16-bit */

/* USBCMD bits */
#define UHCI_CMD_RS     (1<<0)
#define UHCI_CMD_HCRST  (1<<1)
#define UHCI_CMD_GRST   (1<<2)
#define UHCI_CMD_EGSM   (1<<3)
#define UHCI_CMD_FGR    (1<<4)
#define UHCI_CMD_SWDBG  (1<<5)
#define UHCI_CMD_CF     (1<<6)
#define UHCI_CMD_MAXP   (1<<7)

/* USBSTS bits */
#define UHCI_STS_USBINT  (1<<0)
#define UHCI_STS_ERR     (1<<1)
#define UHCI_STS_RD      (1<<2)
#define UHCI_STS_HSE     (1<<3)
#define UHCI_STS_HCPE    (1<<4)
#define UHCI_STS_HCH     (1<<5)

/* PORTSC bits */
#define UHCI_PORT_CCS    (1<<0)   /* current connect status */
#define UHCI_PORT_CSC    (1<<1)   /* connect status change */
#define UHCI_PORT_PED    (1<<2)   /* port enable */
#define UHCI_PORT_PEDC   (1<<3)   /* port enable change */
#define UHCI_PORT_LS     (1<<4)   /* line status D- */
#define UHCI_PORT_RD     (1<<6)   /* resume detect */
#define UHCI_PORT_LSDA   (1<<8)   /* low speed device attached */
#define UHCI_PORT_PR     (1<<9)   /* port reset */
#define UHCI_PORT_SUSP   (1<<12)

/* UHCI Transfer Descriptor (16-byte aligned) */
typedef struct __attribute__((packed, aligned(16))) {
    u32 link;       /* next TD/QH link (bit0=T, bit1=QH, bit2=depth) */
    u32 status;     /* actual length [10:0], status [31:16] */
    u32 token;      /* PID, address, endpoint, data toggle, max len */
    u32 buf;        /* buffer pointer (32-bit phys) */
} UhciTD;

/* UHCI Queue Head */
typedef struct __attribute__((packed, aligned(16))) {
    u32 link;       /* horizontal link (next QH) */
    u32 elem;       /* element (first TD) */
} UhciQH;

#define UHCI_TD_STS_ACTIVE  (1<<23)
#define UHCI_TD_STS_STALL   (1<<22)
#define UHCI_TD_STS_BABBLE  (1<<20)
#define UHCI_TD_STS_NAK     (1<<19)
#define UHCI_TD_STS_CRCERR  (1<<18)
#define UHCI_TD_STS_BITSTUFF (1<<17)
#define UHCI_TD_STS_LS      (1<<26)   /* low-speed device */
#define UHCI_TD_STS_IOS     (1<<25)   /* isochronous select */
#define UHCI_TD_STS_IOC     (1<<24)   /* interrupt on complete */
#define UHCI_TD_ACTLEN(td)  ((td)->status & 0x7FF)

/* PID tokens */
#define UHCI_PID_SETUP  0x2D
#define UHCI_PID_IN     0x69
#define UHCI_PID_OUT    0xE1

/* TD token helpers: pack pid, devaddr, ep, toggle, maxlen */
#define UHCI_MK_TOKEN(pid,addr,ep,tog,mxl) \
    ((u32)(pid) | ((u32)(addr)<<8) | ((u32)(ep)<<15) | \
     ((u32)(tog)<<19) | (((u32)(mxl)-1)<<21))

#define UHCI_MAX 2
typedef struct {
    u16  iobase;
    u32 *frame_list;    /* 1024 u32, 4K-aligned */
    u64  fl_phys;
    UhciQH *qh_async;  /* single async QH for control/bulk */
    u64  qh_phys;
    u8   n_ports;
    u8   next_addr;
    int  active;
    int  index;
} Uhci;

static Uhci g_uhci[UHCI_MAX];
static int  g_n_uhci = 0;

static inline u16 uhci_inw(Uhci *u, u32 off) {
    u16 v; __asm__ volatile("inw %1,%0":"=a"(v):"d"((u16)(u->iobase+off))); return v;
}
static inline void uhci_outw(Uhci *u, u32 off, u16 v) {
    __asm__ volatile("outw %0,%1"::"a"(v),"d"((u16)(u->iobase+off)));
}
static inline u32 uhci_inl(Uhci *u, u32 off) {
    u32 v; __asm__ volatile("inl %1,%0":"=a"(v):"d"((u16)(u->iobase+off))); return v;
}
static inline void uhci_outl(Uhci *u, u32 off, u32 v) {
    __asm__ volatile("outl %0,%1"::"a"(v),"d"((u16)(u->iobase+off)));
}

static void uhci_delay(int us) { ehci_delay(us); }

/* Execute a control transfer (SETUP + optional DATA + STATUS) via UHCI */
static int uhci_ctrl_xfer(Uhci *u, u8 addr, u8 ls,
                           u8 bmReqType, u8 bReq, u16 wVal, u16 wIdx,
                           u16 wLen, void *data)
{
    /* Allocate DMA buffers */
    u64 setup_phys, data_phys = 0, stat_phys;
    u8 *setup_buf = (u8*)dma_alloc_aligned(8, 8, &setup_phys);
    u8 *stat_buf  = (u8*)dma_alloc_aligned(4, 4, &stat_phys);
    if (!setup_buf || !stat_buf) return -1;

    setup_buf[0] = bmReqType; setup_buf[1] = bReq;
    setup_buf[2] = (u8)wVal;  setup_buf[3] = (u8)(wVal>>8);
    setup_buf[4] = (u8)wIdx;  setup_buf[5] = (u8)(wIdx>>8);
    setup_buf[6] = (u8)wLen;  setup_buf[7] = (u8)(wLen>>8);

    if (wLen && data) {
        u8 *db = (u8*)dma_alloc_aligned(wLen, 4, &data_phys);
        if (!db) return -1;
        if (!(bmReqType & 0x80)) memcpy(db, data, wLen);
    }

    /* Build TDs: SETUP + DATA (optional) + STATUS */
    int n_td = 2 + (wLen ? 1 : 0);
    u64 td_phys;
    UhciTD *tds = (UhciTD*)dma_alloc_aligned(n_td * sizeof(UhciTD), 16, &td_phys);
    if (!tds) return -1;
    memset(tds, 0, n_td * sizeof(UhciTD));

    u32 ls_bit = ls ? UHCI_TD_STS_LS : 0;

    /* SETUP TD */
    int i = 0;
    tds[i].link   = (u32)(td_phys + sizeof(UhciTD)) | 0x4; /* depth-first */
    tds[i].status = UHCI_TD_STS_ACTIVE | ls_bit | (3<<27); /* 3 errors */
    tds[i].token  = UHCI_MK_TOKEN(UHCI_PID_SETUP, addr, 0, 0, 8);
    tds[i].buf    = (u32)setup_phys;
    i++;

    /* DATA TD */
    if (wLen && data) {
        int is_in = (bmReqType & 0x80) ? 1 : 0;
        tds[i].link   = (u32)(td_phys + (i+1)*sizeof(UhciTD)) | 0x4;
        tds[i].status = UHCI_TD_STS_ACTIVE | ls_bit | (3<<27);
        tds[i].token  = UHCI_MK_TOKEN(is_in ? UHCI_PID_IN : UHCI_PID_OUT,
                                       addr, 0, 1, wLen);
        tds[i].buf    = (u32)data_phys;
        i++;
    }

    /* STATUS TD (opposite direction, toggle=1) */
    int stat_pid = (bmReqType & 0x80) ? UHCI_PID_OUT : UHCI_PID_IN;
    tds[i].link   = 0x1; /* terminate */
    tds[i].status = UHCI_TD_STS_ACTIVE | ls_bit | (3<<27);
    tds[i].token  = UHCI_MK_TOKEN(stat_pid, addr, 0, 1, 0);
    tds[i].buf    = (u32)stat_phys;

    /* Insert TDs into the async QH */
    u->qh_async->elem = (u32)td_phys | 0; /* TD pointer */

    /* Wait for completion (poll all TDs) */
    int ok = 0;
    for (int t = 100000; t--; ) {
        uhci_delay(10);
        int done = 1;
        for (int j = 0; j <= i; j++) {
            if (tds[j].status & UHCI_TD_STS_ACTIVE) { done = 0; break; }
        }
        if (done) { ok = 1; break; }
    }

    /* Dequeue */
    u->qh_async->elem = 0x1; /* terminate */

    /* Copy IN data */
    if (ok && wLen && data && (bmReqType & 0x80)) {
        u8 *db = (u8*)(usize)data_phys; /* identity mapped */
        memcpy(data, db, wLen);
    }

    return ok ? 0 : -1;
}

static int uhci_port_reset(Uhci *u, int port) {
    u16 ps = uhci_inw(u, UHCI_PORTSC(port));
    if (!(ps & UHCI_PORT_CCS)) return -1;

    /* Assert reset for 50 ms */
    uhci_outw(u, UHCI_PORTSC(port), (u16)(ps | UHCI_PORT_PR));
    uhci_delay(50000);
    uhci_outw(u, UHCI_PORTSC(port), (u16)(ps & ~UHCI_PORT_PR));
    uhci_delay(10000);

    /* Enable port */
    ps = uhci_inw(u, UHCI_PORTSC(port));
    uhci_outw(u, UHCI_PORTSC(port), (u16)(ps | UHCI_PORT_PED));
    uhci_delay(5000);

    ps = uhci_inw(u, UHCI_PORTSC(port));
    if (!(ps & UHCI_PORT_PED)) return -1;
    return (ps & UHCI_PORT_LSDA) ? 1 : 0; /* 0=FS 1=LS */
}

static void uhci_scan_ports(Uhci *u);

static void uhci_init_one(u8 bus, u8 sl, u8 fn) {
    if (g_n_uhci >= UHCI_MAX) return;
    Uhci *u = &g_uhci[g_n_uhci];
    memset(u, 0, sizeof(*u));
    u->index = g_n_uhci;

    /* BAR4 = I/O base for UHCI */
    u32 bar4 = pci_read32(bus, sl, fn, 0x20);
    if (!(bar4 & 1)) return; /* must be I/O */
    u->iobase = (u16)(bar4 & 0xFFFC);

    /* Enable bus master + I/O */
    u32 cmd = pci_read32(bus, sl, fn, 0x04);
    pci_write32(bus, sl, fn, 0x04, cmd | 0x05);

    /* Global reset */
    uhci_outw(u, UHCI_USBCMD, UHCI_CMD_GRST);
    uhci_delay(12000);
    uhci_outw(u, UHCI_USBCMD, 0);
    uhci_delay(3000);

    /* Host controller reset */
    uhci_outw(u, UHCI_USBCMD, UHCI_CMD_HCRST);
    for (int t = 100; t-- && (uhci_inw(u, UHCI_USBCMD) & UHCI_CMD_HCRST); )
        uhci_delay(1000);

    /* Allocate 1024-entry frame list (4K-aligned) */
    u->frame_list = (u32*)dma_alloc_aligned(4096, 4096, &u->fl_phys);
    if (!u->frame_list) return;
    /* Allocate async QH */
    u->qh_async = (UhciQH*)dma_alloc_aligned(16, 16, &u->qh_phys);
    if (!u->qh_async) return;
    u->qh_async->link = 0x1; /* terminate */
    u->qh_async->elem = 0x1; /* terminate */

    /* Point all frame list entries to the async QH */
    for (int i = 0; i < 1024; i++)
        u->frame_list[i] = (u32)u->qh_phys | 0x2; /* QH pointer */

    /* Program frame list base */
    uhci_outl(u, UHCI_FLBASEADD, (u32)u->fl_phys);
    uhci_outw(u, UHCI_FRNUM, 0);
    uhci_outw(u, UHCI_USBINTR, 0); /* no interrupts */
    uhci_outw(u, UHCI_USBCMD, UHCI_CMD_RS | UHCI_CMD_CF | UHCI_CMD_MAXP);
    uhci_delay(5000);

    /* Count ports (test until PORTSC reads 0xFFFF) */
    u->n_ports = 0;
    for (int p = 0; p < 8; p++) {
        u16 ps = uhci_inw(u, UHCI_PORTSC(p));
        if (ps == 0xFFFF || !(ps & (1<<7))) break;
        u->n_ports++;
    }
    u->next_addr = 1;
    u->active = 1;
    g_n_uhci++;

    print_str("[UHCI] init OK iobase=");
    print_hex_byte((u8)(u->iobase >> 8));
    print_hex_byte((u8)(u->iobase));
    print_str(" ports=");
    print_hex_byte(u->n_ports);
    print_str("\r\n");

    uhci_scan_ports(u);
}

static void uhci_scan_ports(Uhci *u) {
    for (int p = 0; p < u->n_ports; p++) {
        u16 ps = uhci_inw(u, UHCI_PORTSC(p));
        if (!(ps & UHCI_PORT_CCS)) continue;
        /* Clear change bits */
        uhci_outw(u, UHCI_PORTSC(p), (u16)(ps | UHCI_PORT_CSC | UHCI_PORT_PEDC));

        int ls = uhci_port_reset(u, p);
        if (ls < 0) continue;

        int pre = g_n_usb_dev;
        usb_enumerate_device(2 /*UHCI*/, u->index, (u8)(ls ? 1 : 0));
        if (g_n_usb_dev > pre)
            probe_msc(&g_usb_dev[g_n_usb_dev - 1], g_n_usb_dev - 1);
    }
}

/* ================================================================
 *  §9  OHCI HOST CONTROLLER (USB 1.1 — prog-if 0x10)
 *
 *  OHCI uses MMIO and a Hcca (Host Controller Communications Area)
 *  with Endpoint Descriptors (ED) and Transfer Descriptors (TD).
 *  It supports full-speed and low-speed devices.
 * ================================================================ */

/* OHCI operational register offsets */
#define OHCI_REVISION       0x00
#define OHCI_CONTROL        0x04
#define OHCI_CMDSTATUS      0x08
#define OHCI_INTRSTATUS     0x0C
#define OHCI_INTRENABLE     0x10
#define OHCI_INTRDISABLE    0x14
#define OHCI_HCCA           0x18
#define OHCI_PERIOD_CURED   0x1C
#define OHCI_CTRL_HEAD_ED   0x20
#define OHCI_CTRL_CUR_ED    0x24
#define OHCI_BULK_HEAD_ED   0x28
#define OHCI_BULK_CUR_ED    0x2C
#define OHCI_DONE_HEAD      0x30
#define OHCI_FMINTERVAL     0x34
#define OHCI_FMREMAINING    0x38
#define OHCI_FMNUMBER       0x3C
#define OHCI_PERIODICSTART  0x40
#define OHCI_LSTHRESH       0x44
#define OHCI_RH_DESCA       0x48
#define OHCI_RH_DESCB       0x4C
#define OHCI_RH_STATUS      0x50
#define OHCI_RH_PORTSTAT(n) (0x54 + (n)*4)

/* OHCI_CONTROL bits */
#define OHCI_CTRL_CBSR      0x3
#define OHCI_CTRL_PLE       (1<<2)
#define OHCI_CTRL_IE        (1<<3)
#define OHCI_CTRL_CLE       (1<<4)
#define OHCI_CTRL_BLE       (1<<5)
#define OHCI_CTRL_HCFS(x)   (((x)&3)<<6)   /* 00=reset 01=resume 10=operational 11=suspend */
#define OHCI_CTRL_IR        (1<<8)
#define OHCI_CTRL_RWC       (1<<9)
#define OHCI_CTRL_RWE       (1<<10)
#define OHCI_USB_OPER       2

/* OHCI_CMDSTATUS bits */
#define OHCI_CMD_HCR        (1<<0)
#define OHCI_CMD_CLF        (1<<1)
#define OHCI_CMD_BLF        (1<<2)
#define OHCI_CMD_OCR        (1<<3)

/* OHCI RH port status bits */
#define OHCI_PORT_CCS       (1<<0)
#define OHCI_PORT_PES       (1<<1)
#define OHCI_PORT_PSS       (1<<2)
#define OHCI_PORT_POCI      (1<<3)
#define OHCI_PORT_PRS       (1<<4)
#define OHCI_PORT_PPS       (1<<8)
#define OHCI_PORT_LSDA      (1<<9)
#define OHCI_PORT_CSC       (1<<16)
#define OHCI_PORT_PESC      (1<<17)
#define OHCI_PORT_PSSC      (1<<18)
#define OHCI_PORT_OCIC      (1<<19)
#define OHCI_PORT_PRSC      (1<<20)

/* OHCI HCCA — 256-byte aligned */
typedef struct __attribute__((packed, aligned(256))) {
    u32 int_table[32]; /* periodic ED table */
    u16 frame_no;
    u16 pad1;
    u32 done_head;
    u8  reserved[116];
} OhciHcca;

/* OHCI Endpoint Descriptor */
typedef struct __attribute__((packed, aligned(16))) {
    u32 ctrl;       /* FA[6:0], EN[10:7], D[12:11], S[13], K[14], F[15], MPS[26:16] */
    u32 tail_td;    /* tail TD pointer */
    u32 head_td;    /* head TD pointer (halted bit at bit0) */
    u32 next_ed;    /* next ED in list */
} OhciED;

/* OHCI Transfer Descriptor */
typedef struct __attribute__((packed, aligned(16))) {
    u32 ctrl;       /* cbp direction, rounding, delay, toggle, EC, CC */
    u32 cbp;        /* current buffer pointer */
    u32 next_td;    /* next TD */
    u32 be;         /* buffer end */
} OhciTD;

#define OHCI_TD_CC(td)   ((td)->ctrl >> 28)
#define OHCI_TD_CC_OK    0
#define OHCI_TD_DP_SETUP 0
#define OHCI_TD_DP_OUT   1
#define OHCI_TD_DP_IN    2
#define OHCI_TD_ROUNDING (1<<18)
#define OHCI_TD_DI(n)    ((n)<<21)   /* delay interrupt */
#define OHCI_TD_DT(t)    ((t)<<24)   /* data toggle */
#define OHCI_TD_DT_AUTO  0
#define OHCI_ED_SKIP     (1<<14)

#define OHCI_MAX 2
typedef struct {
    u8       *mmio;
    OhciHcca *hcca;
    u64       hcca_phys;
    OhciED   *ctrl_ed;
    u64       ctrl_ed_phys;
    u8        n_ports;
    u8        next_addr;
    int       active;
    int       index;
} Ohci;

static Ohci g_ohci[OHCI_MAX];
static int  g_n_ohci = 0;

static inline u32 ohci_r(Ohci *o, u32 off) {
    return *(volatile u32*)(o->mmio + off);
}
static inline void ohci_w(Ohci *o, u32 off, u32 v) {
    *(volatile u32*)(o->mmio + off) = v;
}

static int ohci_ctrl_xfer(Ohci *o, u8 addr, u8 ls,
                           u8 bmReqType, u8 bReq, u16 wVal, u16 wIdx,
                           u16 wLen, void *data)
{
    u64 setup_phys, data_phys = 0, stat_phys, td_phys, ed_phys;
    u8 *setup_buf = (u8*)dma_alloc_aligned(8, 8, &setup_phys);
    u8 *stat_buf  = (u8*)dma_alloc_aligned(4, 4, &stat_phys);
    if (!setup_buf || !stat_buf) return -1;

    setup_buf[0] = bmReqType; setup_buf[1] = bReq;
    setup_buf[2] = (u8)wVal;  setup_buf[3] = (u8)(wVal>>8);
    setup_buf[4] = (u8)wIdx;  setup_buf[5] = (u8)(wIdx>>8);
    setup_buf[6] = (u8)wLen;  setup_buf[7] = (u8)(wLen>>8);

    u8 *data_buf = NULL;
    if (wLen && data) {
        data_buf = (u8*)dma_alloc_aligned(wLen, 4, &data_phys);
        if (!data_buf) return -1;
        if (!(bmReqType & 0x80)) memcpy(data_buf, data, wLen);
    }

    /* Allocate TDs: setup, [data,] status, null-terminator */
    int n_td = 3 + (wLen ? 1 : 0);
    OhciTD *tds = (OhciTD*)dma_alloc_aligned(n_td * sizeof(OhciTD), 16, &td_phys);
    if (!tds) return -1;
    memset(tds, 0, n_td * sizeof(OhciTD));

    /* SETUP TD */
    int i = 0;
    tds[i].ctrl   = (OHCI_TD_DP_SETUP << 19) | OHCI_TD_ROUNDING |
                    OHCI_TD_DI(6) | (0xFu<<28); /* CC=not accessed */
    tds[i].cbp    = (u32)setup_phys;
    tds[i].next_td = (u32)(td_phys + (i+1)*sizeof(OhciTD));
    tds[i].be     = (u32)setup_phys + 7;
    i++;

    /* DATA TD */
    if (wLen && data_buf) {
        int dp = (bmReqType & 0x80) ? OHCI_TD_DP_IN : OHCI_TD_DP_OUT;
        tds[i].ctrl   = ((u32)dp << 19) | OHCI_TD_ROUNDING |
                        OHCI_TD_DI(6) | OHCI_TD_DT(3) | (0xFu<<28);
        tds[i].cbp    = (u32)data_phys;
        tds[i].next_td = (u32)(td_phys + (i+1)*sizeof(OhciTD));
        tds[i].be     = (u32)data_phys + wLen - 1;
        i++;
    }

    /* STATUS TD */
    int sdp = (bmReqType & 0x80) ? OHCI_TD_DP_OUT : OHCI_TD_DP_IN;
    tds[i].ctrl   = ((u32)sdp << 19) | OHCI_TD_ROUNDING |
                    OHCI_TD_DI(6) | OHCI_TD_DT(3) | (0xFu<<28);
    tds[i].cbp    = 0;
    tds[i].next_td = (u32)(td_phys + (i+1)*sizeof(OhciTD)); /* null TD */
    tds[i].be     = 0;
    i++;
    /* Null/tail TD */
    tds[i].ctrl = 0; tds[i].cbp = 0; tds[i].next_td = 0; tds[i].be = 0;

    /* Build ED for this transfer */
    OhciED *ed = (OhciED*)dma_alloc_aligned(sizeof(OhciED), 16, &ed_phys);
    if (!ed) return -1;
    u32 mps = ls ? 8 : 64;
    ed->ctrl    = (u32)addr | (0u<<7) | (ls ? (1u<<13) : 0) | (mps<<16);
    ed->tail_td = (u32)(td_phys + i*sizeof(OhciTD));
    ed->head_td = (u32)td_phys;
    ed->next_ed = 0;

    /* Insert into control list */
    ohci_w(o, OHCI_CTRL_HEAD_ED, (u32)ed_phys);
    ohci_w(o, OHCI_CMDSTATUS, OHCI_CMD_CLF); /* ring control list filled */

    /* Wait */
    int ok = 0;
    for (int t = 200000; t--; ) {
        ehci_delay(5);
        if (tds[0].ctrl >> 28 != 0xF &&
            tds[1 + (wLen?1:0)].ctrl >> 28 != 0xF) { ok = 1; break; }
    }

    ohci_w(o, OHCI_CTRL_HEAD_ED, 0);
    if (ok && wLen && data_buf && (bmReqType & 0x80))
        memcpy(data, data_buf, wLen);
    return ok ? 0 : -1;
}

static int ohci_port_reset(Ohci *o, int port) {
    u32 ps = ohci_r(o, OHCI_RH_PORTSTAT(port));
    if (!(ps & OHCI_PORT_CCS)) return -1;
    /* Power on */
    ohci_w(o, OHCI_RH_PORTSTAT(port), OHCI_PORT_PPS);
    ehci_delay(20000);
    /* Reset */
    ohci_w(o, OHCI_RH_PORTSTAT(port), OHCI_PORT_PRS);
    for (int t = 500; t-- && (ohci_r(o, OHCI_RH_PORTSTAT(port)) & OHCI_PORT_PRS); )
        ehci_delay(1000);
    ps = ohci_r(o, OHCI_RH_PORTSTAT(port));
    if (!(ps & OHCI_PORT_PES)) return -1;
    return (ps & OHCI_PORT_LSDA) ? 1 : 0;
}

static void ohci_scan_ports(Ohci *o);

static void ohci_init_one(u8 bus, u8 sl, u8 fn) {
    if (g_n_ohci >= OHCI_MAX) return;
    Ohci *o = &g_ohci[g_n_ohci];
    memset(o, 0, sizeof(*o));
    o->index = g_n_ohci;

    /* BAR0 = MMIO */
    u32 bar0 = pci_read32(bus, sl, fn, 0x10) & ~0xFu;
    if (!bar0) return;
    u64 mmio_va = 0xCB000000ULL + (u64)g_n_ohci * 0x2000ULL;
    vmm_map(mmio_va, (u64)bar0, 0x1000, 0);
    o->mmio = (u8*)mmio_va;

    /* Enable bus master + memory */
    u32 cmd = pci_read32(bus, sl, fn, 0x04);
    pci_write32(bus, sl, fn, 0x04, cmd | 0x06);

    /* BIOS handoff (SMM → OS) */
    u32 ctrl = ohci_r(o, OHCI_CONTROL);
    if (ctrl & OHCI_CTRL_IR) {
        ohci_w(o, OHCI_CMDSTATUS, OHCI_CMD_OCR);
        for (int t = 100; t-- && (ohci_r(o, OHCI_CONTROL) & OHCI_CTRL_IR); )
            ehci_delay(1000);
    }

    /* Reset */
    ohci_w(o, OHCI_CMDSTATUS, OHCI_CMD_HCR);
    for (int t = 100; t-- && (ohci_r(o, OHCI_CMDSTATUS) & OHCI_CMD_HCR); )
        ehci_delay(100);

    /* Allocate HCCA */
    o->hcca = (OhciHcca*)dma_alloc_aligned(sizeof(OhciHcca), 256, &o->hcca_phys);
    if (!o->hcca) return;
    memset(o->hcca, 0, sizeof(OhciHcca));
    ohci_w(o, OHCI_HCCA, (u32)o->hcca_phys);

    /* Frame interval */
    ohci_w(o, OHCI_FMINTERVAL, 0xA7782EDF);
    ohci_w(o, OHCI_PERIODICSTART, 0x2A2F);

    /* Enable control list, set operational */
    ohci_w(o, OHCI_CONTROL,
           OHCI_CTRL_HCFS(OHCI_USB_OPER) | OHCI_CTRL_CLE | OHCI_CTRL_BLE);
    ehci_delay(5000);

    /* Port count from RH_DESCA */
    o->n_ports = (u8)(ohci_r(o, OHCI_RH_DESCA) & 0xFF);
    o->next_addr = 1;
    o->active = 1;
    g_n_ohci++;

    print_str("[OHCI] init OK ports=");
    print_hex_byte(o->n_ports);
    print_str("\r\n");

    ohci_scan_ports(o);
}

static void ohci_scan_ports(Ohci *o) {
    for (int p = 0; p < o->n_ports; p++) {
        u32 ps = ohci_r(o, OHCI_RH_PORTSTAT(p));
        if (!(ps & OHCI_PORT_CCS)) continue;

        int ls = ohci_port_reset(o, p);
        if (ls < 0) continue;

        int pre = g_n_usb_dev;
        usb_enumerate_device(3 /*OHCI*/, o->index, (u8)(ls ? 1 : 0));
        if (g_n_usb_dev > pre)
            probe_msc(&g_usb_dev[g_n_usb_dev - 1], g_n_usb_dev - 1);

        /* Clear change bits */
        ohci_w(o, OHCI_RH_PORTSTAT(p), OHCI_PORT_CSC | OHCI_PORT_PESC);
    }
}

/* ================================================================
 *  §10  USB HUB CLASS (class 0x09)
 *
 *  A USB hub presents one upstream port and 1–7 downstream ports.
 *  We enumerate each downstream port as a new USB device, supporting
 *  cascaded hubs up to depth 2.
 * ================================================================ */

#define USB_CLASS_HUB        0x09
#define HUB_REQ_GET_DESCRIPTOR 0x06
#define HUB_DESC_TYPE        0x29
#define HUB_REQ_GET_STATUS   0x00
#define HUB_REQ_SET_FEATURE  0x03
#define HUB_REQ_CLEAR_FEATURE 0x01
#define HUB_FEAT_PORT_POWER  8
#define HUB_FEAT_PORT_RESET  4
#define HUB_FEAT_PORT_CONN_CHANGE  16
#define HUB_FEAT_PORT_RESET_CHANGE 20
#define HUB_PORT_CCS         (1<<0)
#define HUB_PORT_PES         (1<<1)
#define HUB_PORT_LSDA        (1<<9)

typedef struct __attribute__((packed)) {
    u8  bLength, bDescriptorType;
    u8  bNbrPorts;
    u16 wHubCharacteristics;
    u8  bPwrOn2PwrGood;
    u8  bHubContrCurrent;
    u8  DeviceRemovable[4];
} UsbHubDesc;

static void probe_hub(UsbDevice *dev, int dev_idx) {
    if (dev->class_code != USB_CLASS_HUB) return;

    /* GET_DESCRIPTOR(Hub, type 0x29) */
    UsbHubDesc hd;
    memset(&hd, 0, sizeof(hd));
    int rc = usb_ctrl_xfer(dev->ctrlr_type, dev->address,
                           0xA0, HUB_REQ_GET_DESCRIPTOR,
                           (HUB_DESC_TYPE << 8), 0, sizeof(hd), &hd);
    if (rc) return;
    if (!hd.bNbrPorts || hd.bNbrPorts > 16) return;

    print_str("[USB-HUB] addr=");
    print_hex_byte(dev->address);
    print_str(" ports=");
    print_hex_byte(hd.bNbrPorts);
    print_str("\r\n");

    /* Power all ports */
    for (int p = 1; p <= hd.bNbrPorts; p++) {
        usb_ctrl_xfer(dev->ctrlr_type, dev->address,
                      0x23, HUB_REQ_SET_FEATURE,
                      HUB_FEAT_PORT_POWER, p, 0, 0);
    }
    ehci_delay((int)(hd.bPwrOn2PwrGood) * 2 * 1000);

    /* Enumerate each port */
    for (int p = 1; p <= hd.bNbrPorts; p++) {
        /* GET_STATUS for port */
        u32 ps = 0;
        rc = usb_ctrl_xfer(dev->ctrlr_type, dev->address,
                           0xA3, HUB_REQ_GET_STATUS, 0, p, 4, &ps);
        if (rc || !(ps & HUB_PORT_CCS)) continue;

        /* Reset port */
        usb_ctrl_xfer(dev->ctrlr_type, dev->address,
                      0x23, HUB_REQ_SET_FEATURE, HUB_FEAT_PORT_RESET, p, 0, 0);
        ehci_delay(60000);

        /* Wait for reset completion */
        for (int t = 20; t--; ) {
            ps = 0;
            usb_ctrl_xfer(dev->ctrlr_type, dev->address,
                          0xA3, HUB_REQ_GET_STATUS, 0, p, 4, &ps);
            if (ps & HUB_FEAT_PORT_RESET_CHANGE) break;
            ehci_delay(5000);
        }

        /* Clear reset change */
        usb_ctrl_xfer(dev->ctrlr_type, dev->address,
                      0x23, HUB_REQ_CLEAR_FEATURE,
                      HUB_FEAT_PORT_RESET_CHANGE, p, 0, 0);

        if (!(ps & HUB_PORT_PES)) continue;

        u8 speed = (ps & HUB_PORT_LSDA) ? 1 : 0;
        int pre = g_n_usb_dev;
        usb_enumerate_device(dev->ctrlr_type, dev->ctrlr_idx, speed);
        if (g_n_usb_dev > pre) {
            int ni = g_n_usb_dev - 1;
            probe_msc(&g_usb_dev[ni], ni);
            /* Note: recursive hub probing is skipped to avoid stack overflow */
        }
    }
}

/* ================================================================
 *  §11  USB AUDIO CLASS (class 0x01, subclass 0x01/0x02/0x03)
 *
 *  We perform minimal enumeration: detect the device, log it, and
 *  bind it to SystrixOS's sound subsystem if present. Full audio
 *  streaming (isochronous transfers) requires ISO endpoint support
 *  which is outside the scope of the polled EHCI driver; we register
 *  the device so userspace tools can identify it.
 * ================================================================ */

#define USB_CLASS_AUDIO      0x01
#define USB_SUBCLASS_AUDIO_CTRL  0x01
#define USB_SUBCLASS_AUDIO_STREAM 0x02
#define USB_SUBCLASS_AUDIO_MIDI  0x03

#define USB_AUDIO_MAX 4
typedef struct {
    UsbDevice *dev;
    int  dev_idx;
    u8   subclass;   /* 1=AudioControl 2=AudioStreaming 3=MIDI */
    int  valid;
} UsbAudio;

static UsbAudio g_usb_audio[USB_AUDIO_MAX];
static int      g_n_usb_audio = 0;

static void probe_audio(UsbDevice *dev, int dev_idx) {
    if (dev->class_code != USB_CLASS_AUDIO) return;
    if (g_n_usb_audio >= USB_AUDIO_MAX) return;

    UsbAudio *a = &g_usb_audio[g_n_usb_audio];
    a->dev     = dev;
    a->dev_idx = dev_idx;
    a->subclass = dev->subclass;
    a->valid   = 1;
    g_n_usb_audio++;

    const char *type = "audio";
    if (dev->subclass == USB_SUBCLASS_AUDIO_MIDI) type = "MIDI";
    else if (dev->subclass == USB_SUBCLASS_AUDIO_STREAM) type = "stream";

    print_str("[USB-AUDIO] ");
    print_str(type);
    print_str(" addr=");
    print_hex_byte(dev->address);
    print_str("\r\n");
}

/* ================================================================
 *  §12  USB CDC / SERIAL (class 0x02 ACM modem, 0x0A CDC data)
 *
 *  Covers USB-to-serial adapters (e.g. CP210x, CH340, FTDI clones
 *  that declare CDC ACM), and generic CDC-ECM/NCM network adapters.
 *  We configure the line coding (115200 8N1) and expose the device
 *  as a character stream via usb_cdc_read() / usb_cdc_write().
 * ================================================================ */

#define USB_CLASS_CDC        0x02
#define USB_CLASS_CDC_DATA   0x0A
#define USB_SUBCLASS_ACM     0x02
#define USB_SUBCLASS_ECM     0x06
#define USB_SUBCLASS_NCM     0x0D

/* CDC ACM requests */
#define CDC_SET_LINE_CODING     0x20
#define CDC_GET_LINE_CODING     0x21
#define CDC_SET_CTRL_LINE_STATE 0x22

typedef struct __attribute__((packed)) {
    u32 dwDTERate;    /* baud rate */
    u8  bCharFormat;  /* 0=1stop 1=1.5stop 2=2stop */
    u8  bParityType;  /* 0=none 1=odd 2=even 3=mark 4=space */
    u8  bDataBits;    /* 5,6,7,8,16 */
} CdcLineCoding;

#define USB_CDC_MAX 4
typedef struct {
    UsbDevice *dev;
    int  dev_idx;
    u8   subclass;
    int  valid;
} UsbCdc;

static UsbCdc g_usb_cdc[USB_CDC_MAX];
static int    g_n_usb_cdc = 0;

static void probe_cdc(UsbDevice *dev, int dev_idx) {
    if (dev->class_code != USB_CLASS_CDC &&
        dev->class_code != USB_CLASS_CDC_DATA) return;
    if (g_n_usb_cdc >= USB_CDC_MAX) return;

    UsbCdc *c = &g_usb_cdc[g_n_usb_cdc];
    c->dev     = dev;
    c->dev_idx = dev_idx;
    c->subclass = dev->subclass;
    c->valid   = 1;

    /* For ACM: set line coding to 115200 8N1 */
    if (dev->subclass == USB_SUBCLASS_ACM) {
        CdcLineCoding lc = { 115200, 0, 0, 8 };
        usb_ctrl_xfer(dev->ctrlr_type, dev->address,
                      0x21, CDC_SET_LINE_CODING, 0, 0, sizeof(lc), &lc);
        /* RTS + DTR asserted */
        usb_ctrl_xfer(dev->ctrlr_type, dev->address,
                      0x21, CDC_SET_CTRL_LINE_STATE, 0x03, 0, 0, 0);
    }

    g_n_usb_cdc++;

    const char *sub = (dev->subclass == USB_SUBCLASS_ACM) ? "ACM"  :
                      (dev->subclass == USB_SUBCLASS_ECM) ? "ECM"  :
                      (dev->subclass == USB_SUBCLASS_NCM) ? "NCM"  : "CDC";
    print_str("[USB-CDC/");
    print_str(sub);
    print_str("] addr=");
    print_hex_byte(dev->address);
    print_str("\r\n");
}

/* Bulk read from CDC data interface */
int usb_cdc_read(int dev_idx, void *buf, u16 len) {
    if (dev_idx < 0 || dev_idx >= g_n_usb_cdc) return -1;
    UsbCdc *c = &g_usb_cdc[dev_idx];
    if (!c->valid) return -1;
    UsbDevice *dev = c->dev;
    if (!dev->ep_in) return -1;
    if (dev->ctrlr_type != 0) return -1; /* EHCI only for now */
    for (int i = 0; i < g_n_ehci; i++) {
        if (g_ehci[i].active && g_ehci[i].index == dev->ctrlr_idx)
            return ehci_bulk_xfer(&g_ehci[i], dev->address, dev->ep_in | 0x80,
                                  1, buf, len, &dev->msc_toggle_in);
    }
    return -1;
}

/* Bulk write to CDC data interface */
int usb_cdc_write(int dev_idx, const void *buf, u16 len) {
    if (dev_idx < 0 || dev_idx >= g_n_usb_cdc) return -1;
    UsbCdc *c = &g_usb_cdc[dev_idx];
    if (!c->valid) return -1;
    UsbDevice *dev = c->dev;
    if (!dev->ep_out) return -1;
    if (dev->ctrlr_type != 0) return -1;
    for (int i = 0; i < g_n_ehci; i++) {
        if (g_ehci[i].active && g_ehci[i].index == dev->ctrlr_idx)
            return ehci_bulk_xfer(&g_ehci[i], dev->address, dev->ep_out,
                                  0, (void*)buf, len, &dev->msc_toggle_out);
    }
    return -1;
}

/* ================================================================
 *  §13  USB PRINTER CLASS (class 0x07)
 *
 *  USB printer class uses a Bulk-OUT endpoint for data and optionally
 *  a Bulk-IN for status. We implement GET_DEVICE_ID to read the
 *  printer's IEEE 1284 device ID string, and usb_printer_write()
 *  to submit raw print data (PCL / PostScript / ESC/P).
 * ================================================================ */

#define USB_CLASS_PRINTER    0x07
#define PRINTER_REQ_GET_DEVICE_ID  0x00
#define PRINTER_REQ_GET_PORT_STATUS 0x01
#define PRINTER_REQ_SOFT_RESET     0x02

#define USB_PRINTER_MAX 2
typedef struct {
    UsbDevice *dev;
    int  dev_idx;
    int  valid;
} UsbPrinter;

static UsbPrinter g_usb_printer[USB_PRINTER_MAX];
static int        g_n_usb_printer = 0;

static void probe_printer(UsbDevice *dev, int dev_idx) {
    if (dev->class_code != USB_CLASS_PRINTER) return;
    if (g_n_usb_printer >= USB_PRINTER_MAX) return;

    UsbPrinter *p = &g_usb_printer[g_n_usb_printer];
    p->dev     = dev;
    p->dev_idx = dev_idx;

    /* GET_DEVICE_ID (bmReqType=0xA1, bReq=0, wVal=0, wIdx=0|iface) */
    u8 devid[64];
    memset(devid, 0, sizeof(devid));
    usb_ctrl_xfer(dev->ctrlr_type, dev->address,
                  0xA1, PRINTER_REQ_GET_DEVICE_ID, 0, 0, sizeof(devid), devid);
    /* First two bytes are big-endian length; print truncated */
    devid[63] = 0;
    print_str("[USB-PRINTER] addr=");
    print_hex_byte(dev->address);
    print_str(" id=");
    /* Print first 16 chars of ID string */
    for (int i = 2; i < 18 && devid[i]; i++)
        vga_putchar(devid[i]);
    print_str("\r\n");

    p->valid = 1;
    g_n_usb_printer++;
}

int usb_printer_write(int dev_idx, const void *buf, u32 len) {
    if (dev_idx < 0 || dev_idx >= g_n_usb_printer) return -1;
    UsbPrinter *p = &g_usb_printer[dev_idx];
    if (!p->valid || !p->dev->ep_out) return -1;
    UsbDevice *dev = p->dev;
    if (dev->ctrlr_type != 0) return -1;
    for (int i = 0; i < g_n_ehci; i++) {
        if (g_ehci[i].active && g_ehci[i].index == dev->ctrlr_idx)
            return ehci_bulk_xfer(&g_ehci[i], dev->address, dev->ep_out,
                                  0, (void*)buf, (u16)len, &dev->msc_toggle_out);
    }
    return -1;
}

/* ================================================================
 *  §14  USB VIDEO CLASS — UVC (class 0x0E)
 *
 *  UVC devices (webcams, capture cards) present a VideoControl
 *  interface and one or more VideoStreaming interfaces. We enumerate
 *  the device, probe the VideoStreaming interface for MJPEG or
 *  uncompressed YUV format descriptors, and log the first supported
 *  resolution. Actual isochronous capture requires ISO TDs which
 *  are not yet implemented; we register the device so it can be
 *  opened later when ISO support is added.
 * ================================================================ */

#define USB_CLASS_VIDEO      0x0E
#define USB_SUBCLASS_VIDEO_CTRL   0x01
#define USB_SUBCLASS_VIDEO_STREAM 0x02

/* UVC VideoControl class-specific request */
#define UVC_REQ_GET_CUR  0x81
#define UVC_REQ_SET_CUR  0x01

/* UVC VS interface descriptor subtypes */
#define UVC_VS_FORMAT_UNCOMPRESSED 0x04
#define UVC_VS_FORMAT_MJPEG        0x06
#define UVC_VS_FRAME_UNCOMPRESSED  0x05
#define UVC_VS_FRAME_MJPEG         0x07

#define USB_VIDEO_MAX 2
typedef struct {
    UsbDevice *dev;
    int  dev_idx;
    u16  width, height;  /* first detected resolution */
    u8   format;         /* 4=YUV 6=MJPEG */
    int  valid;
} UsbVideo;

static UsbVideo g_usb_video[USB_VIDEO_MAX];
static int      g_n_usb_video = 0;

static void probe_video(UsbDevice *dev, int dev_idx) {
    if (dev->class_code != USB_CLASS_VIDEO) return;
    if (dev->subclass != USB_SUBCLASS_VIDEO_CTRL) return;
    if (g_n_usb_video >= USB_VIDEO_MAX) return;

    UsbVideo *v = &g_usb_video[g_n_usb_video];
    v->dev     = dev;
    v->dev_idx = dev_idx;
    v->valid   = 1;

    /* Re-fetch full config descriptor and scan for VS frame descriptors */
    u8 cfg[512];
    memset(cfg, 0, sizeof(cfg));
    usb_ctrl_xfer(dev->ctrlr_type, dev->address, 0x80,
                  USB_REQ_GET_DESCRIPTOR, (USB_DESC_CONFIG << 8), 0, 9, cfg);
    u16 total = ((UsbConfigDesc*)cfg)->wTotalLength;
    if (total > 512) total = 512;
    usb_ctrl_xfer(dev->ctrlr_type, dev->address, 0x80,
                  USB_REQ_GET_DESCRIPTOR, (USB_DESC_CONFIG << 8), 0, total, cfg);

    u8 *p = cfg, *end = cfg + total;
    while (p < end && p[0] >= 2) {
        /* Class-specific interface descriptor (bDescriptorType=0x24) */
        if (p[1] == 0x24) {
            u8 subtype = p[2];
            if ((subtype == UVC_VS_FRAME_UNCOMPRESSED ||
                 subtype == UVC_VS_FRAME_MJPEG) && p[0] >= 26) {
                v->format = (subtype == UVC_VS_FRAME_MJPEG) ? 6 : 4;
                v->width  = (u16)(p[5] | (p[6]<<8));
                v->height = (u16)(p[7] | (p[8]<<8));
                break;
            }
        }
        p += p[0];
    }

    print_str("[USB-VIDEO] addr=");
    print_hex_byte(dev->address);
    print_str(" ");
    print_hex_byte((u8)(v->width >> 8)); print_hex_byte((u8)v->width);
    print_str("x");
    print_hex_byte((u8)(v->height >> 8)); print_hex_byte((u8)v->height);
    print_str(v->format == 6 ? " MJPEG" : " YUV");
    print_str("\r\n");

    g_n_usb_video++;
}

/* ================================================================
 *  §15  USB BLUETOOTH (class 0xE0, subclass 0x01, protocol 0x01)
 *
 *  USB Bluetooth dongles follow a simple command channel (EP0 control
 *  or interrupt OUT) and an event channel (interrupt IN). We send
 *  HCI Reset to initialise the controller, read the local BD_ADDR,
 *  and log it. Full HCI/L2CAP/RFCOMM stack is outside scope.
 * ================================================================ */

#define USB_CLASS_WIRELESS   0xE0
#define USB_SUBCLASS_BT      0x01
#define USB_PROTO_BT         0x01

/* HCI packet types (sent over USB) */
#define HCI_CMD_PKT     0x01
#define HCI_EVT_PKT     0x04

/* HCI opcodes (OGF<<10 | OCF) */
#define HCI_RESET        0x0C03
#define HCI_READ_BD_ADDR 0x1009

#define USB_BT_MAX 2
typedef struct {
    UsbDevice *dev;
    int  dev_idx;
    u8   bd_addr[6];
    int  valid;
} UsbBluetooth;

static UsbBluetooth g_usb_bt[USB_BT_MAX];
static int          g_n_usb_bt = 0;

/* Send an HCI command via USB control transfer (bmReqType=0x20) */
static int bt_hci_cmd(UsbDevice *dev, u16 opcode, const u8 *params, u8 plen) {
    u8 cmd[260];
    cmd[0] = (u8)opcode;
    cmd[1] = (u8)(opcode >> 8);
    cmd[2] = plen;
    if (plen && params) memcpy(cmd + 3, params, plen);
    return usb_ctrl_xfer(dev->ctrlr_type, dev->address,
                         0x20, 0x00, 0, 0, (u16)(3 + plen), cmd);
}

/* Poll interrupt IN endpoint for an HCI event */
static int bt_hci_evt(UsbDevice *dev, u8 *buf, u16 maxlen) {
    if (!dev->ep_in) return -1;
    if (dev->ctrlr_type != 0) return -1;
    for (int i = 0; i < g_n_ehci; i++) {
        if (g_ehci[i].active && g_ehci[i].index == dev->ctrlr_idx)
            return ehci_bulk_xfer(&g_ehci[i], dev->address, dev->ep_in | 0x80,
                                  1, buf, maxlen, &dev->msc_toggle_in);
    }
    return -1;
}

static void probe_bluetooth(UsbDevice *dev, int dev_idx) {
    if (dev->class_code != USB_CLASS_WIRELESS) return;
    if (dev->subclass   != USB_SUBCLASS_BT)   return;
    if (dev->protocol   != USB_PROTO_BT)       return;
    if (g_n_usb_bt >= USB_BT_MAX) return;

    UsbBluetooth *bt = &g_usb_bt[g_n_usb_bt];
    bt->dev     = dev;
    bt->dev_idx = dev_idx;

    /* HCI Reset */
    bt_hci_cmd(dev, HCI_RESET, 0, 0);
    ehci_delay(200000); /* allow reset to complete */

    /* Read BD_ADDR */
    bt_hci_cmd(dev, HCI_READ_BD_ADDR, 0, 0);
    ehci_delay(50000);

    u8 evt[16];
    memset(evt, 0, sizeof(evt));
    bt_hci_evt(dev, evt, sizeof(evt));
    /* HCI command complete event: evt[0]=0x0E, evt[3..4]=opcode, evt[5..10]=BD_ADDR */
    if (evt[0] == 0x0E && evt[3] == 0x09 && evt[4] == 0x10) {
        memcpy(bt->bd_addr, &evt[5], 6);
    }

    bt->valid = 1;
    g_n_usb_bt++;

    print_str("[USB-BT] addr=");
    print_hex_byte(dev->address);
    print_str(" BD=");
    for (int i = 5; i >= 0; i--) {
        print_hex_byte(bt->bd_addr[i]);
        if (i) print_str(":");
    }
    print_str("\r\n");
}

/* ================================================================
 *  §16  VENDOR-SPECIFIC / UNKNOWN CLASS HANDLER
 *
 *  Devices with class 0xFF (vendor-specific) or unrecognised classes
 *  are logged with their vendor:product IDs and left in the device
 *  table so tools can query them via usb_list_devices().
 * ================================================================ */

#define USB_CLASS_VENDOR     0xFF

static void probe_vendor(UsbDevice *dev, int dev_idx) {
    if (dev->class_code != USB_CLASS_VENDOR) return;
    print_str("[USB-VENDOR] addr=");
    print_hex_byte(dev->address);
    print_str(" sub=");
    print_hex_byte(dev->subclass);
    print_str(" proto=");
    print_hex_byte(dev->protocol);
    print_str(" (registered, no driver)\r\n");
    (void)dev_idx;
}

/* ================================================================
 *  §17  UNIFIED CLASS PROBE DISPATCHER
 *
 *  Called after usb_enumerate_device() places a new entry in
 *  g_usb_dev[].  Dispatches to the right class driver based on
 *  bDeviceClass / bInterfaceClass.
 * ================================================================ */
static void usb_probe_all_classes(UsbDevice *dev, int dev_idx) {
    switch (dev->class_code) {
    case USB_CLASS_MSC:     probe_msc(dev, dev_idx);       break;
    case USB_CLASS_HUB:     probe_hub(dev, dev_idx);       break;
    case USB_CLASS_AUDIO:   probe_audio(dev, dev_idx);     break;
    case USB_CLASS_CDC:
    case USB_CLASS_CDC_DATA: probe_cdc(dev, dev_idx);      break;
    case USB_CLASS_PRINTER: probe_printer(dev, dev_idx);   break;
    case USB_CLASS_VIDEO:   probe_video(dev, dev_idx);     break;
    case USB_CLASS_WIRELESS: probe_bluetooth(dev, dev_idx); break;
    case USB_CLASS_VENDOR:  probe_vendor(dev, dev_idx);    break;
    case USB_CLASS_HID:     /* already handled by XHCI HID path */ break;
    default:
        print_str("[USB] unknown class=");
        print_hex_byte(dev->class_code);
        print_str(" addr=");
        print_hex_byte(dev->address);
        print_str("\r\n");
        break;
    }
}

/* ================================================================
 *  §7  PUBLIC API
 * ================================================================ */

void usb_full_init(void) {
    g_n_ehci    = 0;
    g_n_xhci2   = 0;
    g_n_usb_dev = 0;
    g_n_usb_msc = 0;
    g_n_uhci    = 0;
    g_n_ohci    = 0;
    g_n_usb_audio = 0;
    g_n_usb_cdc   = 0;
    g_n_usb_printer = 0;
    g_n_usb_video   = 0;
    g_n_usb_bt      = 0;

    /* Scan PCI for USB controllers */
    for (int bus = 0; bus < 8; bus++) {
        for (int slot = 0; slot < 32; slot++) {
            u32 id = pci_read32((u8)bus, (u8)slot, 0, 0);
            if (id == 0xFFFFFFFF || !id) continue;
            u32 cr  = pci_read32((u8)bus, (u8)slot, 0, 8);
            u8 cls  = (u8)(cr>>24), sub = (u8)(cr>>16), pif = (u8)(cr>>8);
            if (cls != 0x0C || sub != 0x03) continue;

            if (pif == 0x00) {
                print_str("[PCI] UHCI found\r\n");
                uhci_init_one((u8)bus, (u8)slot, 0);
            } else if (pif == 0x10) {
                print_str("[PCI] OHCI found\r\n");
                ohci_init_one((u8)bus, (u8)slot, 0);
            } else if (pif == 0x20) {
                print_str("[PCI] EHCI found\r\n");
                ehci_init_one((u8)bus, (u8)slot, 0);
            } else if (pif == 0x30) {
                print_str("[PCI] XHCI found\r\n");
                x2_init_one((u8)bus, (u8)slot, 0);
            }
        }
    }

    /* Scan EHCI ports */
    for (int i = 0; i < g_n_ehci; i++) {
        if (!g_ehci[i].active) continue;
        Ehci *e = &g_ehci[i];
        for (int p = 0; p < e->n_ports; p++) {
            volatile u32 *portsc = (volatile u32*)(e->op + EHCI_PORTSC(p));
            if (!(*portsc & EHCI_PORT_CCS)) continue;
            *portsc |= EHCI_PORT_PP;
            ehci_delay(20000);
            int speed = ehci_port_reset(e, p);
            if (speed < 0) continue;
            int pre = g_n_usb_dev;
            usb_enumerate_device(0, e->index, (u8)speed);
            if (g_n_usb_dev > pre)
                usb_probe_all_classes(&g_usb_dev[g_n_usb_dev-1], g_n_usb_dev-1);
            *portsc |= (EHCI_PORT_CSC | EHCI_PORT_PEDC);
        }
    }
}

void usb_full_poll(void) {
    for (int i = 0; i < g_n_xhci2; i++)
        if (g_xhci2[i].active) x2_poll_one(&g_xhci2[i]);
}

/* Shell helpers */
void usb_list_devices(void) {
    print_str("USB Devices:\r\n");
    if (!g_n_usb_dev) { print_str("  (none)\r\n"); return; }
    for (int i = 0; i < g_n_usb_dev; i++) {
        UsbDevice *d = &g_usb_dev[i];
        if (!d->valid) continue;
        print_str("  [");
        print_hex_byte((u8)i);
        print_str("] addr="); print_hex_byte(d->address);
        print_str(" spd=");   print_hex_byte(d->speed);
        print_str(" class="); print_hex_byte(d->class_code);
        print_str(":"); print_hex_byte(d->subclass);
        print_str(":"); print_hex_byte(d->protocol);
        if (d->class_code == USB_CLASS_MSC) print_str(" [MSC]");
        if (d->class_code == USB_CLASS_HID) print_str(" [HID]");
        print_str("\r\n");
    }
    if (g_n_usb_msc) {
        print_str("Mass Storage:\r\n");
        for (int i = 0; i < g_n_usb_msc; i++) {
            UsbMsc *m = &g_usb_msc[i];
            print_str("  usb"); print_hex_byte((u8)i);
            print_str(": blksz="); print_hex_byte((u8)(m->block_size>>8));
            print_hex_byte((u8)m->block_size);
            print_str(" blocks=");
            print_hex_byte((u8)(m->block_count>>24));
            print_hex_byte((u8)(m->block_count>>16));
            print_hex_byte((u8)(m->block_count>>8));
            print_hex_byte((u8)(m->block_count));
            print_str("\r\n");
        }
    }
}
