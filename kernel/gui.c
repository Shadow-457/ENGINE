#include "../include/kernel.h"
#include "../include/font8x8.h"

#define SCR_W        (fb_get_width())
#define SCR_H        (fb_get_height())
#define GLYPH_SCALE  1
#define GLYPH_W      (8 * GLYPH_SCALE)
#define GLYPH_H      (8 * GLYPH_SCALE)
#define MENUBAR_H    0
#define DOCK_H       56
#define TITLEBAR_H   30
#define BORDER       3
#define SHADOW_OFF   8
#define RESIZE_HANDLE 14
#define DESKTOP_ICON_W 72
#define DESKTOP_ICON_H 80
#define DOCK_ICON_SZ 42
#define DOCK_PAD     12
#define ANIM_SPEED   8

#define C_BASE          0x05070a   /* Deep abyss */
#define C_SURFACE       0x0f141a   /* Card background */
#define C_ELEVATED      0x1a222c   /* Raised components */
#define C_ELEVATED2     0x2a3544   /* Hovered/Active components */
#define C_ACCENT        0x3584e4   /* Adwaita Blue */
#define C_ACCENT2       0x1c71d8   /* Dark Blue */
#define C_ACCENT_DIM    0x12365a   /* Muted accent */
#define C_ACCENT_GLOW   0x62a0ea   /* Vibrant glow */
#define C_TEXT          0xffffff   /* Pure white */
#define C_TEXT_DIM      0x9a9996   /* GNOME grey */
#define C_TEXT_MID      0xc0bfbc   /* Silver */
#define C_WIN_BG        0x1e1e1e   /* Standard Dark BG */
#define C_WIN_BORDER    0x303030   /* Muted border */
#define C_WIN_BORDER_A  0x3584e4   /* Accent border */
#define C_WIN_BORDER_A2 0x12365a
#define C_TITLE_A_L     0x242424   /* Active title */
#define C_TITLE_A_R     0x1e1e1e
#define C_TITLE_I_L     0x1a1a1a   /* Inactive title */
#define C_TITLE_I_R     0x161616
#define C_CLOSE_BTN     0xc01c28   /* GNOME Red */
#define C_CLOSE_BTN_H   0xe01b24
#define C_MIN_BTN       0xf5c211   /* Yellow */
#define C_MIN_BTN_H     0xf8e45c
#define C_MAX_BTN       0x26a269   /* GNOME Green */
#define C_MAX_BTN_H     0x2ec27e
#define C_DOCK_BG       0x000000   /* Translucent black */
#define C_DOCK_PILL     0x242424
#define C_DOCK_ICON_BG  0x303030
#define C_DOCK_ICON_ACT 0x3584e4
#define C_MENUBAR_BG    0x101010
#define C_MENUBAR_SEP   0x202020
#define C_SHADOW        0x000000
#define C_PROGRESS_FG   0x3584e4
#define C_PROGRESS_FG2  0x1c71d8
#define C_PROGRESS_BG   0x2a2a2a
#define C_SEP_LINE      0x303030
#define C_BTN_BG        0x303030
#define C_BTN_BORDER    0x3a3a3a
#define C_BTN_TOP       0x404040
#define C_BTN_HL        0x404040
#define C_MODAL_DIM     0x000000
#define C_CHECKBOX_BOX  0x303030
#define C_CHECKBOX_CHK  0x3584e4
#define C_TEXTINPUT_BG  0x121212
#define C_TEXTINPUT_BRD 0x303030
#define C_ICON_BG       0x242424
#define C_SHELL_PROMPT  0x3584e4
#define C_SHELL_CMD     0xffffff
#define C_SHELL_OUT     0xc0bfbc
#define C_LAUNCHER_BG   0x1e1e1e
#define C_LAUNCHER_HOVER 0x303030
#define C_LAUNCHER_ACTIVE 0x3584e4
#define C_TRAY_BG     0x101010
#define C_TRAY_ICON   0x9a9996
#define C_CTX_MENU_BG 0x242424
#define C_CTX_MENU_BORDER 0x3a3a3a
#define C_CTX_MENU_HOVER 0x303030
#define C_DESKTOP_ICON_BG 0x242424
#define C_DESKTOP_ICON_SEL 0x3584e4
#define C_NET_ON      0x26a269
#define C_NET_OFF     0xc01c28
#define C_VOL_HIGH    0x3584e4
#define C_VOL_MED     0xe66100
#define C_VOL_LOW     0xc01c28
#define C_WALL_TOP    0x0a0e14
#define C_WALL_BOT    0x05070a
#define C_WALL_ACCENT 0x1a222c

typedef enum {
    WT_NONE=0, WT_LABEL, WT_LABEL_DIM, WT_BUTTON, WT_CHECKBOX,
    WT_PROGRESS, WT_SEPARATOR, WT_LISTROW, WT_TEXTINPUT, WT_ICON,
} WidgetType;

typedef struct {
    WidgetType type;
    int  x,y,w,h;
    char text[64];
    char text2[32];
    int  checked, progress, win_id, id, hovered, cursor_pos;
    char input_buf[128];
    int  icon_type;
} GuiWidget;

typedef struct {
    int x, y;
    char name[32];
    int icon_type;
    int action;
    int selected;
} DesktopIcon;

#define MAX_DESKTOP_ICONS 16
#define CTX_MENU_MAX 8
typedef struct {
    int x, y, w, h;
    char labels[CTX_MENU_MAX][32];
    int actions[CTX_MENU_MAX];
    int count;
    int visible;
    int hovered_idx;
} ContextMenu;

#define LAUNCHER_MAX_APPS 16
typedef struct {
    int x, y, w, h;
    char names[LAUNCHER_MAX_APPS][32];
    char descs[LAUNCHER_MAX_APPS][48];
    int icons[LAUNCHER_MAX_APPS];
    int actions[LAUNCHER_MAX_APPS];
    int count;
    int visible;
    int hovered_idx;
    int anim_progress;
} AppLauncher;

typedef enum {
    WIN_NORMAL=0, WIN_OPENING, WIN_CLOSING, WIN_MINIMIZING, WIN_MINIMIZED,
    WIN_MAXIMIZING, WIN_MAXIMIZED, WIN_RESTORING
} WindowAnimState;

typedef struct {
    int  x,y,w,h, orig_x,orig_y,orig_w,orig_h;
    int  anim_x, anim_y, anim_w, anim_h;
    char title[64];
    int  visible,active,modal,minimized,maximized;
    WindowAnimState anim_state;
    int anim_progress;
    int fade_alpha;
    u32 *pixbuf;             /* Window's private off-screen buffer */
    int dirty;               /* 1 = needs re-render to pixbuf */
} GuiWindow;

#define GUI_MAX_WINDOWS  12
#define GUI_MAX_WIDGETS  160

static GuiWindow  g_wins[GUI_MAX_WINDOWS];
static GuiWidget  g_wgts[GUI_MAX_WIDGETS];
static int        g_wgt_cnt = 0;
static int        g_active  = -1;
static int        g_running = 0;

static int g_dragging=0, g_drag_win=-1, g_drag_off_x=0, g_drag_off_y=0;
static int g_clicked_widget=-1;
static int g_resizing=0, g_resize_win=-1, g_resize_dir=0;
static int g_resize_start_x=0, g_resize_start_y=0;
static int g_resize_start_w=0, g_resize_start_h=0, g_resize_start_ox=0, g_resize_start_oy=0;
static int g_cur_x=512, g_cur_y=384;
static int g_cur_x_old=512, g_cur_y_old=384;
static int g_cursor_type = 0;

static int  g_shell_win_id=-1;
static char g_shell_buf[512];
static int  g_shell_pos=0;
static char g_shell_output[4096];
static int  g_shell_out_pos=0;

static DesktopIcon g_desktop_icons[MAX_DESKTOP_ICONS];
static int g_desktop_icon_cnt = 0;
static ContextMenu g_ctx_menu = {0};
static AppLauncher g_launcher = {0};

static int g_net_status = 0;
static int g_volume_level = 75;
static int g_clock_ticks = 0;
static int g_clock_sec = 0;
static int g_clock_min = 0;
static int g_clock_hour = 0;
static int g_desktop_dirty = 1;
static int g_dock_dirty = 1;
static u32 *g_desktop_cache = 0;
static int g_dmg_x=0, g_dmg_y=0, g_dmg_w=0, g_dmg_h=0;

#define DOCK_BTN_MAX  10
typedef struct { int x, y, w, h; int action; int hover; } DockBtn;
static DockBtn g_dock_btns[DOCK_BTN_MAX];
static int     g_dock_btn_cnt = 0;

static int g_last_click_time = 0;
static int g_last_click_icon = -1;

static void draw_app_icon(int ix,int iy,int sz,int icon_type,int active);
static void draw_window(int id);
static void draw_dock(void);
static void draw_desktop_icons(void);
static void draw_context_menu(void);
static void draw_app_launcher(void);
static void draw_system_tray(void);
static void gui_redraw_window(int id);
void gui_open_about(void);
static int gui_widget_add_label_dim(int wid,int x,int y,const char*text,int id);
static void draw_text_clip(int x,int y,const char*s,u32 fg,int tbg,u32 bg,int max_x);
static u32 lerp_color(u32 a,u32 b,int x,int w);
static int gui_animate(void);
static void gui_add_damage(int x, int y, int w, int h);
void gui_handle_mouse_down(int px, int py);
void gui_handle_mouse_up(int px, int py);
static void open_file_manager(void);
static void open_settings_window(void);

#define CURSOR_W 14
#define CURSOR_H 22
static u32 g_cur_saved[CURSOR_W*CURSOR_H];
static int g_cur_drawn=0;
static int g_cur_drawn_x=0, g_cur_drawn_y=0; /* position at which cursor was last drawn */
/* Pro-Grade Modern Cursor (White fill, Black outline) */
static const u8 cursor_arrow[CURSOR_H][CURSOR_W]={
    {1,1,0,0,0,0,0,0,0,0,0,0,0,0}, {1,3,1,0,0,0,0,0,0,0,0,0,0,0},
    {1,3,3,1,0,0,0,0,0,0,0,0,0,0}, {1,3,3,3,1,0,0,0,0,0,0,0,0,0},
    {1,3,3,3,3,1,0,0,0,0,0,0,0,0}, {1,3,3,3,3,3,1,0,0,0,0,0,0,0},
    {1,3,3,3,3,3,3,1,0,0,0,0,0,0}, {1,3,3,3,3,3,3,3,1,0,0,0,0,0},
    {1,3,3,3,3,3,3,3,3,1,0,0,0,0}, {1,3,3,3,3,3,1,1,1,1,0,0,0,0},
    {1,3,3,3,1,3,3,1,0,0,0,0,0,0}, {1,3,3,1,0,1,3,3,1,0,0,0,0,0},
    {1,3,1,0,0,0,1,3,3,1,0,0,0,0}, {1,1,0,0,0,0,0,1,3,1,0,0,0,0},
    {0,0,0,0,0,0,0,0,1,1,0,0,0,0}, {0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0,0,0,0,0,0,0},
};
static const u8 cursor_resize_h[CURSOR_H][CURSOR_W]={
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0},{0,0,0,0,0,1,1,1,1,0,0,0,0,0},
    {0,0,0,0,1,3,3,3,3,1,0,0,0,0},{0,0,0,1,3,3,3,3,3,3,1,0,0,0},
    {0,0,1,3,3,3,3,3,3,3,3,1,0,0},{0,1,3,3,3,3,3,3,3,3,3,3,1,0},
    {1,3,3,3,3,3,3,3,3,3,3,3,3,1},{0,1,3,3,3,3,3,3,3,3,3,3,1,0},
    {0,0,1,3,3,3,3,3,3,3,3,1,0,0},{0,0,0,1,3,3,3,3,3,3,1,0,0,0},
    {0,0,0,0,1,3,3,3,3,1,0,0,0,0},{0,0,0,0,0,1,1,1,1,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0,0,0,0,0,0,0},
};
static const u8 cursor_resize_v[CURSOR_H][CURSOR_W]={
    {0,0,0,0,0,1,1,1,1,0,0,0,0,0},{0,0,0,0,1,3,3,3,3,1,0,0,0,0},
    {0,0,0,1,3,3,3,3,3,3,1,0,0,0},{0,0,1,3,3,3,3,3,3,3,3,1,0,0},
    {0,1,3,3,3,3,3,3,3,3,3,3,1,0},{1,3,3,3,3,3,3,3,3,3,3,3,3,1},
    {0,0,0,0,0,1,3,3,1,0,0,0,0,0},{0,0,0,0,0,1,3,3,1,0,0,0,0,0},
    {0,0,0,0,0,1,3,3,1,0,0,0,0,0},{0,0,0,0,0,1,3,3,1,0,0,0,0,0},
    {0,0,0,0,0,1,3,3,1,0,0,0,0,0},{0,0,0,0,0,1,3,3,1,0,0,0,0,0},
    {0,0,0,0,0,1,3,3,1,0,0,0,0,0},{0,0,0,0,0,1,3,3,1,0,0,0,0,0},
    {0,0,0,0,0,1,3,3,1,0,0,0,0,0},{0,0,0,0,0,1,3,3,1,0,0,0,0,0},
    {1,3,3,3,3,3,3,3,3,3,3,3,3,1},{0,1,3,3,3,3,3,3,3,3,3,3,1,0},
    {0,0,1,3,3,3,3,3,3,3,3,1,0,0},{0,0,0,1,3,3,3,3,3,3,1,0,0,0},
    {0,0,0,0,1,3,3,3,3,1,0,0,0,0},{0,0,0,0,0,1,1,1,1,0,0,0,0,0},
};

