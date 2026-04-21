/* ================================================================
 *  SoftTail OS Browser — browser/layout.h
 *  Box layout engine (Phase 3b)
 *
 *  Walks the styled DOM tree and computes pixel rectangles for
 *  each visible node. Handles block and inline flow.
 * ================================================================ */
#pragma once
#include "css.h"

/* ── Layout box ──────────────────────────────────────────────── */
typedef struct LayoutBox LayoutBox;
struct LayoutBox {
    DomNode    *node;       /* source DOM node */
    int         x, y;      /* top-left pixel position */
    int         w, h;      /* width and height in pixels */
    LayoutBox  *children[DOM_MAX_CHILDREN];
    int         child_count;
};

#define LAYOUT_MAX_BOXES 1024
static LayoutBox *layout_pool = (LayoutBox*)0;  /* heap-allocated in browser_init() */
static int        layout_pool_used = 0;

static inline void layout_pool_reset(void) { layout_pool_used = 0; }

static inline LayoutBox *layout_alloc(void) {
    if (layout_pool_used >= LAYOUT_MAX_BOXES) return (LayoutBox*)0;
    LayoutBox *b = &layout_pool[layout_pool_used++];
    b->node = (DomNode*)0;
    b->x=0; b->y=0; b->w=0; b->h=0;
    b->child_count = 0;
    return b;
}

/* ── Screen dimensions (set by renderer) ─────────────────────── */
#define BROWSER_WIDTH   1024
#define BROWSER_HEIGHT  768
#define BROWSER_MARGIN  8
#define LINE_HEIGHT_MUL 140   /* line height = font_size * 140 / 100 */

/* ── Glyph width estimate (font8x8 = 8px wide) ───────────────── */
static inline int glyph_w(int font_size) {
    /* font8x8 is 8px at size 8; we scale proportionally */
    return (font_size <= 8) ? 8 : (font_size * 8 / 16);
}

/* ── Layout context ──────────────────────────────────────────── */
typedef struct {
    int cursor_x;   /* current inline cursor x */
    int cursor_y;   /* current block cursor y */
    int line_h;     /* tallest inline item on current line */
    int max_w;      /* available width */
    int indent;     /* left indent (for lists etc.) */
} LayoutCtx;

/* Forward declaration */
static int layout_node(DomNode *node, LayoutBox *parent_box,
                        LayoutCtx *ctx, int avail_w);

static inline int measure_text_w(const char *s, int font_size) {
    int len = 0; while(s[len]) len++;
    return len * glyph_w(font_size);
}

/* ── Word-wrap text into multiple lines ─────────────────────── */
/* returns height consumed */
static inline int layout_text(const char *text, int x, int y,
                               int avail_w, int font_size,
                               int *out_end_x, int *out_end_y) {
    int gw = glyph_w(font_size);
    int lh = font_size * LINE_HEIGHT_MUL / 100;
    int cx = x, cy = y;
    int start_y = y;

    const char *p = text;
    while (*p) {
        /* find end of word */
        const char *word_start = p;
        while (*p && *p != ' ' && *p != '\n') p++;
        int wlen = (int)(p - word_start);
        int wpx  = wlen * gw;

        /* does word fit on current line? */
        if (cx + wpx > x + avail_w && cx > x) {
            cx = x;
            cy += lh;
        }
        cx += wpx;
        if (*p == ' ') { cx += gw; p++; }
        if (*p == '\n') { cx = x; cy += lh; p++; }
    }
    *out_end_x = cx;
    *out_end_y = cy;
    return (cy - start_y) + lh;
}

