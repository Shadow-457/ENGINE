/* ================================================================
 *  Systrix OS — kernel/gfx.c
 *  Phase 2: Graphics & Rendering
 *
 *  Three subsystems:
 *
 *  1. DOUBLE BUFFERING
 *     The Bochs VBE dispi interface supports a virtual framebuffer
 *     taller than the display.  We set VIRT_HEIGHT = 1536 (2 × 768)
 *     so the hardware holds two full pages:
 *       front page  — Y offset   0 .. 767  (what the monitor shows)
 *       back  page  — Y offset 768 .. 1535 (where we draw)
 *     sys_gfx_flip() waits for VGA vertical retrace then sets
 *     VBE_INDEX_Y_OFFSET to swap pages with zero tearing.
 *     After the flip the pages are swapped in software too, so the
 *     next draw always targets the invisible page.
 *
 *  2. SPRITE BLITTER
 *     sys_gfx_blit() copies a user-supplied pixel buffer onto the
 *     back buffer with full clipping and optional colorkey (one
 *     colour treated as transparent).
 *     Colorkey is set once with sys_gfx_set_colorkey(); pass
 *     GFX_COLORKEY_NONE (0xFFFFFFFF) to disable.
 *
 *  3. TILEMAP RENDERER
 *     sys_gfx_set_tilemap() registers a tileset (pixel data) and a
 *     map (array of tile indices) for one of two layers.
 *     sys_gfx_draw_tile() draws one tile from the registered tileset
 *     directly — useful for single-tile sprites or HUD icons.
 *     The full layer is rendered to the back buffer by
 *     sys_gfx_render_layer(); scroll offsets (sx, sy) let the
 *     camera pan across a map larger than the screen.
 *
 *  Syscall numbers (see kernel.h SYS_GFX_*):
 *    310  sys_gfx_flip          ()
 *    311  sys_gfx_clear         (color)
 *    312  sys_gfx_blit          (x, y, w, h, pixels, colorkey)
 *    313  sys_gfx_draw_tile     (x, y, tile_id, layer)
 *    314  sys_gfx_set_tilemap   (layer, GfxTilemap*)
 *    315  sys_gfx_render_layer  (layer, scroll_x, scroll_y)
 * ================================================================ */

#include "../include/kernel.h"

/* ── Bochs VBE port helpers (mirrored from fbdev.c) ─────────── */
#define BOCHS_DISPI_IOPORT_INDEX  0x01CE
#define BOCHS_DISPI_IOPORT_DATA   0x01CF
#define VBE_INDEX_VIRT_HEIGHT     7
#define VBE_INDEX_Y_OFFSET        9

#define FB_WIDTH   1024
#define FB_HEIGHT   768
#define FB_PAGES      2          /* front + back */
#define FB_VIRT_H  (FB_HEIGHT * FB_PAGES)   /* 1536 */

static void vbe_write(u16 idx, u16 val) {
    outw(BOCHS_DISPI_IOPORT_INDEX, idx);
    outw(BOCHS_DISPI_IOPORT_DATA,  val);
}

/* ── VGA vertical retrace port ───────────────────────────────── */
#define VGA_INPUT_STATUS1  0x3DA   /* bit 3 = vblank active */

static void vsync_wait(void) {
    u32 timeout = 1000000;
    /* Wait for vblank to END (so we enter the active period) */
    while ((inb(VGA_INPUT_STATUS1) & 0x08) && --timeout) { watchdog_pet(); }
    /* Wait for vblank to START */
    timeout = 1000000;
    while (!(inb(VGA_INPUT_STATUS1) & 0x08) && --timeout) { watchdog_pet(); }
}

/* ── Double-buffer state ─────────────────────────────────────── */
/* back_page_y: Y origin of the page we are currently drawing into.
 * Starts at 768 so first flip shows what was drawn at Y=768.        */
static int back_page_y  = FB_HEIGHT;   /* 768 = back page  */
static int front_page_y = 0;           /* 0   = front page */
static int db_initialised = 0;

/* Pointer to start of the back page in kernel VA space */
static inline u32 *back_buf_ptr(void) {
    u8 *base = fb_get_ptr();
    if (!base) return (u32*)0;
    return (u32*)(base + (usize)back_page_y * FB_WIDTH * 4);
}

