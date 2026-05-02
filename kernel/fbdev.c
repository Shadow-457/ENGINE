/* ================================================================
 *  Systrix OS — kernel/fbdev.c
 *  VESA VBE 2.0/3.0 framebuffer driver.
 *  Uses the Bochs/VBE dispi I/O interface (ports 0x01CE/0x01CF)
 *  which QEMU's std VGA emulates. Sets 1024x768x32bpp.
 *  Maps the LFB at physical 0xfd000000 (QEMU default) into
 *  kernel virtual space at 0xA0000000.
 * ================================================================ */
#include "../include/kernel.h"

/* ── VBE / Framebuffer state ─────────────────────────────────── */
#define VBE_DISPI_ENABLED      0x01
#define VBE_DISPI_LFB_ENABLED  0x40
#define VBE_DISPI_NOCLEARMEM   0x80
#define VBE_DISPI_GETCAPS      0x02

#define BOCHS_DISPI_IOPORT_INDEX 0x01CE
#define BOCHS_DISPI_IOPORT_DATA  0x01CF

#define VBE_INDEX_ID             0
#define VBE_INDEX_XRES           1
#define VBE_INDEX_YRES           2
#define VBE_INDEX_BPP            3
#define VBE_INDEX_ENABLE         4
#define VBE_INDEX_BANK           5
#define VBE_INDEX_VIRT_WIDTH     6
#define VBE_INDEX_VIRT_HEIGHT    7
#define VBE_INDEX_X_OFFSET       8
#define VBE_INDEX_Y_OFFSET       9
#define VBE_INDEX_MEMORY         10

/* Fallback LFB address for legacy -vga std */
#define QEMU_LFB_PHYS_LEGACY  0xfd000000ULL

/* Auto-detect LFB physical address from PCI BAR.
 * bochs-display (HDMI): vendor 0x1234 dev 0x1111, LFB in BAR1.
 * -vga std:             same IDs, LFB at 0xfd000000. */
static u64 fb_detect_lfb_phys(void) {
    for (u8 slot = 0; slot < 32; slot++) {
        outl(0xCF8u, (1u<<31)|((u32)slot<<11)|0x00u);
        u32 id = inl(0xCFCu);
        if (id == 0xFFFFFFFFu) continue;
        if ((id & 0xFFFF) != 0x1234 || (id>>16) != 0x1111) continue;
        /* BAR1 first (bochs-display HDMI puts LFB there) */
        outl(0xCF8u, (1u<<31)|((u32)slot<<11)|0x14u);
        u32 b1 = inl(0xCFCu);
        if (b1 && b1 != 0xFFFFFFFFu && !(b1 & 1u)) {
            u64 a = (u64)(b1 & ~0xFu); if (a > 0x100000ULL) return a;
        }
        /* BAR0 fallback */
        outl(0xCF8u, (1u<<31)|((u32)slot<<11)|0x10u);
        u32 b0 = inl(0xCFCu);
        if (b0 && b0 != 0xFFFFFFFFu && !(b0 & 1u)) {
            u64 a = (u64)(b0 & ~0xFu); if (a > 0x100000ULL) return a;
        }
        break;
    }
    return QEMU_LFB_PHYS_LEGACY;
}

/* Standard VBE mode numbers for QEMU */
#define VBE_MODE_1024x768x8      0x0105
#define VBE_MODE_1024x768x15     0x0118
#define VBE_MODE_1024x768x16     0x0119
#define VBE_MODE_1024x768x24     0x011A
#define VBE_MODE_1024x768x32     0x011B

/* Default resolution — can be changed at runtime via fb_set_resolution() */
#define FB_BPP     32
#define FB_MAX_WIDTH  1920
#define FB_MAX_HEIGHT 1080

static int  fb_width  = 1920;   /* current width  */
static int  fb_height = 1080;   /* current height */
#define FB_WIDTH   fb_width
#define FB_HEIGHT  fb_height

static u8  *fb_ptr       = NULL;   /* kernel virtual pointer to framebuffer */
static u64  fb_phys_addr = 0;
static u64  fb_size      = 0;
static int  fb_enabled   = 0;

