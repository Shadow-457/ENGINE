/* ================================================================
 *  SoftTail OS Browser — browser/css.h
 *  CSS tokeniser + style resolver (Phase 3)
 *
 *  Parses inline <style> blocks and inline style="" attrs.
 *  Computes final style on each DomNode (bg_color, fg_color,
 *  font_size, bold, italic, display).
 * ================================================================ */
#pragma once
#include "html.h"

/* ── Colour parsing ──────────────────────────────────────────── */
typedef struct { unsigned char r, g, b; int ok; } CssColor;

static inline int css_hex_digit(char c) {
    if (c>='0'&&c<='9') return c-'0';
    if (c>='a'&&c<='f') return c-'a'+10;
    if (c>='A'&&c<='F') return c-'A'+10;
    return -1;
}

static inline CssColor css_parse_color(const char *s) {
    CssColor c; c.r=0; c.g=0; c.b=0; c.ok=0;
    while (*s && html_isspace(*s)) s++;

    /* #rgb or #rrggbb */
    if (s[0]=='#') {
        s++;
        int len = 0; while(s[len] && css_hex_digit(s[len])>=0) len++;
        if (len==3) {
            c.r = (unsigned char)(css_hex_digit(s[0])*17);
            c.g = (unsigned char)(css_hex_digit(s[1])*17);
            c.b = (unsigned char)(css_hex_digit(s[2])*17);
            c.ok=1;
        } else if (len>=6) {
            c.r = (unsigned char)(css_hex_digit(s[0])*16+css_hex_digit(s[1]));
            c.g = (unsigned char)(css_hex_digit(s[2])*16+css_hex_digit(s[3]));
            c.b = (unsigned char)(css_hex_digit(s[4])*16+css_hex_digit(s[5]));
            c.ok=1;
        }
        return c;
    }

    /* Named colours */
    struct { const char *name; unsigned char r,g,b; } named[] = {
        {"white",    255,255,255}, {"black",     0,  0,  0},
        {"red",      255,0,  0  }, {"green",     0,  128,0},
        {"blue",     0,  0,  255}, {"yellow",    255,255,0},
        {"cyan",     0,  255,255}, {"magenta",   255,0,  255},
        {"gray",     128,128,128}, {"grey",      128,128,128},
        {"silver",   192,192,192}, {"orange",    255,165,0},
        {"purple",   128,0,  128}, {"brown",     165,42, 42},
        {"pink",     255,192,203}, {"lightgray", 211,211,211},
        {"darkgray", 169,169,169}, {"navy",      0,  0,  128},
        {"teal",     0,  128,128}, {"maroon",    128,0,  0},
        {"lime",     0,  255,0  }, {"transparent",0,0,0},
        {(const char*)0,0,0,0}
    };
    for (int i = 0; named[i].name; i++) {
        const char *n = named[i].name;
        int j = 0;
        while (n[j] && s[j] && html_tolower(s[j])==n[j]) j++;
        if (!n[j] && (!s[j]||html_isspace(s[j])||s[j]==';'||s[j]==')')) {
            c.r=named[i].r; c.g=named[i].g; c.b=named[i].b; c.ok=1;
            return c;
        }
    }

    /* rgb(r,g,b) */
    if (s[0]=='r'&&s[1]=='g'&&s[2]=='b'&&s[3]=='(') {
        s+=4;
        int rv=0,gv=0,bv=0;
        while(*s&&html_isspace(*s))s++;
        while(*s>='0'&&*s<='9'){rv=rv*10+(*s-'0');s++;}
        while(*s&&(*s==','||html_isspace(*s)))s++;
        while(*s>='0'&&*s<='9'){gv=gv*10+(*s-'0');s++;}
        while(*s&&(*s==','||html_isspace(*s)))s++;
        while(*s>='0'&&*s<='9'){bv=bv*10+(*s-'0');s++;}
        c.r=(unsigned char)rv; c.g=(unsigned char)gv; c.b=(unsigned char)bv; c.ok=1;
        return c;
    }
    return c;
}

