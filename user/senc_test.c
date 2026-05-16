/* ================================================================
 *  SystrixOS — user/senc_test.c
 *  SENC encoding test suite — runs natively in SystrixOS.
 *
 *  Compile & install:
 *    make senc_test
 *    make addprog PROG=SENC_TEST
 *
 *  Run from SystrixOS shell:
 *    elf SENC_TEST
 * ================================================================ */
#include "libc.h"
#include "../include/senc.h"

/* ── tiny test harness ─────────────────────────────────────────── */
static int g_pass = 0;
static int g_fail = 0;

static void test_ok(const char *name) {
    printf("  [PASS] %s\r\n", name);
    g_pass++;
}
static void test_fail(const char *name, const char *reason) {
    printf("  [FAIL] %s — %s\r\n", name, reason);
    g_fail++;
}
#define EXPECT_EQ(name, got, expected) \
    do { if ((got) == (expected)) test_ok(name); \
         else { char _b[64]; sprintf(_b,"got 0x%X expected 0x%X",(unsigned)(got),(unsigned)(expected)); \
                test_fail(name,_b); } } while(0)
#define EXPECT_NE(name, got, notval) \
    do { if ((got) != (notval)) test_ok(name); \
         else test_fail(name,"values should differ"); } while(0)
#define EXPECT_STR(name, got, expected) \
    do { if (strcmp((got),(expected))==0) test_ok(name); \
         else { char _b[80]; sprintf(_b,"got \"%s\" expected \"%s\"",(got),(expected)); \
                test_fail(name,_b); } } while(0)

/* ── helper: hex dump one byte ─────────────────────────────────── */
static void print_hex(const senc_u8 *buf, int n) {
    for (int i = 0; i < n; i++) {
        printf("%02X ", (unsigned)buf[i]);
    }
}

/* ================================================================
 *  TEST GROUPS
 * ================================================================ */

/* 1. senc_encode — single-byte (ASCII) codepoints */
static void test_encode_ascii(void) {
    printf("\r\n[1] senc_encode — ASCII (single-byte) codepoints\r\n");
    senc_u8 buf[4];
    int n;

    n = senc_encode('A', buf);
    EXPECT_EQ("encode 'A' returns 1 byte",  n,      1);
    EXPECT_EQ("encode 'A' byte value",      buf[0], 'A');

    n = senc_encode('z', buf);
    EXPECT_EQ("encode 'z' returns 1 byte",  n,      1);
    EXPECT_EQ("encode 'z' byte value",      buf[0], 'z');

    n = senc_encode('0', buf);
    EXPECT_EQ("encode '0' returns 1 byte",  n,      1);
    EXPECT_EQ("encode '0' byte value",      buf[0], '0');

    n = senc_encode(' ', buf);
    EXPECT_EQ("encode ' ' returns 1 byte",  n,      1);
    EXPECT_EQ("encode ' ' byte value",      buf[0], ' ');

    n = senc_encode(0x7F, buf);
    EXPECT_EQ("encode 0x7F returns 1 byte", n,      1);
    EXPECT_EQ("encode 0x7F byte value",     buf[0], 0x7F);
}

