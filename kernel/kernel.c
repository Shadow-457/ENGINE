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
    vga_update_hw_cursor();
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

void vga_putchar(u8 c) {
    if (c == '\r') { cur_col = 0; vga_update_hw_cursor(); return; }

    if (c == '\n') {
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

    /* Normal character — write to back-buffer current row */
    back_buf[buf_next][cur_col] = (u16)(VGA_ATTR << 8) | c;

    /* Write to VGA only if live */
    if (scroll_offset == 0)
        ((u16*)VGA_BASE)[cur_row * VGA_COLS + cur_col] =
            (u16)(VGA_ATTR << 8) | c;

    if (++cur_col < VGA_COLS) { vga_update_hw_cursor(); return; }
    /* Wrap column → implicit newline */
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
    u16 blank = (u16)(VGA_ATTR << 8) | ' ';
    back_buf[buf_next][cur_col] = blank;
    if (scroll_offset == 0)
        ((u16*)VGA_BASE)[cur_row * VGA_COLS + cur_col] = blank;
    vga_update_hw_cursor();
}

void print_str(const char *s) {
    while (*s) vga_putchar((u8)*s++);
}

void print_hex_byte(u8 v) {
    static const char hex[] = "0123456789ABCDEF";
    vga_putchar((u8)hex[v >> 4]);
    vga_putchar((u8)hex[v & 0xF]);
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

void fat32_create_file_in(const char *name83, u32 dir_clus) {
    u8 de[32]; memset(de, 0, 32);
    memcpy(de, name83, 11);
    de[11] = 0x20; /* Archive */
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
    if (caps && ch >= 'a' && ch <= 'z') ch = (u8)(ch - 32);
    else if (caps && ch >= 'A' && ch <= 'Z') ch = (u8)(ch + 32);
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
        u8 c = (u8)src[i];
        dst[j] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : (char)c;
    }
    /* skip past dot */
    while (src[i] && src[i] != '.') i++;
    if (src[i] == '.') {
        i++;
        /* extension (up to 3) */
        for (int k = 0; k < 3 && src[i] && src[i] != ' '; i++, k++) {
            u8 c = (u8)src[i];
            dst[8 + k] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : (char)c;
        }
    }
}

/* ================================================================
 *  SHELL COMMANDS
 * ================================================================ */
static char input_buf[128];
static char name83[12];
static u8   cat_buf[513];

/* Current working directory cluster (0 = root) */
static u32  cwd_clus = 0;
static char cwd_path[128] = "/";

/* Skip the first word in s, return pointer to start of next word (or NULL) */
static const char *get_arg(const char *s) {
    while (*s && *s != ' ') s++;
    while (*s == ' ') s++;
    return *s ? s : NULL;
}

/* Print an unsigned 32-bit integer in decimal */
static void print_u32(u32 n) {
    if (n == 0) { vga_putchar('0'); return; }
    char buf[10]; int i = 0;
    while (n) { buf[i++] = '0' + (n % 10); n /= 10; }
    while (i--) vga_putchar((u8)buf[i]);
}

/* Print size in human-readable form: B, KB, MB */
static void print_size_human(u32 sz) {
    if (sz < 1024) {
        print_u32(sz); print_str(" B");
    } else if (sz < 1024 * 1024) {
        print_u32(sz / 1024); print_str(".");
        print_u32((sz % 1024) * 10 / 1024); print_str(" KB");
    } else {
        print_u32(sz / (1024*1024)); print_str(".");
        print_u32((sz % (1024*1024)) * 10 / (1024*1024)); print_str(" MB");
    }
}

/* Print right-aligned decimal in `width` chars */
static void print_u32_w(u32 n, int width) {
    char buf[12]; int i = 0;
    if (n == 0) buf[i++] = '0';
    else { u32 t = n; while (t) { buf[i++] = '0' + (t % 10); t /= 10; } }
    for (int k = i; k < width; k++) vga_putchar(' ');
    while (i--) vga_putchar((u8)buf[i]);
}

/* Active cluster for directory ops — root if cwd_clus == 0 */
static u32 cwd_active(void) {
    return (cwd_clus == 0) ? fat32_root_clus : cwd_clus;
}

static void cmd_meminfo(void) {
    u32 free  = pmm_free_pages();
    u32 total = (u32)TOTAL_PAGES;
    u32 used  = total - free;
    print_str("Memory (4 KB pages):\r\n");
    print_str("  Total : "); print_u32(total); print_str(" pages (");
    print_u32(total * 4); print_str(" KB)\r\n");
    print_str("  Used  : "); print_u32(used);  print_str(" pages (");
    print_u32(used  * 4); print_str(" KB)\r\n");
    print_str("  Free  : "); print_u32(free);  print_str(" pages (");
    print_u32(free  * 4); print_str(" KB)\r\n");
}

static void cmd_uptime(void) {
    u32 secs  = (u32)(pit_ticks / 1000);
    u32 mins  = secs / 60;  secs %= 60;
    u32 hours = mins / 60;  mins %= 60;
    print_str("Uptime: ");
    print_u32(hours); print_str("h ");
    print_u32(mins);  print_str("m ");
    print_u32(secs);  print_str("s\r\n");
}

static void cmd_uname(void) {
    print_str("ENGINE 0.1 x86-64 (preemptive, FAT32, ring-3 ELF)\r\n");
}

/* ================================================================
 *  FILE MANAGEMENT COMMANDS
 * ================================================================ */

/* ---- ls -------------------------------------------------------- */
static void cmd_ls(void) {
    u32 clus = cwd_active();
    int file_count = 0, dir_count = 0;
    u32 total_bytes = 0;

    /* Header */
    print_str("Name           Type       Size\r\n");
    print_str("----           ----       ----\r\n");

    while (clus < 0x0FFFFFF8) {
        u32 lba = cluster_to_lba(clus);
        for (u8 s = 0; s < fat32_spc; s++) {
            ata_read_sector(lba + s, dir_buf);
            for (int e = 0; e < 16; e++) {
                u8 *de = dir_buf + e * 32;
                if (de[0] == 0x00) goto ls_done;
                if (de[0] == 0xE5) continue;
                u8 attr = de[11];
                if (attr == 0x0F) continue;  /* LFN entry */
                if (attr & 0x08) continue;   /* volume label */
                if (attr & 0x02) continue;   /* hidden */
                /* skip . and .. */
                if (de[0] == '.') continue;

                /* format 8.3 name */
                char name[13]; int ni = 0;
                for (int k = 0; k < 8 && de[k] != ' '; k++)
                    name[ni++] = (char)de[k];
                if (de[8] != ' ') {
                    name[ni++] = '.';
                    for (int k = 8; k < 11 && de[k] != ' '; k++)
                        name[ni++] = (char)de[k];
                }
                name[ni] = 0;

                int is_dir = (attr & 0x10) ? 1 : 0;

                /* print name, padded to 15 */
                for (int k = 0; k < ni; k++) vga_putchar((u8)name[k]);
                if (is_dir) vga_putchar('/');
                int pad = 15 - ni - is_dir;
                for (int k = 0; k < pad; k++) vga_putchar(' ');

                if (is_dir) {
                    print_str("<DIR>      ");
                    print_str("     -\r\n");
                    dir_count++;
                } else {
                    u32 sz = *(u32*)(de + 28);
                    print_str("file       ");
                    print_u32_w(sz, 6);
                    print_str(" B\r\n");
                    total_bytes += sz;
                    file_count++;
                }
            }
        }
        clus = fat32_next_cluster(clus);
    }
ls_done:
    print_str("----           ----       ----\r\n");
    print_u32(file_count); print_str(" file(s), ");
    print_u32(dir_count);  print_str(" dir(s)   total: ");
    print_size_human(total_bytes);
    print_str("\r\n");
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
                int pl = 0; while (cwd_path[pl]) pl++;
                if (cwd_path[pl-1] != '/') { cwd_path[pl] = '/'; pl++; }
                for (int k = 0; name[k] && pl < 126; k++, pl++) cwd_path[pl] = name[k];
                cwd_path[pl] = 0;
                return;
            }
        }
        clus = fat32_next_cluster(clus);
    }