static void cursor_erase(void){
    if(!g_cur_drawn)return;
    int i=0;
    /* Use the saved draw position, NOT the current cursor position.
       This prevents ghost trails when the cursor moves between draw and erase. */
    for(int r=0;r<CURSOR_H;r++) for(int c=0;c<CURSOR_W;c++,i++){
        int px=g_cur_drawn_x+c,py=g_cur_drawn_y+r;
        if(px>=0&&px<SCR_W&&py>=0&&py<SCR_H) fb_put_pixel(px,py,g_cur_saved[i]);
    }
    g_cur_drawn=0;
}

static void cursor_draw(void){
    const u8 (*shape)[CURSOR_W] = cursor_arrow;
    if(g_cursor_type==1) { shape=cursor_resize_h; }
    else if(g_cursor_type==2) { shape=cursor_resize_v; }
    else if(g_cursor_type==3) { shape=cursor_resize_h; }

    /* Record the position we are drawing at so erase can restore the right pixels */
    g_cur_drawn_x = g_cur_x;
    g_cur_drawn_y = g_cur_y;

    int i=0;
    for(int r=0;r<CURSOR_H;r++) for(int c=0;c<CURSOR_W;c++,i++){
        int px=g_cur_drawn_x+c,py=g_cur_drawn_y+r;
        g_cur_saved[i]=(px>=0&&px<SCR_W&&py>=0&&py<SCR_H)?fb_get_pixel(px,py):0;
    }

    for(int r=0;r<CURSOR_H;r++) {
        for(int c=0;c<CURSOR_W;c++){
            u8 p=shape[r][c]; 
            if(!p) continue;
            int px=g_cur_drawn_x+c, py=g_cur_drawn_y+r;
            if(px<0||px>=SCR_W||py<0||py>=SCR_H) continue;
            fb_put_pixel(px,py,p==1?0x000000:0xffffff);
        }
    }
    g_cur_drawn=1;
}

void gui_cursor_move(int dx,int dy){
    if(!g_running)return;
    watchdog_pet();
    cursor_erase();
    g_cur_x+=dx; g_cur_y+=dy;
    if(g_cur_x<0) { g_cur_x=0; }
    if(g_cur_x>=SCR_W) { g_cur_x=SCR_W-1; }
    if(g_cur_y<MENUBAR_H) { g_cur_y=MENUBAR_H; }
    if(g_cur_y>=SCR_H) { g_cur_y=SCR_H-1; }
    
    if(g_dragging&&g_drag_win>=0){
        GuiWindow *w=&g_wins[g_drag_win];
        int nx=g_cur_x-g_drag_off_x, ny=g_cur_y-g_drag_off_y;
        if(nx<0) { nx=0; }
        if(ny<MENUBAR_H) { ny=MENUBAR_H; }
        w->x=nx; w->y=ny; w->anim_x=nx; w->anim_y=ny;
        gui_redraw(); /* Full redraw during drag for stability as requested */
    } else if(g_resizing&&g_resize_win>=0){
        GuiWindow *w=&g_wins[g_resize_win];
        int ndx=g_cur_x-g_resize_start_x, ndy=g_cur_y-g_resize_start_y;
        if(g_resize_dir&1){w->x=g_resize_start_ox+ndx;w->w=g_resize_start_w-ndx;w->anim_x=w->x;w->anim_w=w->w;}
        if(g_resize_dir&2)w->h=g_resize_start_h+ndy;
        if(g_resize_dir&4)w->w=g_resize_start_w+ndx;
        if(w->w<120) { w->w=120; }
        if(w->h<80) { w->h=80; }
        w->anim_w=w->w; w->anim_h=w->h;
        gui_redraw();
    }
    cursor_draw();
}
void gui_cursor_get(int*ox,int*oy){*ox=g_cur_x;*oy=g_cur_y;}

/* Button layout: close=right, maximize=middle, minimize=left
   btn_sz=22, right margin=4, gap between buttons=6 */
static int hit_close(GuiWindow*w,int px,int py){
    int bx=w->x+w->w-BORDER-22-4,by=w->y+BORDER+(TITLEBAR_H-22)/2;
    return px>=bx&&px<bx+22&&py>=by&&py<by+22;
}
static int hit_maximize(GuiWindow*w,int px,int py){
    int bx=w->x+w->w-BORDER-4-22-6-22,by=w->y+BORDER+(TITLEBAR_H-22)/2;
    return px>=bx&&px<bx+22&&py>=by&&py<by+22;
}
static int hit_minimize(GuiWindow*w,int px,int py){
    int bx=w->x+w->w-BORDER-4-22-6-22-6-22,by=w->y+BORDER+(TITLEBAR_H-22)/2;
    return px>=bx&&px<bx+22&&py>=by&&py<by+22;
}
static int hit_titlebar(GuiWindow*w,int px,int py){
    if(py<w->y+BORDER||py>=w->y+BORDER+TITLEBAR_H)return 0;
    if(px<w->x+BORDER||px>=w->x+w->w-BORDER)return 0;
    if(px>=w->x+w->w-BORDER-120)return 0;
    return 1;
}

static int hit_resize(GuiWindow*w,int px,int py){
    if(w->maximized)return 0;
    int rx=w->x+w->w,ry=w->y+w->h;
    if(px>=rx-RESIZE_HANDLE&&px<=rx&&py>=ry-RESIZE_HANDLE&&py<=ry)return 8;
    if(px>=rx-2&&px<=rx+2&&py>=w->y+BORDER+TITLEBAR_H&&py<=ry-RESIZE_HANDLE)return 3;
    if(px>=w->x+BORDER&&px<=rx-RESIZE_HANDLE&&py>=ry-2&&py<=ry+2)return 2;
    if(px>=w->x-2&&px<=w->x+2&&py>=w->y+BORDER+TITLEBAR_H&&py<=ry-RESIZE_HANDLE)return 4;
    if(px>=w->x-2&&px<=w->x+2&&py>=w->y+BORDER&&py<=w->y+BORDER+TITLEBAR_H)return 6;
    if(px>=rx-RESIZE_HANDLE&&px<=rx&&py>=w->y+BORDER&&py<=w->y+BORDER+RESIZE_HANDLE)return 5;
    if(px>=w->x-2&&px<=w->x+2&&py>=ry-RESIZE_HANDLE&&py<=ry)return 7;
    return 0;
}

/* -----------------------------------------------------------------------
 * Mouse button handling — properly split into mouse_down and mouse_up.
 *
 * mouse_down: starts drags/resizes, records pending widget click.
 * mouse_up:   fires widget actions, stops drags/resizes.
 * gui_handle_click: legacy wrapper — calls mouse_down then mouse_up
 *                   (for callers that only fire on release).
 * ----------------------------------------------------------------------- */

/* Clamp coordinates to screen bounds */
static void clamp_xy(int *px, int *py){
    if(*px<0)*px=0; if(*px>=SCR_W)*px=SCR_W-1;
    if(*py<0)*py=0; if(*py>=SCR_H)*py=SCR_H-1;
}

/* Index of the widget whose mouse-down is pending (fires on mouse-up) */
static int g_pending_widget = -1;
static int g_pending_win    = -1;

/* Open a new file-manager window and populate it */
static void open_file_manager(void){
    for(int w=0;w<GUI_MAX_WINDOWS;w++) if(g_wins[w].visible&&g_wins[w].title[0]=='F'){gui_window_set_active(w);return;}
    int fm=gui_window_create(24,48,480,560,"File Manager");
    if(fm<0)return;
    int rh=GLYPH_H+8,cy=6;
    gui_widget_add_icon(fm,4,cy,0,1);gui_widget_add_label(fm,44,cy+6,"Files",10);cy+=rh+4;
    gui_widget_add_separator(fm,cy,12);cy+=10;
    gui_widget_add_listrow(fm,0,cy,456,"README.TXT","27 B",20);cy+=rh;
    gui_widget_add_listrow(fm,0,cy,456,"HELLO.C","8.2 KB",21);cy+=rh;
    gui_widget_add_listrow(fm,0,cy,456,"MYPROGRAM","64 KB",22);cy+=rh;
    gui_widget_add_listrow(fm,0,cy,456,"SHC","128 KB",23);cy+=rh;
    gui_widget_add_listrow(fm,0,cy,456,"FIB.SHA","512 B",24);cy+=rh;
    gui_widget_add_listrow(fm,0,cy,456,"HELLO.SHA","256 B",25);cy+=rh;
    gui_widget_add_listrow(fm,0,cy,456,"POSIX_TEST","53 KB",26);cy+=rh;
    cy+=6;gui_widget_add_separator(fm,cy,27);cy+=10;
    gui_widget_add_label_dim(fm,6,cy,"7 items  |  42.6 KB free",28);cy+=rh;
    gui_widget_add_button(fm,6,cy,90,GLYPH_H+10,"Refresh",29);
    gui_widget_add_button(fm,104,cy,90,GLYPH_H+10,"Delete",30);
}

static void open_settings_window(void){
    for(int w=0;w<GUI_MAX_WINDOWS;w++) if(g_wins[w].visible&&g_wins[w].title[0]=='S'){gui_window_set_active(w);return;}
    int sid=gui_window_create(200,100,440,460,"Settings");
    if(sid<0)return;
    int rh=GLYPH_H+8,cy=8;
    gui_widget_add_icon(sid,8,cy,2,300);gui_widget_add_label(sid,48,cy+6,"System Settings",301);cy+=rh+8;
    gui_widget_add_separator(sid,cy,302);cy+=10;
    gui_widget_add_label(sid,8,cy,"Display Options:",303);cy+=rh+4;
    gui_widget_add_checkbox(sid,8,cy,"Enable shadows",1,304);cy+=rh;
    gui_widget_add_checkbox(sid,8,cy,"Show grid pattern",1,305);cy+=rh;
    gui_widget_add_checkbox(sid,8,cy,"Animate windows",1,306);cy+=rh;
    cy+=8;gui_widget_add_separator(sid,cy,307);cy+=10;
    gui_widget_add_label(sid,8,cy,"Progress Demo:",308);cy+=rh+4;
    gui_widget_add_progress(sid,8,cy,410,75,309);cy+=rh+10;
    gui_widget_add_label_dim(sid,8,cy,"System load: 75%",310);cy+=rh+8;
    gui_widget_add_separator(sid,cy,311);cy+=10;
    gui_widget_add_label(sid,8,cy,"Text Input:",312);cy+=rh+4;
    gui_widget_add_textinput(sid,8,cy,410,"Type here...",313);cy+=rh+10;
    gui_widget_add_button(sid,8,cy,90,GLYPH_H+10,"Apply",314);
    gui_widget_add_button(sid,106,cy,90,GLYPH_H+10,"Reset",315);
    gui_widget_add_button(sid,336,cy,90,GLYPH_H+10,"Close",316);
}