/* Bochs VGA dispi I/O helpers */
static void dispi_write(u16 index, u16 val) {
    outw(BOCHS_DISPI_IOPORT_INDEX, index);
    outw(BOCHS_DISPI_IOPORT_DATA, val);
}

static u16 dispi_read(u16 index) {
    outw(BOCHS_DISPI_IOPORT_INDEX, index);
    return inw(BOCHS_DISPI_IOPORT_DATA);
}

/* Check if Bochs/VBE dispi interface is available */
static int dispi_check(void) {
    dispi_write(VBE_INDEX_ID, VBE_DISPI_GETCAPS);
    u16 id = dispi_read(VBE_INDEX_ID);
    /* Bochs returns 0xB0C0 or 0xB0C1 for VBE ID */
    return (id == 0xB0C0 || id == 0xB0C1);
}

/* Enable VBE graphics mode via Bochs dispi interface */
void fb_enable(void) {
    watchdog_suspend();   /* fb init + page mapping can take >5s on slow QEMU */
    /* Verify dispi interface is available */
    if (!dispi_check()) {
        /* Fallback: try to use it anyway (QEMU may still work) */
    }

    /* Disable first to reset state */
    dispi_write(VBE_INDEX_ENABLE, 0);

    /* Set resolution and color depth */
    dispi_write(VBE_INDEX_XRES, (u16)fb_width);
    dispi_write(VBE_INDEX_YRES, (u16)fb_height);
    dispi_write(VBE_INDEX_BPP, FB_BPP);

    /* Verify settings were accepted */
    u16 xres = dispi_read(VBE_INDEX_XRES);
    u16 yres = dispi_read(VBE_INDEX_YRES);
    u16 bpp  = dispi_read(VBE_INDEX_BPP);

    if (xres != (u16)fb_width || yres != (u16)fb_height || bpp != FB_BPP) {
        /* Mode not supported, abort gracefully */
        return;
    }

    /* Enable VBE with linear framebuffer */
    dispi_write(VBE_INDEX_ENABLE, VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED | VBE_DISPI_NOCLEARMEM);

    /* Verify mode is active */
    u16 enabled = dispi_read(VBE_INDEX_ENABLE);
    if (!(enabled & VBE_DISPI_ENABLED)) {
        return;  /* Failed to enable */
    }

    fb_phys_addr = fb_detect_lfb_phys();
    fb_size = (u64)fb_width * fb_height * (FB_BPP / 8);

    /* Map framebuffer into kernel address space at 0xA0000000 */
    u64 fb_kernel_va = FB_KERNEL_VA;

    /* Map the framebuffer region identity-mapped for the kernel */
    for (u64 offset = 0; offset < fb_size; offset += PAGE_SIZE) {
        u64 phys = fb_phys_addr + offset;
        u64 virt = fb_kernel_va + offset;
        /* Use PTE_KERNEL_RW (present + writable, no NX) */
        vmm_map(read_cr3(), virt, phys, PTE_KERNEL_RW & ~(1ULL << 63));
    }

    /* Memory barrier to ensure mappings are visible */
    __asm__ volatile("" ::: "memory");

    fb_ptr = (u8 *)fb_kernel_va;
    fb_enabled = 1;

    /* Clear screen to dark blue (retro feel) */
    fb_fill_rect(0, 0, FB_WIDTH, FB_HEIGHT, 0x00003380);
    watchdog_resume();
}

void fb_disable(void) {
    dispi_write(VBE_INDEX_ENABLE, 0);
    fb_enabled = 0;
    fb_ptr = NULL;
}

int fb_is_enabled(void) { return fb_enabled; }

/* ── Drawing primitives ──────────────────────────────────────── */

/* Internal alpha blend: alpha=0 → dst unchanged, alpha=255 → src */
static u32 fb_blend(u32 dst, u32 src, int alpha){
    int sr=(src>>16)&0xff,sg=(src>>8)&0xff,sb=src&0xff;
    int dr=(dst>>16)&0xff,dg=(dst>>8)&0xff,db=dst&0xff;
    int r=dr+((sr-dr)*alpha>>8),g=dg+((sg-dg)*alpha>>8),b=db+((sb-db)*alpha>>8);
    return(u32)((r<<16)|(g<<8)|b);
}

