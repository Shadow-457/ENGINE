/* ================================================================
 *  SHADOW OS — kernel/kernel.c
 *  VGA terminal, ATA PIO driver, FAT32 filesystem, VFS layer,
 *  keyboard driver, interactive shell, and kernel_main().
 * ================================================================ */
#include "../include/kernel.h"

/* ================================================================
 *  VGA TEXT MODE (80×25, 0xB8000)
 *  Scroll back-buffer: SCROLLBACK_ROWS lines of history.
 *  Shift+PageUp / Shift+PageDown scroll the view.
 *  New output always goes to the back-buffer; the screen is only
 *  refreshed from the back-buffer when scrolling, or automatically
 *  on every new character when the view is at the live bottom.
 * ================================================================ */

#define SCROLLBACK_ROWS  200          /* total rows kept in history  */
#define SCROLL_STEP        5          /* rows per PageUp/PageDown     */

/* Back-buffer: circular, indexed by (buf_head + row) % SCROLLBACK_ROWS */
static u16 back_buf[SCROLLBACK_ROWS][VGA_COLS];
static int buf_next  = 0;   /* next row to write in back_buf (absolute) */
static int buf_total = 0;   /* how many rows have ever been written      */

/* Current view: scroll_offset == 0 → showing live bottom of history.
 * scroll_offset > 0 → showing that many rows back from the bottom.    */
static int scroll_offset = 0;

static u8 cur_row = 0, cur_col = 0;

/* Set to 1 when vga_putchar wraps at column 80; cleared immediately
 * by the next call.  Lets an explicit '\n' after a col-wrap be
 * absorbed rather than advancing cur_row a second time. */
static int col_just_wrapped = 0;

/* Update VGA hardware cursor to match cur_row / cur_col */
static void vga_update_hw_cursor(void) {
    u16 pos = (u16)(cur_row * VGA_COLS + cur_col);
    outb(0x3D4, 0x0F); outb(0x3D5, (u8)(pos & 0xFF));
    outb(0x3D4, 0x0E); outb(0x3D5, (u8)((pos >> 8) & 0xFF));
}

/* ================================================================
 *  PS/2 MOUSE DRIVER — polled, text-mode cursor (inverted cell)
 * ================================================================ */
static int  mouse_col    = 40;   /* current mouse column (0..VGA_COLS-1) */
static int  mouse_row    = 12;   /* current mouse row    (0..VGA_ROWS-1) */
static u16  mouse_saved  = 0;    /* original VGA cell under cursor        */
static int  mouse_visible= 0;    /* is cursor drawn on screen?            */
static int  mouse_ready  = 0;    /* mouse initialised successfully?       */
static int  mouse_cycle  = 0;    /* which byte of the 3-byte packet       */
static u8   mouse_pkt[3];        /* accumulated packet bytes              */

/* Wait until i8042 input buffer empty (safe to write command/data) */
static void ps2_wait_write(void) {
    u32 t = 100000;
    while ((inb(0x64) & 2) && --t) {}
}
/* Wait until i8042 output buffer has data (safe to read 0x60) */
static void ps2_wait_read(void) {
    u32 t = 100000;
    while (!(inb(0x64) & 1) && --t) {}
}
static u8 ps2_read(void)       { ps2_wait_read();  return inb(0x60); }
static void ps2_cmd(u8 c)      { ps2_wait_write(); outb(0x64, c); }
static void ps2_data(u8 d)     { ps2_wait_write(); outb(0x60, d); }
static void ps2_mouse_write(u8 d) {
    ps2_cmd(0xD4);   /* next byte goes to mouse */
    ps2_data(d);
}

/* Draw / erase the mouse cursor by inverting the attribute byte of the
 * cell under the pointer.  We only touch VGA framebuffer directly —
 * the back-buffer is intentionally left untouched so scrollback is clean. */
static void mouse_draw(void) {
    if (!mouse_visible) return;
    u16 *fb  = (u16*)VGA_BASE;
    int  idx = mouse_row * VGA_COLS + mouse_col;
    u16  cell = fb[idx];
    u8   attr = (u8)(cell >> 8);
    /* swap fg / bg nibbles for a visible highlight */
    u8   inv  = (u8)(((attr & 0x0F) << 4) | ((attr >> 4) & 0x0F));
    fb[idx]   = (u16)((u16)inv << 8) | (cell & 0xFF);
}
static void mouse_erase(void) {
    if (!mouse_visible) return;
    u16 *fb  = (u16*)VGA_BASE;
    int  idx = mouse_row * VGA_COLS + mouse_col;
    fb[idx]  = mouse_saved;   /* restore original cell */
}
static void mouse_show(void) {
    u16 *fb  = (u16*)VGA_BASE;
    int  idx = mouse_row * VGA_COLS + mouse_col;
    mouse_saved   = fb[idx];
    mouse_visible = 1;
    mouse_draw();
}
static void mouse_move(int dr, int dc) {
    mouse_erase();
    mouse_row += dr;
    mouse_col += dc;
    if (mouse_row < 0) mouse_row = 0;
    if (mouse_row >= VGA_ROWS) mouse_row = VGA_ROWS - 1;
    if (mouse_col < 0) mouse_col = 0;
    if (mouse_col >= VGA_COLS) mouse_col = VGA_COLS - 1;
    /* re-save cell at new position then draw */
    u16 *fb  = (u16*)VGA_BASE;
    mouse_saved = fb[mouse_row * VGA_COLS + mouse_col];
    mouse_draw();
}

/* Called from keyboard poll loop whenever 0x64 bit 5 is set (mouse byte
 * waiting).  Accumulates the 3-byte packet; on completion moves cursor. */
static void mouse_poll(void) {
    u8 b = inb(0x60);
    mouse_pkt[mouse_cycle++] = b;
    if (mouse_cycle < 3) return;
    mouse_cycle = 0;

    u8  flags = mouse_pkt[0];
    /* bit 3 must always be set in byte 0 — resync if not */
    if (!(flags & 0x08)) { mouse_cycle = 0; return; }

    /* overflow bits — discard packet */
    if (flags & 0xC0) return;

    /* delta X/Y — sign-extended from 9-bit */
    int dx = (int)mouse_pkt[1] - ((flags & 0x10) ? 256 : 0);
    int dy = (int)mouse_pkt[2] - ((flags & 0x20) ? 256 : 0);

    /* Scale pixels → text cells (mouse gives ~1-8 px per move) */
    int dc =  dx / 8;
    int dr = -dy / 8;   /* Y axis inverted in VGA */

    if (dc || dr) mouse_move(dr, dc);

    /* Feed the input subsystem ring buffer */
    u8 btns = 0;
    if (flags & 0x01) btns |= INPUT_BTN_LEFT;
    if (flags & 0x02) btns |= INPUT_BTN_RIGHT;
    if (flags & 0x04) btns |= INPUT_BTN_MIDDLE;
    input_push_mouse(dx, dy, btns);
}

/* Initialise PS/2 mouse.  Returns 1 on success, 0 if no mouse found. */
static int mouse_init(void) {
    /* Enable auxiliary (mouse) port */
    ps2_cmd(0xA8);

    /* Enable mouse interrupts in the command byte
     * (we won't use IRQ12, but this wakes the mouse up) */
    ps2_cmd(0x20);
    u8 cb = ps2_read();
    cb |= 0x02;   /* enable IRQ12 bit so mouse sends data */
    cb &= ~0x20;  /* clear "disable mouse clock" bit      */
    ps2_cmd(0x60);
    ps2_data(cb);

    /* Reset mouse */
    ps2_mouse_write(0xFF);
    u8 ack = ps2_read();
    if (ack != 0xFA) return 0;   /* no ACK → no mouse */
    ps2_read();  /* 0xAA self-test passed */
    ps2_read();  /* 0x00 device ID        */

    /* Set default settings */
    ps2_mouse_write(0xF6); ps2_read();
    /* Enable data reporting */
    ps2_mouse_write(0xF4); ps2_read();

    return 1;
}

/* ================================================================
 *  END OF MOUSE DRIVER
 * ================================================================ */

/* Copy VGA_ROWS rows ending at (buf_total - 1 - scroll_offset) to VGA */
static void vga_redraw(void) {
    u16 *fb = (u16*)VGA_BASE;
    int top = buf_total - VGA_ROWS - scroll_offset;
    for (int r = 0; r < VGA_ROWS; r++) {
        int src = top + r;
        if (src < 0) {
            /* above history — blank row */
            for (int c = 0; c < VGA_COLS; c++)
                fb[r * VGA_COLS + c] = (u16)(VGA_ATTR << 8) | ' ';
        } else {
            int idx = src % SCROLLBACK_ROWS;
            for (int c = 0; c < VGA_COLS; c++)
                fb[r * VGA_COLS + c] = back_buf[idx][c];
        }
    }
}

void vga_scroll_up(void) {
    int max_scroll = buf_total - VGA_ROWS;
    if (max_scroll < 0) max_scroll = 0;
    scroll_offset += SCROLL_STEP;
    if (scroll_offset > max_scroll) scroll_offset = max_scroll;
    vga_redraw();
    if (mouse_ready) mouse_show();
}

void vga_scroll_down(void) {
    scroll_offset -= SCROLL_STEP;
    if (scroll_offset < 0) scroll_offset = 0;
    vga_redraw();
    if (mouse_ready) mouse_show();
}

void vga_clear(void) {
    /* Clear back-buffer */
    for (int r = 0; r < SCROLLBACK_ROWS; r++)
        for (int c = 0; c < VGA_COLS; c++)
            back_buf[r][c] = (u16)(VGA_ATTR << 8) | ' ';
    buf_next = 0; buf_total = 0; scroll_offset = 0;
    cur_row = cur_col = 0;
    /* Also blank VGA directly */
    u16 *fb = (u16*)VGA_BASE;
    for (int i = 0; i < VGA_COLS * VGA_ROWS; i++)
        fb[i] = (u16)(VGA_ATTR << 8) | ' ';
    /* Seed back_buf with VGA_ROWS blank rows so buf_total >= VGA_ROWS */
    for (int r = 0; r < VGA_ROWS; r++) {
        buf_next  = r % SCROLLBACK_ROWS;
        buf_total = r + 1;
    }
    buf_next = VGA_ROWS % SCROLLBACK_ROWS;
    buf_total = VGA_ROWS;
    cur_row = cur_col = 0;
    col_just_wrapped = 0;
    vga_update_hw_cursor();
}

/* ── sys_read_char (syscall 335) ────────────────────────────────
 * Block until one key is pressed; return its ASCII value (or a
 * private KEY_* code for arrow/special keys).  No echo.
 * This pumps read_key_raw() directly so user programs get keyboard
 * input even though the shell's read loop isn't running.          */
i64 sys_read_char(void) {
    __asm__ volatile("sti");
    u8 c = read_key_raw();
    __asm__ volatile("cli");
    return (i64)(unsigned char)c;
}

/* Scroll the live VGA framebuffer up one row and blank the bottom line.
 * Only called when scroll_offset==0 (live view) and the screen is full. */
static void vga_fb_scroll_one(void) {
    u16 *fb = (u16*)VGA_BASE;
    for (int i = 0; i < VGA_COLS * (VGA_ROWS - 1); i++)
        fb[i] = fb[i + VGA_COLS];
    for (int i = VGA_COLS * (VGA_ROWS - 1); i < VGA_COLS * VGA_ROWS; i++)
        fb[i] = (u16)(VGA_ATTR << 8) | ' ';
}

/* Advance the circular back-buffer by one row, clearing the new slot. */
static void backbuf_advance(void) {
    buf_next = (buf_next + 1) % SCROLLBACK_ROWS;
    buf_total++;
    for (int i = 0; i < VGA_COLS; i++)
        back_buf[buf_next][i] = (u16)(VGA_ATTR << 8) | ' ';
}

/* Track whether the last character caused a column wrap.  Set to 1
 * when writing the 80th character causes an implicit newline; cleared
 * by the next vga_putchar call.  An explicit '\n' that arrives right
 * after a col-wrap is a no-op row-advance (the wrap already advanced
 * the row), preventing the double-advance that caused draw_screen to
 * scroll all content off the top of the VGA buffer. */

/* Mutable VGA text attribute — lets md_cat (and any other code) switch
 * foreground/background colour without touching vga_putchar's logic.
 * VGA colour byte: bits[7:4]=bg, bits[3:0]=fg  (0=black, F=bright white)
 * Common fg values: 0F white, 0E yellow, 0B cyan, 0A green, 09 blue,
 *                   0D magenta, 07 grey, 08 dark-grey                   */
static u8 vga_cur_attr = VGA_ATTR;
static inline void vga_set_attr(u8 a) { vga_cur_attr = a; }
static inline void vga_reset_attr(void) { vga_cur_attr = VGA_ATTR; }

void vga_putchar(u8 c) {
    if (c == '\r') {
        cur_col = 0;
        col_just_wrapped = 0;
        vga_update_hw_cursor();
        return;
    }

    if (c == '\n') {
        if (col_just_wrapped) {
            /* The previous character already did the implicit newline
             * (col-wrap).  This explicit '\n' is redundant — absorb it
             * so we don't advance cur_row a second time. */
            col_just_wrapped = 0;
            return;
        }
        col_just_wrapped = 0;
        cur_col = 0;
        backbuf_advance();
        if (cur_row < VGA_ROWS - 1) {
            cur_row++;
        } else if (scroll_offset == 0) {
            vga_fb_scroll_one();   /* screen full — push VGA up one line */
        }
        vga_update_hw_cursor();
        return;
    }

    col_just_wrapped = 0;

    /* Normal character — write to back-buffer current row */
    back_buf[buf_next][cur_col] = (u16)(vga_cur_attr << 8) | c;

    /* Write to VGA only if live */
    if (scroll_offset == 0)
        ((u16*)VGA_BASE)[cur_row * VGA_COLS + cur_col] =
            (u16)(vga_cur_attr << 8) | c;

    if (++cur_col < VGA_COLS) { vga_update_hw_cursor(); return; }
    /* Wrap column → implicit newline */
    col_just_wrapped = 1;
    cur_col = 0;
    backbuf_advance();
    if (cur_row < VGA_ROWS - 1) {
        cur_row++;
    } else if (scroll_offset == 0) {
        vga_fb_scroll_one();
    }
    vga_update_hw_cursor();
}

void vga_backspace(void) {
    if (cur_col == 0) return;
    cur_col--;
    u16 blank = (u16)(VGA_ATTR << 8) | ' ';   /* backspace erases with default attr */
    back_buf[buf_next][cur_col] = blank;
    if (scroll_offset == 0)
        ((u16*)VGA_BASE)[cur_row * VGA_COLS + cur_col] = blank;
    vga_update_hw_cursor();
}

void print_str(const char *s) {
    while (*s) { vga_putchar((u8)*s++); watchdog_pet(); }
}

/* kprintf — formatted print directly to VGA via syslibc callback.
 * No temp buffer: each character goes straight to vga_putchar.    */
static void kprintf_putc(void *ctx, char c) {
    (void)ctx;
    vga_putchar((u8)c);
    watchdog_pet();
}
void kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void kprintf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    slibc_vprintf_cb(kprintf_putc, NULL, fmt, ap);
    va_end(ap);
}
static void kprintf_size_human(u32 sz) {
    if (sz < 1024)
        kprintf("%u B", (unsigned)sz);
    else if (sz < 1024 * 1024)
        kprintf("%u.%u KB", (unsigned)(sz/1024),
                (unsigned)((sz%1024)*10/1024));
    else
        kprintf("%u.%u MB", (unsigned)(sz/(1024*1024)),
                (unsigned)((sz%(1024*1024))*10/(1024*1024)));
}