void gui_handle_mouse_down(int px, int py){
    if(!g_running) return;
    clamp_xy(&px,&py);
    g_pending_widget=-1; g_pending_win=-1;

    /* Dismiss context menu on any click */
    if(g_ctx_menu.visible){
        if(px>=g_ctx_menu.x&&px<g_ctx_menu.x+g_ctx_menu.w&&
           py>=g_ctx_menu.y&&py<g_ctx_menu.y+g_ctx_menu.h){
            /* action fires on mouse_up — just record we're inside */
            return;
        }
        g_ctx_menu.visible=0; gui_redraw(); return;
    }

    /* Dismiss launcher on click outside */
    if(g_launcher.visible){
        if(!(px>=g_launcher.x&&px<g_launcher.x+g_launcher.w&&
             py>=g_launcher.y&&py<g_launcher.y+g_launcher.h)){
            g_launcher.visible=0; gui_redraw(); return;
        }
        return; /* action fires on mouse_up */
    }

    /* Dock buttons */
    for(int i=0;i<g_dock_btn_cnt;i++){
        if(px>=g_dock_btns[i].x&&px<g_dock_btns[i].x+g_dock_btns[i].w&&
           py>=g_dock_btns[i].y&&py<g_dock_btns[i].y+g_dock_btns[i].h)
            return; /* fires on mouse_up */
    }

    /* Desktop icons — single click selects, double click opens */
    for(int i=0;i<g_desktop_icon_cnt;i++){
        DesktopIcon *di=&g_desktop_icons[i];
        if(px>=di->x&&px<di->x+DESKTOP_ICON_W&&py>=di->y&&py<di->y+DESKTOP_ICON_H)
            return; /* fires on mouse_up */
    }

    /* Modal windows — only close button works */
    for(int i=0;i<GUI_MAX_WINDOWS;i++){
        if(!g_wins[i].visible||!g_wins[i].modal) continue;
        if(hit_close(&g_wins[i],px,py)) return;
        return;
    }

    /* Normal windows — active first, then others */
    for(int pass=0;pass<2;pass++){
        for(int i=0;i<GUI_MAX_WINDOWS;i++){
            if(!g_wins[i].visible||g_wins[i].modal||g_wins[i].minimized) continue;
            int ia=(i==g_active);
            if(pass==0&&!ia) continue;
            if(pass==1&&ia)  continue;
            GuiWindow *win=&g_wins[i];
            /* Use actual (committed) window rect for hit testing */
            if(px<win->x||px>=win->x+win->w) continue;
            if(py<win->y||py>=win->y+win->h) continue;

            /* Raise window immediately */
            if(!ia){ gui_window_set_active(i); gui_redraw(); }

            /* Resize edges */
            int rd=hit_resize(win,px,py);
            if(rd){
                g_resizing=1; g_resize_win=i; g_resize_dir=rd;
                g_resize_start_x=px; g_resize_start_y=py;
                g_resize_start_w=win->w; g_resize_start_h=win->h;
                g_resize_start_ox=win->x; g_resize_start_oy=win->y;
                return;
            }

            /* Titlebar buttons — fire immediately on down */
            if(hit_close(win,px,py)){    gui_window_close(i); return; }
            if(hit_minimize(win,px,py)){ win->visible=0; win->minimized=1; win->anim_state=WIN_MINIMIZED; g_active=-1; gui_redraw(); return; }
            if(hit_maximize(win,px,py)){
                if(!win->maximized){ win->orig_x=win->x; win->orig_y=win->y; win->orig_w=win->w; win->orig_h=win->h; win->anim_state=WIN_MAXIMIZING; win->anim_progress=0; win->maximized=1; }
                else               { win->x=win->orig_x; win->y=win->orig_y; win->w=win->orig_w; win->h=win->orig_h; win->maximized=0; }
                gui_redraw(); return;
            }

            /* Titlebar drag — start immediately */
            if(hit_titlebar(win,px,py)){
                g_dragging=1; g_drag_win=i;
                g_drag_off_x=px-win->x; g_drag_off_y=py-win->y;
                return;
            }

            /* Widgets — record pending, highlight on down, fire on up */
            int cx=win->x+BORDER, cy2=win->y+BORDER+TITLEBAR_H+1;
            for(int j=0;j<g_wgt_cnt;j++){
                GuiWidget *wg=&g_wgts[j]; if(wg->win_id!=i) continue;
                int wx=cx+wg->x, wy=cy2+wg->y;
                if(px>=wx&&px<wx+wg->w&&py>=wy&&py<wy+wg->h){
                    if(wg->type==WT_CHECKBOX){
                        wg->checked=!wg->checked; gui_redraw_window(i); return;
                    }
                    if(wg->type==WT_BUTTON){
                        /* Highlight immediately; action fires on mouse_up */
                        g_pending_widget=j; g_pending_win=i;
                        g_clicked_widget=j; gui_redraw_window(i);
                        return;
                    }
                }
            }
            return;
        }
    }
}

void gui_handle_mouse_up(int px, int py){
    if(!g_running) return;
    clamp_xy(&px,&py);

    /* Fire pending button click */
    if(g_pending_widget>=0){
        int j=g_pending_widget, i=g_pending_win;
        g_pending_widget=-1; g_pending_win=-1;
        g_clicked_widget=-1;
        if(i>=0&&i<GUI_MAX_WINDOWS&&g_wins[i].visible){
            GuiWidget *wg=&g_wgts[j];
            /* Handle well-known button IDs */
            if(wg->id==316||wg->id==211){ gui_window_close(i); gui_redraw(); return; } /* Close */
            if(wg->id==117)             { gui_window_close(i); gui_redraw(); return; } /* Monitor Close */
            gui_redraw_window(i);
        }
        gui_redraw();
        return;
    }

    /* Stop drag/resize — actual position update already happened in cursor_move */
    if(g_dragging){ g_dragging=0; g_drag_win=-1; gui_redraw(); return; }
    if(g_resizing){ g_resizing=0; g_resize_win=-1; g_resize_dir=0; gui_redraw(); return; }

    /* Context menu item */
    if(g_ctx_menu.visible){
        if(px>=g_ctx_menu.x&&px<g_ctx_menu.x+g_ctx_menu.w&&
           py>=g_ctx_menu.y&&py<g_ctx_menu.y+g_ctx_menu.h){
            int idx=(py-g_ctx_menu.y-4)/(GLYPH_H+8);
            g_ctx_menu.visible=0;
            if(idx>=0&&idx<g_ctx_menu.count){
                int action=g_ctx_menu.actions[idx];
                if(action==1) gui_open_shell_window();
                else if(action==3) gui_open_system_monitor();
            }
            gui_redraw(); return;
        }
        g_ctx_menu.visible=0; gui_redraw(); return;
    }

    /* Launcher item */
    if(g_launcher.visible){
        if(px>=g_launcher.x&&px<g_launcher.x+g_launcher.w&&
           py>=g_launcher.y&&py<g_launcher.y+g_launcher.h){
            int idx=(py-g_launcher.y-GLYPH_H-12)/44;
            g_launcher.visible=0;
            if(idx>=0&&idx<g_launcher.count){
                int action=g_launcher.actions[idx];
                if(action==0)      open_file_manager();
                else if(action==1) gui_open_shell_window();
                else if(action==2) open_settings_window();
                else if(action==3) gui_open_system_monitor();
                else if(action==4) gui_open_about();
            }
            gui_redraw(); return;
        }
        g_launcher.visible=0; gui_redraw(); return;
    }

    /* Dock buttons */
    for(int i=0;i<g_dock_btn_cnt;i++){
        if(px>=g_dock_btns[i].x&&px<g_dock_btns[i].x+g_dock_btns[i].w&&
           py>=g_dock_btns[i].y&&py<g_dock_btns[i].y+g_dock_btns[i].h){
            switch(g_dock_btns[i].action){
            case 0: g_launcher.visible=!g_launcher.visible;
                if(g_launcher.visible){g_launcher.x=4;g_launcher.y=MENUBAR_H+4;g_launcher.w=220;g_launcher.h=g_launcher.count*44+GLYPH_H+20;g_launcher.anim_progress=0;}
                break;
            case 1: open_file_manager(); break;
            case 2: for(int w=0;w<GUI_MAX_WINDOWS;w++) if(g_wins[w].visible&&g_wins[w].title[0]=='T'){gui_window_set_active(w);g_shell_win_id=w;gui_redraw();return;}
                    gui_open_shell_window(); break;
            case 3: open_settings_window(); break;
            case 4: gui_open_about(); break;
            case 5: gui_open_shell_window(); break;
            case 6: gui_open_system_monitor(); break;
            }
            gui_redraw(); return;
        }
    }

    /* Desktop icons */
    for(int i=0;i<g_desktop_icon_cnt;i++){
        DesktopIcon *di=&g_desktop_icons[i];
        if(px>=di->x&&px<di->x+DESKTOP_ICON_W&&py>=di->y&&py<di->y+DESKTOP_ICON_H){
            int ct=g_clock_ticks;
            if(ct-g_last_click_time<50&&g_last_click_icon==i){
                g_last_click_time=0; g_last_click_icon=-1;
                if(di->action==0)      gui_open_shell_window();
                else if(di->action==2) gui_open_system_monitor();
                else if(di->action==3) gui_open_about();
                gui_redraw(); return;
            }
            g_last_click_time=ct; g_last_click_icon=i;
            gui_redraw(); return;
        }
    }
}

/* Legacy single-event click (PS/2 drivers that fire only on release) */
void gui_handle_click(int px,int py){
    if(px<0){px=g_cur_x;py=g_cur_y;}
    gui_handle_mouse_down(px,py);
    /* Only fire mouse_up if we didn't start a drag/resize */
    if(!g_dragging&&!g_resizing) gui_handle_mouse_up(px,py);
}

void gui_handle_right_click(int px,int py){
    if(!g_running)return;
    if(px<0) { px=0; }
    if(py<0) { py=0; }
    if(px>=SCR_W) { px=SCR_W-1; }
    if(py>=SCR_H) { py=SCR_H-1; }
    if(py>=MENUBAR_H&&py<SCR_H-DOCK_H){
        int ow=0;
        for(int i=0;i<GUI_MAX_WINDOWS;i++){
            if(!g_wins[i].visible||g_wins[i].minimized)continue;
            if(px>=g_wins[i].x&&px<g_wins[i].x+g_wins[i].w&&py>=g_wins[i].y&&py<g_wins[i].y+g_wins[i].h){ow=1;break;}
        }
        if(!ow){
            g_ctx_menu.x=px;g_ctx_menu.y=py;g_ctx_menu.w=170;g_ctx_menu.count=4;g_ctx_menu.visible=1;g_ctx_menu.hovered_idx=-1;
            const char *labels[]={"Refresh Desktop","Open Terminal","Settings","System Monitor"};
            for(int i=0;i<4;i++){int j=0;while(labels[i][j]&&j<31){g_ctx_menu.labels[i][j]=labels[i][j];j++;}g_ctx_menu.labels[i][j]='\0';g_ctx_menu.actions[i]=i;}
            g_ctx_menu.h=g_ctx_menu.count*(GLYPH_H+8)+8;
            if(g_ctx_menu.x+g_ctx_menu.w>SCR_W)g_ctx_menu.x=SCR_W-g_ctx_menu.w;
            if(g_ctx_menu.y+g_ctx_menu.h>SCR_H)g_ctx_menu.y=SCR_H-g_ctx_menu.h;
            if(g_ctx_menu.x<0) { g_ctx_menu.x=0; }
            if(g_ctx_menu.y<MENUBAR_H) { g_ctx_menu.y=MENUBAR_H; }
            gui_redraw();
        }
    }
}

void gui_stop_drag(void){
    if(g_dragging){ g_dragging=0; g_drag_win=-1; gui_redraw(); }
    else { g_dragging=0; g_drag_win=-1; }
}
void gui_stop_resize(void){
    if(g_resizing){ g_resizing=0; g_resize_win=-1; g_resize_dir=0; gui_redraw(); }
    else { g_resizing=0; g_resize_win=-1; g_resize_dir=0; }
}

static void gui_redraw_window(int id){
    if(!g_running||id<0||id>=GUI_MAX_WINDOWS)return;
    GuiWindow *w = &g_wins[id];
    gui_add_damage(w->anim_x-10, w->anim_y-10, w->anim_w+20, w->anim_h+20);
}


void gui_open_shell_window(void){
    if(g_shell_win_id>=0&&g_shell_win_id<GUI_MAX_WINDOWS&&g_wins[g_shell_win_id].visible){gui_window_set_active(g_shell_win_id);return;}
    int id=-1;
    for(int i=0; i<GUI_MAX_WINDOWS; i++) {
        if(!g_wins[i].visible) { id=i; break; }
    }
    if (id < 0) return;
    int sw = gui_window_create(100, 80, 560, 440, "Terminal");
    if (sw < 0) return;
    g_shell_win_id = sw;
    g_shell_pos = 0;
    g_shell_buf[0] = '\0';
    g_shell_out_pos = 0;
    g_shell_output[0] = '\0';
    int cy=8;gui_widget_add_label(sw,8,cy,"Systrix Shell v0.1",500);cy+=GLYPH_H+8;gui_widget_add_separator(sw,cy,501);cy+=8;gui_widget_add_label(sw,8,cy,"$ _",502);
}

