/* ================================================================
 *  SystrixLynx — browser/lynx.c
 *  A Lynx-style text-mode web browser for Systrix OS.
 *
 *  Features:
 *   - HTTP fetch via Systrix syscalls
 *   - HTML → plain-text renderer (headings, paragraphs, links, lists)
 *   - Numbered links, keyboard navigation (arrows, Enter, b, g, q)
 *   - Scrollable pager, URL history (back stack)
 *   - VGA 80×25 text display, no ANSI needed
 *
 *  Build (added to Makefile as target "lynx"):
 *    gcc <BCFLAGS> -c -o browser/lynx.o browser/lynx.c
 *    ld  -m elf_x86_64 -static -nostdlib -Ttext=0x400000
 *        -o LYNX user/crt0.o user/libc.o browser/lynx.o
 * ================================================================ */

#include "../user/libc.h"
#include "net.h"           /* http_fetch / browser_fetch / url_parse */

/* ── Terminal dimensions (VGA text mode) ─────────────────────── */
#define COLS        80
#define ROWS        24     /* leave row 24 for status bar */
#define STATUS_ROW  24

/* ── Page buffer ─────────────────────────────────────────────── */
#define PAGE_LINES  2048   /* max rendered lines per page */
#define LINE_LEN    (COLS + 1)

static char page[PAGE_LINES][LINE_LEN];
static int  page_nlines = 0;

/* ── Link table ──────────────────────────────────────────────── */
#define MAX_LINKS   256
typedef struct { char url[512]; char text[64]; int line; } Link;
static Link  links[MAX_LINKS];
static int   nlinks = 0;

/* ── Cursor / scroll ─────────────────────────────────────────── */
static int scroll_top   = 0;   /* first visible line */
static int cur_link     = 0;   /* currently highlighted link (0-based) */

/* ── History stack ───────────────────────────────────────────── */
#define HIST_MAX  32
static char history[HIST_MAX][768];
static int  hist_top = 0;

/* ── Fetch buffer ────────────────────────────────────────────── */
#define FETCH_BUF  (128 * 1024)
static char fetch_buf[FETCH_BUF];

/* ── Current URL ─────────────────────────────────────────────── */
static char cur_url[768];

/* ================================================================
 *  Terminal helpers — write() to stdout
 * ================================================================ */

static void tty_write(const char *s, int n) {
    write(1, s, (size_t)n);
}

static void tty_puts(const char *s) {
    int n = 0;
    while (s[n]) n++;
    tty_write(s, n);
}

static void tty_putc(char c) {
    tty_write(&c, 1);
}

/* Move cursor to row,col (0-based). Uses simple \r and \n only —
 * VGA driver does not support ANSI but we can clear screen and
 * reprint from top each frame. */
static void tty_clear(void) {
    /* Syscall 334 = sys_vga_clear: calls vga_clear() in the kernel,
     * which writes blank cells directly to 0xB8000 and resets the
     * kernel's cur_row/cur_col to 0.  No scroll-buffer side effects. */
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "0"((long)334)
                     : "rcx", "r11", "memory");
}

/* ================================================================
 *  HTML renderer
 *  Converts raw HTML into page[][] lines, fills links[].
 * ================================================================ */

/* Append character to current render line, wrapping at COLS */
static int cur_col = 0;
static int cur_line = 0;
static int in_pre = 0;
static int suppress_ws = 1;  /* collapse whitespace outside <pre> */

static void flush_line(void) {
    if (cur_line >= PAGE_LINES - 1) return;
    page[cur_line][cur_col] = '\0';
    cur_line++;
    cur_col = 0;
    suppress_ws = 1;
}

static void emit_char(char c) {
    if (cur_line >= PAGE_LINES - 1) return;
    if (!in_pre) {
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
        if (c == ' ' && suppress_ws) return;
        suppress_ws = (c == ' ');
    } else {
        if (c == '\n') { flush_line(); return; }
        if (c == '\r') return;
    }
    page[cur_line][cur_col++] = c;
    if (cur_col >= COLS) flush_line();
}

static void emit_str(const char *s) {
    while (*s) emit_char(*s++);
}

static void emit_newline(void) {
    if (cur_col > 0) flush_line();
    else if (cur_line > 0 && page[cur_line-1][0] != '\0') flush_line();
}