cd_notfound:
    print_str("cd: no such directory\r\n");
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

                /* found — print details */
                print_str("File : "); print_str(name); print_str("\r\n");
                print_str("Type : "); print_str((attr & 0x10) ? "Directory" : "File"); print_str("\r\n");
                if (!(attr & 0x10)) {
                    u32 sz = *(u32*)(de + 28);
                    print_str("Size : "); print_u32(sz); print_str(" B (");
                    print_size_human(sz); print_str(")\r\n");
                }
                u32 hi = *(u16*)(de + 20);
                u32 lo = *(u16*)(de + 26);
                u32 fc = (hi << 16) | lo;
                print_str("Clus : "); print_u32(fc); print_str("\r\n");
                /* FAT date/time (creation) */
                u16 cdate = *(u16*)(de + 16);
                u16 ctime = *(u16*)(de + 14);
                u32 year  = 1980 + ((cdate >> 9) & 0x7F);
                u32 mon   = (cdate >> 5) & 0xF;
                u32 day   = cdate & 0x1F;
                u32 hour  = (ctime >> 11) & 0x1F;
                u32 min   = (ctime >> 5) & 0x3F;
                print_str("Date : "); print_u32(year); print_str("-");
                if (mon  < 10) vga_putchar('0');
                print_u32(mon); print_str("-");
                if (day  < 10) vga_putchar('0');
                print_u32(day); print_str("  ");
                if (hour < 10) vga_putchar('0');
                print_u32(hour); print_str(":");
                if (min  < 10) vga_putchar('0');
                print_u32(min);
                print_str("\r\n");
                u8 ro = (attr & 0x01) ? 1 : 0;
                print_str("Attr : "); print_str(ro ? "read-only" : "read-write");
                print_str("\r\n");
                return;
            }
        }
        clus = fat32_next_cluster(clus);
    }
