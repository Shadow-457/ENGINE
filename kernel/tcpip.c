#include "../include/kernel.h"

#define ETH_HDR_LEN 14
#define IP_HDR_LEN 20
#define TCP_HDR_LEN 20
#define UDP_HDR_LEN 8
#define ICMP_HDR_LEN 8

#define ETH_P_IP 0x0800
#define ETH_P_ARP 0x0806

#define IP_PROTO_ICMP 1
#define IP_PROTO_TCP 6
#define IP_PROTO_UDP 17

#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10

#define SOCKET_MAX 64
#define TCP_PORT_MAX 65535

#define ETH_ADDR_LEN 6

typedef struct {
    u8 dst[6];
    u8 src[6];
    u16 type;
} __attribute__((packed)) eth_hdr_t;

typedef struct {
    u8 ver_ihl;
    u8 tos;
    u16 total_len;
    u16 id;
    u16 frag_off;
    u8 ttl;
    u8 protocol;
    u16 checksum;
    u32 saddr;
    u32 daddr;
} __attribute__((packed)) ip_hdr_t;

typedef struct {
    u16 sport;
    u16 dport;
    u32 seq;
    u32 ack_seq;
    u8 data_off;
    u8 flags;
    u16 window;
    u16 check;
    u16 urg_ptr;
} __attribute__((packed)) tcp_hdr_t;

typedef struct {
    u16 sport;
    u16 dport;
    u16 len;
    u16 check;
} __attribute__((packed)) udp_hdr_t;

typedef struct {
    u16 type;
    u16 code;
    u16 checksum;
    u16 id;
    u16 seq;
} __attribute__((packed)) icmp_hdr_t;

typedef struct {
    u16 hw_type;
    u16 proto_type;
    u8 hw_len;
    u8 proto_len;
    u16 op;
    u8 sender_mac[6];
    u32 sender_ip;
    u8 target_mac[6];
    u32 target_ip;
} __attribute__((packed)) arp_pkt_t;

typedef struct {
    int state;
    u32 local_ip;
    u32 remote_ip;
    u16 local_port;
    u16 remote_port;
    u32 seq;
    u32 ack_seq;
    u16 window;
    u8 recv_buf[8192];
    usize recv_len;
    usize recv_pos;
    u8 send_buf[8192];
    usize send_len;
    int type;
    int nonblock;
} socket_t;


/* Forward declarations for net.c functions used here */
extern void net_ip_send(u32 dst_ip, u8 proto, const void *payload, u16 plen);
extern void net_send_frame(const u8 *data, usize len);
extern u32  net_ip;

/* TCP checksum — RFC 793 pseudo-header + segment */
static u16 tcp_checksum(u32 src_ip, u32 dst_ip,
                         const void *seg, u16 seg_len,
                         const void *data, u16 data_len) {
    u32 sum = 0;
    /* pseudo-header: src, dst, zero, proto=6, tcp length */
    u16 tcp_len = (u16)(seg_len + data_len);
    sum += (src_ip >> 16) & 0xFFFF;
    sum += (src_ip)       & 0xFFFF;
    sum += (dst_ip >> 16) & 0xFFFF;
    sum += (dst_ip)       & 0xFFFF;
    sum += 0x0006;              /* protocol TCP */
    sum += tcp_len;
    /* segment bytes */
    const u16 *p = (const u16*)seg;
    u16 n = seg_len;
    while (n > 1) { sum += *p++; n -= 2; }
    if (n) sum += *(const u8*)p;
    /* data bytes */
    p = (const u16*)data;
    n = data_len;
    while (n > 1) { sum += *p++; n -= 2; }
    if (n) sum += *(const u8*)p;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (u16)(~sum);
}

static socket_t socket_table[SOCKET_MAX];
static u16 next_port = 49152;

#define TCP_CLOSED 0
#define TCP_LISTEN 1
#define TCP_SYN_SENT 2
#define TCP_SYN_RECEIVED 3
#define TCP_ESTABLISHED 4
#define TCP_CLOSE_WAIT 5
#define TCP_LAST_ACK 6
#define TCP_TIME_WAIT 7
#define TCP_FIN_WAIT_1 8
#define TCP_FIN_WAIT_2 9
#define TCP_CLOSING 10