static void emit_blank(void) {
    emit_newline();
    if (cur_line > 0 && page[cur_line-1][0] != '\0') flush_line();
}

/* Decode a few HTML entities in-place (returns static buf) */
static char ent_buf[8];
static const char *decode_entity(const char *p, int *adv) {
    /* p points to '&', returns replacement string, *adv = chars consumed */
    if (p[1] == '#') {
        int val = 0; int i = 2;
        if (p[2] == 'x' || p[2] == 'X') {
            i = 3;
            while ((p[i] >= '0' && p[i] <= '9') ||
                   (p[i] >= 'a' && p[i] <= 'f') ||
                   (p[i] >= 'A' && p[i] <= 'F')) {
                int d = p[i] >= 'a' ? p[i]-'a'+10 :
                        p[i] >= 'A' ? p[i]-'A'+10 : p[i]-'0';
                val = val*16 + d; i++;
            }
        } else {
            while (p[i] >= '0' && p[i] <= '9') { val = val*10+(p[i]-'0'); i++; }
        }
        if (p[i] == ';') i++;
        *adv = i;
        ent_buf[0] = (val > 31 && val < 127) ? (char)val : '?';
        ent_buf[1] = '\0';
        return ent_buf;
    }
    /* Named entities */
    struct { const char *name; const char *val; } ents[] = {
        {"amp;","&"},{"lt;","<"},{"gt;",">"},{"quot;","\""},
        {"apos;","'"},{"nbsp;"," "},{"mdash;","--"},{"ndash;","-"},
        {"laquo;","<<"},{"raquo;",">>"},{"copy;","(c)"},{"reg;","(r)"},
        {"trade;","(tm)"},{"hellip;","..."},{"bull;","* "},
        {"eacute;","e"},{"egrave;","e"},{"ecirc;","e"},
        {"agrave;","a"},{"aacute;","a"},{"acirc;","a"},
        {"ugrave;","u"},{"uacute;","u"},{"ocirc;","o"},
        {"ccedil;","c"},{"szlig;","ss"},{NULL,NULL}
    };
    for (int i = 0; ents[i].name; i++) {
        const char *n = ents[i].name;
        int j = 1;
        while (n[j-1] && p[j] == n[j-1]) j++;
        if (!n[j-1]) { *adv = j+1; return ents[i].val; }
    }
    *adv = 1; ent_buf[0] = '&'; ent_buf[1] = '\0'; return ent_buf;
}

/* Skip a tag starting at '<', return pointer past '>' */
static const char *skip_tag(const char *p) {
    while (*p && *p != '>') p++;
    if (*p == '>') p++;
    return p;
}

/* Read tag name from '<' (or after '<'), lower-case, into buf */
static void get_tagname(const char *p, char *buf, int bufsz) {
    if (*p == '<') p++;
    if (*p == '/') p++;
    int i = 0;
    while (*p && *p != '>' && *p != ' ' && *p != '\t' &&
           *p != '\r' && *p != '\n' && i < bufsz-1) {
        char c = *p++;
        buf[i++] = (c >= 'A' && c <= 'Z') ? c+32 : c;
    }
    buf[i] = '\0';
}

/* Get attribute value from tag string, e.g. attr="value" */
static void get_attr(const char *tag, const char *attr, char *out, int outsz) {
    out[0] = '\0';
    int alen = 0; while (attr[alen]) alen++;
    const char *p = tag;
    while (*p) {
        /* case-insensitive attr match */
        int match = 1;
        for (int i = 0; i < alen && p[i]; i++) {
            char c = p[i]; if (c>='A'&&c<='Z') c+=32;
            if (c != attr[i]) { match = 0; break; }
        }
        if (match && p[alen] == '=') {
            p += alen + 1;
            char q = 0;
            if (*p == '"' || *p == '\'') { q = *p++; }
            int i = 0;
            while (*p && i < outsz-1) {
                if (q && *p == q) break;
                if (!q && (*p==' '||*p=='>'||*p=='\t')) break;
                out[i++] = *p++;
            }
            out[i] = '\0';
            return;
        }
        p++;
    }
}