/* ── Layout a single node ────────────────────────────────────── */
static int layout_node(DomNode *node, LayoutBox *parent_box,
                        LayoutCtx *ctx, int avail_w) {
    if (!node) return 0;
    if (node->display == 2) return 0; /* display:none */

    LayoutBox *box = layout_alloc();
    if (!box) return 0;
    box->node = node;

    if (parent_box && parent_box->child_count < DOM_MAX_CHILDREN)
        parent_box->children[parent_box->child_count++] = box;

    /* text node */
    if (node->type == DOM_TEXT) {
        int font_size = (parent_box && parent_box->node) ?
                        parent_box->node->font_size : 16;
        int ex=0, ey=0;
        int h = layout_text(node->text, ctx->cursor_x, ctx->cursor_y,
                            avail_w - (ctx->cursor_x - BROWSER_MARGIN),
                            font_size, &ex, &ey);
        box->x = ctx->cursor_x;
        box->y = ctx->cursor_y;
        box->w = ex - ctx->cursor_x;
        box->h = h;
        /* Update line height */
        if (h > ctx->line_h) ctx->line_h = h;
        ctx->cursor_x = ex;
        if (ey > ctx->cursor_y) {
            ctx->cursor_y = ey;
            ctx->cursor_x = BROWSER_MARGIN;
        }
        return h;
    }

    if (node->type != DOM_ELEMENT) return 0;

    /* Block element */
    int is_inline = (node->display == 1);

    int box_x = is_inline ? ctx->cursor_x : BROWSER_MARGIN + ctx->indent;
    int box_y = is_inline ? ctx->cursor_y : ctx->cursor_y;

    if (!is_inline) {
        /* flush inline line */
        if (ctx->line_h > 0) {
            ctx->cursor_y += ctx->line_h;
            ctx->line_h = 0;
        }
        ctx->cursor_x = box_x;
        box_y = ctx->cursor_y;
    }

    box->x = box_x;
    box->y = box_y;
    box->w = is_inline ? 0 : avail_w;

    /* special block spacing */
    const char *t = node->tag;
    int margin_top    = 0;
    int margin_bottom = 0;
    if (!html_strcmp(t,"p")||!html_strcmp(t,"div")||
        (t[0]=='h'&&t[1]>='1'&&t[1]<='6'&&!t[2])) {
        margin_top = margin_bottom = 8;
    }
    if (!html_strcmp(t,"br")) {
        int lh = node->font_size * LINE_HEIGHT_MUL / 100;
        ctx->cursor_x = BROWSER_MARGIN;
        ctx->cursor_y += lh;
        ctx->line_h = 0;
        box->h = lh;
        return lh;
    }
    if (!html_strcmp(t,"hr")) {
        ctx->cursor_y += 4;
        box->y = ctx->cursor_y;
        box->h = 2;
        ctx->cursor_y += 6;
        ctx->cursor_x = BROWSER_MARGIN;
        return 8;
    }
    if (!html_strcmp(t,"li")) {
        ctx->cursor_x = BROWSER_MARGIN + ctx->indent + 16;
        margin_top = 2; margin_bottom = 2;
    }
    if (!html_strcmp(t,"ul")||!html_strcmp(t,"ol")) {
        ctx->indent += 20;
    }

    ctx->cursor_y += margin_top;
    box->y = ctx->cursor_y;

    /* recurse into children */
    LayoutCtx child_ctx;
    child_ctx.cursor_x = is_inline ? ctx->cursor_x : BROWSER_MARGIN + ctx->indent;
    child_ctx.cursor_y = ctx->cursor_y;
    child_ctx.line_h   = 0;
    child_ctx.max_w    = avail_w;
    child_ctx.indent   = ctx->indent;

    int child_avail = avail_w - ctx->indent;
    for (int i = 0; i < node->child_count; i++) {
        layout_node(node->children[i], box, &child_ctx, child_avail);
    }
    /* flush last inline line */
    if (child_ctx.line_h > 0) {
        child_ctx.cursor_y += child_ctx.line_h;
        child_ctx.line_h = 0;
    }

    if (is_inline) {
        box->w = child_ctx.cursor_x - box->x;
        box->h = child_ctx.cursor_y - box->y + node->font_size;
        ctx->cursor_x = child_ctx.cursor_x;
        ctx->cursor_y = child_ctx.cursor_y;
        ctx->line_h   = child_ctx.line_h;
    } else {
        box->h = child_ctx.cursor_y - box->y;
        ctx->cursor_y = child_ctx.cursor_y + margin_bottom;
        ctx->cursor_x = BROWSER_MARGIN + ctx->indent;
        ctx->line_h = 0;
    }

    if (!html_strcmp(t,"ul")||!html_strcmp(t,"ol")) {
        ctx->indent -= 20;
        if (ctx->indent < 0) ctx->indent = 0;
    }

    return box->h;
}

/* ── Public entry point ──────────────────────────────────────── */
static inline LayoutBox *layout_build(DomNode *root) {
    layout_pool_reset();
    LayoutBox *root_box = layout_alloc();
    if (!root_box) return (LayoutBox*)0;
    root_box->node = root;
    root_box->x = 0; root_box->y = 0;
    root_box->w = BROWSER_WIDTH;
    root_box->h = BROWSER_HEIGHT;

    LayoutCtx ctx;
    ctx.cursor_x = BROWSER_MARGIN;
    ctx.cursor_y = BROWSER_MARGIN;
    ctx.line_h   = 0;
    ctx.max_w    = BROWSER_WIDTH - BROWSER_MARGIN*2;
    ctx.indent   = 0;

    /* find <body> if present, else use root */
    DomNode *body = dom_find(root, "body");
    DomNode *start = body ? body : root;

    for (int i = 0; i < start->child_count; i++)
        layout_node(start->children[i], root_box, &ctx, ctx.max_w);

    root_box->h = ctx.cursor_y + BROWSER_MARGIN;
    return root_box;
}
