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
} socket_t;

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
    for (int i = 0; i < 6; i++) eth->dst[i] = 0xFF;
    for (int i = 0; i < 6; i++) eth->src[i] = net_mac[i];
    eth->type = 0x0806;
    arp_pkt_t *arp = (arp_pkt_t*)(pkt + ETH_HDR_LEN);
    arp->hw_type = 1;
    arp->proto_type = 0x0800;
    arp->hw_len = 6;
    arp->proto_len = 4;
    arp->op = 1;
    for (int i = 0; i < 6; i++) arp->sender_mac[i] = net_mac[i];
    arp->sender_ip = net_ip;
    arp->target_ip = target_ip;
    extern void net_send_frame(const u8 *data, usize len);
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
        for (int i = 0; i < 6; i++) eth->dst[i] = arp->sender_mac[i];
        for (int i = 0; i < 6; i++) eth->src[i] = net_mac[i];
        eth->type = 0x0806;
        arp_pkt_t *arp_r = (arp_pkt_t*)(reply + ETH_HDR_LEN);
        arp_r->hw_type = 1;
        arp_r->proto_type = 0x0800;
        arp_r->hw_len = 6;
        arp_r->proto_len = 4;
        arp_r->op = 2;
        for (int i = 0; i < 6; i++) arp_r->sender_mac[i] = net_mac[i];
        arp_r->sender_ip = net_ip;
        for (int i = 0; i < 6; i++) arp_r->target_mac[i] = arp->sender_mac[i];
        arp_r->target_ip = arp->sender_ip;
        extern void net_send_frame(const u8 *data, usize len);
        net_send_frame(reply, 42);
    }
}

static void icmp_send(u32 dst_ip, u8 type, u8 code, u16 id, u16 seq, const void *data, usize len) {
    extern u8 net_mac[6];
    extern u32 net_ip;
    u8 pkt[ETH_HDR_LEN + IP_HDR_LEN + ICMP_HDR_LEN + 64];
    usize pkt_len = ETH_HDR_LEN + IP_HDR_LEN + ICMP_HDR_LEN + len;
    eth_hdr_t *eth = (eth_hdr_t*)pkt;
    memset(eth->dst, 0xFF, 6);
    for (int i = 0; i < 6; i++) eth->src[i] = net_mac[i];
    eth->type = 0x0800;
    ip_hdr_t *ip = (ip_hdr_t*)(pkt + ETH_HDR_LEN);
    ip->ver_ihl = 0x45;
    ip->tos = 0;
    ip->total_len = IP_HDR_LEN + ICMP_HDR_LEN + len;
    ip->id = 0;
    ip->frag_off = 0;
    ip->ttl = 64;
    ip->protocol = IP_PROTO_ICMP;
    ip->checksum = 0;
    ip->saddr = net_ip;
    ip->daddr = dst_ip;
    ip->checksum = ip_checksum(ip, IP_HDR_LEN);
    icmp_hdr_t *icmp = (icmp_hdr_t*)(pkt + ETH_HDR_LEN + IP_HDR_LEN);
    icmp->type = type;
    icmp->code = code;
    icmp->checksum = 0;
    icmp->id = id;
    icmp->seq = seq;
    if (data && len > 0) memcpy(icmp + 1, data, len);
    icmp->checksum = ip_checksum(icmp, ICMP_HDR_LEN + len);
    extern void net_send_frame(const u8 *data, usize len);
    net_send_frame(pkt, pkt_len);
}

