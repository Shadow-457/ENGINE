/* ================================================================
 *  Systrix OS — kernel/e1000.c
 *  Real network drivers: Intel e1000 (82540EM) + Realtek RTL8139.
 *
 *  Auto-detection order:
 *    1. Intel e1000   (PCI 8086:100E / 8086:100F / 8086:10D3)
 *    2. RTL8139       (PCI 10EC:8139)
 *
 *  Both drivers share the same abstraction:
 *    nic_init()        — scan PCI, init whichever NIC found
 *    nic_send(buf,len) — transmit one Ethernet frame
 *    nic_poll()        — drain RX ring, call net_rx_deliver(buf,len)
 *    nic_mac[6]        — own MAC address (filled by init)
 *
 *  TX/RX for e1000:  16-descriptor rings, DMA into static buffers.
 *  TX/RX for RTL8139: 4-descriptor TX ring, single 64KB RX ring buffer.
 *
 *  All memory is identity-mapped (virt == phys in Systrix OS).
 * ================================================================ */
#include "../include/kernel.h"

/* ── Exported state ──────────────────────────────────────────── */
u8  nic_mac[6]  = {0};
int nic_ready   = 0;

/* ── PCI helpers (use the kernel PCI driver) ─────────────────── */
#define E1000_VENDOR  0x8086
#define E1000_DEV_A   0x100E   /* 82540EM — QEMU default          */
#define E1000_DEV_B   0x100F   /* 82545EM                         */
#define E1000_DEV_C   0x10D3   /* 82574L                          */
#define RTL_VENDOR    0x10EC
#define RTL_DEV       0x8139

/* ════════════════════════════════════════════════════════════════
 *  INTEL e1000 DRIVER
 * ════════════════════════════════════════════════════════════════ */

#define E1000_CTRL    0x0000
#define E1000_STATUS  0x0008
#define E1000_EECD    0x0010
#define E1000_EERD    0x0014
#define E1000_ICR     0x00C0
#define E1000_IMS     0x00D0
#define E1000_IMC     0x00D8
#define E1000_RCTL    0x0100
#define E1000_TCTL    0x0400
#define E1000_TIPG    0x0410
#define E1000_RDBAL   0x2800
#define E1000_RDBAH   0x2804
#define E1000_RDLEN   0x2808
#define E1000_RDH     0x2810
#define E1000_RDT     0x2818
#define E1000_TDBAL   0x3800
#define E1000_TDBAH   0x3804
#define E1000_TDLEN   0x3808
#define E1000_TDH     0x3810
#define E1000_TDT     0x3818
#define E1000_RAL0    0x5400
#define E1000_RAH0    0x5404
#define E1000_MTA     0x5200   /* 128 × u32 multicast table */

/* CTRL bits */
#define CTRL_SLU    (1u<<6)    /* Set Link Up */
#define CTRL_ASDE   (1u<<5)    /* Auto-Speed Detection Enable */
#define CTRL_RST    (1u<<26)   /* Device Reset */
#define CTRL_PHY_RST (1u<<31)

/* RCTL bits */
#define RCTL_EN     (1u<<1)
#define RCTL_SBP    (1u<<2)
#define RCTL_UPE    (1u<<3)    /* unicast promisc */
#define RCTL_MPE    (1u<<4)    /* multicast promisc */
#define RCTL_BAM    (1u<<15)   /* broadcast accept */
#define RCTL_SECRC  (1u<<26)   /* strip CRC */
#define RCTL_BSIZE_2048  0     /* default buffer size */

/* TCTL bits */
#define TCTL_EN     (1u<<1)
#define TCTL_PSP    (1u<<3)    /* pad short packets */
#define TCTL_CT_SHIFT  4
#define TCTL_COLD_SHIFT 12

/* TX descriptor command/status */
#define TDESC_CMD_EOP   (1u<<0)
#define TDESC_CMD_IFCS  (1u<<1)
#define TDESC_CMD_RS    (1u<<3)
#define TDESC_STA_DD    (1u<<0)

