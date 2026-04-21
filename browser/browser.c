/* ================================================================
 *  SoftTail OS Browser — browser/browser.c
 *  Main entry point — event loop, input, navigation
 *
 *  Compile:
 *    gcc -m64 -O2 -ffreestanding -fno-stack-protector -mno-red-zone
 *        -nostdlib -nostdinc -Iinclude -Iuser -Ibrowser
 *        -c -o browser/browser.o browser/browser.c
 *
 *  Link:
 *    ld -m elf_x86_64 -static -nostdlib -Ttext=0x400000
 *       -o BROWSER user/crt0.o user/libc.o browser/browser.o
 * ================================================================ */
#include "../user/libc.h"
#include "net.h"
#include "html.h"
#include "css.h"
#include "layout.h"
#include "render.h"

/* ── Input syscalls ──────────────────────────────────────────── */
/* SoftTail OS syscall table (kernel/isr.S):
 *   300 = sys_poll_keys  (used via libc.h poll_keys())
 *   301 = sys_poll_mouse (used via libc.h poll_mouse())
 *   303 = sys_yield
 */
#define SYS_YIELD  303

static inline void sys_yield(void) {
    long r;
    __asm__ volatile("syscall":"=a"(r):"0"((long)SYS_YIELD):"rcx","r11","memory");
    (void)r;
}

/* Key codes */
#define KEY_UP     0x48
#define KEY_DOWN   0x50
#define KEY_PGUP   0x49
#define KEY_PGDN   0x51
#define KEY_ENTER  0x1C
#define KEY_ESC    0x01
#define KEY_BKSP   0x0E

/* ── Input — use libc.h poll_keys / poll_mouse directly ─────── */
/* libc.h already defines KeyEvent, MouseEvent, poll_keys(),
 * poll_mouse() with the correct syscall numbers (300, 301).      */

/* Absolute mouse state (accumulate relative deltas from SoftTail)  */
typedef struct { int x; int y; int buttons; } MouseState;
static MouseState g_mouse = {512, 384, 0};  /* start centre screen */

/* input_get_key — returns PS/2 scancode, or 0 if ring empty     */
static KeyEvent _key_buf[8];
static int      _key_head = 0, _key_count = 0;

static inline int input_get_key(void) {
    if (_key_head >= _key_count) {
        long n = poll_keys(_key_buf, 8);
        if (n <= 0) return 0;
        _key_count = (int)n;
        _key_head  = 0;
    }
    return (int)_key_buf[_key_head++].scancode;
}

/* input_get_mouse — fills MouseState with current absolute pos   */
static MouseEvent _mou_buf[8];

static inline void input_get_mouse(MouseState *out) {
    long n = poll_mouse(_mou_buf, 8);
    for (long i = 0; i < n; i++) {
        g_mouse.x += (int)_mou_buf[i].dx;
        g_mouse.y -= (int)_mou_buf[i].dy;  /* SoftTail dy: up=positive */
        g_mouse.buttons = (int)_mou_buf[i].buttons;
        if (g_mouse.x < 0)     g_mouse.x = 0;
        if (g_mouse.x >= FB_W) g_mouse.x = FB_W - 1;
        if (g_mouse.y < 0)     g_mouse.y = 0;
        if (g_mouse.y >= FB_H) g_mouse.y = FB_H - 1;
    }
    *out = g_mouse;
}

/* ── Browser state ───────────────────────────────────────────── */
#define URL_MAX    512
#define BUF_SIZE   (512*1024)

static char  current_url[URL_MAX];
static char  url_bar[URL_MAX];
static int   url_bar_len = 0;
static int   url_editing = 0;

static char   *html_buf = (char*)0;  /* heap-allocated in browser_init() */
static DomNode *dom_root   = (DomNode*)0;
static LayoutBox *layout_root = (LayoutBox*)0;
static int     content_h   = 0;
static int     loading     = 0;

/* ── History (simple stack) ──────────────────────────────────── */
#define HIST_MAX 16
static char history[HIST_MAX][URL_MAX];
static int  hist_top = 0;
static int  hist_cur = -1;

