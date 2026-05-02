/* ================================================================
 *  Systrix OS — kernel/net.c
 *
 *  Full network stack:
 *    Layer 1: e1000 (Intel 8254x) driver — PCI, MMIO, TX/RX rings
 *    Layer 2: Ethernet II framing + ARP
 *    Layer 3: IPv4 + ICMP (ping)
 *    Layer 3: UDP
 *    Layer 3: DHCP (over UDP, to get an IP automatically)
 *    Layer 4: TCP (enough for HTTP GET)
 *    Layer 5: HTTP GET — download a file from a server
 *
 *  QEMU default NIC: e1000, PCI 00:03.0, MMIO 0xfeb80000,
 *  I/O port 0xc000, IRQ 11.
 *  We use MMIO (memory-mapped registers) exclusively.
 * ================================================================ */
#include "../include/kernel.h"

/* ── compile-time config ─────────────────────────────────────── */
#define PKT_SIZE      2048

/* ─────────────────────────────────────────────────────────────
 *  Ethernet + protocol headers (must come before any use)
 * ───────────────────────────────────────────────────────────── */
#define ETH_ARP  0x0806
#define ETH_IP   0x0800

typedef struct { u8 dst[6]; u8 src[6]; u16 type; } __attribute__((packed)) EthHdr;
typedef struct { u16 htype,ptype; u8 hlen,plen; u16 oper; u8 sha[6]; u32 spa; u8 tha[6]; u32 tpa; } __attribute__((packed)) ArpPkt;
typedef struct { u8 ihl_ver,dscp; u16 total_len,id,frag_off; u8 ttl,proto; u16 checksum; u32 src_ip,dst_ip; } __attribute__((packed)) IpHdr;
typedef struct { u8 type,code; u16 checksum,id,seq; } __attribute__((packed)) IcmpHdr;
typedef struct { u16 src_port,dst_port,length,checksum; } __attribute__((packed)) UdpHdr;
typedef struct { u16 src_port,dst_port; u32 seq,ack; u8 data_off,flags; u16 window,checksum,urgent; } __attribute__((packed)) TcpHdr;
#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10

static inline u16 htons(u16 v){return(u16)((v>>8)|(v<<8));}
static inline u32 htonl(u32 v){return((v>>24)&0xFF)|((v>>8)&0xFF00)|((v<<8)&0xFF0000)|((v<<24)&0xFF000000);}
#define ntohs htons
#define ntohl htonl

/* forward declarations */
static u16  ip_checksum(const void *data, usize len);
static void arp_table_add(u32 ip, const u8 *mac);
static void arp_send_reply(u32 dst_ip, const u8 *dst_mac);
static void handle_tcp(IpHdr *ip, TcpHdr *tcp_pkt, u16 ip_total);
void net_print_ip(u32 ip);
typedef struct {
    u8  op, htype, hlen, hops;
    u32 xid;
    u16 secs, flags;
    u32 ciaddr, yiaddr, siaddr, giaddr;
    u8  chaddr[16];
    u8  sname[64];
    u8  file[128];
    u8  options[64];
} __attribute__((packed)) DhcpPkt;
static void handle_dhcp(UdpHdr *udp, DhcpPkt *dhcp);
static void dns_handle_reply(UdpHdr *udp, u8 *payload, u16 plen);

/* ── public state ────────────────────────────────────────────── */
u8  net_mac[6];
u32 net_ip    = 0;
int net_ready = 0;
static int icmp_got_reply = 0;

/* ── ARP table (tiny, 16 entries) ───────────────────────────── */
#define ARP_TABLE_SIZE 16
static struct { u32 ip; u8 mac[6]; } arp_table[ARP_TABLE_SIZE];
static int arp_count = 0;

/* ── busy-wait used by protocol timeouts ─────────────────────── */
static void net_delay(u32 n) {
    while (n--) {
        __asm__ volatile("pause");
        if ((n & 127) == 0) watchdog_pet();
    }
}

/* ─────────────────────────────────────────────────────────────
 *  e1000 driver initialisation
 * ───────────────────────────────────────────────────────────── */
void net_init(void) {
    /* Delegate hardware init to the NIC abstraction layer (e1000.c).
     * It auto-detects e1000 or RTL8139 via PCI, sets up TX/RX rings,
     * and fills nic_mac[]. We mirror the MAC into net_mac[] so all the
     * protocol code above (ARP, DHCP, TCP) continues to work unchanged. */
    nic_init();
    if (!nic_ready) return;

    memcpy(net_mac, nic_mac, 6);
    net_ready = 1;
}

void net_send_frame(const u8 *data, usize len) {
    if (len > PKT_SIZE) return;
    /* Route through NIC abstraction layer (e1000.c / rtl8139) */
    nic_send(data, len);
}

/* ─────────────────────────────────────────────────────────────
 *  net_rx_deliver — called by nic_poll() in e1000.c for each
 *  received frame.  Runs the same protocol dispatch that was
 *  previously inlined in net_poll().
 * ───────────────────────────────────────────────────────────── */