stat_notfound:
    print_str("stat: not found\r\n");
}

/* ---- df -------------------------------------------------------- */
static void cmd_df(void) {
    /* Use the cluster count computed at mount time (fat32_total_clus)
     * instead of re-reading and re-parsing the BPB every time df runs.
     * Also use the FAT sector cache (fat_read_cached) so repeated calls
     * don't hammer the disk with redundant sector reads. */
    u32 total_clus = fat32_total_clus;
    if (total_clus == 0) { print_str("df: filesystem not mounted\r\n"); return; }

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

    print_str("Filesystem : FAT32\r\n");
    print_str("Total      : "); print_size_human(total_bytes); print_str("\r\n");
    print_str("Used       : "); print_size_human(used_bytes);  print_str("\r\n");
    print_str("Free       : "); print_size_human(free_bytes);  print_str("\r\n");
    print_str("Clusters   : "); print_u32(free_clus); print_str(" free / ");
    print_u32(total_clus); print_str(" total\r\n");
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
                            char a = name[k+pi];
                            char b = pattern[pi];
                            if (a >= 'A' && a <= 'Z') a += 32;
                            if (b >= 'A' && b <= 'Z') b += 32;
                            if (a != b) break;
                            pi++;
                        }
                        if (!pattern[pi]) match = 1;
                    }
                    if (!match) continue;
                }

                print_str(cwd_path);
                if (cwd_path[1] != 0) vga_putchar('/');  /* not root */
                print_str(name);
                if (attr & 0x10) vga_putchar('/');
                print_str("\r\n");
                found++;
            }
        }
        clus = fat32_next_cluster(clus);
    }
find_done:
    if (!found) print_str("find: no matches\r\n");
    else { print_u32((u32)found); print_str(" match(es)\r\n"); }
}

/* ---- head / tail ----------------------------------------------- */
static void cmd_head(const char *arg) {
    /* head <file> [n]  — default 10 lines */
    const char *a2 = get_arg(arg);
    int lines = 10;
    if (a2) {
        lines = 0;
        for (; *a2 >= '0' && *a2 <= '9'; a2++) lines = lines * 10 + (*a2 - '0');
        if (!lines) lines = 10;
    }
    char n83[12]; format_83_name(arg, n83);
    i64 fd = vfs_open_in(n83, cwd_active());
    if (fd < 0) { print_str("head: file not found\r\n"); return; }
    int lc = 0;
    for (;;) {
        i64 n = vfs_read((u64)fd, cat_buf, 512);
        if (n <= 0) break;
        for (i64 i = 0; i < n && lc < lines; i++) {
            vga_putchar(cat_buf[i]);
            if (cat_buf[i] == '\n') lc++;
        }
        if (lc >= lines) break;
    }
    vfs_close((u64)fd);
    print_str("\r\n");
}

static void cmd_tail(const char *arg) {
    /* tail <file> [n]  — default 10 lines, loads full file into heap */
    const char *a2 = get_arg(arg);
    int lines = 10;
    if (a2) {
        lines = 0;
        for (; *a2 >= '0' && *a2 <= '9'; a2++) lines = lines * 10 + (*a2 - '0');
        if (!lines) lines = 10;
    }
    char n83[12]; format_83_name(arg, n83);
    i64 fd = vfs_open_in(n83, cwd_active());
    if (fd < 0) { print_str("tail: file not found\r\n"); return; }

    /* read full file */
    u8 *buf = (u8*)heap_malloc(32768);
    if (!buf) { vfs_close((u64)fd); print_str("OOM\r\n"); return; }
    usize total = 0;
    for (;;) {
        i64 n = vfs_read((u64)fd, buf + total, 512);
        if (n <= 0) break;
        total += (usize)n;
        if (total >= 32768 - 512) break;
    }
    vfs_close((u64)fd);

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
    print_str("\r\n");
}

