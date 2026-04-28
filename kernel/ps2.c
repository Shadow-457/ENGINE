/* ================================================================
 *  Systrix OS — kernel/ps2.c
 *  Full PS/2 i8042 keyboard + mouse driver.
 *
 *  Keyboard:
 *    – Dual-channel i8042 controller init (self-test, port tests)
 *    – Scancode Set 1 decode with full shift/caps/ctrl/alt tracking
 *    – Extended (0xE0) prefix handling for arrow keys, numpad, etc.
 *    – Key repeat via held-key state (feeds input.c ring)
 *    – Typematic rate/delay programming
 *
 *  Mouse:
 *    – Auxiliary port enable + full reset handshake
 *    – 3-byte packet decode with sign-extension and overflow guard
 *    – Scroll-wheel detection (4-byte IntelliMouse protocol)
 *    – 400 DPI sample rate for smoother tracking
 *    – GUI cursor + input.c ring integration
 *
 *  Polling model: ps2_poll() is called from the kernel main loop.
 *  No IRQ dependency — reads port 0x64 status to decide what to drain.
 * ================================================================ */
#include "../include/kernel.h"

/* ── i8042 ports ────────────────────────────────────────────── */
#define PS2_DATA    0x60
#define PS2_STATUS  0x64   /* read */
#define PS2_CMD     0x64   /* write */

/* Status register bits */
#define PS2_SR_OBF  (1u<<0)   /* output buffer full  (data to read) */
#define PS2_SR_IBF  (1u<<1)   /* input  buffer full  (busy writing) */
#define PS2_SR_AUX  (1u<<5)   /* OBF came from aux (mouse) port     */

/* Controller commands */
#define PS2_CMD_READ_CFG    0x20
#define PS2_CMD_WRITE_CFG   0x60
#define PS2_CMD_DISABLE_AUX 0xA7
#define PS2_CMD_ENABLE_AUX  0xA8
#define PS2_CMD_TEST_AUX    0xA9
#define PS2_CMD_SELF_TEST   0xAA
#define PS2_CMD_TEST_PORT1  0xAB
#define PS2_CMD_DISABLE_KB  0xAD
#define PS2_CMD_ENABLE_KB   0xAE
#define PS2_CMD_WRITE_AUX   0xD4

/* Config byte bits */
#define PS2_CFG_IRQ1    (1u<<0)
#define PS2_CFG_IRQ12   (1u<<1)
#define PS2_CFG_SYSFLAG (1u<<2)
#define PS2_CFG_DIS_KB  (1u<<4)
#define PS2_CFG_DIS_AUX (1u<<5)
#define PS2_CFG_XLAT    (1u<<6)   /* scancode translation */

/* Keyboard commands */
#define KB_CMD_SET_LEDS     0xED
#define KB_CMD_ECHO         0xEE
#define KB_CMD_SET_SCANCODE 0xF0
#define KB_CMD_IDENTIFY     0xF2
#define KB_CMD_TYPEMATIC    0xF3
#define KB_CMD_ENABLE       0xF4
#define KB_CMD_DISABLE      0xF5
#define KB_CMD_RESET        0xFF
#define KB_ACK              0xFA
#define KB_RESEND           0xFE
#define KB_SELF_TEST_OK     0xAA

/* Mouse commands */
#define MOUSE_CMD_RESET         0xFF
#define MOUSE_CMD_DEFAULTS      0xF6
#define MOUSE_CMD_ENABLE        0xF4
#define MOUSE_CMD_SAMPLE_RATE   0xF3
#define MOUSE_CMD_SET_RES       0xE8
#define MOUSE_CMD_GET_DEVID     0xF2
#define MOUSE_ACK               0xFA

/* ── Scroll-wheel (IntelliMouse) magic sequence ─────────────── */
/* Send sample rates 200,100,80 then read device ID; if 0x03 → scroll wheel */
#define IMOUSE_ID  0x03

