/* ================================================================
 *  Systrix OS — user/gfx.h
 *  Phase 2: Graphics & Rendering — user-space API
 *
 *  Include this header in any user program that wants to draw.
 *  All functions operate on the BACK buffer.  Call flip() when
 *  a frame is complete to swap it to the screen without tearing.
 *
 *  Typical game loop:
 *
 *      while (1) {
 *          clear_screen(0x000010);          // dark background
 *          blit(px, py, 16, 16, sprite, COLORKEY_NONE);
 *          flip();                          // vsync + page swap
 *      }
 *
 *  Tilemap example:
 *
 *      GfxTilemap tm = {
 *          .tileset    = my_tiles,          // 16×16 pixel strips
 *          .map        = my_map,
 *          .tile_w     = 16, .tile_h = 16,
 *          .tile_count = 8,
 *          .map_w      = 64, .map_h  = 32,
 *      };
 *      set_tilemap(0, &tm);
 *      render_layer(0, cam_x, cam_y);
 *      flip();
 * ================================================================ */

#pragma once
#include "libc.h"   /* for basic types and syscall infrastructure */

/* ── Types ────────────────────────────────────────────────────── */
typedef struct {
    const unsigned int *tileset;
    const unsigned int *map;
    unsigned int tile_w, tile_h;
    unsigned int tile_count;
    unsigned int map_w, map_h;
    unsigned int _pad;
} GfxTilemap;

/* Special sentinel values */
#define COLORKEY_NONE   0xFFFFFFFFU   /* disable colorkey for this call  */
#define TILE_EMPTY      0xFFFFFFFFU   /* map entry = no tile drawn        */

/* ── Syscall numbers ──────────────────────────────────────────── */
#define SYS_GFX_FLIP          310
#define SYS_GFX_CLEAR         311
#define SYS_GFX_BLIT          312
#define SYS_GFX_SET_COLORKEY  313
#define SYS_GFX_DRAW_TILE     314
#define SYS_GFX_SET_TILEMAP   315
#define SYS_GFX_RENDER_LAYER  316

/* ── Internal syscall helper ──────────────────────────────────── */
/* Six-argument syscall:  rax=nr  rdi  rsi  rdx  r10  r8  r9     */
static inline long __gfx_sc6(long nr, long a, long b, long c,
                              long d, long e, long f) {
    long r;
    register long _r10 __asm__("r10") = d;
    register long _r8  __asm__("r8")  = e;
    register long _r9  __asm__("r9")  = f;
    __asm__ volatile("syscall"
        : "=a"(r)
        : "0"(nr), "D"(a), "S"(b), "d"(c),
          "r"(_r10), "r"(_r8), "r"(_r9)
        : "rcx", "r11", "memory");
    return r;
}
static inline long __gfx_sc1(long nr, long a) {
    long r;
    __asm__ volatile("syscall"
        : "=a"(r) : "0"(nr), "D"(a) : "rcx","r11","memory");
    return r;
}
static inline long __gfx_sc2(long nr, long a, long b) {
    long r;
    __asm__ volatile("syscall"
        : "=a"(r) : "0"(nr), "D"(a), "S"(b) : "rcx","r11","memory");
    return r;
}
static inline long __gfx_sc0(long nr) {
    long r;
    __asm__ volatile("syscall"
        : "=a"(r) : "0"(nr) : "rcx","r11","memory");
    return r;
}

/* ── Public API ───────────────────────────────────────────────── */

/*
 * flip() — vsync wait + page swap.
 * Waits for vertical retrace, makes the current back buffer visible,
 * then swaps so the next frame is drawn into the (now hidden) page.
 * Call once per frame at the end of your render loop.
 */
static inline void flip(void) {
    __gfx_sc0(SYS_GFX_FLIP);
}

/*
 * clear_screen(color) — fill the back buffer with a solid colour.
 * color is 0x00RRGGBB.
 */
static inline void clear_screen(unsigned int color) {
    __gfx_sc1(SYS_GFX_CLEAR, (long)color);
}

/*
 * blit(x, y, w, h, pixels, colorkey)
 * Copy a w×h pixel rectangle from 'pixels' onto the back buffer at
 * (x, y).  Pixels outside the screen are clipped automatically.
 * Set colorkey = COLORKEY_NONE to copy every pixel, or a specific
 * colour to skip those pixels (sprite transparency).
 */
static inline void blit(int x, int y, int w, int h,
                        const unsigned int *pixels,
                        unsigned int colorkey) {
    __gfx_sc6(SYS_GFX_BLIT,
              (long)x, (long)y, (long)w,
              (long)h, (long)(unsigned long)pixels, (long)colorkey);
}

/*
 * set_colorkey(color) — set the persistent global colorkey.
 * Any subsequent blit() / draw_tile() call that passes COLORKEY_NONE
 * will use this colour for transparency.
 * Call set_colorkey(COLORKEY_NONE) to disable globally.
 */
static inline void set_colorkey(unsigned int color) {
    __gfx_sc1(SYS_GFX_SET_COLORKEY, (long)color);
}

/*
 * set_tilemap(layer, tm) — register a tileset + map for layer 0 or 1.
 * Must be called before render_layer() or draw_tile() for that layer.
 */
static inline void set_tilemap(unsigned int layer, const GfxTilemap *tm) {
    __gfx_sc2(SYS_GFX_SET_TILEMAP, (long)layer, (long)(unsigned long)tm);
}

/*
 * draw_tile(x, y, tile_id, layer) — draw one tile from a layer's
 * tileset at pixel position (x, y) on the back buffer.
 * Useful for sprites, HUD icons, or manual tile placement.
 */
static inline void draw_tile(int x, int y,
                             unsigned int tile_id, unsigned int layer) {
    register long _r10 __asm__("r10") = (long)layer;
    long r;
    __asm__ volatile("syscall"
        : "=a"(r)
        : "0"((long)SYS_GFX_DRAW_TILE),
          "D"((long)x), "S"((long)y), "d"((long)tile_id), "r"(_r10)
        : "rcx", "r11", "memory");
    (void)r;
}

/*
 * render_layer(layer, scroll_x, scroll_y) — draw all visible tiles of
 * the registered tilemap onto the back buffer.
 * scroll_x / scroll_y are the pixel offset of the camera into the map.
 * Only tiles that overlap the 1024×768 viewport are drawn.
 */
static inline void render_layer(unsigned int layer,
                                int scroll_x, int scroll_y) {
    long r;
    register long _r10 __asm__("r10") = (long)scroll_x;
    register long _r8  __asm__("r8")  = (long)scroll_y;
    __asm__ volatile("syscall"
        : "=a"(r)
        : "0"((long)SYS_GFX_RENDER_LAYER), "D"((long)layer),
          "r"(_r10), "r"(_r8)
        : "rcx", "r11", "memory");
    (void)r;
}