void net_rx_deliver(const u8 *buf, u16 len) {
    if (len < sizeof(EthHdr)) return;
    EthHdr *eth = (EthHdr*)buf;
    u16 etype = ntohs(eth->type);

    if (etype == ETH_ARP && len >= (u16)(sizeof(EthHdr)+sizeof(ArpPkt))) {
        ArpPkt *arp = (ArpPkt*)(buf + sizeof(EthHdr));
        arp_table_add(arp->spa, arp->sha);
        if (ntohs(arp->oper)==1 && arp->tpa==net_ip)
            arp_send_reply(arp->spa, arp->sha);
    } else if (etype == ETH_IP && len >= (u16)(sizeof(EthHdr)+sizeof(IpHdr))) {
        IpHdr *ip  = (IpHdr*)(buf + sizeof(EthHdr));
        u16 ip_total = ntohs(ip->total_len);
        u8  proto    = ip->proto;
        if (proto == 1) {
            IcmpHdr *icmp = (IcmpHdr*)((u8*)ip + sizeof(IpHdr));
            if (icmp->type == 0) icmp_got_reply = 1;
            if (icmp->type == 8) {
                icmp->type = 0;
                icmp->checksum = 0;
                icmp->checksum = ip_checksum(icmp,
                    (usize)(ip_total - sizeof(IpHdr)));
                u32 tmp = ip->src_ip; ip->src_ip = ip->dst_ip; ip->dst_ip = tmp;
                ip->checksum = 0;
                ip->checksum = ip_checksum(ip, sizeof(IpHdr));
                /* Send reply via NIC layer */
                nic_send((const u8*)eth, len);
            }
        } else if (proto == 17) {
            UdpHdr *udp = (UdpHdr*)((u8*)ip + sizeof(IpHdr));
            u16 dport = ntohs(udp->dst_port);
            u16 sport = ntohs(udp->src_port);
            if (dport == 68) handle_dhcp(udp, (DhcpPkt*)((u8*)udp+sizeof(UdpHdr)));
            if (sport == 53) {
                u8  *pay     = (u8*)udp + sizeof(UdpHdr);
                u16  pay_len = (u16)(ntohs(udp->length) - sizeof(UdpHdr));
                dns_handle_reply(udp, pay, pay_len);
            }
        } else if (proto == 6) {
            TcpHdr *tcp = (TcpHdr*)((u8*)ip + sizeof(IpHdr));
            handle_tcp(ip, tcp, ip_total);
        }
    }
}
static u8 broadcast_mac[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

/* ─────────────────────────────────────────────────────────────
 *  IP checksum
 * ───────────────────────────────────────────────────────────── */
static u16 ip_checksum(const void *data, usize len) {
    const u16 *p = (const u16*)data;
    u32 sum = 0;
    while (len > 1) { sum += *p++; len -= 2; }
    if (len) sum += *(const u8*)p;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (u16)(~sum);
}

/* pseudo-header checksum for TCP/UDP */
static u16 transport_checksum(u32 src, u32 dst, u8 proto,
                               const void *hdr, usize len) {
    u32 sum = 0;
    /* pseudo header */
    sum += (src >> 16) & 0xFFFF;
    sum += src & 0xFFFF;
    sum += (dst >> 16) & 0xFFFF;
    sum += dst & 0xFFFF;
    sum += htons(proto);
    sum += htons((u16)len);
    /* actual data */
    const u16 *p = (const u16*)hdr;
    usize l = len;
    while (l > 1) { sum += *p++; l -= 2; }
    if (l) sum += *(const u8*)p;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (u16)(~sum);
}

/* ─────────────────────────────────────────────────────────────
 *  ARP
 * ───────────────────────────────────────────────────────────── */
static void arp_table_add(u32 ip, const u8 *mac) {
    /* update existing entry */
    for (int i = 0; i < arp_count; i++) {
        if (arp_table[i].ip == ip) {
            memcpy(arp_table[i].mac, mac, 6);
            return;
        }
    }
    if (arp_count < ARP_TABLE_SIZE) {
        arp_table[arp_count].ip = ip;
        memcpy(arp_table[arp_count].mac, mac, 6);
        arp_count++;
    }
}

static const u8 *arp_lookup(u32 ip) {
    for (int i = 0; i < arp_count; i++)
        if (arp_table[i].ip == ip) return arp_table[i].mac;
    return NULL;
}

static void arp_send_request(u32 target_ip) {
    u8 pkt[sizeof(EthHdr) + sizeof(ArpPkt)];
    EthHdr  *eth = (EthHdr*)pkt;
    ArpPkt  *arp = (ArpPkt*)(pkt + sizeof(EthHdr));

    memcpy(eth->dst, broadcast_mac, 6);
    memcpy(eth->src, net_mac, 6);
    eth->type = htons(ETH_ARP);

    arp->htype = htons(1);          /* Ethernet */
    arp->ptype = htons(ETH_IP);
    arp->hlen  = 6;
    arp->plen  = 4;
    arp->oper  = htons(1);          /* request */
    memcpy(arp->sha, net_mac, 6);
    arp->spa   = net_ip;
    memset(arp->tha, 0, 6);
    arp->tpa   = target_ip;

    nic_send((const u8*)(pkt), (usize)(sizeof(pkt)));
}

static void arp_send_reply(u32 dst_ip, const u8 *dst_mac) {
    u8 pkt[sizeof(EthHdr) + sizeof(ArpPkt)];
    EthHdr  *eth = (EthHdr*)pkt;
    ArpPkt  *arp = (ArpPkt*)(pkt + sizeof(EthHdr));

    memcpy(eth->dst, dst_mac, 6);
    memcpy(eth->src, net_mac, 6);
    eth->type = htons(ETH_ARP);

    arp->htype = htons(1);
    arp->ptype = htons(ETH_IP);
    arp->hlen  = 6;
    arp->plen  = 4;
    arp->oper  = htons(2);          /* reply */
    memcpy(arp->sha, net_mac, 6);
    arp->spa   = net_ip;
    memcpy(arp->tha, dst_mac, 6);
    arp->tpa   = dst_ip;

    nic_send((const u8*)(pkt), (usize)(sizeof(pkt)));
}

/* resolve IP→MAC, waits up to ~500ms */
static const u8 *arp_resolve(u32 ip) {
    const u8 *mac = arp_lookup(ip);
    if (mac) return mac;
    arp_send_request(ip);
    /* Wait up to ~1 second. Use both pit_ticks (when scheduler is running)
     * and a raw iteration counter (before scheduler starts, pit_ticks == 0). */
    u64 start = pit_ticks;
    u32 iters = 0;
    while (iters < 2000000) {
        net_poll();
        mac = arp_lookup(ip);
        if (mac) return mac;
        /* Also exit if pit_ticks has advanced by 100 ticks (1 second) */
        if (pit_ticks != start && (pit_ticks - start) >= 100) break;
        net_delay(500);
        iters++;
        watchdog_pet();
    }
    return arp_lookup(ip);   /* one last check */
}

/* ─────────────────────────────────────────────────────────────
 *  IP send
 * ───────────────────────────────────────────────────────────── */
static u16 ip_id_counter = 1;

/* The default gateway — set by net_start() to QEMU slirp's 10.0.2.2 */
u32 net_gateway = 0;

void net_ip_send(u32 dst_ip, u8 proto, const void *payload, u16 plen) {
    /* determine destination MAC */
    const u8 *dst_mac;
    if (dst_ip == 0xFFFFFFFF) {
        /* IP broadcast → Ethernet broadcast, no ARP needed */
        dst_mac = broadcast_mac;
    } else {
        /* In QEMU slirp, ALL traffic (including 10.0.2.x virtual hosts like
         * the DNS server at 10.0.2.3) must go through the gateway 10.0.2.2.
         * Slirp intercepts at the gateway level — direct ARP for 10.0.2.3
         * will never be answered. Always route via gateway when one is set. */
        u32 nexthop = net_gateway ? net_gateway : dst_ip;
        dst_mac = arp_resolve(nexthop);
        if (!dst_mac) return;   /* ARP failed */
    }

    u16 total = (u16)(sizeof(IpHdr) + plen);
    u8 pkt[sizeof(EthHdr) + sizeof(IpHdr) + PKT_SIZE];
    if (total + sizeof(EthHdr) > sizeof(pkt)) return;

    EthHdr *eth = (EthHdr*)pkt;
    IpHdr  *ip  = (IpHdr*)(pkt + sizeof(EthHdr));
    void   *pay = (u8*)ip + sizeof(IpHdr);

    memcpy(eth->dst, dst_mac, 6);
    memcpy(eth->src, net_mac, 6);
    eth->type = htons(ETH_IP);

    ip->ihl_ver   = 0x45;
    ip->dscp      = 0;
    ip->total_len = htons(total);
    ip->id        = htons(ip_id_counter++);
    ip->frag_off  = 0;
    ip->ttl       = 64;
    ip->proto     = proto;
    ip->checksum  = 0;
    ip->src_ip    = net_ip;
    ip->dst_ip    = dst_ip;
    ip->checksum  = ip_checksum(ip, sizeof(IpHdr));

    memcpy(pay, payload, plen);
    nic_send((const u8*)(pkt), (usize)((u16)(sizeof(EthHdr) + total)));
}

/* ─────────────────────────────────────────────────────────────
 *  ICMP ping
 * ───────────────────────────────────────────────────────────── */
static u16 icmp_ping_seq = 0;

static void icmp_send_echo(u32 dst_ip) {
    u8 buf[sizeof(IcmpHdr) + 32];
    IcmpHdr *icmp = (IcmpHdr*)buf;
    icmp->type     = 8;     /* echo request */
    icmp->code     = 0;
    icmp->checksum = 0;
    icmp->id       = htons(0x5348);   /* 'SH' */
    icmp->seq      = htons(icmp_ping_seq++);
    /* payload: 32 bytes of 'A' */
    memset(buf + sizeof(IcmpHdr), 'A', 32);
    icmp->checksum = ip_checksum(buf, sizeof(buf));
    net_ip_send(dst_ip, 1, buf, sizeof(buf));
}

int net_ping(u32 ip) {
    if (!net_ready || !net_ip) { kprintf("net: not ready\r\n"); return 0; }
    icmp_got_reply = 0;
    icmp_send_echo(ip);
    /* Wait up to 3 seconds (300 ticks at 100Hz).
     * Also count raw iterations as a fallback for the window before
     * pit_ticks starts advancing (scheduler not yet running). */
    u64 start = pit_ticks;
    u32 iters = 0;
    while (iters < 6000000) {
        net_poll();
        if (icmp_got_reply) return 1;
        net_delay(1000);
        iters++;
        watchdog_pet();
        if (pit_ticks != start && (pit_ticks - start) >= 300) break;
    }
    return 0;
}

/* ─────────────────────────────────────────────────────────────
 *  UDP send
 * ───────────────────────────────────────────────────────────── */
void net_udp_send(u32 dst_ip, u16 src_port, u16 dst_port,
                     const void *data, u16 dlen) {
    u16 udp_len = (u16)(sizeof(UdpHdr) + dlen);
    u8 buf[sizeof(UdpHdr) + PKT_SIZE];
    UdpHdr *udp = (UdpHdr*)buf;
    udp->src_port = htons(src_port);
    udp->dst_port = htons(dst_port);
    udp->length   = htons(udp_len);
    udp->checksum = 0;
    memcpy(buf + sizeof(UdpHdr), data, dlen);
    udp->checksum = transport_checksum(net_ip, dst_ip, 17, buf, udp_len);
    net_ip_send(dst_ip, 17, buf, udp_len);
}

/* ─────────────────────────────────────────────────────────────
 *  DHCP — get an IP address automatically
 * ───────────────────────────────────────────────────────────── */
#define DHCP_DISCOVER 1
#define DHCP_OFFER    2
#define DHCP_REQUEST  3
#define DHCP_ACK      5

static volatile u32 dhcp_offered_ip = 0;
static volatile u32 dhcp_xid        = 0x12345678;

static void dhcp_send_discover(void) {
    DhcpPkt pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.op    = 1;      /* BOOTREQUEST */
    pkt.htype = 1;      /* Ethernet */
    pkt.hlen  = 6;
    pkt.xid   = htonl(dhcp_xid);
    pkt.flags = htons(0x8000);   /* broadcast */
    memcpy(pkt.chaddr, net_mac, 6);
    /* magic cookie */
    pkt.options[0] = 99; pkt.options[1] = 130;
    pkt.options[2] = 83; pkt.options[3] = 99;
    /* option 53: DHCP DISCOVER */
    pkt.options[4] = 53; pkt.options[5] = 1; pkt.options[6] = DHCP_DISCOVER;
    /* end */
    pkt.options[7] = 255;
    net_udp_send(0xFFFFFFFF, 68, 67, &pkt, sizeof(pkt));
}

static void dhcp_send_request(u32 offered_ip, u32 server_ip) {
    DhcpPkt pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.op    = 1;
    pkt.htype = 1;
    pkt.hlen  = 6;
    pkt.xid   = htonl(dhcp_xid);
    pkt.flags = htons(0x8000);
    memcpy(pkt.chaddr, net_mac, 6);
    pkt.options[0] = 99;  pkt.options[1] = 130;
    pkt.options[2] = 83;  pkt.options[3] = 99;
    pkt.options[4] = 53;  pkt.options[5] = 1; pkt.options[6] = DHCP_REQUEST;
    pkt.options[7] = 50;  pkt.options[8] = 4;   /* requested IP */
    pkt.options[9]  = (u8)(offered_ip>>24); pkt.options[10] = (u8)(offered_ip>>16);
    pkt.options[11] = (u8)(offered_ip>>8);  pkt.options[12] = (u8)(offered_ip);
    pkt.options[13] = 54; pkt.options[14] = 4;  /* server IP */
    pkt.options[15] = (u8)(server_ip>>24); pkt.options[16] = (u8)(server_ip>>16);
    pkt.options[17] = (u8)(server_ip>>8);  pkt.options[18] = (u8)(server_ip);
    pkt.options[19] = 255;
    (void)server_ip;
    net_udp_send(0xFFFFFFFF, 68, 67, &pkt, sizeof(pkt));
}

/* run DHCP, returns 1 on success */
static int dhcp_run(void) {
    /* Try up to 3 times, waiting up to 3 seconds each attempt.
     * QEMU slirp DHCP typically responds within 100-500ms.
     * We use pit_ticks (100Hz timer) for real time measurement. */
    for (int attempt = 0; attempt < 3; attempt++) {
        dhcp_offered_ip = 0;
        dhcp_send_discover();

        /* Wait up to 3 seconds (300 ticks at 100Hz) for OFFER */
        u64 start = pit_ticks;
        while (pit_ticks - start < 300) {
            net_poll();
            if (dhcp_offered_ip) break;
            net_delay(1000);
        }
        if (!dhcp_offered_ip) continue;

        u32 offered = dhcp_offered_ip;
        dhcp_send_request(offered, 0);

        /* Wait up to 1 second for ACK */
        start = pit_ticks;
        while (pit_ticks - start < 100) {
            net_poll();
            net_delay(1000);
        }
        net_ip = htonl(offered);
        return 1;
    }
    return 0;
}

/* ─────────────────────────────────────────────────────────────
 *  TCP — connection table (replaces single global TcpConn)
 *
 *  TCP_MAX_CONNS slots, each can be in one of these states:
 *    CLOSED      — slot is free
 *    SYN_SENT    — client: SYN sent, waiting for SYN-ACK
 *    ESTABLISHED — data can flow in both directions
 *    CLOSE_WAIT  — received FIN, waiting for app to close
 *    LISTEN      — server: bound, waiting for inbound SYN
 *    SYN_RCVD    — server: received SYN, sent SYN-ACK, waiting for ACK
 * ───────────────────────────────────────────────────────────── */
#define TCP_MAX_CONNS   8
#define TCP_RX_BUF_SZ   4096

#define TCP_CLOSED      0
#define TCP_SYN_SENT    1
#define TCP_ESTABLISHED 2
#define TCP_CLOSE_WAIT  3
#define TCP_LISTEN      4
#define TCP_SYN_RCVD    5

typedef struct TcpConn {
    u32 remote_ip;
    u16 local_port, remote_port;
    u32 seq, ack;
    u8  state;
    u8  rx_buf[TCP_RX_BUF_SZ];
    u32 rx_len;
    int fin_received;
} TcpConn;

static TcpConn tcp_conns[TCP_MAX_CONNS];

/* ── connection table helpers ────────────────────────────────── */
static TcpConn *tcp_alloc(void) {
    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        if (tcp_conns[i].state == TCP_CLOSED)
            return &tcp_conns[i];
    }
    return NULL;
}

