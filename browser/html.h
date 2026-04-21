/* ================================================================
 *  SoftTail OS Browser — browser/html.h
 *  HTML5 tokeniser + DOM tree (Phase 2)
 *
 *  Produces a tree of DomNode structs from raw HTML bytes.
 *  No dynamic library dependencies — uses malloc/free from libc.h.
 * ================================================================ */
#pragma once
#include "../user/libc.h"

/* ── Node types ─────────────────────────────────────────────── */
#define DOM_ELEMENT   1
#define DOM_TEXT      2
#define DOM_COMMENT   3
#define DOM_DOCTYPE   4

/* ── Max limits (keep memory predictable on bare metal) ──────── */
#define DOM_MAX_ATTRS   16
#define DOM_MAX_CHILDREN 128
#define DOM_TAG_MAX     32
#define DOM_ATTR_MAX    64
#define DOM_TEXT_MAX    4096

typedef struct DomAttr {
    char name[DOM_ATTR_MAX];
    char value[DOM_ATTR_MAX];
} DomAttr;

typedef struct DomNode DomNode;
struct DomNode {
    int          type;
    char         tag[DOM_TAG_MAX];      /* element tag name, lowercase */
    char         text[DOM_TEXT_MAX];    /* text/comment content        */
    DomAttr      attrs[DOM_MAX_ATTRS];
    int          attr_count;
    DomNode     *children[DOM_MAX_CHILDREN];
    int          child_count;
    DomNode     *parent;
    /* Computed style (filled by CSS engine) */
    unsigned int bg_color;    /* 0x00RRGGBB, 0 = transparent */
    unsigned int fg_color;    /* 0x00RRGGBB, default 0x000000 */
    int          font_size;   /* px, default 16 */
    int          bold;
    int          italic;
    int          display;     /* 0=block 1=inline 2=none */
};

/* ── Allocator ───────────────────────────────────────────────── */
static inline DomNode *dom_alloc(void) {
    DomNode *n = (DomNode*)malloc(sizeof(DomNode));
    if (!n) return (DomNode*)0;
    /* zero everything */
    char *p = (char*)n;
    for (size_t i = 0; i < sizeof(DomNode); i++) p[i] = 0;
    n->fg_color  = 0x000000;
    n->font_size = 16;
    n->display   = 0; /* block by default */
    return n;
}

static inline void dom_free_tree(DomNode *n) {
    if (!n) return;
    for (int i = 0; i < n->child_count; i++)
        dom_free_tree(n->children[i]);
    free(n);
}