static inline unsigned int css_color_to_u32(CssColor c) {
    return ((unsigned int)c.r<<16)|((unsigned int)c.g<<8)|(unsigned int)c.b;
}

/* ── CSS rule ────────────────────────────────────────────────── */
#define CSS_MAX_RULES  256
#define CSS_SEL_MAX    64
#define CSS_PROP_MAX   32
#define CSS_VAL_MAX    128

typedef struct {
    char selector[CSS_SEL_MAX];
    char prop[CSS_PROP_MAX];
    char value[CSS_VAL_MAX];
} CssRule;

typedef struct {
    CssRule rules[CSS_MAX_RULES];
    int     count;
} CssSheet;

/* ── CSS parser ──────────────────────────────────────────────── */
static inline void css_skip_spaces(const char *s, int *i) {
    while (s[*i] && html_isspace(s[*i])) (*i)++;
}

static inline void css_parse_sheet(const char *css, CssSheet *sheet) {
    int i = 0;
    int len = 0; while(css[len]) len++;

    while (i < len && sheet->count < CSS_MAX_RULES) {
        css_skip_spaces(css, &i);
        if (!css[i]) break;

        /* skip comments */
        if (css[i]=='/'&&css[i+1]=='*') {
            i+=2;
            while (i+1<len&&!(css[i]=='*'&&css[i+1]=='/')) i++;
            i+=2; continue;
        }

        /* Read selector */
        char sel[CSS_SEL_MAX]; int si=0;
        while (css[i] && css[i]!='{' && si<CSS_SEL_MAX-1) {
            if (!html_isspace(css[i])) sel[si++]=css[i];
            i++;
        }
        sel[si]=0;
        if (css[i]!='{') break;
        i++; /* skip { */

        /* Read declarations until } */
        while (css[i] && css[i]!='}' && sheet->count < CSS_MAX_RULES) {
            css_skip_spaces(css, &i);
            if (!css[i]||css[i]=='}') break;

            char prop[CSS_PROP_MAX]; int pi=0;
            while (css[i]&&css[i]!=':'&&css[i]!='}'&&pi<CSS_PROP_MAX-1) {
                char ch = css[i++];
                if (!html_isspace(ch)) prop[pi++]=html_tolower(ch);
            }
            prop[pi]=0;
            if (css[i]!=':') { while(css[i]&&css[i]!=';'&&css[i]!='}')i++; if(css[i]==';')i++; continue; }
            i++; /* skip : */
            css_skip_spaces(css, &i);

            char val[CSS_VAL_MAX]; int vi=0;
            while (css[i]&&css[i]!=';'&&css[i]!='}'&&vi<CSS_VAL_MAX-1)
                val[vi++]=css[i++];
            val[vi]=0;
            /* trim trailing spaces */
            while (vi>0&&html_isspace(val[vi-1])) val[--vi]=0;

            if (prop[0] && val[0]) {
                CssRule *r = &sheet->rules[sheet->count++];
                html_strncpy(r->selector, sel, CSS_SEL_MAX);
                html_strncpy(r->prop,     prop, CSS_PROP_MAX);
                html_strncpy(r->value,    val, CSS_VAL_MAX);
            }
            if (css[i]==';') i++;
        }
        if (css[i]=='}') i++;
    }
}

