/* ================================================================
 *  Systrix OS — kernel/input.c
 *  Phase 1: Input & Controls
 *
 *  Provides three ring-buffer-based input subsystems exposed via
 *  syscalls:
 *    sys_poll_keys  (300) — keyboard: raw scancodes + ASCII,
 *                           held-key repeat driven by PIT ticks
 *    sys_poll_mouse (301) — PS/2 mouse: relative dx/dy + buttons
 *    sys_poll_pad   (302) — gamepad/joystick: axis + button state
 *                           (PS/2 joystick or USB HID stub)
 *
 *  The GUI event loop and read_key_raw() call:
 *    input_push_key(scancode, ascii)  — on every keyboard make-code
 *    input_push_mouse(dx, dy, flags)  — on every completed PS/2 packet
 *
 *  User programs call sys_poll_* to drain the ring buffers without
 *  blocking.  Returns the number of events copied, or 0 if empty.
 * ================================================================ */

#include "../include/kernel.h"

/* ── Ring buffer geometry ──────────────────────────────────────── */
#define KEY_BUF_SIZE   64   /* must be power-of-two */
#define MOUSE_BUF_SIZE 32
#define PAD_BUF_SIZE   16

/* ── Ring buffer helpers ───────────────────────────────────────── */
static KeyEvent   key_ring[KEY_BUF_SIZE];
static u32        key_head = 0, key_tail = 0;   /* head = read, tail = write */

static MouseEvent mou_ring[MOUSE_BUF_SIZE];
static u32        mou_head = 0, mou_tail = 0;

static PadState   pad_state;   /* single shared state snapshot */

/* held-key repeat */
#define REPEAT_DELAY_TICKS  500  /* 500ms initial delay  (1ms ticks) */
#define REPEAT_RATE_TICKS    60  /* 60ms between repeats (1ms ticks) */
static u8  held_scancode = 0;
static u8  held_ascii    = 0;
static u8  held_mods     = 0;
static u64 held_since    = 0;   /* pit_ticks when key was pressed */
static u64 held_last_rep = 0;   /* pit_ticks of last repeat event */

/* Current modifier state (mirrored from kernel.c) */
static u8 inp_shift = 0;
static u8 inp_ctrl  = 0;
static u8 inp_caps  = 0;

/* ── Public: push a key event (called by GUI loop / read_key_raw) */
void input_push_key(u8 scancode, u8 ascii) {
    u8 mods = 0;
    if (inp_shift) mods |= INPUT_MOD_SHIFT;
    if (inp_ctrl)  mods |= INPUT_MOD_CTRL;
    if (inp_caps)  mods |= INPUT_MOD_CAPS;

    u32 next = (key_tail + 1) & (KEY_BUF_SIZE - 1);
    if (next == key_head) return;   /* buffer full — drop oldest would be wrong; just drop newest */

    key_ring[key_tail].scancode = scancode;
    key_ring[key_tail].ascii    = ascii;
    key_ring[key_tail].mods     = mods;
    key_tail = next;

    /* Track held key for repeat */
    held_scancode = scancode;
    held_ascii    = ascii;
    held_mods     = mods;
    held_since    = pit_ticks;
    held_last_rep = pit_ticks;
}

/* Call this when any key is released — stops repeat */
void input_key_release(void) {
    held_scancode = 0;
    held_ascii    = 0;
}

/* Update modifier shadow state (called from the raw input path) */
void input_mod_update(u8 shift, u8 ctrl, u8 caps) {
    inp_shift = shift;
    inp_ctrl  = ctrl;
    inp_caps  = caps;
}

/* ── Held-key repeat tick (call from PIT handler or poll loop) ── */
void input_key_repeat_tick(void) {
    if (!held_scancode) return;

    u64 now = pit_ticks;
    u64 elapsed = now - held_since;

    if (elapsed < REPEAT_DELAY_TICKS) return;

    if ((now - held_last_rep) >= REPEAT_RATE_TICKS) {
        held_last_rep = now;

        u32 next = (key_tail + 1) & (KEY_BUF_SIZE - 1);
        if (next == key_head) return;   /* full */

        key_ring[key_tail].scancode = held_scancode;
        key_ring[key_tail].ascii    = held_ascii;
        key_ring[key_tail].mods     = held_mods;
        key_tail = next;
    }
}