/* Add a link entry */
static void add_link(const char *url, const char *base_url) {
    if (nlinks >= MAX_LINKS) return;
    char resolved[512];
    /* Resolve relative URLs */
    if (url[0] == '/') {
        /* absolute path: prepend scheme+host from base */
        ParsedUrl pu;
        url_parse(base_url, &pu);
        snprintf(resolved, sizeof(resolved), "%s://%s%s", pu.scheme, pu.host, url);
    } else if (url[0] == '#') {
        /* fragment: same page */
        snprintf(resolved, sizeof(resolved), "%s%s", base_url, url);
    } else if (!strncmp(url, "http://", 7) || !strncmp(url, "https://", 8)) {
        snprintf(resolved, sizeof(resolved), "%s", url);
    } else if (!strncmp(url, "javascript:", 11) || !strncmp(url, "mailto:", 7)) {
        return; /* skip */
    } else {
        /* relative path: append to base directory */
        ParsedUrl pu;
        url_parse(base_url, &pu);
        /* find last slash in path */
        int plen = 0; while (pu.path[plen]) plen++;
        int slash = 0;
        for (int i = plen-1; i >= 0; i--) {
            if (pu.path[i] == '/') { slash = i; break; }
        }
        char dir[512];
        int di = 0;
        for (int i = 0; i <= slash && di < 511; i++) dir[di++] = pu.path[i];
        dir[di] = '\0';
        snprintf(resolved, sizeof(resolved), "%s://%s%s%s",
                 pu.scheme, pu.host, dir, url);
    }
    snprintf(links[nlinks].url, sizeof(links[nlinks].url), "%s", resolved);
    links[nlinks].line = cur_line;
    nlinks++;
}