/* 2. senc_encode — two-byte (custom) codepoints */
static void test_encode_custom(void) {
    printf("\r\n[2] senc_encode — custom (two-byte) codepoints\r\n");
    senc_u8 buf[4];
    int n;

    /* SENC_CP_BLOCK = 0x80 */
    n = senc_encode(SENC_CP_BLOCK, buf);
    EXPECT_EQ("encode BLOCK returns 2 bytes", n, 2);
    /* byte[0] = 0x80 | (0x80 >> 7) = 0x80 | 1 = 0x81 */
    EXPECT_EQ("encode BLOCK byte[0]", buf[0], 0x81);
    /* byte[1] = 0x80 | (0x80 & 0x7F) = 0x80 | 0 = 0x80 */
    EXPECT_EQ("encode BLOCK byte[1]", buf[1], 0x80);

    /* SENC_CP_CHECK = 0x8C */
    n = senc_encode(SENC_CP_CHECK, buf);
    EXPECT_EQ("encode CHECK returns 2 bytes", n, 2);
    /* byte[0] = 0x80 | (0x8C >> 7) = 0x80 | 1 = 0x81 */
    EXPECT_EQ("encode CHECK byte[0]", buf[0], 0x81);
    /* byte[1] = 0x80 | (0x8C & 0x7F) = 0x80 | 0x0C = 0x8C */
    EXPECT_EQ("encode CHECK byte[1]", buf[1], 0x8C);

    /* SENC_CP_UPPER_A = 0x9A */
    n = senc_encode(SENC_CP_UPPER_A, buf);
    EXPECT_EQ("encode UPPER_A returns 2 bytes", n, 2);
    /* byte[0] = 0x80 | (0x9A >> 7) = 0x80 | 1 = 0x81 */
    EXPECT_EQ("encode UPPER_A byte[0]", buf[0], 0x81);
    /* byte[1] = 0x80 | (0x9A & 0x7F) = 0x80 | 0x1A = 0x9A */
    EXPECT_EQ("encode UPPER_A byte[1]", buf[1], 0x9A);

    /* SENC_CP_DIGIT_9 = 0xD7 */
    n = senc_encode(SENC_CP_DIGIT_9, buf);
    EXPECT_EQ("encode DIGIT_9 returns 2 bytes", n, 2);
    /* byte[0] = 0x80 | (0xD7 >> 7) = 0x80 | 1 = 0x81 */
    EXPECT_EQ("encode DIGIT_9 byte[0]", buf[0], 0x81);
    /* byte[1] = 0x80 | (0xD7 & 0x7F) = 0x80 | 0x57 = 0xD7 */
    EXPECT_EQ("encode DIGIT_9 byte[1]", buf[1], 0xD7);

    /* Out-of-range codepoint */
    n = senc_encode(0x4000, buf);
    EXPECT_EQ("encode out-of-range returns 0", n, 0);
}

/* 3. senc_decode — round-trip */
static void test_decode(void) {
    printf("\r\n[3] senc_decode — round-trip encode→decode\r\n");
    senc_u8 buf[4];
    senc_cp cp_out;
    int n;

    /* ASCII 'H' */
    senc_encode('H', buf);
    n = senc_decode(buf, &cp_out);
    EXPECT_EQ("decode ASCII 'H' bytes consumed", n,      1);
    EXPECT_EQ("decode ASCII 'H' codepoint",      cp_out, (senc_cp)'H');

    /* Custom SENC_CP_HEART = 0x8F */
    senc_encode(SENC_CP_HEART, buf);
    n = senc_decode(buf, &cp_out);
    EXPECT_EQ("decode HEART bytes consumed", n,      2);
    EXPECT_EQ("decode HEART codepoint",      cp_out, SENC_CP_HEART);

    /* UPPER_Z = 0xB3 */
    senc_encode(SENC_CP_UPPER_Z, buf);
    n = senc_decode(buf, &cp_out);
    EXPECT_EQ("decode UPPER_Z bytes consumed", n,      2);
    EXPECT_EQ("decode UPPER_Z codepoint",      cp_out, SENC_CP_UPPER_Z);

    /* DIGIT_0 = 0xCE */
    senc_encode(SENC_CP_DIGIT_0, buf);
    n = senc_decode(buf, &cp_out);
    EXPECT_EQ("decode DIGIT_0 bytes consumed", n,      2);
    EXPECT_EQ("decode DIGIT_0 codepoint",      cp_out, SENC_CP_DIGIT_0);

    /* Malformed: high byte with no valid second byte */
    buf[0] = 0x81; buf[1] = 0x00;
    n = senc_decode(buf, &cp_out);
    EXPECT_EQ("decode malformed returns 1",     n,      1);
    EXPECT_EQ("decode malformed gives '?'",     cp_out, (senc_cp)'?');
}

