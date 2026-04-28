/* ================================================================
 *  browser/png.c  — PNG encoder for SystrixOS user space
 *
 *  Encodes a 32-bit XRGB (or ARGB) framebuffer as an RGB 8-bit
 *  PNG using uncompressed deflate blocks (BTYPE=00).  The result
 *  is a valid PNG that can be decoded by kernel/pngview.c.
 *
 *  No external dependencies beyond the kernel VFS syscalls.
 * ================================================================ */

#include "png.h"
#include "../user/libc.h"

/* ── helpers ────────────────────────────────────────────────────── */
static void wp32_be(uint8_t *p, uint32_t v) {
    p[0]=(uint8_t)(v>>24); p[1]=(uint8_t)(v>>16);
    p[2]=(uint8_t)(v>>8);  p[3]=(uint8_t)v;
}
static void wp16_le(uint8_t *p, uint16_t v) {
    p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8);
}

/* CRC-32 (PNG uses standard Ethernet polynomial) */
static uint32_t crc32_tab[256];
static int crc_init_done = 0;
static void crc_init(void) {
    if (crc_init_done) return;
    crc_init_done = 1;
    for (int i = 0; i < 256; i++) {
        uint32_t c = (uint32_t)i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc32_tab[i] = c;
    }
}
static uint32_t crc32(const uint8_t *buf, size_t len) {
    crc_init();
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++)
        c = crc32_tab[(c ^ buf[i]) & 0xFF] ^ (c >> 8);
    return ~c;
}

/* Adler-32 (zlib checksum) */
static uint32_t adler32(const uint8_t *buf, size_t len) {
    uint32_t s1 = 1, s2 = 0;
    for (size_t i = 0; i < len; i++) {
        s1 = (s1 + buf[i]) % 65521;
        s2 = (s2 + s1)    % 65521;
    }
    return (s2 << 16) | s1;
}

/* ── write helpers using VFS ────────────────────────────────────── */
typedef struct { int fd; } Writer;

static void wr(Writer *w, const void *buf, size_t len) {
    write(w->fd, (void*)buf, len);
}

static void wr_chunk(Writer *w, const char *type, const uint8_t *data, uint32_t dlen) {
    uint8_t hdr[8];
    wp32_be(hdr,     dlen);
    hdr[4]=(uint8_t)type[0]; hdr[5]=(uint8_t)type[1];
    hdr[6]=(uint8_t)type[2]; hdr[7]=(uint8_t)type[3];
    wr(w, hdr, 8);
    if (dlen && data) wr(w, data, dlen);

    /* CRC covers type + data */
    uint32_t c = crc32(hdr + 4, 4);
    if (dlen && data) {
        /* we need CRC over type||data combined; build it incrementally */
        uint32_t c2 = 0xFFFFFFFFu;
        const uint8_t *tp = hdr + 4;
        for (int i = 0; i < 4; i++)
            c2 = crc32_tab[(c2 ^ tp[i]) & 0xFF] ^ (c2 >> 8);
        for (uint32_t i = 0; i < dlen; i++)
            c2 = crc32_tab[(c2 ^ data[i]) & 0xFF] ^ (c2 >> 8);
        c = ~c2;
    }
    uint8_t crcb[4]; wp32_be(crcb, c); wr(w, crcb, 4);
}

/* ── PNG encoder ────────────────────────────────────────────────── */
/*
 * We emit the IDAT as a raw zlib stream containing only BTYPE=00
 * (uncompressed) deflate blocks.  Deflate limits each block to
 * 65535 bytes; we emit one block per scanline (max width = 21845 px
 * for 3-byte RGB, well within limit).
 *
 * Zlib envelope:
 *   CMF=0x78  (deflate, window 32K)
 *   FLG       must make (CMF*256+FLG) % 31 == 0
 *   <deflate blocks>
 *   Adler-32 (big-endian)
 */

#define MAX_IDAT_BUF (4 * 1024 * 1024)   /* 4 MB intermediate buffer */