/* ---- append ---------------------------------------------------- */
static void cmd_append(const char *arg) {
    const char *text = get_arg(arg);
    if (!text) { print_str("Usage: append <file> <text>\r\n"); return; }
    char n83[12]; format_83_name(arg, n83);
    u32 dummy;
    if (fat32_find_file_in(n83, &dummy, cwd_active()) < 0)
        fat32_create_file_in(n83, cwd_active());
    i64 fd = vfs_open_in(n83, cwd_active());
    if (fd < 0) { print_str("Error.\r\n"); return; }
    FD *slot = &fd_table[fd];
    slot->pos = slot->size;
    usize tlen = strlen(text);
    usize written = 0;
    while (written < tlen) {
        i64 n = vfs_write((u64)fd, text + written, tlen - written);
        if (n <= 0) break;
        written += (usize)n;
    }
    vfs_write((u64)fd, "\r\n", 2);
    vfs_close((u64)fd);
    print_str("Appended.\r\n");
}

/* ---- mkdir / rmdir --------------------------------------------- */
static void cmd_mkdir(const char *name) {
    u32 c = fat32_alloc_cluster();
    if (c == (u32)-1) { print_str("mkdir: disk full\r\n"); return; }
    u32 lba = cluster_to_lba(c);
    u8 zbuf[512]; memset(zbuf, 0, 512);
    for (u8 s = 0; s < fat32_spc; s++) ata_write_sector(lba + s, zbuf);
    char n83[12]; format_83_name(name, n83);
    fat32_create_dir_entry_in(n83, c, cwd_active());
    print_str("Directory created.\r\n");
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
    if (dir_clus == (u32)-1) { print_str("rmdir: not found\r\n"); return; }

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
                    print_str("rmdir: directory not empty\r\n");
                    return;
                }
            }
        }
        dc = fat32_next_cluster(dc);
    }
rmdir_check_done:
    /* delete via fat32_delete_file (marks entry 0xE5, frees chain) */
    if (fat32_delete_file_in(n83, cwd_active()) == 0)
        print_str("Directory removed.\r\n");
    else
        print_str("rmdir: failed\r\n");
}

/* ---- wc -------------------------------------------------------- */
static void cmd_wc(const char *name) {
    char n83[12]; format_83_name(name, n83);
    i64 fd = vfs_open_in(n83, cwd_active());
    if (fd < 0) { print_str("wc: file not found\r\n"); return; }
    u32 lines = 0, words = 0, bytes = 0;
    int in_word = 0;
    for (;;) {
        i64 n = vfs_read((u64)fd, cat_buf, 512);
        if (n <= 0) break;
        for (i64 i = 0; i < n; i++) {
            u8 c = cat_buf[i];
            bytes++;
            if (c == '\n') lines++;
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') in_word = 0;
            else if (!in_word) { in_word = 1; words++; }
        }
    }
    vfs_close((u64)fd);
    print_u32(lines); print_str(" lines  ");
    print_u32(words); print_str(" words  ");
    print_u32(bytes); print_str(" bytes\r\n");
}

/* ---- cp -------------------------------------------------------- */
static void cmd_cp(const char *arg) {
    const char *dst = get_arg(arg);
    if (!dst) { print_str("Usage: cp <src> <dst>\r\n"); return; }
    char src83[12], dst83[12];
    format_83_name(arg, src83);
    format_83_name(dst, dst83);

    i64 fd_in = vfs_open_in(src83, cwd_active());
    if (fd_in < 0) { print_str("cp: source not found\r\n"); return; }
    u32 dummy;
    if (fat32_find_file_in(dst83, &dummy, cwd_active()) < 0)
        fat32_create_file_in(dst83, cwd_active());
    i64 fd_out = vfs_open_in(dst83, cwd_active());
    if (fd_out < 0) { vfs_close((u64)fd_in); print_str("cp: cannot create dest\r\n"); return; }

    static u8 cp_buf[512];
    u32 total = 0;
    i64 n;
    while ((n = vfs_read((u64)fd_in, cp_buf, sizeof(cp_buf))) > 0) {
        vfs_write((u64)fd_out, cp_buf, (usize)n);
        total += (u32)n;
    }
    vfs_close((u64)fd_in);
    vfs_close((u64)fd_out);
    print_str("Copied "); print_size_human(total); print_str("\r\n");
}