/* ── Main HTML→text render ───────────────────────────────────── */
static void render_html(const char *html, const char *base_url) {
    /* Reset render state */
    cur_col = 0; cur_line = 0; in_pre = 0; suppress_ws = 1;
    nlinks = 0;
    for (int i = 0; i < PAGE_LINES; i++) page[i][0] = '\0';

    int in_head    = 0;
    int in_script  = 0;
    int in_style   = 0;
    int in_a       = 0;
    int link_idx   = -1;
    char link_url[512];
    char link_text[64];
    int  lt_pos    = 0;
    int  li_indent = 0;
    int  ol_count  = 0;
    int  in_ol     = 0;

    const char *p = html;
    while (*p) {
        if (*p == '<') {
            /* Tag */
            const char *tag_start = p;
            char tname[32];
            get_tagname(p, tname, sizeof(tname));
            int is_close = (p[1] == '/');

            if (!strcmp(tname, "head"))   { in_head   = !is_close; }
            if (!strcmp(tname, "script")) { in_script = !is_close; }
            if (!strcmp(tname, "style"))  { in_style  = !is_close; }

            if (!in_head && !in_script && !in_style) {
                if (!strcmp(tname, "pre") || !strcmp(tname, "code")) {
                    in_pre = !is_close;
                    if (!is_close) { emit_blank(); }
                    else { emit_blank(); }
                }
                else if (!strcmp(tname, "br")) { emit_newline(); }
                else if (!strcmp(tname, "p") || !strcmp(tname, "div") ||
                         !strcmp(tname, "section") || !strcmp(tname, "article") ||
                         !strcmp(tname, "main") || !strcmp(tname, "header") ||
                         !strcmp(tname, "footer") || !strcmp(tname, "nav") ||
                         !strcmp(tname, "aside")) {
                    emit_blank();
                }
                else if (!strcmp(tname, "h1") || !strcmp(tname, "h2") ||
                         !strcmp(tname, "h3") || !strcmp(tname, "h4") ||
                         !strcmp(tname, "h5") || !strcmp(tname, "h6")) {
                    if (!is_close) {
                        emit_blank();
                        /* underline-style heading prefix */
                        int level = tname[1] - '0';
                        if (level == 1)      emit_str("=== ");
                        else if (level == 2) emit_str("--- ");
                        else                 emit_str("  * ");
                    } else {
                        emit_blank();
                    }
                }
                else if (!strcmp(tname, "li")) {
                    if (!is_close) {
                        emit_newline();
                        if (in_ol) {
                            char num[8];
                            int n = ++ol_count;
                            int i = 0;
                            if (n >= 100) num[i++] = '0' + n/100;
                            if (n >= 10)  num[i++] = '0' + (n%100)/10;
                            num[i++] = '0' + n%10;
                            num[i++] = '.'; num[i++] = ' '; num[i] = '\0';
                            for (int j=0;j<li_indent;j++) emit_char(' ');
                            emit_str(num);
                        } else {
                            for (int j=0;j<li_indent;j++) emit_char(' ');
                            emit_str("* ");
                        }
                    }
                }
                else if (!strcmp(tname, "ul")) {
                    if (!is_close) { emit_newline(); li_indent += 2; }
                    else { li_indent -= 2; if (li_indent<0) li_indent=0; emit_newline(); }
                }
                else if (!strcmp(tname, "ol")) {
                    if (!is_close) { emit_newline(); li_indent += 2; in_ol++; ol_count=0; }
                    else { li_indent -= 2; if (li_indent<0) li_indent=0; if(in_ol>0)in_ol--; emit_newline(); }
                }
                else if (!strcmp(tname, "tr")) {
                    if (is_close) emit_newline();
                }
                else if (!strcmp(tname, "td") || !strcmp(tname, "th")) {
                    if (!is_close) emit_str("  ");
                    else emit_str(" |");
                }
                else if (!strcmp(tname, "hr")) {
                    emit_newline();
                    for (int i = 0; i < COLS; i++) emit_char('-');
                    emit_newline();
                }
                else if (!strcmp(tname, "title")) {
                    /* skip title content */
                    if (!is_close) {
                        p = skip_tag(p);
                        while (*p && !(*p=='<' && p[1]=='/' &&
                               (p[2]=='t'||p[2]=='T') &&
                               (p[3]=='i'||p[3]=='I'))) p++;
                        continue;
                    }
                }
                else if (!strcmp(tname, "a")) {
                    if (!is_close) {
                        /* extract href */
                        const char *ts = tag_start + 1;
                        char href[512]; href[0]='\0';
                        get_attr(ts, "href", href, sizeof(href));
                        if (href[0]) {
                            in_a = 1;
                            link_idx = nlinks;
                            lt_pos = 0; link_text[0]='\0';
                            snprintf(link_url, sizeof(link_url), "%s", href);
                            add_link(href, base_url);
                            /* emit link marker [N] */
                            char marker[8];
                            int n = link_idx + 1;
                            int i = 0;
                            marker[i++]='[';
                            if (n>=100) marker[i++]='0'+n/100;
                            if (n>=10)  marker[i++]='0'+(n%100)/10;
                            marker[i++]='0'+n%10;
                            marker[i++]=']'; marker[i]='\0';
                            emit_str(marker);
                        }
                    } else {
                        in_a = 0; link_idx = -1;
                    }
                }
                else if (!strcmp(tname, "img")) {
                    /* emit alt text */
                    const char *ts = tag_start + 1;
                    char alt[64]; alt[0]='\0';
                    get_attr(ts, "alt", alt, sizeof(alt));
                    if (alt[0]) { emit_char('['); emit_str(alt); emit_char(']'); }
                    else emit_str("[img]");
                }
            }
            p = skip_tag(p);
            (void)tag_start;
            (void)link_url;
            (void)link_text;
            (void)lt_pos;
        }
        else if (*p == '&') {
            if (!in_head && !in_script && !in_style) {
                int adv = 0;
                const char *r = decode_entity(p, &adv);
                emit_str(r);
                p += adv;
            } else p++;
        }
        else {
            if (!in_head && !in_script && !in_style) {
                emit_char(*p);
                /* track link text for display (not used visually here) */
                (void)link_text;
            }
            p++;
        }
    }
    /* Flush last line */
    if (cur_col > 0) flush_line();
    page_nlines = cur_line;
}

/* ================================================================
 *  Display
 * ================================================================ */

/* Pad / truncate string to exactly 'w' chars, write to buffer */
static void fmt_line(char *out, const char *src, int w) {
    int i = 0;
    while (i < w && src[i]) { out[i] = src[i]; i++; }
    while (i < w) out[i++] = ' ';
    out[w] = '\0';
}

