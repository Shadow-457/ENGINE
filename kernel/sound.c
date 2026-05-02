/* ================================================================
 *  Systrix OS — kernel/sound.c
 *  Phase 3: Audio
 *
 *  Two independent audio subsystems:
 *
 *  1. OPL2 FM SYNTHESIS  (AdLib / OPL2, ports 0x388–0x389)
 *     QEMU emulates an OPL2 when -soundhw adlib is passed or when
 *     sb16 is present (-soundhw sb16 includes OPL2).
 *     We expose a minimal but complete OPL2 register interface:
 *       sys_snd_opl_write(reg, val)  — write one OPL register
 *       sys_snd_opl_note(ch, freq, vol, on) — play/stop a note
 *     This is enough for DOOM's OPL music engine (mus2midi→OPL).
 *
 *  2. PCM MIXER  (8-channel software mixer → SB16 DAC)
 *     SB16 base port 0x220 (QEMU default).  We use the direct DAC
 *     output register (DSP command 0x10) for simplicity — no DMA
 *     required.  Samples are u8 (0-255, centre=128) at 22050 Hz.
 *     The PIT fires at ~100Hz; each tick we push ~220 samples.
 *       sys_snd_mix_play(id, samples, len, loop) — queue a sound
 *       sys_snd_mix_stop(id)                     — stop channel
 *       sys_snd_mix_tick()                        — advance mixer
 *     For DOOM: sound effects use the PCM mixer; music uses OPL2.
 *
 *  Syscall numbers (SYS_SND_*):
 *    320  sys_snd_opl_write   (reg, val)
 *    321  sys_snd_opl_note    (ch, freq_num, vol, key_on)
 *    322  sys_snd_opl_reset   ()
 *    323  sys_snd_mix_play    (ch, samples, len, loop)
 *    324  sys_snd_mix_stop    (ch)
 *    325  sys_snd_mix_volume  (ch, vol)
 *    326  sys_snd_mix_tick    ()
 * ================================================================ */

#include "../include/kernel.h"

/* ── OPL2 ports ──────────────────────────────────────────────── */
#define OPL2_INDEX  0x388
#define OPL2_DATA   0x389
#define OPL2_STATUS 0x388   /* read */

/* OPL2 key register base addresses */
#define OPL2_REG_TEST       0x01   /* waveform select enable */
#define OPL2_REG_TIMER1     0x02
#define OPL2_REG_TIMER2     0x03
#define OPL2_REG_TIMER_CTRL 0x04
#define OPL2_REG_AM_VIB     0xBD   /* rhythm + AM/VIB depth */

/* Per-channel register bases (channel 0..8) */
#define OPL2_REG_FNUM_LO(ch)   (0xA0 + (ch))  /* F-number low 8 bits   */
#define OPL2_REG_FNUM_HI(ch)   (0xB0 + (ch))  /* block + F-num high + key-on */
#define OPL2_REG_FEEDBACK(ch)  (0xC0 + (ch))  /* feedback + algorithm  */

/* Operator register bases — two operators per channel */
#define OPL2_OP_OFFSET(ch, op) ((ch) < 3 ? (ch)+(op)*3 : \
                                 (ch) < 6 ? (ch)+(op)*3+1 : \
                                            (ch)+(op)*3+2)

/* Standard operator register offsets */
#define OPL2_REG_KSL_TL(op)   (0x40 + (op))  /* KSL + total level     */
#define OPL2_REG_AR_DR(op)    (0x60 + (op))  /* attack + decay        */
#define OPL2_REG_SL_RR(op)    (0x80 + (op))  /* sustain + release     */
#define OPL2_REG_AM_EG(op)    (0x20 + (op))  /* AM/VIB/EG/KSR/mult   */
#define OPL2_REG_WAVE(op)     (0xE0 + (op))  /* waveform select       */

/* OPL2 channel to operator slot mapping (standard AdLib layout) */
static const u8 opl2_op[9][2] = {
    {0x00, 0x03}, {0x01, 0x04}, {0x02, 0x05},
    {0x08, 0x0B}, {0x09, 0x0C}, {0x0A, 0x0D},
    {0x10, 0x13}, {0x11, 0x14}, {0x12, 0x15},
};

/* Write a value to an OPL2 register with the required delay */
static void opl2_write(u8 reg, u8 val) {
    outb(OPL2_INDEX, reg);
    /* AdLib spec: 3.3µs delay after index write (~6 I/O reads) */
    for (int i = 0; i < 6; i++) inb(OPL2_STATUS);
    outb(OPL2_DATA, val);
    /* 23µs delay after data write (~35 I/O reads) */
    for (int i = 0; i < 35; i++) inb(OPL2_STATUS);
}