/* RX descriptor status */
#define RDESC_STA_DD    (1u<<0)
#define RDESC_STA_EOP   (1u<<1)

#define E1000_TX_COUNT  16
#define E1000_RX_COUNT  16
#define E1000_PKT_SIZE  2048

typedef struct __attribute__((packed)) {
    u64 addr;
    u16 length;
    u16 checksum;
    u8  status;
    u8  errors;
    u16 special;
} E1000RXDesc;

typedef struct __attribute__((packed)) {
    u64 addr;
    u16 length;
    u8  cso;
    u8  cmd;
    u8  status;
    u8  css;
    u16 special;
} E1000TXDesc;

static volatile u32   *g_e1000      = 0;
static E1000TXDesc     g_e1000_tx[E1000_TX_COUNT] __attribute__((aligned(16)));
static E1000RXDesc     g_e1000_rx[E1000_RX_COUNT] __attribute__((aligned(16)));
static u8 __attribute__((aligned(16))) g_e1000_tx_buf[E1000_TX_COUNT][E1000_PKT_SIZE];
static u8 __attribute__((aligned(16))) g_e1000_rx_buf[E1000_RX_COUNT][E1000_PKT_SIZE];
static u32 g_e1000_tx_tail = 0;
static u32 g_e1000_rx_tail = 0;

static inline u32 e1k_r(u32 reg) {
    return *(volatile u32*)((u8*)g_e1000 + reg);
}
static inline void e1k_w(u32 reg, u32 val) {
    *(volatile u32*)((u8*)g_e1000 + reg) = val;
}

static u16 e1k_eeprom_read(u8 addr) {
    e1k_w(E1000_EERD, (1u) | ((u32)addr << 2));
    u32 v; int tries = 100000;
    do { v = e1k_r(E1000_EERD); } while (!(v & (1u<<4)) && --tries);
    return (u16)(v >> 16);
}

static void e1k_delay(u32 n) {
    for (volatile u32 i = 0; i < n; i++) __asm__ volatile("nop");
}