void print_hex_byte(u8 v) {
    kprintf("%02X", (unsigned)v);
}

/* ================================================================
 *  ATA PIO (28-bit LBA, primary channel 0x1F0)
 * ================================================================ */
void ata_read_sector(u32 lba, void *buf) {
    /* FIX (Bug 7): bounded poll — if disk absent this returns zeroes */
    watchdog_suspend();
    u32 timeout = 0x100000;
    while ((inb(ATA_STATUS) & 0x80) && --timeout) {}
    if (!timeout) { u16 *p = (u16*)buf; for(int i=0;i<256;i++) p[i]=0; watchdog_resume(); return; }
    outb(ATA_DRIVE,  0xE0 | (u8)((lba >> 24) & 0x0F));
    outb(ATA_COUNT,  1);
    outb(ATA_LBA_LO, (u8)(lba));
    outb(ATA_LBA_MID,(u8)(lba >> 8));
    outb(ATA_LBA_HI, (u8)(lba >> 16));
    outb(ATA_CMD,    ATA_CMD_READ);
    timeout = 0x100000;
    while (!(inb(ATA_STATUS) & 0x08) && --timeout) {}
    if (!timeout) { u16 *p = (u16*)buf; for(int i=0;i<256;i++) p[i]=0; watchdog_resume(); return; }
    u16 *p = (u16*)buf;
    for (int i = 0; i < 256; i++) p[i] = inw(ATA_DATA);
    watchdog_resume();
}

void ata_write_sector(u32 lba, const void *buf) {
    u32 timeout = 0x100000;
    while ((inb(ATA_STATUS) & 0x80) && --timeout) {}
    if (!timeout) return;
    outb(ATA_DRIVE,  0xE0 | (u8)((lba >> 24) & 0x0F));
    outb(ATA_COUNT,  1);
    outb(ATA_LBA_LO, (u8)(lba));
    outb(ATA_LBA_MID,(u8)(lba >> 8));
    outb(ATA_LBA_HI, (u8)(lba >> 16));
    outb(ATA_CMD,    ATA_CMD_WRITE);
    timeout = 0x100000;
    while (!(inb(ATA_STATUS) & 0x08) && --timeout) {}
    if (!timeout) return;
    const u16 *p = (const u16*)buf;
    for (int i = 0; i < 256; i++) outw(ATA_DATA, p[i]);
    outb(ATA_CMD, ATA_CMD_FLUSH);
}

/* ================================================================
 *  FAT32 DRIVER
 * ================================================================ */
static u8  fat32_spc = 0;          /* sectors per cluster */
static u32 fat32_fat_start  = 0;
static u32 fat32_data_start = 0;
static u32 fat32_root_clus  = 0;
static u32 fat32_total_clus = 0;   /* total data clusters on volume */

static u8 sector_buf[512];
static u8 dir_buf[512];
static u8 fat_buf[512];
static u8 file_buf[512];

/* FAT sector cache: tracks which LBA is loaded in fat_buf to skip redundant reads */
static u32 fat_buf_lba = (u32)-1;

static void fat_read_cached(u32 lba) {
    if (lba == fat_buf_lba) return;
    ata_read_sector(lba, fat_buf);
    fat_buf_lba = lba;
}

static void fat_write_cached(u32 lba) {
    ata_write_sector(lba, fat_buf);
    fat_buf_lba = lba;
}

void fat32_init(void) {
    ata_read_sector(PART_START, sector_buf);
    fat32_spc        = sector_buf[13];
    u16 rsvd         = *(u16*)(sector_buf + 14);
    u8  num_fats     = sector_buf[16];
    u32 fat_size     = *(u32*)(sector_buf + 36);
    fat32_fat_start  = PART_START + rsvd;
    fat32_data_start = fat32_fat_start + num_fats * fat_size;
    fat32_root_clus  = *(u32*)(sector_buf + 44);
    /* Compute total data cluster count from BPB so fat32_alloc_cluster
     * can scan the full FAT instead of stopping at an arbitrary limit. */
    u32 total_sec32  = *(u32*)(sector_buf + 32);
    u16 total_sec16  = *(u16*)(sector_buf + 19);
    u32 total_sec    = total_sec32 ? total_sec32 : (u32)total_sec16;
    u32 data_secs    = total_sec - (u32)(fat32_data_start - PART_START);
    fat32_total_clus = (fat32_spc > 0) ? (data_secs / fat32_spc) : 0;
}

u32 cluster_to_lba(u32 clus) {
    return fat32_data_start + (clus - 2) * fat32_spc;
}

u32 fat32_next_cluster(u32 clus) {
    u32 byte_off = clus * 4;
    u32 sector   = byte_off / 512 + fat32_fat_start;
    u32 idx      = byte_off % 512;
    fat_read_cached(sector);
    return *(u32*)(fat_buf + idx) & 0x0FFFFFFF;
}

/* Find file by 8.3 name in dir_clus; returns start cluster via *out_clus, size via *out_size.
 * Returns 0 on success, -1 if not found. */
static i64 fat32_find_raw_in(const char *name83, u32 *out_clus, u32 *out_size, u32 dir_clus) {
    u32 clus = dir_clus;
    while (clus < 0x0FFFFFF8) {
        u32 lba = cluster_to_lba(clus);
        for (u8 s = 0; s < fat32_spc; s++) {
            ata_read_sector(lba + s, dir_buf);
            for (int e = 0; e < 16; e++) {
                u8 *de = dir_buf + e * 32;
                if (de[0] == 0) return -1;    /* end of dir */
                if (de[0] == 0xE5) continue;  /* deleted */
                if (de[11] & 0x08) continue;  /* volume label */
                if (strncmp((char*)de, name83, 11) == 0) {
                    if (out_clus) *out_clus = ((u32)de[20] << 16) | de[26] |
                                              ((u32)de[21] << 24) | ((u32)de[27] << 8);
                    if (out_size) *out_size = *(u32*)(de + 28);
                    return 0;
                }
            }
        }
        clus = fat32_next_cluster(clus);
    }
    return -1;
}

static i64 fat32_find_raw(const char *name83, u32 *out_clus, u32 *out_size) {
    return fat32_find_raw_in(name83, out_clus, out_size, fat32_root_clus);
}

i64 fat32_find_file_in(const char *name83, u32 *out_size, u32 dir_clus) {
    u32 clus;
    return fat32_find_raw_in(name83, &clus, out_size, dir_clus);
}

i64 fat32_find_file(const char *name83, u32 *out_size) {
    u32 clus;
    i64 rc = fat32_find_raw(name83, &clus, out_size);
    return rc; /* 0 or -1 */
}

u32 fat32_alloc_cluster(void) {
    /* Scan the full FAT (up to fat32_total_clus entries) instead of
     * stopping at an arbitrary limit of 10 000.  fat32_total_clus is
     * computed from the BPB in fat32_init() so this is always correct. */
    u32 limit = (fat32_total_clus > 0) ? (fat32_total_clus + 2) : 65536u;
    for (u32 clus = 2; clus < limit; clus++) {
        u32 byte_off = clus * 4;
        u32 sec = byte_off / 512 + fat32_fat_start;
        u32 idx = byte_off % 512;
        fat_read_cached(sec);
        if ((*(u32*)(fat_buf + idx) & 0x0FFFFFFF) == 0) {
            *(u32*)(fat_buf + idx) = 0x0FFFFFFF; /* mark EOC */
            fat_write_cached(sec);
            return clus;
        }
    }
    return (u32)-1;  /* disk full */
}

/* ── Directory entry helpers ────────────────────────────────── */

/* Walk dir_clus, find slot (empty or deleted), write 32-byte entry */
static void write_dir_entry_in(u8 *entry32, u32 dir_clus) {
    u32 clus = dir_clus;
    while (clus < 0x0FFFFFF8) {
        u32 lba = cluster_to_lba(clus);
        for (u8 s = 0; s < fat32_spc; s++) {
            ata_read_sector(lba + s, dir_buf);
            for (int e = 0; e < 16; e++) {
                u8 *de = dir_buf + e * 32;
                if (de[0] == 0 || de[0] == 0xE5) {
                    memcpy(de, entry32, 32);
                    ata_write_sector(lba + s, dir_buf);
                    return;
                }
            }
        }
        clus = fat32_next_cluster(clus);
    }
}

/* Read one RTC CMOS register (BCD -> binary) */
static u8 rtc_read_reg(u8 reg) {
    outb(0x70, reg);
    u8 v = inb(0x71);
    return (u8)(((v >> 4) * 10) + (v & 0xF));  /* BCD to binary */
}

/* Build FAT16 date word: bits [15:9]=year-1980, [8:5]=month, [4:0]=day */
static u16 fat_date_now(void) {
    u8 year  = rtc_read_reg(0x09);   /* 0-99, assume 2000+ */
    u8 month = rtc_read_reg(0x08);
    u8 day   = rtc_read_reg(0x07);
    if (month < 1)  month = 1;
    if (month > 12) month = 12;
    if (day   < 1)  day   = 1;
    if (day   > 31) day   = 31;
    u32 yr1980 = (u32)(2000 + year) - 1980;
    return (u16)((yr1980 << 9) | ((u32)month << 5) | day);
}

/* Build FAT16 time word: bits [15:11]=hour, [10:5]=min, [4:0]=sec/2 */
static u16 fat_time_now(void) {
    u8 hour = rtc_read_reg(0x04);
    u8 min  = rtc_read_reg(0x02);
    u8 sec  = rtc_read_reg(0x00);
    return (u16)(((u32)hour << 11) | ((u32)min << 5) | (sec >> 1));
}

void fat32_create_file_in(const char *name83, u32 dir_clus) {
    u8 de[32]; memset(de, 0, 32);
    memcpy(de, name83, 11);
    de[11] = 0x20; /* Archive */
    /* Stamp creation and modified date/time from RTC */
    u16 fdate = fat_date_now();
    u16 ftime = fat_time_now();
    /* Creation time: bytes 14-15, creation date: bytes 16-17 */
    de[14] = (u8)(ftime);
    de[15] = (u8)(ftime >> 8);
    de[16] = (u8)(fdate);
    de[17] = (u8)(fdate >> 8);
    /* Last modified time: bytes 22-23, last modified date: bytes 24-25 */
    de[22] = (u8)(ftime);
    de[23] = (u8)(ftime >> 8);
    de[24] = (u8)(fdate);
    de[25] = (u8)(fdate >> 8);
    write_dir_entry_in(de, dir_clus);
}
void fat32_create_file(const char *name83) {
    fat32_create_file_in(name83, fat32_root_clus);
}

void fat32_create_dir_entry_in(const char *name83, u32 cluster, u32 dir_clus) {
    u8 de[32]; memset(de, 0, 32);
    memcpy(de, name83, 11);
    de[11] = 0x10; /* Directory */
    de[26] = (u8)(cluster);
    de[27] = (u8)(cluster >> 8);
    de[20] = (u8)(cluster >> 16);
    de[21] = (u8)(cluster >> 24);
    write_dir_entry_in(de, dir_clus);
}
void fat32_create_dir_entry(const char *name83, u32 cluster) {
    fat32_create_dir_entry_in(name83, cluster, fat32_root_clus);
}

/* Walk dir, find entry by name; update size field */
static void patch_dir_size(FD *fd, u32 new_size) {
    u32 clus = (fd->dir_clus != 0) ? fd->dir_clus : fat32_root_clus;
    while (clus < 0x0FFFFFF8) {
        u32 lba = cluster_to_lba(clus);
        for (u8 s = 0; s < fat32_spc; s++) {
            ata_read_sector(lba + s, dir_buf);
            for (int e = 0; e < 16; e++) {
                u8 *de = dir_buf + e * 32;
                if (de[0] == 0)    goto size_next_clus;
                if (de[0] == 0xE5) continue;
                if (strncmp((char*)de, (char*)fd->name83, 11) == 0) {
                    *(u32*)(de + 28) = new_size;
                    ata_write_sector(lba + s, dir_buf);
                    return;
                }
            }
        }
size_next_clus:
        clus = fat32_next_cluster(clus);
    }
}

/* Walk dir, find entry by name with cluster==0; assign start_clus */
static void patch_dir_cluster(FD *fd, u32 start_clus) {
    u32 clus = (fd->dir_clus != 0) ? fd->dir_clus : fat32_root_clus;
    while (clus < 0x0FFFFFF8) {
        u32 lba = cluster_to_lba(clus);
        for (u8 s = 0; s < fat32_spc; s++) {
            ata_read_sector(lba + s, dir_buf);
            for (int e = 0; e < 16; e++) {
                u8 *de = dir_buf + e * 32;
                if (de[0] == 0)    goto clus_next_clus;
                if (de[0] == 0xE5) continue;
                if (strncmp((char*)de, (char*)fd->name83, 11) == 0) {
                    de[26] = (u8)(start_clus);
                    de[27] = (u8)(start_clus >> 8);
                    de[20] = (u8)(start_clus >> 16);
                    de[21] = (u8)(start_clus >> 24);
                    ata_write_sector(lba + s, dir_buf);
                    return;
                }
            }
        }
clus_next_clus:
        clus = fat32_next_cluster(clus);
    }
}

/* Link FAT: write 'next' into FAT entry for cluster 'cur' */
static void fat_link(u32 cur, u32 next) {
    u32 byte_off = cur * 4;
    u32 sec = byte_off / 512 + fat32_fat_start;
    u32 idx = byte_off % 512;
    fat_read_cached(sec);
    *(u32*)(fat_buf + idx) = next;
    fat_write_cached(sec);
}

i64 fat32_delete_file_in(const char *name83, u32 dir_clus) {
    u32 start_clus, fsize;
    u32 clus = dir_clus;
    while (clus < 0x0FFFFFF8) {
        u32 lba = cluster_to_lba(clus);
        for (u8 s = 0; s < fat32_spc; s++) {
            ata_read_sector(lba + s, dir_buf);
            for (int e = 0; e < 16; e++) {
                u8 *de = dir_buf + e * 32;
                if (de[0] == 0)    return -1;
                if (de[0] == 0xE5) continue;
                if (de[11] & 0x08) continue;
                if (strncmp((char*)de, name83, 11) == 0) {
                    start_clus = ((u32)de[20] << 16) | ((u32)de[21] << 24) |
                                 ((u32)de[26]) | ((u32)de[27] << 8);
                    fsize = *(u32*)(de + 28);
                    (void)fsize;
                    de[0] = 0xE5; /* mark deleted */
                    ata_write_sector(lba + s, dir_buf);
                    /* Free FAT chain */
                    u32 fc = start_clus;
                    while (fc >= 2 && fc < 0x0FFFFFF8) {
                        u32 next = fat32_next_cluster(fc);
                        u32 bo = fc * 4;
                        u32 fsec = bo / 512 + fat32_fat_start;
                        u32 fidx = bo % 512;
                        fat_read_cached(fsec);
                        *(u32*)(fat_buf + fidx) = 0;
                        fat_write_cached(fsec);
                        fc = next;
                    }
                    return 0;
                }
            }
        }
        clus = fat32_next_cluster(clus);
    }
    return -1;
}

i64 fat32_delete_file(const char *name83) {
    return fat32_delete_file_in(name83, fat32_root_clus);
}

/* ================================================================
 *  VFS (virtual filesystem — thin wrapper over FAT32)
 * ================================================================ */