static void draw_screen(void) {
    tty_clear();
    /* Draw ROWS content lines */
    char line[COLS + 2];
    for (int r = 0; r < ROWS; r++) {
        int li = scroll_top + r;
        if (li < page_nlines) {
            fmt_line(line, page[li], COLS);
        } else {
            for (int i = 0; i < COLS; i++) line[i] = ' ';
            line[COLS] = '\0';
        }
        tty_puts(line);
        tty_putc('\n');
    }
    /* Status bar — exactly COLS printable ASCII chars + newline */
    char status[COLS + 2];
    int total = page_nlines > 0 ? page_nlines : 1;
    int pct = (scroll_top + ROWS) * 100 / total;
    if (pct > 100) pct = 100;
    /* Pre-fill with spaces so no uninitialised or high bytes leak */
    for (int i = 0; i < COLS; i++) status[i] = ' ';
    status[COLS]     = '\n';
    status[COLS + 1] = '\0';
    /* Build label into tmp, then copy only 32-126 into status */
    char tmp[COLS + 1];
    snprintf(tmp, sizeof(tmp),
             " LYNX: %-40s  [%d lnk] %3d%%  q:quit b:back g:URL",
             cur_url[0] ? cur_url : "(no page)", nlinks, pct);
    for (int i = 0; i < COLS && tmp[i]; i++) {
        unsigned char ch = (unsigned char)tmp[i];
        status[i] = (ch >= 32 && ch < 127) ? (char)ch : ' ';
    }
    tty_write(status, COLS + 1);
    /* Prompt on very next line (will be overwritten next draw) */
}

/* ================================================================
 *  Navigation helpers
 * ================================================================ */

static void scroll_to_link(int lnk_idx) {
    if (lnk_idx < 0 || lnk_idx >= nlinks) return;
    int line = links[lnk_idx].line;
    if (line < scroll_top || line >= scroll_top + ROWS) {
        scroll_top = line - ROWS / 2;
        if (scroll_top < 0) scroll_top = 0;
        if (scroll_top + ROWS > page_nlines)
            scroll_top = page_nlines - ROWS;
        if (scroll_top < 0) scroll_top = 0;
    }
}

static void hist_push(const char *url) {
    if (hist_top < HIST_MAX) {
        snprintf(history[hist_top], sizeof(history[hist_top]), "%s", url);
        hist_top++;
    }
}

static int hist_pop(char *url_out, int outsz) {
    if (hist_top <= 1) return 0;
    hist_top--;
    snprintf(url_out, (size_t)outsz, "%s", history[hist_top - 1]);
    return 1;
}

/* ================================================================
 *  Page load
 * ================================================================ */

static void show_error(const char *msg) {
    cur_col=0; cur_line=0; in_pre=0; suppress_ws=1; nlinks=0;
    for (int i=0;i<PAGE_LINES;i++) page[i][0]='\0';
    emit_str("ERROR: ");
    emit_str(msg);
    emit_newline();
    emit_blank();
    emit_str("Press 'b' to go back, 'g' to enter a URL, 'q' to quit.");
    emit_newline();
    if (cur_col>0) flush_line();
    page_nlines = cur_line;
    scroll_top = 0; cur_link = 0;
}

static void show_help(void) {
    cur_col=0; cur_line=0; in_pre=0; suppress_ws=1; nlinks=0;
    for (int i=0;i<PAGE_LINES;i++) page[i][0]='\0';

    emit_str("=== SystrixLynx — Keyboard Reference ===");
    emit_newline(); emit_blank();
    emit_str("  UP/DOWN   Scroll one line"); emit_newline();
    emit_str("  PGUP/PGDN Scroll one page"); emit_newline();
    emit_str("  g         Enter a URL"); emit_newline();
    emit_str("  1-9       Jump to link number"); emit_newline();
    emit_str("  ENTER     Follow current link"); emit_newline();
    emit_str("  TAB       Move to next link"); emit_newline();
    emit_str("  b / BS    Go back"); emit_newline();
    emit_str("  r         Reload page"); emit_newline();
    emit_str("  h/?       This help screen"); emit_newline();
    emit_str("  q         Quit"); emit_newline();
    emit_blank();
    emit_str("Links are shown as [N] inline. Press the number to follow,");
    emit_newline();
    emit_str("or TAB/ENTER to navigate through them."); emit_newline();
    emit_blank();
    emit_str("To start browsing, press g and type a URL:");
    emit_newline();
    emit_str("  http://example.com"); emit_newline();
    if (cur_col>0) flush_line();
    page_nlines = cur_line;
    scroll_top = 0; cur_link = 0;
}