static inline void hist_push(const char *url) {
    hist_cur++;
    if (hist_cur >= HIST_MAX) {
        for (int i=0;i<HIST_MAX-1;i++) {
            for (int j=0;j<URL_MAX;j++) history[i][j]=history[i+1][j];
        }
        hist_cur = HIST_MAX-1;
    }
    for (int i=0;url[i]&&i<URL_MAX-1;i++) history[hist_cur][i]=url[i];
    history[hist_cur][URL_MAX-1]=0;
    hist_top = hist_cur;
}

/* ── Navigate to URL ─────────────────────────────────────────── */
static void navigate(const char *url) {
    /* copy url to current */
    int i=0;
    while(url[i]&&i<URL_MAX-1){current_url[i]=url[i];i++;}
    current_url[i]=0;

    for(int j=0;j<URL_MAX;j++) url_bar[j]=current_url[j];
    url_bar_len=i;
    url_editing=0;
    loading=1;

    /* render "loading" frame */
    if (layout_root) render_page(layout_root, current_url, 1, content_h);

    /* Free old DOM */
    if (dom_root) { dom_free_tree(dom_root); dom_root=(DomNode*)0; }

    /* Fetch page */
    int len = browser_fetch(current_url, html_buf, BUF_SIZE-1);
    loading = 0;

    if (len <= 0) {
        /* Show error page */
        static const char err_html[] =
            "<html><body style='background:#fff;color:#c00;font-size:18;'>"
            "<h1>Cannot load page</h1>"
            "<p>Could not connect to server.</p>"
            "<p>Check that SoftTail OS networking is running.</p>"
            "</body></html>";
        for (int j=0; err_html[j]; j++) html_buf[j]=err_html[j];
        html_buf[sizeof(err_html)-1]=0;
    }

    /* Parse + style + layout */
    dom_root    = html_parse(html_buf);
    css_resolve(dom_root);
    layout_root = layout_build(dom_root);
    content_h   = layout_root ? layout_root->h : BROWSER_HEIGHT;
    render_scroll_y = 0;

    hist_push(current_url);

    /* Draw */
    render_page(layout_root, current_url, 0, content_h);
}

/* ── Click hit test ──────────────────────────────────────────── */
static const char *find_link_at(LayoutBox *box, int mx, int my) {
    if (!box||!box->node) return (const char*)0;
    /* adjust for scroll */
    int sy = box->y - render_scroll_y + URLBAR_H;
    if (mx>=box->x && mx<=box->x+box->w && my>=sy && my<=sy+box->h) {
        if (box->node->type==DOM_ELEMENT && !html_strcmp(box->node->tag,"a")) {
            return dom_attr(box->node,"href");
        }
    }
    for (int i=0;i<box->child_count;i++) {
        const char *r = find_link_at(box->children[i],mx,my);
        if (r) return r;
    }
    return (const char*)0;
}

/* ── Resolve relative URL ────────────────────────────────────── */
static void resolve_url(const char *base, const char *href, char *out) {
    /* absolute url */
    if (href[0]=='h'&&href[1]=='t'&&href[2]=='t'&&href[3]=='p') {
        int i=0; while(href[i]&&i<URL_MAX-1){out[i]=href[i];i++;} out[i]=0;
        return;
    }
    /* // relative */
    if (href[0]=='/'&&href[1]=='/') {
        /* get scheme from base */
        int i=0;
        while(base[i]&&base[i]!=':'&&i<8){out[i]=base[i];i++;}
        out[i++]=':'; out[i]=0;
        int j=0; while(href[j]&&i<URL_MAX-1){out[i++]=href[j++];}
        out[i]=0;
        return;
    }
    /* / absolute path */
    if (href[0]=='/') {
        /* extract scheme://host from base */
        int i=0;
        /* find end of scheme://host */
        while(base[i]&&(i<8)&&base[i]!=':') i++;
        if (base[i]==':'&&base[i+1]=='/'&&base[i+2]=='/') i+=3;
        while(base[i]&&base[i]!='/') i++;
        for(int j=0;j<i&&j<URL_MAX-1;j++) out[j]=base[j];
        int oi=i;
        int j=0; while(href[j]&&oi<URL_MAX-1){out[oi++]=href[j++];}
        out[oi]=0;
        return;
    }
    /* relative path — find last '/' in base path */
    int base_len=0; while(base[base_len]) base_len++;
    int last_slash=0;
    for(int i=0;i<base_len;i++) if(base[i]=='/') last_slash=i;
    int oi=0;
    for(int i=0;i<=last_slash&&oi<URL_MAX-1;i++) out[oi++]=base[i];
    int j=0; while(href[j]&&oi<URL_MAX-1){out[oi++]=href[j++];}
    out[oi]=0;
}