/* Set a single pixel (x, y) to color (0xAARRGGBB) */
void fb_put_pixel(int x, int y, u32 color) {
    if (!fb_enabled || x < 0 || x >= FB_WIDTH || y < 0 || y >= FB_HEIGHT) return;
    u32 *pixel = (u32 *)(fb_ptr + (y * FB_WIDTH + x) * 4);
    *pixel = color;
}

/* Get pixel color at (x, y) */
u32 fb_get_pixel(int x, int y) {
    if (!fb_enabled || x < 0 || x >= FB_WIDTH || y < 0 || y >= FB_HEIGHT) return 0;
    u32 *pixel = (u32 *)(fb_ptr + (y * FB_WIDTH + x) * 4);
    return *pixel;
}

/* Fill a rectangle with a solid color */
void fb_fill_rect(int x, int y, int w, int h, u32 color) {
    if (!fb_enabled) return;
    /* Clip */
    if (x + w <= 0 || y + h <= 0 || x >= FB_WIDTH || y >= FB_HEIGHT) return;
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w > FB_WIDTH ? FB_WIDTH : x + w;
    int y1 = y + h > FB_HEIGHT ? FB_HEIGHT : y + h;

    for (int row = y0; row < y1; row++) {
        u32 *line = (u32 *)(fb_ptr + (row * FB_WIDTH + x0) * 4);
        for (int col = x0; col < x1; col++) {
            line[col - x0] = color;
        }
    }
}

/* Fast gradient fill: horizontal (left color ca, right color cb) */
void fb_fill_gradient_h(int x, int y, int w, int h, u32 ca, u32 cb) {
    if (!fb_enabled || w <= 0 || h <= 0) return;
    if (x >= FB_WIDTH || y >= FB_HEIGHT || x+w <= 0 || y+h <= 0) return;
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x+w > FB_WIDTH ? FB_WIDTH : x+w;
    int y1 = y+h > FB_HEIGHT ? FB_HEIGHT : y+h;
    int ar=(ca>>16)&0xff, ag=(ca>>8)&0xff, ab=ca&0xff;
    int br=(cb>>16)&0xff, bg=(cb>>8)&0xff, bb=cb&0xff;
    /* Precompute one gradient row, then copy it h times */
    static u32 grad_row[FB_MAX_WIDTH];
    int rw = x1 - x0;
    for (int col = 0; col < rw; col++) {
        int gc = col + (x0 - x); /* position in original gradient */
        int r = w > 1 ? ar + (br-ar)*gc/(w-1) : ar;
        int g = w > 1 ? ag + (bg-ag)*gc/(w-1) : ag;
        int b = w > 1 ? ab + (bb-ab)*gc/(w-1) : ab;
        grad_row[col] = (u32)((r<<16)|(g<<8)|b);
    }
    for (int row = y0; row < y1; row++) {
        u32 *line = (u32 *)(fb_ptr + (row * FB_WIDTH + x0) * 4);
        for (int col = 0; col < rw; col++) line[col] = grad_row[col];
    }
}

/* Fast gradient fill: vertical (top color ca, bottom color cb) */
void fb_fill_gradient_v(int x, int y, int w, int h, u32 ca, u32 cb) {
    if (!fb_enabled || w <= 0 || h <= 0) return;
    if (x >= FB_WIDTH || y >= FB_HEIGHT || x+w <= 0 || y+h <= 0) return;
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x+w > FB_WIDTH ? FB_WIDTH : x+w;
    int y1 = y+h > FB_HEIGHT ? FB_HEIGHT : y+h;
    int ar=(ca>>16)&0xff, ag=(ca>>8)&0xff, ab=ca&0xff;
    int br=(cb>>16)&0xff, bg=(cb>>8)&0xff, bb=cb&0xff;
    int rw = x1 - x0;
    for (int row = y0; row < y1; row++) {
        int gr = row + (y0 - y);
        int r = h > 1 ? ar + (br-ar)*gr/(h-1) : ar;
        int g = h > 1 ? ag + (bg-ag)*gr/(h-1) : ag;
        int b = h > 1 ? ab + (bb-ab)*gr/(h-1) : ab;
        u32 c = (u32)((r<<16)|(g<<8)|b);
        u32 *line = (u32 *)(fb_ptr + (row * FB_WIDTH + x0) * 4);
        for (int col = 0; col < rw; col++) line[col] = c;
    }
}