FD fd_table[MAX_FILES];

/* Open a file by 8.3 name in dir_clus; returns fd index or -1 */
static i64 fat32_open_raw_in(const char *name83, FD *slot, u32 dir_clus) {
    u32 clus, fsize;
    if (fat32_find_raw_in(name83, &clus, &fsize, dir_clus) != 0) return -1;
    slot->size       = fsize;
    slot->start_clus = clus;
    slot->cur_clus   = clus;
    slot->pos        = 0;
    slot->dir_clus   = dir_clus;
    memcpy(slot->name83, name83, 11);
    return 0;
}

static i64 fat32_open_raw(const char *name83, FD *slot) {
    return fat32_open_raw_in(name83, slot, fat32_root_clus);
}

/* Read up to 512 bytes from current position into buf */
static i64 fat32_read_raw(FD *fd, void *buf, usize n) {
    if (fd->pos >= fd->size) return 0; /* EOF */
    u32 avail = fd->size - fd->pos;
    if (n > avail) n = avail;
    if (n > 512)   n = 512;

    /* Which sector within the current cluster? */
    u32 logical_sec  = fd->pos / 512;
    u32 clus_sec     = logical_sec % fat32_spc;
    u32 byte_off     = fd->pos % 512;
    /* Clamp write to sector boundary */
    u32 in_sec = 512 - byte_off;
    if (n > in_sec) n = in_sec;

    u32 lba = cluster_to_lba(fd->cur_clus) + clus_sec;
    ata_read_sector(lba, file_buf);
    memcpy(buf, file_buf + byte_off, n);
    fd->pos += (u32)n;

    /* Advance cluster if we hit a cluster boundary */
    u32 clus_bytes = (u32)fat32_spc * 512;
    if ((fd->pos % clus_bytes) == 0) {
        fd->cur_clus = fat32_next_cluster(fd->cur_clus);
    }
    return (i64)n;
}

/* Write up to 512 bytes (one sector) */
static i64 fat32_write_raw(FD *fd, const void *buf, usize n) {
    if (n > 512) n = 512;

    /* Allocate first cluster if file is empty */
    if (fd->start_clus == 0) {
        u32 c = fat32_alloc_cluster();
        if (c == (u32)-1) return -1;
        fd->start_clus = fd->cur_clus = c;
        fd->pos = 0;
        patch_dir_cluster(fd, c);
    }

    u32 logical_sec = fd->pos / 512;
    u32 clus_sec    = logical_sec % fat32_spc;
    u32 byte_off    = fd->pos % 512;
    u32 in_sec      = 512 - byte_off;
    if (n > in_sec) n = in_sec;

    u32 lba = cluster_to_lba(fd->cur_clus) + clus_sec;
    ata_read_sector(lba, file_buf);
    memcpy(file_buf + byte_off, buf, n);
    ata_write_sector(lba, file_buf);

    fd->pos += (u32)n;
    if (fd->pos > fd->size) {
        fd->size = fd->pos;
        patch_dir_size(fd, fd->size);
    }

    /* Advance cluster */
    u32 clus_bytes = (u32)fat32_spc * 512;
    if ((fd->pos % clus_bytes) == 0) {
        u32 next = fat32_next_cluster(fd->cur_clus);
        if (next >= 0x0FFFFFF8) {
            /* Allocate new cluster and link */
            u32 nc = fat32_alloc_cluster();
            if (nc == (u32)-1) return -1;
            fat_link(fd->cur_clus, nc);
            next = nc;
        }
        fd->cur_clus = next;
    }
    return (i64)n;
}

void vfs_init(void) {
    memset(fd_table, 0, sizeof(fd_table));
}

/* Open by 8.3 name (already formatted) */
i64 vfs_open_in(const char *name83, u32 dir_clus) {
    for (int i = 3; i < MAX_FILES; i++) {  /* 0=stdin 1=stdout 2=stderr reserved */
        if (!fd_table[i].in_use) {
            if (fat32_open_raw_in(name83, &fd_table[i], dir_clus) != 0) return -1;
            fd_table[i].in_use = 1;
            return i;
        }
    }
    return -1; /* too many open files */
}

i64 fat32_vfs_open(const char *name83) {
    return vfs_open_in(name83, fat32_root_clus);
}

i64 fat32_vfs_read(u64 fd, void *buf, usize n) {
    if (fd >= MAX_FILES || !fd_table[fd].in_use) return -1;
    return fat32_read_raw(&fd_table[fd], buf, n);
}

i64 fat32_vfs_write(u64 fd, const void *buf, usize n) {
    if (fd >= MAX_FILES || !fd_table[fd].in_use) return -1;
    return fat32_write_raw(&fd_table[fd], buf, n);
}

i64 fat32_vfs_close(u64 fd) {
    if (fd >= MAX_FILES || !fd_table[fd].in_use) return -1;
    fd_table[fd].in_use = 0;
    return 0;
}

/* Seek to absolute byte offset within an open file.
 * Walks the FAT chain from the start cluster to land on the right cluster.
 * Returns new position on success, -1 if offset is out of range. */
i64 fat32_vfs_seek(u64 fd, i64 offset, u64 whence) {
    if (fd >= MAX_FILES || !fd_table[fd].in_use) return (i64)EBADF;
    FD *f = &fd_table[fd];
    u32 new_pos;
    if (whence == 0)       new_pos = (u32)offset;                 /* SEEK_SET */
    else if (whence == 1)  new_pos = f->pos + (u32)offset;        /* SEEK_CUR */
    else if (whence == 2)  new_pos = f->size + (u32)offset;       /* SEEK_END */
    else return (i64)EINVAL;
    if (new_pos > f->size) new_pos = f->size;

    /* Walk chain from start to find the cluster that owns this offset */
    u32 clus_bytes = (u32)fat32_spc * 512;
    u32 target_clus_idx = new_pos / clus_bytes;  /* which cluster in chain */
    u32 clus = f->start_clus;
    for (u32 i = 0; i < target_clus_idx && clus < 0x0FFFFFF8; i++)
        clus = fat32_next_cluster(clus);

    f->cur_clus = clus;
    f->pos      = new_pos;   /* Bug fix: was incorrectly set to raw offset */
    return (i64)new_pos;
}

/* Rename a file in dir_clus (both names must be 8.3 formatted).
 * Returns 0 on success, -1 if src not found or dst name already taken. */
i64 fat32_rename_in(const char *old83, const char *new83, u32 dir_clus) {
    /* Check new name doesn't already exist */
    u32 dummy_clus, dummy_size;
    if (fat32_find_raw_in(new83, &dummy_clus, &dummy_size, dir_clus) == 0) return -1;

    u32 clus = dir_clus;
    while (clus < 0x0FFFFFF8) {
        u32 lba = cluster_to_lba(clus);
        for (u8 s = 0; s < fat32_spc; s++) {
            ata_read_sector(lba + s, dir_buf);
            for (int e = 0; e < 16; e++) {
                u8 *de = dir_buf + e * 32;
                if (de[0] == 0)    return -1;
                if (de[0] == 0xE5) continue;
                if (de[11] & 0x08) continue; /* skip volume label */
                if (strncmp((char*)de, old83, 11) == 0) {
                    memcpy(de, new83, 11);   /* overwrite name field in-place */
                    ata_write_sector(lba + s, dir_buf);
                    return 0;
                }
            }
        }
        clus = fat32_next_cluster(clus);
    }
    return -1;
}

i64 fat32_rename(const char *old83, const char *new83) {
    return fat32_rename_in(old83, new83, fat32_root_clus);
}

/* ================================================================
 *  FAT32 VFS OPS — bridge inode_ops_t → raw fat32 layer
 *  inode->ino holds the cluster number (root = fat32_root_clus).
 *  inode->fs_data holds a heap-allocated FD for open files.
 * ================================================================ */

/* Convert long name to 8.3: pad with spaces, upper-case, dot stripped. */
static void to_83(const char *name, char out[11]) {
    memset(out, ' ', 11);
    int i = 0, j = 0;
    while (name[i] && name[i] != '.' && j < 8) {
        out[j++] = (char)toupper((unsigned char)name[i++]);
    }
    while (name[i] && name[i] != '.') i++; /* skip rest of base */
    if (name[i] == '.') {
        i++;
        int k = 8;
        while (name[i] && k < 11) {
            out[k++] = (char)toupper((unsigned char)name[i++]);
        }
    }
}

static i64 fat32_ops_read(struct vfs_inode *inode, void *buf, usize count, usize offset) {
    FD *f = (FD*)inode->fs_data;
    if (!f) return (i64)EINVAL;
    /* Seek to requested offset if needed */
    if (f->pos != (u32)offset) {
        u32 clus_bytes = (u32)fat32_spc * 512;
        u32 target_idx = (u32)offset / clus_bytes;
        u32 clus = f->start_clus;
        for (u32 i = 0; i < target_idx && clus < 0x0FFFFFF8; i++)
            clus = fat32_next_cluster(clus);
        f->cur_clus = clus;
        f->pos      = (u32)offset;
    }
    usize done = 0;
    while (done < count) {
        i64 n = fat32_read_raw(f, (u8*)buf + done, count - done);
        if (n <= 0) break;
        done += (usize)n;
    }
    return (i64)done;
}

static i64 fat32_ops_write(struct vfs_inode *inode, const void *buf, usize count, usize offset) {
    FD *f = (FD*)inode->fs_data;
    if (!f) return (i64)EINVAL;
    if (f->pos != (u32)offset) {
        u32 clus_bytes = (u32)fat32_spc * 512;
        u32 target_idx = (u32)offset / clus_bytes;
        u32 clus = f->start_clus ? f->start_clus : fat32_root_clus;
        for (u32 i = 0; i < target_idx && clus < 0x0FFFFFF8; i++)
            clus = fat32_next_cluster(clus);
        f->cur_clus = clus;
        f->pos      = (u32)offset;
    }
    usize done = 0;
    while (done < count) {
        i64 n = fat32_write_raw(f, (const u8*)buf + done, count - done);
        if (n <= 0) break;
        done += (usize)n;
    }
    inode->attr.size = f->size;
    return (i64)done;
}

/* dir_buf already defined as file-static above; use a local buffer here */
static i64 fat32_ops_readdir(struct vfs_inode *inode, void *buf, usize count, usize *offset) {
    u32 clus = (u32)inode->ino;
    usize written = 0;
    u8 tmp[32];
    while (clus < 0x0FFFFFF8 && written + 64 <= count) {
        u32 lba = cluster_to_lba(clus);
        for (u8 s = 0; s < fat32_spc; s++) {
            ata_read_sector(lba + s, dir_buf);
            for (int e = 0; e < 16; e++) {
                u8 *de = dir_buf + e * 32;
                if (de[0] == 0) goto rdone;
                if (de[0] == 0xE5) continue;
                if (de[11] & 0x08) continue; /* volume label */
                if (de[11] & 0x10) continue; /* skip subdirs for now */
                if (written + 64 > count) goto rdone;
                memset(tmp, 0, 64);
                /* Copy 8.3 name, strip trailing spaces */
                int ni = 0;
                for (int k = 0; k < 8 && de[k] != ' '; k++) tmp[ni++] = de[k];
                if (de[8] != ' ') {
                    tmp[ni++] = '.';
                    for (int k = 8; k < 11 && de[k] != ' '; k++) tmp[ni++] = de[k];
                }
                memcpy((u8*)buf + written, tmp, 64);
                written += 64;
            }
        }
        clus = fat32_next_cluster(clus);
    }
    rdone:
    *offset += written;
    return (i64)written;
}

static i64 fat32_ops_lookup(struct vfs_inode *dir_inode, const char *name, struct vfs_inode *out) {
    char n83[11];
    to_83(name, n83);
    u32 dir_clus = (u32)dir_inode->ino;
    u32 out_clus, out_size;
    i64 r = fat32_find_raw_in(n83, &out_clus, &out_size, dir_clus);
    if (r < 0) return (i64)ENOENT;
    /* Allocate an FD for this file */
    FD *f = (FD*)heap_malloc(sizeof(FD));
    if (!f) return (i64)ENOMEM;
    memset(f, 0, sizeof(FD));
    f->in_use    = 1;
    f->size      = out_size;
    f->start_clus= out_clus;
    f->cur_clus  = out_clus;
    f->pos       = 0;
    f->dir_clus  = dir_clus;
    memcpy(f->name83, n83, 11);
    out->valid    = 1;
    out->ino      = out_clus;
    out->dev      = dir_inode->dev;
    out->rdev     = 0;
    out->refcount = 1;
    out->ops      = dir_inode->ops;
    out->fs_data  = f;
    out->mount    = dir_inode->mount;
    out->attr.type    = 1; /* IT_FILE */
    out->attr.mode    = 0644;
    out->attr.uid     = 0;
    out->attr.gid     = 0;
    out->attr.size    = out_size;
    out->attr.nlink   = 1;
    out->attr.blksize = 512;
    out->attr.blocks  = (out_size + 511) / 512;
    return 0;
}

static i64 fat32_ops_create(struct vfs_inode *dir_inode, const char *name, u16 mode, struct vfs_inode *out) {
    char n83[11];
    to_83(name, n83);
    u32 dir_clus = (u32)dir_inode->ino;
    fat32_create_file_in(n83, dir_clus);
    return fat32_ops_lookup(dir_inode, name, out);
}

static i64 fat32_ops_mkdir(struct vfs_inode *dir_inode, const char *name, u16 mode) {
    char n83[11];
    to_83(name, n83);
    u32 c = fat32_alloc_cluster();
    if (c == (u32)-1) return (i64)ENOSPC;
    fat32_create_dir_entry_in(n83, c, (u32)dir_inode->ino);
    return 0;
}

static i64 fat32_ops_unlink(struct vfs_inode *dir_inode, const char *name) {
    char n83[11];
    to_83(name, n83);
    i64 r = fat32_delete_file_in(n83, (u32)dir_inode->ino);
    return r < 0 ? (i64)ENOENT : 0;
}

static i64 fat32_ops_getsize(struct vfs_inode *inode) {
    FD *f = (FD*)inode->fs_data;
    return f ? (i64)f->size : (i64)inode->attr.size;
}

static i64 fat32_ops_setattr(struct vfs_inode *inode, inode_attr_t *attr) {
    inode->attr = *attr;
    return 0;
}

static inode_ops_t fat32_inode_ops = {
    .read    = fat32_ops_read,
    .write   = fat32_ops_write,
    .readdir = fat32_ops_readdir,
    .lookup  = fat32_ops_lookup,
    .create  = fat32_ops_create,
    .mkdir   = fat32_ops_mkdir,
    .unlink  = fat32_ops_unlink,
    .rmdir   = fat32_ops_unlink,
    .setattr = fat32_ops_setattr,
    .getsize = fat32_ops_getsize,
};

/* Call after fat32_init(). Mounts FAT32 root at "/" in the new VFS layer. */
void vfs_register_fat32(void) {
    FD *root_fd = (FD*)heap_malloc(sizeof(FD));
    if (!root_fd) return;
    memset(root_fd, 0, sizeof(FD));
    root_fd->in_use    = 1;
    root_fd->start_clus= fat32_root_clus;
    root_fd->cur_clus  = fat32_root_clus;
    root_fd->dir_clus  = fat32_root_clus;
    vfs_inode_t root;
    memset(&root, 0, sizeof(root));
    root.valid    = 1;
    root.ino      = fat32_root_clus;
    root.dev      = 1;
    root.refcount = 1;
    root.ops      = &fat32_inode_ops;
    root.fs_data  = root_fd;
    root.attr.type    = 2; /* IT_DIR */
    root.attr.mode    = 0755;
    root.attr.nlink   = 1;
    root.attr.blksize = 512;
    vfs_mount_fs("/", "fat32", 1, &root);
}