/* 4. senc_alpha — ASCII letter/digit → SENC codepoint mapping */
static void test_senc_alpha(void) {
    printf("\r\n[4] senc_alpha — ASCII letter/digit mapping\r\n");

    EXPECT_EQ("senc_alpha('A') == UPPER_A", senc_alpha('A'), SENC_CP_UPPER_A);
    EXPECT_EQ("senc_alpha('Z') == UPPER_Z", senc_alpha('Z'), SENC_CP_UPPER_Z);
    EXPECT_EQ("senc_alpha('a') == LOWER_A", senc_alpha('a'), SENC_CP_LOWER_A);
    EXPECT_EQ("senc_alpha('z') == LOWER_Z", senc_alpha('z'), SENC_CP_LOWER_Z);
    EXPECT_EQ("senc_alpha('0') == DIGIT_0", senc_alpha('0'), SENC_CP_DIGIT_0);
    EXPECT_EQ("senc_alpha('9') == DIGIT_9", senc_alpha('9'), SENC_CP_DIGIT_9);

    /* Spot check: 'M' is the 13th letter (0-indexed = 12), so UPPER_A + 12 */
    EXPECT_EQ("senc_alpha('M') == UPPER_A+12", senc_alpha('M'), SENC_CP_UPPER_A + 12);
    /* 'n' lowercase: 'n' - 'a' = 13 */
    EXPECT_EQ("senc_alpha('n') == LOWER_A+13", senc_alpha('n'), SENC_CP_LOWER_A + 13);
    /* '5' digit: '5' - '0' = 5 */
    EXPECT_EQ("senc_alpha('5') == DIGIT_0+5",  senc_alpha('5'), SENC_CP_DIGIT_0 + 5);

    /* Non-alphanumeric — passthrough */
    EXPECT_EQ("senc_alpha('!') passthrough", senc_alpha('!'), (senc_cp)'!');
    EXPECT_EQ("senc_alpha(' ') passthrough", senc_alpha(' '), (senc_cp)' ');
    EXPECT_EQ("senc_alpha('.') passthrough", senc_alpha('.'), (senc_cp)'.');

    /* Custom codepoints — passthrough unchanged */
    EXPECT_EQ("senc_alpha(HEART) passthrough", senc_alpha(SENC_CP_HEART), SENC_CP_HEART);
}

/* 5. senc_strlen — codepoint count (not bytes) */
static void test_strlen(void) {
    printf("\r\n[5] senc_strlen — codepoint count\r\n");

    /* Pure ASCII string */
    EXPECT_EQ("strlen pure ASCII \"Hello\"", senc_strlen((const senc_u8*)"Hello"), 5);
    EXPECT_EQ("strlen empty string",         senc_strlen((const senc_u8*)""),     0);

    /* Mixed: ASCII + one 2-byte custom codepoint */
    senc_u8 mixed[8];
    mixed[0] = 'O';
    mixed[1] = 'K';
    /* embed SENC_CP_CHECK (2 bytes) */
    senc_encode(SENC_CP_CHECK, &mixed[2]);
    mixed[4] = '!';
    mixed[5] = '\0';
    /* "OK" + CHECK + "!" = 4 codepoints, but 5 bytes */
    EXPECT_EQ("strlen mixed ASCII+custom = 4 cp", senc_strlen(mixed), 4);

    /* All-custom: 3 two-byte codepoints */
    senc_u8 custom[8];
    senc_encode(SENC_CP_STAR,  &custom[0]);
    senc_encode(SENC_CP_HEART, &custom[2]);
    senc_encode(SENC_CP_CHECK, &custom[4]);
    custom[6] = '\0';
    EXPECT_EQ("strlen 3 custom codepoints = 3", senc_strlen(custom), 3);
}

/* 6. senc_next — iterator */
static void test_senc_next(void) {
    printf("\r\n[6] senc_next — string iterator\r\n");

    /* Build string: "Hi" + HEART + "!" */
    senc_u8 s[8];
    s[0] = 'H'; s[1] = 'i';
    senc_encode(SENC_CP_HEART, &s[2]);
    s[4] = '!';
    s[5] = '\0';

    const senc_u8 *p = s;
    senc_cp cp;
    int count = 0;
    senc_cp seen[8];

    while ((p = senc_next(p, &cp))) {
        seen[count++] = cp;
    }

    EXPECT_EQ("iterator visits 4 codepoints", count, 4);
    EXPECT_EQ("iterator cp[0] == 'H'",        seen[0], (senc_cp)'H');
    EXPECT_EQ("iterator cp[1] == 'i'",        seen[1], (senc_cp)'i');
    EXPECT_EQ("iterator cp[2] == HEART",      seen[2], SENC_CP_HEART);
    EXPECT_EQ("iterator cp[3] == '!'",        seen[3], (senc_cp)'!');
}