/* ── URL bar editing ─────────────────────────────────────────── */
static inline void urlbar_handle_key(int key, char ascii) {
    if (key == KEY_ENTER) {
        url_editing = 0;
        /* ensure http:// prefix */
        int has_scheme = (url_bar[0]=='h'&&url_bar[1]=='t'&&url_bar[2]=='t'&&url_bar[3]=='p');
        if (!has_scheme) {
            char tmp[URL_MAX];
            const char *pre = "http://";
            int pi=0; while(pre[pi]&&pi<8) tmp[pi]=pre[pi++];
            for(int i=0;url_bar[i]&&pi<URL_MAX-1;i++) tmp[pi++]=url_bar[i];
            tmp[pi]=0;
            for(int i=0;i<pi;i++) url_bar[i]=tmp[i];
            url_bar_len=pi;
        }
        navigate(url_bar);
        return;
    }
    if (key == KEY_BKSP) {
        if (url_bar_len > 0) url_bar[--url_bar_len]=0;
    } else if (ascii >= 0x20 && ascii < 0x7F && url_bar_len < URL_MAX-1) {
        url_bar[url_bar_len++] = ascii;
        url_bar[url_bar_len] = 0;
    }
    render_page(layout_root, url_bar, 0, content_h);
}

/* ── Scancode to ASCII (US layout) ──────────────────────────── */
static inline char sc_to_ascii(int sc) {
    static const char map[128] = {
        0,0,'1','2','3','4','5','6','7','8','9','0','-','=',0,0,
        'q','w','e','r','t','y','u','i','o','p','[',']',0,0,
        'a','s','d','f','g','h','j','k','l',';','\'','`',0,'\\',
        'z','x','c','v','b','n','m',',','.','/',0,'*',0,' ',0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,'7','8','9','-','4','5','6','+',
        '1','2','3','0','.',0,0,0,0,0,0
    };
    if (sc<0||sc>=128) return 0;
    return map[sc];
}

/* ── Home page HTML ──────────────────────────────────────────── */
static const char home_html[] =
"<html><head><title>SoftTail Browser</title></head>"
"<body style='background:#f0f4ff;color:#222;'>"
"<h1 style='color:#1a6cc4;'>SoftTail Browser</h1>"
"<p>Welcome to SoftTail OS Browser — running directly on the metal!</p>"
"<hr>"
"<h2>Quick links</h2>"
"<ul>"
"<li><a href='http://example.com'>example.com</a> — simple test page</li>"
"<li><a href='http://neverssl.com'>neverssl.com</a> — plain HTTP test</li>"
"<li><a href='http://info.cern.ch'>info.cern.ch</a> — first website ever</li>"
"</ul>"
"<hr>"
"<p style='color:#888;font-size:12;'>Type a URL in the bar above and press Enter.</p>"
"</body></html>";