/* Shadow rect: semi-transparent black overlay (no read-modify-write, just darken) */
void fb_draw_shadow(int x, int y, int w, int h) {
    if (!fb_enabled) return;
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x+w > FB_WIDTH ? FB_WIDTH : x+w;
    int y1 = y+h > FB_HEIGHT ? FB_HEIGHT : y+h;
    int rw = x1 - x0;
    for (int row = y0; row < y1; row++) {
        int alpha = 140 - (row - y0) * 2; if (alpha < 30) alpha = 30;
        int inv = 256 - alpha;
        u32 *line = (u32 *)(fb_ptr + (row * FB_WIDTH + x0) * 4);
        for (int col = 0; col < rw; col++) {
            u32 p = line[col];
            int r = ((p>>16)&0xff)*inv>>8;
            int g = ((p>>8)&0xff)*inv>>8;
            int b = (p&0xff)*inv>>8;
            line[col] = (u32)((r<<16)|(g<<8)|b);
        }
    }
}

/* Draw a rectangle outline */
void fb_draw_rect(int x, int y, int w, int h, u32 color) {
    if (!fb_enabled) return;
    /* Top and bottom edges */
    for (int i = 0; i < w; i++) {
        fb_put_pixel(x + i, y, color);
        fb_put_pixel(x + i, y + h - 1, color);
    }
    /* Left and right edges */
    for (int i = 0; i < h; i++) {
        fb_put_pixel(x, y + i, color);
        fb_put_pixel(x + w - 1, y + i, color);
    }
}

/* Draw a horizontal line */
void fb_draw_hline(int x, int y, int w, u32 color) {
    fb_fill_rect(x, y, w, 1, color);
}

/* Draw a vertical line */
void fb_draw_vline(int x, int y, int h, u32 color) {
    fb_fill_rect(x, y, 1, h, color);
}

/* Bresenham's line algorithm */
void fb_draw_line(int x0, int y0, int x1, int y1, u32 color) {
    if (!fb_enabled) return;
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    int e2;

    while (1) {
        fb_put_pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx)  { err += dx; y0 += sy; }
    }
}

/* Draw a rounded rectangle (simple: rectangle with 2-pixel corners) */
void fb_draw_rounded_rect(int x, int y, int w, int h, u32 color) {
    if (!fb_enabled) return;
    int r = 4; /* 4px radius for smooth modern corners */
    if (w < r*2+2) r = w/2-1;
    if (h < r*2+2) r = h/2-1;
    if (r < 1) r = 1;
    /* Straight edges */
    fb_draw_hline(x + r, y,         w - r*2, color);
    fb_draw_hline(x + r, y + h - 1, w - r*2, color);
    fb_draw_vline(x,         y + r, h - r*2, color);
    fb_draw_vline(x + w - 1, y + r, h - r*2, color);
    /* Anti-aliased arc for each corner using precomputed coverage */
    for (int i = 0; i < r; i++) {
        /* distance from corner centre to pixel edge, normalised to alpha */
        int cr = r - i;
        int aa_outer = 255 - (cr * 255 / r);
        int aa_inner = cr * 200 / r;
        /* top-left */
        fb_put_pixel(x + i,         y + r - i - 1, fb_blend(fb_get_pixel(x+i, y+r-i-1), color, aa_outer));
        fb_put_pixel(x + r - i - 1, y + i,         fb_blend(fb_get_pixel(x+r-i-1, y+i), color, aa_inner));
        /* top-right */
        fb_put_pixel(x+w-1-i,       y + r - i - 1, fb_blend(fb_get_pixel(x+w-1-i, y+r-i-1), color, aa_outer));
        fb_put_pixel(x+w-r+i,       y + i,         fb_blend(fb_get_pixel(x+w-r+i, y+i), color, aa_inner));
        /* bottom-left */
        fb_put_pixel(x + i,         y+h-r+i,       fb_blend(fb_get_pixel(x+i, y+h-r+i), color, aa_outer));
        fb_put_pixel(x + r - i - 1, y+h-1-i,       fb_blend(fb_get_pixel(x+r-i-1, y+h-1-i), color, aa_inner));
        /* bottom-right */
        fb_put_pixel(x+w-1-i,       y+h-r+i,       fb_blend(fb_get_pixel(x+w-1-i, y+h-r+i), color, aa_outer));
        fb_put_pixel(x+w-r+i,       y+h-1-i,       fb_blend(fb_get_pixel(x+w-r+i, y+h-1-i), color, aa_inner));
    }
}

