/* ================================================================
 *  Systrix OS — user/i_video.c
 *  DOOM platform layer: video
 *
 *  DOOM renders to a 320×200 software buffer (screens[0]).
 *  We scale it up 3× to 960×600 and centre it on the 1024×768
 *  framebuffer, then flip.  Colormap is the standard DOOM 256-colour
 *  palette passed via I_SetPalette().
 *
 *  Build alongside the other DOOM source files and link with
 *  crt0.o libc.o — no other Systrix-specific objects needed.
 * ================================================================ */

#include "libc.h"
#include "gfx.h"

/* DOOM screen dimensions */
#define DOOM_W  320
#define DOOM_H  200
#define SCALE   3

/* Centred position on 1024×768 */
#define DEST_X  ((1024 - DOOM_W * SCALE) / 2)   /* 32  */
#define DEST_Y  ((768  - DOOM_H * SCALE) / 2)   /* 84  */

/* Palette: 256 ARGB32 colours, set by I_SetPalette() */
static unsigned int pal[256];

/* Scale buffer: DOOM_W*SCALE × DOOM_H*SCALE pixels */
static unsigned int scale_buf[DOOM_W * SCALE * DOOM_H * SCALE];

void I_InitGraphics(void) {
    /* Nothing to open — gfx subsystem is always on */
}

void I_ShutdownGraphics(void) {
    /* Nothing to tear down */
}

/*
 * I_SetPalette — called by DOOM when the palette changes (e.g. damage
 * flash).  palette is 768 bytes: R G B × 256.
 */
void I_SetPalette(unsigned char *palette) {
    for (int i = 0; i < 256; i++) {
        unsigned char r = palette[i * 3 + 0];
        unsigned char g = palette[i * 3 + 1];
        unsigned char b = palette[i * 3 + 2];
        pal[i] = ((unsigned int)r << 16)
               | ((unsigned int)g <<  8)
               | ((unsigned int)b);
    }
}

/*
 * I_FinishUpdate — called once per frame after DOOM has filled
 * screens[0] with 320×200 palette indices.
 */
void I_FinishUpdate(void) {
    /* screens[0] is declared in DOOM's i_video.h as byte* */
    extern unsigned char *screens[5];
    const unsigned char *src = screens[0];

    /* Expand palette indices → ARGB32 with nearest-neighbour 3× scale */
    unsigned int *out = scale_buf;
    for (int y = 0; y < DOOM_H; y++) {
        /* Build one scaled row */
        unsigned int row[DOOM_W * SCALE];
        for (int x = 0; x < DOOM_W; x++) {
            unsigned int c = pal[src[y * DOOM_W + x]];
            row[x * SCALE + 0] = c;
            row[x * SCALE + 1] = c;
            row[x * SCALE + 2] = c;
        }
        /* Copy row SCALE times */
        for (int s = 0; s < SCALE; s++) {
            for (int x = 0; x < DOOM_W * SCALE; x++)
                *out++ = row[x];
        }
    }

    /* Blit scaled frame onto back buffer, then flip */
    blit(DEST_X, DEST_Y, DOOM_W * SCALE, DOOM_H * SCALE,
         scale_buf, COLORKEY_NONE);
    flip();
}

/*
 * I_ReadScreen — copy current frame into a flat buffer (used by
 * DOOM's screenshot code).
 */
void I_ReadScreen(unsigned char *scr) {
    extern unsigned char *screens[5];
    for (int i = 0; i < DOOM_W * DOOM_H; i++) scr[i] = screens[0][i];
}

/* I_UpdateNoBlit — no-op; DOOM calls this on some paths */
void I_UpdateNoBlit(void) {}