/* 7. Full alphanumeric codepoint range coverage */
static void test_alpha_range(void) {
    printf("\r\n[7] alphanumeric codepoint range — full A-Z, a-z, 0-9\r\n");
    int all_ok = 1;

    /* A-Z */
    for (int i = 0; i < 26; i++) {
        senc_cp expected = SENC_CP_UPPER_A + i;
        senc_cp got      = senc_alpha('A' + i);
        if (got != expected) { all_ok = 0; break; }
    }
    if (all_ok) test_ok("A-Z all map to UPPER_A..UPPER_Z");
    else        test_fail("A-Z range", "one or more mismatched");

    /* a-z */
    all_ok = 1;
    for (int i = 0; i < 26; i++) {
        senc_cp expected = SENC_CP_LOWER_A + i;
        senc_cp got      = senc_alpha('a' + i);
        if (got != expected) { all_ok = 0; break; }
    }
    if (all_ok) test_ok("a-z all map to LOWER_A..LOWER_Z");
    else        test_fail("a-z range", "one or more mismatched");

    /* 0-9 */
    all_ok = 1;
    for (int i = 0; i < 10; i++) {
        senc_cp expected = SENC_CP_DIGIT_0 + i;
        senc_cp got      = senc_alpha('0' + i);
        if (got != expected) { all_ok = 0; break; }
    }
    if (all_ok) test_ok("0-9 all map to DIGIT_0..DIGIT_9");
    else        test_fail("0-9 range", "one or more mismatched");

    /* All 62 alphanumeric round-trip encode→decode */
    all_ok = 1;
    senc_u8 buf[4];
    senc_cp cp_out;
    const char *alphanum =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    for (int i = 0; alphanum[i]; i++) {
        senc_cp cp = senc_alpha((senc_cp)(unsigned char)alphanum[i]);
        int n = senc_encode(cp, buf);
        if (n != 2) { all_ok = 0; break; }
        senc_decode(buf, &cp_out);
        if (cp_out != cp) { all_ok = 0; break; }
    }
    if (all_ok) test_ok("all 62 alphanumerics encode+decode round-trip (2 bytes each)");
    else        test_fail("alphanumeric round-trip", "encode/decode mismatch");
}

/* 8. SENC_LIT macro */
static void test_senc_lit(void) {
    printf("\r\n[8] SENC_LIT macro — embed custom codepoints in C strings\r\n");

    /* Build expected bytes for SENC_CP_CHECK manually */
    senc_u8 expected[4];
    senc_encode(SENC_CP_CHECK, expected);
    expected[2] = '\0';

    const char *lit = SENC_LIT(SENC_CP_CHECK);
    int ok = ((unsigned char)lit[0] == expected[0] &&
              (unsigned char)lit[1] == expected[1]);
    if (ok) test_ok("SENC_LIT(CHECK) bytes match senc_encode(CHECK)");
    else    test_fail("SENC_LIT", "byte mismatch");

    /* Build "Status: " + CHECK(2 bytes) + " OK" manually into a buffer.
     * SENC_LIT uses a compound literal so it can't be string-concatenated;
     * instead we memcpy the pieces together. */
    senc_u8 greeting[16];
    int pos = 0;
    const char *prefix = "Status: ";
    for (int i = 0; prefix[i]; i++) greeting[pos++] = (senc_u8)prefix[i];
    senc_encode(SENC_CP_CHECK, &greeting[pos]); pos += 2;
    const char *suffix = " OK";
    for (int i = 0; suffix[i]; i++) greeting[pos++] = (senc_u8)suffix[i];
    greeting[pos] = '\0';
    /* "Status: " = 8 cp, CHECK = 1 cp, " OK" = 3 cp → 12 codepoints total */
    int cplen = senc_strlen(greeting);
    EXPECT_EQ("SENC_LIT-style string: codepoint count", cplen, 12);
}