/* Fill a rounded rectangle with smooth 4px-radius corners */
void fb_fill_rounded_rect(int x, int y, int w, int h, u32 color) {
    if (!fb_enabled) return;
    int r = 4;
    if (w < r*2+2) r = w/2-1;
    if (h < r*2+2) r = h/2-1;
    if (r < 1) r = 1;
    /* Fill body in three horizontal bands */
    fb_fill_rect(x,     y + r, w, h - r*2, color); /* middle band   */
    fb_fill_rect(x + r, y,     w - r*2, r, color); /* top cap       */
    fb_fill_rect(x + r, y+h-r, w - r*2, r, color); /* bottom cap    */
    /* Fill corner arcs — midpoint circle fill per row */
    for (int dy = 0; dy < r; dy++) {
        /* compute filled width at this row using circle equation: x = sqrt(r^2 - dy^2) */
        int fy = r - dy; /* distance from centre row */
        int fx = 0;
        /* integer sqrt via iteration */
        while ((fx+1)*(fx+1) + fy*fy <= r*r) fx++;
        int row_x = r - fx;
        /* top corners */
        fb_fill_rect(x + row_x, y + dy, w - row_x*2, 1, color);
        /* bottom corners */
        fb_fill_rect(x + row_x, y+h-1-dy, w - row_x*2, 1, color);
        /* anti-alias the corner edge pixel */
        int aa = fx * 180 / r;
        u32 tl_bg = fb_get_pixel(x+row_x-1, y+dy);
        u32 tr_bg = fb_get_pixel(x+w-row_x, y+dy);
        u32 bl_bg = fb_get_pixel(x+row_x-1, y+h-1-dy);
        u32 br_bg = fb_get_pixel(x+w-row_x, y+h-1-dy);
        fb_put_pixel(x+row_x-1,  y+dy,      fb_blend(tl_bg, color, aa));
        fb_put_pixel(x+w-row_x,  y+dy,      fb_blend(tr_bg, color, aa));
        fb_put_pixel(x+row_x-1,  y+h-1-dy,  fb_blend(bl_bg, color, aa));
        fb_put_pixel(x+w-row_x,  y+h-1-dy,  fb_blend(br_bg, color, aa));
    }
}

/* Draw a circle outline (midpoint algorithm) */
void fb_draw_circle(int cx, int cy, int r, u32 color) {
    if (!fb_enabled) return;
    int x = r, y = 0;
    int err = 0;

    while (x >= y) {
        fb_put_pixel(cx + x, cy + y, color);
        fb_put_pixel(cx + y, cy + x, color);
        fb_put_pixel(cx - y, cy + x, color);
        fb_put_pixel(cx - x, cy + y, color);
        fb_put_pixel(cx - x, cy - y, color);
        fb_put_pixel(cx - y, cy - x, color);
        fb_put_pixel(cx + y, cy - x, color);
        fb_put_pixel(cx + x, cy - y, color);

        y++;
        if (err <= 0) {
            err += 2 * y + 1;
            if (x > y) {
                err -= 2 * x - 1;
                x--;
            }
        }
    }
}