/* ── Scancode Set 1 → ASCII tables ─────────────────────────── */
/* Normal (unshifted) */
static const u8 sc1_ascii[128] = {
/*00*/  0,    0x1B, '1',  '2',  '3',  '4',  '5',  '6',
/*08*/  '7',  '8',  '9',  '0',  '-',  '=',  '\b', '\t',
/*10*/  'q',  'w',  'e',  'r',  't',  'y',  'u',  'i',
/*18*/  'o',  'p',  '[',  ']',  '\r', 0,    'a',  's',
/*20*/  'd',  'f',  'g',  'h',  'j',  'k',  'l',  ';',
/*28*/  '\'', '`',  0,    '\\', 'z',  'x',  'c',  'v',
/*30*/  'b',  'n',  'm',  ',',  '.',  '/',  0,    '*',
/*38*/  0,    ' ',  0,    0,    0,    0,    0,    0,
/*40*/  0,    0,    0,    0,    0,    0,    0,    '7',
/*48*/  '8',  '9',  '-',  '4',  '5',  '6',  '+',  '1',
/*50*/  '2',  '3',  '0',  '.',  0,    0,    0,    0,
/*58*/  0,
};

/* Shifted */
static const u8 sc1_shift[128] = {
/*00*/  0,    0x1B, '!',  '@',  '#',  '$',  '%',  '^',
/*08*/  '&',  '*',  '(',  ')',  '_',  '+',  '\b', '\t',
/*10*/  'Q',  'W',  'E',  'R',  'T',  'Y',  'U',  'I',
/*18*/  'O',  'P',  '{',  '}',  '\r', 0,    'A',  'S',
/*20*/  'D',  'F',  'G',  'H',  'J',  'K',  'L',  ':',
/*28*/  '"',  '~',  0,    '|',  'Z',  'X',  'C',  'V',
/*30*/  'B',  'N',  'M',  '<',  '>',  '?',  0,    '*',
/*38*/  0,    ' ',  0,    0,    0,    0,    0,    0,
};

/* ── Driver state ────────────────────────────────────────────── */
static int ps2_kb_present   = 0;
static int ps2_mouse_present = 0;
static int ps2_scroll_wheel  = 0;   /* IntelliMouse scroll-wheel present */

/* Keyboard state */
static u8  kb_shift = 0;
static u8  kb_ctrl  = 0;
static u8  kb_alt   = 0;
static u8  kb_caps  = 0;
static u8  kb_num   = 0;
static u8  kb_e0    = 0;   /* last byte was 0xE0 prefix */
static u8  kb_leds  = 0;   /* current LED state */

/* Mouse state */
static int  mouse_cycle = 0;
static u8   mouse_pkt[4];
static int  mouse_pkt_len = 3;   /* 4 if scroll wheel present */

/* VGA cursor tracking (for text-mode pointer) */
static int  ps2_mouse_col     = 40;
static int  ps2_mouse_row     = 12;
static u16  ps2_mouse_saved   = 0;
static int  ps2_mouse_visible = 0;
static int  ps2_mouse_ready   = 0;

/* ── Low-level i8042 helpers ─────────────────────────────────── */
static void ps2_flush(void) {
    /* Drain any stale bytes in output buffer */
    for (int i = 0; i < 16; i++) {
        if (!(inb(PS2_STATUS) & PS2_SR_OBF)) break;
        inb(PS2_DATA);
        for (volatile int d = 0; d < 1000; d++);
    }
}

static void ps2_wait_write(void) {
    u32 t = 100000;
    while ((inb(PS2_STATUS) & PS2_SR_IBF) && --t)
        for (volatile int d = 0; d < 10; d++);
}

static void ps2_wait_read(void) {
    u32 t = 100000;
    while (!(inb(PS2_STATUS) & PS2_SR_OBF) && --t)
        for (volatile int d = 0; d < 10; d++);
}