/* ================================================================
 *  SHARED KEYBOARD SCANCODE TABLES
 *  Defined once here; used by read_key_raw(), read_key(), and the
 *  GUI event loop.  Previously each site had its own private copy,
 *  risking divergence (e.g. the shift table was already slightly
 *  different between read_key_raw and the GUI).
 * ================================================================ */
static const u8 kb_sc[0x3A] = {
    0,0,'1','2','3','4','5','6','7','8','9','0','-','=','\b','\t',
    'q','w','e','r','t','y','u','i','o','p','[',']','\r',0,
    'a','s','d','f','g','h','j','k','l',';','\''  ,'`',0,'\\',
    'z','x','c','v','b','n','m',',','.','/',0,'*',0,' '
};
static const u8 kb_sc_shift[0x3A] = {
    0,0,'!','@','#','$','%','^','&','*','(',')','_','+','\b','\t',
    'Q','W','E','R','T','Y','U','I','O','P','{','}','\r',0,
    'A','S','D','F','G','H','J','K','L',':','"','~',0,'|',
    'Z','X','C','V','B','N','M','<','>','?',0,'*',0,' '
};
/* Apply shift + caps-lock to a raw scancode; returns ASCII or 0. */
static inline u8 kb_translate(u8 sc, u8 shift, u8 caps) {
    if (sc >= 0x3A) return 0;
    u8 ch = shift ? kb_sc_shift[sc] : kb_sc[sc];
    if (!ch) return 0;
    if (caps && islower((unsigned char)ch)) ch = (u8)toupper((unsigned char)ch);
    else if (caps && isupper((unsigned char)ch)) ch = (u8)tolower((unsigned char)ch);
    return ch;
}

/* ================================================================
 *  KEYBOARD (PS/2 polled)
 *
 *  Upgrades over original:
 *   - Caps Lock toggle (SC 0x3A / 0xBA)
 *   - Ctrl key tracking (Left SC 0x1D, Right 0xE0 0x1D)
 *   - Arrow keys returned as private codes (0x80–0x83)
 *   - Delete key returned as 0x7F
 *   - read_key() split into read_key_raw() + read_key() so callers
 *     that need raw codes (read_line) can get them.
 * ================================================================ */
#define KEY_UP    0x80
#define KEY_DOWN  0x81
#define KEY_LEFT  0x82
#define KEY_RIGHT 0x83
#define KEY_DEL   0x7F

/* Scancode tables moved to kb_sc / kb_sc_shift above — use kb_translate() */
static u8 shift_flag  = 0;
static u8 caps_flag   = 0;  /* Caps Lock toggle state */
static u8 ctrl_flag   = 0;  /* Ctrl held */
static u8 e0_flag     = 0;  /* 0xE0 prefix received */

/* Returns a raw key code — printable ASCII, \r, \b, or KEY_* special codes.
 * Scroll side-effects happen here too. */

/* read_key_raw — exported for sys_read (syscall.c uses this for stdin).
 * Returns printable ASCII, \r, \b, or KEY_* special codes. */
u8 read_key_raw(void) {
    for (;;) {
        /* Poll until either keyboard (bit0) or mouse (bit5) data arrives */
        u8 st;
        while (!((st = inb(0x64)) & 1)) {
            if (st & 0x20) mouse_poll();
        }
        /* Mouse byte sneaked in just before keyboard — drain it */
        if (inb(0x64) & 0x20) { mouse_poll(); continue; }
        u8 sc = inb(0x60);

        if (sc == 0xE0) { e0_flag = 1; continue; }
        if (sc == 0x2A || sc == 0x36) { shift_flag = 1; e0_flag = 0; input_mod_update(shift_flag,ctrl_flag,caps_flag); continue; }
        if (sc == 0xAA || sc == 0xB6) { shift_flag = 0; e0_flag = 0; input_mod_update(shift_flag,ctrl_flag,caps_flag); continue; }
        if (sc == 0x1D) { ctrl_flag = 1; e0_flag = 0; input_mod_update(shift_flag,ctrl_flag,caps_flag); continue; }
        if (sc == 0x9D) { ctrl_flag = 0; e0_flag = 0; input_mod_update(shift_flag,ctrl_flag,caps_flag); continue; }
        if (sc == 0x3A) { caps_flag ^= 1; e0_flag = 0; input_mod_update(shift_flag,ctrl_flag,caps_flag); continue; }
        if (sc == 0xBA) { e0_flag = 0; continue; }
        if (sc & 0x80)  {
            /* key release — stop held-key repeat */
            input_key_release();
            e0_flag = 0; continue;
        }
        if (e0_flag) {
            e0_flag = 0;
            if (sc == 0x48) { input_push_key(0x80|0x48, KEY_UP);    return KEY_UP;    }
            if (sc == 0x50) { input_push_key(0x80|0x50, KEY_DOWN);  return KEY_DOWN;  }
            if (sc == 0x4B) { input_push_key(0x80|0x4B, KEY_LEFT);  return KEY_LEFT;  }
            if (sc == 0x4D) { input_push_key(0x80|0x4D, KEY_RIGHT); return KEY_RIGHT; }
            if (sc == 0x53) { input_push_key(0x80|0x53, KEY_DEL);   return KEY_DEL;   }
            if (sc == 0x1D) { ctrl_flag = 1; continue; }
            if (sc == 0x9D) { ctrl_flag = 0; continue; }
            continue;
        }
        if (sc >= 0x3A) continue;
        u8 ch = kb_translate(sc, shift_flag, caps_flag);
        if (!ch) continue;
        input_push_key(sc, ch);
        return ch;
    }
}

static u8 read_key(void) {
    for (;;) {
        /* Poll until keyboard (bit0) or mouse (bit5) data ready */
        u8 st;
        while (!((st = inb(0x64)) & 1)) {
            if (st & 0x20) mouse_poll();
            watchdog_pet();   /* shell is alive, just waiting for keypress */
        }
        if (inb(0x64) & 0x20) { mouse_poll(); continue; }
        u8 sc = inb(0x60);

        /* Extended scancode prefix */
        if (sc == 0xE0) { e0_flag = 1; continue; }

        /* --- Modifier key-press / release --- */
        /* Shift press */
        if (sc == 0x2A || sc == 0x36) { shift_flag = 1; e0_flag = 0; continue; }
        /* Shift release */
        if (sc == 0xAA || sc == 0xB6) { shift_flag = 0; e0_flag = 0; continue; }
        /* Left Ctrl press / Right Ctrl press (0xE0 0x1D) */
        if (sc == 0x1D) { ctrl_flag = 1; e0_flag = 0; continue; }
        /* Left Ctrl release / Right Ctrl release */
        if (sc == 0x9D) { ctrl_flag = 0; e0_flag = 0; continue; }
        /* Caps Lock press (toggle on make, ignore break 0xBA) */
        if (sc == 0x3A) { caps_flag ^= 1; e0_flag = 0; continue; }
        if (sc == 0xBA) { e0_flag = 0; continue; }

        /* Key release (bit 7 set) — ignore */
        if (sc & 0x80) { e0_flag = 0; continue; }

        /* --- Extended keys (0xE0 prefix) --- */
        if (e0_flag) {
            e0_flag = 0;
            if (sc == 0x48) { return KEY_UP;    }  /* Up arrow    */
            if (sc == 0x50) { return KEY_DOWN;  }  /* Down arrow  */
            if (sc == 0x4B) { return KEY_LEFT;  }  /* Left arrow  */
            if (sc == 0x4D) { return KEY_RIGHT; }  /* Right arrow */
            if (sc == 0x53) { return KEY_DEL;   }  /* Delete      */
            if (sc == 0x49) { vga_scroll_up();   continue; } /* PageUp   */
            if (sc == 0x51) { vga_scroll_down(); continue; } /* PageDown */
            if (sc == 0x1D) { ctrl_flag = 1;    continue; } /* Right Ctrl press */
            if (sc == 0x9D) { ctrl_flag = 0;    continue; } /* Right Ctrl release */
            continue;
        }

        if (sc >= 0x3A) continue;

        u8 ch = kb_translate(sc, shift_flag, caps_flag);
        if (!ch) continue;

        /* Snap back to live view on any regular keypress */
        if (scroll_offset != 0) {
            scroll_offset = 0;
            vga_redraw();
            if (mouse_ready) mouse_show();
        }
        return ch;
    }
}

/* ================================================================
 *  COMMAND HISTORY  (ring buffer, navigated with Up/Down arrows)
 * ================================================================ */
#define HIST_MAX   16
#define HIST_LEN  128

static char  hist_buf[HIST_MAX][HIST_LEN];
static int   hist_count = 0;   /* total entries stored (capped at HIST_MAX) */
static int   hist_head  = 0;   /* next write slot (ring) */

static void hist_push(const char *s) {
    if (!s || !s[0]) return;
    /* Don't push duplicate of the most recent entry */
    if (hist_count > 0) {
        int prev = (hist_head - 1 + HIST_MAX) % HIST_MAX;
        if (strncmp(hist_buf[prev], s, HIST_LEN - 1) == 0) return;
    }
    int i = 0;
    while (i < HIST_LEN - 1 && s[i]) { hist_buf[hist_head][i] = s[i]; i++; }
    hist_buf[hist_head][i] = 0;
    hist_head = (hist_head + 1) % HIST_MAX;
    if (hist_count < HIST_MAX) hist_count++;
}

/* Returns pointer to history entry at position 'back' steps ago (1 = newest).
 * Returns NULL if out of range. */
static const char *hist_get(int back) {
    if (back < 1 || back > hist_count) return (void*)0;
    int idx = (hist_head - back + HIST_MAX * 2) % HIST_MAX;
    return hist_buf[idx];
}

/* Move the terminal cursor left by n positions using vga_backspace() */
static void cur_left(int n) {
    for (int i = 0; i < n; i++) vga_backspace();
}

/* Erase n characters: move left, overwrite with space, move left again */
static void erase_chars(int n) {
    for (int i = 0; i < n; i++) {
        vga_backspace();
        vga_putchar(' ');
        vga_backspace();
    }
}

static void read_line(char *buf, int max) {
    int n        = 0;   /* current length */
    int cur      = 0;   /* cursor position within buf (0..n) */
    int hist_pos = 0;   /* 0 = live input; >0 = browsing history */

    for (;;) {
        u8 c = read_key();

        /* --- Enter --- */
        if (c == '\r') {
            buf[n] = 0;
            vga_putchar('\n');
            hist_push(buf);
            return;
        }

        /* --- Backspace: delete char before cursor --- */
        if (c == '\b') {
            if (cur > 0) {
                /* Shift buffer left */
                for (int i = cur - 1; i < n - 1; i++) buf[i] = buf[i + 1];
                n--; cur--;
                /* Move terminal cursor to deletion point */
                vga_backspace();
                /* Redraw tail + erase the now-extra character at end */
                for (int i = cur; i < n; i++) vga_putchar((u8)buf[i]);
                vga_putchar(' ');
                /* Move cursor back to insertion point */
                cur_left(n - cur + 1);
            }
            continue;
        }

        /* --- Delete: forward-delete char at cursor --- */
        if (c == KEY_DEL) {
            if (cur < n) {
                for (int i = cur; i < n - 1; i++) buf[i] = buf[i + 1];
                n--;
                /* Redraw tail + erase extra char at end */
                for (int i = cur; i < n; i++) vga_putchar((u8)buf[i]);
                vga_putchar(' ');
                cur_left(n - cur + 1);
            }
            continue;
        }

        /* --- Left arrow --- */
        if (c == KEY_LEFT) {
            if (cur > 0) { cur--; vga_backspace(); }
            continue;
        }

        /* --- Right arrow --- */
        if (c == KEY_RIGHT) {
            if (cur < n) { vga_putchar((u8)buf[cur]); cur++; }
            continue;
        }

        /* --- Up arrow: older history --- */
        if (c == KEY_UP) {
            int next = hist_pos + 1;
            const char *h = hist_get(next);
            if (!h) continue;
            /* Move to end of line, then erase everything back to start */
            for (int i = cur; i < n; i++) vga_putchar((u8)buf[i]);
            erase_chars(n);
            hist_pos = next;
            n = 0; cur = 0;
            const char *p = h;
            while (*p && n < max - 1) { buf[n++] = *p; vga_putchar((u8)*p); p++; }
            cur = n;
            continue;
        }

        /* --- Down arrow: newer history or live input --- */
        if (c == KEY_DOWN) {
            if (hist_pos == 0) continue;
            for (int i = cur; i < n; i++) vga_putchar((u8)buf[i]);
            erase_chars(n);
            hist_pos--;
            n = 0; cur = 0;
            if (hist_pos > 0) {
                const char *h = hist_get(hist_pos);
                if (h) {
                    const char *p = h;
                    while (*p && n < max - 1) { buf[n++] = *p; vga_putchar((u8)*p); p++; }
                    cur = n;
                }
            }
            continue;
        }

        /* --- Regular printable character --- */
        if (c < 0x20 || c >= KEY_UP) continue; /* skip other non-printable */
        if (n >= max - 1) continue;

        hist_pos = 0; /* any typed char cancels history browse */

        /* Insert at cursor position */
        for (int i = n; i > cur; i--) buf[i] = buf[i - 1];
        buf[cur] = (char)c;
        n++;
        /* Redraw from cursor to end of line */
        for (int i = cur; i < n; i++) vga_putchar((u8)buf[i]);
        /* Move cursor back to just after inserted character */
        cur_left(n - cur - 1);
        cur++;
    }
}

/* ================================================================
 *  8.3 FILENAME FORMATTING
 * ================================================================ */
void format_83_name(const char *src, char *dst) {
    int i, j;
    /* init with spaces */
    for (i = 0; i < 11; i++) dst[i] = ' ';
    dst[11] = 0;

    /* name part (up to 8) */
    for (i = 0, j = 0; j < 8 && src[i] && src[i] != '.' && src[i] != ' '; i++, j++) {
        dst[j] = (char)toupper((unsigned char)src[i]);
    }
    /* skip past dot */
    while (src[i] && src[i] != '.') i++;
    if (src[i] == '.') {
        i++;
        /* extension (up to 3) */
        for (int k = 0; k < 3 && src[i] && src[i] != ' '; i++, k++) {
            dst[8 + k] = (char)toupper((unsigned char)src[i]);
        }
    }
}

/* ================================================================
 *  SHELL COMMANDS
 * ================================================================ */
static char input_buf[512];
static char name83[12];
static u8   cat_buf[513];

/* Current working directory cluster (0 = root) */
u32  cwd_clus = 0;   /* exported — syscall.c uses these */
char cwd_path[128] = "/";

/* ── Shell VFS helpers ──────────────────────────────────────────
 * Build full path from cwd_path + name and call new VFS layer.
 * sh_open  → vfs_open   (new abstracted VFS)
 * sh_read  → vfs_read
 * sh_write → vfs_write
 * sh_close → vfs_close
 * sh_create→ vfs_create (creates + opens)
 * sh_unlink→ vfs_unlink
 * sh_mkdir → vfs_mkdir
 * ──────────────────────────────────────────────────────────────*/