/* Initialise virtual framebuffer height for double-buffering.
 * Called lazily on first gfx syscall that needs it.               */
static void db_init(void) {
    if (db_initialised) return;
    if (!fb_is_enabled()) return;
    vbe_write(VBE_INDEX_VIRT_HEIGHT, (u16)FB_VIRT_H);
    /* Start displaying the front page (Y=0) */
    vbe_write(VBE_INDEX_Y_OFFSET, 0);
    db_initialised = 1;
}

/* ── Colorkey ─────────────────────────────────────────────────── */
static u32 gfx_colorkey = GFX_COLORKEY_NONE;

/* ── Tilemap layer state ──────────────────────────────────────── */
#define GFX_MAX_LAYERS  2
static GfxTilemap layers[GFX_MAX_LAYERS];
static int        layer_set[GFX_MAX_LAYERS] = {0, 0};

/* ================================================================
 *  sys_gfx_flip — vsync page flip
 * ================================================================ */
i64 sys_gfx_flip(void) {
    if (!fb_is_enabled()) return (i64)EINVAL;
    db_init();

    vsync_wait();

    /* Flip: make the back page visible */
    vbe_write(VBE_INDEX_Y_OFFSET, (u16)back_page_y);

    /* Swap page roles */
    int tmp    = front_page_y;
    front_page_y = back_page_y;
    back_page_y  = tmp;

    return 0;
}

/* ================================================================
 *  sys_gfx_clear — fill back buffer with solid colour
 * ================================================================ */
i64 sys_gfx_clear(u32 color) {
    if (!fb_is_enabled()) return (i64)EINVAL;
    db_init();

    u32 *p   = back_buf_ptr();
    if (!p) return (i64)EINVAL;
    usize n  = (usize)FB_WIDTH * FB_HEIGHT;
    for (usize i = 0; i < n; i++) p[i] = color;
    return 0;
}

/* ================================================================
 *  sys_gfx_blit — copy pixel data onto back buffer with clipping
 *                 and optional colorkey transparency
 *
 *  pixels  — pointer to w×h ARGB32 pixel array (user VA)
 *  colorkey — colour to treat as transparent; GFX_COLORKEY_NONE
 *             disables colorkey for this call (ignores global key)
 * ================================================================ */
i64 sys_gfx_blit(int dx, int dy, int w, int h,
                 const u32 *pixels, u32 colorkey) {
    if (!fb_is_enabled() || !pixels || w <= 0 || h <= 0)
        return (i64)EINVAL;
    db_init();

    /* Use per-call colorkey if provided, else global key */
    u32 ckey = (colorkey != GFX_COLORKEY_NONE) ? colorkey : gfx_colorkey;
    int use_ckey = (ckey != GFX_COLORKEY_NONE);

    /* Clip source/dest rectangles */
    int sx0 = 0, sy0 = 0;
    int dst_x = dx, dst_y = dy;
    int cw = w, ch = h;

    if (dst_x < 0) { sx0 -= dst_x; cw += dst_x; dst_x = 0; }
    if (dst_y < 0) { sy0 -= dst_y; ch += dst_y; dst_y = 0; }
    if (dst_x + cw > FB_WIDTH)  cw = FB_WIDTH  - dst_x;
    if (dst_y + ch > FB_HEIGHT) ch = FB_HEIGHT - dst_y;
    if (cw <= 0 || ch <= 0) return 0;

    u8  *base = fb_get_ptr();
    if (!base) return (i64)EINVAL;

    for (int row = 0; row < ch; row++) {
        int abs_y = back_page_y + dst_y + row;
        u32 *dst_line = (u32 *)(base + (usize)abs_y * FB_WIDTH * 4) + dst_x;
        const u32 *src_line = pixels + (usize)(sy0 + row) * (usize)w + sx0;
        if (use_ckey) {
            for (int col = 0; col < cw; col++) {
                u32 px = src_line[col];
                if (px != ckey) dst_line[col] = px;
            }
        } else {
            for (int col = 0; col < cw; col++)
                dst_line[col] = src_line[col];
        }
    }
    return 0;
}