static void ps2_wait_aux_read(void) {
    u32 t = 100000;
    while (t--) {
        u8 s = inb(PS2_STATUS);
        if ((s & PS2_SR_OBF) && (s & PS2_SR_AUX)) return;
        for (volatile int d = 0; d < 10; d++);
    }
}

static u8 ps2_read_data(void) {
    ps2_wait_read();
    return inb(PS2_DATA);
}

static void ps2_write_cmd(u8 cmd) {
    ps2_wait_write();
    outb(PS2_CMD, cmd);
}

static void ps2_write_data(u8 data) {
    ps2_wait_write();
    outb(PS2_DATA, data);
}

static void ps2_write_aux(u8 data) {
    ps2_write_cmd(PS2_CMD_WRITE_AUX);
    ps2_write_data(data);
}

/* Send command to keyboard, retry on RESEND (up to 3 times) */
static u8 kb_send(u8 cmd) {
    for (int try = 0; try < 3; try++) {
        ps2_flush();
        ps2_write_data(cmd);
        u8 r = ps2_read_data();
        if (r == KB_ACK) return KB_ACK;
        if (r != KB_RESEND) return r;
    }
    return 0;
}

/* Send command + byte to keyboard */
static u8 kb_send2(u8 cmd, u8 arg) {
    if (kb_send(cmd) != KB_ACK) return 0;
    return kb_send(arg);
}

/* Send command to mouse, return ACK */
static u8 mouse_send(u8 cmd) {
    for (int try = 0; try < 3; try++) {
        ps2_write_aux(cmd);
        ps2_wait_aux_read();
        u8 r = inb(PS2_DATA);
        if (r == MOUSE_ACK) return MOUSE_ACK;
    }
    return 0;
}

static u8 mouse_send2(u8 cmd, u8 arg) {
    if (mouse_send(cmd) != MOUSE_ACK) return 0;
    return mouse_send(arg);
}

/* ── LED update ──────────────────────────────────────────────── */
static void kb_update_leds(void) {
    kb_send2(KB_CMD_SET_LEDS, kb_leds);
}

/* ── VGA text-mode mouse cursor ──────────────────────────────── */
static void ps2_mouse_cursor_draw(void) {
    if (!ps2_mouse_visible) return;
    u16 *fb  = (u16*)VGA_BASE;
    int  idx = ps2_mouse_row * VGA_COLS + ps2_mouse_col;
    u16  cell = fb[idx];
    u8   attr = (u8)(cell >> 8);
    u8   inv  = (u8)(((attr & 0x0F) << 4) | ((attr >> 4) & 0x0F));
    fb[idx]   = (u16)((u16)inv << 8) | (cell & 0xFF);
}

static void ps2_mouse_cursor_erase(void) {
    if (!ps2_mouse_visible) return;
    u16 *fb  = (u16*)VGA_BASE;
    int  idx = ps2_mouse_row * VGA_COLS + ps2_mouse_col;
    fb[idx]  = ps2_mouse_saved;
}

static void ps2_mouse_cursor_show(void) {
    u16 *fb  = (u16*)VGA_BASE;
    int  idx = ps2_mouse_row * VGA_COLS + ps2_mouse_col;
    ps2_mouse_saved   = fb[idx];
    ps2_mouse_visible = 1;
    ps2_mouse_cursor_draw();
}

static void ps2_mouse_cursor_move(int dr, int dc) {
    ps2_mouse_cursor_erase();
    ps2_mouse_row += dr;
    ps2_mouse_col += dc;
    if (ps2_mouse_row < 0)         ps2_mouse_row = 0;
    if (ps2_mouse_row >= VGA_ROWS) ps2_mouse_row = VGA_ROWS - 1;
    if (ps2_mouse_col < 0)         ps2_mouse_col = 0;
    if (ps2_mouse_col >= VGA_COLS) ps2_mouse_col = VGA_COLS - 1;
    u16 *fb  = (u16*)VGA_BASE;
    ps2_mouse_saved = fb[ps2_mouse_row * VGA_COLS + ps2_mouse_col];
    ps2_mouse_cursor_draw();
}