/* ---- cat / hexdump / touch / rm / rename / write (unchanged) --- */
static void cmd_cat(const char *name) {
    format_83_name(name, name83);
    i64 fd = vfs_open_in(name83, cwd_active());
    if (fd < 0) { print_str("File not found.\r\n"); return; }
    for (;;) {
        i64 n = vfs_read((u64)fd, cat_buf, 512);
        if (n <= 0) break;
        cat_buf[n] = 0;
        print_str((char*)cat_buf);
    }
    vfs_close((u64)fd); print_str("\r\n");
}

static void cmd_hexdump(const char *name) {
    format_83_name(name, name83);
    i64 fd = vfs_open_in(name83, cwd_active());
    if (fd < 0) { print_str("File not found.\r\n"); return; }
    int col = 0;
    for (;;) {
        i64 n = vfs_read((u64)fd, cat_buf, 512);
        if (n <= 0) break;
        for (i64 i = 0; i < n; i++) {
            print_hex_byte(cat_buf[i]); vga_putchar(' ');
            if (++col == 16) { col = 0; print_str("\r\n"); }
        }
    }
    if (col) print_str("\r\n");
    vfs_close((u64)fd);
}

static void cmd_touch(const char *name) {
    format_83_name(name, name83);
    u32 dummy;
    if (fat32_find_file_in(name83, &dummy, cwd_active()) == 0)
        { print_str("(exists)\r\n"); return; }
    fat32_create_file_in(name83, cwd_active());
    print_str("File created.\r\n");
}

static void cmd_rm(const char *name) {
    format_83_name(name, name83);
    if (fat32_delete_file_in(name83, cwd_active()) == 0)
        print_str("File deleted.\r\n");
    else
        print_str("File not found.\r\n");
}

static void cmd_rename(const char *arg) {
    const char *dst = get_arg(arg);
    if (!dst) { print_str("Usage: rename <old> <new>\r\n"); return; }
    char old83[12], new83[12];
    format_83_name(arg, old83);
    format_83_name(dst, new83);
    if (fat32_rename_in(old83, new83, cwd_active()) == 0)
        print_str("Renamed.\r\n");
    else
        print_str("Rename failed.\r\n");
}