static int e1000_init(u64 mmio_phys) {
    /* Map MMIO (128KB) — identity map in Systrix OS */
    for (u64 off = 0; off < 0x20000; off += PAGE_SIZE)
        vmm_map(read_cr3(), mmio_phys + off, mmio_phys + off,
                PTE_PRESENT | PTE_WRITE | PTE_NX);

    g_e1000 = (volatile u32*)(usize)mmio_phys;

    /* 1. Read MAC from RAL0/RAH0 before reset (QEMU pre-loads it) */
    {
        u32 ral = e1k_r(E1000_RAL0);
        u32 rah = e1k_r(E1000_RAH0);
        nic_mac[0] = (u8)(ral);       nic_mac[1] = (u8)(ral >>  8);
        nic_mac[2] = (u8)(ral >> 16); nic_mac[3] = (u8)(ral >> 24);
        nic_mac[4] = (u8)(rah);       nic_mac[5] = (u8)(rah >>  8);
    }
    /* Fall back to EEPROM if RAL was zero */
    if (!nic_mac[0] && !nic_mac[1] && !nic_mac[2]) {
        u16 w0 = e1k_eeprom_read(0);
        u16 w1 = e1k_eeprom_read(1);
        u16 w2 = e1k_eeprom_read(2);
        nic_mac[0]=(u8)w0; nic_mac[1]=(u8)(w0>>8);
        nic_mac[2]=(u8)w1; nic_mac[3]=(u8)(w1>>8);
        nic_mac[4]=(u8)w2; nic_mac[5]=(u8)(w2>>8);
    }
    if (!nic_mac[0] && !nic_mac[1] && !nic_mac[2] &&
        !nic_mac[3] && !nic_mac[4] && !nic_mac[5]) {
        kprintf("[e1000] no MAC — NIC absent\r\n");
        return 0;
    }

    /* 2. Reset */
    e1k_w(E1000_CTRL, e1k_r(E1000_CTRL) | CTRL_RST);
    e1k_delay(200000);
    /* 3. Link up */
    e1k_w(E1000_CTRL, (e1k_r(E1000_CTRL) | CTRL_SLU | CTRL_ASDE) & ~CTRL_RST);
    e1k_delay(10000);
    /* 4. Mask all interrupts */
    e1k_w(E1000_IMC, 0xFFFFFFFF);
    e1k_r(E1000_ICR);
    /* 5. Restore MAC */
    e1k_w(E1000_RAL0, (u32)nic_mac[0] | ((u32)nic_mac[1]<<8) |
                      ((u32)nic_mac[2]<<16) | ((u32)nic_mac[3]<<24));
    e1k_w(E1000_RAH0, (u32)nic_mac[4] | ((u32)nic_mac[5]<<8) | (1u<<31));
    /* Zero multicast table */
    for (int i = 0; i < 128; i++) e1k_w(E1000_MTA + i*4, 0);

    /* 6. RX ring */
    for (int i = 0; i < E1000_RX_COUNT; i++) {
        g_e1000_rx[i].addr   = (u64)(usize)g_e1000_rx_buf[i];
        g_e1000_rx[i].status = 0;
    }
    u64 rx_phys = (u64)(usize)g_e1000_rx;
    e1k_w(E1000_RDBAL, (u32)(rx_phys));
    e1k_w(E1000_RDBAH, (u32)(rx_phys >> 32));
    e1k_w(E1000_RDLEN, (u32)(E1000_RX_COUNT * sizeof(E1000RXDesc)));
    e1k_w(E1000_RDH,   0);
    e1k_w(E1000_RDT,   E1000_RX_COUNT - 1);
    g_e1000_rx_tail = E1000_RX_COUNT - 1;
    e1k_w(E1000_RCTL,  RCTL_EN | RCTL_BAM | RCTL_SECRC | RCTL_MPE);

    /* 7. TX ring */
    for (int i = 0; i < E1000_TX_COUNT; i++) {
        g_e1000_tx[i].addr   = (u64)(usize)g_e1000_tx_buf[i];
        g_e1000_tx[i].status = TDESC_STA_DD;
    }
    u64 tx_phys = (u64)(usize)g_e1000_tx;
    e1k_w(E1000_TDBAL, (u32)(tx_phys));
    e1k_w(E1000_TDBAH, (u32)(tx_phys >> 32));
    e1k_w(E1000_TDLEN, (u32)(E1000_TX_COUNT * sizeof(E1000TXDesc)));
    e1k_w(E1000_TDH,   0);
    e1k_w(E1000_TDT,   0);
    g_e1000_tx_tail = 0;
    e1k_w(E1000_TCTL,
        TCTL_EN | TCTL_PSP |
        (0x0F << TCTL_CT_SHIFT) |
        (0x40 << TCTL_COLD_SHIFT));
    /* Inter-packet gap */
    e1k_w(E1000_TIPG, 0x00702008u);

    return 1;
}

static void e1000_send(const void *buf, u16 len) {
    if (!g_e1000) return;
    /* Wait for descriptor free */
    int tries = 100000;
    while (!(g_e1000_tx[g_e1000_tx_tail].status & TDESC_STA_DD) && --tries)
        e1k_delay(10);
    if (!tries) return;
    memcpy(g_e1000_tx_buf[g_e1000_tx_tail], buf, len);
    g_e1000_tx[g_e1000_tx_tail].length = len;
    g_e1000_tx[g_e1000_tx_tail].cmd    = TDESC_CMD_EOP|TDESC_CMD_IFCS|TDESC_CMD_RS;
    g_e1000_tx[g_e1000_tx_tail].status = 0;
    u32 old = g_e1000_tx_tail;
    g_e1000_tx_tail = (g_e1000_tx_tail + 1) % E1000_TX_COUNT;
    e1k_w(E1000_TDT, g_e1000_tx_tail);
    /* Wait for completion */
    tries = 100000;
    while (!(g_e1000_tx[old].status & TDESC_STA_DD) && --tries)
        e1k_delay(10);
}