/* ── Mouse packet decode ─────────────────────────────────────── */
static void ps2_decode_mouse_packet(void) {
    u8 flags = mouse_pkt[0];

    /* Byte 0 bit 3 must be set — resync if not */
    if (!(flags & 0x08)) { mouse_cycle = 0; return; }

    /* Overflow bits — discard */
    if (flags & 0xC0) return;

    /* Sign-extend 9-bit deltas */
    int dx = (int)mouse_pkt[1] - ((flags & 0x10) ? 256 : 0);
    int dy = (int)mouse_pkt[2] - ((flags & 0x20) ? 256 : 0);
    int dz = 0;
    if (ps2_scroll_wheel) {
        /* byte 3: lower 4 bits are scroll, sign-extended */
        int raw = (int)(mouse_pkt[3] & 0x0F);
        if (raw & 0x08) raw |= ~0x0F;   /* sign extend */
        dz = raw;
    }

    /* Text-mode cursor: scale px → cells */
    int dc =  dx / 8;
    int dr = -dy / 8;
    if (dc || dr) ps2_mouse_cursor_move(dr, dc);

    /* Feed input subsystem */
    u8 btns = 0;
    if (flags & 0x01) btns |= INPUT_BTN_LEFT;
    if (flags & 0x02) btns |= INPUT_BTN_RIGHT;
    if (flags & 0x04) btns |= INPUT_BTN_MIDDLE;
    (void)dz;  /* scroll wheel available for future use */
    input_push_mouse(dx, dy, btns);
}

/* ── Keyboard scancode decode ────────────────────────────────── */
static void ps2_decode_key(u8 sc) {
    /* ── Modifier / special keys ── */
    int release = (sc & 0x80) ? 1 : 0;
    u8  key     = sc & 0x7F;

    if (kb_e0) {
        /* Extended keycode */
        kb_e0 = 0;
        if (!release) {
            u8 ascii = 0;
            if (key == 0x48) ascii = 0x80;  /* up    */
            if (key == 0x50) ascii = 0x81;  /* down  */
            if (key == 0x4B) ascii = 0x82;  /* left  */
            if (key == 0x4D) ascii = 0x83;  /* right */
            if (key == 0x1C) ascii = '\r';  /* numpad enter */
            if (ascii) input_push_key(key | 0x80, ascii);
        } else {
            input_key_release();
        }
        return;
    }

    if (sc == 0xE0) { kb_e0 = 1; return; }

    /* Modifier tracking */
    switch (key) {
    case 0x2A: case 0x36:  /* L/R shift */
        kb_shift = release ? 0 : 1;
        input_mod_update(kb_shift, kb_ctrl, kb_caps);
        return;
    case 0x1D:             /* L ctrl (R ctrl via E0) */
        kb_ctrl = release ? 0 : 1;
        input_mod_update(kb_shift, kb_ctrl, kb_caps);
        return;
    case 0x38:             /* L alt */
        kb_alt = release ? 0 : 1;
        return;
    case 0x3A:             /* Caps Lock — toggle on press */
        if (!release) {
            kb_caps ^= 1;
            kb_leds  = (kb_leds & ~0x04) | (kb_caps ? 0x04 : 0);
            kb_update_leds();
            input_mod_update(kb_shift, kb_ctrl, kb_caps);
        }
        return;
    case 0x45:             /* Num Lock */
        if (!release) {
            kb_num ^= 1;
            kb_leds = (kb_leds & ~0x02) | (kb_num ? 0x02 : 0);
            kb_update_leds();
        }
        return;
    default: break;
    }

    if (release) {
        input_key_release();
        return;
    }

    /* Key press — look up ASCII */
    u8 ascii = 0;
    if (key < 128) {
        int use_shift = kb_shift;
        /* Caps Lock only flips alpha keys */
        if (kb_caps && key >= 0x10 && key <= 0x32)
            use_shift ^= 1;
        ascii = use_shift ? sc1_shift[key] : sc1_ascii[key];
        /* Ctrl sequences */
        if (kb_ctrl && islower((unsigned char)ascii)) ascii = (u8)(ascii - 96);
        if (kb_ctrl && isupper((unsigned char)ascii)) ascii = (u8)(ascii - 64);
    }
    if (ascii) input_push_key(key, ascii);
}