void socket_init(void) {
    memset(socket_table, 0, sizeof(socket_table));
}

static u16 alloc_port(void) {
    u16 p = next_port++;
    if (next_port > 65000) next_port = 49152;
    return p;
}

static int socket_alloc(void) {
    for (int i = 0; i < SOCKET_MAX; i++) {
        if (socket_table[i].state == TCP_CLOSED && socket_table[i].type == 0) {
            socket_table[i].state = TCP_CLOSED;
            socket_table[i].type = 1;
            return i;
        }
    }
    return -1;
}

static socket_t *socket_get(int fd) {
    if (fd < 0 || fd >= SOCKET_MAX || socket_table[fd].type == 0) return NULL;
    return &socket_table[fd];
}

static u16 ip_checksum(void *buf, usize len) {
    u32 sum = 0;
    u16 *p = (u16*)buf;
    while (len > 1) { sum += *p++; len -= 2; }
    if (len) sum += *(u8*)p;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (u16)(~sum);
}

static void arp_send(u32 target_ip, u8 *target_mac) {
    extern u8 net_mac[6];
    extern u32 net_ip;
    u8 pkt[42];
    memset(pkt, 0, 42);
    eth_hdr_t *eth = (eth_hdr_t*)pkt;
    memset(eth->dst, 0xFF, 6);
    memcpy(eth->src, net_mac, 6);
    eth->type = 0x0806;
    arp_pkt_t *arp = (arp_pkt_t*)(pkt + ETH_HDR_LEN);
    arp->hw_type = 1;
    arp->proto_type = 0x0800;
    arp->hw_len = 6;
    arp->proto_len = 4;
    arp->op = 1;
    memcpy(arp->sender_mac, net_mac, 6);
    arp->sender_ip = net_ip;
    arp->target_ip = target_ip;
    net_send_frame(pkt, 42);
    (void)target_mac;
}

static void arp_handle(u8 *data, usize len) {
    (void)len;
    extern u32 net_ip;
    arp_pkt_t *arp = (arp_pkt_t*)data;
    if (arp->target_ip == net_ip && arp->op == 1) {
        extern u8 net_mac[6];
        u8 reply[42];
        memset(reply, 0, 42);
        eth_hdr_t *eth = (eth_hdr_t*)reply;
        memcpy(eth->dst, arp->sender_mac, 6);
        memcpy(eth->src, net_mac, 6);
        eth->type = 0x0806;
        arp_pkt_t *arp_r = (arp_pkt_t*)(reply + ETH_HDR_LEN);
        arp_r->hw_type = 1;
        arp_r->proto_type = 0x0800;
        arp_r->hw_len = 6;
        arp_r->proto_len = 4;
        arp_r->op = 2;
        memcpy(arp_r->sender_mac, net_mac, 6);
        arp_r->sender_ip = net_ip;
        memcpy(arp_r->target_mac, arp->sender_mac, 6);
        arp_r->target_ip = arp->sender_ip;
        extern void net_send_frame(const u8 *data, usize len);
        net_send_frame(reply, 42);
    }
}

static void icmp_send(u32 dst_ip, u8 type, u8 code, u16 id, u16 seq, const void *data, usize len) {
    u8 seg[ICMP_HDR_LEN + 64];
    icmp_hdr_t *icmp = (icmp_hdr_t*)seg;
    icmp->type = type; icmp->code = code;
    icmp->checksum = 0; icmp->id = id; icmp->seq = seq;
    if (data && len > 0 && len <= 64) memcpy(seg + ICMP_HDR_LEN, data, len);
    icmp->checksum = ip_checksum(seg, ICMP_HDR_LEN + len);
    net_ip_send(dst_ip, IP_PROTO_ICMP, seg, (u16)(ICMP_HDR_LEN + len));
}