static void cmd_write(const char *arg) {
    const char *text = get_arg(arg);
    if (!text) { print_str("Usage: write <file> <text>\r\n"); return; }
    format_83_name(arg, name83);
    u32 tmp;
    if (fat32_find_file_in(name83, &tmp, cwd_active()) != 0)
        fat32_create_file_in(name83, cwd_active());
    i64 fd = vfs_open_in(name83, cwd_active());
    if (fd < 0) { print_str("Error.\r\n"); return; }
    usize tlen = strlen(text);
    usize written = 0;
    while (written < tlen) {
        i64 n = vfs_write((u64)fd, text + written, tlen - written);
        if (n <= 0) break;
        written += (usize)n;
    }
    vfs_write((u64)fd, "\r\n", 2);
    vfs_close((u64)fd);
    print_str("Written.\r\n");
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

static void print_int(int v) {
    char buf[12]; int i=0;
    if (!v) { vga_putchar('0'); return; }
    while (v) { buf[i++] = (char)('0' + v%10); v /= 10; }
    while (i--) vga_putchar((u8)buf[i]);
}

static void cmd_ifconfig(void) {
    if (!net_ready) { print_str("eth0: not ready\r\n"); return; }
    print_str("eth0  MAC=");
    for (int i = 0; i < 6; i++) {
        print_hex_byte(net_mac[i]);
        if (i < 5) print_str(":");
    }
    print_str("\r\n      IP=");
    if (net_ip) net_print_ip(net_ip);
    else print_str("(none)");
    print_str("  GW=");
    if (net_gateway) net_print_ip(net_gateway);
    else print_str("(none)");
    print_str("  DNS=10.0.2.3\r\n");
}

static void cmd_ping(const char *arg) {
    if (!arg) { print_str("Usage: ping <ip or hostname>\r\n"); return; }
    print_str("Resolving "); print_str(arg); print_str("...\r\n");
    u32 ip = net_dns_resolve(arg);
    if (!ip) { print_str("DNS failed\r\n"); return; }
    print_str("PING "); net_print_ip(ip); print_str(" ...\r\n");
    if (net_ping(ip)) print_str("Reply received!\r\n");
    else              print_str("Request timed out.\r\n");
}

static void cmd_wget(const char *arg) {
    if (!arg) { print_str("Usage: wget <ip> <path> <outfile>\r\n"); return; }
    char ip_s[32]={0}, path_s[128]={0}, out_s[16]={0};
    int field=0; usize j=0;
    for (const char *p=arg; *p; p++) {
        if (*p==' ') { field++; j=0; continue; }
        if (field==0 && j<31)  ip_s[j++]   = *p;
        if (field==1 && j<127) path_s[j++] = *p;
        if (field==2 && j<15)  out_s[j++]  = *p;
    }
    if (!ip_s[0] || !path_s[0]) {
        print_str("Usage: wget <ip> <path> <outfile>\r\n"); return;
    }
    print_str("Resolving "); print_str(ip_s); print_str("...\r\n");
    u32 ip = net_dns_resolve(ip_s);
    if (!ip) { print_str("DNS failed\r\n"); return; }
    print_str("Connecting to "); net_print_ip(ip);
    print_str(path_s); print_str("\r\n");
    static u8 dl_buf[32768];
    int n = net_http_get(ip, 80, path_s, dl_buf, sizeof(dl_buf));
    if (n < 0) { print_str("wget: connection failed\r\n"); return; }
    print_str("Downloaded "); print_int(n); print_str(" bytes\r\n");
    if (out_s[0]) {
        char name83[12];
        format_83_name(out_s, name83);
        fat32_create_file_in(name83, cwd_active());
        i64 fd = vfs_open_in(name83, cwd_active());
        if (fd >= 0) {
            vfs_write((u64)fd, dl_buf, (usize)n);
            vfs_close((u64)fd);
            print_str("Saved to "); print_str(out_s); print_str("\r\n");
        }
    }
}

static void cmd_run(const char *name) {
    format_83_name(name, name83);
    i64 fd = vfs_open_in(name83, cwd_active());
    if (fd < 0) { print_str("File not found.\r\n"); return; }
    void *buf = heap_malloc(524288);
    if (!buf) { vfs_close((u64)fd); print_str("OOM.\r\n"); return; }
    usize total = 0;
    for (;;) { i64 n = vfs_read((u64)fd, (u8*)buf + total, 512); if (n <= 0) break; total += (usize)n; }
    vfs_close((u64)fd);
    i64 pid = process_create((u64)buf, name);
    heap_free(buf);
    if (pid < 0) { print_str("Process create failed.\r\n"); return; }
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
    print_str("Launching Retro GUI...\r\n");

    /* Initialize the GUI system */
    gui_init();

    /* gui_init() now sets up the full desktop layout:
     *   - File Manager window (left half, VGA cols/rows)
     *   - Terminal window     (right half)
     *   - About modal dialog  (centred, on top)
     *   - Menu bar + Dock bar
     * All widget coordinates are in VGA character cells, not pixels.
     */

    print_str("GUI running! Press ESC to exit...\r\n");

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
    int  lbtn_prev   = 0;   /* previous left-button state for edge detect */
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

                /* Left button state for dragging */
                lbtn_curr = flags & 0x01;

                /* Left button click (rising edge) */
                if (lbtn_curr && !lbtn_prev) {
                    gui_handle_click(-1, -1);
                }
                
                /* Right button click (rising edge) */
                if ((flags & 0x02) && !(lbtn_prev & 0x02)) {
                    int cx, cy;
                    gui_cursor_get(&cx, &cy);
                    gui_handle_right_click(cx, cy);
                }

                /* Dragging is handled inside gui_cursor_move - no extra call needed */

                /* Button released */
                if (!lbtn_curr && lbtn_prev) {
                    gui_stop_drag();
                }

                lbtn_prev = lbtn_curr;

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
        usb_hid_poll();
        __asm__ volatile("hlt");
    }
    gui_exit:;

    print_str("Exiting GUI, returning to shell...\r\n");
    gui_shutdown();

    /* Restore VGA text mode cursor */
    outb(0x3D4, 0x0A); outb(0x3D5, (inb(0x3D5) & 0xC0) | 14);
    outb(0x3D4, 0x0B); outb(0x3D5, (inb(0x3D5) & 0xE0) | 15);
    vga_clear();
}

