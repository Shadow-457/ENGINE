/* ================================================================
 *  SoftTail OS Browser — browser/net.h
 *  HTTP/HTTPS fetch layer (Phase 1 - already exists in kernel,
 *  this is the user-space wrapper that calls SoftTail OS syscalls)
 * ================================================================ */
#pragma once
#include "../user/libc.h"
#include "../user/tls.h"

#define NET_BUF_SIZE   (512 * 1024)   /* 512 KB response buffer */
#define NET_HDR_MAX    4096

/* ── URL parser ──────────────────────────────────────────────── */
typedef struct {
    char scheme[8];    /* "http" or "https" */
    char host[256];
    int  port;
    char path[1024];
} ParsedUrl;

static inline int url_parse(const char *url, ParsedUrl *out) {
    /* defaults */
    out->port = 80;
    out->path[0] = '/'; out->path[1] = 0;

    const char *p = url;

    /* scheme */
    int si = 0;
    while (*p && *p != ':' && si < 7) out->scheme[si++] = *p++;
    out->scheme[si] = 0;

    if (p[0]==':'&&p[1]=='/'&&p[2]=='/') {
        p += 3;
    } else return -1;

    if (out->scheme[0]=='h'&&out->scheme[4]=='s') out->port = 443;

    /* host */
    int hi = 0;
    while (*p && *p != '/' && *p != ':' && hi < 255) out->host[hi++] = *p++;
    out->host[hi] = 0;

    /* port override */
    if (*p == ':') {
        p++;
        int port = 0;
        while (*p>='0'&&*p<='9') { port=port*10+(*p-'0'); p++; }
        out->port = port;
    }

    /* path */
    if (*p == '/') {
        int pi = 0;
        while (*p && pi < 1023) out->path[pi++] = *p++;
        out->path[pi] = 0;
    }

    return 0;
}

/* ── DNS resolve via SoftTail OS syscall 333 (sys_net_dns) ─────── */
#define SYS_NET_DNS   333

static inline unsigned int browser_dns_resolve(const char *host) {
    long r;
    __asm__ volatile("syscall"
        : "=a"(r)
        : "0"((long)SYS_NET_DNS), "D"((long)host)
        : "rcx","r11","memory");
    return (unsigned int)r;
}

/* ── TCP connect via SoftTail OS syscall ───────────────────────── */
#define SYS_SOCKET   41
#define SYS_CONNECT  42
#define AF_INET      2
#define SOCK_STREAM  1

typedef struct {
    unsigned short sa_family;
    unsigned short sin_port;   /* big-endian */
    unsigned int   sin_addr;
    unsigned char  pad[8];
} SockAddrIn;

static inline int browser_tcp_connect(unsigned int ip, int port) {
    /* socket(AF_INET, SOCK_STREAM, 0) */
    long fd;
    __asm__ volatile("syscall"
        : "=a"(fd)
        : "0"((long)SYS_SOCKET),
          "D"((long)AF_INET),
          "S"((long)SOCK_STREAM),
          "d"((long)0)
        : "rcx","r11","memory");
    if (fd < 0) return -1;

    SockAddrIn addr;
    addr.sa_family = AF_INET;
    addr.sin_port  = (unsigned short)((port>>8)|((port&0xFF)<<8)); /* htons */
    addr.sin_addr  = ip;
    for (int i=0;i<8;i++) addr.pad[i]=0;

    register long _len __asm__("r10") = (long)sizeof(SockAddrIn);
    long r;
    __asm__ volatile("syscall"
        : "=a"(r)
        : "0"((long)SYS_CONNECT),
          "D"(fd),
          "S"((long)&addr),
          "r"(_len)
        : "rcx","r11","memory");
    if (r < 0) { /* close(fd) */ return -1; }
    return (int)fd;
}

/* ── HTTP fetch ──────────────────────────────────────────────── */
/* Returns number of bytes written to buf, or -1 on error.
 * Strips HTTP headers — buf contains only the response body. */
