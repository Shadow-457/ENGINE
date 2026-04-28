/* ================================================================
 *  kernel/pngview.c
 *  Kernel-side PNG decoder + photo viewer command.
 *
 *  Supports PNGs written by our user-space png_encode():
 *    - RGB 8-bit colour depth (IHDR colour type 2)
 *    - Deflate-compressed IDAT using ONLY uncompressed blocks
 *      (BTYPE=00), which is what our encoder produces.
 *    - Multiple IDAT chunks are concatenated transparently.
 *    - PNG filter types 0–4 on each scanline.
 *
 *  Usage:  photo <file.png>
 *  ESC     → return to CLI
 *
 *  The image is centred on the framebuffer with a dark border.
 *  If the framebuffer is not active the command prints an error.
 * ================================================================ */

#include "../include/kernel.h"

/* ── forward declarations for kernel helpers used here ─────────── */
extern void kprintf(const char *fmt, ...) __attribute__((format(printf,1,2)));
extern void *heap_malloc(usize n);
extern void  heap_free(void *p);
extern i64   vfs_open(const char *path);
extern i64   vfs_read(u64 fd, void *buf, usize n);
extern i64   vfs_close(u64 fd);
extern char  cwd_path[128];           /* from kernel.c */
extern int   slibc_path_join(char *out, usize outsz, const char *base, const char *rel);
extern int   fb_is_enabled(void);
extern void  fb_enable(void);
extern void  fb_disable(void);
extern int   fb_get_width(void);
extern int   fb_get_height(void);
extern void  fb_put_pixel(int x, int y, u32 color);
extern void  fb_fill_rect(int x, int y, int w, int h, u32 color);
extern u8    inb(u16 port);
extern void  outb(u16 port, u8 val);
extern void  vga_clear(void);

/* ── small memory helpers (avoid pulling in full libc here) ─────── */
static void pv_memset(void *dst, int c, usize n) {
    u8 *p = (u8 *)dst; while (n--) *p++ = (u8)c;
}
static void pv_memcpy(void *dst, const void *src, usize n) {
    u8 *d = (u8 *)dst; const u8 *s = (const u8 *)src; while (n--) *d++ = *s++;
}

/* ── PNG signature & chunk reading ─────────────────────────────── */
#define PNG_SIG0  0x89
#define PNG_SIG1  0x50   /* 'P' */

static u32 pv_be32(const u8 *p) {
    return ((u32)p[0]<<24)|((u32)p[1]<<16)|((u32)p[2]<<8)|(u32)p[3];
}

/* ── Raw file buffer ─────────────────────────────────────────────
 * We read the whole PNG into memory once (up to 32 MB).          */
#define PNG_MAX_FILE  (32*1024*1024)
#define PNG_MAX_DIM   4096

/* ── Deflate uncompressed block decoder ─────────────────────────
 *
 * Format of a zlib stream wrapping IDAT:
 *   2-byte zlib header (CMF, FLG) — skip
 *   one or more deflate blocks:
 *     1 bit  BFINAL
 *     2 bits BTYPE  (00 = uncompressed, 01/10 = compressed)
 *     if BTYPE==00:
 *       skip to byte boundary
 *       LEN  (2 bytes LE)
 *       NLEN (2 bytes LE, one's complement of LEN)
 *       LEN bytes of literal data
 *   4-byte Adler-32 checksum — skip
 *
 * We only handle BTYPE==00 blocks (our encoder never produces others).
 */
typedef struct {
    const u8 *data;
    usize      len;
    usize      pos;
    int        bit_pos;    /* bit offset within current byte, 0..7 */
} BitReader;

static void br_init(BitReader *br, const u8 *data, usize len) {
    br->data = data; br->len = len; br->pos = 0; br->bit_pos = 0;
}

/* Skip remaining bits in current byte to align to byte boundary */
static void br_align(BitReader *br) {
    if (br->bit_pos) { br->pos++; br->bit_pos = 0; }
}

/* Read one bit */
static int br_bit(BitReader *br) {
    if (br->pos >= br->len) return 0;
    int b = (br->data[br->pos] >> br->bit_pos) & 1;
    if (++br->bit_pos == 8) { br->bit_pos = 0; br->pos++; }
    return b;
}