void gui_shell_append(const char*text){
    int len=0;while(text[len])len++;
    if(g_shell_out_pos+len>=4090){int k=1024;for(int i=0;i<k;i++)g_shell_output[i]=g_shell_output[g_shell_out_pos-k+i];g_shell_out_pos=k;}
    for(int i=0;i<len&&g_shell_out_pos<4090;i++)g_shell_output[g_shell_out_pos++]=text[i];
    g_shell_output[g_shell_out_pos]='\0';
}

static void gui_process_command(void){
    if(g_shell_pos==0)return;
    g_shell_buf[g_shell_pos]='\0';
    gui_shell_append("$ ");gui_shell_append(g_shell_buf);gui_shell_append("\n");
    const char*s=g_shell_buf;while(*s==' ')s++;
    if(strcmp(s,"help")==0)gui_shell_append("Commands: help, clear, ls, meminfo, uname, echo, ver, date, whoami, ps, net, 720p, 1080p, browser\n");
    else if(strcmp(s,"clear")==0){g_shell_out_pos=0;g_shell_output[0]='\0';}
    else if(strcmp(s,"ls")==0)gui_shell_append("README.TXT  HELLO.C  MYPROGRAM  SHC  FIB.SHA  HELLO.SHA  POSIX_TEST  DOCS/\n");
    else if(strcmp(s,"meminfo")==0)gui_shell_append("Total: 128 MB  Used: 16 MB  Free: 112 MB\n");
    else if(strcmp(s,"uname")==0||strcmp(s,"uname -a")==0)gui_shell_append("Systrix 0.1 x86_64\n");
    else if(strcmp(s,"ver")==0)gui_shell_append("Systrix v0.1 Desktop Environment (x86-64)\n");
    else if(strcmp(s,"whoami")==0)gui_shell_append("root\n");
    else if(strcmp(s,"ps")==0)gui_shell_append("PID  STATE    NAME\n 01  RUNNING  shell\n 02  READY    file_manager\n 03  READY    terminal\n");
    else if(strcmp(s,"net")==0)gui_shell_append("e1000: DOWN  IP: 0.0.0.0  GW: 0.0.0.0\n");
    else if(strncmp(s,"echo ",5)==0){gui_shell_append(s+5);gui_shell_append("\n");}
    else if(strcmp(s,"date")==0)gui_shell_append("2026-04-14 Systrix\n");
    else if(strcmp(s,"720p")==0){
        if(fb_set_resolution(1280,720)){
            gui_shell_append("Resolution: 1280x720 (720p)\n");
            gui_redraw();
        }else gui_shell_append("720p not supported.\n");
    }
    else if(strcmp(s,"1080p")==0){
        if(fb_set_resolution(1920,1080)){
            gui_shell_append("Resolution: 1920x1080 (1080p)\n");
            gui_redraw();
        }else gui_shell_append("1080p not supported.\n");
    }
    else if(strcmp(s,"browser")==0)gui_shell_append("Browser: coming soon.\n");
    else{gui_shell_append("Unknown: ");gui_shell_append(s);gui_shell_append("\nType 'help'\n");}
    g_shell_pos=0;g_shell_buf[0]='\0';
}

void gui_shell_keypress(u8 ch){
    if(g_shell_win_id<0||!g_wins[g_shell_win_id].visible)return;
    if(ch=='\r'||ch=='\n'){
        gui_process_command();
        if(g_shell_win_id>=0&&g_wins[g_shell_win_id].visible){
            gui_redraw_window(g_shell_win_id);
        }
        return;
    }
    if(ch=='\b'){
        if(g_shell_pos>0){g_shell_pos--;g_shell_buf[g_shell_pos]='\0';}
        if(g_shell_win_id>=0&&g_wins[g_shell_win_id].visible){
            gui_redraw_window(g_shell_win_id);
        }
        return;
    }
    if(ch<0x20)return;
    if(g_shell_pos<510){g_shell_buf[g_shell_pos++]=(char)ch;g_shell_buf[g_shell_pos]='\0';}
    if(g_shell_win_id>=0&&g_wins[g_shell_win_id].visible){
        gui_redraw_window(g_shell_win_id);
    }
}

void gui_open_system_monitor(void){
    for(int i=0;i<GUI_MAX_WINDOWS;i++)if(g_wins[i].visible&&g_wins[i].title[0]=='M'){gui_window_set_active(i);return;}
    int id=gui_window_create(150,100,500,420,"System Monitor");
    if(id<0)return;
    int cy=8;
    if(gui_widget_add_icon(id,8,cy,5,100)<0)return;
    if(gui_widget_add_label(id,48,cy+6,"System Monitor",101)<0) return;
    cy+=GLYPH_H+8;
    if(gui_widget_add_separator(id,cy,102)<0) return;
    cy+=10;
    if(gui_widget_add_label(id,8,cy,"CPU Usage:",103)<0) return;
    cy+=GLYPH_H+6;
    if(gui_widget_add_progress(id,8,cy,460,0,104)<0) return;
    cy+=(GLYPH_H+6)+16;
    if(gui_widget_add_label(id,8,cy,"Memory Usage:",105)<0) return;
    cy+=GLYPH_H+6;
    if(gui_widget_add_progress(id,8,cy,460,62,106)<0) return;
    cy+=(GLYPH_H+6)+16;
    if(gui_widget_add_label(id,8,cy,"Disk Usage:",107)<0) return;
    cy+=GLYPH_H+6;
    if(gui_widget_add_progress(id,8,cy,460,28,108)<0) return;
    cy+=(GLYPH_H+6)+16;
    if(gui_widget_add_separator(id,cy,109)<0) return;
    cy+=10;
    if(gui_widget_add_label(id,8,cy,"Running Processes:",110)<0)return;
    cy+=GLYPH_H+4;
    if(gui_widget_add_listrow(id,0,cy,480,"shell","PID 1",111)<0)return;
    cy+=GLYPH_H+8;
    if(gui_widget_add_listrow(id,0,cy,480,"file_manager","PID 2",112)<0)return;
    cy+=GLYPH_H+8;
    if(gui_widget_add_listrow(id,0,cy,480,"terminal","PID 3",113)<0)return;
    cy+=GLYPH_H+8;
    if(gui_widget_add_listrow(id,0,cy,480,"settings","PID 4",114)<0)return;
    cy+=GLYPH_H+8;
    cy+=6; if(gui_widget_add_separator(id,cy,115)<0)return; cy+=10;
    if(gui_widget_add_button(id,8,cy,90,GLYPH_H+10,"Refresh",116)<0)return;
    if(gui_widget_add_button(id,106,cy,90,GLYPH_H+10,"Close",117)<0)return;
}

void gui_open_about(void){
    for(int i=0;i<GUI_MAX_WINDOWS;i++)if(g_wins[i].visible&&g_wins[i].title[0]=='A'){gui_window_set_active(i);return;}
    int id=gui_window_create(312,190,420,340,"About Systrix");
    if(id<0)return;
    int row_h=GLYPH_H+8;int cy=8;
    gui_widget_add_icon(id,188,cy,3,200);cy+=40;
    gui_widget_add_label(id,164,cy,"Systrix",200);cy+=GLYPH_H+2;
    gui_widget_add_label_dim(id,120,cy,"v0.1 Desktop Environment",201);cy+=GLYPH_H+10;
    gui_widget_add_separator(id,cy,202);cy+=10;
    gui_widget_add_label(id,8,cy,"Arch:  x86-64",203);cy+=row_h;
    gui_widget_add_label(id,8,cy,"Mode:  Ring 0 / Ring 3",204);cy+=row_h;
    gui_widget_add_label(id,8,cy,"FS:    FAT32 + JFS",205);cy+=row_h;
    gui_widget_add_label(id,8,cy,"Net:   e1000 + TCP/IP",206);cy+=row_h;
    gui_widget_add_label(id,8,cy,"GUI:   1920x1080 32bpp VBE",208);cy+=row_h;
    cy+=4;gui_widget_add_separator(id,cy,210);cy+=10;
    gui_widget_add_button(id,160,cy,90,GLYPH_H+10,"Close",211);
}

static u32 blend_color(u32 dst,u32 src,int alpha){
    int sr=(src>>16)&0xff,sg=(src>>8)&0xff,sb=src&0xff;
    int dr=(dst>>16)&0xff,dg=(dst>>8)&0xff,db=dst&0xff;
    int r=dr+((sr-dr)*alpha>>8),g=dg+((sg-dg)*alpha>>8),b=db+((sb-db)*alpha>>8);
    return(u32)((r<<16)|(g<<8)|b);
}
static u32 lerp_color(u32 a,u32 b,int x,int w){
    if(w<=1)return a;
    int ar=(a>>16)&0xff,ag=(a>>8)&0xff,ab=a&0xff;
    int br=(b>>16)&0xff,bg=(b>>8)&0xff,bb=b&0xff;
    int r=ar+(br-ar)*x/(w-1),g=ag+(bg-ag)*x/(w-1),bv=ab+(bb-ab)*x/(w-1);
    return(u32)((r<<16)|(g<<8)|bv);
}
static void fill_gradient_h(int x,int y,int w,int h,u32 ca,u32 cb){fb_fill_gradient_h(x,y,w,h,ca,cb);}
static void fill_gradient_v(int x,int y,int w,int h,u32 ca,u32 cb){fb_fill_gradient_v(x,y,w,h,ca,cb);}

static void draw_char(int x,int y,char c,u32 fg,int tbg,u32 bg){
    if((unsigned char)c<0x20||(unsigned char)c>0x7e)c='?';
    int idx=(unsigned char)c-0x20;
    for(int row=0;row<8;row++){
        u8 bits=font8x8_data[idx][row];
        for(int col=0;col<8;col++){
            int on=(bits>>(7-col))&1;
            for(int sy=0;sy<GLYPH_SCALE;sy++)for(int sx=0;sx<GLYPH_SCALE;sx++){
                int px=x+col*GLYPH_SCALE+sx,py=y+row*GLYPH_SCALE+sy;
                if(on)fb_put_pixel(px,py,fg);else if(!tbg)fb_put_pixel(px,py,bg);
            }
        }
    }
}
static int text_w(const char*s){int n=0;while(s[n])n++;return n*GLYPH_W;}
static void draw_text_clip(int x,int y,const char*s,u32 fg,int tbg,u32 bg,int max_x){
    while(*s){if(max_x>0&&x+GLYPH_W>max_x)break;if(x>=0&&x<SCR_W)draw_char(x,y,*s,fg,tbg,bg);x+=GLYPH_W;s++;}
}
static void draw_text(int x,int y,const char*s,u32 fg,int tbg,u32 bg){draw_text_clip(x,y,s,fg,tbg,bg,SCR_W);}
static void draw_text_c(int x,int y,int w,const char*s,u32 fg,int tbg,u32 bg){int ox=(w-text_w(s))/2;if(ox<0)ox=0;draw_text_clip(x+ox,y,s,fg,tbg,bg,x+w);}
static void draw_text_r(int x,int y,int w,const char*s,u32 fg,int tbg,u32 bg){int ox=w-text_w(s);if(ox<0)ox=0;draw_text_clip(x+ox,y,s,fg,tbg,bg,x+w);}

static void wcopy(char*dst,const char*src,int max){int i=0;while(src[i]&&i<max-1){dst[i]=src[i];i++;}dst[i]='\0';}
static int wgt_new(int win_id,int wid){
    if(g_wgt_cnt>=GUI_MAX_WIDGETS)return -1;
    int id=g_wgt_cnt++;if(id<0||id>=GUI_MAX_WIDGETS){g_wgt_cnt--;return -1;}
    GuiWidget*w=&g_wgts[id];w->win_id=win_id;w->id=wid;w->type=WT_NONE;
    w->checked=w->progress=w->x=w->y=w->w=w->h=0;w->text[0]=w->text2[0]='\0';
    return id;
}

int gui_window_create(int x,int y,int w,int h,const char*title){
    int id=-1;for(int i=0;i<GUI_MAX_WINDOWS;i++)if(!g_wins[i].visible){id=i;break;}
    if (id < 0) return -1;
    if (w < 120) { w = 120; }
    if (h < 80) { h = 80; }
    if (x + w > SCR_W) { x = SCR_W - w; }
    if (y + h > SCR_H) { y = SCR_H - h; }
    if (x < 0) { x = 0; }
    if (y < 0) { y = 0; }
    if(x<0||y<0||x+w>SCR_W||y+h>SCR_H)return -1;
    GuiWindow*win=&g_wins[id];
    win->x=x;win->y=y;win->w=w;win->h=h;
    win->anim_x=x;win->anim_y=y;win->anim_w=w;win->anim_h=h;
    wcopy(win->title,title,64);
    win->visible=1;win->active=0;win->modal=0;win->minimized=0;win->maximized=0;
    win->anim_state=WIN_NORMAL;win->anim_progress=100;win->fade_alpha=255;
    win->pixbuf = (u32*)heap_malloc((usize)w * (usize)h * 4);
    win->dirty = 1;
    gui_window_set_active(id);
    return id;
}