static void tcp_free(TcpConn *c) {
    if (c) {
        memset(c, 0, sizeof(TcpConn));   /* state → TCP_CLOSED */
    }
}

/* Find an established/close-wait conn by remote ip+port and local port */
static TcpConn *tcp_find(u32 remote_ip, u16 remote_port, u16 local_port) {
    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        TcpConn *c = &tcp_conns[i];
        if (c->state == TCP_CLOSED || c->state == TCP_LISTEN) continue;
        if (c->remote_ip == remote_ip &&
            c->remote_port == remote_port &&
            c->local_port  == local_port)
            return c;
    }
    return NULL;
}

/* Find a LISTEN slot on a given local port */
static TcpConn *tcp_find_listener(u16 local_port) {
    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        TcpConn *c = &tcp_conns[i];
        if (c->state == TCP_LISTEN && c->local_port == local_port)
            return c;
    }
    return NULL;
}

/* ── server accept queue ─────────────────────────────────────── */
#define ACCEPT_QUEUE_SZ  4
typedef struct {
    TcpConn *conn;               /* points into tcp_conns[] */
} AcceptEntry;

static AcceptEntry accept_queue[ACCEPT_QUEUE_SZ];
static int accept_head = 0;   /* next slot to read  */
static int accept_tail = 0;   /* next slot to write */
static int accept_count = 0;