static void sh_fullpath(const char *name, char *out, usize outsz) {
    /* slibc_path_join handles absolute vs. relative and separator insertion */
    if (slibc_path_join(out, outsz, cwd_path, name) < 0)
        out[0] = '\0';   /* truncation — caller will get an error from vfs */
}

static i64 sh_open(const char *name) {
    char p[256]; sh_fullpath(name, p, sizeof(p));
    return vfs_open(p);
}
static i64 sh_create(const char *name, u16 mode) {
    char p[256]; sh_fullpath(name, p, sizeof(p));
    return vfs_create(p, mode);
}
static i64 sh_unlink(const char *name) {
    char p[256]; sh_fullpath(name, p, sizeof(p));
    return vfs_unlink(p);
}
static i64 sh_mkdir(const char *name, u16 mode) {
    char p[256]; sh_fullpath(name, p, sizeof(p));
    return vfs_mkdir(p, mode);
}
/* read/write/close are just vfs_read/write/close directly */
#define sh_read(fd,buf,n)   vfs_read((u64)(fd),(buf),(n))
#define sh_write(fd,buf,n)  vfs_write((u64)(fd),(buf),(n))
#define sh_close(fd)        vfs_close((u64)(fd))

/* Skip the first word in s, return pointer to start of next word (or NULL).
 * Uses isspace() from systrix_libc for robust whitespace classification. */
static const char *get_arg(const char *s) {
    /* skip the first token (non-space characters) */
    while (*s && !isspace((unsigned char)*s)) s++;
    /* skip whitespace using isspace() from systrix_libc */
    while (isspace((unsigned char)*s)) s++;
    return *s ? s : NULL;
}

/* Active cluster for directory ops — root if cwd_clus == 0 */
static u32 cwd_active(void) {
    return (cwd_clus == 0) ? fat32_root_clus : cwd_clus;
}

static void cmd_meminfo(void) {
    u32 free  = pmm_free_pages();
    /* Use runtime ram_end_actual (from E820) — NOT the compile-time
     * TOTAL_PAGES which always equals the 64 GB ceiling regardless of
     * how much RAM QEMU was given.  This is why meminfo used to report
     * 4 GB even when only 128 MB was configured. */
    u64 actual_pages = (ram_end_actual > RAM_START)
                       ? (ram_end_actual - RAM_START) / PAGE_SIZE : 0;
    u64 total = actual_pages;
    u64 used  = (total > free) ? (total - free) : 0;
    /* Display in KB (page * 4) and MB for readability */
    u64 total_mb = (total * 4) / 1024;
    u64 used_mb  = (used  * 4) / 1024;
    u64 free_mb  = (free  * 4ULL) / 1024;
    kprintf("Memory (4 KB pages):\r\n");
    kprintf("  Total : %u pages (%u MB)\r\n", (unsigned)total, (unsigned)total_mb);
    kprintf("  Used  : %u pages (%u MB)\r\n", (unsigned)used, (unsigned)used_mb);
    kprintf("  Free  : %u pages (%u MB)\r\n", (unsigned)free, (unsigned)free_mb);
    kprintf("  RAM ceiling (E820 high): ");
    /* print ram_end_actual in MB */
    kprintf("%u", (unsigned)(ram_end_actual / (1024*1024))); kprintf(" MB\r\n");
}

static void cmd_uptime(void) {
    u32 secs  = (u32)(pit_ticks / 1000);
    u32 mins  = secs / 60;  secs %= 60;
    u32 hours = mins / 60;  mins %= 60;
    kprintf("Uptime: %uh %um %us\r\n", (unsigned)hours, (unsigned)mins, (unsigned)secs);
}

static void cmd_uname(void) {
    kprintf("Systrix 0.1 x86-64 (microkernel, preemptive, FAT32, ring-3 ELF)\r\n");
}

/* ================================================================
 *  FILE MANAGEMENT COMMANDS
 * ================================================================ */

/* ---- ls -------------------------------------------------------- */
static void cmd_ls(void) {
    u8 *dbuf = (u8*)heap_malloc(512);
    if (!dbuf) return;
    u32 clus = cwd_active();
    int file_count = 0, dir_count = 0;
    u32 total_bytes = 0;

    /* Header */
    kprintf("Name                         Type    Size\r\n----                         ----    ----\r\n");

    /* LFN accumulator: up to 20 LFN entries * 13 UCS-2 chars = 260 chars */
    char lfn_buf[261];
    int  lfn_len = 0;

    while (clus < 0x0FFFFFF8) {
        u32 lba = cluster_to_lba(clus);
        for (u8 s = 0; s < fat32_spc; s++) {
            ata_read_sector(lba + s, dbuf);
            for (int e = 0; e < 16; e++) {
                watchdog_pet();
                u8 *de = dbuf + e * 32;
                if (de[0] == 0x00) { heap_free(dbuf); goto ls_done; }
                if (de[0] == 0xE5) { lfn_len = 0; continue; }
                u8 attr = de[11];

                /* Collect LFN entry (attr == 0x0F) */
                if (attr == 0x0F) {
                    /* LFN entry: chars at offsets 1,3,5,7,9 (5 chars),
                       14,16,18,20,22,24 (6 chars), 28,30 (2 chars) = 13 UCS-2 */
                    static const int lfn_off[13] = {1,3,5,7,9,14,16,18,20,22,24,28,30};
                    u8 seq = de[0] & 0x1F;  /* sequence number 1-based */
                    if (seq < 1 || seq > 20) { lfn_len = 0; continue; }
                    /* Write 13 chars into correct position in lfn_buf */
                    int base = (seq - 1) * 13;
                    for (int ci = 0; ci < 13; ci++) {
                        u16 wc = (u16)(de[lfn_off[ci]]) | ((u16)(de[lfn_off[ci]+1]) << 8);
                        if (wc == 0x0000 || wc == 0xFFFF) {
                            /* end of name in this entry */
                            if (base + ci > lfn_len) lfn_len = base + ci;
                            break;
                        }
                        lfn_buf[base + ci] = (wc < 0x80) ? (char)wc : '?';
                        if (base + ci + 1 > lfn_len) lfn_len = base + ci + 1;
                    }
                    continue;
                }

                if (attr & 0x08) { lfn_len = 0; continue; }  /* volume label */
                if (attr & 0x02) { lfn_len = 0; continue; }  /* hidden */
                if (de[0] == '.') { lfn_len = 0; continue; } /* . and .. */

                /* Determine display name: prefer LFN, fall back to 8.3 */
                char name[261];
                int  ni;
                if (lfn_len > 0) {
                    lfn_buf[lfn_len] = 0;
                    /* copy LFN into name */
                    for (ni = 0; ni < lfn_len && ni < 260; ni++)
                        name[ni] = lfn_buf[ni];
                    name[ni] = 0;
                    lfn_len = 0;
                } else {
                    /* format 8.3 name */
                    ni = 0;
                    for (int k = 0; k < 8 && de[k] != ' '; k++)
                        name[ni++] = (char)de[k];
                    if (de[8] != ' ') {
                        name[ni++] = '.';
                        for (int k = 8; k < 11 && de[k] != ' '; k++)
                            name[ni++] = (char)de[k];
                    }
                    name[ni] = 0;
                }

                int is_dir = (attr & 0x10) ? 1 : 0;

                /* print name, padded to 28 chars */
                kprintf("%s", name);
                if (is_dir) vga_putchar('/');
                { char pad_buf[32];
                  int used = ni + is_dir;
                  int pad  = (used < 28) ? 28 - used : 1;
                  slibc_str_repeat(pad_buf, ' ', (usize)pad);
                  kprintf("%s", pad_buf); }

                if (is_dir) {
                    kprintf("<DIR>      -\r\n");
                    dir_count++;
                } else {
                    u32 sz = *(u32*)(de + 28);
                    kprintf("file  %*u B\r\n", 6, (unsigned)(sz));
                    total_bytes += sz;
                    file_count++;
                }
            }
        }
        clus = fat32_next_cluster(clus);
    }
ls_done:
    kprintf("----                         ----    ----\r\n%d file(s), %d dir(s)   total: ", file_count, dir_count);
    kprintf_size_human(total_bytes);
    kprintf("\r\n");
    if (dbuf) heap_free(dbuf);
}

/* ---- cd -------------------------------------------------------- */
static void cmd_cd(const char *name) {
    if (!name || (name[0] == '/' && name[1] == 0)) {
        cwd_clus = 0;
        cwd_path[0] = '/'; cwd_path[1] = 0;
        return;
    }
    /* handle ".." — go to root (flat FAT32, one level deep) */
    if (name[0] == '.' && name[1] == '.' && name[2] == 0) {
        cwd_clus = 0;
        cwd_path[0] = '/'; cwd_path[1] = 0;
        return;
    }

    char n83[12]; format_83_name(name, n83);

    /* scan current dir for a matching directory entry */
    u32 clus = cwd_active();
    while (clus < 0x0FFFFFF8) {
        u32 lba = cluster_to_lba(clus);
        for (u8 s = 0; s < fat32_spc; s++) {
            ata_read_sector(lba + s, dir_buf);
            for (int e = 0; e < 16; e++) {
                u8 *de = dir_buf + e * 32;
                if (de[0] == 0x00) goto cd_notfound;
                if (de[0] == 0xE5) continue;
                if (!(de[11] & 0x10)) continue;  /* must be dir */
                if (de[0] == '.') continue;
                /* compare 8.3 */
                int match = 1;
                for (int k = 0; k < 11; k++) if (de[k] != (u8)n83[k]) { match = 0; break; }
                if (!match) continue;
                /* found — get cluster */
                u32 hi = *(u16*)(de + 20);
                u32 lo = *(u16*)(de + 26);
                cwd_clus = (hi << 16) | lo;
                /* update path string */
                if (cwd_path[1] != '\0') strlcat(cwd_path, "/", sizeof(cwd_path));
                strlcat(cwd_path, name, sizeof(cwd_path));
                return;
            }
        }
        clus = fat32_next_cluster(clus);
    }
cd_notfound:
    kprintf("cd: no such directory\r\n");
}

/* ---- stat ------------------------------------------------------ */
static void cmd_stat(const char *name) {
    char n83[12]; format_83_name(name, n83);
    u32 clus = cwd_active();
    while (clus < 0x0FFFFFF8) {
        u32 lba = cluster_to_lba(clus);
        for (u8 s = 0; s < fat32_spc; s++) {
            ata_read_sector(lba + s, dir_buf);
            for (int e = 0; e < 16; e++) {
                u8 *de = dir_buf + e * 32;
                if (de[0] == 0x00) goto stat_notfound;
                if (de[0] == 0xE5) continue;
                u8 attr = de[11];
                if (attr == 0x0F || (attr & 0x08)) continue;
                int match = 1;
                for (int k = 0; k < 11; k++) if (de[k] != (u8)n83[k]) { match = 0; break; }
                if (!match) continue;

                kprintf("File : "); kprintf("%s", name); kprintf("\r\n");
                kprintf("Type : "); kprintf("%s", (attr & 0x10) ? "Directory" : "File"); kprintf("\r\n");
                if (!(attr & 0x10)) {
                    u32 sz = *(u32*)(de + 28);
                    kprintf("Size : "); kprintf("%u", (unsigned)(sz)); kprintf(" B (");
                    kprintf_size_human(sz); kprintf(")\r\n");
                }
                u32 hi = *(u16*)(de + 20);
                u32 lo = *(u16*)(de + 26);
                u32 fc = (hi << 16) | lo;
                kprintf("Clus : "); kprintf("%u", (unsigned)(fc)); kprintf("\r\n");
                /* FAT date/time (creation) */
                u16 cdate = *(u16*)(de + 16);
                u16 ctime = *(u16*)(de + 14);
                u32 year  = 1980 + ((cdate >> 9) & 0x7F);
                u32 mon   = (cdate >> 5) & 0xF;
                u32 day   = cdate & 0x1F;
                u32 hour  = (ctime >> 11) & 0x1F;
                u32 min   = (ctime >> 5) & 0x3F;
                kprintf("Date : %u-%02u-%02u  %02u:%02u\r\n",
                            year, mon, day, hour, min);
                u8 ro = (attr & 0x01) ? 1 : 0;
                kprintf("Attr : "); kprintf("%s", ro ? "read-only" : "read-write");
                kprintf("\r\n");
                return;
            }
        }
        clus = fat32_next_cluster(clus);
    }
stat_notfound:
    kprintf("stat: not found\r\n");
}

/* ---- df -------------------------------------------------------- */
static void cmd_df(void) {
    /* Use the cluster count computed at mount time (fat32_total_clus)
     * instead of re-reading and re-parsing the BPB every time df runs.
     * Also use the FAT sector cache (fat_read_cached) so repeated calls
     * don't hammer the disk with redundant sector reads. */
    u32 total_clus = fat32_total_clus;
    if (total_clus == 0) { kprintf("df: filesystem not mounted\r\n"); return; }

    u32 free_clus = 0;
    for (u32 c = 2; c < total_clus + 2; c++) {
        u32 byte_off = c * 4;
        u32 fat_sec  = byte_off / 512 + fat32_fat_start;
        u32 fat_off  = byte_off % 512;
        fat_read_cached(fat_sec);   /* uses the shared FAT cache */
        u32 entry = *(u32*)(fat_buf + fat_off) & 0x0FFFFFFF;
        if (entry == 0) free_clus++;
    }

    u32 clus_bytes  = (u32)fat32_spc * 512;
    u32 total_bytes = total_clus * clus_bytes;
    u32 free_bytes  = free_clus  * clus_bytes;
    u32 used_bytes  = total_bytes - free_bytes;

    kprintf("Filesystem : FAT32\r\n");
    kprintf("Total      : "); kprintf_size_human(total_bytes); kprintf("\r\n");
    kprintf("Used       : "); kprintf_size_human(used_bytes);  kprintf("\r\n");
    kprintf("Free       : "); kprintf_size_human(free_bytes);  kprintf("\r\n");
    kprintf("Clusters   : "); kprintf("%u", (unsigned)(free_clus)); kprintf(" free / ");
    kprintf("%u", (unsigned)(total_clus)); kprintf(" total\r\n");
}

/* ---- find ------------------------------------------------------ */
static void cmd_find(const char *pattern) {
    /* Simple substring match across root dir (and one level of subdirs) */
    u32 clus = cwd_active();
    int found = 0;
    while (clus < 0x0FFFFFF8) {
        u32 lba = cluster_to_lba(clus);
        for (u8 s = 0; s < fat32_spc; s++) {
            ata_read_sector(lba + s, dir_buf);
            for (int e = 0; e < 16; e++) {
                u8 *de = dir_buf + e * 32;
                if (de[0] == 0x00) goto find_done;
                if (de[0] == 0xE5) continue;
                u8 attr = de[11];
                if (attr == 0x0F || (attr & 0x08)) continue;
                if (de[0] == '.') continue;

                /* build display name */
                char name[13]; int ni = 0;
                for (int k = 0; k < 8 && de[k] != ' '; k++) name[ni++] = (char)de[k];
                if (de[8] != ' ') {
                    name[ni++] = '.';
                    for (int k = 8; k < 11 && de[k] != ' '; k++) name[ni++] = (char)de[k];
                }
                name[ni] = 0;

                /* substring search (case-insensitive) */
                if (pattern) {
                    int pi = 0, match = 0;
                    for (int k = 0; k <= ni && !match; k++) {
                        pi = 0;
                        while (pattern[pi] && k + pi <= ni) {
                            char a = (char)tolower((unsigned char)name[k+pi]);
                            char b = (char)tolower((unsigned char)pattern[pi]);
                            if (a != b) break;
                            pi++;
                        }
                        if (!pattern[pi]) match = 1;
                    }
                    if (!match) continue;
                }

                kprintf("%s", cwd_path);
                if (cwd_path[1] != 0) vga_putchar('/');  /* not root */
                kprintf("%s", name);
                if (attr & 0x10) vga_putchar('/');
                kprintf("\r\n");
                found++;
            }
        }
        clus = fat32_next_cluster(clus);
    }
find_done:
    if (!found) kprintf("find: no matches\r\n");
    else { kprintf("%u", (unsigned)(found)); kprintf(" match(es)\r\n"); }
}