static inline int http_fetch(const char *host, int port, const char *path,
                              char *buf, int bufsz) {
    unsigned int ip = browser_dns_resolve(host);
    if (!ip) return -1;

    int fd = browser_tcp_connect(ip, port);
    if (fd < 0) return -1;

    /* Build request */
    char req[1024];
    int ri = 0;
    /* GET {path} HTTP/1.0\r\n */
    const char *method = "GET ";
    for (int i=0;method[i];i++) req[ri++]=method[i];
    for (int i=0;path[i]&&ri<900;i++) req[ri++]=path[i];
    const char *ver = " HTTP/1.0\r\nHost: ";
    for (int i=0;ver[i];i++) req[ri++]=ver[i];
    for (int i=0;host[i]&&ri<980;i++) req[ri++]=host[i];
    const char *end = "\r\nUser-Agent: SoftTail-Browser/1.0\r\nAccept: text/html\r\nConnection: close\r\n\r\n";
    for (int i=0;end[i]&&ri<1023;i++) req[ri++]=end[i];
    req[ri]=0;

    /* Send */
    write(fd, req, (size_t)ri);

    /* Read response */
    char *raw = (char*)malloc(bufsz + NET_HDR_MAX);
    if (!raw) { close(fd); return -1; }

    int total = 0;
    int n;
    while ((n = (int)read(fd, raw+total, (size_t)(bufsz+NET_HDR_MAX-1-total))) > 0)
        total += n;
    raw[total] = 0;
    close(fd);

    /* Strip HTTP headers (find \r\n\r\n) */
    int body_start = 0;
    for (int i = 0; i < total-3; i++) {
        if (raw[i]=='\r'&&raw[i+1]=='\n'&&raw[i+2]=='\r'&&raw[i+3]=='\n') {
            body_start = i+4; break;
        }
    }

    int body_len = total - body_start;
    if (body_len > bufsz-1) body_len = bufsz-1;
    for (int i = 0; i < body_len; i++) buf[i] = raw[body_start+i];
    buf[body_len] = 0;

    free(raw);
    return body_len;
}

/* ── IP → dotted-decimal helper ─────────────────────────────── */
static inline void ip_to_str(unsigned int ip_net, char *out) {
    /* ip_net is big-endian (network order) from net_dns_resolve    */
    unsigned char *b = (unsigned char *)&ip_net;
    int si = 0;
    for (int oct = 0; oct < 4; oct++) {
        unsigned char v = b[oct];
        if (v >= 100) { out[si++] = '0' + v/100; v = (unsigned char)(v%100); }
        if (v >= 10)  { out[si++] = '0' + v/10;  v = (unsigned char)(v%10);  }
        out[si++] = '0' + v;
        if (oct < 3) out[si++] = '.';
    }
    out[si] = 0;
}

/* ── HTTPS fetch (uses user/tls.h) ───────────────────────────── */
static inline int https_fetch(const char *host, int port, const char *path,
                               char *buf, int bufsz) {
    unsigned int ip = browser_dns_resolve(host);
    if (!ip) return -1;

    /* Convert IP to dotted-decimal for tls_connect */
    char ip_str[16];
    ip_to_str(ip, ip_str);

    /* tls_connect opens the TCP socket internally */
    TlsCtx tls;
    if (tls_connect(&tls, ip_str, port, host) < 0) return -1;

    /* Build HTTP/1.0 request */
    char req[1024]; int ri = 0;
    const char *method = "GET ";
    for (int i=0;method[i];i++) req[ri++]=method[i];
    for (int i=0;path[i]&&ri<900;i++) req[ri++]=path[i];
    const char *ver = " HTTP/1.0\r\nHost: ";
    for (int i=0;ver[i];i++) req[ri++]=ver[i];
    for (int i=0;host[i]&&ri<980;i++) req[ri++]=host[i];
    const char *end = "\r\nUser-Agent: SoftTail-Browser/1.0\r\nAccept: text/html\r\nConnection: close\r\n\r\n";
    for (int i=0;end[i]&&ri<1023;i++) req[ri++]=end[i];

    tls_write(&tls, (unsigned char*)req, (unsigned int)ri);

    char *raw = (char*)malloc(bufsz + NET_HDR_MAX);
    if (!raw) { tls_close(&tls); return -1; }

    int total = 0;
    unsigned char tbuf[4096]; int n;
    while ((n = tls_read(&tls, tbuf, sizeof(tbuf))) > 0) {
        for (int i=0;i<n&&total<bufsz+NET_HDR_MAX-1;i++) raw[total++]=(char)tbuf[i];
    }
    raw[total] = 0;
    tls_close(&tls);

    /* Strip HTTP headers */
    int body_start = 0;
    for (int i = 0; i < total-3; i++) {
        if (raw[i]=='\r'&&raw[i+1]=='\n'&&raw[i+2]=='\r'&&raw[i+3]=='\n') {
            body_start = i+4; break;
        }
    }
    int body_len = total - body_start;
    if (body_len > bufsz-1) body_len = bufsz-1;
    for (int i = 0; i < body_len; i++) buf[i] = raw[body_start+i];
    buf[body_len] = 0;
    free(raw);
    return body_len;
}

/* ── Unified fetch ───────────────────────────────────────────── */
static inline int browser_fetch(const char *url, char *buf, int bufsz) {
    ParsedUrl pu;
    if (url_parse(url, &pu) < 0) return -1;

    if (pu.scheme[0]=='h'&&pu.scheme[4]=='s')
        return https_fetch(pu.host, pu.port, pu.path, buf, bufsz);
    else
        return http_fetch(pu.host, pu.port, pu.path, buf, bufsz);
}