static void tcp_send(int sock_idx, u8 flags) {
    socket_t *s = &socket_table[sock_idx];
    extern u32 net_ip;
    /* Build only the TCP segment — net_ip_send handles Ethernet + ARP */
    u8 seg[TCP_HDR_LEN];
    tcp_hdr_t *tcp = (tcp_hdr_t*)seg;
    tcp->sport    = s->local_port;
    tcp->dport    = s->remote_port;
    tcp->seq      = s->seq;
    tcp->ack_seq  = (flags & TCP_ACK) ? s->ack_seq : 0;
    tcp->data_off = (TCP_HDR_LEN / 4) << 4;
    tcp->flags    = flags;
    tcp->window   = s->window;
    tcp->check    = 0;
    tcp->urg_ptr  = 0;
    /* TCP checksum over pseudo-header + segment */
    tcp->check = tcp_checksum(net_ip, s->remote_ip, seg, TCP_HDR_LEN, 0, 0);
    net_ip_send(s->remote_ip, IP_PROTO_TCP, seg, TCP_HDR_LEN);
    if (flags & TCP_SYN) s->seq++;
    if ((flags & TCP_FIN)) s->seq++;
}

i64 sys_socket(int domain, int type, int protocol) {
    (void)domain; (void)protocol;
    int nonblock = (type & 0x800) ? 1 : 0;
    type &= ~0x800;
    int idx = socket_alloc();
    if (idx < 0) return (i64)ENOMEM;
    socket_table[idx].type     = type ? type : 1;
    socket_table[idx].nonblock = nonblock;
    socket_table[idx].state    = TCP_CLOSED;
    socket_table[idx].local_port = alloc_port();
    socket_table[idx].window   = 8192;
    return idx;
}

i64 sys_bind(int sockfd, const void *addr, usize addrlen) {
    (void)addrlen;
    socket_t *s = socket_get(sockfd);
    if (!s) return (i64)EBADF;
    if (s->type != 1) return (i64)EOPNOTSUPP;
    const u8 *a = (const u8*)addr;
    s->local_port = (u16)a[2] << 8 | a[3];
    s->state = TCP_LISTEN;
    return 0;
}

i64 sys_listen(int sockfd, int backlog) {
    (void)backlog;
    socket_t *s = socket_get(sockfd);
    if (!s) return (i64)EBADF;
    if (s->state != TCP_LISTEN) return (i64)EINVAL;
    return 0;
}

i64 sys_connect(int sockfd, const void *addr, usize addrlen) {
    (void)addrlen;
    socket_t *s = socket_get(sockfd);
    if (!s) return (i64)EBADF;
    /* already connecting */
    if (s->state == TCP_SYN_SENT) return (i64)EALREADY;
    if (s->state == TCP_ESTABLISHED) return (i64)EISCONN;
    const u8 *a = (const u8*)addr;
    s->remote_ip   = *(u32*)(a + 4);
    s->remote_port = (u16)a[2] << 8 | a[3];
    s->seq         = 0x12345678;
    s->state       = TCP_SYN_SENT;
    tcp_send(sockfd, TCP_SYN);
    /* nonblock: return EINPROGRESS immediately */
    if (s->nonblock) return (i64)EINPROGRESS;
    /* blocking: spin until established or timeout */
    for (int i = 0; i < 100000; i++) {
        if (s->state == TCP_ESTABLISHED) return 0;
        if (s->state == TCP_CLOSED)      return (i64)ECONNREFUSED;
        __asm__ volatile("pause");
        if ((i & 127) == 0) watchdog_pet();
    }
    return (i64)ETIMEDOUT;
}

i64 sys_accept(int sockfd, void *addr, usize *addrlen) {
    (void)addr; (void)addrlen;
    socket_t *s = socket_get(sockfd);
    if (!s) return (i64)EBADF;
    if (s->state != TCP_LISTEN) return (i64)EINVAL;
    for (int i = 0; i < SOCKET_MAX; i++) {
        if (socket_table[i].state == TCP_ESTABLISHED && socket_table[i].remote_port != 0) {
            return i;
        }
    }
    /* no pending connection */
    if (s->nonblock) return (i64)EAGAIN;
    /* blocking: spin-wait for incoming connection */
    for (int spin = 0; spin < 2000000; spin++) {
        for (int i = 0; i < SOCKET_MAX; i++) {
            if (socket_table[i].state == TCP_ESTABLISHED && socket_table[i].remote_port != 0)
                return i;
        }
        __asm__ volatile("pause");
        if ((spin & 127) == 0) watchdog_pet();
    }
    return (i64)EAGAIN;
}

