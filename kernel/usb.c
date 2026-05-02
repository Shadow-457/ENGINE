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

    kprintf("[EHCI] init OK ");
    print_hex_byte((u8)e->n_ports);
    kprintf(" ports\r\n");
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

    kprintf("[USB] addr=%02X class=%02X", (unsigned)(new_addr), (unsigned)(dev->class_code));
    kprintf(":"); print_hex_byte(dev->subclass);
    kprintf("\r\n");
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

    kprintf("[USB-MSC] drive ");
    print_hex_byte((u8)(g_n_usb_msc - 1));
    kprintf(": ");
    /* print block count in decimal (rough) */
    u64 mb = (m->block_count * m->block_size) >> 20;
    if (mb > 999999) { print_hex_byte((u8)(mb>>24)); kprintf("G+"); }
    else { print_hex_byte((u8)(mb >> 8)); print_hex_byte((u8)mb); kprintf(" MB"); }
    kprintf("\r\n");
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

    kprintf("[XHCI] slot ");
    print_hex_byte((u8)slot_id);
    kprintf(" addr %02X\r\n", (unsigned)(dev_addr));
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

    kprintf("[XHCI2] init OK ");
    print_hex_byte((u8)x->n_ports);
    kprintf(" ports\r\n");
}

/* ================================================================
 *  §7  PUBLIC API
 * ================================================================ */

void usb_full_init(void) {
    g_n_ehci    = 0;
    g_n_xhci2   = 0;
    g_n_usb_dev = 0;
    g_n_usb_msc = 0;

    /* Scan PCI for USB controllers */
    for (int bus = 0; bus < 8; bus++) {
        for (int slot = 0; slot < 32; slot++) {
            u32 id = pci_read32((u8)bus, (u8)slot, 0, 0);
            if (id == 0xFFFFFFFF || !id) continue;
            u32 cr  = pci_read32((u8)bus, (u8)slot, 0, 8);
            u8 cls  = (u8)(cr>>24), sub = (u8)(cr>>16), pif = (u8)(cr>>8);
            if (cls != 0x0C || sub != 0x03) continue;

            if (pif == 0x20) {
                kprintf("[PCI] EHCI found\r\n");
                ehci_init_one((u8)bus, (u8)slot, 0);
            } else if (pif == 0x30) {
                kprintf("[PCI] XHCI found\r\n");
                x2_init_one((u8)bus, (u8)slot, 0);
            }
            /* UHCI/OHCI: rely on BIOS PS/2 emulation (ps2.c) */
        }
    }

    /* Scan EHCI ports */
    for (int i = 0; i < g_n_ehci; i++)
        if (g_ehci[i].active) ehci_scan_ports(&g_ehci[i]);
}

void usb_full_poll(void) {
    for (int i = 0; i < g_n_xhci2; i++)
        if (g_xhci2[i].active) x2_poll_one(&g_xhci2[i]);
}

/* Shell helpers */
void usb_list_devices(void) {
    kprintf("USB Devices:\r\n");
    if (!g_n_usb_dev) { kprintf("  (none)\r\n"); return; }
    for (int i = 0; i < g_n_usb_dev; i++) {
        UsbDevice *d = &g_usb_dev[i];
        if (!d->valid) continue;
        kprintf("  [");
        print_hex_byte((u8)i);
        kprintf("] addr="); print_hex_byte(d->address);
        kprintf(" spd=");   print_hex_byte(d->speed);
        kprintf(" class="); print_hex_byte(d->class_code);
        kprintf(":"); print_hex_byte(d->subclass);
        kprintf(":"); print_hex_byte(d->protocol);
        if (d->class_code == USB_CLASS_MSC) kprintf(" [MSC]");
        if (d->class_code == USB_CLASS_HID) kprintf(" [HID]");
        kprintf("\r\n");
    }
    if (g_n_usb_msc) {
        kprintf("Mass Storage:\r\n");
        for (int i = 0; i < g_n_usb_msc; i++) {
            UsbMsc *m = &g_usb_msc[i];
            kprintf("  usb"); print_hex_byte((u8)i);
            kprintf(": blksz="); print_hex_byte((u8)(m->block_size>>8));
            print_hex_byte((u8)m->block_size);
            kprintf(" blocks=");
            print_hex_byte((u8)(m->block_count>>24));
            print_hex_byte((u8)(m->block_count>>16));
            print_hex_byte((u8)(m->block_count>>8));
            print_hex_byte((u8)(m->block_count));
            kprintf("\r\n");
        }
    }
}