/* ---- head / tail ----------------------------------------------- */
static void cmd_head(const char *arg) {
    /* head <file> [n]  — default 10 lines */
    const char *a2 = get_arg(arg);
    int lines = 10;
    if (a2) {
        lines = atoi(a2);
        if (!lines) lines = 10;
    }
    i64 fd = sh_open(arg);
    if (fd < 0) { kprintf("head: file not found\r\n"); return; }
    int lc = 0;
    for (;;) {
        i64 n = sh_read(fd, cat_buf, 512);
        if (n <= 0) break;
        for (i64 i = 0; i < n && lc < lines; i++) {
            vga_putchar(cat_buf[i]);
            if (cat_buf[i] == '\n') lc++;
        }
        if (lc >= lines) break;
    }
    sh_close(fd);
    kprintf("\r\n");
}

static void cmd_tail(const char *arg) {
    /* tail <file> [n]  — default 10 lines, loads full file into heap */
    const char *a2 = get_arg(arg);
    int lines = 10;
    if (a2) {
        lines = atoi(a2);
        if (!lines) lines = 10;
    }
    i64 fd = sh_open(arg);
    if (fd < 0) { kprintf("tail: file not found\r\n"); return; }

    /* read full file */
    u8 *buf = (u8*)heap_malloc(32768);
    if (!buf) { sh_close(fd); kprintf("OOM\r\n"); return; }
    usize total = 0;
    for (;;) {
        i64 n = sh_read(fd, buf + total, 512);
        if (n <= 0) break;
        total += (usize)n;
        if (total >= 32768 - 512) break;
    }
    sh_close(fd);

    /* find start of last N lines */
    int lc = 0;
    usize start = total;
    while (start > 0 && lc < lines) {
        start--;
        if (buf[start] == '\n' && start < total - 1) lc++;
    }
    if (start > 0) start++;  /* skip the newline we stopped on */
    for (usize i = start; i < total; i++) vga_putchar(buf[i]);
    heap_free(buf);
    kprintf("\r\n");
}

/* ---- append ---------------------------------------------------- */
static void cmd_append(const char *arg) {
    const char *text = get_arg(arg);
    if (!text) { kprintf("Usage: append <file> <text>\r\n"); return; }
    i64 fd = sh_open(arg);
    if (fd < 0) fd = sh_create(arg, 0644);
    if (fd < 0) { kprintf("Error.\r\n"); return; }
    i64 sz = vfs_seek((u64)fd, 0, 2); /* seek to end */
    (void)sz;
    usize tlen = strlen(text);
    usize written = 0;
    while (written < tlen) {
        i64 n = sh_write(fd, text + written, tlen - written);
        if (n <= 0) break;
        written += (usize)n;
    }
    sh_write(fd, "\r\n", 2);
    sh_close(fd);
    kprintf("Appended.\r\n");
}

/* ---- mkdir / rmdir --------------------------------------------- */
static void cmd_mkdir(const char *name) {
    char n83[12]; format_83_name(name, n83);
    i64 r = sh_mkdir(n83, 0755);
    if (r == 0)
        kprintf("Directory created.\r\n");
    else
        kprintf("mkdir: %s\r\n", strerror((int)r));
}

static void cmd_rmdir(const char *name) {
    /* Only remove empty directories — scan cluster for non-dot entries */
    char n83[12]; format_83_name(name, n83);
    /* find dir entry */
    u32 clus = cwd_active();
    u32 dir_clus = (u32)-1;
    while (clus < 0x0FFFFFF8) {
        u32 lba = cluster_to_lba(clus);
        for (u8 s = 0; s < fat32_spc; s++) {
            ata_read_sector(lba + s, dir_buf);
            for (int e = 0; e < 16; e++) {
                u8 *de = dir_buf + e * 32;
                if (de[0] == 0x00) goto rmdir_search_done;
                if (de[0] == 0xE5) continue;
                if (!(de[11] & 0x10)) continue;
                if (de[0] == '.') continue;
                int match = 1;
                for (int k = 0; k < 11; k++) if (de[k] != (u8)n83[k]) { match = 0; break; }
                if (!match) continue;
                u32 hi = *(u16*)(de + 20);
                u32 lo = *(u16*)(de + 26);
                dir_clus = (hi << 16) | lo;
                goto rmdir_search_done;
            }
        }
        clus = fat32_next_cluster(clus);
    }
rmdir_search_done:
    if (dir_clus == (u32)-1) { kprintf("rmdir: not found\r\n"); return; }

    /* check if empty */
    u32 dc = dir_clus;
    while (dc < 0x0FFFFFF8) {
        u32 lba = cluster_to_lba(dc);
        for (u8 s = 0; s < fat32_spc; s++) {
            ata_read_sector(lba + s, dir_buf);
            for (int e = 0; e < 16; e++) {
                u8 *de = dir_buf + e * 32;
                if (de[0] == 0x00) goto rmdir_check_done;
                if (de[0] == 0xE5) continue;
                if (de[0] == '.') continue;
                if (!(de[11] & 0x08)) {
                    kprintf("rmdir: directory not empty\r\n");
                    return;
                }
            }
        }
        dc = fat32_next_cluster(dc);
    }
rmdir_check_done:
    /* delete via fat32_delete_file (marks entry 0xE5, frees chain) */
    if (sh_unlink(n83) == 0)
        kprintf("Directory removed.\r\n");
    else
        kprintf("rmdir: failed\r\n");
}

/* ---- calc ------------------------------------------------------
 * Evaluate a simple arithmetic expression:  <num> <op> <num>
 * Supports: +  -  *  /  %  and optional parens around the whole expr.
 * All arithmetic is 64-bit signed.  Division by zero is caught.
 * Examples:
 *   calc 6 * 7          → 42
 *   calc 1024 / 8       → 128
 *   calc 100 - 37       → 63
 *   calc 7 % 3          → 1
 *
 * Extended form (no-spaces, e.g. "calc 3+4") is also accepted by
 * the inline scanner below.
 * ---------------------------------------------------------------- */

/* Skip leading whitespace, parse a signed 64-bit integer, advance *pp. */
static i64 calc_parse_int(const char **pp) {
    const char *p = *pp;
    while (isspace((unsigned char)*p)) p++;
    i64 neg = 1;
    if (*p == '-') { neg = -1; p++; }
    else if (*p == '+') { p++; }
    if (!isdigit((unsigned char)*p)) { *pp = p; return INT64_MIN; } /* sentinel = parse error */
    i64 v = 0;
    while (isdigit((unsigned char)*p))
        v = v * 10 + (*p++ - '0');
    *pp = p;
    return neg * v;
}

static void cmd_calc(const char *arg) {
    if (!arg || !*arg) {
        kprintf("Usage: calc <expr>   e.g.  calc 6 * 7\r\n");
        kprintf("  Operators: + - * / %%\r\n");
        return;
    }

    const char *p = arg;
    i64 a = calc_parse_int(&p);
    if (a == INT64_MIN) { kprintf("calc: invalid number\r\n"); return; }

    while (isspace((unsigned char)*p)) p++;
    if (!*p) {
        /* Single number — just echo it */
        kprintf("%lld\r\n", (long long)a);
        return;
    }

    char op = *p++;

    i64 b = calc_parse_int(&p);
    if (b == INT64_MIN) { kprintf("calc: invalid number after operator\r\n"); return; }

    /* Skip any trailing chars (spaces, closing paren, etc.) */
    while (isspace((unsigned char)*p)) p++;
    if (*p) { kprintf("calc: unexpected input after expression\r\n"); return; }

    i64 result;
    switch (op) {
        case '+': result = a + b; break;
        case '-': result = a - b; break;
        case '*': result = a * b; break;
        case 'x': result = a * b; break;   /* 'x' as alternative multiply */
        case '/':
            if (b == 0) { kprintf("calc: division by zero\r\n"); return; }
            result = a / b;
            break;
        case '%':
            if (b == 0) { kprintf("calc: modulo by zero\r\n"); return; }
            result = a % b;
            break;
        default:
            kprintf("calc: unknown operator '%c'  (use + - * / %%)\r\n", op);
            return;
    }

    kprintf("%lld\r\n", (long long)result);
}

/* ---- wc -------------------------------------------------------- */
static void cmd_wc(const char *name) {
    i64 fd = sh_open(name);
    if (fd < 0) { kprintf("wc: file not found\r\n"); return; }
    u32 lines = 0, words = 0, bytes = 0;
    int in_word = 0;
    for (;;) {
        i64 n = sh_read(fd, cat_buf, 512);
        if (n <= 0) break;
        for (i64 i = 0; i < n; i++) {
            u8 c = cat_buf[i];
            bytes++;
            if (c == '\n') lines++;
            if (isspace((unsigned char)c)) in_word = 0;
            else if (!in_word) { in_word = 1; words++; }
        }
    }
    sh_close(fd);
    kprintf("%u lines  %u words  %u bytes\r\n",
                lines, words, bytes);
}

/* ---- cp -------------------------------------------------------- */
static void cmd_cp(const char *arg) {
    const char *dst = get_arg(arg);
    if (!dst) { kprintf("Usage: cp <src> <dst>\r\n"); return; }
    char src83[12], dst83[12];
    format_83_name(arg, src83);
    format_83_name(dst, dst83);

    i64 fd_in = sh_open(src83);
    if (fd_in < 0) { kprintf("cp: source not found\r\n"); return; }
    i64 fd_out = sh_open(dst83);
    if (fd_out < 0) fd_out = sh_create(dst83, 0644);
    if (fd_out < 0) { sh_close(fd_in); kprintf("cp: cannot create dest\r\n"); return; }

    static u8 cp_buf[512];
    u32 total = 0;
    i64 n;
    while ((n = sh_read(fd_in, cp_buf, sizeof(cp_buf))) > 0) {
        sh_write(fd_out, cp_buf, (usize)n);
        total += (u32)n;
    }
    sh_close(fd_in);
    sh_close(fd_out);
    kprintf("Copied "); kprintf_size_human(total); kprintf("\r\n");
}

/* ---- cat / hexdump / touch / rm / rename / write (unchanged) --- */

/* ---- Markdown renderer -----------------------------------------
 * Renders .md files with VGA colours instead of raw syntax.
 *
 * VGA fg colour palette (bg=0 black):
 *   0x0F white   0x0E yellow  0x0B cyan    0x0A green
 *   0x09 blue    0x0D magenta 0x07 grey    0x08 dark-grey
 *
 * Supported syntax:
 *   # / ## / ###   headers      (yellow / cyan / green)
 *   **text**        bold         (bright white on black = 0x0F, already default)
 *   *text*          italic       (magenta)
 *   `text`          inline code  (cyan)
 *   - / * item     bullet       (green dash + white text)
 *   > text         blockquote   (grey)
 *   ---            horizontal rule
 *   blank line     paragraph break
 ----------------------------------------------------------------- */

/* Emit 'len' chars from 'p' using colour 'attr', then restore default. */
static void md_puts_col(const char *p, int len, u8 attr) {
    vga_set_attr(attr);
    for (int i = 0; i < len; i++) vga_putchar((u8)p[i]);
    vga_reset_attr();
}

/* Render one line of markdown inline spans (**bold**, *italic*, `code`).
 * Called after the line-level prefix (header marker etc.) has been handled. */
static void md_render_inline(const char *p, int len) {
    int i = 0;
    while (i < len) {
        /* backtick inline code */
        if (p[i] == '`') {
            int j = i + 1;
            while (j < len && p[j] != '`') j++;
            vga_set_attr(0x0B); /* cyan */
            for (int k = i + 1; k < j; k++) vga_putchar((u8)p[k]);
            vga_reset_attr();
            i = (j < len) ? j + 1 : j;
            continue;
        }
        /* **bold** */
        if (p[i] == '*' && i + 1 < len && p[i+1] == '*') {
            int j = i + 2;
            while (j + 1 < len && !(p[j] == '*' && p[j+1] == '*')) j++;
            vga_set_attr(0x0F); /* bright white — already default but explicit */
            for (int k = i + 2; k < j; k++) vga_putchar((u8)p[k]);
            vga_reset_attr();
            i = (j + 1 < len) ? j + 2 : len;
            continue;
        }
        /* *italic* */
        if (p[i] == '*') {
            int j = i + 1;
            while (j < len && p[j] != '*') j++;
            vga_set_attr(0x0D); /* magenta */
            for (int k = i + 1; k < j; k++) vga_putchar((u8)p[k]);
            vga_reset_attr();
            i = (j < len) ? j + 1 : j;
            continue;
        }
        /* plain character */
        vga_putchar((u8)p[i]);
        i++;
    }
}

static void cmd_mdcat(const char *name) {
    i64 fd = sh_open(name);
    if (fd < 0) { kprintf("File not found.\r\n"); return; }

    /* Read whole file into a heap buffer so we can walk lines. */
    /* We reuse cat_buf for 512-byte chunks and accumulate into a line buf. */
    static char line[256];
    int llen = 0;

    for (;;) {
        i64 n = sh_read(fd, cat_buf, 512);
        if (n <= 0) break;
        for (i64 ci = 0; ci < n; ci++) {
            char c = (char)cat_buf[ci];
            if (c == '\r') continue;
            if (c == '\n') {
                /* process accumulated line */
                line[llen] = '\0';
                const char *l = line;
                int ll = llen;
                llen = 0;

                /* --- line-level elements --- */

                /* horizontal rule: --- or *** or ___ (at least 3) */
                if (ll >= 3 && (l[0]=='-'||l[0]=='*'||l[0]=='_')) {
                    int all = 1;
                    for (int i = 0; i < ll; i++)
                        if (l[i] != l[0] && l[i] != ' ') { all = 0; break; }
                    if (all) {
                        vga_set_attr(0x08); /* dark grey */
                        for (int i = 0; i < 80; i++) vga_putchar('-');
                        vga_reset_attr();
                        kprintf("\r\n");
                        continue;
                    }
                }

                /* headers ### ## # */
                if (l[0] == '#') {
                    int hlvl = 0;
                    while (hlvl < ll && l[hlvl] == '#') hlvl++;
                    u8 hcol = (hlvl == 1) ? 0x0E :   /* yellow  H1 */
                              (hlvl == 2) ? 0x0B :   /* cyan    H2 */
                                            0x0A;    /* green   H3+ */
                    const char *htxt = l + hlvl;
                    int htlen = ll - hlvl;
                    while (htlen > 0 && htxt[0] == ' ') { htxt++; htlen--; }
                    vga_set_attr(hcol);
                    md_render_inline(htxt, htlen);
                    vga_reset_attr();
                    kprintf("\r\n");
                    continue;
                }

                /* blockquote > */
                if (l[0] == '>' ) {
                    vga_set_attr(0x07); /* grey */
                    vga_putchar('>');
                    vga_putchar(' ');
                    int off = (ll > 1 && l[1] == ' ') ? 2 : 1;
                    md_render_inline(l + off, ll - off);
                    vga_reset_attr();
                    kprintf("\r\n");
                    continue;
                }

                /* bullet: "- " or "* " (but not "---") */
                if (ll >= 2 && (l[0] == '-' || l[0] == '*') && l[1] == ' ') {
                    vga_set_attr(0x0A); /* green bullet */
                    kprintf("  - ");
                    vga_reset_attr();
                    md_render_inline(l + 2, ll - 2);
                    kprintf("\r\n");
                    continue;
                }

                /* blank line */
                if (ll == 0) { kprintf("\r\n"); continue; }

                /* normal paragraph line — render inline spans */
                md_render_inline(l, ll);
                kprintf("\r\n");
            } else {
                if (llen < 255) line[llen++] = c;
            }
        }
    }
    /* flush any remaining line without trailing newline */
    if (llen > 0) {
        line[llen] = '\0';
        md_render_inline(line, llen);
        kprintf("\r\n");
    }
    vga_reset_attr();
    sh_close(fd);
}