int png_encode(const char *filename, uint32_t *fb, uint32_t w, uint32_t h) {
    if (!w || !h || !fb) return -1;

    int fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return -1;

    Writer wr_s = { fd };
    Writer *ww = &wr_s;

    crc_init();

    /* ── PNG signature ── */
    static const uint8_t sig[8] = {0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A};
    wr(ww, sig, 8);

    /* ── IHDR ── */
    {
        uint8_t ihdr[13];
        wp32_be(ihdr,   w);
        wp32_be(ihdr+4, h);
        ihdr[8]  = 8;   /* bit depth */
        ihdr[9]  = 2;   /* colour type: RGB */
        ihdr[10] = 0;   /* compression */
        ihdr[11] = 0;   /* filter */
        ihdr[12] = 0;   /* interlace */
        wr_chunk(ww, "IHDR", ihdr, 13);
    }

    /* ── Build IDAT payload (zlib-wrapped uncompressed deflate) ── */
    /* stride = w * 3 bytes RGB + 1 filter byte per row */
    uint32_t stride = w * 3;
    /* Each uncompressed block: 5-byte header + (1+stride) bytes */
    /* Total body = h rows × (5 + 1 + stride) + 2 (zlib hdr) + 4 (adler) */

    /* We'll write into a heap buffer then emit as one IDAT chunk */
    uint8_t *idat = (uint8_t*)malloc(MAX_IDAT_BUF);
    if (!idat) { close(fd); return -1; }

    uint32_t ip = 0;

    /* Zlib header: CMF=0x78, FLG chosen so (0x78*256+FLG)%31==0 */
    idat[ip++] = 0x78;
    idat[ip++] = 0x01;  /* 0x7801 % 31 == 0 */

    /* Adler-32 accumulator over all raw (filtered) scanline bytes */
    uint32_t adl_s1 = 1, adl_s2 = 0;

    for (uint32_t y = 0; y < h; y++) {
        int is_last = (y == h - 1);
        uint16_t blen = (uint16_t)(1 + stride); /* filter byte + RGB row */

        /* Deflate uncompressed block header */
        idat[ip++] = is_last ? 0x01 : 0x00;  /* BFINAL | BTYPE=00 */
        wp16_le(idat + ip, blen);  ip += 2;
        wp16_le(idat + ip, (uint16_t)~blen); ip += 2;

        /* Filter byte 0 (None) */
        idat[ip++] = 0x00;
        adl_s1 = (adl_s1 + 0) % 65521;
        adl_s2 = (adl_s2 + adl_s1) % 65521;

        /* RGB pixels */
        uint32_t *row = fb + y * w;
        for (uint32_t x = 0; x < w; x++) {
            uint32_t px = row[x];
            uint8_t r = (uint8_t)(px >> 16);
            uint8_t g = (uint8_t)(px >> 8);
            uint8_t b = (uint8_t)(px);

            idat[ip++] = r;
            idat[ip++] = g;
            idat[ip++] = b;

            adl_s1 = (adl_s1 + r) % 65521;
            adl_s2 = (adl_s2 + adl_s1) % 65521;
            adl_s1 = (adl_s1 + g) % 65521;
            adl_s2 = (adl_s2 + adl_s1) % 65521;
            adl_s1 = (adl_s1 + b) % 65521;
            adl_s2 = (adl_s2 + adl_s1) % 65521;

            if (ip + 20 >= MAX_IDAT_BUF) goto overflow;
        }
    }

    /* Zlib Adler-32 checksum (big-endian) */
    {
        uint32_t adler = (adl_s2 << 16) | adl_s1;
        wp32_be(idat + ip, adler); ip += 4;
    }

    wr_chunk(ww, "IDAT", idat, ip);
    goto done;

overflow:
    /* Image too large for our static buffer — write empty IDAT */
    idat[0] = 0x78; idat[1] = 0x01;
    idat[2] = 0x01; /* BFINAL, BTYPE=00 */
    idat[3] = 0x00; idat[4] = 0x00;  /* LEN=0 */
    idat[5] = 0xFF; idat[6] = 0xFF;  /* NLEN */
    /* Adler-32 of empty = 0x00000001 */
    idat[7] = 0x00; idat[8] = 0x00; idat[9] = 0x00; idat[10] = 0x01;
    wr_chunk(ww, "IDAT", idat, 11);

done:
    free(idat);

    /* ── IEND ── */
    wr_chunk(ww, "IEND", NULL, 0);

    close(fd);
    return 0;
}