i64 sys_send(int sockfd, const void *buf, usize len, int flags) {
    (void)flags;
    socket_t *s = socket_get(sockfd);
    if (!s) return (i64)EBADF;
    if (s->state != TCP_ESTABLISHED) return (i64)ENOTCONN;
    if (len > 1500) len = 1500;
    extern u32 net_ip;
    /* Build TCP segment with data — net_ip_send handles Ethernet + ARP */
    u8 seg[TCP_HDR_LEN + 1500];
    tcp_hdr_t *tcp = (tcp_hdr_t*)seg;
    tcp->sport    = s->local_port;
    tcp->dport    = s->remote_port;
    tcp->seq      = s->seq;
    tcp->ack_seq  = s->ack_seq;
    tcp->data_off = (TCP_HDR_LEN / 4) << 4;
    tcp->flags    = TCP_PSH | TCP_ACK;
    tcp->window   = s->window;
    tcp->check    = 0;
    tcp->urg_ptr  = 0;
    if (buf && len > 0) memcpy(seg + TCP_HDR_LEN, buf, len);
    tcp->check = tcp_checksum(net_ip, s->remote_ip, seg, TCP_HDR_LEN, buf, (u16)len);
    net_ip_send(s->remote_ip, IP_PROTO_TCP, seg, (u16)(TCP_HDR_LEN + len));
    s->seq += len;
    return (i64)len;
}

i64 sys_recv(int sockfd, void *buf, usize len, int flags) {
    (void)flags;
    socket_t *s = socket_get(sockfd);
    if (!s) return (i64)EBADF;
    if (s->state != TCP_ESTABLISHED) return (i64)ENOTCONN;
    /* nonblock: return EAGAIN immediately if no data */
    if (s->recv_len == 0) {
        if (s->nonblock) return (i64)EAGAIN;
        /* blocking: spin-wait */
        for (int i = 0; i < 2000000; i++) {
            if (s->recv_len > 0) break;
            if (s->state != TCP_ESTABLISHED) return (i64)ENOTCONN;
            __asm__ volatile("pause");
            if ((i & 127) == 0) watchdog_pet();
        }
        if (s->recv_len == 0) return (i64)EAGAIN;
    }
    if (len > s->recv_len) len = s->recv_len;
    if (buf && len > 0) {
        memcpy(buf, s->recv_buf + s->recv_pos, len);
        s->recv_pos += len;
        s->recv_len -= len;
        if (s->recv_len == 0) s->recv_pos = 0;
    }
    return (i64)len;
}

i64 sys_close_socket(int sockfd) {
    socket_t *s = socket_get(sockfd);
    if (!s) return (i64)EBADF;
    if (s->state == TCP_ESTABLISHED) {
        tcp_send(sockfd, TCP_FIN | TCP_ACK);
        s->state = TCP_FIN_WAIT_1;
    }
    s->type = 0;
    s->state = TCP_CLOSED;
    s->remote_ip = 0;
    s->remote_port = 0;
    s->recv_len = 0;
    s->recv_pos = 0;
    s->send_len = 0;
    return 0;
}