static void cmd_cat(const char *name) {
    /* Auto-detect .md / .MD extension and use the markdown renderer */
    const char *dot = (const char*)0;
    for (const char *p = name; *p; p++) if (*p == '.') dot = p;
    if (dot && (
            (dot[1]=='m'||dot[1]=='M') &&
            (dot[2]=='d'||dot[2]=='D') &&
            dot[3]=='\0')) {
        cmd_mdcat(name);
        return;
    }
    i64 fd = sh_open(name);
    if (fd < 0) { kprintf("File not found.\r\n"); return; }
    for (;;) {
        i64 n = sh_read(fd, cat_buf, 512);
        if (n <= 0) break;
        cat_buf[n] = 0;
        kprintf("%s", (char*)cat_buf);
    }
    sh_close(fd); kprintf("\r\n");
}

static void cmd_hexdump(const char *name) {
    i64 fd = sh_open(name);
    if (fd < 0) { kprintf("File not found.\r\n"); return; }
    int col = 0;
    for (;;) {
        i64 n = sh_read(fd, cat_buf, 512);
        if (n <= 0) break;
        for (i64 i = 0; i < n; i++) {
            print_hex_byte(cat_buf[i]); vga_putchar(' ');
            if (++col == 16) { col = 0; kprintf("\r\n"); }
        }
    }
    if (col) kprintf("\r\n");
    sh_close(fd);
}

static void cmd_touch(const char *name) {
    i64 fd = sh_open(name);
    if (fd >= 0) { sh_close(fd); return; }
    fd = sh_create(name, 0644);
    if (fd >= 0) sh_close(fd);
    kprintf("File created.\r\n");
}

/* mktxt FILE1 FILE2 FILE3 ...
 * Creates any number of empty text or markdown files in one command.
 * Names are space-separated; each is auto-suffixed with .TXT if
 * no extension is given.  Use .md suffix for markdown files.
 * All names are uppercased to satisfy FAT32 8.3 rules. */
static void cmd_mktxt(const char *arg) {
    if (!arg || !arg[0]) {
        kprintf("Usage: mktxt <name1> [name2] [name3] ...\r\n");
        kprintf("  Creates one or more empty text/markdown files.\r\n");
        kprintf("  No extension = .TXT added automatically.\r\n");
        kprintf("  Use .md suffix for markdown:  mktxt README.md NOTES.md\r\n");
        return;
    }

    int created = 0, skipped = 0;
    const char *p = arg;

    while (p && *p) {
        /* skip leading spaces */
        while (*p == ' ') p++;
        if (!*p) break;

        /* copy one token into name[] */
        char name[16];
        int ni = 0;
        while (*p && *p != ' ' && ni < 15) name[ni++] = *p++;
        name[ni] = '\0';
        if (!ni) continue;

        /* uppercase the whole name */
        for (int i = 0; i < ni; i++) {
            if (name[i] >= 'a' && name[i] <= 'z')
                name[i] = (char)(name[i] - 32);
        }

        /* auto-append .TXT if there is no dot in the name */
        int has_dot = 0;
        for (int i = 0; i < ni; i++) if (name[i] == '.') { has_dot = 1; break; }
        if (!has_dot && ni <= 8) {
            /* room for .TXT (4 chars) */
            name[ni++] = '.';
            name[ni++] = 'T';
            name[ni++] = 'X';
            name[ni++] = 'T';
            name[ni]   = '\0';
        }

        /* create only if it doesn't already exist */
        i64 fd = sh_open(name);
        if (fd >= 0) {
            sh_close(fd);
            kprintf("  skip  %s  (already exists)\r\n", name);
            skipped++;
        } else {
            fd = sh_create(name, 0644);
            if (fd >= 0) {
                sh_close(fd);
                kprintf("  ok    %s\r\n", name);
                created++;
            } else {
                kprintf("  fail  %s\r\n", name);
            }
        }
    }
    kprintf("%d created, %d skipped.\r\n", created, skipped);
}

static void cmd_rm(const char *name) {
    if (sh_unlink(name) == 0)
        kprintf("File deleted.\r\n");
    else
        kprintf("File not found.\r\n");
}

static void cmd_rename(const char *arg) {
    const char *dst = get_arg(arg);
    if (!dst) { kprintf("Usage: rename <old> <new>\r\n"); return; }
    char old83[12], new83[12];
    format_83_name(arg, old83);
    format_83_name(dst, new83);
    char oldp[256], newp[256];
    sh_fullpath(old83, oldp, sizeof(oldp));
    sh_fullpath(new83, newp, sizeof(newp));
    /* Derive dir cluster from path for FAT32 rename */
    i64 rr = fat32_rename_in(old83, new83, cwd_active());
    if (rr == 0)
        kprintf("Renamed.\r\n");
    else
        kprintf("rename: %s\r\n", strerror((int)rr));
}

static void cmd_write(const char *arg) {
    const char *text = get_arg(arg);
    if (!text) { kprintf("Usage: write <file> <text>\r\n"); return; }
    i64 fd = sh_open(arg);
    if (fd < 0) fd = sh_create(arg, 0644);
    if (fd < 0) { kprintf("Error.\r\n"); return; }
    usize tlen = strlen(text);
    usize written = 0;
    while (written < tlen) {
        i64 n = sh_write(fd, text + written, tlen - written);
        if (n <= 0) break;
        written += (usize)n;
    }
    sh_write(fd, "\r\n", 2);
    sh_close(fd);
    kprintf("Written.\r\n");
}

/* ── Dispatch ─────────────────────────────────────────────────── */
/* ── network command helpers ─────────────────────────────────── */
static u32 parse_ip(const char *s) {
    u8 a=0,b=0,c=0,d=0; int n=0;
    for (; *s; s++) {
        if (*s=='.') { n++; }
        else if (*s>='0' && *s<='9') {
            u8 *oc = n==0?&a:n==1?&b:n==2?&c:&d;
            *oc = (u8)(*oc * 10 + (*s - '0'));
        }
    }
    return net_make_ip(a,b,c,d);
}

static void cmd_ifconfig(void) {
    if (!net_ready) { kprintf("eth0: not ready\r\n"); return; }
    kprintf("eth0  MAC=");
    for (int i = 0; i < 6; i++) {
        print_hex_byte(net_mac[i]);
        if (i < 5) kprintf(":");
    }
    kprintf("\r\n      IP=");
    if (net_ip) net_print_ip(net_ip);
    else kprintf("(none)");
    kprintf("  GW=");
    if (net_gateway) net_print_ip(net_gateway);
    else kprintf("(none)");
    kprintf("  DNS=10.0.2.3\r\n");
}

static void cmd_ping(const char *arg) {
    if (!arg) { kprintf("Usage: ping <ip or hostname>\r\n"); return; }
    kprintf("Resolving "); kprintf("%s", arg); kprintf("...\r\n");
    u32 ip = net_dns_resolve(arg);
    if (!ip) { kprintf("DNS failed\r\n"); return; }
    kprintf("PING "); net_print_ip(ip); kprintf(" ...\r\n");
    if (net_ping(ip)) kprintf("Reply received!\r\n");
    else              kprintf("Request timed out.\r\n");
}

static void cmd_wget(const char *arg) {
    if (!arg) { kprintf("Usage: wget <ip> <path> <outfile>\r\n"); return; }

    /* split "ip_str path outfile" by whitespace using strchr + isspace() */
    char ip_s[32] = {0}, path_s[128] = {0}, out_s[16] = {0};
    const char *p1 = strchr(arg, ' ');
    if (!p1) { kprintf("Usage: wget <ip> <path> <outfile>\r\n"); return; }
    strlcpy(ip_s, arg, MIN((usize)(p1 - arg) + 1, sizeof(ip_s)));
    while (isspace((unsigned char)*p1)) p1++;
    const char *p2 = strchr(p1, ' ');
    if (p2) {
        strlcpy(path_s, p1, MIN((usize)(p2 - p1) + 1, sizeof(path_s)));
        while (isspace((unsigned char)*p2)) p2++;
        strlcpy(out_s,  p2, sizeof(out_s));
    } else {
        strlcpy(path_s, p1, sizeof(path_s));
    }

    if (!ip_s[0] || !path_s[0]) {
        kprintf("Usage: wget <ip> <path> <outfile>\r\n"); return;
    }
    kprintf("Resolving "); kprintf("%s", ip_s); kprintf("...\r\n");
    u32 ip = net_dns_resolve(ip_s);
    if (!ip) { kprintf("DNS failed\r\n"); return; }
    kprintf("Connecting to "); net_print_ip(ip);
    kprintf("%s", path_s); kprintf("\r\n");
    static u8 dl_buf[32768];
    int n = net_http_get(ip, 80, path_s, dl_buf, sizeof(dl_buf));
    if (n < 0) { kprintf("wget: connection failed\r\n"); return; }
    kprintf("Downloaded "); kprintf("%d", (int)(n)); kprintf(" bytes\r\n");
    if (out_s[0]) {
        char name83[12];
        i64 fd = sh_open(out_s);
        if (fd < 0) fd = sh_create(out_s, 0644);
        if (fd >= 0) {
            sh_write(fd, dl_buf, (usize)n);
            sh_close(fd);
            kprintf("Saved to "); kprintf("%s", out_s); kprintf("\r\n");
        }
    }
}

static void cmd_run(const char *name) {
    format_83_name(name, name83);
    i64 fd = sh_open(name83);
    if (fd < 0) { kprintf("File not found.\r\n"); return; }
    void *buf = heap_malloc(524288);
    if (!buf) { sh_close(fd); kprintf("OOM.\r\n"); return; }
    usize total = 0;
    for (;;) { i64 n = sh_read(fd, (u8*)buf + total, 512); if (n <= 0) break; total += (usize)n; }
    sh_close(fd);
    i64 pid = process_create((u64)buf, name);
    heap_free(buf);
    if (pid < 0) { kprintf("Process create failed.\r\n"); return; }
    {
        u8 *pp = (u8*)PROC_TABLE;
        for (int i = 0; i < PROC_MAX; i++, pp += PROC_PCB_SIZE) {
            PCB *t = (PCB*)pp;
            if (t->pid == (u64)pid) {
                u64 stack_page = PROC_STACK_TOP - PAGE_SIZE;
                u64 phys = pmm_alloc();
                if (phys) { memset((void*)phys, 0, PAGE_SIZE); vmm_map(t->cr3, stack_page, phys, PTE_USER_RW); }
                break;
            }
        }
    }
    process_run((u64)pid);
}

/* ── GUI demo command ────────────────────────────────────────── */
static void cmd_gui(void) {
    kprintf("Launching Retro GUI...\r\n");

    /* Initialize the GUI system */
    gui_init();

    /* gui_init() now sets up the full desktop layout:
     *   - File Manager window (left half, VGA cols/rows)
     *   - Terminal window     (right half)
     *   - About modal dialog  (centred, on top)
     *   - Menu bar + Dock bar
     * All widget coordinates are in VGA character cells, not pixels.
     */

    kprintf("GUI running! Press ESC to exit...\r\n");

    /* Drain any stale keyboard/mouse data before entering event loop */
    for (int d = 0; d < 500; d++) {
        if (inb(0x64) & 1) { inb(0x60); }  /* consume whatever is pending */
    }

    /* Draw the GUI */
    gui_redraw();

    /* Event loop: exits ONLY on ESC keypress.
     *
     * i8042 status (port 0x64):
     *   bit 0 = output buffer full (data ready at 0x60)
     *   bit 5 = 1 -> byte is from MOUSE; 0 -> byte is from KEYBOARD
     *
     * Bug that was here before: reading 0x60 without checking bit 5
     * caused mouse-movement bytes (many fall in 0x01-0x7F) to be
     * mistaken for keyboard make-codes and close the GUI on any
     * mouse movement.  Always check bit 5 first.
     */
    /* Mouse packet state for GUI event loop */
    int  gui_mcycle  = 0;
    u8   gui_mpkt[3];
    int  lbtn_prev   = 0;   /* previous LEFT button state for edge detect */
    int  rbtn_prev   = 0;   /* previous RIGHT button state for edge detect */
    int  lbtn_curr   = 0;   /* current left-button state for dragging */

    for (;;) {
        /* Drain ALL pending i8042 bytes before sleeping.
         * Mouse sends 3 bytes back-to-back; if we hlt between them
         * we stall for a full timer tick and the cursor looks jerky. */
        int did_work = 0;
        while (1) {
            u8 st = inb(0x64);   /* read status first */
            if (!(st & 1)) break; /* no data ready */
            did_work = 1;
            u8 b = inb(0x60);    /* read data byte (clears OBF) */

            if (st & 0x20) {
                /* ---- Mouse byte ---- */
                /* Resync: byte 0 must have bit 3 set */
                if (gui_mcycle == 0 && !(b & 0x08)) continue;

                gui_mpkt[gui_mcycle++] = b;
                if (gui_mcycle < 3) continue;
                gui_mcycle = 0;

                u8 flags = gui_mpkt[0];
                if (flags & 0xC0) continue;   /* overflow — discard */

                /* Sign-extend 9-bit deltas */
                int dx =  (int)gui_mpkt[1] - ((flags & 0x10) ? 256 : 0);
                int dy = -((int)gui_mpkt[2] - ((flags & 0x20) ? 256 : 0));

                /* Scale: divide by 2 for smoother feel at 1024x768 */
                dx /= 2;
                dy /= 2;

                if (dx || dy) gui_cursor_move(dx, dy);

                /* Left button state */
                lbtn_curr = (flags & 0x01) ? 1 : 0;
                int rbtn_curr = (flags & 0x02) ? 1 : 0;

                /* Left button press (rising edge) → mouse down */
                if (lbtn_curr && !lbtn_prev) {
                    int cx, cy;
                    gui_cursor_get(&cx, &cy);
                    gui_handle_mouse_down(cx, cy);
                }

                /* Left button release (falling edge) → mouse up, stop drag+resize */
                if (!lbtn_curr && lbtn_prev) {
                    int cx, cy;
                    gui_cursor_get(&cx, &cy);
                    gui_handle_mouse_up(cx, cy);
                    gui_stop_drag();
                    gui_stop_resize();
                }

                /* Right button click (rising edge) — use rbtn_prev, not lbtn_prev */
                if (rbtn_curr && !rbtn_prev) {
                    int cx, cy;
                    gui_cursor_get(&cx, &cy);
                    gui_handle_right_click(cx, cy);
                }

                lbtn_prev = lbtn_curr;
                rbtn_prev = rbtn_curr;

                /* Feed input ring buffer */
                {
                    u8 ibtns = 0;
                    if (flags & 0x01) ibtns |= INPUT_BTN_LEFT;
                    if (flags & 0x02) ibtns |= INPUT_BTN_RIGHT;
                    if (flags & 0x04) ibtns |= INPUT_BTN_MIDDLE;
                    input_push_mouse(dx, dy, ibtns);
                }

            } else {
                /* ---- Keyboard byte ---- */
                if (b == 0x01) goto gui_exit;  /* ESC make */
                if (b == 0x81) continue;        /* ESC break — ignore */

                /* Full scancode-to-ASCII conversion for shell input */
                static u8 gui_shift = 0, gui_caps = 0, gui_ctrl = 0, gui_e0 = 0;

                if (b == 0xE0) { gui_e0 = 1; continue; }
                if (b == 0x2A || b == 0x36) { gui_shift = 1; gui_e0 = 0; input_mod_update(gui_shift,gui_ctrl,gui_caps); continue; }
                if (b == 0xAA || b == 0xB6) { gui_shift = 0; gui_e0 = 0; input_mod_update(gui_shift,gui_ctrl,gui_caps); continue; }
                if (b == 0x1D) { gui_ctrl = 1; gui_e0 = 0; input_mod_update(gui_shift,gui_ctrl,gui_caps); continue; }
                if (b == 0x9D) { gui_ctrl = 0; gui_e0 = 0; input_mod_update(gui_shift,gui_ctrl,gui_caps); continue; }
                if (b == 0x3A) { gui_caps ^= 1; gui_e0 = 0; input_mod_update(gui_shift,gui_ctrl,gui_caps); continue; }
                if (b == 0xBA) { gui_e0 = 0; continue; }
                if (b & 0x80) { input_key_release(); gui_e0 = 0; continue; } /* key release */
                if (gui_e0) {
                    gui_e0 = 0;
                    if (b == 0x48) { input_push_key(0x80|0x48, KEY_UP);    }
                    if (b == 0x50) { input_push_key(0x80|0x50, KEY_DOWN);  }
                    continue;
                }
                if (b >= 0x3A) continue;

                /* Use shared kb_translate() — eliminates the third copy of the
                 * scancode tables that previously lived here. */
                u8 ch = kb_translate(b, gui_shift, gui_caps);
                if (!ch) continue;

                input_push_key(b, (u8)ch);
                gui_shell_keypress((u8)ch);
            }
        }
        watchdog_pet();   /* GUI is alive — prevent watchdog firing */
        ps2_poll();       /* drain i8042 keyboard + mouse bytes */
        usb_full_poll();
        __asm__ volatile("hlt");
    }
    gui_exit:;

    kprintf("Exiting GUI, returning to shell...\r\n");
    gui_shutdown();

    /* Restore VGA text mode cursor */
    outb(0x3D4, 0x0A); outb(0x3D5, (inb(0x3D5) & 0xC0) | 14);
    outb(0x3D4, 0x0B); outb(0x3D5, (inb(0x3D5) & 0xE0) | 15);
    vga_clear();
}