/* ── Inline style parser ─────────────────────────────────────── */
static inline void css_apply_inline(DomNode *node, const char *style) {
    /* "prop: value; prop: value;" */
    int i = 0;
    int len = 0; while(style[len]) len++;
    while (i < len) {
        while(style[i]&&html_isspace(style[i]))i++;
        char prop[CSS_PROP_MAX]; int pi=0;
        while(style[i]&&style[i]!=':'&&pi<CSS_PROP_MAX-1){
            char ch=style[i++];
            if(!html_isspace(ch))prop[pi++]=html_tolower(ch);
        }
        prop[pi]=0;
        if(style[i]!=':'){while(style[i]&&style[i]!=';')i++;if(style[i]==';')i++;continue;}
        i++;
        while(style[i]&&html_isspace(style[i]))i++;
        char val[CSS_VAL_MAX]; int vi=0;
        while(style[i]&&style[i]!=';'&&vi<CSS_VAL_MAX-1)val[vi++]=style[i++];
        val[vi]=0;
        while(vi>0&&html_isspace(val[vi-1]))val[--vi]=0;
        if(style[i]==';')i++;
        if(!prop[0]||!val[0])continue;

        /* Apply property to node */
        if(!html_strcmp(prop,"color")){
            CssColor c=css_parse_color(val);
            if(c.ok) node->fg_color=css_color_to_u32(c);
        } else if(!html_strcmp(prop,"background-color")||!html_strcmp(prop,"background")){
            CssColor c=css_parse_color(val);
            if(c.ok) node->bg_color=css_color_to_u32(c);
        } else if(!html_strcmp(prop,"font-size")){
            int sz=0; int j=0;
            while(val[j]>='0'&&val[j]<='9'){sz=sz*10+(val[j]-'0');j++;}
            if(sz>4&&sz<200) node->font_size=sz;
        } else if(!html_strcmp(prop,"font-weight")){
            if(!html_strcmp(val,"bold")||!html_strcmp(val,"700")||!html_strcmp(val,"800")||!html_strcmp(val,"900"))
                node->bold=1;
            else node->bold=0;
        } else if(!html_strcmp(prop,"font-style")){
            node->italic=(!html_strcmp(val,"italic")||!html_strcmp(val,"oblique"))?1:0;
        } else if(!html_strcmp(prop,"display")){
            if(!html_strcmp(val,"none"))       node->display=2;
            else if(!html_strcmp(val,"inline"))node->display=1;
            else                               node->display=0;
        }
    }
}

/* ── Selector match ──────────────────────────────────────────── */
static inline int css_selector_matches(DomNode *node, const char *sel) {
    if (!node || node->type != DOM_ELEMENT) return 0;
    /* element selector */
    if (!html_strcmp(sel, node->tag)) return 1;
    /* .class */
    if (sel[0]=='.') {
        const char *cls = dom_attr(node, "class");
        if (!cls) return 0;
        const char *want = sel+1;
        /* class can be space-separated list */
        const char *p = cls;
        while (*p) {
            while (*p && html_isspace(*p)) p++;
            int j=0;
            while (want[j] && p[j] && want[j]==p[j]) j++;
            if (!want[j]&&(!p[j]||html_isspace(p[j]))) return 1;
            while (*p && !html_isspace(*p)) p++;
        }
        return 0;
    }
    /* #id */
    if (sel[0]=='#') {
        const char *id = dom_attr(node, "id");
        if (!id) return 0;
        return !html_strcmp(id, sel+1);
    }
    /* * universal */
    if (sel[0]=='*') return 1;
    return 0;
}

/* ── Default browser styles ──────────────────────────────────── */
static inline void css_apply_defaults(DomNode *n) {
    if (n->type != DOM_ELEMENT) return;
    const char *t = n->tag;
    /* headings */
    if (t[0]=='h'&&t[1]>='1'&&t[1]<='6'&&!t[2]) {
        n->bold = 1;
        int sizes[] = {32,28,24,20,18,16};
        n->font_size = sizes[t[1]-'1'];
    }
    /* bold */
    if (!html_strcmp(t,"b")||!html_strcmp(t,"strong")) n->bold=1;
    /* italic */
    if (!html_strcmp(t,"i")||!html_strcmp(t,"em")) n->italic=1;
    /* inline elements */
    if (!html_strcmp(t,"span")||!html_strcmp(t,"a")||
        !html_strcmp(t,"b")||!html_strcmp(t,"i")||
        !html_strcmp(t,"strong")||!html_strcmp(t,"em")||
        !html_strcmp(t,"code")||!html_strcmp(t,"small")||
        !html_strcmp(t,"big")||!html_strcmp(t,"abbr")||
        !html_strcmp(t,"label")||!html_strcmp(t,"img")) {
        n->display = 1;
    }
    /* hide */
    if (!html_strcmp(t,"head")||!html_strcmp(t,"script")||
        !html_strcmp(t,"style")||!html_strcmp(t,"meta")||
        !html_strcmp(t,"link")||!html_strcmp(t,"title")) {
        n->display = 2;
    }
    /* body default */
    if (!html_strcmp(t,"body")) {
        n->bg_color = 0xFFFFFF;
        n->fg_color = 0x000000;
    }
    /* anchor */
    if (!html_strcmp(t,"a")) n->fg_color = 0x0000EE;
    /* code */
    if (!html_strcmp(t,"code")||!html_strcmp(t,"pre")) {
        n->bg_color = 0xF4F4F4;
        n->fg_color = 0x333333;
    }
}