static void tcp_send(int sock_idx, u8 flags) {
    socket_t *s = &socket_table[sock_idx];
    extern u8 net_mac[6];
    extern u32 net_ip;
    u8 pkt[ETH_HDR_LEN + IP_HDR_LEN + TCP_HDR_LEN + 1500];
    eth_hdr_t *eth = (eth_hdr_t*)pkt;
    memset(eth->dst, 0xFF, 6);
    for (int i = 0; i < 6; i++) eth->src[i] = net_mac[i];
    eth->type = 0x0800;
    ip_hdr_t *ip = (ip_hdr_t*)(pkt + ETH_HDR_LEN);
    ip->ver_ihl = 0x45;
    ip->tos = 0;
    ip->total_len = IP_HDR_LEN + TCP_HDR_LEN;
    ip->id = 0;
    ip->frag_off = 0;
    ip->ttl = 64;
    ip->protocol = IP_PROTO_TCP;
    ip->checksum = 0;
    ip->saddr = net_ip;
    ip->daddr = s->remote_ip;
    ip->checksum = ip_checksum(ip, IP_HDR_LEN);
    tcp_hdr_t *tcp = (tcp_hdr_t*)(pkt + ETH_HDR_LEN + IP_HDR_LEN);
    tcp->sport = s->local_port;
    tcp->dport = s->remote_port;
    tcp->seq = s->seq;
    tcp->ack_seq = s->ack_seq;
    tcp->data_off = (TCP_HDR_LEN / 4) << 4;
    tcp->flags = flags;
    tcp->window = s->window;
    tcp->check = 0;
    tcp->urg_ptr = 0;
    usize total_len = ETH_HDR_LEN + IP_HDR_LEN + TCP_HDR_LEN;
    extern void net_send_frame(const u8 *data, usize len);
    net_send_frame(pkt, total_len);
    if (flags & TCP_SYN) s->seq++;
    if (flags & TCP_ACK) s->seq++;
}

i64 sys_socket(int domain, int type, int protocol) {
    (void)domain; (void)protocol;
    int idx = socket_alloc();
    if (idx < 0) return (i64)ENOMEM;
    socket_table[idx].type = type;
    socket_table[idx].state = TCP_CLOSED;
    socket_table[idx].local_port = alloc_port();
    socket_table[idx].window = 8192;
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
    const u8 *a = (const u8*)addr;
    s->remote_ip = *(u32*)(a + 4);
    s->remote_port = (u16)a[2] << 8 | a[3];
    s->seq = 0x12345678;
    s->state = TCP_SYN_SENT;
    tcp_send(sockfd, TCP_SYN);
    return 0;
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
    return (i64)EAGAIN;
}

i64 sys_send(int sockfd, const void *buf, usize len, int flags) {
    (void)flags;
    socket_t *s = socket_get(sockfd);
    if (!s) return (i64)EBADF;
    if (s->state != TCP_ESTABLISHED) return (i64)ENOTCONN;
    if (len > 1500) len = 1500;
    extern u8 net_mac[6];
    extern u32 net_ip;
    u8 pkt[ETH_HDR_LEN + IP_HDR_LEN + TCP_HDR_LEN + 1500];
    eth_hdr_t *eth = (eth_hdr_t*)pkt;
    memset(eth->dst, 0xFF, 6);
    for (int i = 0; i < 6; i++) eth->src[i] = net_mac[i];
    eth->type = 0x0800;
    ip_hdr_t *ip = (ip_hdr_t*)(pkt + ETH_HDR_LEN);
    ip->ver_ihl = 0x45;
    ip->tos = 0;
    ip->total_len = IP_HDR_LEN + TCP_HDR_LEN + len;
    ip->id = 0;
    ip->frag_off = 0;
    ip->ttl = 64;
    ip->protocol = IP_PROTO_TCP;
    ip->checksum = 0;
    ip->saddr = net_ip;
    ip->daddr = s->remote_ip;
    ip->checksum = ip_checksum(ip, IP_HDR_LEN);
    tcp_hdr_t *tcp = (tcp_hdr_t*)(pkt + ETH_HDR_LEN + IP_HDR_LEN);
    tcp->sport = s->local_port;
    tcp->dport = s->remote_port;
    tcp->seq = s->seq;
    tcp->ack_seq = s->ack_seq;
    tcp->data_off = (TCP_HDR_LEN / 4) << 4;
    tcp->flags = TCP_PSH | TCP_ACK;
    tcp->window = s->window;
    tcp->check = 0;
    tcp->urg_ptr = 0;
    if (buf && len > 0) memcpy(tcp + 1, buf, len);
    usize total_len = ETH_HDR_LEN + IP_HDR_LEN + TCP_HDR_LEN + len;
    extern void net_send_frame(const u8 *data, usize len);
    net_send_frame(pkt, total_len);
    s->seq += len;
    return (i64)len;
}

i64 sys_recv(int sockfd, void *buf, usize len, int flags) {
    (void)flags;
    socket_t *s = socket_get(sockfd);
    if (!s) return (i64)EBADF;
    if (s->state != TCP_ESTABLISHED) return (i64)ENOTCONN;
    if (s->recv_len == 0) return (i64)EAGAIN;
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