/* Blit a rectangular block of pixels from src to framebuffer */
void fb_blit(int dx, int dy, int w, int h, const u32 *src, int src_stride) {
    if (!fb_enabled) return;
    if (dx + w <= 0 || dy + h <= 0 || dx >= FB_WIDTH || dy >= FB_HEIGHT) return;

    int src_x0 = 0, src_y0 = 0;
    int dst_x0 = dx, dst_y0 = dy;
    int copy_w = w, copy_h = h;

    /* Clip left/top */
    if (dst_x0 < 0) { src_x0 -= dst_x0; copy_w += dst_x0; dst_x0 = 0; }
    if (dst_y0 < 0) { src_y0 -= dst_y0; copy_h += dst_y0; dst_y0 = 0; }

    /* Clip right/bottom */
    if (dst_x0 + copy_w > FB_WIDTH)  copy_w = FB_WIDTH - dst_x0;
    if (dst_y0 + copy_h > FB_HEIGHT) copy_h = FB_HEIGHT - dst_y0;

    if (copy_w <= 0 || copy_h <= 0) return;

    for (int row = 0; row < copy_h; row++) {
        u32 *dst_line = (u32 *)(fb_ptr + ((dst_y0 + row) * FB_WIDTH + dst_x0) * 4);
        const u32 *src_line = src + (src_y0 + row) * src_stride + src_x0;
        for (int col = 0; col < copy_w; col++) {
            dst_line[col] = src_line[col];
        }
    }
}

/* ── Accessors ───────────────────────────────────────────────── */
int  fb_get_width(void)  { return fb_width; }
int  fb_get_height(void) { return fb_height; }
int  fb_get_bpp(void)    { return FB_BPP; }
u8  *fb_get_ptr(void)    { return fb_ptr; }
u64  fb_get_phys(void)   { return fb_phys_addr; }

/* ── Dynamic resolution change ─────────────────────────────── */
/*
 * fb_set_resolution(w, h)
 *
 * Switches the Bochs/VBE display to a new resolution at runtime.
 * Safe to call while the framebuffer is active; re-maps only if
 * the new fb_size exceeds the old mapped region (we map the full
 * FB_MAX_WIDTH*FB_MAX_HEIGHT*4 bytes on first enable, so in
 * practice no re-map is needed for 720p or 1080p).
 *
 * Returns 1 on success, 0 if the mode was rejected by the hardware.
 */
int fb_set_resolution(int w, int h) {
    if (w <= 0 || h <= 0) return 0;
    if (w > FB_MAX_WIDTH)  w = FB_MAX_WIDTH;
    if (h > FB_MAX_HEIGHT) h = FB_MAX_HEIGHT;

    /* Disable, reconfigure, re-enable */
    dispi_write(VBE_INDEX_ENABLE, 0);

    dispi_write(VBE_INDEX_XRES, (u16)w);
    dispi_write(VBE_INDEX_YRES, (u16)h);
    dispi_write(VBE_INDEX_BPP, FB_BPP);

    u16 xres = dispi_read(VBE_INDEX_XRES);
    u16 yres = dispi_read(VBE_INDEX_YRES);

    if (xres != (u16)w || yres != (u16)h) {
        /* Rejected — restore previous mode */
        dispi_write(VBE_INDEX_XRES, (u16)fb_width);
        dispi_write(VBE_INDEX_YRES, (u16)fb_height);
        dispi_write(VBE_INDEX_BPP, FB_BPP);
        dispi_write(VBE_INDEX_ENABLE,
                    VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED | VBE_DISPI_NOCLEARMEM);
        return 0;
    }

    dispi_write(VBE_INDEX_ENABLE,
                VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED | VBE_DISPI_NOCLEARMEM);

    fb_width  = w;
    fb_height = h;
    fb_size   = (u64)w * h * (FB_BPP / 8);

    /* Clear new viewport */
    fb_fill_rect(0, 0, w, h, 0x00003380);
    return 1;
}