/* ── Apply stylesheet to whole tree ─────────────────────────── */
static inline void css_apply_sheet_tree(DomNode *root, CssSheet *sheet) {
    if (!root) return;
    if (root->type == DOM_ELEMENT) {
        for (int i = 0; i < sheet->count; i++) {
            if (!css_selector_matches(root, sheet->rules[i].selector)) continue;
            /* apply single rule */
            char tmp[CSS_PROP_MAX+CSS_VAL_MAX+4];
            /* build fake inline style string to reuse apply_inline */
            int ti=0;
            const char *p=sheet->rules[i].prop;
            while(*p&&ti<CSS_PROP_MAX-1)tmp[ti++]=*p++;
            tmp[ti++]=':'; tmp[ti++]=' ';
            p=sheet->rules[i].value;
            while(*p&&ti<CSS_PROP_MAX+CSS_VAL_MAX-1)tmp[ti++]=*p++;
            tmp[ti++]=';'; tmp[ti]=0;
            css_apply_inline(root, tmp);
        }
        /* inline style wins over sheet */
        const char *istyle = dom_attr(root, "style");
        if (istyle) css_apply_inline(root, istyle);
    }
    for (int i = 0; i < root->child_count; i++)
        css_apply_sheet_tree(root->children[i], sheet);
}

/* ── Inherit fg_color and font_size from parent ──────────────── */
static inline void css_inherit(DomNode *n, DomNode *parent) {
    if (!n) return;
    if (parent && n->type == DOM_ELEMENT) {
        if (n->fg_color == 0x000000 && parent->fg_color != 0x000000)
            n->fg_color = parent->fg_color;
        if (n->font_size == 16 && parent->font_size != 16)
            n->font_size = parent->font_size;
        if (!n->bold  && parent->bold)  n->bold  = 1;
        if (!n->italic && parent->italic) n->italic = 1;
    }
    for (int i = 0; i < n->child_count; i++)
        css_inherit(n->children[i], n);
}

/* ── Main entry: resolve styles on entire DOM tree ───────────── */
static inline void css_resolve(DomNode *root) {
    if (!root) return;

    /* 1. apply browser defaults (depth-first) */
    /* we apply defaults first, then sheet, then inline */
    /* traverse all nodes */
    typedef void (*ApplyFn)(DomNode*);
    /* inline traversal */
    DomNode *stack[512]; int top=0;
    stack[top++] = root;
    while (top > 0) {
        DomNode *n = stack[--top];
        css_apply_defaults(n);
        for (int i = n->child_count-1; i >= 0; i--)
            if (top < 511) stack[top++] = n->children[i];
    }

    /* 2. find <style> blocks and parse them */
    CssSheet sheet; sheet.count=0;
    /* look for <head><style> */
    DomNode *style_node = dom_find(root, "style");
    if (style_node && style_node->text[0])
        css_parse_sheet(style_node->text, &sheet);

    /* 3. apply sheet + inline styles */
    css_apply_sheet_tree(root, &sheet);

    /* 4. inheritance pass */
    css_inherit(root, (DomNode*)0);
}