static int load_url(const char *url) {
    /* Show loading indicator */
    tty_clear();
    tty_puts("Loading: ");
    tty_puts(url);
    tty_puts("\r\nPlease wait...\r\n");

    int n = browser_fetch(url, fetch_buf, FETCH_BUF - 1);
    if (n < 0) {
        show_error("Failed to fetch URL. Check network or URL format.");
        return 0;
    }
    fetch_buf[n] = '\0';

    /* Check if response is HTML */
    int is_html = 0;
    /* Look for DOCTYPE or <html */
    for (int i = 0; i < n - 5 && i < 512; i++) {
        if ((fetch_buf[i]=='<' && (fetch_buf[i+1]=='!'||fetch_buf[i+1]=='h'||fetch_buf[i+1]=='H'))) {
            is_html = 1; break;
        }
    }

    if (is_html) {
        render_html(fetch_buf, url);
    } else {
        /* Plain text: split into lines */
        cur_col=0; cur_line=0; in_pre=1; suppress_ws=0; nlinks=0;
        for (int i=0;i<PAGE_LINES;i++) page[i][0]='\0';
        for (int i = 0; i < n; i++) emit_char(fetch_buf[i]);
        if (cur_col>0) flush_line();
        in_pre=0;
        page_nlines = cur_line;
    }

    snprintf(cur_url, sizeof(cur_url), "%s", url);
    scroll_top = 0;
    cur_link   = 0;
    return 1;
}

/* ================================================================
 *  Input — read URL from terminal
 * ================================================================ */

/* Read a line using sys_read_char (syscall 335) — blocking single-
 * char input that pumps the PS/2 hardware directly, no ring needed. */
static int read_line(char *buf, int bufsz) {
    int n = 0;
    buf[0] = '\0';
    tty_puts("URL: ");

    for (;;) {
        long c;
        __asm__ volatile("syscall" : "=a"(c) : "0"((long)335)
                         : "rcx", "r11", "memory");
        if (c <= 0) continue;
        unsigned char asc = (unsigned char)c;

        /* Enter / Return */
        if (asc == '\r' || asc == '\n') {
            tty_putc('\n');
            buf[n] = '\0';
            return n;
        }
        /* Backspace */
        if (asc == 8 || asc == 127) {
            if (n > 0) {
                n--;
                tty_write("\b \b", 3);
            }
            continue;
        }
        /* Escape — cancel */
        if (asc == 27) {
            tty_putc('\n');
            buf[0] = '\0';
            return 0;
        }
        /* Printable ASCII */
        if (asc >= 32 && asc < 127 && n < bufsz - 1) {
            buf[n++] = (char)asc;
            tty_putc((char)asc);
        }
    }
}

/* ================================================================
 *  Keyboard scancodes via poll_keys()
 * ================================================================ */
#define SC_UP     0x48
#define SC_DOWN   0x50
#define SC_LEFT   0x4B
#define SC_RIGHT  0x4D
#define SC_PGUP   0x49
#define SC_PGDN   0x51
#define SC_ENTER  0x1C
#define SC_TAB    0x0F
#define SC_ESC    0x01
#define SC_BKSP   0x0E


static int get_key(void) {
    /* Syscall 335 = sys_read_char: calls read_key_raw() in the kernel.
     * Blocks until a key is pressed, returns ASCII or KEY_* private code.
     * This works even when the shell loop isn't running to pump the ring. */
    long c;
    __asm__ volatile("syscall" : "=a"(c) : "0"((long)335)
                     : "rcx", "r11", "memory");
    if (c == 0) return 0;
    /* Map kernel KEY_* private codes (0x80–0x83) to negative scancodes
     * so the existing switch in main() still works. */
    if (c == 0x80) return -SC_UP;
    if (c == 0x81) return -SC_DOWN;
    if (c == 0x82) return -SC_LEFT;    /* KEY_LEFT  */
    if (c == 0x83) return -SC_RIGHT;   /* KEY_RIGHT */
    if (c == 0x84) return -SC_PGUP;    /* KEY_PGUP  */
    if (c == 0x85) return -SC_PGDN;    /* KEY_PGDN  */
    return (int)(unsigned char)c;
}