/* ── String helpers ──────────────────────────────────────────── */
static inline int html_isspace(char c) {
    return c==' '||c=='\t'||c=='\n'||c=='\r'||c=='\f';
}
static inline int html_isalpha(char c) {
    return (c>='a'&&c<='z')||(c>='A'&&c<='Z');
}
static inline char html_tolower(char c) {
    return (c>='A'&&c<='Z') ? (char)(c+32) : c;
}
static inline void html_strcpy_lower(char *dst, const char *src, int maxlen) {
    int i = 0;
    while (src[i] && i < maxlen-1) { dst[i] = html_tolower(src[i]); i++; }
    dst[i] = 0;
}
static inline void html_strncpy(char *dst, const char *src, int n) {
    int i = 0;
    while (src[i] && i < n-1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}
static inline int html_strcmp(const char *a, const char *b) {
    while (*a && *b && *a==*b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

/* ── Tokeniser state ─────────────────────────────────────────── */
typedef struct {
    const char *src;
    int         pos;
    int         len;
} HtmlParser;

static inline char hp_peek(HtmlParser *p) {
    return (p->pos < p->len) ? p->src[p->pos] : 0;
}
static inline char hp_get(HtmlParser *p) {
    return (p->pos < p->len) ? p->src[p->pos++] : 0;
}
static inline void hp_skip_spaces(HtmlParser *p) {
    while (p->pos < p->len && html_isspace(p->src[p->pos])) p->pos++;
}

/* Read until ch (exclusive), store in buf up to maxlen-1 */
static inline void hp_read_until(HtmlParser *p, char ch, char *buf, int maxlen) {
    int i = 0;
    while (p->pos < p->len && p->src[p->pos] != ch && i < maxlen-1)
        buf[i++] = p->src[p->pos++];
    buf[i] = 0;
}

/* ── Parse a tag's attributes into node ─────────────────────── */
static inline void hp_parse_attrs(HtmlParser *p, DomNode *node) {
    while (p->pos < p->len) {
        hp_skip_spaces(p);
        char c = hp_peek(p);
        if (c == '>' || c == '/' || c == 0) break;

        if (node->attr_count >= DOM_MAX_ATTRS) {
            /* skip rest */
            while (p->pos < p->len && hp_peek(p) != '>') p->pos++;
            break;
        }

        DomAttr *a = &node->attrs[node->attr_count];

        /* attr name */
        int i = 0;
        while (p->pos < p->len) {
            c = hp_peek(p);
            if (c=='='||c=='>'||c=='/'||html_isspace(c)||c==0) break;
            if (i < DOM_ATTR_MAX-1) a->name[i++] = html_tolower(c);
            p->pos++;
        }
        a->name[i] = 0;
        if (!a->name[0]) break;

        hp_skip_spaces(p);
        if (hp_peek(p) != '=') {
            /* boolean attr */
            a->value[0] = 0;
            node->attr_count++;
            continue;
        }
        p->pos++; /* skip '=' */
        hp_skip_spaces(p);

        /* attr value */
        char quote = hp_peek(p);
        if (quote == '"' || quote == '\'') {
            p->pos++;
            int j = 0;
            while (p->pos < p->len && hp_peek(p) != quote && j < DOM_ATTR_MAX-1)
                a->value[j++] = p->src[p->pos++];
            a->value[j] = 0;
            if (hp_peek(p) == quote) p->pos++;
        } else {
            int j = 0;
            while (p->pos < p->len) {
                c = hp_peek(p);
                if (html_isspace(c)||c=='>'||c==0) break;
                if (j < DOM_ATTR_MAX-1) a->value[j++] = c;
                p->pos++;
            }
            a->value[j] = 0;
        }
        node->attr_count++;
    }
}

/* Get attribute value, returns NULL if not found */
static inline const char *dom_attr(DomNode *n, const char *name) {
    for (int i = 0; i < n->attr_count; i++)
        if (!html_strcmp(n->attrs[i].name, name))
            return n->attrs[i].value;
    return (const char*)0;
}

/* ── Void elements (no closing tag) ─────────────────────────── */
static inline int html_is_void(const char *tag) {
    const char *voids[] = {
        "area","base","br","col","embed","hr","img","input",
        "link","meta","param","source","track","wbr", (const char*)0
    };
    for (int i = 0; voids[i]; i++)
        if (!html_strcmp(tag, voids[i])) return 1;
    return 0;
}

/* ── Main parser ─────────────────────────────────────────────── */

/* forward decl */
static DomNode *hp_parse_node(HtmlParser *p, DomNode *parent);

static inline void hp_add_child(DomNode *parent, DomNode *child) {
    if (!parent || !child) return;
    if (parent->child_count >= DOM_MAX_CHILDREN) { dom_free_tree(child); return; }
    child->parent = parent;
    parent->children[parent->child_count++] = child;
}

/* Parse text content until '<' */
static inline DomNode *hp_parse_text(HtmlParser *p, DomNode *parent) {
    int start = p->pos;
    int i = 0;
    char buf[DOM_TEXT_MAX];
    while (p->pos < p->len && hp_peek(p) != '<' && i < DOM_TEXT_MAX-1) {
        buf[i++] = p->src[p->pos++];
    }
    buf[i] = 0;
    /* trim leading/trailing whitespace for display */
    int s = 0;
    while (buf[s] && html_isspace(buf[s])) s++;
    int e = i - 1;
    while (e > s && html_isspace(buf[e])) e--;
    if (s > e && !buf[s]) return (DomNode*)0; /* all whitespace */

    DomNode *n = dom_alloc();
    if (!n) return (DomNode*)0;
    n->type = DOM_TEXT;
    /* copy trimmed */
    int j = 0;
    for (int k = s; k <= e && j < DOM_TEXT_MAX-1; k++) n->text[j++] = buf[k];
    n->text[j] = 0;
    (void)start;
    return n;
}

/* Parse <!-- comment --> */
static inline DomNode *hp_parse_comment(HtmlParser *p) {
    /* already consumed '<!' */
    /* skip until '-->' */
    DomNode *n = dom_alloc();
    if (!n) return (DomNode*)0;
    n->type = DOM_COMMENT;
    int i = 0;
    while (p->pos + 2 < p->len) {
        if (p->src[p->pos]=='-' && p->src[p->pos+1]=='-' && p->src[p->pos+2]=='>') {
            p->pos += 3; break;
        }
        if (i < DOM_TEXT_MAX-1) n->text[i++] = p->src[p->pos];
        p->pos++;
    }
    n->text[i] = 0;
    return n;
}

/* Parse one tag + its children */
static DomNode *hp_parse_node(HtmlParser *p, DomNode *parent) {
    if (p->pos >= p->len) return (DomNode*)0;

    char c = hp_peek(p);

    /* Text node */
    if (c != '<') return hp_parse_text(p, parent);

    p->pos++; /* skip '<' */
    c = hp_peek(p);

    /* Closing tag — signal caller to stop */
    if (c == '/') return (DomNode*)0;

    /* Comment */
    if (c == '!' ) {
        p->pos++;
        /* DOCTYPE */
        if (p->pos + 6 < p->len &&
            (p->src[p->pos]=='D'||p->src[p->pos]=='d') &&
            (p->src[p->pos+1]=='O'||p->src[p->pos+1]=='o')) {
            while (p->pos < p->len && p->src[p->pos] != '>') p->pos++;
            if (p->pos < p->len) p->pos++;
            DomNode *dt = dom_alloc();
            if (dt) { dt->type = DOM_DOCTYPE; html_strncpy(dt->tag, "!doctype", DOM_TAG_MAX); }
            return dt;
        }
        /* -- comment */
        if (p->pos + 1 < p->len && p->src[p->pos]=='-' && p->src[p->pos+1]=='-') {
            p->pos += 2;
            return hp_parse_comment(p);
        }
        /* skip unknown <! */
        while (p->pos < p->len && p->src[p->pos] != '>') p->pos++;
        if (p->pos < p->len) p->pos++;
        return (DomNode*)0;
    }

    /* Element */
    DomNode *node = dom_alloc();
    if (!node) return (DomNode*)0;
    node->type = DOM_ELEMENT;

    /* Read tag name */
    int i = 0;
    while (p->pos < p->len) {
        c = p->src[p->pos];
        if (html_isspace(c) || c=='>' || c=='/') break;
        if (i < DOM_TAG_MAX-1) node->tag[i++] = html_tolower(c);
        p->pos++;
    }
    node->tag[i] = 0;

    /* Parse attributes */
    hp_parse_attrs(p, node);
    hp_skip_spaces(p);

    /* Self-closing or void */
    if (hp_peek(p) == '/') { p->pos++; }  /* skip '/' */
    if (hp_peek(p) == '>') p->pos++;       /* skip '>' */

    if (html_is_void(node->tag)) return node;

    /* skip script/style content raw */
    if (!html_strcmp(node->tag,"script") || !html_strcmp(node->tag,"style")) {
        char close[12];
        close[0]='<'; close[1]='/';
        int ti = 2;
        for (int k = 0; node->tag[k] && ti < 10; k++) close[ti++] = node->tag[k];
        close[ti] = 0;
        /* store raw content for style tag */
        int ci = 0;
        while (p->pos + ti < p->len) {
            int match = 1;
            for (int k = 0; k < ti; k++) {
                char sc = p->src[p->pos+k];
                char cc = close[k];
                if (sc != cc && !(sc>='A'&&sc<='Z'&&sc+32==cc)) { match = 0; break; }
            }
            if (match) {
                /* skip to '>' */
                while (p->pos < p->len && p->src[p->pos] != '>') p->pos++;
                if (p->pos < p->len) p->pos++;
                break;
            }
            if (!html_strcmp(node->tag,"style") && ci < DOM_TEXT_MAX-1)
                node->text[ci++] = p->src[p->pos];
            p->pos++;
        }
        node->text[ci] = 0;
        return node;
    }

    /* Parse children */
    while (p->pos < p->len) {
        /* Check for closing tag */
        if (hp_peek(p) == '<' && p->pos+1 < p->len && p->src[p->pos+1]=='/') {
            p->pos += 2; /* skip '</' */
            /* skip tag name */
            while (p->pos < p->len && p->src[p->pos] != '>') p->pos++;
            if (p->pos < p->len) p->pos++; /* skip '>' */
            break;
        }
        DomNode *child = hp_parse_node(p, node);
        if (child) hp_add_child(node, child);
        else if (hp_peek(p) == '<' && p->pos+1 < p->len && p->src[p->pos+1]=='/') {
            p->pos += 2;
            while (p->pos < p->len && p->src[p->pos] != '>') p->pos++;
            if (p->pos < p->len) p->pos++;
            break;
        } else if (!child && p->pos < p->len && hp_peek(p) != '<') {
            p->pos++; /* skip stuck char */
        }
    }

    return node;
}

/* ── Public entry point ──────────────────────────────────────── */
/* Parse html_src (null-terminated) and return root DomNode.
 * Caller must dom_free_tree(root) when done. */
static inline DomNode *html_parse(const char *html_src) {
    HtmlParser p;
    p.src = html_src;
    p.pos = 0;
    /* strlen */
    p.len = 0;
    while (html_src[p.len]) p.len++;

    DomNode *root = dom_alloc();
    if (!root) return (DomNode*)0;
    root->type = DOM_ELEMENT;
    html_strncpy(root->tag, "#document", DOM_TAG_MAX);

    while (p.pos < p.len) {
        DomNode *child = hp_parse_node(&p, root);
        if (child) hp_add_child(root, child);
        else if (p.pos < p.len && hp_peek(&p) != '<') p.pos++;
    }
    return root;
}

/* ── Find first element by tag name ─────────────────────────── */
static inline DomNode *dom_find(DomNode *root, const char *tag) {
    if (!root) return (DomNode*)0;
    if (root->type == DOM_ELEMENT && !html_strcmp(root->tag, tag)) return root;
    for (int i = 0; i < root->child_count; i++) {
        DomNode *r = dom_find(root->children[i], tag);
        if (r) return r;
    }
    return (DomNode*)0;
}

/* ── Collect all text inside a subtree ───────────────────────── */
static inline void dom_inner_text(DomNode *n, char *buf, int maxlen, int *pos) {
    if (!n || *pos >= maxlen-1) return;
    if (n->type == DOM_TEXT) {
        int i = 0;
        while (n->text[i] && *pos < maxlen-1) buf[(*pos)++] = n->text[i++];
        if (*pos < maxlen-1) buf[(*pos)++] = ' ';
    }
    for (int i = 0; i < n->child_count; i++)
        dom_inner_text(n->children[i], buf, maxlen, pos);
}