/* ================================================================
 *  sys_gfx_set_colorkey — set global colorkey (persistent)
 * ================================================================ */
i64 sys_gfx_set_colorkey(u32 color) {
    gfx_colorkey = color;
    return 0;
}

/* ================================================================
 *  sys_gfx_set_tilemap — register a tileset + map for a layer
 * ================================================================ */
i64 sys_gfx_set_tilemap(u64 layer_id, const GfxTilemap *tm) {
    if (layer_id >= GFX_MAX_LAYERS || !tm) return (i64)EINVAL;
    layers[layer_id] = *tm;
    layer_set[layer_id] = 1;
    return 0;
}

/* ================================================================
 *  sys_gfx_draw_tile — draw one tile from a layer's tileset
 *
 *  x, y      — destination top-left on the back buffer
 *  tile_id   — index into the tileset (0-based)
 *  layer_id  — which layer's tileset to use (0 or 1)
 *
 *  Colorkey transparency uses the global colorkey.
 * ================================================================ */
i64 sys_gfx_draw_tile(int x, int y, u32 tile_id, u64 layer_id) {
    if (!fb_is_enabled()) return (i64)EINVAL;
    if (layer_id >= GFX_MAX_LAYERS || !layer_set[layer_id])
        return (i64)EINVAL;
    db_init();

    const GfxTilemap *tm = &layers[layer_id];
    if (!tm->tileset || tile_id >= tm->tile_count) return (i64)EINVAL;

    int tw = (int)tm->tile_w;
    int th = (int)tm->tile_h;
    /* Pointer to the tile's pixel data in the tileset strip */
    const u32 *tile_px = tm->tileset + (usize)tile_id * (usize)tw * (usize)th;

    /* Reuse blit with per-call colorkey = GFX_COLORKEY_NONE so the
     * global colorkey is applied                                    */
    return sys_gfx_blit(x, y, tw, th, tile_px, GFX_COLORKEY_NONE);
}

/* ================================================================
 *  sys_gfx_render_layer — draw a full tilemap layer to back buffer
 *
 *  scroll_x, scroll_y — pixel offset into the map (camera position)
 *  Only tiles that overlap the viewport are drawn.
 * ================================================================ */
i64 sys_gfx_render_layer(u64 layer_id, int scroll_x, int scroll_y) {
    if (!fb_is_enabled()) return (i64)EINVAL;
    if (layer_id >= GFX_MAX_LAYERS || !layer_set[layer_id])
        return (i64)EINVAL;
    db_init();

    const GfxTilemap *tm = &layers[layer_id];
    if (!tm->tileset || !tm->map || !tm->map_w || !tm->map_h)
        return (i64)EINVAL;

    int tw = (int)tm->tile_w;
    int th = (int)tm->tile_h;

    /* First and last tile indices visible on screen */
    int tx_start = scroll_x / tw;
    int ty_start = scroll_y / th;
    int tx_end   = (scroll_x + FB_WIDTH  + tw - 1) / tw;
    int ty_end   = (scroll_y + FB_HEIGHT + th - 1) / th;

    /* Clamp to map bounds */
    if (tx_start < 0) tx_start = 0;
    if (ty_start < 0) ty_start = 0;
    if (tx_end > (int)tm->map_w) tx_end = (int)tm->map_w;
    if (ty_end > (int)tm->map_h) ty_end = (int)tm->map_h;

    for (int ty = ty_start; ty < ty_end; ty++) {
        for (int tx = tx_start; tx < tx_end; tx++) {
            u32 tile_id = tm->map[(usize)ty * tm->map_w + tx];
            if (tile_id == GFX_TILE_EMPTY) continue;
            if (tile_id >= tm->tile_count)  continue;

            int dst_x = tx * tw - scroll_x;
            int dst_y = ty * th - scroll_y;

            const u32 *tile_px = tm->tileset +
                                 (usize)tile_id * (usize)tw * (usize)th;
            sys_gfx_blit(dst_x, dst_y, tw, th,
                         tile_px, GFX_COLORKEY_NONE);
        }
    }
    return 0;
}