void gui_window_close(int id){
    if(id<0||id>=GUI_MAX_WINDOWS||!g_wins[id].visible)return;
    g_desktop_dirty=1;g_dock_dirty=1;
    g_wins[id].visible=0;g_wins[id].anim_state=WIN_NORMAL;
    if(g_active==id)g_active=-1;
    gui_redraw();
}

void gui_window_set_active(int id){
    for(int i=0;i<GUI_MAX_WINDOWS;i++)g_wins[i].active=(i==id);
    g_active=id;
}
int gui_window_get_active(void){return g_active;}

int gui_widget_add_button(int wid,int x,int y,int w,int h,const char*text,int id){
    int i=wgt_new(wid,id);if(i<0)return -1;
    g_wgts[i].type=WT_BUTTON;g_wgts[i].x=x;g_wgts[i].y=y;g_wgts[i].w=w;g_wgts[i].h=h?h:GLYPH_H+10;
    wcopy(g_wgts[i].text,text,64);return i;
}
int gui_widget_add_label(int wid,int x,int y,const char*text,int id){
    int i=wgt_new(wid,id);if(i<0)return -1;
    g_wgts[i].type=WT_LABEL;g_wgts[i].x=x;g_wgts[i].y=y;g_wgts[i].w=text_w(text);wcopy(g_wgts[i].text,text,64);return i;
}
static int gui_widget_add_label_dim(int wid,int x,int y,const char*text,int id){
    int i=wgt_new(wid,id);if(i<0)return -1;
    g_wgts[i].type=WT_LABEL_DIM;g_wgts[i].x=x;g_wgts[i].y=y;g_wgts[i].w=text_w(text);wcopy(g_wgts[i].text,text,64);return i;
}
int gui_widget_add_checkbox(int wid,int x,int y,const char*text,int chk,int id){
    int i=wgt_new(wid,id);if(i<0)return -1;
    g_wgts[i].type=WT_CHECKBOX;g_wgts[i].x=x;g_wgts[i].y=y;g_wgts[i].checked=chk;g_wgts[i].w=GLYPH_H+8+text_w(text);wcopy(g_wgts[i].text,text,64);return i;
}
int gui_widget_add_progress(int wid,int x,int y,int w,int prog,int id){
    int i=wgt_new(wid,id);if(i<0)return -1;
    g_wgts[i].type=WT_PROGRESS;g_wgts[i].x=x;g_wgts[i].y=y;g_wgts[i].w=w;g_wgts[i].h=GLYPH_H+6;g_wgts[i].progress=prog;return i;
}
int gui_widget_add_separator(int wid,int y,int id){
    int i=wgt_new(wid,id);if(i<0)return -1;g_wgts[i].type=WT_SEPARATOR;g_wgts[i].y=y;return i;
}
int gui_widget_add_listrow(int wid,int x,int y,int w,const char*nm,const char*sz,int id){
    int i=wgt_new(wid,id);if(i<0)return -1;
    g_wgts[i].type=WT_LISTROW;g_wgts[i].x=x;g_wgts[i].y=y;g_wgts[i].w=w;g_wgts[i].h=GLYPH_H+8;wcopy(g_wgts[i].text,nm,64);wcopy(g_wgts[i].text2,sz,32);return i;
}
int gui_widget_add_textinput(int wid,int x,int y,int w,const char*ph,int id){
    int i=wgt_new(wid,id);if(i<0)return -1;
    g_wgts[i].type=WT_TEXTINPUT;g_wgts[i].x=x;g_wgts[i].y=y;g_wgts[i].w=w;g_wgts[i].h=GLYPH_H+10;g_wgts[i].cursor_pos=0;g_wgts[i].input_buf[0]='\0';wcopy(g_wgts[i].text,ph,64);return i;
}
int gui_widget_add_icon(int wid,int x,int y,int icon_type,int id){
    int i=wgt_new(wid,id);if(i<0)return -1;
    g_wgts[i].type=WT_ICON;g_wgts[i].x=x;g_wgts[i].y=y;g_wgts[i].w=32;g_wgts[i].h=32;g_wgts[i].icon_type=icon_type;return i;
}

static void draw_window(int id){
    GuiWindow*win=&g_wins[id];if(!win->visible)return;
    int x=win->anim_x,y=win->anim_y,w=win->anim_w,h=win->anim_h;
    if(win->anim_state==WIN_OPENING||win->anim_state==WIN_CLOSING){
        if(win->fade_alpha<=0)return;
    }

    /* === Soft multi-layer drop shadow (gradient falloff, clamped to screen) === */
    {
        int sa[]={180,120,70,35,15,6};
        int so=SHADOW_OFF;
        for(int i=0;i<6;i++){
            int d=i;
            int sx=x+so-d, sy=y+so-d, sw=w+d*2, sh=h+d*2;
            if (sx < 0) { sw += sx; sx = 0; }
            if (sy < 0) { sh += sy; sy = 0; }
            if (sx + sw > SCR_W) { sw = SCR_W - sx; }
            if (sy + sh > SCR_H) { sh = SCR_H - sy; }
            if (sw <= 0 || sh <= 0) continue;
            u32 sc = blend_color(0x000000, 0x050810, sa[i]);
            fb_draw_rect(sx, sy, sw, sh, sc);
        }
        int sx = x + so, sy = y + so, sw = w, sh = h;
        if (sx < 0) { sw += sx; sx = 0; }
        if (sy < 0) { sh += sy; sy = 0; }
        if (sx + sw > SCR_W) { sw = SCR_W - sx; }
        if (sy + sh > SCR_H) { sh = SCR_H - sy; }
        if (sw > 0 && sh > 0) fb_fill_rounded_rect(sx, sy, sw, sh, 0x000000);
    }

    /* === Active window multi-ring accent glow === */
    if(win->active&&!win->modal){
        if(x>3&&y>3&&x+w+6<SCR_W&&y+h+6<SCR_H){
            fb_draw_rounded_rect(x-3,y-3,w+6,h+6,blend_color(C_WIN_BG,C_ACCENT,18));
            fb_draw_rounded_rect(x-2,y-2,w+4,h+4,blend_color(C_WIN_BG,C_ACCENT,40));
        }
        fb_draw_rounded_rect(x-1,y-1,w+2,h+2,C_WIN_BORDER_A);
    } else if(!win->modal){
        fb_draw_rounded_rect(x-1,y-1,w+2,h+2,blend_color(C_WIN_BORDER,0x000000,60));
    }

    /* Window body */
    fb_fill_rounded_rect(x,y,w,h,C_WIN_BG);

    /* Border */
    u32 bc=win->active?C_WIN_BORDER_A:C_WIN_BORDER;
    fb_draw_rounded_rect(x,y,w,h,bc);
    if(win->active)fb_draw_rounded_rect(x+1,y+1,w-2,h-2,C_WIN_BORDER_A2);

    /* === Titlebar: richer 3-stop gradient + glass shine === */
    u32 tl=win->modal?0x3d0010:win->active?0x1e3248:0x141c28;
    u32 tr=win->modal?0x200008:win->active?0x0e1a28:0x0e141e;
    u32 tb=win->modal?0x180006:win->active?0x091218:0x080e16;
    /* Bottom half darker */
    int th2=TITLEBAR_H/2;
    fill_gradient_h(x+BORDER,y+BORDER,       w-BORDER*2,th2,   tl,tr);
    fill_gradient_h(x+BORDER,y+BORDER+th2,   w-BORDER*2,TITLEBAR_H-th2, tr,tb);
    /* Separator line at bottom of titlebar */
    fb_draw_hline(x+BORDER,y+BORDER+TITLEBAR_H-1,w-BORDER*2,win->active?C_ACCENT:C_WIN_BORDER);
    /* Glass shine: bright top line + softer second line */
    fb_draw_hline(x+BORDER+2,y+BORDER,   w-BORDER*2-4, blend_color(tl,0xffffff,win->active?22:10));
    fb_draw_hline(x+BORDER+3,y+BORDER+1, w-BORDER*2-6, blend_color(tl,0xffffff,win->active?10:4));
    /* Left accent edge on active titlebar */
    if(win->active&&!win->modal){
        fb_fill_rect(x+BORDER,y+BORDER,2,TITLEBAR_H,blend_color(C_ACCENT,C_WIN_BG,80));
    }

    /* === Traffic-light buttons — 22px, inner shine, symbol, outer ring === */
    int btn_sz=22;
    int by2=y+BORDER+(TITLEBAR_H-btn_sz)/2;
    /* Close — red */
    int bx=x+w-BORDER-btn_sz-4;
    {
        u32 bc2=C_CLOSE_BTN;
        /* Outer ring glow */
        fb_draw_rounded_rect(bx-1,by2-1,btn_sz+2,btn_sz+2,blend_color(bc2,0x000000,100));
        fb_fill_rounded_rect(bx,by2,btn_sz,btn_sz,bc2);
        /* Inner top shine */
        fb_draw_hline(bx+4,by2+2,btn_sz-8,blend_color(bc2,0xffffff,70));
        fb_draw_hline(bx+5,by2+3,btn_sz-10,blend_color(bc2,0xffffff,35));
        /* X symbol */
        fb_draw_line(bx+6,by2+6,bx+btn_sz-7,by2+btn_sz-7,blend_color(bc2,0x000000,180));
        fb_draw_line(bx+btn_sz-7,by2+6,bx+6,by2+btn_sz-7,blend_color(bc2,0x000000,180));
    }
    /* Maximize — green */
    int ax=bx-btn_sz-6;
    {
        u32 ac=C_MAX_BTN;
        fb_draw_rounded_rect(ax-1,by2-1,btn_sz+2,btn_sz+2,blend_color(ac,0x000000,100));
        fb_fill_rounded_rect(ax,by2,btn_sz,btn_sz,ac);
        fb_draw_hline(ax+4,by2+2,btn_sz-8,blend_color(ac,0xffffff,70));
        fb_draw_hline(ax+5,by2+3,btn_sz-10,blend_color(ac,0xffffff,35));
        /* Square expand symbol */
        fb_draw_rounded_rect(ax+5,by2+5,btn_sz-10,btn_sz-10,blend_color(ac,0x000000,160));
    }
    /* Minimize — yellow */
    int mnx=ax-btn_sz-6;
    {
        u32 mc=C_MIN_BTN;
        fb_draw_rounded_rect(mnx-1,by2-1,btn_sz+2,btn_sz+2,blend_color(mc,0x000000,100));
        fb_fill_rounded_rect(mnx,by2,btn_sz,btn_sz,mc);
        fb_draw_hline(mnx+4,by2+2,btn_sz-8,blend_color(mc,0xffffff,70));
        fb_draw_hline(mnx+5,by2+3,btn_sz-10,blend_color(mc,0xffffff,35));
        /* Minus symbol */
        fb_draw_hline(mnx+5,by2+btn_sz/2,btn_sz-10,blend_color(mc,0x000000,180));
        fb_draw_hline(mnx+5,by2+btn_sz/2+1,btn_sz-10,blend_color(mc,0x000000,120));
    }

    /* === Title text with 1px drop shadow === */
    int ty=y+BORDER+(TITLEBAR_H-GLYPH_H)/2;
    /* Shadow pass — 1px offset, dark */
    draw_text(x+BORDER+11,ty+1,win->title,blend_color(tl,0x000000,200),1,0);
    /* Main text */
    draw_text(x+BORDER+10,ty,win->title,win->active?C_TEXT:C_TEXT_MID,1,0);

    /* Client area */
    int sep_y=y+BORDER+TITLEBAR_H;
    int cx=x+BORDER,cy2=sep_y+1;
    int cw=w-BORDER*2,chh=h-BORDER-TITLEBAR_H-BORDER-1;
    if(chh<1)chh=1;

    /* Subtle alternating row tint */
    for(int r=0;r<chh;r+=6)fb_fill_rect(cx,cy2+r,cw,3,blend_color(C_WIN_BG,0xffffff,3));

    /* Shell window rendering */
    if(id==g_shell_win_id){
        int sy=cy2+6,line_h=GLYPH_H+6;
        int max_lines=(chh-10)/line_h;if(max_lines<1)max_lines=1;
        int total_lines=1;for(int i=0;i<g_shell_out_pos;i++)if(g_shell_output[i]=='\n')total_lines++;
        total_lines++;
        int start_line=total_lines>max_lines?total_lines-max_lines:0;
        int cur_line=0,line_start=0;
        for(int i=0;i<g_shell_out_pos&&cur_line<start_line;i++)if(g_shell_output[i]=='\n'){cur_line++;line_start=i+1;}
        int draw_y=sy,draw_x=cx+8,max_x=cx+cw-8;
        for(int i=line_start;i<g_shell_out_pos&&draw_y+GLYPH_H<cy2+chh-line_h;i++){
            if(g_shell_output[i]=='\n'){draw_y+=line_h;draw_x=cx+8;continue;}
            if(g_shell_output[i]>=' '&&g_shell_output[i]<='~'&&draw_x+GLYPH_W<max_x){draw_char(draw_x,draw_y,g_shell_output[i],C_SHELL_OUT,1,0);draw_x+=GLYPH_W;}
        }
        if(draw_y+GLYPH_H<cy2+chh){
            draw_x=cx+8;
            fb_fill_rect(cx,draw_y-2,cw,line_h+2,blend_color(C_WIN_BG,C_ACCENT,8));
            draw_text(draw_x,draw_y,"$ ",C_SHELL_PROMPT,1,0);draw_x+=text_w("$ ");
            for(int j=0;g_shell_buf[j]&&draw_x+GLYPH_W<max_x;j++){draw_char(draw_x,draw_y,g_shell_buf[j],C_SHELL_CMD,1,0);draw_x+=GLYPH_W;}
            int blink=(g_clock_ticks/30)%2;
            if(blink)fb_fill_rect(draw_x,draw_y,2,GLYPH_H,C_ACCENT);
        }
    }else{
        int clip_x1=cx,clip_y1=cy2,clip_x2=cx+cw,clip_y2=cy2+chh;
        for(int i=0;i<g_wgt_cnt;i++){
            GuiWidget*wg=&g_wgts[i];if(wg->win_id!=id)continue;
            int wx=cx+wg->x,wy=cy2+wg->y;
            if(wx>=clip_x2||wy>=clip_y2||wy+GLYPH_H<=clip_y1||wx+wg->w<=clip_x1)continue;
            if(wx<clip_x1) { wx=clip_x1; }
            if(wy<clip_y1) { wy=clip_y1; }
            switch(wg->type){
            case WT_LABEL:draw_text_clip(wx,wy,wg->text,C_TEXT,1,0,clip_x2);break;
            case WT_LABEL_DIM:draw_text_clip(wx,wy,wg->text,C_TEXT_DIM,1,0,clip_x2);break;
            case WT_BUTTON:{
                u32 btn_bg=(g_clicked_widget==i)?C_BTN_HL:C_BTN_BG;
                fb_fill_rounded_rect(wx,wy,wg->w,wg->h,btn_bg);
                fb_draw_rounded_rect(wx,wy,wg->w,wg->h,C_BTN_BORDER);
                fb_draw_hline(wx+3,wy,wg->w-6,C_BTN_TOP);
                draw_text_c(wx,wy+(wg->h-GLYPH_H)/2,wg->w,wg->text,C_TEXT,1,0);
                break;
            }
            case WT_CHECKBOX:{
                int bsz=GLYPH_H+4;
                fb_fill_rounded_rect(wx,wy,bsz,bsz,C_CHECKBOX_BOX);
                fb_draw_rounded_rect(wx,wy,bsz,bsz,wg->checked?C_ACCENT:C_WIN_BORDER);
                if(wg->checked){fb_draw_line(wx+3,wy+bsz/2,wx+bsz/2-1,wy+bsz-4,C_CHECKBOX_CHK);fb_draw_line(wx+bsz/2-1,wy+bsz-4,wx+bsz-3,wy+3,C_CHECKBOX_CHK);}
                draw_text(wx+bsz+8,wy+2,wg->text,C_TEXT,1,0);
                break;
            }
            case WT_PROGRESS:{
                int pw=wg->w,ph=wg->h?wg->h:GLYPH_H+6;
                fb_fill_rounded_rect(wx,wy,pw,ph,C_PROGRESS_BG);
                fb_draw_rounded_rect(wx,wy,pw,ph,C_WIN_BORDER);
                int fill=(pw-4)*wg->progress/100;
                if(fill>0){fill_gradient_h(wx+2,wy+2,fill,ph-4,C_PROGRESS_FG,C_PROGRESS_FG2);fb_draw_hline(wx+2,wy+2,fill,blend_color(C_PROGRESS_FG,0xffffff,80));}
                break;
            }
            case WT_SEPARATOR:fb_draw_hline(cx+2,wy,cw-4,C_SEP_LINE);fb_draw_hline(cx+2,wy+1,cw-4,blend_color(C_SEP_LINE,0xffffff,10));break;
            case WT_LISTROW:{
                int rh=wg->h?wg->h:GLYPH_H+8;
                if((wg->id&1)==0)fb_fill_rect(wx,wy,wg->w,rh,blend_color(C_WIN_BG,0xffffff,6));
                fb_fill_rect(wx,wy,3,rh,blend_color(C_ACCENT,C_WIN_BG,40));
                draw_text_clip(wx+10,wy+(rh-GLYPH_H)/2,wg->text,C_TEXT,1,0,clip_x2-6);
                if(wg->text2[0])draw_text_r(wx,wy+(rh-GLYPH_H)/2,wg->w-6,wg->text2,C_TEXT_DIM,1,0);
                fb_draw_hline(wx,wy+rh-1,wg->w,C_SEP_LINE);
                break;
            }
            case WT_TEXTINPUT:{
                fb_fill_rounded_rect(wx,wy,wg->w,wg->h,C_TEXTINPUT_BG);
                fb_draw_rounded_rect(wx,wy,wg->w,wg->h,C_TEXTINPUT_BRD);
                if(wg->input_buf[0])draw_text_clip(wx+8,wy+(wg->h-GLYPH_H)/2,wg->input_buf,C_TEXT,1,0,wx+wg->w-4);
                else draw_text_clip(wx+8,wy+(wg->h-GLYPH_H)/2,wg->text,C_TEXT_DIM,1,0,wx+wg->w-4);
                break;
            }
            case WT_ICON:{
                draw_app_icon(wx,wy,wg->w<wg->h?wg->w:wg->h,wg->icon_type,1);
                break;
            }
            default:break;
            }
        }
    }

    /* Bottom accent bar */
    if(win->active)fill_gradient_h(x+BORDER,y+h-BORDER-2,w-BORDER*2,3,C_ACCENT,C_ACCENT2);

    /* Resize handle */
    if(!win->maximized){
        int hx=x+w-RESIZE_HANDLE,hy=y+h-RESIZE_HANDLE;
        for(int i=0;i<4;i++){
            fb_fill_rect(hx+i*3,hy+i*3,RESIZE_HANDLE-i*3,2,C_ACCENT_DIM);
            fb_fill_rect(hx+i*3,hy+i*3,2,RESIZE_HANDLE-i*3,C_ACCENT_DIM);
        }
    }
}