/* ── IntelliMouse scroll-wheel probe ────────────────────────── */
static int ps2_probe_scroll_wheel(void) {
    /* Magic sequence: set sample rate 200, 100, 80, then read device ID */
    mouse_send2(MOUSE_CMD_SAMPLE_RATE, 200);
    mouse_send2(MOUSE_CMD_SAMPLE_RATE, 100);
    mouse_send2(MOUSE_CMD_SAMPLE_RATE,  80);
    /* Read device ID */
    ps2_write_aux(MOUSE_CMD_GET_DEVID);
    ps2_wait_aux_read(); /* ACK */
    inb(PS2_DATA);
    ps2_wait_aux_read(); /* device ID */
    u8 id = inb(PS2_DATA);
    return (id == IMOUSE_ID) ? 1 : 0;
}

/* ── Mouse init ──────────────────────────────────────────────── */
static int ps2_mouse_init(void) {
    /* Enable auxiliary port */
    ps2_write_cmd(PS2_CMD_ENABLE_AUX);
    for (volatile int d = 0; d < 10000; d++);

    /* Reset mouse */
    ps2_write_aux(MOUSE_CMD_RESET);
    ps2_wait_aux_read();
    u8 ack = inb(PS2_DATA);
    if (ack != MOUSE_ACK) return 0;
    /* Read self-test result (0xAA) and device ID (0x00) */
    ps2_wait_aux_read(); inb(PS2_DATA);
    ps2_wait_aux_read(); inb(PS2_DATA);

    /* Probe for IntelliMouse scroll wheel */
    ps2_scroll_wheel = ps2_probe_scroll_wheel();
    mouse_pkt_len    = ps2_scroll_wheel ? 4 : 3;

    /* Set sample rate 100 (default is fine; 200 uses more CPU) */
    mouse_send2(MOUSE_CMD_SAMPLE_RATE, 100);

    /* Set resolution: 4 counts/mm (index 3) */
    mouse_send2(MOUSE_CMD_SET_RES, 3);

    /* Defaults + enable reporting */
    mouse_send(MOUSE_CMD_DEFAULTS);
    mouse_send(MOUSE_CMD_ENABLE);

    return 1;
}

/* ── Keyboard init ───────────────────────────────────────────── */
static int ps2_kb_init(void) {
    /* Reset keyboard */
    ps2_write_data(KB_CMD_RESET);
    u8 r = ps2_read_data();
    if (r != KB_ACK && r != KB_SELF_TEST_OK) {
        /* Try once more — some firmware sends ACK then self-test */
        r = ps2_read_data();
    }
    if (r != KB_ACK && r != KB_SELF_TEST_OK) return 0;
    /* Drain self-test passed byte if ACK came first */
    if (r == KB_ACK) {
        ps2_wait_read();
        inb(PS2_DATA); /* 0xAA */
    }

    /* Use scancode set 1 (most compatible) */
    kb_send2(KB_CMD_SET_SCANCODE, 1);

    /* Set typematic: 30ms delay (0b00), 30 cps rate (0b00000) */
    kb_send2(KB_CMD_TYPEMATIC, 0x00);

    /* Enable keyboard */
    kb_send(KB_CMD_ENABLE);

    /* Sync LEDs */
    kb_leds = 0;
    kb_update_leds();

    return 1;
}