/* ── Main ────────────────────────────────────────────────────── */
int main(void) {
    /* Allocate large buffers from heap (avoids 5MB BSS → Bad ELF) */
    render_fb   = (unsigned int*)malloc((unsigned long)(FB_W * FB_H * 4));
    layout_pool = (LayoutBox*)   malloc((unsigned long)(LAYOUT_MAX_BOXES * sizeof(LayoutBox)));
    html_buf    = (char*)        malloc((unsigned long)(BUF_SIZE));
    if (!render_fb || !layout_pool || !html_buf) {
        /* OOM — nothing to display, just hang */
        for(;;) sys_yield();
    }

    /* Load home page from static HTML */
    for (int i=0; home_html[i]; i++) html_buf[i]=home_html[i];
    html_buf[sizeof(home_html)-1]=0;

    for (int i=0;i<URL_MAX;i++) current_url[i]=0;
    const char *home = "softtail://home";
    for (int i=0;home[i];i++) current_url[i]=home[i];
    for (int i=0;i<URL_MAX;i++) url_bar[i]=current_url[i];
    url_bar_len=13; /* strlen("softtail://home") */

    dom_root    = html_parse(html_buf);
    css_resolve(dom_root);
    layout_root = layout_build(dom_root);
    content_h   = layout_root ? layout_root->h : BROWSER_HEIGHT;

    render_page(layout_root, current_url, 0, content_h);

    /* ── Event loop ── */
    MouseState prev_mouse; prev_mouse.x=0; prev_mouse.y=0; prev_mouse.buttons=0;

    while (1) {
        /* Keyboard */
        int key = input_get_key();
        if (key > 0) {
            if (url_editing) {
                urlbar_handle_key(key, sc_to_ascii(key));
            } else {
                switch (key) {
                case KEY_UP:
                    render_scroll_y -= RENDER_SCROLL_STEP;
                    if (render_scroll_y < 0) render_scroll_y = 0;
                    render_page(layout_root, current_url, 0, content_h);
                    break;
                case KEY_DOWN:
                    render_scroll_y += RENDER_SCROLL_STEP;
                    if (render_scroll_y > content_h - BROWSER_HEIGHT)
                        render_scroll_y = content_h - BROWSER_HEIGHT;
                    if (render_scroll_y < 0) render_scroll_y = 0;
                    render_page(layout_root, current_url, 0, content_h);
                    break;
                case KEY_PGUP:
                    render_scroll_y -= BROWSER_HEIGHT - URLBAR_H;
                    if (render_scroll_y < 0) render_scroll_y = 0;
                    render_page(layout_root, current_url, 0, content_h);
                    break;
                case KEY_PGDN:
                    render_scroll_y += BROWSER_HEIGHT - URLBAR_H;
                    if (render_scroll_y > content_h - BROWSER_HEIGHT)
                        render_scroll_y = content_h - BROWSER_HEIGHT;
                    if (render_scroll_y < 0) render_scroll_y = 0;
                    render_page(layout_root, current_url, 0, content_h);
                    break;
                default:
                    /* any printable char -> start url edit */
                    {
                        char a = sc_to_ascii(key);
                        if (a >= 0x20) {
                            url_editing = 1;
                            url_bar_len = 0;
                            url_bar[0] = a; url_bar[1]=0;
                            url_bar_len = 1;
                            render_page(layout_root, url_bar, 0, content_h);
                        }
                    }
                    break;
                }
            }
        }

        /* Mouse */
        MouseState ms;
        input_get_mouse(&ms);

        /* Click in URL bar */
        if (ms.buttons & 1) {
            if (!(prev_mouse.buttons & 1)) { /* button just pressed */
                if (ms.y < URLBAR_H) {
                    /* clicked url bar */
                    if (ms.x >= 58 && ms.x < FB_W - 60) {
                        url_editing = 1;
                        /* select all — clear bar */
                        url_bar_len = 0; url_bar[0]=0;
                        render_page(layout_root, url_bar, 0, content_h);
                    }
                    /* back button */
                    else if (ms.x >= 4 && ms.x < 26) {
                        if (hist_cur > 0) {
                            hist_cur--;
                            navigate(history[hist_cur]);
                        }
                    }
                    /* forward button */
                    else if (ms.x >= 30 && ms.x < 52) {
                        if (hist_cur < hist_top) {
                            hist_cur++;
                            navigate(history[hist_cur]);
                        }
                    }
                    /* Go button */
                    else if (ms.x >= FB_W-58) {
                        url_editing = 0;
                        navigate(url_bar);
                    }
                } else {
                    /* clicked page — check links */
                    url_editing = 0;
                    const char *href = find_link_at(layout_root, ms.x, ms.y);
                    if (href) {
                        char resolved[URL_MAX];
                        resolve_url(current_url, href, resolved);
                        navigate(resolved);
                    }
                }
            }
        }
        prev_mouse = ms;

        sys_yield();
    }

    return 0;
}