/* Shared icon painter — used by dock, desktop, launcher, and widget icons.
   icon_type: 0=Terminal 1=Settings 2=Help 3=Files 4=Monitor 5=Sound
   sz: square size in pixels.  active: 1 = bright accent colours */
static void draw_app_icon(int ix, int iy, int sz, int icon_type, int active){
    int t=icon_type<6?icon_type:0;
    u32 col_top, col_bot, col_det;
    switch(t){
        case 0: col_top=0x1a3a2a; col_bot=0x0a1a10; col_det=0x40e080; break; /* Terminal  - green  */
        case 1: col_top=0x2a2a1a; col_bot=0x141408; col_det=0xe0d040; break; /* Settings  - yellow */
        case 2: col_top=0x3a1a1a; col_bot=0x1a0808; col_det=0xe04040; break; /* Help      - red    */
        case 3: col_top=0x1a2a3a; col_bot=0x081014; col_det=0x40a0e0; break; /* Files     - blue   */
        case 4: col_top=0x2a1a3a; col_bot=0x100814; col_det=0xa040e0; break; /* Monitor   - purple */
        case 5: col_top=0x3a2a1a; col_bot=0x14100a; col_det=0xe09030; break; /* Sound     - orange */
        default:col_top=C_ELEVATED2;col_bot=C_DESKTOP_ICON_BG;col_det=C_ACCENT;break;
    }
    if(!active){ col_det=blend_color(col_det,0x000000,120); col_top=blend_color(col_top,0x000000,80); col_bot=blend_color(col_bot,0x000000,80); }

    /* Body */
    fill_gradient_v(ix,iy,sz,sz,col_top,col_bot);
    fb_draw_rounded_rect(ix,iy,sz,sz,active?col_det:blend_color(col_det,0x000000,60));
    /* Shine */
    fb_draw_hline(ix+4,iy+2,sz-8,blend_color(col_det,0xffffff,active?40:20));
    fb_draw_hline(ix+5,iy+3,sz-10,blend_color(col_det,0xffffff,active?15:8));

    /* Pixel art — scale detail to icon size */
    int cx2=ix+sz/2, cy2=iy+sz/2;
    int s2=sz/4;  /* ~quarter-size unit for detail lines */
    switch(t){
        case 0: /* Terminal: prompt lines */
            fb_fill_rect(cx2-s2*2,cy2-s2-1,2,sz/3,col_det);
            fb_draw_hline(cx2-s2*2,cy2-s2-1,s2+1,col_det);
            fb_draw_hline(cx2-s2*2,cy2,    s2*2+2,col_det);
            fb_draw_hline(cx2-s2*2,cy2+s2, s2+s2/2,col_det);
            break;
        case 1: /* Settings: gear cross + corner bolts */
            fb_fill_rect(cx2-1,cy2-s2*2,2,s2*4,col_det);
            fb_fill_rect(cx2-s2*2,cy2-1,s2*4,2,col_det);
            fb_fill_rect(cx2-s2-1,cy2-s2-1,3,3,col_det);
            fb_fill_rect(cx2+s2-1,cy2-s2-1,3,3,col_det);
            fb_fill_rect(cx2-s2-1,cy2+s2-1,3,3,col_det);
            fb_fill_rect(cx2+s2-1,cy2+s2-1,3,3,col_det);
            fb_draw_rounded_rect(cx2-s2/2,cy2-s2/2,s2+1,s2+1,col_det);
            break;
        case 2: /* Help: ? mark */
            fb_draw_hline(cx2-s2,cy2-s2*2,s2*2,col_det);
            fb_draw_vline(cx2+s2,cy2-s2*2,s2+2,col_det);
            fb_draw_hline(cx2-1,cy2-s2+2,s2+2,col_det);
            fb_fill_rect(cx2-1,cy2,3,s2-1,col_det);
            fb_fill_rect(cx2-1,cy2+s2+1,3,3,col_det);
            break;
        case 3: /* Files: folder */
            fb_fill_rect(cx2-s2*2,cy2-s2/2,s2*4,s2*2+2,col_det);
            fb_fill_rect(cx2-s2*2,cy2-s2*2,s2+s2/2,s2+s2/2,col_det);
            fb_draw_hline(cx2-s2*2,cy2-s2/2,s2*4,blend_color(col_det,0xffffff,50));
            fb_draw_hline(cx2-s2,cy2+s2/2,s2*2,blend_color(col_det,0x000000,100));
            break;
        case 4: /* Monitor: screen */
            fb_draw_rect(cx2-s2*2,cy2-s2-2,s2*4,s2*3,col_det);
            fb_fill_rect(cx2-1,cy2+s2+1,2,s2,col_det);
            fb_draw_hline(cx2-s2,cy2+s2*2,s2*2,col_det);
            fb_draw_hline(cx2-s2,cy2-s2+1,s2,blend_color(col_det,0xffffff,60));
            fb_draw_hline(cx2-s2,cy2,    s2-2,blend_color(col_det,0xffffff,35));
            break;
        case 5: /* Sound: speaker + waves */
            fb_fill_rect(cx2-s2*2,cy2-s2/2,s2+1,s2+2,col_det);
            fb_draw_line(cx2-s2,cy2-s2*2,cx2,cy2-s2*2+s2/2,col_det);
            fb_draw_line(cx2-s2,cy2+s2*2,cx2,cy2+s2*2-s2/2,col_det);
            fb_draw_line(cx2,cy2-s2,cx2+s2,cy2-s2*2,col_det);
            fb_draw_line(cx2,cy2+s2,cx2+s2,cy2+s2*2,col_det);
            fb_draw_line(cx2+s2,cy2-s2/2,cx2+s2*2,cy2-s2,col_det);
            fb_draw_line(cx2+s2,cy2+s2/2,cx2+s2*2,cy2+s2,col_det);
            break;
    }
}

