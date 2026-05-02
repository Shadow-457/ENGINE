/* ================================================================
 *  Systrix OS — user/sound.h
 *  Phase 3: Audio — user-space API
 *
 *  Two paths:
 *
 *  OPL2 FM (music):
 *      opl_write(0xB0, 0x25);   // raw register access for MUS/MIDI
 *      opl_note(ch, fnum, block, vol, 1);  // play note
 *      opl_note(ch, 0,    0,     0,  0);   // stop note
 *      opl_reset();                         // silence all channels
 *
 *  PCM mixer (sound effects):
 *      snd_play(0, my_samples, sizeof(my_samples), 0); // one-shot
 *      snd_play(1, bg_loop,    sizeof(bg_loop),    1); // looping
 *      snd_volume(0, 200);                              // 0-255
 *      snd_stop(0);
 *      snd_tick();    // call once per frame / PIT tick
 * ================================================================ */

#pragma once
#include "libc.h"

/* Syscall numbers */
#define SYS_SND_OPL_WRITE   320
#define SYS_SND_OPL_NOTE    321
#define SYS_SND_OPL_RESET   322
#define SYS_SND_MIX_PLAY    323
#define SYS_SND_MIX_STOP    324
#define SYS_SND_MIX_VOLUME  325
#define SYS_SND_MIX_TICK    326

/* ── Inline syscall helpers ───────────────────────────────────── */
static inline long __snd_sc2(long nr, long a, long b) {
    long r;
    __asm__ volatile("syscall"
        : "=a"(r) : "0"(nr), "D"(a), "S"(b) : "rcx","r11","memory");
    return r;
}
static inline long __snd_sc1(long nr, long a) {
    long r;
    __asm__ volatile("syscall"
        : "=a"(r) : "0"(nr), "D"(a) : "rcx","r11","memory");
    return r;
}
static inline long __snd_sc0(long nr) {
    long r;
    __asm__ volatile("syscall"
        : "=a"(r) : "0"(nr) : "rcx","r11","memory");
    return r;
}
static inline long __snd_sc5(long nr,long a,long b,long c,long d,long e){
    long r;
    register long _r10 __asm__("r10") = d;
    register long _r8  __asm__("r8")  = e;
    __asm__ volatile("syscall"
        : "=a"(r)
        : "0"(nr),"D"(a),"S"(b),"d"(c),"r"(_r10),"r"(_r8)
        : "rcx","r11","memory");
    return r;
}

/* ── OPL2 FM synthesis ────────────────────────────────────────── */

/* opl_write(reg, val) — raw OPL2 register write.
 * Use this when feeding output from a MUS/MIDI→OPL player directly. */
static inline void opl_write(unsigned char reg, unsigned char val) {
    __snd_sc2(SYS_SND_OPL_WRITE, (long)reg, (long)val);
}

/* opl_note(ch, fnum, block, vol, key_on)
 *   ch     : OPL2 channel 0-8
 *   fnum   : F-number 0-1023  (frequency × 2^(20-block) / 49716)
 *   block  : octave block 0-7
 *   vol    : 0=loudest .. 63=silent  (OPL2 attenuation convention)
 *   key_on : 1=note on, 0=note off
 *
 * Common F-numbers at block 4 (middle octave):
 *   C4=172  D4=193  E4=217  F4=230  G4=258  A4=290  B4=325  C5=344
 */
static inline void opl_note(unsigned int ch, unsigned int fnum,
                             unsigned int block, unsigned int vol,
                             unsigned int key_on) {
    __snd_sc5(SYS_SND_OPL_NOTE,
              (long)ch, (long)fnum, (long)block, (long)vol, (long)key_on);
}

/* opl_reset() — key-off all 9 OPL2 channels (use on level change etc.) */
static inline void opl_reset(void) {
    __snd_sc0(SYS_SND_OPL_RESET);
}

/* ── PCM mixer ────────────────────────────────────────────────── */

/* snd_play(ch, samples, len, loop)
 *   ch      : mixer channel 0-7
 *   samples : pointer to u8 PCM data (0-255, centre=128) at 22050Hz
 *   len     : byte count
 *   loop    : 1=loop, 0=one-shot
 * The sample buffer must remain valid while the channel is active. */
static inline void snd_play(unsigned int ch,
                             const unsigned char *samples,
                             unsigned int len, unsigned int loop) {
    register long _r10 __asm__("r10") = (long)loop;
    long r;
    __asm__ volatile("syscall"
        : "=a"(r)
        : "0"((long)SYS_SND_MIX_PLAY),
          "D"((long)ch),
          "S"((long)(unsigned long)samples),
          "d"((long)len),
          "r"(_r10)
        : "rcx","r11","memory");
    (void)r;
}

/* snd_stop(ch) — immediately stop channel */
static inline void snd_stop(unsigned int ch) {
    __snd_sc1(SYS_SND_MIX_STOP, (long)ch);
}

/* snd_volume(ch, vol) — set channel volume 0 (silent) to 255 (full) */
static inline void snd_volume(unsigned int ch, unsigned int vol) {
    __snd_sc2(SYS_SND_MIX_VOLUME, (long)ch, (long)vol);
}

/* snd_tick() — advance the software mixer by one PIT tick (~10ms).
 * Call once per frame in your game loop, or once per PIT interrupt.
 * Pushes ~220 mixed samples to the SB16 DAC. */
static inline void snd_tick(void) {
    __snd_sc0(SYS_SND_MIX_TICK);
}