static void accept_enqueue(TcpConn *c) {
    if (accept_count >= ACCEPT_QUEUE_SZ) return;   /* queue full — drop */
    accept_queue[accept_tail].conn = c;
    accept_tail = (accept_tail + 1) % ACCEPT_QUEUE_SZ;
    accept_count++;
}

static TcpConn *accept_dequeue(void) {
    if (accept_count == 0) return NULL;
    TcpConn *c = accept_queue[accept_head].conn;
    accept_head = (accept_head + 1) % ACCEPT_QUEUE_SZ;
    accept_count--;
    return c;
}

/* ── ephemeral port counter ──────────────────────────────────── */
static u16 next_ephemeral_port = 49152;

static void tcp_send_pkt(TcpConn *c, u8 flags, const void *data, u16 dlen) {
    u16 hdr_len = sizeof(TcpHdr);
    u8 buf[sizeof(TcpHdr) + PKT_SIZE];
    TcpHdr *tcp = (TcpHdr*)buf;

    tcp->src_port = htons(c->local_port);
    tcp->dst_port = htons(c->remote_port);
    tcp->seq      = htonl(c->seq);
    tcp->ack      = htonl(c->ack);
    tcp->data_off = (u8)((hdr_len / 4) << 4);
    tcp->flags    = flags;
    tcp->window   = htons(4096);
    tcp->checksum = 0;
    tcp->urgent   = 0;
    if (dlen > 0) memcpy(buf + hdr_len, data, dlen);
    u16 total = (u16)(hdr_len + dlen);
    tcp->checksum = transport_checksum(net_ip, c->remote_ip, 6, buf, total);
    net_ip_send(c->remote_ip, 6, buf, total);
}