static void e1000_poll(void) {
    for (;;) {
        u32 next = (g_e1000_rx_tail + 1) % E1000_RX_COUNT;
        if (!(g_e1000_rx[next].status & RDESC_STA_DD)) break;
        u16 len = g_e1000_rx[next].length;
        if (len && (g_e1000_rx[next].status & RDESC_STA_EOP))
            net_rx_deliver(g_e1000_rx_buf[next], len);
        g_e1000_rx[next].status = 0;
        g_e1000_rx_tail = next;
        e1k_w(E1000_RDT, g_e1000_rx_tail);
    }
}

/* ════════════════════════════════════════════════════════════════
 *  REALTEK RTL8139 DRIVER
 *  Fallback for systems/VMs that expose an RTL8139 instead of e1000.
 * ════════════════════════════════════════════════════════════════ */

/* RTL8139 I/O register offsets */
#define RTL_MAC0        0x00
#define RTL_MAR0        0x08   /* multicast filter */
#define RTL_TXSTATUS0   0x10   /* 4 TX status regs (0x10,0x14,0x18,0x1C) */
#define RTL_TXADDR0     0x20   /* 4 TX address regs */
#define RTL_RXBUF       0x30   /* RX buffer start */
#define RTL_CMD         0x37
#define RTL_CAPR        0x38   /* current address of packet read */
#define RTL_CBR         0x3A   /* current buffer address */
#define RTL_IMR         0x3C
#define RTL_ISR         0x3E
#define RTL_TCR         0x40   /* TX config */
#define RTL_RCR         0x44   /* RX config */
#define RTL_TSAD        0x60   /* TX status of all descriptors */
#define RTL_CONFIG1     0x52

/* CMD bits */
#define RTL_CMD_RST     (1u<<4)
#define RTL_CMD_RE      (1u<<3)
#define RTL_CMD_TE      (1u<<2)

/* RCR bits */
#define RTL_RCR_AAP     (1u<<0)  /* all physical */
#define RTL_RCR_APM     (1u<<1)  /* physical match */
#define RTL_RCR_AM      (1u<<2)  /* multicast */
#define RTL_RCR_AB      (1u<<3)  /* broadcast */
#define RTL_RCR_WRAP    (1u<<7)  /* WRAP bit — ring buffer wrap */
#define RTL_RCR_RBLEN_64K (3u<<11)
#define RTL_RCR_MXDMA_UNLIMITED (7u<<8)
#define RTL_RCR_RXFTH_NONE (7u<<13)

/* TX status bits */
#define RTL_TXS_TOK     (1u<<15)
#define RTL_TXS_OWN     (1u<<13)

/* RX buffer size: 64KB + 16 bytes wrap padding */
#define RTL_RX_BUF_SIZE (64*1024 + 16)
#define RTL_TX_BUF_SIZE 1536
#define RTL_TX_COUNT    4

static u16  g_rtl_io   = 0;
static u8   g_rtl_tx_buf[RTL_TX_COUNT][RTL_TX_BUF_SIZE] __attribute__((aligned(4)));
static u8   g_rtl_rx_buf[RTL_RX_BUF_SIZE] __attribute__((aligned(4)));
static int  g_rtl_tx_idx = 0;
static u32  g_rtl_rx_ptr = 0;   /* u32 so the >= 64KB comparison is valid */

static inline void rtl_outb(u16 off, u8  v)  { outb((u16)(g_rtl_io+off), v);  }
static inline void rtl_outw(u16 off, u16 v)  { outw((u16)(g_rtl_io+off), v);  }
static inline void rtl_outl(u16 off, u32 v)  { outl((u16)(g_rtl_io+off), v);  }
static inline u8   rtl_inb (u16 off)         { return inb ((u16)(g_rtl_io+off)); }
static inline u8   rtl_inw (u16 off)         { return inw ((u16)(g_rtl_io+off)); }
static inline u32  rtl_inl (u16 off)         { return inl ((u16)(g_rtl_io+off)); }