/* 9. SYSTRIX_LOGO removed — 0x92 must be blank/stub */
static void test_logo_removed(void) {
    printf("\r\n[9] SENC_CP_SYSTRIX_LOGO removed — 0x92 stub\r\n");

    /* 0x92 should still encode as a valid 2-byte codepoint (it's in range) */
    senc_u8 buf[4];
    int n = senc_encode(0x92u, buf);
    EXPECT_EQ("0x92 still encodes (2 bytes, in range)", n, 2);

    /* But there must be no #define for it — validated at compile time:
     * the header comment says "0x92 was SENC_CP_SYSTRIX_LOGO (removed)".
     * We can only verify the value is NOT equal to any named codepoint. */
    int clash = 0;
    senc_cp all_named[] = {
        SENC_CP_BLOCK, SENC_CP_DRIVE,  SENC_CP_NET,    SENC_CP_FOLDER,
        SENC_CP_TERM,  SENC_CP_LOCK,   SENC_CP_CPU,    SENC_CP_MEM,
        SENC_CP_ARROW_UP, SENC_CP_ARROW_DOWN, SENC_CP_ARROW_LEFT, SENC_CP_ARROW_RIGHT,
        SENC_CP_CHECK, SENC_CP_CROSS,  SENC_CP_STAR,   SENC_CP_HEART,
        SENC_CP_WIFI,  SENC_CP_BATTERY,
        SENC_CP_SNAKE, SENC_CP_PACMAN, SENC_CP_BELL,   SENC_CP_MUSIC,
        SENC_CP_SUN,   SENC_CP_MOON,   SENC_CP_TRASH,
        SENC_CP_UPPER_A, SENC_CP_UPPER_Z,
        SENC_CP_LOWER_A, SENC_CP_LOWER_Z,
        SENC_CP_DIGIT_0, SENC_CP_DIGIT_9
    };
    for (int i = 0; i < (int)(sizeof(all_named)/sizeof(all_named[0])); i++) {
        if (all_named[i] == 0x92u) { clash = 1; break; }
    }
    if (!clash) test_ok("no named codepoint equals 0x92 (logo successfully unregistered)");
    else        test_fail("logo removal", "0x92 still aliased to a named codepoint!");
}

/* 10. Byte dump — visual confirmation */
static void test_byte_dump(void) {
    printf("\r\n[10] Byte dump — visual check\r\n");
    senc_u8 buf[4];

    printf("  'A'   (ASCII 0x41)     → "); senc_encode('A', buf); print_hex(buf,1); printf("\r\n");
    printf("  'z'   (ASCII 0x7A)     → "); senc_encode('z', buf); print_hex(buf,1); printf("\r\n");
    printf("  UPPER_A (SENC 0x9A)    → "); senc_encode(SENC_CP_UPPER_A, buf); print_hex(buf,2); printf("\r\n");
    printf("  LOWER_Z (SENC 0xCD)    → "); senc_encode(SENC_CP_LOWER_Z, buf); print_hex(buf,2); printf("\r\n");
    printf("  DIGIT_9 (SENC 0xD7)    → "); senc_encode(SENC_CP_DIGIT_9, buf); print_hex(buf,2); printf("\r\n");
    printf("  CHECK   (SENC 0x8C)    → "); senc_encode(SENC_CP_CHECK,   buf); print_hex(buf,2); printf("\r\n");
    printf("  HEART   (SENC 0x8F)    → "); senc_encode(SENC_CP_HEART,   buf); print_hex(buf,2); printf("\r\n");
    printf("  BLOCK   (SENC 0x80)    → "); senc_encode(SENC_CP_BLOCK,   buf); print_hex(buf,2); printf("\r\n");
    test_ok("byte dump complete");
}

/* ================================================================
 *  ENTRY POINT
 * ================================================================ */
int main(void) {
    printf("================================================\r\n");
    printf("  SENC Test Suite — SystrixOS\r\n");
    printf("================================================\r\n");

    test_encode_ascii();
    test_encode_custom();
    test_decode();
    test_senc_alpha();
    test_strlen();
    test_senc_next();
    test_alpha_range();
    test_senc_lit();
    test_logo_removed();
    test_byte_dump();

    printf("\r\n================================================\r\n");
    printf("  Results: %d passed, %d failed\r\n", g_pass, g_fail);
    printf("================================================\r\n");

    return g_fail ? 1 : 0;
}
