/* ================================================================
 *  SystrixOS — include/senc.h
 *  SENC: SystrixOS Encoding — custom character encoding scheme.
 *
 *  Design:
 *    - Codepoints 0x00–0x7F : 1 byte  (ASCII-compatible, existing strings work)
 *    - Codepoints 0x80–0x3FFF: 2 bytes (up to 16256 custom codepoints)
 *
 *  2-byte encoding (high-bit continuation, similar intuition to UTF-8):
 *    byte[0] = 0x80 | (cp >> 7)       — high 7 bits, continuation flag set
 *    byte[1] = 0x80 | (cp & 0x7F)     — low  7 bits, continuation flag set
 *
 *  Decoding: if byte & 0x80, read two bytes and reconstruct codepoint.
 *            otherwise, single ASCII byte.
 *
 *  All existing char* strings are valid SENC strings (ASCII subset).
 *  Custom SENC codepoints live in include/senc_font.h as bitmap glyphs.
 * ================================================================ */
#pragma once

/* ----------------------------------------------------------------
 *  Types
 * ---------------------------------------------------------------- */
typedef unsigned int  senc_cp;   /* codepoint — u32-width */
typedef unsigned char senc_u8;

/* ----------------------------------------------------------------
 *  Limits
 * ---------------------------------------------------------------- */
#define SENC_ASCII_MAX   0x7Fu        /* last single-byte codepoint      */
#define SENC_CP_MAX      0x3FFFu      /* largest encodable codepoint      */
#define SENC_CUSTOM_BASE 0x80u        /* first custom (2-byte) codepoint  */

/* ----------------------------------------------------------------
 *  Well-known custom codepoints (add your own below)
 * ---------------------------------------------------------------- */
#define SENC_CP_BLOCK        0x80u  /* █  solid block                    */
#define SENC_CP_DRIVE        0x81u  /* 🖴  drive/disk icon               */
#define SENC_CP_NET          0x82u  /* 🌐  network/globe icon            */
#define SENC_CP_FOLDER       0x83u  /* 📁  folder icon                   */
#define SENC_CP_TERM         0x84u  /* >_  terminal icon                 */
#define SENC_CP_LOCK         0x85u  /* 🔒  lock icon                     */
#define SENC_CP_CPU          0x86u  /* ⚙   CPU/gear icon                 */
#define SENC_CP_MEM          0x87u  /* ≡   memory/bars icon              */
#define SENC_CP_ARROW_UP     0x88u  /* ▲   arrow up                      */
#define SENC_CP_ARROW_DOWN   0x89u  /* ▼   arrow down                    */
#define SENC_CP_ARROW_LEFT   0x8Au  /* ◀   arrow left                    */
#define SENC_CP_ARROW_RIGHT  0x8Bu  /* ▶   arrow right                   */
#define SENC_CP_CHECK        0x8Cu  /* ✓   checkmark                     */
#define SENC_CP_CROSS        0x8Du  /* ✗   cross/X mark                  */
#define SENC_CP_STAR         0x8Eu  /* ★   star                          */
#define SENC_CP_HEART        0x8Fu  /* ♥   heart                         */
#define SENC_CP_WIFI         0x90u  /* ))) wifi signal                   */
#define SENC_CP_BATTERY      0x91u  /* ▐▌  battery icon                  */
/* 0x92 was SENC_CP_SYSTRIX_LOGO (removed)                               */
#define SENC_CP_SNAKE        0x93u  /* ~ snake head                      */
#define SENC_CP_PACMAN       0x94u  /* C   pacman glyph                  */
#define SENC_CP_BELL         0x95u  /* 🔔  bell/alert icon               */
#define SENC_CP_MUSIC        0x96u  /* ♪   music note                    */
#define SENC_CP_SUN          0x97u  /* ☀   sun/brightness icon           */
#define SENC_CP_MOON         0x98u  /* ☽   moon icon                     */
#define SENC_CP_TRASH        0x99u  /* 🗑   trash/delete icon             */

/* ----------------------------------------------------------------
 *  Alphanumeric codepoints — A-Z, a-z, 0-9 encoded via SENC.
 *  Use senc_alpha(c) to map ASCII letters/digits to SENC codepoints.
 * ---------------------------------------------------------------- */
