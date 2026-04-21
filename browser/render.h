/* ================================================================
 *  SoftTail OS Browser — browser/render.h
 *  Framebuffer renderer (Phase 4)
 *
 *  Walks LayoutBox tree and draws everything to the SoftTail OS
 *  framebuffer using the syscall API from gfx.h.
 *  Uses font8x8.h for glyph rendering.
 * ================================================================ */
#pragma once
#include "layout.h"
#include "../include/font8x8.h"

/* ── Framebuffer syscalls ────────────────────────────────────── */
/* We write directly to a pixel buffer, then blit via gfx.h.
 * The buffer is statically allocated — 1024×768×4 = 3 MB.
 * This sits in BSS so it doesn't bloat the binary. */

#define FB_W  BROWSER_WIDTH
#define FB_H  BROWSER_HEIGHT

static unsigned int *render_fb = (unsigned int*)0;  /* heap-allocated in browser_init() */

/* scroll offset */
static int render_scroll_y = 0;
#define RENDER_SCROLL_STEP 40

static inline void render_clear(unsigned int color) {
    for (int i = 0; i < FB_W * FB_H; i++) render_fb[i] = color;
}

static inline void render_set_pixel(int x, int y, unsigned int color) {
    if (x<0||x>=FB_W||y<0||y>=FB_H) return;
    render_fb[y*FB_W+x] = color;
}

static inline void render_fill_rect(int x, int y, int w, int h, unsigned int color) {
    for (int row = y; row < y+h; row++)
        for (int col = x; col < x+w; col++)
            render_set_pixel(col, row, color);
}

static inline void render_hline(int x, int y, int w, unsigned int color) {
    for (int i = x; i < x+w; i++) render_set_pixel(i, y, color);
}

/* ── Glyph renderer (font8x8) ────────────────────────────────── */
/* font8x8_data is a 128×8 bitmap font — each char is 8 bytes.
 * Each byte is a row; bit 0 = leftmost pixel. */

static inline void render_char(int x, int y, unsigned char c,
                                unsigned int fg, unsigned int bg,
                                int font_size, int bold) {
    if (c >= 128) c = '?';
    const char *glyph = font8x8_data[(int)c];
    int scale = (font_size <= 8) ? 1 : (font_size / 8);
    if (scale < 1) scale = 1;
    if (scale > 4) scale = 4;

    for (int row = 0; row < 8; row++) {
        unsigned char bits = (unsigned char)glyph[row];
        for (int col = 0; col < 8; col++) {
            int set = (bits >> col) & 1;
            if (!set && bg == 0xFFFFFFFF) continue; /* transparent bg */
            unsigned int pixel = set ? fg : bg;
            for (int sy = 0; sy < scale; sy++)
                for (int sx = 0; sx < scale; sx++) {
                    render_set_pixel(x+col*scale+sx, y+row*scale+sy, pixel);
                    /* bold: draw again 1px right */
                    if (bold && set)
                        render_set_pixel(x+col*scale+sx+1, y+row*scale+sy, pixel);
                }
        }
    }
}

static inline void render_text(int x, int y, const char *text,
                                unsigned int fg, unsigned int bg,
                                int font_size, int bold, int avail_w) {
    int scale = (font_size <= 8) ? 1 : (font_size / 8);
    if (scale < 1) scale = 1; if (scale > 4) scale = 4;
    int gw = 8 * scale + (bold ? 1 : 0);
    int lh = font_size * LINE_HEIGHT_MUL / 100;
    int cx = x, cy = y;
    int right_edge = x + avail_w;

    for (int i = 0; text[i]; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c == '\n') { cx = x; cy += lh; continue; }
        if (c == ' ') { cx += gw; continue; }

        /* word wrap */
        if (cx + gw > right_edge) {
            cx = x; cy += lh;
        }
        render_char(cx, cy, c, fg, 0xFFFFFFFF, font_size, bold);
        cx += gw;
    }
}

/* ── Scrollbar ───────────────────────────────────────────────── */
#define SCROLLBAR_W 10

static inline void render_scrollbar(int content_h) {
    if (content_h <= FB_H) return;
    int bar_h = (FB_H * FB_H) / content_h;
    if (bar_h < 20) bar_h = 20;
    int bar_y = (render_scroll_y * (FB_H - bar_h)) / (content_h - FB_H);
    render_fill_rect(FB_W - SCROLLBAR_W, 0, SCROLLBAR_W, FB_H, 0xDDDDDD);
    render_fill_rect(FB_W - SCROLLBAR_W, bar_y, SCROLLBAR_W, bar_h, 0x888888);
}

/* ── URL bar ─────────────────────────────────────────────────── */
#define URLBAR_H   28
#define URLBAR_BG  0xF5F5F5
#define URLBAR_FG  0x333333
#define URLBAR_BDR 0xCCCCCC