static int tcp_connect(TcpConn *c, u32 ip, u16 port) {
    c->remote_ip    = ip;
    c->local_port   = next_ephemeral_port++;
    if (next_ephemeral_port == 0) next_ephemeral_port = 49152;
    c->remote_port  = port;
    c->seq          = 0x12345678;
    c->ack          = 0;
    c->state        = TCP_SYN_SENT;
    c->rx_len       = 0;
    c->fin_received = 0;

    tcp_send_pkt(c, TCP_SYN, NULL, 0);
    c->seq++;

    for (int i = 0; i < 500000; i++) {
        net_poll();
        if (c->state == TCP_ESTABLISHED) return 1;
        net_delay(10);
        watchdog_pet();
    }
    tcp_free(c);
    return 0;
}

static void tcp_send_data(TcpConn *c, const void *data, u16 len) {
    tcp_send_pkt(c, TCP_ACK | TCP_PSH, data, len);
    c->seq += len;
}

static void tcp_close(TcpConn *c) {
    tcp_send_pkt(c, TCP_FIN | TCP_ACK, NULL, 0);
    c->seq++;
    tcp_free(c);
}

/* ─────────────────────────────────────────────────────────────
 *  Packet receive and dispatch
 * ───────────────────────────────────────────────────────────── */
static void handle_tcp(IpHdr *ip, TcpHdr *tcp_pkt, u16 ip_total) {
    u16 ip_hdr_len  = (u16)((ip->ihl_ver & 0x0F) * 4);
    u16 tcp_hdr_len = (u16)((tcp_pkt->data_off >> 4) * 4);
    u16 data_len    = (u16)(ip_total - ip_hdr_len - tcp_hdr_len);
    u8  *data       = (u8*)tcp_pkt + tcp_hdr_len;
    u8   flags      = tcp_pkt->flags;

    u16 dst_port = ntohs(tcp_pkt->dst_port);
    u16 src_port = ntohs(tcp_pkt->src_port);

    /* ── server path: inbound SYN to a LISTEN port ────────────── */
    if (flags & TCP_SYN && !(flags & TCP_ACK)) {
        TcpConn *listener = tcp_find_listener(dst_port);
        if (listener) {
            /* Allocate a new conn slot for this peer */
            TcpConn *c = tcp_alloc();
            if (!c) return;   /* no free slots */
            c->remote_ip   = ip->src_ip;
            c->local_port  = dst_port;
            c->remote_port = src_port;
            c->seq         = 0xABCD1234;   /* server ISN */
            c->ack         = ntohl(tcp_pkt->seq) + 1;
            c->state       = TCP_SYN_RCVD;
            c->rx_len      = 0;
            c->fin_received = 0;
            /* Send SYN-ACK */
            tcp_send_pkt(c, TCP_SYN | TCP_ACK, NULL, 0);
            c->seq++;
            return;
        }
        /* No listener — send RST */
        return;
    }

    /* ── find existing connection ──────────────────────────────── */
    TcpConn *c = tcp_find(ip->src_ip, src_port, dst_port);
    if (!c) return;

    /* ── SYN_RCVD: waiting for final ACK of server handshake ───── */
    if (c->state == TCP_SYN_RCVD) {
        if (flags & TCP_ACK) {
            c->state = TCP_ESTABLISHED;
            accept_enqueue(c);
        }
        return;
    }

    /* ── client SYN_SENT: waiting for SYN-ACK ────────────────── */
    if (c->state == TCP_SYN_SENT) {
        if ((flags & (TCP_SYN|TCP_ACK)) == (TCP_SYN|TCP_ACK)) {
            c->ack = ntohl(tcp_pkt->seq) + 1;
            c->state = TCP_ESTABLISHED;
            tcp_send_pkt(c, TCP_ACK, NULL, 0);
        }
        return;
    }

    /* ── ESTABLISHED / CLOSE_WAIT: data and FIN handling ─────── */
    if (c->state == TCP_ESTABLISHED || c->state == TCP_CLOSE_WAIT) {
        if (data_len > 0) {
            c->ack = ntohl(tcp_pkt->seq) + data_len;
            usize copy = data_len;
            if (c->rx_len + copy > TCP_RX_BUF_SZ)
                copy = TCP_RX_BUF_SZ - c->rx_len;
            if (copy > 0) {
                memcpy(c->rx_buf + c->rx_len, data, copy);
                c->rx_len += (u32)copy;
            }
            tcp_send_pkt(c, TCP_ACK, NULL, 0);
        }
        if (flags & TCP_FIN) {
            c->ack++;
            c->fin_received = 1;
            c->state = TCP_CLOSE_WAIT;
            tcp_send_pkt(c, TCP_ACK, NULL, 0);
        }
    }
}

static void handle_dhcp(UdpHdr *udp, DhcpPkt *dhcp) {
    (void)udp;
    /* check magic cookie */
    if (dhcp->options[0] != 99 || dhcp->options[1] != 130) return;
    if (ntohl(dhcp->xid) != dhcp_xid) return;

    /* find message type */
    u8 msg_type = 0;
    for (int i = 4; i < 64 && dhcp->options[i] != 255; ) {
        u8 opt = dhcp->options[i];
        u8 len = dhcp->options[i+1];
        if (opt == 53 && len == 1) { msg_type = dhcp->options[i+2]; }
        i += 2 + len;
    }
    if (msg_type == DHCP_OFFER || msg_type == DHCP_ACK)
        dhcp_offered_ip = ntohl(dhcp->yiaddr);
}

void net_poll(void) {
    if (!net_ready) return;
    /* Delegate RX to the NIC abstraction layer (e1000.c / rtl8139).
     * Each received frame calls back into net_rx_deliver() above,
     * which runs the full ARP/IP/TCP/UDP dispatch. */
    nic_poll();
}

/* ─────────────────────────────────────────────────────────────
 *  HTTP GET
 *  Downloads http://ip:port/path into buf (up to bufsz bytes).
 *  Returns number of bytes in body, or -1 on error.
 * ───────────────────────────────────────────────────────────── */