static int rtl8139_init(u16 io_base) {
    g_rtl_io = io_base;

    /* Power on */
    rtl_outb(RTL_CONFIG1, 0x00);
    for (volatile int d = 0; d < 10000; d++);

    /* Software reset */
    rtl_outb(RTL_CMD, RTL_CMD_RST);
    for (int t = 10000; t-- && (rtl_inb(RTL_CMD) & RTL_CMD_RST);)
        for (volatile int d = 0; d < 100; d++);

    /* Read MAC */
    u32 mac_lo = rtl_inl(RTL_MAC0);
    u16 mac_hi = rtl_inw(RTL_MAC0 + 4);
    nic_mac[0] = (u8)(mac_lo);       nic_mac[1] = (u8)(mac_lo >>  8);
    nic_mac[2] = (u8)(mac_lo >> 16); nic_mac[3] = (u8)(mac_lo >> 24);
    nic_mac[4] = (u8)(mac_hi);       nic_mac[5] = (u8)(mac_hi >> 8);

    /* Set RX buffer address */
    rtl_outl(RTL_RXBUF, (u32)(usize)g_rtl_rx_buf);

    /* Set TX buffer addresses */
    for (int i = 0; i < RTL_TX_COUNT; i++)
        rtl_outl((u16)(RTL_TXADDR0 + i*4), (u32)(usize)g_rtl_tx_buf[i]);

    /* Mask all interrupts (poll mode) */
    rtl_outw(RTL_IMR, 0x0000);
    rtl_outw(RTL_ISR, 0xFFFF);

    /* RX config: accept broadcast+unicast+multicast, 64KB ring, no wrap issues */
    rtl_outl(RTL_RCR,
        RTL_RCR_AB | RTL_RCR_APM | RTL_RCR_AM |
        RTL_RCR_RBLEN_64K | RTL_RCR_MXDMA_UNLIMITED | RTL_RCR_RXFTH_NONE |
        RTL_RCR_WRAP);

    /* TX config: max DMA burst, IFG = normal */
    rtl_outl(RTL_TCR, (6u<<8) | (3u<<24));

    /* Enable RX + TX */
    rtl_outb(RTL_CMD, RTL_CMD_RE | RTL_CMD_TE);

    g_rtl_rx_ptr = 0;
    g_rtl_tx_idx = 0;
    return 1;
}

static void rtl8139_send(const void *buf, u16 len) {
    int slot = g_rtl_tx_idx;
    /* Wait for slot to be free */
    for (int t = 100000; t--; ) {
        u32 s = rtl_inl((u16)(RTL_TXSTATUS0 + slot*4));
        if (s & RTL_TXS_OWN) break;
        for (volatile int d = 0; d < 10; d++);
    }
    if (len > RTL_TX_BUF_SIZE) len = RTL_TX_BUF_SIZE;
    memcpy(g_rtl_tx_buf[slot], buf, len);
    /* Set length + clear OWN to hand to NIC */
    rtl_outl((u16)(RTL_TXSTATUS0 + slot*4), (u32)len & 0x1FFF);
    /* Wait for TOK */
    for (int t = 100000; t--; ) {
        if (rtl_inl((u16)(RTL_TXSTATUS0 + slot*4)) & RTL_TXS_TOK) break;
        for (volatile int d = 0; d < 10; d++);
    }
    g_rtl_tx_idx = (g_rtl_tx_idx + 1) % RTL_TX_COUNT;
}