/* ── Controller initialisation ───────────────────────────────── */
void ps2_init(void) {
    /* Disable both ports during init */
    ps2_write_cmd(PS2_CMD_DISABLE_KB);
    ps2_write_cmd(PS2_CMD_DISABLE_AUX);
    ps2_flush();

    /* Read and modify configuration byte:
     * - Enable IRQs (we poll, but enabling them wakes up the FW)
     * - Disable translation so we get raw Set 1 scancodes
     * - Enable both clocks */
    ps2_write_cmd(PS2_CMD_READ_CFG);
    u8 cfg = ps2_read_data();
    cfg |=  (PS2_CFG_IRQ1 | PS2_CFG_IRQ12 | PS2_CFG_SYSFLAG);
    cfg &= ~(PS2_CFG_DIS_KB | PS2_CFG_DIS_AUX | PS2_CFG_XLAT);
    ps2_write_cmd(PS2_CMD_WRITE_CFG);
    ps2_write_data(cfg);

    /* Controller self-test */
    ps2_write_cmd(PS2_CMD_SELF_TEST);
    u8 st = ps2_read_data();
    if (st != 0x55) {
        kprintf("[PS2] controller self-test FAIL\r\n");
        /* Carry on anyway — some emulators skip this */
    }

    /* Test port 1 (keyboard) */
    ps2_write_cmd(PS2_CMD_TEST_PORT1);
    u8 pt1 = ps2_read_data();

    /* Re-enable keyboard port */
    ps2_write_cmd(PS2_CMD_ENABLE_KB);

    if (pt1 == 0x00) {
        ps2_kb_present = ps2_kb_init();
    }

    /* Test aux port (mouse) */
    ps2_write_cmd(PS2_CMD_TEST_AUX);
    u8 pt2 = ps2_read_data();

    if (pt2 == 0x00) {
        ps2_mouse_present = ps2_mouse_init();
    }

    if (ps2_mouse_present) {
        /* Draw initial cursor */
        ps2_mouse_cursor_show();
        ps2_mouse_ready = 1;
    }

    kprintf("[PS2] KB=");
    kprintf("%s", ps2_kb_present   ? "OK" : "NONE");
    kprintf(" MOUSE=");
    kprintf("%s", ps2_mouse_present ? "OK" : "NONE");
    if (ps2_scroll_wheel) kprintf("+SCROLL");
    kprintf("\r\n");
}

/* ── Poll loop (call from kernel main event loop) ────────────── */
void ps2_poll(void) {
    /* Drain all available bytes from the i8042 output buffer */
    for (int i = 0; i < 32; i++) {
        u8 status = inb(PS2_STATUS);
        if (!(status & PS2_SR_OBF)) break;

        u8 byte = inb(PS2_DATA);

        if (status & PS2_SR_AUX) {
            /* Mouse byte */
            if (!ps2_mouse_present) continue;
            mouse_pkt[mouse_cycle++] = byte;
            if (mouse_cycle >= mouse_pkt_len) {
                mouse_cycle = 0;
                ps2_decode_mouse_packet();
            }
        } else {
            /* Keyboard byte */
            if (!ps2_kb_present) continue;
            ps2_decode_key(byte);
        }
    }
}

/* ── Public accessors ────────────────────────────────────────── */
int  ps2_kb_ok(void)    { return ps2_kb_present;    }
int  ps2_mouse_ok(void) { return ps2_mouse_present;  }
int  ps2_mouse_x(void)  { return ps2_mouse_col;      }
int  ps2_mouse_y(void)  { return ps2_mouse_row;      }

/* Refresh cursor visibility after VGA redraws wipe the screen */
void ps2_mouse_refresh(void) {
    if (ps2_mouse_ready) ps2_mouse_cursor_show();
}

/* Hide/show cursor (for GUI transitions) */
void ps2_mouse_hide(void) { ps2_mouse_cursor_erase(); ps2_mouse_visible = 0; }
void ps2_mouse_show_cursor(void) { ps2_mouse_cursor_show(); }