/* Read n bits (LSB first) */
static u32 br_bits(BitReader *br, int n) {
    u32 v = 0;
    for (int i = 0; i < n; i++) v |= (u32)br_bit(br) << i;
    return v;
}

/* Read one byte (must be byte-aligned) */
static u8 br_byte(BitReader *br) {
    if (br->pos >= br->len) return 0;
    return br->data[br->pos++];
}

/* Decompress zlib/deflate stream (uncompressed blocks only).
 * out     : output buffer
 * out_max : maximum bytes to write
 * Returns number of bytes written, or -1 on error. */
static i64 deflate_uncompressed(const u8 *zdata, usize zlen,
                                 u8 *out, usize out_max) {
    BitReader br;
    br_init(&br, zdata, zlen);

    /* Skip 2-byte zlib header */
    if (zlen < 2) return -1;
    br.pos = 2;

    usize out_pos = 0;

    for (;;) {
        int bfinal = br_bit(&br);
        int btype  = (int)br_bits(&br, 2);

        if (btype == 0) {
            /* Uncompressed block */
            br_align(&br);
            if (br.pos + 4 > br.len) return -1;
            u16 len  = (u16)(br.data[br.pos] | ((u16)br.data[br.pos+1] << 8));
            u16 nlen = (u16)(br.data[br.pos+2] | ((u16)br.data[br.pos+3] << 8));
            br.pos += 4;
            if ((u16)(len ^ nlen) != 0xFFFF) return -1; /* corrupt */
            if (br.pos + len > br.len) return -1;
            if (out_pos + len > out_max) len = (u16)(out_max - out_pos);
            pv_memcpy(out + out_pos, br.data + br.pos, len);
            br.pos   += len;
            out_pos  += len;
        } else {
            /* Compressed block — we don't support these */
            return -1;
        }

        if (bfinal) break;
    }
    return (i64)out_pos;
}

/* ── PNG filter reconstruction ──────────────────────────────────
 * Applied scanline-by-scanline after decompression.
 * Pixels are 3 bytes (RGB).                                      */
#define PV_BPP 3   /* bytes per pixel: RGB */

static u8 pae_abs(int x) { return (u8)(x < 0 ? -x : x); }

static u8 paeth(u8 a, u8 b, u8 c) {
    int p  = (int)a + (int)b - (int)c;
    int pa = pae_abs(p - (int)a);
    int pb = pae_abs(p - (int)b);
    int pc = pae_abs(p - (int)c);
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc)             return b;
    return c;
}

/* Reconstruct one scanline.
 * raw     : filtered scanline data (stride bytes, first byte is filter type)
 * prev    : previous reconstructed scanline (or NULL for first line)
 * out     : output buffer for this scanline (stride bytes, no filter byte)
 * stride  : width * PV_BPP
 */
static void png_unfilter(const u8 *raw, const u8 *prev, u8 *out, u32 stride) {
    u8 ftype = raw[0];
    raw++;   /* skip filter byte */

    switch (ftype) {
    case 0:  /* None */
        pv_memcpy(out, raw, stride);
        break;
    case 1:  /* Sub */
        for (u32 i = 0; i < stride; i++)
            out[i] = raw[i] + (i >= PV_BPP ? out[i - PV_BPP] : 0);
        break;
    case 2:  /* Up */
        for (u32 i = 0; i < stride; i++)
            out[i] = raw[i] + (prev ? prev[i] : 0);
        break;
    case 3:  /* Average */
        for (u32 i = 0; i < stride; i++) {
            u8 a = (i >= PV_BPP) ? out[i - PV_BPP] : 0;
            u8 b = prev ? prev[i] : 0;
            out[i] = raw[i] + (u8)(((u32)a + b) >> 1);
        }
        break;
    case 4:  /* Paeth */
        for (u32 i = 0; i < stride; i++) {
            u8 a = (i >= PV_BPP) ? out[i - PV_BPP] : 0;
            u8 b = prev ? prev[i] : 0;
            u8 c = (prev && i >= PV_BPP) ? prev[i - PV_BPP] : 0;
            out[i] = raw[i] + paeth(a, b, c);
        }
        break;
    default:
        pv_memcpy(out, raw, stride); /* unknown — treat as None */
        break;
    }
}