static inline void render_urlbar(const char *url, int loading) {
    render_fill_rect(0, 0, FB_W, URLBAR_H, URLBAR_BG);
    render_hline(0, URLBAR_H-1, FB_W, URLBAR_BDR);

    /* back button */
    render_fill_rect(4, 4, 22, 20, 0xDDDDDD);
    render_text(8, 8, "<", 0x333333, 0xFFFFFFFF, 14, 1, 22);

    /* forward button */
    render_fill_rect(30, 4, 22, 20, 0xDDDDDD);
    render_text(34, 8, ">", 0x333333, 0xFFFFFFFF, 14, 1, 22);

    /* url box */
    render_fill_rect(58, 3, FB_W-120, 22, 0xFFFFFF);
    render_fill_rect(58, 3, FB_W-120, 1, URLBAR_BDR);
    render_fill_rect(58, 24, FB_W-120, 1, URLBAR_BDR);
    render_fill_rect(58, 3, 1, 22, URLBAR_BDR);
    render_fill_rect(FB_W-62, 3, 1, 22, URLBAR_BDR);
    render_text(62, 8, url, URLBAR_FG, 0xFFFFFFFF, 14, 0, FB_W-130);

    /* go/loading indicator */
    const char *btn = loading ? "..." : "Go";
    render_fill_rect(FB_W-58, 3, 54, 22, 0x4A90D9);
    render_text(FB_W-50, 8, btn, 0xFFFFFF, 0xFFFFFFFF, 14, 1, 54);
}

/* ── Recursive box renderer ──────────────────────────────────── */
static inline void render_box(LayoutBox *box, int scroll_y) {
    if (!box || !box->node) return;

    DomNode *node = box->node;
    int sx = box->x;
    int sy = box->y - scroll_y + URLBAR_H;  /* apply scroll + urlbar offset */

    /* clip off-screen */
    if (sy + box->h < URLBAR_H) goto children;
    if (sy > FB_H) goto children;

    /* background */
    if (node->type == DOM_ELEMENT && node->bg_color) {
        render_fill_rect(sx, sy, box->w > 0 ? box->w : FB_W, box->h > 0 ? box->h : 2,
                         node->bg_color);
    }

    /* HR */
    if (node->type == DOM_ELEMENT && !html_strcmp(node->tag, "hr")) {
        render_hline(BROWSER_MARGIN, sy, FB_W - BROWSER_MARGIN*2 - SCROLLBAR_W, 0xAAAAAA);
        goto children;
    }

    /* text node */
    if (node->type == DOM_TEXT && box->w > 0) {
        DomNode *par = node->parent;
        unsigned int fg = par ? par->fg_color : 0x000000;
        int fsz  = par ? par->font_size : 16;
        int bold = par ? par->bold : 0;
        int italic = par ? par->italic : 0;
        (void)italic; /* TODO: italic rendering */
        render_text(sx, sy, node->text, fg, 0xFFFFFFFF, fsz, bold,
                    FB_W - sx - SCROLLBAR_W - BROWSER_MARGIN);
    }

    /* underline links */
    if (node->type == DOM_ELEMENT && !html_strcmp(node->tag, "a")) {
        render_hline(sx, sy + node->font_size + 1, box->w, node->fg_color);
    }

    /* list bullet */
    if (node->type == DOM_ELEMENT && !html_strcmp(node->tag, "li")) {
        render_fill_rect(sx - 12, sy + node->font_size/2 - 2, 4, 4, 0x333333);
    }

children:
    for (int i = 0; i < box->child_count; i++)
        render_box(box->children[i], scroll_y);
}

/* ── Present to screen via SoftTail OS GFX syscall ─────────────── */
/* sys_gfx_blit(dx, dy, w, h, pixels, colorkey)
 * ABI: rdi=dx, rsi=dy, rdx=w, r10=h, r8=pixels, r9=colorkey    */
#define SYS_FB_BLIT       312
#define GFX_COLORKEY_NONE 0xFFFFFFFFu

static inline void render_present(void) {
    register long _r10 __asm__("r10") = (long)FB_H;
    register long _r8  __asm__("r8")  = (long)render_fb;
    register long _r9  __asm__("r9")  = (long)GFX_COLORKEY_NONE;
    long r;
    __asm__ volatile("syscall"
        : "=a"(r)
        : "0"((long)SYS_FB_BLIT),
          "D"((long)0),      /* dx */
          "S"((long)0),      /* dy */
          "d"((long)FB_W),   /* w  */
          "r"(_r10),         /* h  */
          "r"(_r8),          /* pixels ptr */
          "r"(_r9)           /* colorkey  */
        : "rcx","r11","memory");
    (void)r;
}

/* Flip page */
#define SYS_GFX_FLIP 310
static inline void render_flip(void) {
    long r;
    __asm__ volatile("syscall":"=a"(r):"0"((long)SYS_GFX_FLIP):
                     "rcx","r11","memory");
    (void)r;
}

/* ── Full page render ────────────────────────────────────────── */
static inline void render_page(LayoutBox *root_box, const char *url,
                                int loading, int content_h) {
    /* background */
    unsigned int page_bg = 0xFFFFFF;
    if (root_box && root_box->node) {
        DomNode *body = dom_find(root_box->node, "body");
        if (body && body->bg_color) page_bg = body->bg_color;
    }
    render_clear(page_bg);

    /* content */
    if (root_box) render_box(root_box, render_scroll_y);

    /* UI chrome */
    render_scrollbar(content_h);
    render_urlbar(url, loading);

    render_present();
    render_flip();
}