void tcpip_handle_packet(u8 *data, usize len) {
    if (len < ETH_HDR_LEN) return;
    eth_hdr_t *eth = (eth_hdr_t*)data;
    if (eth->type == 0x0806) {
        arp_handle(data + ETH_HDR_LEN, len - ETH_HDR_LEN);
        return;
    }
    if (eth->type != 0x0800) return;
    if (len < ETH_HDR_LEN + IP_HDR_LEN) return;
    ip_hdr_t *ip = (ip_hdr_t*)(data + ETH_HDR_LEN);
    if ((ip->ver_ihl & 0xF0) != 0x40) return;
    if (ip->protocol == IP_PROTO_ICMP) {
        icmp_hdr_t *icmp = (icmp_hdr_t*)(data + ETH_HDR_LEN + (ip->ver_ihl & 0x0F) * 4);
        if (icmp->type == 8) {
            icmp_send(ip->saddr, 0, 0, icmp->id, icmp->seq, icmp + 1,
                      len - ETH_HDR_LEN - IP_HDR_LEN - ICMP_HDR_LEN);
        }
    } else if (ip->protocol == IP_PROTO_TCP) {
        tcp_hdr_t *tcp = (tcp_hdr_t*)(data + ETH_HDR_LEN + (ip->ver_ihl & 0x0F) * 4);
        u16 dport = tcp->dport;
        for (int i = 0; i < SOCKET_MAX; i++) {
            if (socket_table[i].type != 0 && socket_table[i].local_port == dport) {
                socket_t *s = &socket_table[i];
                if (tcp->flags & TCP_SYN) {
                    if (s->state == TCP_LISTEN) {
                        s->remote_ip = ip->saddr;
                        s->remote_port = tcp->sport;
                        s->ack_seq = tcp->seq + 1;
                        s->seq = 0x87654321;
                        s->state = TCP_SYN_RECEIVED;
                        tcp_send(i, TCP_SYN | TCP_ACK);
                    }
                } else if (tcp->flags & TCP_ACK) {
                    if (s->state == TCP_SYN_SENT) {
                        s->ack_seq = tcp->seq + 1;
                        s->state = TCP_ESTABLISHED;
                        tcp_send(i, TCP_ACK);
                    } else if (s->state == TCP_SYN_RECEIVED) {
                        s->state = TCP_ESTABLISHED;
                    } else if (s->state == TCP_ESTABLISHED) {
                        s->ack_seq = tcp->seq + 1;
                        usize payload_len = len - ETH_HDR_LEN - IP_HDR_LEN - TCP_HDR_LEN;
                        if (payload_len > 0 && payload_len <= 8192 - s->recv_len) {
                            memcpy(s->recv_buf + s->recv_len, (u8*)(tcp + 1), payload_len);
                            s->recv_len += payload_len;
                        }
                        tcp_send(i, TCP_ACK);
                    }
                } else if (tcp->flags & TCP_FIN) {
                    if (s->state == TCP_ESTABLISHED) {
                        s->state = TCP_CLOSE_WAIT;
                        tcp_send(i, TCP_FIN | TCP_ACK);
                    }
                } else if (tcp->flags & TCP_RST) {
                    s->state = TCP_CLOSED;
                }
                break;
            }
        }
    } else if (ip->protocol == IP_PROTO_UDP) {
        udp_hdr_t *udp = (udp_hdr_t*)(data + ETH_HDR_LEN + (ip->ver_ihl & 0x0F) * 4);
        (void)udp;
    }
}

/* ── 72: sys_fcntl ────────────────────────────────────────────── */
i64 sys_fcntl(u64 fd, u64 cmd, u64 arg) {
    /* Try socket table first */
    if ((int)fd >= 0 && (int)fd < SOCKET_MAX && socket_table[(int)fd].type != 0) {
        socket_t *s = &socket_table[(int)fd];
        switch (cmd) {
        case 3: /* F_GETFL */
            return s->nonblock ? 0x800 : 0;
        case 4: /* F_SETFL */
            s->nonblock = (arg & 0x800) ? 1 : 0;
            return 0;
        case 1: /* F_GETFD */
            return 0;
        case 2: /* F_SETFD */
            return 0;
        default:
            return 0;
        }
    }
    /* Regular file fd: just handle F_GETFL/F_SETFL as no-ops */
    (void)arg;
    if (cmd == 1 || cmd == 2) return 0; /* F_GETFD/F_SETFD */
    if (cmd == 3) return 0;             /* F_GETFL */
    if (cmd == 4) return 0;             /* F_SETFL */
    return 0;
}

/* ── epoll internals ──────────────────────────────────────────── */
#define EPOLL_MAX_INSTANCES 8
#define EPOLL_MAX_WATCHES   64

typedef struct {
    int          fd;
    u32          events;
    epoll_data_t data;
} epoll_watch_t;

typedef struct {
    int          used;
    int          nwatches;
    epoll_watch_t watches[EPOLL_MAX_WATCHES];
} epoll_inst_t;