int net_http_get(u32 ip, u16 port, const char *path,
                 void *buf, usize bufsz) {
    if (!net_ready || !net_ip) return -1;

    TcpConn *c = tcp_alloc();
    if (!c) { kprintf("http: no free conn slots\r\n"); return -1; }

    if (!tcp_connect(c, ip, port)) {
        kprintf("http: connect failed\r\n");
        return -1;
    }

    /* build GET request */
    char req[512];
    u32 hip = ntohl(ip);
    int rlen = snprintf(req, sizeof(req),
        "GET %s HTTP/1.0\r\nHost: %u.%u.%u.%u\r\nConnection: close\r\n\r\n",
        path,
        (hip >> 24) & 0xFF, (hip >> 16) & 0xFF,
        (hip >>  8) & 0xFF,  hip        & 0xFF);

    tcp_send_data(c, (u8*)req, (u16)rlen);

    /* wait for FIN or buffer full */
    for (int i = 0; i < 2000000; i++) {
        net_poll();
        if (c->fin_received) break;
        net_delay(10);
        watchdog_pet();
    }
    tcp_close(c);

    /* strip HTTP headers (\r\n\r\n separator) */
    u8 *rbuf = c->rx_buf;
    u32 rlen2 = c->rx_len;
    u32 body_start = 0;
    for (u32 i = 0; i + 3 < rlen2; i++) {
        if (rbuf[i]=='\r' && rbuf[i+1]=='\n' &&
            rbuf[i+2]=='\r' && rbuf[i+3]=='\n') {
            body_start = i + 4;
            break;
        }
    }
    u32 body_len = rlen2 - body_start;
    if (body_len > bufsz) body_len = (u32)bufsz;
    memcpy(buf, rbuf + body_start, body_len);
    return (int)body_len;
}

/* ─────────────────────────────────────────────────────────────
 *  Public helpers
 * ───────────────────────────────────────────────────────────── */
/* ─────────────────────────────────────────────────────────────
 *  DNS — resolve hostname to IP using QEMU's DNS at 10.0.2.3
 * ───────────────────────────────────────────────────────────── */
static volatile u32 dns_result = 0;
static u16 dns_txid = 0;

static void dns_handle_reply(UdpHdr *udp, u8 *payload, u16 plen) {
    (void)udp;
    if (plen < 12) return;
    u16 txid = (u16)((payload[0]<<8)|payload[1]);
    /* Accept replies with matching txid OR if txid is 0 (accept any) */
    if (dns_txid != 0 && txid != dns_txid) return;
    u16 flags   = (u16)((payload[2]<<8)|payload[3]);
    u16 ancount = (u16)((payload[6]<<8)|payload[7]);
    if (!(flags & 0x8000) || ancount == 0) return;

    /* skip question section: find end of QNAME */
    int off = 12;
    while (off < plen && payload[off]) {
        off += payload[off] + 1;
        if (off > plen) return;   /* malformed */
    }
    off += 5;  /* null byte + qtype(2) + qclass(2) */

    /* scan ALL answer records until we find an A (type 1) record */
    for (u16 i = 0; i < ancount; i++) {
        if (off >= plen) return;
        /* skip name: pointer (0xC0xx) or label sequence */
        if ((payload[off] & 0xC0) == 0xC0) {
            off += 2;
        } else {
            while (off < plen && payload[off]) off += payload[off] + 1;
            off++;  /* null terminator */
        }
        if (off + 10 > plen) return;
        u16 rtype = (u16)((payload[off]<<8)|payload[off+1]); off += 2;
        off += 2;  /* class */
        off += 4;  /* TTL */
        u16 rdlen = (u16)((payload[off]<<8)|payload[off+1]); off += 2;
        if (rtype == 1 && rdlen == 4 && off + 4 <= plen) {
            /* A record found — store in network byte order (matches net_ip) */
            dns_result = htonl(((u32)payload[off]   << 24) |
                               ((u32)payload[off+1] << 16) |
                               ((u32)payload[off+2] <<  8) |
                                (u32)payload[off+3]);
            return;
        }
        /* skip this record's RDATA and continue */
        off += rdlen;
    }
}

/* Resolve hostname → IP. Returns 0 on failure. */
u32 net_dns_resolve(const char *hostname) {
    /* Check if it's already a dotted-decimal IP */
    int dots = 0;
    int all_digits = 1;
    for (const char *p = hostname; *p; p++) {
        if (*p == '.') dots++;
        else if (*p < '0' || *p > '9') all_digits = 0;
    }
    if (dots == 3 && all_digits) {
        u8 a=0,b=0,c=0,d=0; int n=0;
        for (const char *p=hostname; *p; p++) {
            if (*p=='.') n++;
            else if (*p>='0'&&*p<='9') {
                u8 *oc=n==0?&a:n==1?&b:n==2?&c:&d;
                *oc=(u8)(*oc*10+(*p-'0'));
            }
        }
        return net_make_ip(a,b,c,d);
    }

    u32 dns_server = net_make_ip(10, 0, 2, 3);  /* QEMU slirp DNS */

    /* Try up to 3 times */
    for (int attempt = 0; attempt < 3; attempt++) {
        dns_result = 0;
        dns_txid++;

        /* Build DNS query packet */
        u8 pkt[256];
        u16 off = 0;
        pkt[off++] = (u8)(dns_txid>>8); pkt[off++] = (u8)dns_txid;
        pkt[off++] = 0x01; pkt[off++] = 0x00;  /* RD=1 */
        pkt[off++] = 0x00; pkt[off++] = 0x01;  /* QDCOUNT=1 */
        pkt[off++] = 0x00; pkt[off++] = 0x00;
        pkt[off++] = 0x00; pkt[off++] = 0x00;
        pkt[off++] = 0x00; pkt[off++] = 0x00;
        const char *h = hostname;
        while (*h) {
            const char *dot = h;
            while (*dot && *dot != '.') dot++;
            u8 llen = (u8)(dot - h);
            pkt[off++] = llen;
            while (h < dot) pkt[off++] = (u8)(*h++);
            if (*h == '.') h++;
        }
        pkt[off++] = 0x00;
        pkt[off++] = 0x00; pkt[off++] = 0x01;  /* QTYPE=A */
        pkt[off++] = 0x00; pkt[off++] = 0x01;  /* QCLASS=IN */

        /* Send from our fixed DNS client port (1024) to port 53 */
        net_udp_send(dns_server, 1024, 53, pkt, off);
        kprintf("dns: query sent (attempt ");
        vga_putchar((u8)('1' + attempt));
        kprintf("), waiting...\r\n");

        /* Wait up to 2 seconds (200 ticks) for reply */
        u64 start = pit_ticks;
        u32 iters = 0;
        while (iters < 4000000) {
            net_poll();
            if (dns_result) return dns_result;
            if (pit_ticks != start && (pit_ticks - start) >= 200) break;
            net_delay(500);
            iters++;
            watchdog_pet();
        }
    }
    kprintf("dns: failed to resolve '");
    kprintf("%s", hostname);
    kprintf("'\r\n");
    return 0;
}