static void cmd_elf(const char *name) {
    format_83_name(name, name83);
    i64 fd = sh_open(name83);
    if (fd < 0) { kprintf("File not found.\r\n"); return; }
    void *buf = heap_malloc(524288);
    if (!buf) { sh_close(fd); kprintf("OOM.\r\n"); return; }
    usize total = 0;
    for (;;) { i64 n = sh_read(fd, (u8*)buf + total, 512); if (n <= 0) break; total += (usize)n; }
    sh_close(fd);
    i64 pid = elf_load(buf, total, name);
    heap_free(buf);
    if (pid < 0) { kprintf("Bad ELF.\r\n"); return; }
    process_run((u64)pid);
}

static void exec_cmd(void) {
    /* skip leading whitespace using slibc_ltrim (returns ptr past spaces) */
    const char *s = slibc_ltrim((char *)input_buf);
    if (!*s) return;

#define CMD(name, body) \
    if (strncmp(s, name, sizeof(name)-1) == 0 && \
        (s[sizeof(name)-1] == ' ' || s[sizeof(name)-1] == 0)) { body; return; }

    /* --- system --- */
    CMD("clear",   vga_clear())
    CMD("reboot",  { kprintf("Rebooting...\r\n"); outb(0x64, 0xFE); cli(); hlt(); })
    CMD("halt",    { kprintf("System halted.\r\n"); cli(); hlt(); })
    CMD("ps",      ps_list())
    CMD("meminfo", cmd_meminfo())
    CMD("uptime",  cmd_uptime())
    CMD("uname",   cmd_uname())
    CMD("df",      cmd_df())
    CMD("help",    kprintf(
        "Files:\r\n"
        "  ls                   list directory\r\n"
        "  cd <dir>             change directory  (.. goes up)\r\n"
        "  pwd                  print working directory\r\n"
        "  stat <name>          file/dir info\r\n"
        "  cat <file>           print file\r\n"
        "  head <file> [n]      first N lines (default 10)\r\n"
        "  tail <file> [n]      last  N lines (default 10)\r\n"
        "  hexdump <file>       hex dump\r\n"
        "  wc <file>            word/line/byte count\r\n"
        "  find [pattern]       search filenames\r\n"
        "  df                   disk free\r\n"
        "\r\n"
        "  touch <file>         create empty file\r\n"
        "  mktxt <f1> [f2] ...  create multiple text files at once\r\n"
        "  write <file> <txt>   overwrite file\r\n"
        "  append <file> <txt>  append line to file\r\n"
        "  rm <file>            delete file\r\n"
        "  cp <src> <dst>       copy file\r\n"
        "  rename <old> <new>   rename file\r\n"
        "  mkdir <dir>          create directory\r\n"
        "  rmdir <dir>          remove empty directory\r\n"
        "\r\n"
        "Processes: elf <f>  run <f>  ps\r\n"
        "Network:   ifconfig  ping  wget  serve  netcat\r\n"
        "System:    meminfo  uname  uptime  df  clear  reboot  halt\r\n"
        "Math:      calc <n> <op> <n>     e.g.  calc 6 * 7\r\n"
        "GUI:       gui                launch retro GUI demo\r\n"\
        "           photo <f.png>        view PNG image (ESC to return)\r\n"
        "Display:   720p  1080p        switch resolution (while in GUI)\r\n"
        "Network:   lynx <url>        text-mode web browser\r\n"))

    /* --- file management --- */
    const char *arg = get_arg(s);
    CMD("echo",    { if (arg) { kprintf("%s", arg); kprintf("\r\n"); } else kprintf("\r\n"); })
    CMD("ls",      cmd_ls())
    CMD("pwd",     { kprintf("%s", cwd_path); kprintf("\r\n"); })
    CMD("cd",      { cmd_cd(arg); })
    CMD("stat",    { if (arg) cmd_stat(arg);    else kprintf("Usage: stat <name>\r\n"); })
    CMD("cat",     { if (arg) cmd_cat(arg);     else kprintf("Usage: cat <file>\r\n"); })
    CMD("head",    { if (arg) cmd_head(arg);    else kprintf("Usage: head <file> [n]\r\n"); })
    CMD("tail",    { if (arg) cmd_tail(arg);    else kprintf("Usage: tail <file> [n]\r\n"); })
    CMD("hexdump", { if (arg) cmd_hexdump(arg); else kprintf("Usage: hexdump <file>\r\n"); })
    CMD("wc",      { if (arg) cmd_wc(arg);      else kprintf("Usage: wc <file>\r\n"); })
    CMD("find",    { cmd_find(arg); })
    CMD("touch",   { if (arg) cmd_touch(arg);   else kprintf("Usage: touch <file>\r\n"); })
    CMD("mktxt",   { cmd_mktxt(arg); })
    CMD("write",   { if (arg) cmd_write(arg);   else kprintf("Usage: write <file> <text>\r\n"); })
    CMD("append",  { if (arg) cmd_append(arg);  else kprintf("Usage: append <file> <text>\r\n"); })
    CMD("rm",      { if (arg) cmd_rm(arg);      else kprintf("Usage: rm <file>\r\n"); })
    CMD("cp",      { if (arg) cmd_cp(arg);      else kprintf("Usage: cp <src> <dst>\r\n"); })
    CMD("rename",  { if (arg) cmd_rename(arg);  else kprintf("Usage: rename <old> <new>\r\n"); })
    CMD("mkdir",   { if (arg) cmd_mkdir(arg);   else kprintf("Usage: mkdir <dir>\r\n"); })
    CMD("rmdir",   { if (arg) cmd_rmdir(arg);   else kprintf("Usage: rmdir <dir>\r\n"); })

    /* --- processes --- */
    CMD("run",     { if (arg) cmd_run(arg);     else kprintf("Usage: run <file>\r\n"); })
    CMD("elf",     { if (arg) cmd_elf(arg);     else kprintf("Usage: elf <file>\r\n"); })
    CMD("calc",    { cmd_calc(arg); })
    CMD("photo",   { if (arg) cmd_photo(arg); else kprintf("Usage: photo <file.png>\r\n"); })

#undef CMD

    /* --- networking (manual dispatch for multi-word arg parsing) --- */
    if (strncmp(s, "ifconfig", 8)==0 && (s[8]==' '||s[8]==0)) { cmd_ifconfig(); return; }
    if (strncmp(s, "gui",      3)==0 && (s[3]==' '||s[3]==0)) { cmd_gui();      return; }

    /* Resolution switching */
    if (strncmp(s, "720p", 4)==0 && (s[4]==' '||s[4]==0)) {
        if (!fb_is_enabled()) { kprintf("GUI not active. Run 'gui' first.\r\n"); return; }
        if (fb_set_resolution(1280, 720))
            kprintf("Resolution set to 1280x720 (720p).\r\n");
        else
            kprintf("720p not supported by display.\r\n");
        return;
    }
    if (strncmp(s, "1080p", 5)==0 && (s[5]==' '||s[5]==0)) {
        if (!fb_is_enabled()) { kprintf("GUI not active. Run 'gui' first.\r\n"); return; }
        if (fb_set_resolution(1920, 1080))
            kprintf("Resolution set to 1920x1080 (1080p).\r\n");
        else
            kprintf("1080p not supported by display.\r\n");
        return;
    }

    if (strncmp(s, "ping",     4)==0 && (s[4]==' '||s[4]==0)) { cmd_ping(arg);  return; }
    if (strncmp(s, "wget",     4)==0 && (s[4]==' '||s[4]==0)) { cmd_wget(arg);  return; }
    if (strncmp(s, "serve",    5)==0 && (s[5]==' '||s[5]==0)) {
        u16 port = 80;
        if (arg) { port = 0; for (const char *p=arg; *p>='0'&&*p<='9'; p++) port=(u16)(port*10+(*p-'0')); }
        net_http_serve(port); return;
    }
    if (strncmp(s, "netcat", 6)==0 && (s[6]==' '||s[6]==0)) {
        u16 port = 4444;
        if (arg) { port = 0; for (const char *p=arg; *p>='0'&&*p<='9'; p++) port=(u16)(port*10+(*p-'0')); }
        if (!net_tcp_listen(port)) { kprintf("netcat: bind failed\r\n"); return; }
        kprintf("netcat: waiting on port %u (any key to cancel)\r\n", (unsigned)port);
        TcpConn *c = NULL;
        for (;;) { net_poll(); if (inb(0x64)&1){inb(0x60);net_tcp_unlisten(port);return;} c=net_tcp_accept_nb(port); if(c) break; for(volatile int _d=0;_d<5000;_d++) __asm__ volatile("pause"); }
        kprintf("netcat: connected\r\n");
        static u8 nc_buf[512];
        for (;;) { int n=net_tcp_recv(c,nc_buf,sizeof(nc_buf)-1); if(n<=0) break; nc_buf[n]=0; kprintf("%s", (char*)nc_buf); if(inb(0x64)&1){inb(0x60);break;} }
        net_tcp_close(c); net_tcp_unlisten(port);
        kprintf("\r\nnetcat: done.\r\n"); return;
    }

    kprintf("Unknown command. Try 'help'.\r\n");
}

/* ================================================================
 *  KERNEL ENTRY POINT  (called from entry.S)
 * ================================================================ */
static void kbd_init_dummy(void) {}  /* polled I/O needs no init */

void kernel_main(void) {
    /* Enable VGA hardware cursor (scanlines 14–15 = underline style) */
    outb(0x3D4, 0x0A); outb(0x3D5, (inb(0x3D5) & 0xC0) | 14);
    outb(0x3D4, 0x0B); outb(0x3D5, (inb(0x3D5) & 0xE0) | 15);
    vga_clear();

    heap_init();
    pmm_init();
    vmm_init_kernel();
    tss_init();        /* must run before STI — sets RSP0 for ring-3 exceptions */
    syscall_init();
    ipc_init();
    memset((void*)PROC_TABLE, 0, PROC_PCB_SIZE * PROC_MAX);
    process_init();
    scheduler_init();

    vfs_init();
    fat32_init();
    vfs_core_init();       /* zero inode/mount/fd tables for new VFS layer */
    jfs_init();            /* init JFS on second partition */
    vfs_register_fat32();  /* mount FAT32 at /    via inode_ops_t */
    vfs_register_jfs();    /* mount JFS   at /jfs via inode_ops_t */
    net_start();          /* init e1000, run DHCP */

    /* Drain any stale i8042 bytes that accumulated during network init.
     * Without this, DHCP broadcast packets can cause phantom keypresses. */
    { int t = 1000; while ((inb(0x64) & 1) && --t) inb(0x60); }

    pci_scan_all();           /* enumerate all PCI devices — must run first */

    /* ── AHCI SATA (class 0x01, subclass 0x06, prog-if 0x01) ── */
    {
        void *ahci_dev = pci_find_class_progif(0x01, 0x06, 0x01);
        if (ahci_dev) {
            u32 bar5 = (u32)pci_bar_base(ahci_dev, 5);
            ahci_init(bar5);
        } else {
            kprintf("[AHCI] no SATA controller found\r\n");
        }
    }

    /* ── NVMe (class 0x01, subclass 0x08, prog-if 0x02) ── */
    {
        void *nvme_dev = pci_find_class_progif(0x01, 0x08, 0x02);
        if (nvme_dev) {
            u64 bar0 = pci_bar_base(nvme_dev, 0);
            nvme_init(bar0);
        } else {
            kprintf("[NVMe] no NVMe controller found\r\n");
        }
    }

    ps2_init();               /* full i8042 controller init — KB + mouse */
    kbd_init_dummy();         /* polled I/O — no keyboard init needed */
    mouse_ready = ps2_mouse_ok();
    if (mouse_ready) ps2_mouse_refresh();
    scheduler_start();    /* STI — timer fires from here */
    usb_full_init();      /* EHCI + XHCI full enumeration + mass storage */

    smp_init();
    watchdog_init();

    kprintf("========================================\r\n  Systrix v0.1\r\n  x86-64 | preemptive | FAT32 | ring-3\r\n  Type 'help' for available commands.\r\n========================================\r\n");

    for (;;) {
        watchdog_pet();   /* kernel idle/shell is alive */
        kprintf("systrix:");
        kprintf("%s", cwd_path);
        kprintf("$ ");
        read_line(input_buf, sizeof(input_buf));
        watchdog_pet();   /* pet again after potentially long input wait */
        exec_cmd();
    }
}