static void rtl8139_poll(void) {
    for (;;) {
        /* ISR bit 0 = ROK (receive OK) */
        u16 isr = rtl_inw(RTL_ISR);
        if (!(isr & 0x01)) break;
        rtl_outw(RTL_ISR, 0x01);

        /* Current buffer write pointer */
        u16 cbr = rtl_inw(RTL_CBR);
        while (g_rtl_rx_ptr != cbr) {
            /* Packet header is at CAPR+0x10 offset */
            u8 *hdr = g_rtl_rx_buf + g_rtl_rx_ptr;
            u8  rx_status = hdr[0];
            u16 rx_len    = (u16)(hdr[2] | ((u16)hdr[3] << 8));

            if (!(rx_status & 0x01)) break; /* ROK bit must be set */
            if (rx_len < 4 || rx_len > 1522 + 4) goto next;

            /* Deliver (subtract 4-byte CRC that RTL appends) */
            net_rx_deliver(hdr + 4, (u16)(rx_len - 4));

        next:
            /* Advance read pointer (16-byte aligned, +4 for header) */
            g_rtl_rx_ptr = (g_rtl_rx_ptr + rx_len + 4 + 3) & ~3u;
            /* Wrap within 64KB */
            if (g_rtl_rx_ptr >= 64*1024) g_rtl_rx_ptr -= 64*1024;
            /* Update CAPR (subtract 0x10 as per datasheet quirk) */
            rtl_outw(RTL_CAPR, (u16)(g_rtl_rx_ptr - 0x10));
        }
    }
}

/* ════════════════════════════════════════════════════════════════
 *  NIC ABSTRACTION LAYER
 * ════════════════════════════════════════════════════════════════ */
typedef enum { NIC_NONE, NIC_E1000, NIC_RTL8139 } NicType;
static NicType g_nic_type = NIC_NONE;

void nic_init(void) {
    g_nic_type = NIC_NONE;
    nic_ready  = 0;

    /* ── Try e1000 first ── */
    void *pdev = 0;
    u64   bar  = 0;

    if ((pdev = pci_find_device(E1000_VENDOR, E1000_DEV_A)) ||
        (pdev = pci_find_device(E1000_VENDOR, E1000_DEV_B)) ||
        (pdev = pci_find_device(E1000_VENDOR, E1000_DEV_C))) {
        pci_enable_device(pdev);
        pci_enable_bus_master(pdev);
        bar = pci_bar_base(pdev, 0);
        if (bar && e1000_init(bar)) {
            g_nic_type = NIC_E1000;
            nic_ready  = 1;
            kprintf("[e1000] MAC=");
            for (int i = 0; i < 6; i++) {
                print_hex_byte(nic_mac[i]);
                if (i < 5) kprintf(":");
            }
            kprintf("\r\n");
            return;
        }
    }

    /* ── Try RTL8139 ── */
    if ((pdev = pci_find_device(RTL_VENDOR, RTL_DEV))) {
        pci_enable_device(pdev);
        pci_enable_bus_master(pdev);
        u16 io = pci_bar_io(pdev, 0);
        if (io && rtl8139_init(io)) {
            g_nic_type = NIC_RTL8139;
            nic_ready  = 1;
            kprintf("[rtl8139] MAC=");
            for (int i = 0; i < 6; i++) {
                print_hex_byte(nic_mac[i]);
                if (i < 5) kprintf(":");
            }
            kprintf("\r\n");
            return;
        }
    }

    kprintf("[nic] no supported NIC found\r\n");
}

/* Send one raw Ethernet frame */
void nic_send(const u8 *buf, usize len) {
    if (!nic_ready || len > 1514) return;
    switch (g_nic_type) {
    case NIC_E1000:  e1000_send(buf, (u16)len); break;
    case NIC_RTL8139: rtl8139_send(buf, (u16)len); break;
    default: break;
    }
}

/* Poll RX ring — calls net_rx_deliver(buf, len) for each packet */
void nic_poll(void) {
    if (!nic_ready) return;
    switch (g_nic_type) {
    case NIC_E1000:  e1000_poll();  break;
    case NIC_RTL8139: rtl8139_poll(); break;
    default: break;
    }
}
