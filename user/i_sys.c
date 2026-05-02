/* ================================================================
 *  Systrix OS — user/i_sys.c
 *  DOOM platform layer: system
 *
 *  Covers timing, keyboard input, error handling, and the main
 *  loop glue.  All Systrix-specific calls go through libc.h and
 *  gfx.h / sound.h wrappers.
 *
 *  DOOM calls (all must be defined):
 *    I_Init, I_Quit
 *    I_Error (va_list error with exit)
 *    I_GetTime  (returns tics; 35 tics/sec = DOOM standard rate)
 *    I_BaseTiccmd, I_BuildTiccmd
 *    I_StartTic   (poll input → event queue)
 * ================================================================ */

#include "libc.h"
#include "gfx.h"
#include "sound.h"

/* Forward declarations from i_sound.c and i_video.c */
void I_InitSound(void);
void I_ShutdownSound(void);
void I_InitMusic(void);
void I_ShutdownMusic(void);
void I_InitGraphics(void);
void I_ShutdownGraphics(void);

/* ── Timing ───────────────────────────────────────────────────── */
/*
 * DOOM runs at 35 tics per second.
 * gettime_ms() gives milliseconds since boot.
 * 1 tic = 1000/35 ≈ 28.57 ms.
 * We use fixed-point: tics = ms * 35 / 1000.
 */
#define TICRATE  35

int I_GetTime(void) {
    long long ms = gettime_ms();
    return (int)((ms * TICRATE) / 1000LL);
}

/* High-resolution version (for interpolation) — same source, finer units */
int I_GetTimeMS(void) {
    return (int)gettime_ms();
}

/* ── Initialisation / shutdown ───────────────────────────────── */
void I_Init(void) {
    I_InitSound();
    I_InitMusic();
    I_InitGraphics();
    /* Grab mouse for FPS look */
    mouse_setrelative(1);
}

void I_Quit(void) {
    I_ShutdownSound();
    I_ShutdownMusic();
    I_ShutdownGraphics();
    mouse_setrelative(0);
    exit(0);
}

/* ── Error handling ───────────────────────────────────────────── */
void I_Error(const char *error, ...) {
    va_list ap;
    va_start(ap, error);
    /* Print to stderr */
    char buf[512];
    vsnprintf(buf, sizeof(buf), error, ap);
    va_end(ap);
    /* Write directly — FILE* stderr might not be set up yet */
    write(2, "I_Error: ", 9);
    write(2, buf, strlen(buf));
    write(2, "\n", 1);
    I_Quit();
}

/* ── Input ────────────────────────────────────────────────────── */
/*
 * DOOM event types (from doomdef.h):
 *   ev_keydown=0, ev_keyup=1, ev_mouse=2, ev_joystick=3
 *
 * DOOM key codes relevant to Systrix:
 *   KEY_RIGHTARROW=0xae, KEY_LEFTARROW=0xac, KEY_UPARROW=0xad,
 *   KEY_DOWNARROW=0xaf, KEY_ESCAPE=27, KEY_ENTER=13, KEY_SPACE=32,
 *   KEY_CTRL=0x80|0x1d, KEY_SHIFT=0x80|0x36, KEY_ALT=0x80|0x38
 *   Lowercase ASCII for letter keys.
 */

/* DOOM event structure (must match doom's d_event.h exactly) */
typedef struct {
    int type;   /* ev_keydown=0 ev_keyup=1 ev_mouse=2 ev_joystick=3 */
    int data1;  /* key code / mouse buttons */
    int data2;  /* mouse x delta */
    int data3;  /* mouse y delta */
} event_t;

/* Forward declaration — defined in DOOM's d_main.c */
extern void D_PostEvent(event_t *ev);

/* Systrix scancode → DOOM key code translation */
static int sc_to_doom(unsigned char sc, unsigned char ascii) {
    /* Arrow keys (E0-prefixed stored as 0x80|sc) */
    if (sc == (0x80|0x48)) return 0xad;   /* up    */
    if (sc == (0x80|0x50)) return 0xaf;   /* down  */
    if (sc == (0x80|0x4B)) return 0xac;   /* left  */
    if (sc == (0x80|0x4D)) return 0xae;   /* right */
    if (sc == (0x80|0x53)) return 0x7f;   /* del   */
    /* Ctrl / Shift / Alt */
    if (sc == 0x1D || sc == 0x9D) return 0x80|0x1d;
    if (sc == 0x2A || sc == 0x36) return 0x80|0x36;
    if (sc == 0x38)               return 0x80|0x38;
    /* F1-F10 scancodes 0x3B-0x44 → DOOM KEY_F1=0xbb etc */
    if (sc >= 0x3B && sc <= 0x44) return 0xbb + (sc - 0x3B);
    /* Escape */
    if (sc == 0x01) return 27;
    /* Enter */
    if (sc == 0x1C) return 13;
    /* Printable ASCII — lowercase */
    if (ascii >= 'A' && ascii <= 'Z') return ascii + 32;
    if (ascii) return (int)ascii;
    return -1;
}

/*
 * I_StartTic — called by DOOM once per tic to collect pending input.
 * We drain the Systrix key and mouse ring buffers and post events.
 */
void I_StartTic(void) {
    /* ── Keyboard ── */
    KeyEvent keys[16];
    long n = poll_keys(keys, 16);
    for (long i = 0; i < n; i++) {
        int doom_key = sc_to_doom(keys[i].scancode, keys[i].ascii);
        if (doom_key < 0) continue;
        event_t ev;
        ev.type  = 0;   /* ev_keydown */
        ev.data1 = doom_key;
        ev.data2 = 0; ev.data3 = 0;
        D_PostEvent(&ev);
    }

    /* ── Mouse ── */
    MouseEvent mevs[8];
    long nm = poll_mouse(mevs, 8);
    int total_dx = 0, total_dy = 0;
    int buttons = 0;
    for (long i = 0; i < nm; i++) {
        total_dx += (int)mevs[i].dx;
        total_dy += (int)mevs[i].dy;
        buttons   = (int)mevs[i].buttons;
    }
    if (nm > 0) {
        event_t ev;
        ev.type  = 2;   /* ev_mouse */
        ev.data1 = buttons & 0x07;
        ev.data2 = total_dx * 4;    /* DOOM expects larger deltas */
        ev.data3 = total_dy * 4;
        D_PostEvent(&ev);
    }
}

/* ── Ticcmd stubs ─────────────────────────────────────────────── */
/* These are usually defined in g_game.c or i_net.c in DOOM.
 * Provided here as weak stubs so single-file builds link cleanly. */

typedef struct {
    char forwardmove;   /* *2048 for move */
    char sidemove;      /* *2048 for move */
    short angleturn;    /* <<16 for angle delta */
    short consistancy;
    unsigned char chatchar;
    unsigned char buttons;
} ticcmd_t;

void I_BaseTiccmd(ticcmd_t *cmd) {
    /* Zero the command — DOOM fills it in via G_BuildTiccmd */
    for (int i = 0; i < (int)sizeof(ticcmd_t); i++)
        ((unsigned char*)cmd)[i] = 0;
}

void I_BuildTiccmd(ticcmd_t *cmd) {
    I_BaseTiccmd(cmd);
}