static void draw_desktop_icon(int idx){
    if(idx<0||idx>=g_desktop_icon_cnt)return;
    DesktopIcon *di=&g_desktop_icons[idx];
    int x=di->x,y=di->y;
    int icon_sz=38;
    int icon_x=x+(DESKTOP_ICON_W-icon_sz)/2;
    int icon_y=y+4;
    int t=di->icon_type<6?di->icon_type:0;
    /* Selection glow using per-type accent colour */
    u32 det_colors[]={0x40e080,0xe0d040,0xe04040,0x40a0e0,0xa040e0,0xe09030};
    u32 col_det=det_colors[t];
    if(di->selected){
        fb_draw_rect(icon_x-2,icon_y-2,icon_sz+4,icon_sz+4,blend_color(col_det,0x000000,80));
        fb_draw_rect(icon_x-1,icon_y-1,icon_sz+2,icon_sz+2,blend_color(col_det,0x000000,120));
    }
    draw_app_icon(icon_x,icon_y,icon_sz,t,1);

    /* Label with subtle background for readability */
    int label_y=y+DESKTOP_ICON_H-14;
    int label_w=DESKTOP_ICON_W-4;
    fb_fill_rect(x+2,label_y-1,label_w,GLYPH_H+2,blend_color(C_BASE,0x000000,180));
    draw_text_c(x,label_y,DESKTOP_ICON_W,di->name,di->selected?col_det:C_TEXT,0,0);
}

static void draw_desktop_icons(void){
    for(int i=0;i<g_desktop_icon_cnt;i++){
        DesktopIcon *di=&g_desktop_icons[i];
        if(g_launcher.visible&&di->x<g_launcher.x+g_launcher.w+4&&di->x+DESKTOP_ICON_W>g_launcher.x&&di->y<g_launcher.y+g_launcher.h+4&&di->y+DESKTOP_ICON_H>g_launcher.y)continue;
        draw_desktop_icon(i);
    }
}

static void draw_context_menu(void){
    if(!g_ctx_menu.visible)return;
    int x=g_ctx_menu.x,y=g_ctx_menu.y,w=g_ctx_menu.w,h=g_ctx_menu.h;
    for(int s=6;s>=2;s--)fb_draw_rect(x+s,y+s,w,h,blend_color(0x000000,C_BASE,255-s*30));
    fb_fill_rounded_rect(x,y,w,h,C_CTX_MENU_BG);
    fb_draw_rounded_rect(x,y,w,h,C_CTX_MENU_BORDER);
    int item_h=GLYPH_H+8;
    for(int i=0;i<g_ctx_menu.count;i++){
        int iy=y+4+i*item_h;
        if(i==g_ctx_menu.hovered_idx)fb_fill_rect(x+2,iy+2,w-4,item_h-4,C_CTX_MENU_HOVER);
        draw_text(x+10,iy+4,g_ctx_menu.labels[i],C_TEXT,1,0);
    }
}

static void draw_app_launcher(void){
    if(!g_launcher.visible)return;
    int x=g_launcher.x,y=g_launcher.y,w=g_launcher.w,h=g_launcher.h;
    for(int s=8;s>=2;s--)fb_draw_rect(x+s,y+s,w,h,blend_color(0x000000,C_BASE,255-s*25));
    fb_fill_rounded_rect(x,y,w,h,C_LAUNCHER_BG);
    fb_draw_rounded_rect(x,y,w,h,C_ACCENT_DIM);
    fill_gradient_h(x,y,w,GLYPH_H+10,C_ACCENT,C_ACCENT2);
    draw_text_c(x,y+4,w,"Applications",C_BASE,1,0);
    int item_h=44;
    for(int i=0;i<g_launcher.count;i++){
        int iy=y+GLYPH_H+14+i*item_h;
        if(i==g_launcher.hovered_idx)fb_fill_rect(x+4,iy+2,w-8,item_h-4,C_LAUNCHER_HOVER);
        int icon_x=x+8,icon_y=iy+6,icon_sz=28;
        draw_app_icon(icon_x,icon_y,icon_sz,g_launcher.icons[i],1);
        draw_text(x+42,iy+8,g_launcher.names[i],C_TEXT,1,0);
        draw_text(x+42,iy+8+GLYPH_H+2,g_launcher.descs[i],C_TEXT_DIM,1,0);
    }
}

static void draw_system_tray(void){
    int tray_y=(MENUBAR_H-GLYPH_H)/2;
    int tray_x=SCR_W-200;

    /* Network */
    fb_fill_rounded_rect(tray_x,tray_y,18,18,g_net_status?C_NET_ON:C_NET_OFF);
    if(g_net_status){fb_draw_hline(tray_x+5,tray_y+6,8,C_BASE);fb_draw_hline(tray_x+6,tray_y+9,6,C_BASE);fb_draw_hline(tray_x+7,tray_y+12,4,C_BASE);}
    else{fb_draw_line(tray_x+5,tray_y+5,tray_x+13,tray_y+13,C_BASE);}

    /* Volume */
    int vol_x=tray_x+26;
    u32 vol_color=g_volume_level>60?C_VOL_HIGH:(g_volume_level>30?C_VOL_MED:C_VOL_LOW);
    fb_fill_rounded_rect(vol_x,tray_y,18,18,vol_color);
    fb_put_pixel(vol_x+4,tray_y+8,C_BASE);fb_put_pixel(vol_x+5,tray_y+7,C_BASE);fb_put_pixel(vol_x+5,tray_y+9,C_BASE);
    fb_put_pixel(vol_x+6,tray_y+6,C_BASE);fb_put_pixel(vol_x+6,tray_y+10,C_BASE);
    fb_fill_rect(vol_x+7,tray_y+6,2,8,C_BASE);
    if(g_volume_level>30)fb_draw_hline(vol_x+10,tray_y+7,2,C_BASE);
    if(g_volume_level>60)fb_draw_hline(vol_x+13,tray_y+6,2,C_BASE);

    /* Clock */
    int clock_x=vol_x+26;
    char time_str[12];
    int h=g_clock_hour,m=g_clock_min,s=g_clock_sec;
    time_str[0]='0'+h/10;time_str[1]='0'+h%10;time_str[2]=':';
    time_str[3]='0'+m/10;time_str[4]='0'+m%10;time_str[5]=':';
    time_str[6]='0'+s/10;time_str[7]='0'+s%10;time_str[8]='\0';
    draw_text(clock_x,tray_y,time_str,C_TEXT_DIM,1,0);
}

static unsigned int g_star_seed=0x5EED1234;
static unsigned int star_rand(void){g_star_seed^=g_star_seed<<13;g_star_seed^=g_star_seed>>17;g_star_seed^=g_star_seed<<5;return g_star_seed;}

/* Blend aurora color additively onto a pixel — NO fb_get_pixel readback.
   We know the base is a near-black gradient so just additive-clamp. */
static void aurora_pixel(u32 *target, int px,int py,u32 col,int alpha){
    if(px<0||px>=SCR_W||py<0||py>=SCR_H)return;
    u32 base=lerp_color(0x070b11,0x050810,py,SCR_H);
    target[py*SCR_W+px] = blend_color(base,col,alpha);
}

static void aurora_band(u32 *target, int cy,int half_h,u32 col,int peak_alpha){
    for(int dy=-half_h;dy<=half_h;dy++){
        int y=cy+dy; if(y<MENUBAR_H||y>=SCR_H-DOCK_H)continue;
        int dist=dy<0?-dy:dy;
        int alpha=peak_alpha*(half_h-dist)/half_h;
        if(alpha<=0)continue;
        for(int x=0;x<SCR_W;x+=2) aurora_pixel(target,x,y,col,alpha);
    }
}

static void nebula_blob(u32 *target, int cx,int cy,int radius,u32 col,int peak_alpha){
    for(int dy=-radius;dy<=radius;dy++){
        for(int dx=-radius;dx<=radius;dx++){
            int d2=dx*dx+dy*dy;
            int r2=radius*radius;
            if(d2>=r2)continue;
            int dist_norm=(int)( (d2*256)/r2 );
            int alpha=peak_alpha*(256-dist_norm)/256;
            if(alpha<=0)continue;
            aurora_pixel(target,cx+dx,cy+dy,col,alpha);
        }
    }
}

static void render_desktop_to(u32 *target){
    if(!target) return;
    for(int row=0;row<SCR_H;row++){
        u32 c=lerp_color(0x070b11,0x050810,row,SCR_H);
        for(int col=0;col<SCR_W;col++) target[row*SCR_W+col] = c;
    }
    aurora_band(target, SCR_H-DOCK_H-60, 45, 0x004466, 55);
    aurora_band(target, MENUBAR_H+120,   38, 0x0a1a40, 40);
    aurora_band(target, MENUBAR_H+60,    25, 0x200830, 30);
    aurora_band(target, SCR_H-DOCK_H-30, 20, 0x003a52, 70);
    nebula_blob(target, SCR_W-180, MENUBAR_H+90,   90, 0x0c1840, 50);
    nebula_blob(target, 120,       SCR_H-DOCK_H-90,75, 0x0a2018, 45);
    nebula_blob(target, SCR_W/2+80,MENUBAR_H+180,  65, 0x18082c, 35);
    nebula_blob(target, 200,       MENUBAR_H+80,   55, 0x080e24, 40);
    /* Stars */
    g_star_seed=0x5EED1234;
    for(int s=0;s<200;s++){
        int sx=(int)(star_rand()%SCR_W);
        int sy=(int)(star_rand()%(SCR_H-MENUBAR_H-DOCK_H))+MENUBAR_H;
        int br=(int)(star_rand()%120)+40;
        target[sy*SCR_W+sx]= (u32)(((br)<<16)|((br)<<8)|br);
    }
}
static void gui_draw_overlays(void){
    draw_desktop_icons();
    if (MENUBAR_H > 0) {
        fill_gradient_v(0,0,SCR_W,MENUBAR_H,C_MENUBAR_BG,0x100c06);
        fb_draw_hline(0,MENUBAR_H-1,SCR_W,C_MENUBAR_SEP);
        fb_draw_hline(0,MENUBAR_H,SCR_W,blend_color(C_MENUBAR_SEP,0xffffff,8));
        int my=(MENUBAR_H-GLYPH_H)/2;
        int logo_x=54;
        draw_text(logo_x,my,"Systrix",C_ACCENT,1,0);
        int vbx=logo_x+text_w("Systrix")+10;
        int vbw=text_w("v0.1")+10;
        fb_fill_rounded_rect(vbx,my-1,vbw,GLYPH_H+2,blend_color(C_ACCENT,C_BASE,30));
        fb_draw_rounded_rect(vbx,my-1,vbw,GLYPH_H+2,C_ACCENT_DIM);
        draw_text(vbx+5,my,"v0.1",C_ACCENT,1,0);
    }
    draw_system_tray();
    draw_app_launcher();
    draw_context_menu();
}

static void draw_dock(void){
    /* Do NOT draw full-width bar anymore - floating look */

    /* Buttons config */
    struct{const char*label;int busy;int action;int icon_type;}btns[]={
        {"Apps",    0,0,-1}, /* Special icon for launcher */
        {"Files",   1,1,3},
        {"Term",    1,2,0},
        {"Settings",0,3,1},
        {"Monitor", 0,6,4},
        {"About",   0,4,2},
        {0,0,-1,0}
    };
    int n=0;while(btns[n].label)n++;
    int pill_w=n*(DOCK_ICON_SZ+DOCK_PAD)-DOCK_PAD+32;
    int pill_x=(SCR_W-pill_w)/2;
    int pill_y=SCR_H-DOCK_H-16;
    int pill_h=DOCK_ICON_SZ+16;

    /* Glass pill background */
    fb_fill_rounded_rect(pill_x,pill_y,pill_w,pill_h,C_DOCK_BG);
    fb_draw_rounded_rect(pill_x,pill_y,pill_w,pill_h,C_WIN_BORDER);
    /* Subtle shine on top edge */
    fb_draw_hline(pill_x+10, pill_y+1, pill_w-20, 0x444444);

    g_dock_btn_cnt=0;
    int bx=pill_x+16,by=pill_y+8;
    for(int i=0;btns[i].label;i++){
        int bw=DOCK_ICON_SZ,bh=DOCK_ICON_SZ;
        if (btns[i].icon_type == -1) {
            /* Draw a special Logo icon for launcher */
            fb_fill_rounded_rect(bx,by,bw,bh,C_ACCENT);
            fb_draw_rounded_rect(bx+8,by+8,bw-16,bh-16,0xffffff);
        } else {
            draw_app_icon(bx,by,bw,btns[i].icon_type,btns[i].busy);
        }
        
        if(btns[i].busy){
            fb_fill_rounded_rect(bx+bw/2-2,by+bh+4,4,4,C_ACCENT);
        }
        if(g_dock_btn_cnt<DOCK_BTN_MAX){
            g_dock_btns[g_dock_btn_cnt].x=bx;g_dock_btns[g_dock_btn_cnt].y=by;
            g_dock_btns[g_dock_btn_cnt].w=bw;g_dock_btns[g_dock_btn_cnt].h=bh;
            g_dock_btns[g_dock_btn_cnt].action=btns[i].action;g_dock_btn_cnt++;
        }
        bx+=bw+DOCK_PAD;
    }
}

