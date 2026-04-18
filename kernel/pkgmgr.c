#include "../include/kernel.h"

#define PKG_MAX 64
#define PKG_NAME_MAX 32
#define PKG_DESC_MAX 128
#define PKG_URL_MAX 256

typedef struct {
    char name[PKG_NAME_MAX];
    char desc[PKG_DESC_MAX];
    char url[PKG_URL_MAX];
    u32 size;
    int installed;
} package_t;

static package_t packages[PKG_MAX];
static int pkg_count = 0;

void pkg_init(void) {
    memset(packages, 0, sizeof(packages));
    pkg_count = 0;
}

void pkg_add(const char *name, const char *desc, const char *url, u32 size) {
    if (pkg_count >= PKG_MAX) return;
    package_t *p = &packages[pkg_count];
    usize nlen = strlen(name);
    if (nlen >= PKG_NAME_MAX) nlen = PKG_NAME_MAX - 1;
    memcpy(p->name, name, nlen);
    usize dlen = strlen(desc);
    if (dlen >= PKG_DESC_MAX) dlen = PKG_DESC_MAX - 1;
    memcpy(p->desc, desc, dlen);
    usize ulen = strlen(url);
    if (ulen >= PKG_URL_MAX) ulen = PKG_URL_MAX - 1;
    memcpy(p->url, url, ulen);
    p->size = size;
    p->installed = 0;
    pkg_count++;
}

void pkg_list(void) {
    print_str("Available packages:\r\n");
    print_str("NAME            SIZE    INSTALLED  DESCRIPTION\r\n");
    for (int i = 0; i < pkg_count; i++) {
        print_str(packages[i].name);
        for (int j = strlen(packages[i].name); j < 16; j++) print_str(" ");
        print_hex_byte((u8)(packages[i].size >> 8));
        print_hex_byte((u8)packages[i].size);
        print_str("   ");
        print_str(packages[i].installed ? "YES" : "NO ");
        print_str("   ");
        print_str(packages[i].desc);
        print_str("\r\n");
    }
}

i64 pkg_install(const char *name) {
    for (int i = 0; i < pkg_count; i++) {
        if (strcmp(packages[i].name, name) == 0) {
            if (packages[i].installed) {
                print_str("Package already installed\r\n");
                return 0;
            }
            print_str("Downloading: ");
            print_str(packages[i].url);
            print_str("\r\n");
            print_str("Size: ");
            print_hex_byte((u8)(packages[i].size >> 8));
            print_hex_byte((u8)packages[i].size);
            print_str(" bytes\r\n");
            u32 ip = net_dns_resolve("packages.engineos.org");
            if (ip == 0) {
                print_str("DNS resolution failed\r\n");
                return (i64)ENETUNREACH;
            }
            int sock = (int)sys_socket(2, 1, 0);
            if (sock < 0) return (i64)ENOMEM;
            u8 addr[16];
            memset(addr, 0, 16);
            addr[0] = 2;
            addr[1] = 16;
            addr[2] = (u8)(80 >> 8);
            addr[3] = (u8)80;
            *(u32*)(addr + 4) = ip;
            i64 r = sys_connect(sock, addr, 16);
            if (r < 0) { sys_close_socket(sock); return r; }
            char req[512];
            int pos = 0;
            const char *get = "GET /";
            for (int j = 0; get[j]; j++) req[pos++] = get[j];
            for (int j = 0; packages[i].url[j]; j++) {
                if (packages[i].url[j] == '/' && packages[i].url[j-1] == '/' && packages[i].url[j-2] == ':') {
                    while (packages[i].url[j] && packages[i].url[j] != '/') j++;
                }
                if (j < PKG_URL_MAX && packages[i].url[j]) req[pos++] = packages[i].url[j];
            }
            const char *http = " HTTP/1.0\r\nHost: packages.engineos.org\r\n\r\n";
            for (int j = 0; http[j]; j++) req[pos++] = http[j];
            req[pos] = 0;
            sys_send(sock, req, pos, 0);
            char buf[512];
            i64 total = 0;
            while ((r = sys_recv(sock, buf, 512, 0)) > 0) {
                total += r;
            }
            sys_close_socket(sock);
            packages[i].installed = 1;
            print_str("Installed: ");
            print_str(name);
            print_str(" (");
            print_hex_byte((u8)(total >> 8));
            print_hex_byte((u8)total);
            print_str(" bytes)\r\n");
            return 0;
        }
    }
    print_str("Package not found: ");
    print_str(name);
    print_str("\r\n");
    return (i64)ENOENT;
}

i64 pkg_remove(const char *name) {
    for (int i = 0; i < pkg_count; i++) {
        if (strcmp(packages[i].name, name) == 0) {
            if (!packages[i].installed) {
                print_str("Package not installed\r\n");
                return (i64)ENOENT;
            }
            packages[i].installed = 0;
            print_str("Removed: ");
            print_str(name);
            print_str("\r\n");
            return 0;
        }
    }
    return (i64)ENOENT;
}

i64 sys_pkg_install(const char *name) { return pkg_install(name); }
i64 sys_pkg_remove(const char *name) { return pkg_remove(name); }
void sys_pkg_list(void) { pkg_list(); }