/* epoll instances live above socket fd space */
#define EPOLL_FD_BASE 200
static epoll_inst_t epoll_table[EPOLL_MAX_INSTANCES];

/* ── 213: sys_epoll_create ────────────────────────────────────── */
i64 sys_epoll_create(int flags) {
    (void)flags;
    for (int i = 0; i < EPOLL_MAX_INSTANCES; i++) {
        if (!epoll_table[i].used) {
            epoll_table[i].used     = 1;
            epoll_table[i].nwatches = 0;
            return EPOLL_FD_BASE + i;
        }
    }
    return (i64)ENOMEM;
}

/* ── 233: sys_epoll_ctl ───────────────────────────────────────── */
i64 sys_epoll_ctl(int epfd, int op, int fd, epoll_event_t *ev) {
    int idx = epfd - EPOLL_FD_BASE;
    if (idx < 0 || idx >= EPOLL_MAX_INSTANCES || !epoll_table[idx].used)
        return (i64)EBADF;
    epoll_inst_t *ep = &epoll_table[idx];

    if (op == 1) { /* EPOLL_CTL_ADD */
        if (ep->nwatches >= EPOLL_MAX_WATCHES) return (i64)ENOMEM;
        ep->watches[ep->nwatches].fd     = fd;
        ep->watches[ep->nwatches].events = ev ? ev->events : 0;
        ep->watches[ep->nwatches].data   = ev ? ev->data : (epoll_data_t){0};
        ep->nwatches++;
        return 0;
    } else if (op == 3) { /* EPOLL_CTL_MOD */
        for (int i = 0; i < ep->nwatches; i++) {
            if (ep->watches[i].fd == fd) {
                ep->watches[i].events = ev ? ev->events : 0;
                ep->watches[i].data   = ev ? ev->data : (epoll_data_t){0};
                return 0;
            }
        }
        return (i64)ENOENT;
    } else if (op == 2) { /* EPOLL_CTL_DEL */
        for (int i = 0; i < ep->nwatches; i++) {
            if (ep->watches[i].fd == fd) {
                ep->watches[i] = ep->watches[ep->nwatches - 1];
                ep->nwatches--;
                return 0;
            }
        }
        return (i64)ENOENT;
    }
    return (i64)EINVAL;
}

/* helper: check if a watched fd is ready */
static u32 epoll_check_fd(int fd) {
    if (fd < 0 || fd >= SOCKET_MAX || socket_table[fd].type == 0) return 0;
    socket_t *s = &socket_table[fd];
    u32 ready = 0;
    if (s->recv_len > 0 || s->state == TCP_CLOSE_WAIT)
        ready |= 0x00000001u; /* EPOLLIN */
    if (s->state == TCP_ESTABLISHED)
        ready |= 0x00000004u; /* EPOLLOUT */
    if (s->state == TCP_CLOSED && s->type != 0)
        ready |= 0x00000010u; /* EPOLLHUP */
    return ready;
}

/* ── 232: sys_epoll_wait ──────────────────────────────────────── */
i64 sys_epoll_wait(int epfd, epoll_event_t *evs, int maxevents, int timeout_ms) {
    int idx = epfd - EPOLL_FD_BASE;
    if (idx < 0 || idx >= EPOLL_MAX_INSTANCES || !epoll_table[idx].used)
        return (i64)EBADF;
    if (!evs || maxevents <= 0) return (i64)EINVAL;
    epoll_inst_t *ep = &epoll_table[idx];

    /* how many loops: timeout_ms<0 = infinite (cap at 10M), 0 = nonblock */
    int loops = (timeout_ms < 0) ? 10000000 : (timeout_ms == 0 ? 1 : timeout_ms * 1000);

    for (int iter = 0; iter < loops; iter++) {
        int n = 0;
        for (int i = 0; i < ep->nwatches && n < maxevents; i++) {
            u32 ready = epoll_check_fd(ep->watches[i].fd);
            u32 fired = ready & ep->watches[i].events;
            if (fired) {
                evs[n].events = fired;
                evs[n].data   = ep->watches[i].data;
                n++;
            }
        }
        if (n > 0) return n;
        if (timeout_ms == 0) return 0;
        __asm__ volatile("pause");
        if ((iter & 1023) == 0) watchdog_pet();
    }
    return 0; /* timeout */
}