static int gui_animate(void){
    int active=0;
    for(int i=0;i<GUI_MAX_WINDOWS;i++){
        GuiWindow *w=&g_wins[i];
        if(w->anim_state==WIN_OPENING){active=1;
            w->anim_progress+=ANIM_SPEED;
            int p=w->anim_progress>100?100:w->anim_progress;
            w->anim_x=w->x-(w->w/4)*(100-p)/100;
            w->anim_y=w->y-(w->h/4)*(100-p)/100;
            w->anim_w=w->w*p/100;
            w->anim_h=w->h*p/100;
            w->fade_alpha=p*255/100;
            if(p>=100){w->anim_x=w->x;w->anim_y=w->y;w->anim_w=w->w;w->anim_h=w->h;w->anim_state=WIN_NORMAL;w->fade_alpha=255;}
        }else if(w->anim_state==WIN_CLOSING){active=1;
            w->anim_progress-=ANIM_SPEED*2;
            int p=w->anim_progress<0?0:w->anim_progress;
            w->anim_x=w->x-(w->w/4)*(100-p)/100;
            w->anim_y=w->y-(w->h/4)*(100-p)/100;
            w->anim_w=w->w*p/100;
            w->anim_h=w->h*p/100;
            w->fade_alpha=p*255/100;
            if(p<=0){w->visible=0;w->anim_state=WIN_NORMAL;if(g_active==i)g_active=-1;}
        }else if(w->anim_state==WIN_MINIMIZING){active=1;
            w->anim_progress-=ANIM_SPEED*2;
            int p=w->anim_progress<0?0:w->anim_progress;
            w->anim_w=w->w*p/100;
            w->anim_h=w->h*p/100;
            w->anim_x=w->x+(w->w-w->anim_w)/2;
            w->anim_y=w->y+(w->h-w->anim_h)/2;
            w->fade_alpha=p*255/100;
            if(p<=0){w->visible=0;w->minimized=1;w->anim_state=WIN_MINIMIZED;w->anim_w=w->w;w->anim_h=w->h;w->anim_x=w->x;w->anim_y=w->y;w->fade_alpha=255;}
        }else if(w->anim_state==WIN_MAXIMIZING){active=1;
            w->anim_progress+=ANIM_SPEED;
            int p=w->anim_progress>100?100:w->anim_progress;
            w->anim_x=w->orig_x+(w->x-w->orig_x)*(100-p)/100+(BORDER-w->orig_x)*p/100;
            w->anim_y=w->orig_y+(w->y-w->orig_y)*(100-p)/100+(MENUBAR_H+BORDER-w->orig_y)*p/100;
            w->anim_w=w->orig_w+(w->w-w->orig_w)*(100-p)/100+(SCR_W-BORDER*2-w->orig_w)*p/100;
            w->anim_h=w->orig_h+(w->h-w->orig_h)*(100-p)/100+(SCR_H-MENUBAR_H-DOCK_H-BORDER*2-w->orig_h)*p/100;
            if(p>=100){w->anim_x=w->x;w->anim_y=w->y;w->anim_w=w->w;w->anim_h=w->h;w->anim_state=WIN_NORMAL;}
        }
    }
    if(g_launcher.visible&&g_launcher.anim_progress<100){
        g_launcher.anim_progress+=ANIM_SPEED*2;
        if(g_launcher.anim_progress>100)g_launcher.anim_progress=100;
        active=1;
    }
    return active;
}

static void gui_add_damage(int x, int y, int w, int h) {
    if (g_dmg_w <= 0) { g_dmg_x=x; g_dmg_y=y; g_dmg_w=w; g_dmg_h=h; return; }
    int x1 = x < g_dmg_x ? x : g_dmg_x;
    int y1 = y < g_dmg_y ? y : g_dmg_y;
    int x2 = (x+w) > (g_dmg_x+g_dmg_w) ? (x+w) : (g_dmg_x+g_dmg_w);
    int y2 = (y+h) > (g_dmg_y+g_dmg_h) ? (y+h) : (g_dmg_y+g_dmg_h);
    g_dmg_x=x1; g_dmg_y=y1; g_dmg_w=x2-x1; g_dmg_h=y2-y1;
}

void gui_redraw(void){
    if(!g_running) return;
    watchdog_pet();
    int animating=gui_animate();
    if (g_desktop_dirty) {
        render_desktop_to(g_desktop_cache);
        g_desktop_dirty = 0;
        g_dmg_w = SCR_W; g_dmg_h = SCR_H;
    }
    if (g_dmg_w <= 0 && !animating) {
        /* Even if no damage, we must ensure cursor is drawn if it was erased */
        if (!g_cur_drawn) { cursor_draw(); }
        return;
    }
    if (g_desktop_cache) fb_blit(g_dmg_x, g_dmg_y, g_dmg_w, g_dmg_h, g_desktop_cache, SCR_W);
    for(int i=0;i<GUI_MAX_WINDOWS;i++) if(g_wins[i].visible) draw_window(i);
    draw_dock();
    gui_draw_overlays();
    g_cur_drawn=0; cursor_draw();
    g_dmg_w = 0;
}

void cursor_blink_tick(void){
    g_clock_ticks++;
}

void gui_init(void){
    if(!fb_is_enabled()){fb_enable();if(!fb_is_enabled())return;}
    if(!g_desktop_cache) g_desktop_cache = (u32*)heap_malloc(fb_get_width() * fb_get_height() * 4);
    g_running=1;g_wgt_cnt=0;g_active=-1;
    /* Center cursor on current resolution */
    g_cur_x = fb_get_width()  / 2;
    g_cur_y = fb_get_height() / 2;
    g_shell_win_id=-1;g_shell_pos=0;g_shell_buf[0]='\0';
    g_shell_out_pos=0;g_shell_output[0]='\0';
    g_dragging=0;g_drag_win=-1;g_clicked_widget=-1;
    g_resizing=0;g_resize_win=-1;g_resize_dir=0;
    g_ctx_menu.visible=0;g_launcher.visible=0;g_launcher.anim_progress=100;
    for(int i=0;i<GUI_MAX_WINDOWS;i++){g_wins[i].visible=0;g_wins[i].minimized=0;g_wins[i].maximized=0;g_wins[i].anim_state=WIN_NORMAL;g_wins[i].anim_progress=100;g_wins[i].fade_alpha=255;}

    g_desktop_icon_cnt=0;
    int icon_x=SCR_W-DESKTOP_ICON_W-16,icon_y=MENUBAR_H+16;
    int icon_spacing_y=DESKTOP_ICON_H+8;
    const char *dnames[]={"Terminal","Settings","Monitor","About"};
    int dtypes[]={0,1,4,3},dactions[]={0,1,2,3};
    for(int i=0;i<4&&g_desktop_icon_cnt<MAX_DESKTOP_ICONS;i++){
        DesktopIcon *di=&g_desktop_icons[g_desktop_icon_cnt++];
        di->x=icon_x;di->y=icon_y+icon_spacing_y*i;di->selected=0;di->icon_type=dtypes[i];di->action=dactions[i];
        int j=0;while(dnames[i][j]&&j<31){di->name[j]=dnames[i][j];j++;}di->name[j]='\0';
    }

    g_launcher.count=5;g_launcher.x=4;g_launcher.y=MENUBAR_H+4;g_launcher.w=220;g_launcher.h=g_launcher.count*44+GLYPH_H+20;
    g_launcher.visible=0;g_launcher.hovered_idx=-1;g_launcher.anim_progress=100;
    const char *anames[]={"File Manager","Terminal","Settings","System Monitor","About"};
    const char *adescs[]={"Browse files","Command line","System config","Resource usage","System info"};
    int aicons[]={3,0,1,4,2},aacts[]={0,1,2,3,4};
    for(int i=0;i<g_launcher.count;i++){
        int j=0;while(anames[i][j]&&j<31){g_launcher.names[i][j]=anames[i][j];j++;}g_launcher.names[i][j]='\0';
        j=0;while(adescs[i][j]&&j<47){g_launcher.descs[i][j]=adescs[i][j];j++;}g_launcher.descs[i][j]='\0';
        g_launcher.icons[i]=aicons[i];g_launcher.actions[i]=aacts[i];
    }

    int row_h=GLYPH_H+8;
    int fm=gui_window_create(24,48,460,540,"File Manager");
    if(fm>=0){int cy=6;gui_widget_add_icon(fm,4,cy,0,1);gui_widget_add_label(fm,44,cy+6,"Files",10);cy+=row_h+4;
        gui_widget_add_separator(fm,cy,12);cy+=10;
        gui_widget_add_listrow(fm,0,cy,456,"README.TXT","27 B",20);cy+=row_h;
        gui_widget_add_listrow(fm,0,cy,456,"HELLO.C","8.2 KB",21);cy+=row_h;
        gui_widget_add_listrow(fm,0,cy,456,"MYPROGRAM","64 KB",22);cy+=row_h;
        gui_widget_add_listrow(fm,0,cy,456,"SHC","128 KB",23);cy+=row_h;
        gui_widget_add_listrow(fm,0,cy,456,"FIB.SHA","512 B",24);cy+=row_h;
        gui_widget_add_listrow(fm,0,cy,456,"HELLO.SHA","256 B",25);cy+=row_h;
        gui_widget_add_listrow(fm,0,cy,456,"POSIX_TEST","53 KB",26);cy+=row_h;
        cy+=6;gui_widget_add_separator(fm,cy,27);cy+=10;
        gui_widget_add_label_dim(fm,6,cy,"7 items  |  42.6 KB free",28);cy+=row_h;
        gui_widget_add_button(fm,6,cy,90,GLYPH_H+10,"Refresh",29);gui_widget_add_button(fm,104,cy,90,GLYPH_H+10,"Delete",30);
    }

    int term=gui_window_create(500,48,500,680,"Terminal");
    if(term>=0){g_shell_win_id=term;gui_shell_append("Systrix v0.1 - Terminal\nType 'help' for commands.\n\n");}

    int settings=gui_window_create(200,100,440,460,"Settings");
    if(settings>=0){int cy=8;gui_widget_add_icon(settings,8,cy,2,300);gui_widget_add_label(settings,48,cy+6,"System Settings",301);cy+=row_h+8;
        gui_widget_add_separator(settings,cy,302);cy+=10;
        gui_widget_add_label(settings,8,cy,"Display Options:",303);cy+=row_h+4;
        gui_widget_add_checkbox(settings,8,cy,"Enable shadows",1,304);cy+=row_h;
        gui_widget_add_checkbox(settings,8,cy,"Show grid pattern",1,305);cy+=row_h;
        gui_widget_add_checkbox(settings,8,cy,"Animate windows",1,306);cy+=row_h;
        cy+=8;gui_widget_add_separator(settings,cy,307);cy+=10;
        gui_widget_add_label(settings,8,cy,"Progress Demo:",308);cy+=row_h+4;
        gui_widget_add_progress(settings,8,cy,410,75,309);cy+=row_h+10;
        gui_widget_add_label_dim(settings,8,cy,"System load: 75%",310);cy+=row_h+8;
        gui_widget_add_separator(settings,cy,311);cy+=10;
        gui_widget_add_label(settings,8,cy,"Text Input:",312);cy+=row_h+4;
        gui_widget_add_textinput(settings,8,cy,410,"Type here...",313);cy+=row_h+10;
        gui_widget_add_button(settings,8,cy,90,GLYPH_H+10,"Apply",314);gui_widget_add_button(settings,106,cy,90,GLYPH_H+10,"Reset",315);gui_widget_add_button(settings,336,cy,90,GLYPH_H+10,"Close",316);
    }

    /* About window: not shown at startup - open from dock/desktop icon */

    gui_redraw();
}

void gui_shutdown(void){g_running=0;if(fb_is_enabled())fb_fill_rect(0,0,SCR_W,SCR_H,0x000000);}
int gui_is_active(void){return g_running;}