static void cmd_shc_info(void) {
    print_str("ENGINE Compiler (SHC) is installed on this disk.\r\n\r\n");
    print_str("Usage:\r\n");
    print_str("  elf SHC              launch the compiler\r\n");
    print_str("  (enter filename)     e.g. PROG.SHA\r\n\r\n");
    print_str("ENGINE language syntax:\r\n");
    print_str("  set x to 10         set y to x + 5\r\n");
    print_str("  if x greater y then ... else ... end\r\n");
    print_str("  repeat while x greater 0 ... end\r\n");
    print_str("  return 0\r\n\r\n");
    print_str("Comparisons: greater less equals notequals greaterequals lessequals\r\n");
    print_str("Operators:   + - * / %%\r\n");
}

static void cmd_elf(const char *name) {
    format_83_name(name, name83);
    i64 fd = vfs_open_in(name83, cwd_active());
    if (fd < 0) { print_str("File not found.\r\n"); return; }
    void *buf = heap_malloc(524288);
    if (!buf) { vfs_close((u64)fd); print_str("OOM.\r\n"); return; }
    usize total = 0;
    for (;;) { i64 n = vfs_read((u64)fd, (u8*)buf + total, 512); if (n <= 0) break; total += (usize)n; }
    vfs_close((u64)fd);
    i64 pid = elf_load(buf, total, name);
    heap_free(buf);
    if (pid < 0) { print_str("Bad ELF.\r\n"); return; }
    process_run((u64)pid);
}