/* Uppercase A-Z: 0x9A - 0xB3 */
#define SENC_CP_UPPER_A      0x9Au
#define SENC_CP_UPPER_B      0x9Bu
#define SENC_CP_UPPER_C      0x9Cu
#define SENC_CP_UPPER_D      0x9Du
#define SENC_CP_UPPER_E      0x9Eu
#define SENC_CP_UPPER_F      0x9Fu
#define SENC_CP_UPPER_G      0xA0u
#define SENC_CP_UPPER_H      0xA1u
#define SENC_CP_UPPER_I      0xA2u
#define SENC_CP_UPPER_J      0xA3u
#define SENC_CP_UPPER_K      0xA4u
#define SENC_CP_UPPER_L      0xA5u
#define SENC_CP_UPPER_M      0xA6u
#define SENC_CP_UPPER_N      0xA7u
#define SENC_CP_UPPER_O      0xA8u
#define SENC_CP_UPPER_P      0xA9u
#define SENC_CP_UPPER_Q      0xAAu
#define SENC_CP_UPPER_R      0xABu
#define SENC_CP_UPPER_S      0xACu
#define SENC_CP_UPPER_T      0xADu
#define SENC_CP_UPPER_U      0xAEu
#define SENC_CP_UPPER_V      0xAFu
#define SENC_CP_UPPER_W      0xB0u
#define SENC_CP_UPPER_X      0xB1u
#define SENC_CP_UPPER_Y      0xB2u
#define SENC_CP_UPPER_Z      0xB3u
/* Lowercase a-z: 0xB4 - 0xCD */
#define SENC_CP_LOWER_A      0xB4u
#define SENC_CP_LOWER_B      0xB5u
#define SENC_CP_LOWER_C      0xB6u
#define SENC_CP_LOWER_D      0xB7u
#define SENC_CP_LOWER_E      0xB8u
#define SENC_CP_LOWER_F      0xB9u
#define SENC_CP_LOWER_G      0xBAu
#define SENC_CP_LOWER_H      0xBBu
#define SENC_CP_LOWER_I      0xBCu
#define SENC_CP_LOWER_J      0xBDu
#define SENC_CP_LOWER_K      0xBEu
#define SENC_CP_LOWER_L      0xBFu
#define SENC_CP_LOWER_M      0xC0u
#define SENC_CP_LOWER_N      0xC1u
#define SENC_CP_LOWER_O      0xC2u
#define SENC_CP_LOWER_P      0xC3u
#define SENC_CP_LOWER_Q      0xC4u
#define SENC_CP_LOWER_R      0xC5u
#define SENC_CP_LOWER_S      0xC6u
#define SENC_CP_LOWER_T      0xC7u
#define SENC_CP_LOWER_U      0xC8u
#define SENC_CP_LOWER_V      0xC9u
#define SENC_CP_LOWER_W      0xCAu
#define SENC_CP_LOWER_X      0xCBu
#define SENC_CP_LOWER_Y      0xCCu
#define SENC_CP_LOWER_Z      0xCDu
/* Digits 0-9: 0xCE - 0xD7 */
#define SENC_CP_DIGIT_0      0xCEu
#define SENC_CP_DIGIT_1      0xCFu
#define SENC_CP_DIGIT_2      0xD0u
#define SENC_CP_DIGIT_3      0xD1u
#define SENC_CP_DIGIT_4      0xD2u
#define SENC_CP_DIGIT_5      0xD3u
#define SENC_CP_DIGIT_6      0xD4u
#define SENC_CP_DIGIT_7      0xD5u
#define SENC_CP_DIGIT_8      0xD6u
#define SENC_CP_DIGIT_9      0xD7u

/* ----------------------------------------------------------------
 *  Map an ASCII letter or digit to its SENC codepoint.
 *  Returns the SENC codepoint, or the original cp if not a letter/digit.
 * ---------------------------------------------------------------- */
static inline senc_cp senc_alpha(senc_cp cp) {
    if (cp >= 'A' && cp <= 'Z') return SENC_CP_UPPER_A + (cp - 'A');
    if (cp >= 'a' && cp <= 'z') return SENC_CP_LOWER_A + (cp - 'a');
    if (cp >= '0' && cp <= '9') return SENC_CP_DIGIT_0 + (cp - '0');
    return cp;
}

/* ----------------------------------------------------------------
 *  Encode one codepoint → bytes.
 *  buf must have room for at least 2 bytes.
 *  Returns number of bytes written (1 or 2), or 0 on invalid cp.
 * ---------------------------------------------------------------- */
static inline int senc_encode(senc_cp cp, senc_u8 *buf) {
    if (cp <= SENC_ASCII_MAX) {
        buf[0] = (senc_u8)cp;
        return 1;
    }
    if (cp <= SENC_CP_MAX) {
        buf[0] = (senc_u8)(0x80u | (cp >> 7));
        buf[1] = (senc_u8)(0x80u | (cp & 0x7Fu));
        return 2;
    }
    return 0; /* codepoint out of range */
}

/* ----------------------------------------------------------------
 *  Decode one codepoint from bytes.
 *  Writes decoded codepoint to *cp_out.
 *  Returns number of bytes consumed (1 or 2).
 *  On malformed input, returns 1 and sets *cp_out = '?'.
 * ---------------------------------------------------------------- */
static inline int senc_decode(const senc_u8 *s, senc_cp *cp_out) {
    if (!(s[0] & 0x80u)) {
        /* single-byte ASCII */
        *cp_out = s[0];
        return 1;
    }
    if ((s[0] & 0x80u) && s[1] && (s[1] & 0x80u)) {
        /* two-byte custom codepoint */
        *cp_out = (senc_cp)((s[0] & 0x7Fu) << 7) | (senc_cp)(s[1] & 0x7Fu);
        return 2;
    }
    /* malformed — skip one byte */
    *cp_out = (senc_cp)'?';
    return 1;
}

/* ----------------------------------------------------------------
 *  Iterate over a SENC string.
 *  Usage:
 *    const senc_u8 *p = (const senc_u8 *)str;
 *    senc_cp cp;
 *    while ((p = senc_next(p, &cp))) { ... use cp ... }
 *
 *  Returns pointer to next char, or NULL at end of string.
 * ---------------------------------------------------------------- */
static inline const senc_u8 *senc_next(const senc_u8 *s, senc_cp *cp_out) {
    if (!s || !*s) return (void*)0;
    int n = senc_decode(s, cp_out);
    return s + n;
}

/* ----------------------------------------------------------------
 *  Count codepoints in a SENC string (not bytes).
 * ---------------------------------------------------------------- */
static inline int senc_strlen(const senc_u8 *s) {
    int n = 0;
    senc_cp cp;
    while ((s = senc_next(s, &cp))) n++;
    return n;
}

/* ----------------------------------------------------------------
 *  Convenience macro: embed a custom codepoint literal in a C string.
 *  Only works for codepoints 0x80–0x3FFF (2-byte range).
 *  Usage: "Status: " SENC_LIT(SENC_CP_CHECK) " OK"
 * ---------------------------------------------------------------- */
#define SENC_LIT(cp) \
    ((char[]){ (char)(0x80u | ((cp) >> 7)), \
               (char)(0x80u | ((cp) & 0x7Fu)), 0 })