/* Detect OPL2 using the standard timer test */
static int opl2_detect(void) {
    /* Reset timers */
    outb(OPL2_INDEX, 0x04); outb(OPL2_DATA, 0x60);
    outb(OPL2_INDEX, 0x04); outb(OPL2_DATA, 0x80);
    u8 status1 = inb(OPL2_STATUS) & 0xE0;
    /* Start timer 1 */
    outb(OPL2_INDEX, 0x02); outb(OPL2_DATA, 0xFF);
    outb(OPL2_INDEX, 0x04); outb(OPL2_DATA, 0x21);
    /* Wait */
    for (volatile int i = 0; i < 10000; i++) {}
    u8 status2 = inb(OPL2_STATUS) & 0xE0;
    /* Reset again */
    outb(OPL2_INDEX, 0x04); outb(OPL2_DATA, 0x60);
    outb(OPL2_INDEX, 0x04); outb(OPL2_DATA, 0x80);
    return (status1 == 0x00) && (status2 == 0xC0);
}

static int opl2_ready = 0;

static void opl2_init(void) {
    if (opl2_ready) return;
    if (!opl2_detect()) return;  /* no OPL2 present */
    /* Reset all registers */
    for (int r = 0x01; r <= 0xF5; r++) opl2_write((u8)r, 0x00);
    /* Enable waveform select */
    opl2_write(OPL2_REG_TEST, 0x20);
    opl2_ready = 1;
}

/* ================================================================
 *  sys_snd_opl_write — raw OPL2 register write (for DOOM's music)
 * ================================================================ */
i64 sys_snd_opl_write(u8 reg, u8 val) {
    opl2_init();
    if (!opl2_ready) return (i64)EINVAL;
    opl2_write(reg, val);
    return 0;
}

/* ================================================================
 *  sys_snd_opl_note — convenience: play/stop a note on a channel
 *
 *  ch       0..8   OPL2 channel
 *  fnum     0..1023  F-number (frequency)
 *  block    0..7   octave block
 *  vol      0..63  volume (0=loudest, 63=silent — OPL convention)
 *  key_on   1=note on, 0=note off
 * ================================================================ */
i64 sys_snd_opl_note(u64 ch, u32 fnum, u32 block, u32 vol, u32 key_on) {
    opl2_init();
    if (!opl2_ready || ch >= 9) return (i64)EINVAL;

    u8 op_mod = opl2_op[ch][0];   /* modulator operator slot */
    u8 op_car = opl2_op[ch][1];   /* carrier operator slot   */

    /* Basic sine-wave patch: full envelope, no feedback */
    opl2_write(OPL2_REG_AM_EG(op_mod), 0x01);  /* sustain, multiplier=1 */
    opl2_write(OPL2_REG_AM_EG(op_car), 0x01);
    opl2_write(OPL2_REG_KSL_TL(op_mod), (u8)(vol & 0x3F));
    opl2_write(OPL2_REG_KSL_TL(op_car), 0x00);  /* carrier full volume */
    opl2_write(OPL2_REG_AR_DR(op_mod), 0xF0);   /* fast attack */
    opl2_write(OPL2_REG_AR_DR(op_car), 0xF0);
    opl2_write(OPL2_REG_SL_RR(op_mod), 0x77);
    opl2_write(OPL2_REG_SL_RR(op_car), 0x77);
    opl2_write(OPL2_REG_FEEDBACK(ch),  0x00);   /* no feedback, algo 0 */

    /* Set F-number low byte */
    opl2_write(OPL2_REG_FNUM_LO(ch), (u8)(fnum & 0xFF));
    /* Set block + F-number high bits + key-on */
    u8 hi = (u8)(((block & 0x07) << 2) | ((fnum >> 8) & 0x03));
    if (key_on) hi |= 0x20;
    opl2_write(OPL2_REG_FNUM_HI(ch), hi);

    return 0;
}

/* ================================================================
 *  sys_snd_opl_reset — silence all OPL2 channels
 * ================================================================ */
i64 sys_snd_opl_reset(void) {
    opl2_init();
    if (!opl2_ready) return 0;
    for (int ch = 0; ch < 9; ch++)
        opl2_write(OPL2_REG_FNUM_HI(ch), 0x00);  /* key-off all */
    return 0;
}

/* ================================================================
 *  PCM MIXER  — SB16 direct DAC output
 *
 *  SB16 base: 0x220.  We use DSP "direct" single-byte DAC output
 *  (DSP command 0x10) which doesn't need DMA.  This tops out at
 *  ~22050 samples/sec on real hardware; QEMU handles it fine.
 *
 *  8 channels, each playing a u8 sample buffer (0-255, centre=128).
 *  The mixer sums them, clamps to 0-255, and writes to the DAC.
 *  sys_snd_mix_tick() is called from kernel_main's PIT handler
 *  (or polled); each call pushes ~220 samples (100Hz × 22050).
 * ================================================================ */

#define SB16_BASE   0x220
#define SB16_RESET  (SB16_BASE + 0x06)
#define SB16_READ   (SB16_BASE + 0x0A)
#define SB16_WRITE  (SB16_BASE + 0x0C)   /* command/data port */
#define SB16_STATUS (SB16_BASE + 0x0C)   /* write: bit7=0 → ready */
#define SB16_ACK    (SB16_BASE + 0x0E)