/* ================================================================
 *  Main
 * ================================================================ */

int main(void) {
    tty_clear();
    show_help();
    draw_screen();

    char goto_buf[768];
    int  reload_url_valid = 0;

    for (;;) {
        /* get_key() calls sys_read_char (335) which blocks in the kernel
         * until a key is pressed — no busy-spin or yield needed. */
        int k = get_key();
        if (k == 0) continue;

        if (k == 'q' || k == 'Q') break;

        else if (k == 'h' || k == 'H' || k == '?') {
            show_help();
            draw_screen();
        }

        else if (k == 'g' || k == 'G') {
            /* Go to URL */
            tty_clear();
            tty_puts("Enter URL (e.g. http://example.com):\r\n");
            if (read_line(goto_buf, sizeof(goto_buf)) > 0) {
                /* Default to http:// if no scheme */
                if (strncmp(goto_buf, "http://", 7) &&
                    strncmp(goto_buf, "https://", 8)) {
                    char tmp[768];
                    snprintf(tmp, sizeof(tmp), "http://%s", goto_buf);
                    snprintf(goto_buf, sizeof(goto_buf), "%s", tmp);
                }
                if (cur_url[0]) hist_push(cur_url);
                if (load_url(goto_buf)) {
                    reload_url_valid = 1;
                }
            }
            draw_screen();
        }

        else if (k == 'r' || k == 'R') {
            /* Reload */
            if (reload_url_valid && cur_url[0]) {
                load_url(cur_url);
                draw_screen();
            }
        }

        else if (k == 'b' || k == 'B' || k == -SC_BKSP || k == 8) {
            /* Back */
            if (hist_pop(goto_buf, sizeof(goto_buf))) {
                load_url(goto_buf);
                /* Don't push to history — already there */
                if (hist_top > 0) hist_top--; /* undo push inside load... */
                snprintf(cur_url, sizeof(cur_url), "%s", goto_buf);
                reload_url_valid = 1;
            } else {
                show_help();
            }
            draw_screen();
        }

        else if (k == -SC_UP || k == 'k') {
            if (scroll_top > 0) { scroll_top--; draw_screen(); }
        }
        else if (k == -SC_DOWN || k == 'j') {
            if (scroll_top + ROWS < page_nlines) { scroll_top++; draw_screen(); }
        }
        else if (k == -SC_PGUP) {
            scroll_top -= ROWS;
            if (scroll_top < 0) scroll_top = 0;
            draw_screen();
        }
        else if (k == -SC_PGDN) {
            scroll_top += ROWS;
            if (scroll_top + ROWS > page_nlines)
                scroll_top = page_nlines - ROWS;
            if (scroll_top < 0) scroll_top = 0;
            draw_screen();
        }

        else if (k == -SC_TAB || k == '\t') {
            /* Next link */
            if (nlinks > 0) {
                cur_link = (cur_link + 1) % nlinks;
                scroll_to_link(cur_link);
                draw_screen();
            }
        }

        else if (k == -SC_ENTER || k == '\r' || k == '\n') {
            /* Follow current highlighted link */
            if (nlinks > 0 && cur_link < nlinks) {
                hist_push(cur_url);
                if (load_url(links[cur_link].url)) reload_url_valid = 1;
                draw_screen();
            }
        }

        else if (k >= '1' && k <= '9') {
            /* Direct link number */
            int num = k - '0';
            /* Read second digit? Check next key immediately */
            /* For simplicity, support 1-9 directly */
            if (num > 0 && num <= nlinks) {
                cur_link = num - 1;
                hist_push(cur_url);
                if (load_url(links[cur_link].url)) reload_url_valid = 1;
                draw_screen();
            }
        }

        else if (k == '0') {
            /* Link 10 */
            if (nlinks >= 10) {
                cur_link = 9;
                hist_push(cur_url);
                if (load_url(links[cur_link].url)) reload_url_valid = 1;
                draw_screen();
            }
        }
    }

    tty_clear();
    tty_puts("Goodbye from SystrixLynx!\r\n");
    return 0;
}