/* ── Public: push a mouse event (called by GUI loop / mouse_poll) */
void input_push_mouse(int dx, int dy, u8 buttons) {
    u32 next = (mou_tail + 1) & (MOUSE_BUF_SIZE - 1);
    if (next == mou_head) {
        /* buffer full — overwrite oldest (smooth cursor requires recency) */
        mou_head = (mou_head + 1) & (MOUSE_BUF_SIZE - 1);
        next = (mou_tail + 1) & (MOUSE_BUF_SIZE - 1);
    }
    mou_ring[mou_tail].dx      = (i64)dx;
    mou_ring[mou_tail].dy      = (i64)dy;
    mou_ring[mou_tail].buttons = buttons;
    mou_tail = next;
}

/* ── Public: update gamepad state ─────────────────────────────── */
void input_pad_update(int axis_x, int axis_y, u16 buttons, u8 connected) {
    pad_state.axis_x    = (i64)axis_x;
    pad_state.axis_y    = (i64)axis_y;
    pad_state.buttons   = buttons;
    pad_state.connected = connected;
}

/* ================================================================
 *  Syscall implementations
 * ================================================================ */

/*
 * sys_poll_keys(KeyEvent *buf, usize max_events) → i64 count
 *
 * Drains up to max_events key events from the ring into user buffer.
 * Returns number of events copied (0 if nothing pending).
 * Runs held-key repeat generation before draining.
 */
i64 sys_poll_keys(void *buf, usize max_events) {
    /* Generate any pending repeat events */
    input_key_repeat_tick();

    if (!buf || max_events == 0) return 0;

    KeyEvent *out = (KeyEvent *)buf;
    usize count = 0;

    while (count < max_events && key_head != key_tail) {
        out[count] = key_ring[key_head];
        key_head = (key_head + 1) & (KEY_BUF_SIZE - 1);
        count++;
    }
    return (i64)count;
}

/*
 * sys_poll_mouse(MouseEvent *buf, usize max_events) → i64 count
 *
 * Drains up to max_events mouse events from the ring into user buffer.
 * Returns number of events copied.
 */
i64 sys_poll_mouse(void *buf, usize max_events) {
    if (!buf || max_events == 0) return 0;

    MouseEvent *out = (MouseEvent *)buf;
    usize count = 0;

    while (count < max_events && mou_head != mou_tail) {
        out[count] = mou_ring[mou_head];
        mou_head = (mou_head + 1) & (MOUSE_BUF_SIZE - 1);
        count++;
    }
    return (i64)count;
}

/*
 * sys_poll_pad(PadState *buf) → i64
 *
 * Copies the latest gamepad/joystick state snapshot into *buf.
 * Returns 1 if a pad is connected, 0 otherwise.
 * (For a real USB HID pad, this would be wired to the HID parser;
 *  here we expose the stub state that input_pad_update() writes.)
 */
i64 sys_poll_pad(void *buf) {
    if (!buf) return 0;
    PadState *out = (PadState *)buf;
    *out = pad_state;
    return (i64)pad_state.connected;
}

/* ── Mouse grab / relative mode ──────────────────────────────── */
static int mouse_grabbed = 0;   /* 0=absolute, 1=relative/grabbed */

int input_is_grabbed(void) { return mouse_grabbed; }

void input_set_mouse_grab(int grabbed) {
    mouse_grabbed = grabbed ? 1 : 0;
    if (!grabbed) {
        /* Clear accumulated deltas when releasing grab */
        mou_head = mou_tail = 0;
    }
}

/* Override input_push_mouse to suppress GUI cursor updates in
 * grabbed mode.  The GUI checks input_is_grabbed() before moving
 * its cursor.  Raw deltas still go into the ring so the game
 * can read them via sys_poll_mouse.                              */
void input_push_mouse_ex(int dx, int dy, u8 buttons, int from_gui) {
    /* In grabbed mode, discard GUI-path calls; accept only raw PS/2 */
    if (mouse_grabbed && from_gui) return;
    input_push_mouse(dx, dy, buttons);
}