/* ── Draw a text label in the viewer using VGA characters ───────
 * Falls back to kprintf (which outputs to VGA text buffer behind  *
 * the framebuffer) — instead we draw directly into the fb using   *
 * fb_put_pixel with a simple 5×7 inline font for the status bar.  *
 *                                                                  *
 * We use an ultra-minimal 5×7 proportional bitmap font for the    *
 * "Press ESC to return" hint at the bottom of the screen.         */

/* 5-wide × 7-tall bitmaps for digits 0–9, letters, space, . */
static const u8 mini_font[128][7] = {
    /* We only need printable ASCII 0x20–0x7E; rest are zeroed.
     * Each entry: 7 rows, each row a 5-bit mask (bit4=leftmost). */
    [' ']  = {0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    ['!']  = {0x04,0x04,0x04,0x04,0x00,0x04,0x00},
    ['.']  = {0x00,0x00,0x00,0x00,0x00,0x04,0x00},
    ['<']  = {0x02,0x04,0x08,0x04,0x02,0x00,0x00},
    ['>']  = {0x08,0x04,0x02,0x04,0x08,0x00,0x00},
    ['E']  = {0x1F,0x10,0x1E,0x10,0x10,0x1F,0x00},
    ['S']  = {0x0F,0x10,0x0E,0x01,0x01,0x1E,0x00},
    ['C']  = {0x0F,0x10,0x10,0x10,0x10,0x0F,0x00},
    ['P']  = {0x1E,0x11,0x1E,0x10,0x10,0x10,0x00},
    ['r']  = {0x00,0x16,0x19,0x10,0x10,0x10,0x00},
    ['e']  = {0x00,0x0E,0x11,0x1F,0x10,0x0F,0x00},
    ['s']  = {0x00,0x0F,0x10,0x0E,0x01,0x1E,0x00},
    ['t']  = {0x04,0x1F,0x04,0x04,0x04,0x03,0x00},
    ['o']  = {0x00,0x0E,0x11,0x11,0x11,0x0E,0x00},
    ['u']  = {0x00,0x11,0x11,0x11,0x13,0x0D,0x00},
    ['n']  = {0x00,0x16,0x19,0x11,0x11,0x11,0x00},
    ['R']  = {0x1E,0x11,0x1E,0x14,0x12,0x11,0x00},
    ['T']  = {0x1F,0x04,0x04,0x04,0x04,0x04,0x00},
    ['a']  = {0x00,0x0E,0x01,0x0F,0x11,0x0F,0x00},
    ['b']  = {0x10,0x10,0x1E,0x11,0x11,0x1E,0x00},
    ['c']  = {0x00,0x0F,0x10,0x10,0x10,0x0F,0x00},
    ['d']  = {0x01,0x01,0x0F,0x11,0x11,0x0F,0x00},
    ['f']  = {0x03,0x04,0x0E,0x04,0x04,0x04,0x00},
    ['g']  = {0x00,0x0F,0x11,0x0F,0x01,0x0E,0x00},
    ['h']  = {0x10,0x10,0x16,0x19,0x11,0x11,0x00},
    ['i']  = {0x00,0x04,0x00,0x04,0x04,0x04,0x00},
    ['k']  = {0x10,0x12,0x14,0x18,0x14,0x12,0x00},
    ['l']  = {0x04,0x04,0x04,0x04,0x04,0x02,0x00},
    ['m']  = {0x00,0x1B,0x15,0x15,0x11,0x11,0x00},
    ['p']  = {0x00,0x1E,0x11,0x1E,0x10,0x10,0x00},
    ['q']  = {0x00,0x0F,0x11,0x0F,0x01,0x01,0x00},
    ['v']  = {0x00,0x11,0x11,0x11,0x0A,0x04,0x00},
    ['w']  = {0x00,0x11,0x11,0x15,0x1B,0x11,0x00},
    ['x']  = {0x00,0x11,0x0A,0x04,0x0A,0x11,0x00},
    ['y']  = {0x00,0x11,0x0A,0x04,0x04,0x04,0x00},
    ['z']  = {0x00,0x1F,0x02,0x04,0x08,0x1F,0x00},
};

static void pv_draw_char(int x, int y, char c, u32 color) {
    if ((u8)c >= 128) return;
    const u8 *glyph = mini_font[(u8)c];
    for (int row = 0; row < 7; row++) {
        u8 bits = glyph[row];
        for (int col = 0; col < 5; col++) {
            if (bits & (0x10 >> col))
                fb_put_pixel(x + col, y + row, color);
        }
    }
}

static void pv_draw_str(int x, int y, const char *s, u32 color) {
    while (*s) { pv_draw_char(x, y, *s++, color); x += 6; }
}

/* ── Wait for ESC key (scancode 0x01) ──────────────────────────── */
static void pv_wait_esc(void) {
    /* Drain any pending bytes first */
    for (int i = 0; i < 200; i++) {
        if (inb(0x64) & 1) inb(0x60);
    }
    for (;;) {
        if (inb(0x64) & 1) {
            u8 b = inb(0x60);
            /* Skip mouse bytes (bit 5 of status would tell us, but we
             * already drained status above; just ignore non-0x01 bytes
             * and break on 0x01 = ESC make) */
            if (b == 0x01) return;
        }
        /* Tiny spin — no timer available here outside GUI */
        for (volatile int s = 0; s < 2000; s++) __asm__ volatile("pause");
    }
}

/* ── Main photo command ─────────────────────────────────────────── */
void cmd_photo(const char *arg) {
    if (!arg || !arg[0]) {
        kprintf("Usage: photo <file.png>\r\n");
        return;
    }

    int we_enabled_fb = 0;
    if (!fb_is_enabled()) {
        kprintf("photo: enabling framebuffer...\r\n");
        fb_enable();
        if (!fb_is_enabled()) {
            kprintf("photo: framebuffer not available on this display.\r\n");
            return;
        }
        we_enabled_fb = 1;
    }

    /* Build full path */
    char path[256];
    if (slibc_path_join(path, sizeof(path), cwd_path, arg) < 0) {
        kprintf("photo: path too long\r\n");
        return;
    }

    /* Open file */
    i64 fd = vfs_open(path);
    if (fd < 0) {
        kprintf("photo: cannot open '%s'\r\n", arg);
        return;
    }

    /* Read entire file */
    u8 *fbuf = (u8 *)heap_malloc(PNG_MAX_FILE);
    if (!fbuf) { vfs_close((u64)fd); kprintf("photo: out of memory\r\n"); return; }

    usize flen = 0;
    for (;;) {
        i64 n = vfs_read((u64)fd, fbuf + flen, 4096);
        if (n <= 0) break;
        flen += (usize)n;
        if (flen >= PNG_MAX_FILE) break;
    }
    vfs_close((u64)fd);

    /* Validate PNG signature */
    if (flen < 8 || fbuf[0] != PNG_SIG0 || fbuf[1] != PNG_SIG1) {
        kprintf("photo: not a valid PNG file\r\n");
        heap_free(fbuf);
        return;
    }

    /* Parse chunks */
    u32 img_w = 0, img_h = 0;
    u8  bit_depth = 0, color_type = 0;
    int got_ihdr = 0;

    /* Collect all IDAT data contiguously */
    u8 *idat_buf = NULL;
    usize idat_len = 0;

    usize pos = 8;  /* skip signature */
    while (pos + 12 <= flen) {
        u32 chunk_len  = pv_be32(fbuf + pos);
        u8 *chunk_type = fbuf + pos + 4;
        u8 *chunk_data = fbuf + pos + 8;

        if (chunk_len > flen - pos - 12) break; /* corrupt */

        if (chunk_type[0]=='I' && chunk_type[1]=='H' && chunk_type[2]=='D' && chunk_type[3]=='R') {
            if (chunk_len < 13) break;
            img_w      = pv_be32(chunk_data);
            img_h      = pv_be32(chunk_data + 4);
            bit_depth  = chunk_data[8];
            color_type = chunk_data[9];
            got_ihdr   = 1;
        } else if (chunk_type[0]=='I' && chunk_type[1]=='D' && chunk_type[2]=='A' && chunk_type[3]=='T') {
            /* Accumulate IDAT data */
            if (!idat_buf) {
                idat_buf = (u8 *)heap_malloc(PNG_MAX_FILE);
                if (!idat_buf) { heap_free(fbuf); kprintf("photo: OOM\r\n"); return; }
            }
            if (idat_len + chunk_len <= PNG_MAX_FILE) {
                pv_memcpy(idat_buf + idat_len, chunk_data, chunk_len);
                idat_len += chunk_len;
            }
        } else if (chunk_type[0]=='I' && chunk_type[1]=='E' && chunk_type[2]=='N' && chunk_type[3]=='D') {
            break;
        }

        pos += 12 + chunk_len;
    }

    heap_free(fbuf);

    if (!got_ihdr || !idat_buf) {
        kprintf("photo: missing IHDR or IDAT chunks\r\n");
        if (idat_buf) heap_free(idat_buf);
        return;
    }
    if (bit_depth != 8 || color_type != 2) {
        kprintf("photo: only 8-bit RGB PNGs are supported (got depth=%u type=%u)\r\n",
                (u32)bit_depth, (u32)color_type);
        heap_free(idat_buf);
        return;
    }
    if (img_w == 0 || img_h == 0 || img_w > PNG_MAX_DIM || img_h > PNG_MAX_DIM) {
        kprintf("photo: image dimensions out of range (%ux%u)\r\n", img_w, img_h);
        heap_free(idat_buf);
        return;
    }

    /* Decompress: each scanline is (1 + width*3) bytes */
    u32  stride    = img_w * PV_BPP;
    usize raw_size = (usize)(stride + 1) * img_h;
    u8 *raw = (u8 *)heap_malloc(raw_size);
    if (!raw) {
        kprintf("photo: OOM for decompressed data\r\n");
        heap_free(idat_buf);
        return;
    }

    i64 decompressed = deflate_uncompressed(idat_buf, idat_len, raw, raw_size);
    heap_free(idat_buf);

    if (decompressed < 0 || (usize)decompressed < raw_size) {
        kprintf("photo: decompression failed (got %lld, expected %llu)\r\n",
                decompressed, (u64)raw_size);
        heap_free(raw);
        return;
    }

    /* Unfilter scanlines into pixel buffer */
    u8 *pixels = (u8 *)heap_malloc((usize)stride * img_h);
    if (!pixels) {
        kprintf("photo: OOM for pixel buffer\r\n");
        heap_free(raw);
        return;
    }

    u8 *prev = NULL;
    for (u32 y = 0; y < img_h; y++) {
        u8 *scanline_raw = raw + y * (stride + 1);
        u8 *scanline_out = pixels + y * stride;
        png_unfilter(scanline_raw, prev, scanline_out, stride);
        prev = scanline_out;
    }
    heap_free(raw);

    /* Centre image on framebuffer */
    int scr_w = fb_get_width();
    int scr_h = fb_get_height();
    int off_x = (scr_w - (int)img_w) / 2;
    int off_y = (scr_h - (int)img_h) / 2;

    /* Black background */
    fb_fill_rect(0, 0, scr_w, scr_h, 0x00000000);

    /* Draw image */
    for (u32 y = 0; y < img_h; y++) {
        u8 *row = pixels + y * stride;
        for (u32 x = 0; x < img_w; x++) {
            u8 r = row[x * 3 + 0];
            u8 g = row[x * 3 + 1];
            u8 b = row[x * 3 + 2];
            u32 color = ((u32)r << 16) | ((u32)g << 8) | b;
            fb_put_pixel(off_x + (int)x, off_y + (int)y, color);
        }
    }

    heap_free(pixels);

    /* Status bar at bottom */
    int bar_y = scr_h - 20;
    fb_fill_rect(0, bar_y, scr_w, 20, 0x00222222);
    pv_draw_str(10, bar_y + 6, "Press ESC to return", 0x00CCCCCC);

    /* Wait for ESC, then restore */
    pv_wait_esc();

    if (we_enabled_fb) {
        /* We turned on the fb — disable it and restore VGA text mode */
        fb_disable();
        /* Restore VGA text cursor */
        outb(0x3D4, 0x0A); outb(0x3D5, (inb(0x3D5) & 0xC0) | 14);
        outb(0x3D4, 0x0B); outb(0x3D5, (inb(0x3D5) & 0xE0) | 15);
        vga_clear();
    } else {
        /* GUI was already running — restore its background colour */
        fb_fill_rect(0, 0, scr_w, scr_h, 0x00003380);
    }

    kprintf("Returned to shell.\r\n");
}