/* ── 7: sys_poll ──────────────────────────────────────────────── */
i64 sys_poll(pollfd_t *fds, u64 nfds, int timeout_ms) {
    if (!fds || nfds == 0) return 0;

    int loops = (timeout_ms < 0) ? 10000000 : (timeout_ms == 0 ? 1 : timeout_ms * 1000);

    for (int iter = 0; iter < loops; iter++) {
        int n = 0;
        for (u64 i = 0; i < nfds; i++) {
            fds[i].revents = 0;
            if (fds[i].fd < 0) continue;
            u32 ready = epoll_check_fd(fds[i].fd);
            short r = 0;
            if ((fds[i].events & 0x0001) && (ready & 0x1)) r |= 0x0001; /* POLLIN  */
            if ((fds[i].events & 0x0004) && (ready & 0x4)) r |= 0x0004; /* POLLOUT */
            if (ready & 0x8)  r |= 0x0008; /* POLLERR */
            if (ready & 0x10) r |= 0x0010; /* POLLHUP */
            fds[i].revents = r;
            if (r) n++;
        }
        if (n > 0) return n;
        if (timeout_ms == 0) return 0;
        __asm__ volatile("pause");
        if ((iter & 1023) == 0) watchdog_pet();
    }
    return 0;
}

/* ── 23: sys_select ───────────────────────────────────────────── */
/* fd_set: 128 bytes (1024 bits) matching Linux ABI */
typedef struct { u64 bits[16]; } kernel_fd_set;

static inline int fdset_isset(kernel_fd_set *s, int fd) {
    if (!s || fd < 0 || fd >= 1024) return 0;
    return (s->bits[fd / 64] >> (fd % 64)) & 1;
}
static inline void fdset_set(kernel_fd_set *s, int fd) {
    if (!s || fd < 0 || fd >= 1024) return;
    s->bits[fd / 64] |= (1ull << (fd % 64));
}
static inline void fdset_clr(kernel_fd_set *s, int fd) {
    if (!s || fd < 0 || fd >= 1024) return;
    s->bits[fd / 64] &= ~(1ull << (fd % 64));
}

i64 sys_select(int nfds, void *rfds_v, void *wfds_v, void *efds_v, void *tv_v) {
    kernel_fd_set *rfds = (kernel_fd_set*)rfds_v;
    kernel_fd_set *wfds = (kernel_fd_set*)wfds_v;
    kernel_fd_set *efds = (kernel_fd_set*)efds_v;
    (void)efds;

    /* timeout: NULL = infinite (cap), {0,0} = nonblock */
    int loops = 10000000;
    if (tv_v) {
        u64 *tv = (u64*)tv_v;
        u64 sec = tv[0], usec = tv[1];
        u64 total_us = sec * 1000000 + usec;
        loops = (int)(total_us > 0 ? total_us : 1);
    }

    /* working copies to return ready sets */
    kernel_fd_set out_r = {0}, out_w = {0};

    for (int iter = 0; iter < loops; iter++) {
        int n = 0;
        for (int fd = 0; fd < nfds && fd < SOCKET_MAX; fd++) {
            u32 ready = epoll_check_fd(fd);
            if (rfds && fdset_isset(rfds, fd) && (ready & 0x1)) {
                fdset_set(&out_r, fd); n++;
            }
            if (wfds && fdset_isset(wfds, fd) && (ready & 0x4)) {
                fdset_set(&out_w, fd); n++;
            }
        }
        if (n > 0) {
            if (rfds) *rfds = out_r;
            if (wfds) *wfds = out_w;
            return n;
        }
        if (tv_v) {
            u64 *tv = (u64*)tv_v;
            if (tv[0] == 0 && tv[1] == 0) return 0; /* nonblock */
        }
        __asm__ volatile("pause");
        if ((iter & 1023) == 0) watchdog_pet();
    }
    if (rfds) memset(rfds->bits, 0, sizeof(rfds->bits));
    if (wfds) memset(wfds->bits, 0, sizeof(wfds->bits));
    return 0;
}