u32 net_make_ip(u8 a, u8 b, u8 c, u8 d) {
    return htonl(((u32)a<<24)|((u32)b<<16)|((u32)c<<8)|d);
}

void net_print_ip(u32 ip) {
    u32 h = ntohl(ip);
    char buf[20];
    kprintf("%u.%u.%u.%u",
              (h >> 24) & 0xFF, (h >> 16) & 0xFF,
              (h >>  8) & 0xFF,  h        & 0xFF);
}

/* ─────────────────────────────────────────────────────────────
 *  Public TCP server API
 *
 *  Typical server usage from the shell:
 *    net_tcp_listen(port)          — bind a LISTEN slot
 *    c = net_tcp_accept(port)      — block until client connects
 *    n = net_tcp_recv(c, buf, sz)  — read data (0 = peer closed)
 *    net_tcp_send(c, buf, n)       — send data
 *    net_tcp_close(c)              — send FIN, free slot
 * ───────────────────────────────────────────────────────────── */

/* Start listening on local_port.  Returns 1 on success, 0 if no slot. */
int net_tcp_listen(u16 port) {
    if (!net_ready) return 0;
    /* Check not already listening on this port */
    if (tcp_find_listener(port)) return 1;   /* already up */
    TcpConn *c = tcp_alloc();
    if (!c) return 0;
    c->local_port = port;
    c->state      = TCP_LISTEN;
    return 1;
}

/* Non-blocking accept: returns NULL immediately if no client ready. */
TcpConn *net_tcp_accept_nb(u16 port) {
    for (int i = 0; i < accept_count; i++) {
        int idx = (accept_head + i) % ACCEPT_QUEUE_SZ;
        TcpConn *c = accept_queue[idx].conn;
        if (c && c->local_port == port && c->state == TCP_ESTABLISHED) {
            while (accept_count > 0) {
                TcpConn *front = accept_dequeue();
                if (front == c) return c;
                accept_enqueue(front);
            }
        }
    }
    return NULL;
}

/* Block until a client completes the handshake on port, then return
 * a connection handle.  Polls the NIC while waiting.
 * Returns NULL if net is not ready. */
TcpConn *net_tcp_accept(u16 port) {
    if (!net_ready) return NULL;
    for (;;) {
        net_poll();
        /* Check accept queue for a connection on our port */
        for (int i = 0; i < accept_count; i++) {
            int idx = (accept_head + i) % ACCEPT_QUEUE_SZ;
            TcpConn *c = accept_queue[idx].conn;
            if (c && c->local_port == port && c->state == TCP_ESTABLISHED) {
                /* Dequeue it (swap-and-shrink the ring isn't trivial; just
                 * drain from the head until we hit our conn) */
                TcpConn *got = NULL;
                while (accept_count > 0) {
                    TcpConn *front = accept_dequeue();
                    if (front->local_port == port) { got = front; break; }
                    /* re-enqueue connections for other ports */
                    accept_enqueue(front);
                }
                if (got) return got;
            }
        }
        net_delay(1000);
    }
}

/* Stop listening on port (frees the LISTEN slot; does NOT close
 * established connections that came from it). */
void net_tcp_unlisten(u16 port) {
    TcpConn *c = tcp_find_listener(port);
    if (c) tcp_free(c);
}

/* Send data on an established connection.
 * Fragments into PKT_SIZE chunks automatically.
 * Returns number of bytes sent, or -1 on error. */
int net_tcp_send(TcpConn *c, const void *data, u16 len) {
    if (!c || c->state != TCP_ESTABLISHED) return -1;
    const u8 *p = (const u8*)data;
    u16 sent = 0;
    while (sent < len) {
        u16 chunk = (u16)(len - sent);
        if (chunk > PKT_SIZE - 60) chunk = PKT_SIZE - 60;
        tcp_send_data(c, p + sent, chunk);
        sent += chunk;
    }
    return sent;
}

/* Receive data from an established connection into buf (up to bufsz bytes).
 * Blocks until data arrives or peer closes.
 * Returns bytes copied, 0 on clean close, -1 on error. */
int net_tcp_recv(TcpConn *c, void *buf, usize bufsz) {
    if (!c) return -1;
    /* Wait until we have data or peer has sent FIN */
    while (c->rx_len == 0 && !c->fin_received &&
           c->state == TCP_ESTABLISHED) {
        net_poll();
        net_delay(500);
        watchdog_pet();
    }
    if (c->rx_len == 0) return 0;   /* EOF */
    usize n = c->rx_len;
    if (n > bufsz) n = bufsz;
    memcpy(buf, c->rx_buf, n);
    /* Shift remaining data down */
    usize remaining = c->rx_len - n;
    if (remaining > 0)
        memcpy(c->rx_buf, c->rx_buf + n, remaining);
    c->rx_len = (u32)remaining;
    return (int)n;
}

/* Close an established or close-wait connection gracefully. */
void net_tcp_close(TcpConn *c) {
    if (!c) return;
    if (c->state == TCP_ESTABLISHED || c->state == TCP_CLOSE_WAIT)
        tcp_close(c);   /* sends FIN+ACK, frees slot */
    else
        tcp_free(c);
}

/* ─────────────────────────────────────────────────────────────
 *  Simple built-in HTTP file server
 *
 *  net_http_serve(port) binds port and serves GET requests for
 *  files from the FAT32 root directory until net_http_serve_stop()
 *  is called (or an error occurs).
 *
 *  Responses: 200 OK with file data, or 404 Not Found.
 *  Only handles one request at a time (no threading).
 * ───────────────────────────────────────────────────────────── */