#define SB16_CMD_DAC_DIRECT  0x10  /* write one byte directly to DAC */
#define SB16_CMD_SPEAKER_ON  0xD1
#define SB16_CMD_VERSION     0xE1

#define MIX_CHANNELS   8
#define MIX_RATE       22050
#define MIX_TICK_HZ    1000        /* PIT now fires at 1000 Hz     */
#define MIX_SAMPLES_PER_TICK  22   /* 22050 / 1000 = 22 samples    */

typedef struct {
    const u8 *buf;      /* sample data (u8, 0-255)    */
    u32       len;      /* total samples               */
    u32       pos;      /* current playback position   */
    u8        active;   /* 1 if playing                */
    u8        loop;     /* 1 to loop                   */
    u8        vol;      /* 0-255 channel volume        */
    u8        _pad;
} MixChannel;

static MixChannel mix_ch[MIX_CHANNELS];
static int sb16_ready = 0;

/* Write a byte to SB16 DSP, spin-wait for ready */
static void sb16_dsp_write(u8 val) {
    int t = 100000;
    while ((inb(SB16_STATUS) & 0x80) && --t) {}
    outb(SB16_WRITE, val);
}

/* Reset and detect SB16 */
static int sb16_init(void) {
    if (sb16_ready) return 1;

    /* Reset: write 1, wait, write 0 */
    outb(SB16_RESET, 1);
    for (volatile int i = 0; i < 10000; i++) {}
    outb(SB16_RESET, 0);

    /* Wait for 0xAA ready byte */
    int t = 100000;
    while (--t) {
        if ((inb(SB16_ACK) & 0x80)) {
            if (inb(SB16_READ) == 0xAA) { sb16_ready = 1; break; }
        }
    }
    if (!sb16_ready) return 0;

    /* Turn speaker on */
    sb16_dsp_write(SB16_CMD_SPEAKER_ON);
    return 1;
}

/* Write one sample to DAC directly */
static inline void sb16_dac_write(u8 sample) {
    sb16_dsp_write(SB16_CMD_DAC_DIRECT);
    sb16_dsp_write(sample);
}

/* ================================================================
 *  sys_snd_mix_play — start a PCM channel
 *
 *  ch      0..7    mixer channel
 *  samples pointer to u8 sample data (user VA, stays valid!)
 *  len     number of bytes/samples
 *  loop    1 = loop continuously, 0 = one-shot
 * ================================================================ */
i64 sys_snd_mix_play(u64 ch, const u8 *samples, u32 len, u32 loop) {
    if (ch >= MIX_CHANNELS || !samples || len == 0) return (i64)EINVAL;
    sb16_init();
    mix_ch[ch].buf    = samples;
    mix_ch[ch].len    = len;
    mix_ch[ch].pos    = 0;
    mix_ch[ch].loop   = (u8)(loop ? 1 : 0);
    mix_ch[ch].vol    = 255;
    mix_ch[ch].active = 1;
    return 0;
}

/* ================================================================
 *  sys_snd_mix_stop — halt a PCM channel
 * ================================================================ */
i64 sys_snd_mix_stop(u64 ch) {
    if (ch >= MIX_CHANNELS) return (i64)EINVAL;
    mix_ch[ch].active = 0;
    return 0;
}

/* ================================================================
 *  sys_snd_mix_volume — set channel volume (0-255)
 * ================================================================ */
i64 sys_snd_mix_volume(u64 ch, u32 vol) {
    if (ch >= MIX_CHANNELS) return (i64)EINVAL;
    mix_ch[ch].vol = (u8)(vol > 255 ? 255 : vol);
    return 0;
}

/* ================================================================
 *  sys_snd_mix_tick — advance mixer by one PIT tick (~220 samples)
 *  Call from PIT ISR or kernel poll loop once per ~10ms.
 * ================================================================ */
i64 sys_snd_mix_tick(void) {
    if (!sb16_ready) { sb16_init(); return 0; }

    for (int s = 0; s < MIX_SAMPLES_PER_TICK; s++) {
        int sum = 0;
        int active = 0;

        for (int c = 0; c < MIX_CHANNELS; c++) {
            MixChannel *mc = &mix_ch[c];
            if (!mc->active) continue;
            active = 1;
            u8 sample = mc->buf[mc->pos];
            /* Scale by channel volume: shift 8 bits */
            int scaled = ((int)sample * (int)mc->vol) >> 8;
            sum += scaled;
            mc->pos++;
            if (mc->pos >= mc->len) {
                if (mc->loop) mc->pos = 0;
                else          mc->active = 0;
            }
        }

        if (!active) break;  /* nothing playing — skip DAC writes */

        /* Clamp mixed sum to 0-255 */
        if (sum > 255) sum = 255;
        if (sum < 0)   sum = 0;
        sb16_dac_write((u8)sum);
    }
    return 0;
}