static void exec_cmd(void) {
    const char *s = input_buf;
    while (*s == ' ') s++;
    if (!*s) return;

#define CMD(name, body) \
    if (strncmp(s, name, sizeof(name)-1) == 0 && \
        (s[sizeof(name)-1] == ' ' || s[sizeof(name)-1] == 0)) { body; return; }

    /* --- system --- */
    CMD("clear",   vga_clear())
    CMD("reboot",  { print_str("Rebooting...\r\n"); outb(0x64, 0xFE); cli(); hlt(); })
    CMD("halt",    { print_str("System halted.\r\n"); cli(); hlt(); })
    CMD("ps",      ps_list())
    CMD("meminfo", cmd_meminfo())
    CMD("uptime",  cmd_uptime())
    CMD("uname",   cmd_uname())
    CMD("df",      cmd_df())
    CMD("help",    print_str(
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
        "GUI:       gui                launch retro GUI demo\r\n"
        "Display:   720p  1080p        switch resolution (while in GUI)\r\n"
        "           browser            web browser (coming soon)\r\n"))

    /* --- file management --- */
    const char *arg = get_arg(s);
    CMD("echo",    { if (arg) { print_str(arg); print_str("\r\n"); } else print_str("\r\n"); })
    CMD("shc",     cmd_shc_info())
    CMD("ls",      cmd_ls())
    CMD("pwd",     { print_str(cwd_path); print_str("\r\n"); })
    CMD("cd",      { cmd_cd(arg); })
    CMD("stat",    { if (arg) cmd_stat(arg);    else print_str("Usage: stat <name>\r\n"); })
    CMD("cat",     { if (arg) cmd_cat(arg);     else print_str("Usage: cat <file>\r\n"); })
    CMD("head",    { if (arg) cmd_head(arg);    else print_str("Usage: head <file> [n]\r\n"); })
    CMD("tail",    { if (arg) cmd_tail(arg);    else print_str("Usage: tail <file> [n]\r\n"); })
    CMD("hexdump", { if (arg) cmd_hexdump(arg); else print_str("Usage: hexdump <file>\r\n"); })
    CMD("wc",      { if (arg) cmd_wc(arg);      else print_str("Usage: wc <file>\r\n"); })
    CMD("find",    { cmd_find(arg); })
    CMD("touch",   { if (arg) cmd_touch(arg);   else print_str("Usage: touch <file>\r\n"); })
    CMD("write",   { if (arg) cmd_write(arg);   else print_str("Usage: write <file> <text>\r\n"); })
    CMD("append",  { if (arg) cmd_append(arg);  else print_str("Usage: append <file> <text>\r\n"); })
    CMD("rm",      { if (arg) cmd_rm(arg);      else print_str("Usage: rm <file>\r\n"); })
    CMD("cp",      { if (arg) cmd_cp(arg);      else print_str("Usage: cp <src> <dst>\r\n"); })
    CMD("rename",  { if (arg) cmd_rename(arg);  else print_str("Usage: rename <old> <new>\r\n"); })
    CMD("mkdir",   { if (arg) cmd_mkdir(arg);   else print_str("Usage: mkdir <dir>\r\n"); })
    CMD("rmdir",   { if (arg) cmd_rmdir(arg);   else print_str("Usage: rmdir <dir>\r\n"); })

    /* --- processes --- */
    CMD("run",     { if (arg) cmd_run(arg);     else print_str("Usage: run <file>\r\n"); })
    CMD("elf",     { if (arg) cmd_elf(arg);     else print_str("Usage: elf <file>\r\n"); })

#undef CMD

    /* --- networking (manual dispatch for multi-word arg parsing) --- */
    if (strncmp(s, "ifconfig", 8)==0 && (s[8]==' '||s[8]==0)) { cmd_ifconfig(); return; }
    if (strncmp(s, "gui",      3)==0 && (s[3]==' '||s[3]==0)) { cmd_gui();      return; }

    /* Resolution switching */
    if (strncmp(s, "720p", 4)==0 && (s[4]==' '||s[4]==0)) {
        if (!fb_is_enabled()) { print_str("GUI not active. Run 'gui' first.\r\n"); return; }
        if (fb_set_resolution(1280, 720))
            print_str("Resolution set to 1280x720 (720p).\r\n");
        else
            print_str("720p not supported by display.\r\n");
        return;
    }
    if (strncmp(s, "1080p", 5)==0 && (s[5]==' '||s[5]==0)) {
        if (!fb_is_enabled()) { print_str("GUI not active. Run 'gui' first.\r\n"); return; }
        if (fb_set_resolution(1920, 1080))
            print_str("Resolution set to 1920x1080 (1080p).\r\n");
        else
            print_str("1080p not supported by display.\r\n");
        return;
    }

    /* Browser (future feature) */
    if (strncmp(s, "browser", 7)==0 && (s[7]==' '||s[7]==0)) {
        print_str("Browser: not yet implemented.\r\n");
        print_str("Run 'gui' first, then use browser from GUI when available.\r\n");
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
        if (!net_tcp_listen(port)) { print_str("netcat: bind failed\r\n"); return; }
        print_str("netcat: waiting on port "); { char pb[8]; int pi=0; u16 tmp=port; if(!tmp){pb[pi++]='0';} else {while(tmp){pb[pi++]='0'+tmp%10;tmp/=10;}} for(int i=pi-1;i>=0;i--) vga_putchar((u8)pb[i]); }
        print_str(" (any key to cancel)\r\n");
        TcpConn *c = NULL;
        for (;;) { net_poll(); if (inb(0x64)&1){inb(0x60);net_tcp_unlisten(port);return;} c=net_tcp_accept_nb(port); if(c) break; for(volatile int _d=0;_d<5000;_d++) __asm__ volatile("pause"); }
        print_str("netcat: connected\r\n");
        static u8 nc_buf[512];
        for (;;) { int n=net_tcp_recv(c,nc_buf,sizeof(nc_buf)-1); if(n<=0) break; nc_buf[n]=0; print_str((char*)nc_buf); if(inb(0x64)&1){inb(0x60);break;} }
        net_tcp_close(c); net_tcp_unlisten(port);
        print_str("\r\nnetcat: done.\r\n"); return;
    }

    print_str("Unknown command. Try 'help'.\r\n");
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
    net_start();          /* init e1000, run DHCP */

    /* Drain any stale i8042 bytes that accumulated during network init.
     * Without this, DHCP broadcast packets can cause phantom keypresses. */
    { int t = 1000; while ((inb(0x64) & 1) && --t) inb(0x60); }

    ps2_init();               /* full i8042 controller init — KB + mouse */
    kbd_init_dummy();         /* polled I/O — no keyboard init needed */
    mouse_ready = ps2_mouse_ok();
    if (mouse_ready) ps2_mouse_refresh();
    scheduler_start();    /* STI — timer fires from here */
    usb_hid_init();       /* USB HID XHCI — supplements PS/2 */

    smp_init();
    watchdog_init();

    print_str("========================================\r\n");
    print_str("  ENGINE v0.1\r\n");
    print_str("  x86-64 | preemptive | FAT32 | ring-3\r\n");
    print_str("  Type 'help' for available commands.\r\n");
    print_str("========================================\r\n");

    for (;;) {
        watchdog_pet();   /* kernel idle/shell is alive */
        print_str("engine:");
        print_str(cwd_path);
        print_str("$ ");
        read_line(input_buf, sizeof(input_buf));
        watchdog_pet();   /* pet again after potentially long input wait */
        exec_cmd();
    }
}