static volatile int http_server_running = 0;

static void http_send_response(TcpConn *c, int status,
                               const char *body, u16 blen) {
    const char *status_str = (status == 200) ? "200 OK" : "404 Not Found";
    char hdr[128];
    int hlen = snprintf(hdr, sizeof(hdr),
        "HTTP/1.0 %s\r\nContent-Length: %u\r\nConnection: close\r\n\r\n",
        status_str, (unsigned)blen);

    net_tcp_send(c, hdr, hlen);
    if (blen > 0) net_tcp_send(c, body, blen);
}

/* Handle one HTTP request on conn c.  Reads the first line, parses
 * the path, opens the file from FAT32, and sends back the content. */
static void http_handle_one(TcpConn *c) {
    /* Read request (wait up to 2s) */
    static u8 req_buf[512];
    u64 start = pit_ticks;
    u32 req_len = 0;
    while (pit_ticks - start < 200) {
        int n = net_tcp_recv(c, req_buf + req_len,
                             sizeof(req_buf) - 1 - req_len);
        if (n <= 0) break;
        req_len += (u32)n;
        /* Check for end of headers */
        req_buf[req_len] = 0;
        int found = 0;
        for (u32 i = 0; i + 3 < req_len; i++) {
            if (req_buf[i]=='\r' && req_buf[i+1]=='\n' &&
                req_buf[i+2]=='\r' && req_buf[i+3]=='\n') {
                found = 1; break;
            }
        }
        if (found) break;
    }
    req_buf[req_len] = 0;

    /* Parse: GET /filename HTTP/... */
    char path[64] = {0};
    int pi = 0;
    /* Skip "GET /" */
    u8 *p = req_buf;
    if (req_len < 5) goto bad_request;
    if (p[0]!='G' || p[1]!='E' || p[2]!='T' || p[3]!=' ') goto bad_request;
    p += 4;
    if (*p == '/') p++;   /* skip leading slash */
    while (*p && *p != ' ' && *p != '\r' && *p != '\n' && pi < 63)
        path[pi++] = (char)*p++;
    path[pi] = 0;

    if (pi == 0) {
        /* Root request — list files */
        static const char index_page[] =
            "<html><body><h2>Systrix HTTP Server</h2>"
            "<p>Specify a filename in the URL, e.g. /HELLO.TXT</p>"
            "</body></html>";
        http_send_response(c, 200, index_page,
                           (u16)(sizeof(index_page) - 1));
        goto done;
    }

    {
        /* Open the file from FAT32 */
        char name83[12];
        format_83_name(path, name83);
        i64 fd = vfs_open(name83);
        if (fd < 0) {
            static const char nf[] = "<html><body>404 Not Found</body></html>";
            http_send_response(c, 404, nf, (u16)(sizeof(nf)-1));
            goto done;
        }

        /* Stream file contents (up to 32KB) */
        static u8 file_buf[32768];
        u32 total = 0;
        for (;;) {
            i64 n = vfs_read((u64)fd, file_buf + total,
                             sizeof(file_buf) - total);
            if (n <= 0) break;
            total += (u32)n;
            if (total >= sizeof(file_buf)) break;
        }
        vfs_close((u64)fd);
        http_send_response(c, 200, (char*)file_buf, (u16)total);
    }
    goto done;

bad_request:
    {
        static const char br[] = "HTTP/1.0 400 Bad Request\r\n\r\n";
        net_tcp_send(c, br, (u16)(sizeof(br)-1));
    }
done:
    return;
}

void net_http_serve(u16 port) {
    if (!net_ready || !net_ip) {
        kprintf("httpd: network not ready\r\n");
        return;
    }
    if (!net_tcp_listen(port)) {
        kprintf("httpd: could not bind port\r\n");
        return;
    }
    http_server_running = 1;
    kprintf("httpd: listening on port %u\r\n\r\nPress any key to stop.\r\n", (unsigned)port);

    while (http_server_running) {
        net_poll();
        /* Non-blocking accept check */
        TcpConn *client = NULL;
        for (int i = 0; i < accept_count; i++) {
            int idx = (accept_head + i) % ACCEPT_QUEUE_SZ;
            TcpConn *candidate = accept_queue[idx].conn;
            if (candidate && candidate->local_port == port &&
                candidate->state == TCP_ESTABLISHED) {
                /* Pull from accept queue */
                while (accept_count > 0) {
                    TcpConn *front = accept_dequeue();
                    if (front == candidate) { client = front; break; }
                    accept_enqueue(front);
                }
                break;
            }
        }
        if (client) {
            kprintf("httpd: connection from ");
            net_print_ip(client->remote_ip);
            kprintf("\r\n");
            http_handle_one(client);
            net_tcp_close(client);
        }

        /* Check keyboard (non-blocking) */
        if (inb(0x64) & 1) {
            inb(0x60);   /* drain scancode */
            http_server_running = 0;
        }
        net_delay(500);
    }

    net_tcp_unlisten(port);
    kprintf("httpd: stopped.\r\n");
}

/* Called from kernel_main after fat32_init() */
void net_start(void) {
    net_init();

    /* QEMU slirp always assigns 10.0.2.15 to the guest.
     * Configure statically — works immediately without DHCP. */
    net_ip      = net_make_ip(10, 0, 2, 15);
    net_gateway = net_make_ip(10, 0, 2, 2);   /* QEMU slirp gateway */

    /* Pre-populate ARP table with gateway MAC (QEMU slirp: 52:55:0a:00:02:02).
     * This avoids an ARP round-trip for the very first packet and ensures
     * DNS/ping work immediately after boot, even before pit_ticks advances. */
    static const u8 gw_mac[6] = {0x52, 0x55, 0x0a, 0x00, 0x02, 0x02};
    arp_table_add(net_gateway, gw_mac);

    /* Also pre-populate QEMU DNS server MAC (same gateway MAC for slirp) */
    u32 dns_ip = net_make_ip(10, 0, 2, 3);
    arp_table_add(dns_ip, gw_mac);

    kprintf("eth0: IP=");
    net_print_ip(net_ip);
    kprintf(" GW=");
    net_print_ip(net_gateway);
    kprintf("\r\n");
}
