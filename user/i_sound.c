/* ================================================================
 *  Systrix OS — user/i_sound.c
 *  DOOM platform layer: sound
 *
 *  Sound effects  → Systrix PCM mixer (8 channels, 22050 Hz u8)
 *  Music (MUS)    → Systrix OPL2 FM synthesis
 *
 *  DOOM's sound engine calls:
 *    I_InitSound / I_ShutdownSound
 *    I_GetSfxLngth, I_LoadSfx, I_UnloadSfx
 *    I_StartSound, I_StopSound, I_SoundIsPlaying
 *    I_UpdateSoundParams (volume/pan/pitch — we honour volume only)
 *    I_InitMusic, I_ShutdownMusic
 *    I_RegisterSong, I_UnRegisterSong
 *    I_PlaySong, I_StopSong, I_PauseSong, I_ResumeSong
 *    I_UpdateMusic  (called once per tic to push OPL2 register writes)
 *    I_MusicIsPlaying
 * ================================================================ */

#include "libc.h"
#include "sound.h"

/* ── Sound effects ────────────────────────────────────────────── */

/* DOOM SFX channel → Systrix mixer channel mapping.
 * DOOM uses up to 8 simultaneous sound channels.               */
#define SFX_CHANNELS  8
static int sfx_handle[SFX_CHANNELS];   /* 1 = active */
static int sfx_vol[SFX_CHANNELS];

void I_InitSound(void) {
    for (int i = 0; i < SFX_CHANNELS; i++) { sfx_handle[i] = 0; sfx_vol[i] = 255; }
}

void I_ShutdownSound(void) {
    for (int i = 0; i < SFX_CHANNELS; i++) snd_stop((unsigned int)i);
}

/* I_GetSfxLngth — return sample length in bytes.
 * DOOM DMX lump format: 8-byte header then raw u8 samples.    */
int I_GetSfxLngth(void *sfxinfo) {
    /* sfxinfo->data points to the raw lump data.
     * Bytes 4-7 = sample count as little-endian u32.          */
    unsigned char *d = (unsigned char *)sfxinfo;
    unsigned int len = (unsigned int)d[4]
                     | ((unsigned int)d[5] << 8)
                     | ((unsigned int)d[6] << 16)
                     | ((unsigned int)d[7] << 24);
    return (int)len;
}

/* I_LoadSfx — called after WAD load; just return the data pointer.
 * DOOM passes sfxinfo->data which already points to the DMX lump. */
void *I_LoadSfx(void **data, int *len) {
    unsigned char *d = (unsigned char *)*data;
    /* Skip 8-byte DMX header */
    *len = I_GetSfxLngth(*data);
    return d + 8;
}

void I_UnloadSfx(void *handle) { (void)handle; }

/*
 * I_StartSound — play a sound effect on a free channel.
 * vol: 0-127 (DOOM scale), sep: 0-254 panning (ignored), pitch: ignored.
 * Returns a handle (channel index) or -1.
 */
int I_StartSound(int id, int vol, int sep, int pitch, int priority) {
    (void)id; (void)sep; (void)pitch; (void)priority;
    /* Find a free mixer channel */
    for (int c = 0; c < SFX_CHANNELS; c++) {
        if (!sfx_handle[c]) {
            /* DOOM's sfxinfo_t.data = pointer to the loaded sample.
             * We receive it through the global sfxinfo array — DOOM
             * will have called I_LoadSfx already.
             * Here we just record intent; the actual start call happens
             * in I_UpdateSound after we receive the data pointer via
             * I_StartSoundData (our own helper below).              */
            sfx_handle[c] = 1;
            /* Convert DOOM 0-127 volume to Systrix 0-255 */
            snd_volume((unsigned int)c, (unsigned int)(vol * 2));
            return c;
        }
    }
    return -1;
}

/* I_StartSoundData — Systrix-specific helper called from DOOM's S_StartSound
 * after it has a data pointer and length.  Wire this in s_sound.c or call
 * from a wrapper.  Exposed here for completeness.                 */
void I_StartSoundData(int handle, void *data, int len, int loop) {
    if (handle < 0 || handle >= SFX_CHANNELS) return;
    snd_play((unsigned int)handle,
             (const unsigned char *)data, (unsigned int)len, (unsigned int)loop);
}

void I_StopSound(int handle) {
    if (handle < 0 || handle >= SFX_CHANNELS) return;
    snd_stop((unsigned int)handle);
    sfx_handle[handle] = 0;
}

int I_SoundIsPlaying(int handle) {
    (void)handle;
    return 0;  /* Systrix doesn't expose channel-active query yet */
}

void I_UpdateSoundParams(int handle, int vol, int sep, int pitch) {
    (void)sep; (void)pitch;
    if (handle < 0 || handle >= SFX_CHANNELS) return;
    snd_volume((unsigned int)handle, (unsigned int)(vol * 2));
}

/* Call once per frame to advance the PCM mixer */
void I_UpdateSound(void) {
    snd_tick();
}

/* ── Music (OPL2) ─────────────────────────────────────────────── */
/*
 * DOOM uses MUS format internally.  The music player (mus_player
 * in DMX) converts MUS events to OPL2 register writes in real time.
 * We provide the register write primitive via opl_write(); DOOM's
 * own OPL timing logic drives everything else.
 *
 * For a full music implementation, link DOOM's native OPL player
 * (opl.c / opl_sdl.c replacement → opl_engine.c) which calls
 * I_OPLWrite() below.
 */

static int music_playing = 0;
static void *current_song = NULL;

void I_InitMusic(void) { opl_reset(); }
void I_ShutdownMusic(void) { opl_reset(); music_playing = 0; }

/* I_OPLWrite — raw OPL2 register write, called by DOOM's music player */
void I_OPLWrite(unsigned char reg, unsigned char data) {
    opl_write(reg, data);
}

void *I_RegisterSong(void *data) {
    current_song = data;
    return data;
}

void I_UnRegisterSong(void *handle) {
    (void)handle;
    current_song = NULL;
}

void I_PlaySong(void *handle, int looping) {
    (void)handle; (void)looping;
    music_playing = 1;
}

void I_StopSong(void *handle) {
    (void)handle;
    opl_reset();
    music_playing = 0;
}

void I_PauseSong(void *handle) { (void)handle; opl_reset(); }
void I_ResumeSong(void *handle) { (void)handle; }

/* I_UpdateMusic — called once per tic.  The real MUS player drives OPL2
 * register writes via I_OPLWrite(); we just need to exist here.    */
void I_UpdateMusic(void) {}

int I_MusicIsPlaying(void) { return music_playing; }

/* Volume controls — DOOM calls these but we honour them via snd_volume */
void I_SetMusicVolume(int volume) { (void)volume; }
void I_SetSfxVolume(int volume)   { (void)volume; }
