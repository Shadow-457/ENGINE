/* ================================================================
 *  SystrixOS — kernel/kernel.c
 *
 *  Implements:
 *    - VGA text-mode terminal with scrollback history
 *    - PS/2 mouse driver (polled)
 *    - PS/2 keyboard driver (polled)
 *    - ATA PIO disk driver + AHCI abstraction layer
 *    - FAT32 filesystem driver
 *    - Virtual filesystem (VFS) layer
 *    - Interactive shell with command history
 *    - kernel_main() entry point
 * ================================================================ */

#include "../include/kernel.h"


/* ================================================================
 *  VGA TEXT MODE  (80×25, base address 0xB8000)
 *
 *  We maintain a circular scrollback buffer of SCROLLBACK_ROWS lines.
 *  All output is written to that buffer first; the physical VGA
 *  framebuffer is only updated when:
 *    - The view is live (scroll_offset == 0), or
 *    - The user scrolls with Shift+PageUp / Shift+PageDown.
 * ================================================================ */

#define SCROLLBACK_ROWS  200    /* total lines kept in scroll history */
#define SCROLL_STEP        5    /* lines moved per PageUp/PageDown    */

/* Circular back-buffer.  The "current" row is at index buf_next. */
static u16 back_buf[SCROLLBACK_ROWS][VGA_COLS];

/* How many rows have been written in total (absolute, ever-increasing). */
static int buf_next  = 0;
static int buf_total = 0;

/* How far the view is scrolled back from the live bottom.
 * 0 = showing live output; N = showing N rows back. */
static int scroll_offset = 0;

/* Current text cursor position on screen. */
static u8 cur_row = 0;
static u8 cur_col = 0;


/* Move the VGA hardware cursor to match cur_row / cur_col. */
static void vga_update_hw_cursor(void)
{
    u16 pos = (u16)(cur_row * VGA_COLS + cur_col);
    outb(0x3D4, 0x0F);
    outb(0x3D5, (u8)(pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (u8)((pos >> 8) & 0xFF));
}


/* ================================================================
 *  PS/2 MOUSE DRIVER  (polled, text-mode cursor via inverted cell)
 * ================================================================ */

static int  mouse_col     = 40;   /* current column (0 .. VGA_COLS-1) */
static int  mouse_row     = 12;   /* current row    (0 .. VGA_ROWS-1) */
static u16  mouse_saved   = 0;    /* VGA cell saved from under cursor  */
static int  mouse_visible = 0;    /* is cursor currently drawn?        */
static int  mouse_ready   = 0;    /* did init succeed?                 */
static int  mouse_cycle   = 0;    /* which byte of the 3-byte packet   */
static u8   mouse_pkt[3];         /* accumulated packet bytes          */


/* Wait until the i8042 input buffer is empty (safe to write). */
static void ps2_wait_write(void)
{
    u32 timeout = 100000;
    while ((inb(0x64) & 2) && --timeout) {}
}

/* Wait until the i8042 output buffer has data (safe to read). */
static void ps2_wait_read(void)
{
    u32 timeout = 100000;
    while (!(inb(0x64) & 1) && --timeout) {}
}

static u8   ps2_read(void)       { ps2_wait_read();  return inb(0x60); }
static void ps2_cmd(u8 cmd)      { ps2_wait_write(); outb(0x64, cmd); }
static void ps2_data(u8 data)    { ps2_wait_write(); outb(0x60, data); }

/* Route the next byte to the mouse port (0xD4 = "next byte to mouse"). */
static void ps2_mouse_write(u8 data)
{
    ps2_cmd(0xD4);
    ps2_data(data);
}


/* Render the mouse cursor by inverting the fg/bg nibbles of the cell under
 * the pointer.  We only touch the VGA framebuffer — intentionally leaving
 * the back-buffer clean so scrollback history stays unaffected. */
static void mouse_draw(void)
{
    if (!mouse_visible) return;

    u16 *fb  = (u16 *)VGA_BASE;
    int  idx = mouse_row * VGA_COLS + mouse_col;
    u16  cell = fb[idx];
    u8   attr = (u8)(cell >> 8);

    /* Swap foreground and background nibbles to highlight the cell. */
    u8 inverted = (u8)(((attr & 0x0F) << 4) | ((attr >> 4) & 0x0F));
    fb[idx] = (u16)((u16)inverted << 8) | (cell & 0xFF);
}

/* Restore the cell that was under the cursor (erase the highlight). */
static void mouse_erase(void)
{
    if (!mouse_visible) return;

    u16 *fb  = (u16 *)VGA_BASE;
    int  idx = mouse_row * VGA_COLS + mouse_col;
    fb[idx]  = mouse_saved;
}

/* Save the cell under the current position and draw the cursor. */
static void mouse_show(void)
{
    u16 *fb  = (u16 *)VGA_BASE;
    int  idx = mouse_row * VGA_COLS + mouse_col;
    mouse_saved   = fb[idx];
    mouse_visible = 1;
    mouse_draw();
}

/* Move the cursor by (dr rows, dc columns), clamped to the screen. */
static void mouse_move(int dr, int dc)
{
    mouse_erase();

    mouse_row += dr;
    mouse_col += dc;

    if (mouse_row < 0)         mouse_row = 0;
    if (mouse_row >= VGA_ROWS) mouse_row = VGA_ROWS - 1;
    if (mouse_col < 0)         mouse_col = 0;
    if (mouse_col >= VGA_COLS) mouse_col = VGA_COLS - 1;

    /* Save the cell at the new position before drawing. */
    u16 *fb     = (u16 *)VGA_BASE;
    mouse_saved = fb[mouse_row * VGA_COLS + mouse_col];
    mouse_draw();
}


/* Called from the keyboard poll loop whenever bit 5 of port 0x64 is set
 * (a mouse byte is waiting).  Accumulates the 3-byte PS/2 packet; on a
 * complete packet, moves the cursor and feeds the input ring buffer. */
static void mouse_poll(void)
{
    u8 byte = inb(0x60);
    mouse_pkt[mouse_cycle++] = byte;

    if (mouse_cycle < 3) return;   /* packet not complete yet */
        mouse_cycle = 0;

    u8 flags = mouse_pkt[0];

    /* Byte 0 must always have bit 3 set — resync if not. */
    if (!(flags & 0x08)) {
        mouse_cycle = 0;
        return;
    }

    /* Overflow bits mean the packet is garbage — discard it. */
    if (flags & 0xC0) return;

    /* Sign-extend the 9-bit X and Y deltas. */
    int dx =  (int)mouse_pkt[1] - ((flags & 0x10) ? 256 : 0);
    int dy =  (int)mouse_pkt[2] - ((flags & 0x20) ? 256 : 0);

    /* Scale pixel deltas to text cells (~8 pixels per cell column). */
    int dc =  dx / 8;
    int dr = -dy / 8;   /* VGA Y axis is inverted relative to mouse Y */

    if (dc || dr) mouse_move(dr, dc);

    /* Forward button state to the input subsystem. */
    u8 buttons = 0;
    if (flags & 0x01) buttons |= INPUT_BTN_LEFT;
    if (flags & 0x02) buttons |= INPUT_BTN_RIGHT;
    if (flags & 0x04) buttons |= INPUT_BTN_MIDDLE;
    input_push_mouse(dx, dy, buttons);
}


/* Initialise the PS/2 mouse.  Returns 1 on success, 0 if no mouse found. */
static int mouse_init(void)
{
    /* Enable the auxiliary (mouse) port. */
    ps2_cmd(0xA8);

    /* Read the current command byte, set the IRQ12 enable bit,
     * and clear the "disable mouse clock" bit so the mouse sends data. */
    ps2_cmd(0x20);
    u8 cb = ps2_read();
    cb |=  0x02;   /* enable IRQ12 */
    cb &= ~0x20;   /* enable mouse clock */
    ps2_cmd(0x60);
    ps2_data(cb);

    /* Reset the mouse and check for ACK. */
    ps2_mouse_write(0xFF);
    u8 ack = ps2_read();
    if (ack != 0xFA) return 0;   /* no ACK means no mouse */

        ps2_read();   /* 0xAA — self-test passed */
        ps2_read();   /* 0x00 — device ID */

        /* Apply default settings and enable data reporting. */
        ps2_mouse_write(0xF6); ps2_read();
    ps2_mouse_write(0xF4); ps2_read();

    return 1;
}


/* ================================================================
 *  VGA SCROLLBACK & OUTPUT
 * ================================================================ */

/* Repaint VGA_ROWS rows from the back-buffer, ending at (buf_total - 1 -
 * scroll_offset).  Called whenever the scroll position changes. */
static void vga_redraw(void)
{
    u16 *fb  = (u16 *)VGA_BASE;
    int  top = buf_total - VGA_ROWS - scroll_offset;

    for (int r = 0; r < VGA_ROWS; r++) {
        int src = top + r;

        if (src < 0) {
            /* Before the start of history — draw a blank row. */
            for (int c = 0; c < VGA_COLS; c++)
                fb[r * VGA_COLS + c] = (u16)(VGA_ATTR << 8) | ' ';
        } else {
            int idx = src % SCROLLBACK_ROWS;
            for (int c = 0; c < VGA_COLS; c++)
                fb[r * VGA_COLS + c] = back_buf[idx][c];
        }
    }
}

void vga_scroll_up(void)
{
    int max_scroll = buf_total - VGA_ROWS;
    if (max_scroll < 0) max_scroll = 0;

    scroll_offset += SCROLL_STEP;
    if (scroll_offset > max_scroll) scroll_offset = max_scroll;

    vga_redraw();
    if (mouse_ready) mouse_show();
}

void vga_scroll_down(void)
{
    scroll_offset -= SCROLL_STEP;
    if (scroll_offset < 0) scroll_offset = 0;

    vga_redraw();
    if (mouse_ready) mouse_show();
}

void vga_clear(void)
{
    /* Blank the circular back-buffer. */
    for (int r = 0; r < SCROLLBACK_ROWS; r++)
        for (int c = 0; c < VGA_COLS; c++)
            back_buf[r][c] = (u16)(VGA_ATTR << 8) | ' ';

    buf_next = 0; buf_total = 0; scroll_offset = 0;
    cur_row  = 0; cur_col   = 0;

    /* Blank the physical VGA framebuffer directly. */
    u16 *fb = (u16 *)VGA_BASE;
    for (int i = 0; i < VGA_COLS * VGA_ROWS; i++)
        fb[i] = (u16)(VGA_ATTR << 8) | ' ';

    /* Seed buf_total with VGA_ROWS blank rows so redraw always has
     * enough history to fill the screen without out-of-bounds reads. */
    for (int r = 0; r < VGA_ROWS; r++) {
        buf_next  = r % SCROLLBACK_ROWS;
        buf_total = r + 1;
    }
    buf_next  = VGA_ROWS % SCROLLBACK_ROWS;
    buf_total = VGA_ROWS;

    cur_row = 0; cur_col = 0;
    vga_update_hw_cursor();
}


/* Scroll the VGA framebuffer up by one line and blank the bottom row.
 * Only called when the view is live (scroll_offset == 0) and the screen is full. */
static void vga_fb_scroll_one(void)
{
    u16 *fb = (u16 *)VGA_BASE;

    /* Shift all rows up by one. */
    for (int i = 0; i < VGA_COLS * (VGA_ROWS - 1); i++)
        fb[i] = fb[i + VGA_COLS];

    /* Blank the newly-exposed bottom row. */
    for (int i = VGA_COLS * (VGA_ROWS - 1); i < VGA_COLS * VGA_ROWS; i++)
        fb[i] = (u16)(VGA_ATTR << 8) | ' ';
}

/* Advance the circular back-buffer by one row, clearing the new slot. */
static void backbuf_advance(void)
{
    buf_next = (buf_next + 1) % SCROLLBACK_ROWS;
    buf_total++;
    for (int i = 0; i < VGA_COLS; i++)
        back_buf[buf_next][i] = (u16)(VGA_ATTR << 8) | ' ';
}

void vga_putchar(u8 c)
{
    if (c == '\r') {
        cur_col = 0;
        vga_update_hw_cursor();
        return;
    }

    if (c == '\n') {
        cur_col = 0;
        backbuf_advance();
        if (cur_row < VGA_ROWS - 1) {
            cur_row++;
        } else if (scroll_offset == 0) {
            vga_fb_scroll_one();
        }
        vga_update_hw_cursor();
        return;
    }

    /* Normal printable character: write to the back-buffer. */
    back_buf[buf_next][cur_col] = (u16)(VGA_ATTR << 8) | c;

    /* Only write to VGA if the view is live (not scrolled back). */
    if (scroll_offset == 0)
        ((u16 *)VGA_BASE)[cur_row * VGA_COLS + cur_col] = (u16)(VGA_ATTR << 8) | c;

    cur_col++;
    if (cur_col < VGA_COLS) {
        vga_update_hw_cursor();
        return;
    }

    /* Column wrapped — implicit newline. */
    cur_col = 0;
    backbuf_advance();
    if (cur_row < VGA_ROWS - 1) {
        cur_row++;
    } else if (scroll_offset == 0) {
        vga_fb_scroll_one();
    }
    vga_update_hw_cursor();
}

void vga_backspace(void)
{
    if (cur_col == 0) return;

    cur_col--;
    u16 blank = (u16)(VGA_ATTR << 8) | ' ';
    back_buf[buf_next][cur_col] = blank;

    if (scroll_offset == 0)
        ((u16 *)VGA_BASE)[cur_row * VGA_COLS + cur_col] = blank;

    vga_update_hw_cursor();
}

void print_str(const char *s)
{
    while (*s) vga_putchar((u8)*s++);
}

void print_hex_byte(u8 v)
{
    static const char hex[] = "0123456789ABCDEF";
    vga_putchar((u8)hex[v >> 4]);
    vga_putchar((u8)hex[v & 0xF]);
}


/* ================================================================
 *  DISK ABSTRACTION LAYER
 *
 *  The kernel supports two disk backends:
 *    1. Legacy ATA PIO (28-bit LBA, primary channel 0x1F0)
 *    2. AHCI SATA (faster, used when available)
 *
 *  All filesystem code calls ata_read_sector() / ata_write_sector().
 *  Those wrappers transparently route to whichever backend is active,
 *  so FAT32/JFS work identically regardless of the disk type.
 *
 *  disk_use_ahci() is called from kernel_main() after ahci_init()
 *  succeeds; otherwise we default to ATA PIO.
 * ================================================================ */

#define DISK_ATA_PIO  0
#define DISK_AHCI     1

static int disk_backend = DISK_ATA_PIO;   /* default: legacy PIO */

void disk_use_ahci(void)
{
    disk_backend = DISK_AHCI;
}


/* ── Legacy ATA PIO ─────────────────────────────────────────────── */

static void ata_pio_read(u32 lba, void *buf)
{
    watchdog_suspend();

    /* Wait for the drive to be ready (BSY bit clear). */
    u32 timeout = 0x100000;
    while ((inb(ATA_STATUS) & 0x80) && --timeout) {}

    if (!timeout) {
        /* Drive timed out — return a zeroed sector. */
        u16 *p = (u16 *)buf;
        for (int i = 0; i < 256; i++) p[i] = 0;
        watchdog_resume();
        return;
    }

    /* Send the 28-bit LBA read command. */
    outb(ATA_DRIVE,   0xE0 | (u8)((lba >> 24) & 0x0F));
    outb(ATA_COUNT,   1);
    outb(ATA_LBA_LO,  (u8)(lba));
    outb(ATA_LBA_MID, (u8)(lba >> 8));
    outb(ATA_LBA_HI,  (u8)(lba >> 16));
    outb(ATA_CMD,     ATA_CMD_READ);

    /* Wait for data ready (DRQ bit set). */
    timeout = 0x100000;
    while (!(inb(ATA_STATUS) & 0x08) && --timeout) {}

    if (!timeout) {
        u16 *p = (u16 *)buf;
        for (int i = 0; i < 256; i++) p[i] = 0;
        watchdog_resume();
        return;
    }

    /* Read 256 words (512 bytes) from the data port. */
    u16 *p = (u16 *)buf;
    for (int i = 0; i < 256; i++) p[i] = inw(ATA_DATA);

    watchdog_resume();
}

static void ata_pio_write(u32 lba, const void *buf)
{
    /* Wait for the drive to be ready. */
    u32 timeout = 0x100000;
    while ((inb(ATA_STATUS) & 0x80) && --timeout) {}
    if (!timeout) return;

    /* Send the 28-bit LBA write command. */
    outb(ATA_DRIVE,   0xE0 | (u8)((lba >> 24) & 0x0F));
    outb(ATA_COUNT,   1);
    outb(ATA_LBA_LO,  (u8)(lba));
    outb(ATA_LBA_MID, (u8)(lba >> 8));
    outb(ATA_LBA_HI,  (u8)(lba >> 16));
    outb(ATA_CMD,     ATA_CMD_WRITE);

    /* Wait for the drive to signal it is ready to accept data. */
    timeout = 0x100000;
    while (!(inb(ATA_STATUS) & 0x08) && --timeout) {}
    if (!timeout) return;

    /* Write 256 words (512 bytes) to the data port. */
    const u16 *p = (const u16 *)buf;
    for (int i = 0; i < 256; i++) outw(ATA_DATA, p[i]);

    /* Flush the write to the physical disk. */
    outb(ATA_CMD, ATA_CMD_FLUSH);
}


/* ── Public disk API used by all filesystem code ────────────────── */

void ata_read_sector(u32 lba, void *buf)
{
    if (disk_backend == DISK_AHCI) {
        /* Try AHCI first; fall back to PIO if it fails. */
        if (ahci_read_sector(0, lba, buf) == 0) return;
    }
    ata_pio_read(lba, buf);
}

void ata_write_sector(u32 lba, const void *buf)
{
    if (disk_backend == DISK_AHCI) {
        if (ahci_write_sector(0, lba, buf) == 0) return;
    }
    ata_pio_write(lba, buf);
}


/* ================================================================
 *  FAT32 FILESYSTEM DRIVER
 * ================================================================ */

/* Volume geometry (read from the BIOS Parameter Block at mount time). */
static u8  fat32_spc        = 0;   /* sectors per cluster                */
static u32 fat32_fat_start  = 0;   /* LBA of the first FAT sector        */
static u32 fat32_data_start = 0;   /* LBA of the first data sector       */
static u32 fat32_root_clus  = 0;   /* cluster number of the root dir     */
static u32 fat32_total_clus = 0;   /* total data clusters on the volume  */

/* Sector-sized I/O buffers (one per logical use to avoid aliasing). */
static u8 sector_buf[512];
static u8 dir_buf[512];
static u8 fat_buf[512];
static u8 file_buf[512];

/* FAT sector cache: avoids re-reading the same FAT sector repeatedly. */
static u32 fat_buf_lba = (u32)-1;

static void fat_read_cached(u32 lba)
{
    if (lba == fat_buf_lba) return;   /* already in cache */
        ata_read_sector(lba, fat_buf);
    fat_buf_lba = lba;
}

static void fat_write_cached(u32 lba)
{
    ata_write_sector(lba, fat_buf);
    fat_buf_lba = lba;
}


void fat32_init(void)
{
    /* Read the Volume Boot Record / BPB from the start of the partition. */
    ata_read_sector(PART_START, sector_buf);

    fat32_spc        = sector_buf[13];
    u16 reserved_sec = *(u16 *)(sector_buf + 14);
    u8  num_fats     = sector_buf[16];
    u32 fat_size     = *(u32 *)(sector_buf + 36);

    fat32_fat_start  = PART_START + reserved_sec;
    fat32_data_start = fat32_fat_start + num_fats * fat_size;
    fat32_root_clus  = *(u32 *)(sector_buf + 44);

    /* Compute the total data cluster count from the BPB so that
     * fat32_alloc_cluster can scan the full FAT without an arbitrary cap. */
    u32 total_sec = *(u32 *)(sector_buf + 32);
    if (!total_sec) total_sec = (u32)(*(u16 *)(sector_buf + 19));

    u32 data_secs    = total_sec - (fat32_data_start - PART_START);
    fat32_total_clus = (fat32_spc > 0) ? (data_secs / fat32_spc) : 0;
}


/* Convert a cluster number to its starting LBA. */
u32 cluster_to_lba(u32 cluster)
{
    return fat32_data_start + (cluster - 2) * fat32_spc;
}

/* Follow the FAT chain: return the cluster that comes after 'cluster'. */
u32 fat32_next_cluster(u32 cluster)
{
    u32 byte_off = cluster * 4;
    u32 sector   = byte_off / 512 + fat32_fat_start;
    u32 idx      = byte_off % 512;
    fat_read_cached(sector);
    return *(u32 *)(fat_buf + idx) & 0x0FFFFFFF;
}


/* Search a directory (starting at dir_clus) for an 8.3-format filename.
 * On success, fills *out_clus and *out_size and returns 0.
 * Returns -1 if the file is not found. */
static i64 fat32_find_raw_in(const char *name83, u32 *out_clus, u32 *out_size, u32 dir_clus)
{
    u32 clus = dir_clus;

    while (clus < 0x0FFFFFF8) {
        u32 lba = cluster_to_lba(clus);

        for (u8 s = 0; s < fat32_spc; s++) {
            ata_read_sector(lba + s, dir_buf);

            for (int e = 0; e < 16; e++) {
                u8 *de = dir_buf + e * 32;

                if (de[0] == 0x00) return -1;    /* end of directory */
                    if (de[0] == 0xE5) continue;      /* deleted entry    */
                        if (de[11] & 0x08) continue;      /* volume label     */

                            if (strncmp((char *)de, name83, 11) == 0) {
                                if (out_clus) {
                                    *out_clus = ((u32)de[20] << 16) | ((u32)de[21] << 24)
                                    | (u32)de[26]          | ((u32)de[27] << 8);
                                }
                                if (out_size) *out_size = *(u32 *)(de + 28);
                                return 0;
                            }
            }
        }
        clus = fat32_next_cluster(clus);
    }
    return -1;
}

static i64 fat32_find_raw(const char *name83, u32 *out_clus, u32 *out_size)
{
    return fat32_find_raw_in(name83, out_clus, out_size, fat32_root_clus);
}

i64 fat32_find_file_in(const char *name83, u32 *out_size, u32 dir_clus)
{
    u32 clus;
    return fat32_find_raw_in(name83, &clus, out_size, dir_clus);
}

i64 fat32_find_file(const char *name83, u32 *out_size)
{
    u32 clus;
    i64 rc = fat32_find_raw(name83, &clus, out_size);
    return rc;
}


/* Scan the entire FAT and return the first free cluster.
 * Marks the cluster as end-of-chain (0x0FFFFFFF) in the FAT.
 * Returns (u32)-1 if the disk is full. */
u32 fat32_alloc_cluster(void)
{
    /* Scan the full FAT — fat32_total_clus ensures we never miss a cluster. */
    u32 limit = (fat32_total_clus > 0) ? (fat32_total_clus + 2) : 65536u;

    for (u32 clus = 2; clus < limit; clus++) {
        u32 byte_off = clus * 4;
        u32 sec      = byte_off / 512 + fat32_fat_start;
        u32 idx      = byte_off % 512;

        fat_read_cached(sec);

        if ((*(u32 *)(fat_buf + idx) & 0x0FFFFFFF) == 0) {
            *(u32 *)(fat_buf + idx) = 0x0FFFFFFF;   /* mark end-of-chain */
            fat_write_cached(sec);
            return clus;
        }
    }
    return (u32)-1;   /* disk full */
}


/* ── Directory entry helpers ─────────────────────────────────────── */

/* Walk dir_clus looking for an empty or deleted slot; write the 32-byte
 * directory entry there. */
static void write_dir_entry_in(u8 *entry32, u32 dir_clus)
{
    u32 clus = dir_clus;

    while (clus < 0x0FFFFFF8) {
        u32 lba = cluster_to_lba(clus);

        for (u8 s = 0; s < fat32_spc; s++) {
            ata_read_sector(lba + s, dir_buf);

            for (int e = 0; e < 16; e++) {
                u8 *de = dir_buf + e * 32;

                /* An empty (0x00) or deleted (0xE5) slot is usable. */
                if (de[0] == 0x00 || de[0] == 0xE5) {
                    memcpy(de, entry32, 32);
                    ata_write_sector(lba + s, dir_buf);
                    return;
                }
            }
        }
        clus = fat32_next_cluster(clus);
    }
}


/* Read one BCD-encoded RTC CMOS register and convert to binary. */
static u8 rtc_read_reg(u8 reg)
{
    outb(0x70, reg);
    u8 v = inb(0x71);
    return (u8)(((v >> 4) * 10) + (v & 0xF));
}

/* Build a FAT16 date word: bits [15:9]=year-1980, [8:5]=month, [4:0]=day. */
static u16 fat_date_now(void)
{
    u8 year  = rtc_read_reg(0x09);   /* 0–99, we assume 2000+ */
    u8 month = rtc_read_reg(0x08);
    u8 day   = rtc_read_reg(0x07);

    /* Clamp to valid FAT ranges. */
    if (month < 1)  month = 1;
    if (month > 12) month = 12;
    if (day   < 1)  day   = 1;
    if (day   > 31) day   = 31;

    u32 yr1980 = (u32)(2000 + year) - 1980;
    return (u16)((yr1980 << 9) | ((u32)month << 5) | day);
}

/* Build a FAT16 time word: bits [15:11]=hour, [10:5]=min, [4:0]=sec/2. */
static u16 fat_time_now(void)
{
    u8 hour = rtc_read_reg(0x04);
    u8 min  = rtc_read_reg(0x02);
    u8 sec  = rtc_read_reg(0x00);
    return (u16)(((u32)hour << 11) | ((u32)min << 5) | (sec >> 1));
}


void fat32_create_file_in(const char *name83, u32 dir_clus)
{
    u8 de[32];
    memset(de, 0, 32);
    memcpy(de, name83, 11);
    de[11] = 0x20;   /* Archive attribute */

    /* Stamp creation and last-modified timestamps from the RTC. */
    u16 fdate = fat_date_now();
    u16 ftime = fat_time_now();

    /* Creation time/date at offsets 14–17. */
    de[14] = (u8)(ftime);
    de[15] = (u8)(ftime >> 8);
    de[16] = (u8)(fdate);
    de[17] = (u8)(fdate >> 8);

    /* Last-modified time/date at offsets 22–25. */
    de[22] = (u8)(ftime);
    de[23] = (u8)(ftime >> 8);
    de[24] = (u8)(fdate);
    de[25] = (u8)(fdate >> 8);

    write_dir_entry_in(de, dir_clus);
}

void fat32_create_file(const char *name83)
{
    fat32_create_file_in(name83, fat32_root_clus);
}


void fat32_create_dir_entry_in(const char *name83, u32 cluster, u32 dir_clus)
{
    u8 de[32];
    memset(de, 0, 32);
    memcpy(de, name83, 11);
    de[11] = 0x10;   /* Directory attribute */

    /* Store the cluster number split across the two FAT32 fields. */
    de[26] = (u8)(cluster);
    de[27] = (u8)(cluster >> 8);
    de[20] = (u8)(cluster >> 16);
    de[21] = (u8)(cluster >> 24);

    write_dir_entry_in(de, dir_clus);
}

void fat32_create_dir_entry(const char *name83, u32 cluster)
{
    fat32_create_dir_entry_in(name83, cluster, fat32_root_clus);
}


/* Walk a directory and update the file-size field for an open file. */
static void patch_dir_size(FD *fd, u32 new_size)
{
    u32 clus = (fd->dir_clus != 0) ? fd->dir_clus : fat32_root_clus;

    while (clus < 0x0FFFFFF8) {
        u32 lba = cluster_to_lba(clus);

        for (u8 s = 0; s < fat32_spc; s++) {
            ata_read_sector(lba + s, dir_buf);

            for (int e = 0; e < 16; e++) {
                u8 *de = dir_buf + e * 32;
                if (de[0] == 0x00) goto size_next_clus;
                if (de[0] == 0xE5) continue;

                if (strncmp((char *)de, (char *)fd->name83, 11) == 0) {
                    *(u32 *)(de + 28) = new_size;
                    ata_write_sector(lba + s, dir_buf);
                    return;
                }
            }
        }
        size_next_clus:
        clus = fat32_next_cluster(clus);
    }
}

/* Walk a directory and patch the start-cluster field for a newly-written file. */
static void patch_dir_cluster(FD *fd, u32 start_clus)
{
    u32 clus = (fd->dir_clus != 0) ? fd->dir_clus : fat32_root_clus;

    while (clus < 0x0FFFFFF8) {
        u32 lba = cluster_to_lba(clus);

        for (u8 s = 0; s < fat32_spc; s++) {
            ata_read_sector(lba + s, dir_buf);

            for (int e = 0; e < 16; e++) {
                u8 *de = dir_buf + e * 32;
                if (de[0] == 0x00) goto clus_next_clus;
                if (de[0] == 0xE5) continue;

                if (strncmp((char *)de, (char *)fd->name83, 11) == 0) {
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

/* Write 'next' into the FAT entry for cluster 'cur' (links two clusters). */
static void fat_link(u32 cur, u32 next)
{
    u32 byte_off = cur * 4;
    u32 sec      = byte_off / 512 + fat32_fat_start;
    u32 idx      = byte_off % 512;
    fat_read_cached(sec);
    *(u32 *)(fat_buf + idx) = next;
    fat_write_cached(sec);
}


i64 fat32_delete_file_in(const char *name83, u32 dir_clus)
{
    u32 clus = dir_clus;

    while (clus < 0x0FFFFFF8) {
        u32 lba = cluster_to_lba(clus);

        for (u8 s = 0; s < fat32_spc; s++) {
            ata_read_sector(lba + s, dir_buf);

            for (int e = 0; e < 16; e++) {
                u8 *de = dir_buf + e * 32;
                if (de[0] == 0x00) return -1;    /* end of directory */
                    if (de[0] == 0xE5) continue;      /* already deleted  */
                        if (de[11] & 0x08) continue;      /* volume label     */

                            if (strncmp((char *)de, name83, 11) != 0) continue;

                            /* Found the entry — read the start cluster and size. */
                            u32 start_clus = ((u32)de[20] << 16) | ((u32)de[21] << 24)
                            | (u32)de[26]          | ((u32)de[27] << 8);
                        (void)*(u32 *)(de + 28);   /* fsize, unused */

                        /* Mark the directory entry as deleted. */
                        de[0] = 0xE5;
                        ata_write_sector(lba + s, dir_buf);

                        /* Walk the FAT chain and free every cluster. */
                        u32 fc = start_clus;
                        while (fc >= 2 && fc < 0x0FFFFFF8) {
                            u32 next    = fat32_next_cluster(fc);
                            u32 bo      = fc * 4;
                            u32 fsec    = bo / 512 + fat32_fat_start;
                            u32 fidx    = bo % 512;
                            fat_read_cached(fsec);
                            *(u32 *)(fat_buf + fidx) = 0;
                            fat_write_cached(fsec);
                            fc = next;
                        }
                        return 0;
            }
        }
        clus = fat32_next_cluster(clus);
    }
    return -1;
}

i64 fat32_delete_file(const char *name83)
{
    return fat32_delete_file_in(name83, fat32_root_clus);
}


/* ================================================================
 *  VFS LAYER  (thin wrapper over the FAT32 driver)
 * ================================================================ */

FD fd_table[MAX_FILES];

/* Open a file by 8.3 name in dir_clus; populate the FD slot.
 * Returns 0 on success, -1 if not found. */
static i64 fat32_open_raw_in(const char *name83, FD *slot, u32 dir_clus)
{
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

static i64 fat32_open_raw(const char *name83, FD *slot)
{
    return fat32_open_raw_in(name83, slot, fat32_root_clus);
}


/* Read up to 512 bytes from the current file position into buf. */
static i64 fat32_read_raw(FD *fd, void *buf, usize n)
{
    if (fd->pos >= fd->size) return 0;   /* EOF */

        /* Clamp to what is available. */
        u32 avail = fd->size - fd->pos;
    if (n > avail) n = avail;
    if (n > 512)   n = 512;

    /* Which sector within the current cluster? */
    u32 logical_sec = fd->pos / 512;
    u32 clus_sec    = logical_sec % fat32_spc;
    u32 byte_off    = fd->pos % 512;

    /* Don't read past the current sector boundary. */
    u32 in_sec = 512 - byte_off;
    if (n > in_sec) n = in_sec;

    u32 lba = cluster_to_lba(fd->cur_clus) + clus_sec;
    ata_read_sector(lba, file_buf);
    memcpy(buf, file_buf + byte_off, n);
    fd->pos += (u32)n;

    /* Advance to the next cluster when we cross a cluster boundary. */
    u32 clus_bytes = (u32)fat32_spc * 512;
    if ((fd->pos % clus_bytes) == 0)
        fd->cur_clus = fat32_next_cluster(fd->cur_clus);

    return (i64)n;
}

/* Write up to 512 bytes (one sector) at the current file position. */
static i64 fat32_write_raw(FD *fd, const void *buf, usize n)
{
    if (n > 512) n = 512;

    /* Allocate the first cluster if the file has no data yet. */
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

    /* Read-modify-write the sector to preserve bytes we are not overwriting. */
    u32 lba = cluster_to_lba(fd->cur_clus) + clus_sec;
    ata_read_sector(lba, file_buf);
    memcpy(file_buf + byte_off, buf, n);
    ata_write_sector(lba, file_buf);

    fd->pos += (u32)n;
    if (fd->pos > fd->size) {
        fd->size = fd->pos;
        patch_dir_size(fd, fd->size);
    }

    /* Advance cluster at a cluster boundary, allocating a new one if needed. */
    u32 clus_bytes = (u32)fat32_spc * 512;
    if ((fd->pos % clus_bytes) == 0) {
        u32 next = fat32_next_cluster(fd->cur_clus);
        if (next >= 0x0FFFFFF8) {
            u32 nc = fat32_alloc_cluster();
            if (nc == (u32)-1) return -1;
            fat_link(fd->cur_clus, nc);
            next = nc;
        }
        fd->cur_clus = next;
    }
    return (i64)n;
}


void vfs_init(void)
{
    memset(fd_table, 0, sizeof(fd_table));
}

/* Open a file by 8.3 name in dir_clus; return a file descriptor (>= 3) or -1. */
i64 vfs_open_in(const char *name83, u32 dir_clus)
{
    /* fd 0/1/2 are reserved for stdin/stdout/stderr. */
    for (int i = 3; i < MAX_FILES; i++) {
        if (!fd_table[i].in_use) {
            if (fat32_open_raw_in(name83, &fd_table[i], dir_clus) != 0) return -1;
            fd_table[i].in_use = 1;
            return i;
        }
    }
    return -1;   /* too many open files */
}

i64 fat32_vfs_open(const char *name83)
{
    return vfs_open_in(name83, fat32_root_clus);
}

i64 fat32_vfs_read(u64 fd, void *buf, usize n)
{
    if (fd >= MAX_FILES || !fd_table[fd].in_use) return -1;
    return fat32_read_raw(&fd_table[fd], buf, n);
}

i64 fat32_vfs_write(u64 fd, const void *buf, usize n)
{
    if (fd >= MAX_FILES || !fd_table[fd].in_use) return -1;
    return fat32_write_raw(&fd_table[fd], buf, n);
}

i64 fat32_vfs_close(u64 fd)
{
    if (fd >= MAX_FILES || !fd_table[fd].in_use) return -1;
    fd_table[fd].in_use = 0;
    return 0;
}


/* Seek to an absolute byte offset within an open file.
 * Walks the FAT chain from start_clus to land on the correct cluster.
 * Returns the new position on success, or a negative error code. */
i64 fat32_vfs_seek(u64 fd, i64 offset, u64 whence)
{
    if (fd >= MAX_FILES || !fd_table[fd].in_use) return (i64)EBADF;

    FD *f = &fd_table[fd];
    u32 new_pos;

    if      (whence == 0) new_pos = (u32)offset;                  /* SEEK_SET */
        else if (whence == 1) new_pos = f->pos  + (u32)offset;        /* SEEK_CUR */
            else if (whence == 2) new_pos = f->size + (u32)offset;        /* SEEK_END */
                else return (i64)EINVAL;

                if (new_pos > f->size) new_pos = f->size;

                /* Walk the FAT chain to find the cluster that owns new_pos. */
                u32 clus_bytes      = (u32)fat32_spc * 512;
            u32 target_clus_idx = new_pos / clus_bytes;
        u32 clus            = f->start_clus;

    for (u32 i = 0; i < target_clus_idx && clus < 0x0FFFFFF8; i++)
        clus = fat32_next_cluster(clus);

    f->cur_clus = clus;
    f->pos      = new_pos;
    return (i64)new_pos;
}


/* Rename a file in dir_clus (both names must already be in 8.3 format).
 * Returns 0 on success, -1 if src not found or dst name already exists. */
i64 fat32_rename_in(const char *old83, const char *new83, u32 dir_clus)
{
    /* Reject if the destination name already exists. */
    u32 dummy_clus, dummy_size;
    if (fat32_find_raw_in(new83, &dummy_clus, &dummy_size, dir_clus) == 0)
        return -1;

    u32 clus = dir_clus;
    while (clus < 0x0FFFFFF8) {
        u32 lba = cluster_to_lba(clus);

        for (u8 s = 0; s < fat32_spc; s++) {
            ata_read_sector(lba + s, dir_buf);

            for (int e = 0; e < 16; e++) {
                u8 *de = dir_buf + e * 32;
                if (de[0] == 0x00) return -1;    /* end of directory */
                    if (de[0] == 0xE5) continue;      /* deleted          */
                        if (de[11] & 0x08) continue;      /* volume label     */

                            if (strncmp((char *)de, old83, 11) == 0) {
                                memcpy(de, new83, 11);   /* overwrite the name field in-place */
                                ata_write_sector(lba + s, dir_buf);
                                return 0;
                            }
            }
        }
        clus = fat32_next_cluster(clus);
    }
    return -1;
}

i64 fat32_rename(const char *old83, const char *new83)
{
    return fat32_rename_in(old83, new83, fat32_root_clus);
}


/* ================================================================
 *  FAT32 VFS OPS  (inode_ops_t bridge → raw FAT32 layer)
 *
 *  inode->ino      holds the cluster number (root = fat32_root_clus).
 *  inode->fs_data  holds a heap-allocated FD for open files.
 * ================================================================ */

/* Convert a long filename to an 8.3 name: pad with spaces, uppercase, dot stripped. */
static void to_83(const char *name, char out[11])
{
    memset(out, ' ', 11);
    int i = 0, j = 0;

    /* Copy up to 8 characters of the base name. */
    while (name[i] && name[i] != '.' && j < 8) {
        char c = name[i++];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        out[j++] = c;
    }

    /* Skip any remaining characters before the dot. */
    while (name[i] && name[i] != '.') i++;

    /* Copy up to 3 characters of the extension. */
    if (name[i] == '.') {
        i++;
        int k = 8;
        while (name[i] && k < 11) {
            char c = name[i++];
            if (c >= 'a' && c <= 'z') c = (char)(c - 32);
            out[k++] = c;
        }
    }
}

static i64 fat32_ops_read(struct vfs_inode *inode, void *buf, usize count, usize offset)
{
    FD *f = (FD *)inode->fs_data;
    if (!f) return (i64)EINVAL;

    /* Seek to the requested offset if the internal cursor is elsewhere. */
    if (f->pos != (u32)offset) {
        u32 clus_bytes  = (u32)fat32_spc * 512;
        u32 target_idx  = (u32)offset / clus_bytes;
        u32 clus        = f->start_clus;
        for (u32 i = 0; i < target_idx && clus < 0x0FFFFFF8; i++)
            clus = fat32_next_cluster(clus);
        f->cur_clus = clus;
        f->pos      = (u32)offset;
    }

    usize done = 0;
    while (done < count) {
        i64 n = fat32_read_raw(f, (u8 *)buf + done, count - done);
        if (n <= 0) break;
        done += (usize)n;
    }
    return (i64)done;
}

static i64 fat32_ops_write(struct vfs_inode *inode, const void *buf, usize count, usize offset)
{
    FD *f = (FD *)inode->fs_data;
    if (!f) return (i64)EINVAL;

    if (f->pos != (u32)offset) {
        u32 clus_bytes = (u32)fat32_spc * 512;
        u32 target_idx = (u32)offset / clus_bytes;
        u32 clus       = f->start_clus ? f->start_clus : fat32_root_clus;
        for (u32 i = 0; i < target_idx && clus < 0x0FFFFFF8; i++)
            clus = fat32_next_cluster(clus);
        f->cur_clus = clus;
        f->pos      = (u32)offset;
    }

    usize done = 0;
    while (done < count) {
        i64 n = fat32_write_raw(f, (const u8 *)buf + done, count - done);
        if (n <= 0) break;
        done += (usize)n;
    }
    inode->attr.size = f->size;
    return (i64)done;
}

static i64 fat32_ops_readdir(struct vfs_inode *inode, void *buf, usize count, usize *offset)
{
    u32   clus    = (u32)inode->ino;
    usize written = 0;
    u8    tmp[32];

    while (clus < 0x0FFFFFF8 && written + 64 <= count) {
        u32 lba = cluster_to_lba(clus);

        for (u8 s = 0; s < fat32_spc; s++) {
            ata_read_sector(lba + s, dir_buf);

            for (int e = 0; e < 16; e++) {
                u8 *de = dir_buf + e * 32;
                if (de[0] == 0x00) goto rdone;
                if (de[0] == 0xE5) continue;
                if (de[11] & 0x08) continue;   /* volume label */
                    if (de[11] & 0x10) continue;   /* subdirectory (skip for now) */
                        if (written + 64 > count) goto rdone;

                        /* Build a null-terminated display name from the 8.3 fields. */
                        memset(tmp, 0, 64);
                    int ni = 0;
                for (int k = 0; k < 8 && de[k] != ' '; k++) tmp[ni++] = de[k];
                if (de[8] != ' ') {
                    tmp[ni++] = '.';
                    for (int k = 8; k < 11 && de[k] != ' '; k++) tmp[ni++] = de[k];
                }
                memcpy((u8 *)buf + written, tmp, 64);
                written += 64;
            }
        }
        clus = fat32_next_cluster(clus);
    }
    rdone:
    *offset += written;
    return (i64)written;
}

static i64 fat32_ops_lookup(struct vfs_inode *dir_inode, const char *name, struct vfs_inode *out)
{
    char n83[11];
    to_83(name, n83);

    u32 dir_clus = (u32)dir_inode->ino;
    u32 out_clus, out_size;

    if (fat32_find_raw_in(n83, &out_clus, &out_size, dir_clus) < 0)
        return (i64)ENOENT;

    /* Allocate an FD to track this file's read/write state. */
    FD *f = (FD *)heap_malloc(sizeof(FD));
    if (!f) return (i64)ENOMEM;

    memset(f, 0, sizeof(FD));
    f->in_use     = 1;
    f->size       = out_size;
    f->start_clus = out_clus;
    f->cur_clus   = out_clus;
    f->pos        = 0;
    f->dir_clus   = dir_clus;
    memcpy(f->name83, n83, 11);

    out->valid    = 1;
    out->ino      = out_clus;
    out->dev      = dir_inode->dev;
    out->rdev     = 0;
    out->refcount = 1;
    out->ops      = dir_inode->ops;
    out->fs_data  = f;
    out->mount    = dir_inode->mount;
    out->attr.type    = 1;        /* IT_FILE */
    out->attr.mode    = 0644;
    out->attr.uid     = 0;
    out->attr.gid     = 0;
    out->attr.size    = out_size;
    out->attr.nlink   = 1;
    out->attr.blksize = 512;
    out->attr.blocks  = (out_size + 511) / 512;
    return 0;
}

static i64 fat32_ops_create(struct vfs_inode *dir_inode, const char *name, u16 mode, struct vfs_inode *out)
{
    char n83[11];
    to_83(name, n83);
    fat32_create_file_in(n83, (u32)dir_inode->ino);
    return fat32_ops_lookup(dir_inode, name, out);
}

static i64 fat32_ops_mkdir(struct vfs_inode *dir_inode, const char *name, u16 mode)
{
    char n83[11];
    to_83(name, n83);
    u32 c = fat32_alloc_cluster();
    if (c == (u32)-1) return (i64)ENOSPC;
    fat32_create_dir_entry_in(n83, c, (u32)dir_inode->ino);
    return 0;
}

static i64 fat32_ops_unlink(struct vfs_inode *dir_inode, const char *name)
{
    char n83[11];
    to_83(name, n83);
    i64 r = fat32_delete_file_in(n83, (u32)dir_inode->ino);
    return (r < 0) ? (i64)ENOENT : 0;
}

static i64 fat32_ops_getsize(struct vfs_inode *inode)
{
    FD *f = (FD *)inode->fs_data;
    return f ? (i64)f->size : (i64)inode->attr.size;
}

static i64 fat32_ops_setattr(struct vfs_inode *inode, inode_attr_t *attr)
{
    inode->attr = *attr;
    return 0;
}

/* Jump table of FAT32 operations for the VFS layer. */
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

/* Mount the FAT32 filesystem at "/" in the VFS layer.
 * Must be called after fat32_init(). */
void vfs_register_fat32(void)
{
    FD *root_fd = (FD *)heap_malloc(sizeof(FD));
    if (!root_fd) return;

    memset(root_fd, 0, sizeof(FD));
    root_fd->in_use    = 1;
    root_fd->start_clus = fat32_root_clus;
    root_fd->cur_clus   = fat32_root_clus;
    root_fd->dir_clus   = fat32_root_clus;

    vfs_inode_t root;
    memset(&root, 0, sizeof(root));
    root.valid        = 1;
    root.ino          = fat32_root_clus;
    root.dev          = 1;
    root.refcount     = 1;
    root.ops          = &fat32_inode_ops;
    root.fs_data      = root_fd;
    root.attr.type    = 2;     /* IT_DIR */
    root.attr.mode    = 0755;
    root.attr.nlink   = 1;
    root.attr.blksize = 512;

    vfs_mount_fs("/", "fat32", 1, &root);
}


/* ================================================================
 *  SHARED KEYBOARD SCANCODE TABLES
 *
 *  Defined once here and used by read_key_raw(), read_key(), and
 *  the GUI event loop — previously each site had its own private
 *  copy, which risked divergence between them.
 * ================================================================ */

static const u8 kb_sc[0x3A] = {
    0,  0, '1','2','3','4','5','6','7','8','9','0','-','=','\b','\t',
    'q','w','e','r','t','y','u','i','o','p','[',']','\r', 0,
    'a','s','d','f','g','h','j','k','l',';','\'','`',  0, '\\',
    'z','x','c','v','b','n','m',',','.','/', 0, '*',  0, ' '
};

static const u8 kb_sc_shift[0x3A] = {
    0,  0, '!','@','#','$','%','^','&','*','(',')','_','+','\b','\t',
    'Q','W','E','R','T','Y','U','I','O','P','{','}','\r', 0,
    'A','S','D','F','G','H','J','K','L',':','"','~',  0, '|',
    'Z','X','C','V','B','N','M','<','>','?', 0, '*',  0, ' '
};

/* Translate a raw scancode to ASCII, applying shift and caps-lock state.
 * Returns the ASCII character, or 0 if the key has no printable mapping. */
static inline u8 kb_translate(u8 sc, u8 shift, u8 caps)
{
    if (sc >= 0x3A) return 0;

    u8 ch = shift ? kb_sc_shift[sc] : kb_sc[sc];
    if (!ch) return 0;

    /* Caps Lock inverts the case of letters (including shifted ones). */
    if (caps && ch >= 'a' && ch <= 'z') ch = (u8)(ch - 32);
    else if (caps && ch >= 'A' && ch <= 'Z') ch = (u8)(ch + 32);

    return ch;
}


/* ================================================================
 *  KEYBOARD DRIVER  (PS/2 polled)
 *
 *  Features:
 *    - Caps Lock toggle (scancode 0x3A / 0xBA)
 *    - Ctrl key tracking (Left 0x1D, Right 0xE0 0x1D)
 *    - Arrow keys returned as private codes KEY_UP .. KEY_RIGHT
 *    - Delete key returned as KEY_DEL (0x7F)
 *    - read_key_raw() / read_key() split so callers that need raw
 *      codes (e.g. read_line) can get them directly
 * ================================================================ */

#define KEY_UP    0x80
#define KEY_DOWN  0x81
#define KEY_LEFT  0x82
#define KEY_RIGHT 0x83
#define KEY_DEL   0x7F

static u8 shift_flag = 0;   /* Shift held            */
static u8 caps_flag  = 0;   /* Caps Lock toggle      */
static u8 ctrl_flag  = 0;   /* Ctrl held             */
static u8 e0_flag    = 0;   /* 0xE0 prefix received  */


/* Read one keypress; return printable ASCII, \r, \b, or a KEY_* code.
 * Also handles scroll side-effects (PageUp/PageDown).
 * Exported for sys_read (stdin) in syscall.c. */
u8 read_key_raw(void)
{
    for (;;) {
        /* Poll until keyboard data (bit 0) or mouse data (bit 5) arrives. */
        u8 status;
        while (!((status = inb(0x64)) & 1)) {
            if (status & 0x20) mouse_poll();
        }

        /* If a mouse byte sneaked in just before the keyboard byte, drain it. */
        if (inb(0x64) & 0x20) { mouse_poll(); continue; }

        u8 sc = inb(0x60);

        /* Extended scancode prefix. */
        if (sc == 0xE0) { e0_flag = 1; continue; }

        /* Shift press / release. */
        if (sc == 0x2A || sc == 0x36) { shift_flag = 1; e0_flag = 0; input_mod_update(shift_flag, ctrl_flag, caps_flag); continue; }
        if (sc == 0xAA || sc == 0xB6) { shift_flag = 0; e0_flag = 0; input_mod_update(shift_flag, ctrl_flag, caps_flag); continue; }

        /* Ctrl press / release. */
        if (sc == 0x1D) { ctrl_flag = 1; e0_flag = 0; input_mod_update(shift_flag, ctrl_flag, caps_flag); continue; }
        if (sc == 0x9D) { ctrl_flag = 0; e0_flag = 0; input_mod_update(shift_flag, ctrl_flag, caps_flag); continue; }

        /* Caps Lock press (toggle) and release (ignore). */
        if (sc == 0x3A) { caps_flag ^= 1; e0_flag = 0; input_mod_update(shift_flag, ctrl_flag, caps_flag); continue; }
        if (sc == 0xBA) { e0_flag = 0; continue; }

        /* Any key-release scancode (bit 7 set) — stop held-key repeat. */
        if (sc & 0x80) { input_key_release(); e0_flag = 0; continue; }

        /* Extended keys following 0xE0 prefix. */
        if (e0_flag) {
            e0_flag = 0;
            if (sc == 0x48) { input_push_key(0x80 | 0x48, KEY_UP);    return KEY_UP;    }
            if (sc == 0x50) { input_push_key(0x80 | 0x50, KEY_DOWN);  return KEY_DOWN;  }
            if (sc == 0x4B) { input_push_key(0x80 | 0x4B, KEY_LEFT);  return KEY_LEFT;  }
            if (sc == 0x4D) { input_push_key(0x80 | 0x4D, KEY_RIGHT); return KEY_RIGHT; }
            if (sc == 0x53) { input_push_key(0x80 | 0x53, KEY_DEL);   return KEY_DEL;   }
            if (sc == 0x1D) { ctrl_flag = 1; continue; }
            if (sc == 0x9D) { ctrl_flag = 0; continue; }
            continue;
        }

        if (sc >= 0x3A) continue;   /* unknown/function key — ignore */

            u8 ch = kb_translate(sc, shift_flag, caps_flag);
        if (!ch) continue;

        input_push_key(sc, ch);
        return ch;
    }
}

/* Shell variant of read_key_raw(): also pets the watchdog and handles PageUp/
 * PageDown scrolling.  Snaps the view back to live on any regular keypress. */
static u8 read_key(void)
{
    for (;;) {
        u8 status;
        while (!((status = inb(0x64)) & 1)) {
            if (status & 0x20) mouse_poll();
            watchdog_pet();   /* shell is alive, just waiting for input */
        }

        if (inb(0x64) & 0x20) { mouse_poll(); continue; }

        u8 sc = inb(0x60);

        if (sc == 0xE0) { e0_flag = 1; continue; }

        /* Modifier keys. */
        if (sc == 0x2A || sc == 0x36) { shift_flag = 1; e0_flag = 0; continue; }
        if (sc == 0xAA || sc == 0xB6) { shift_flag = 0; e0_flag = 0; continue; }
        if (sc == 0x1D) { ctrl_flag = 1; e0_flag = 0; continue; }
        if (sc == 0x9D) { ctrl_flag = 0; e0_flag = 0; continue; }
        if (sc == 0x3A) { caps_flag ^= 1; e0_flag = 0; continue; }
        if (sc == 0xBA) { e0_flag = 0; continue; }

        /* Key release — ignore. */
        if (sc & 0x80) { e0_flag = 0; continue; }

        /* Extended keys (0xE0 prefix). */
        if (e0_flag) {
            e0_flag = 0;
            if (sc == 0x48) return KEY_UP;
            if (sc == 0x50) return KEY_DOWN;
            if (sc == 0x4B) return KEY_LEFT;
            if (sc == 0x4D) return KEY_RIGHT;
            if (sc == 0x53) return KEY_DEL;
            if (sc == 0x49) { vga_scroll_up();   continue; }   /* PageUp   */
                if (sc == 0x51) { vga_scroll_down(); continue; }   /* PageDown */
                    if (sc == 0x1D) { ctrl_flag = 1;     continue; }
                    if (sc == 0x9D) { ctrl_flag = 0;     continue; }
                    continue;
        }

        if (sc >= 0x3A) continue;

        u8 ch = kb_translate(sc, shift_flag, caps_flag);
        if (!ch) continue;

        /* Any printable keypress snaps the view back to live. */
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

#define HIST_MAX  16    /* maximum number of history entries */
#define HIST_LEN  128   /* maximum length of each entry      */

static char hist_buf[HIST_MAX][HIST_LEN];
static int  hist_count = 0;   /* entries stored (capped at HIST_MAX) */
static int  hist_head  = 0;   /* next write slot (ring index)        */

static void hist_push(const char *s)
{
    if (!s || !s[0]) return;

    /* Don't duplicate the most recent entry. */
    if (hist_count > 0) {
        int prev = (hist_head - 1 + HIST_MAX) % HIST_MAX;
        if (strncmp(hist_buf[prev], s, HIST_LEN - 1) == 0) return;
    }

    int i = 0;
    while (i < HIST_LEN - 1 && s[i]) {
        hist_buf[hist_head][i] = s[i];
        i++;
    }
    hist_buf[hist_head][i] = 0;

    hist_head = (hist_head + 1) % HIST_MAX;
    if (hist_count < HIST_MAX) hist_count++;
}

/* Return a pointer to the entry 'back' steps ago (1 = newest), or NULL. */
static const char *hist_get(int back)
{
    if (back < 1 || back > hist_count) return NULL;
    int idx = (hist_head - back + HIST_MAX * 2) % HIST_MAX;
    return hist_buf[idx];
}


/* ── Line-editing helpers ────────────────────────────────────────── */

/* Move the terminal cursor left by n positions. */
static void cur_left(int n)
{
    for (int i = 0; i < n; i++) vga_backspace();
}

/* Erase n characters: move left, overwrite with space, move left again. */
static void erase_chars(int n)
{
    for (int i = 0; i < n; i++) {
        vga_backspace();
        vga_putchar(' ');
        vga_backspace();
    }
}


/* Read a line of input into buf (max bytes), with full line-editing:
 *   - Left/Right arrows move the cursor within the line.
 *   - Backspace / Delete remove characters.
 *   - Up/Down arrows navigate command history.
 *   - Enter submits the line and pushes it to history. */
static void read_line(char *buf, int max)
{
    int n        = 0;   /* current length of the line    */
    int cur      = 0;   /* cursor position within buf    */
    int hist_pos = 0;   /* 0 = live input; > 0 = history */

    for (;;) {
        u8 c = read_key();

        /* Enter — submit the line. */
        if (c == '\r') {
            buf[n] = 0;
            vga_putchar('\n');
            hist_push(buf);
            return;
        }

        /* Backspace — delete the character before the cursor. */
        if (c == '\b') {
            if (cur > 0) {
                /* Shift buffer left by one from the cursor. */
                for (int i = cur - 1; i < n - 1; i++) buf[i] = buf[i + 1];
                n--;
                cur--;
                vga_backspace();
                /* Redraw the tail and erase the now-extra character at the end. */
                for (int i = cur; i < n; i++) vga_putchar((u8)buf[i]);
                vga_putchar(' ');
                cur_left(n - cur + 1);
            }
            continue;
        }

        /* Delete — forward-delete the character at the cursor. */
        if (c == KEY_DEL) {
            if (cur < n) {
                for (int i = cur; i < n - 1; i++) buf[i] = buf[i + 1];
                n--;
                for (int i = cur; i < n; i++) vga_putchar((u8)buf[i]);
                vga_putchar(' ');
                cur_left(n - cur + 1);
            }
            continue;
        }

        /* Left arrow — move cursor back one position. */
        if (c == KEY_LEFT) {
            if (cur > 0) { cur--; vga_backspace(); }
            continue;
        }

        /* Right arrow — move cursor forward one position. */
        if (c == KEY_RIGHT) {
            if (cur < n) { vga_putchar((u8)buf[cur]); cur++; }
            continue;
        }

        /* Up arrow — recall an older history entry. */
        if (c == KEY_UP) {
            int next = hist_pos + 1;
            const char *h = hist_get(next);
            if (!h) continue;

            /* Move to end of line, then erase everything back to start. */
            for (int i = cur; i < n; i++) vga_putchar((u8)buf[i]);
            erase_chars(n);

            hist_pos = next;
            n = 0; cur = 0;

            const char *p = h;
            while (*p && n < max - 1) { buf[n++] = *p; vga_putchar((u8)*p); p++; }
            cur = n;
            continue;
        }

        /* Down arrow — move to a newer history entry (or back to live input). */
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

        /* Regular printable character — insert at cursor position. */
        if (c < 0x20 || c >= KEY_UP) continue;   /* skip non-printable */
            if (n >= max - 1) continue;               /* line buffer full   */

                hist_pos = 0;   /* typing cancels history navigation */

                /* Shift the buffer right to make room at cur. */
                for (int i = n; i > cur; i--) buf[i] = buf[i - 1];
                buf[cur] = (char)c;
        n++;

        /* Redraw from the cursor to the end of line. */
        for (int i = cur; i < n; i++) vga_putchar((u8)buf[i]);
        cur_left(n - cur - 1);
        cur++;
    }
}


/* ================================================================
 *  8.3 FILENAME FORMATTING
 * ================================================================ */

/* Convert src to an 11-character FAT 8.3 name in dst (space-padded, uppercase). */
void format_83_name(const char *src, char *dst)
{
    int i, j;

    for (i = 0; i < 11; i++) dst[i] = ' ';
    dst[11] = 0;

    /* Base name: up to 8 characters. */
    for (i = 0, j = 0; j < 8 && src[i] && src[i] != '.' && src[i] != ' '; i++, j++) {
        u8 c = (u8)src[i];
        dst[j] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : (char)c;
    }

    /* Skip any overflow characters before the dot. */
    while (src[i] && src[i] != '.') i++;

    /* Extension: up to 3 characters. */
    if (src[i] == '.') {
        i++;
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

/* Current working directory: cwd_clus == 0 means the root. */
static u32  cwd_clus = 0;
static char cwd_path[128] = "/";


/* ── Shell VFS helpers ───────────────────────────────────────────── */

/* Build an absolute path from cwd_path + name into out. */
static void sh_fullpath(const char *name, char *out, usize outsz)
{
    if (name[0] == '/') {
        /* Already absolute — use as-is. */
        usize n = strlen(name);
        if (n >= outsz) n = outsz - 1;
        memcpy(out, name, n);
        out[n] = 0;
        return;
    }

    usize clen = strlen(cwd_path);
    usize nlen = strlen(name);

    if (clen + 1 + nlen + 1 > outsz) { out[0] = 0; return; }

    memcpy(out, cwd_path, clen);
    if (cwd_path[clen - 1] != '/') out[clen++] = '/';
    memcpy(out + clen, name, nlen + 1);
}

static i64 sh_open(const char *name)
{
    char p[256]; sh_fullpath(name, p, sizeof(p));
    return vfs_open(p);
}

static i64 sh_create(const char *name, u16 mode)
{
    char p[256]; sh_fullpath(name, p, sizeof(p));
    return vfs_create(p, mode);
}

static i64 sh_unlink(const char *name)
{
    char p[256]; sh_fullpath(name, p, sizeof(p));
    return vfs_unlink(p);
}

static i64 sh_mkdir(const char *name, u16 mode)
{
    char p[256]; sh_fullpath(name, p, sizeof(p));
    return vfs_mkdir(p, mode);
}

/* read/write/close pass directly through to the VFS layer. */
#define sh_read(fd, buf, n)   vfs_read((u64)(fd), (buf), (n))
#define sh_write(fd, buf, n)  vfs_write((u64)(fd), (buf), (n))
#define sh_close(fd)          vfs_close((u64)(fd))


/* Skip the first word in s; return a pointer to the next word, or NULL. */
static const char *get_arg(const char *s)
{
    while (*s && *s != ' ') s++;
    while (*s == ' ') s++;
    return *s ? s : NULL;
}

/* Print an unsigned 32-bit integer in decimal. */
static void print_u32(u32 n)
{
    char buf[12];
    ksnprintf(buf, sizeof(buf), "%u", (unsigned)n);
    print_str(buf);
}

/* Print a byte size in human-readable form (B / KB / MB). */
static void print_size_human(u32 sz)
{
    char buf[32];
    if (sz < 1024) {
        ksnprintf(buf, sizeof(buf), "%u B", (unsigned)sz);
    } else if (sz < 1024 * 1024) {
        ksnprintf(buf, sizeof(buf), "%u.%u KB",
                  (unsigned)(sz / 1024),
                  (unsigned)((sz % 1024) * 10 / 1024));
    } else {
        ksnprintf(buf, sizeof(buf), "%u.%u MB",
                  (unsigned)(sz / (1024 * 1024)),
                  (unsigned)((sz % (1024 * 1024)) * 10 / (1024 * 1024)));
    }
    print_str(buf);
}

/* Print an unsigned 32-bit integer right-aligned in a field of 'width' chars. */
static void print_u32_w(u32 n, int width)
{
    char num[12];
    int  len = 0;

    if (n == 0) {
        num[len++] = '0';
    } else {
        char tmp[12];
        int  t = 0;
        u32  v = n;
        while (v) { tmp[t++] = (char)('0' + v % 10); v /= 10; }
        for (int i = t - 1; i >= 0; i--) num[len++] = tmp[i];
    }

    /* Left-pad with spaces. */
    for (int i = len; i < width; i++) vga_putchar(' ');
    for (int i = 0;   i < len;   i++) vga_putchar((u8)num[i]);
}

/* Return the active directory cluster (root if cwd_clus is 0). */
static u32 cwd_active(void)
{
    return (cwd_clus == 0) ? fat32_root_clus : cwd_clus;
}


/* ---- meminfo ---------------------------------------------------- */
static void cmd_meminfo(void)
{
    u32 free = pmm_free_pages();

    /* Use the E820 RAM ceiling, not the compile-time TOTAL_PAGES macro,
     * so meminfo reports the actual amount of RAM in the machine. */
    u64 actual_pages = (ram_end_actual > RAM_START)
    ? (ram_end_actual - RAM_START) / PAGE_SIZE : 0;

    u64 total   = actual_pages;
    u64 used    = (total > free) ? (total - free) : 0;
    u64 total_mb = (total * 4) / 1024;
    u64 used_mb  = (used  * 4) / 1024;
    u64 free_mb  = (free  * 4ULL) / 1024;

    print_str("Memory (4 KB pages):\r\n");
    print_str("  Total : "); print_u32((u32)total); print_str(" pages ("); print_u32((u32)total_mb); print_str(" MB)\r\n");
    print_str("  Used  : "); print_u32((u32)used);  print_str(" pages ("); print_u32((u32)used_mb);  print_str(" MB)\r\n");
    print_str("  Free  : "); print_u32(free);        print_str(" pages ("); print_u32((u32)free_mb);  print_str(" MB)\r\n");
    print_str("  RAM ceiling (E820 high): ");
    print_u32((u32)(ram_end_actual / (1024 * 1024)));
    print_str(" MB\r\n");
}

/* ---- uptime ----------------------------------------------------- */
static void cmd_uptime(void)
{
    u32 secs  = (u32)(pit_ticks / 1000);
    u32 mins  = secs / 60; secs %= 60;
    u32 hours = mins / 60; mins %= 60;

    print_str("Uptime: ");
    print_u32(hours); print_str("h ");
    print_u32(mins);  print_str("m ");
    print_u32(secs);  print_str("s\r\n");
}

/* ---- uname ------------------------------------------------------ */
static void cmd_uname(void)
{
    print_str("Systrix 0.1 x86-64 (microkernel, preemptive, FAT32, ring-3 ELF)\r\n");
}

/* ---- sysinfo ---------------------------------------------------- */
static void cmd_sysinfo(void)
{
    print_str("=== Systrix System Info ===\r\n");
    print_str("OS      : Systrix v0.1\r\n");
    print_str("Arch    : x86-64\r\n");
    print_str("Display : bochs-display (QEMU)\r\n");

    int fw = fb_get_width();
    int fh = fb_get_height();
    if (fw > 0 && fh > 0) {
        print_str("FB Res  : ");
        print_u32((u32)fw);
        print_str("x");
        print_u32((u32)fh);
        print_str(" @ 32bpp  [OK]\r\n");
    } else {
        print_str("FB Res  : not initialized (run 'gui' first)\r\n");
    }

    print_str("RAM     : 512M (QEMU -m 512M)\r\n");
    print_str("Shell   : type 'gui' to launch desktop\r\n");
    print_str("===========================\r\n");
}


/* ================================================================
 *  FILE MANAGEMENT COMMANDS
 * ================================================================ */

/* ---- ls --------------------------------------------------------- */
static void cmd_ls(void)
{
    u32 clus        = cwd_active();
    int file_count  = 0;
    int dir_count   = 0;
    u32 total_bytes = 0;

    print_str("Name           Type       Size\r\n");
    print_str("----           ----       ----\r\n");

    while (clus < 0x0FFFFFF8) {
        watchdog_pet();   /* ATA reads can be slow under QEMU TCG */
        u32 lba = cluster_to_lba(clus);

        for (u8 s = 0; s < fat32_spc; s++) {
            ata_read_sector(lba + s, dir_buf);

            for (int e = 0; e < 16; e++) {
                u8 *de = dir_buf + e * 32;

                if (de[0] == 0x00) goto ls_done;
                if (de[0] == 0xE5) continue;

                u8 attr = de[11];
                if (attr == 0x0F)  continue;   /* LFN entry   */
                    if (attr & 0x08)   continue;   /* volume label */
                        if (attr & 0x02)   continue;   /* hidden       */
                            if (de[0] == '.')  continue;   /* . and ..     */

                                /* Build the display name from the 8.3 fields. */
                                char name[13];
                            int  ni = 0;
                        for (int k = 0; k < 8 && de[k] != ' '; k++) name[ni++] = (char)de[k];
                        if (de[8] != ' ') {
                            name[ni++] = '.';
                            for (int k = 8; k < 11 && de[k] != ' '; k++) name[ni++] = (char)de[k];
                        }
                        name[ni] = 0;

                        int is_dir = (attr & 0x10) ? 1 : 0;

                        /* Print the name, padded to 15 characters. */
                        print_str(name);
                        if (is_dir) vga_putchar('/');

                        int used = ni + is_dir;
                int pad  = (used < 15) ? 15 - used : 0;
                char pad_buf[16];
                for (int k = 0; k < pad; k++) pad_buf[k] = ' ';
                pad_buf[pad] = '\0';
                print_str(pad_buf);

                if (is_dir) {
                    print_str("<DIR>           -\r\n");
                    dir_count++;
                } else {
                    u32 sz = *(u32 *)(de + 28);
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
    { char fbuf[32]; ksnprintf(fbuf, sizeof(fbuf), "%d file(s), ", file_count); print_str(fbuf); }
    { char dbuf[32]; ksnprintf(dbuf, sizeof(dbuf), "%d dir(s)   total: ", dir_count); print_str(dbuf); }
    print_size_human(total_bytes);
    print_str("\r\n");
}


/* ---- cd --------------------------------------------------------- */
static void cmd_cd(const char *name)
{
    /* "cd" or "cd /" both jump to the root. */
    if (!name || (name[0] == '/' && name[1] == 0)) {
        cwd_clus = 0;
        cwd_path[0] = '/'; cwd_path[1] = 0;
        return;
    }

    /* ".." goes up one level — we currently only support one level of dirs. */
    if (name[0] == '.' && name[1] == '.' && name[2] == 0) {
        cwd_clus = 0;
        cwd_path[0] = '/'; cwd_path[1] = 0;
        return;
    }

    char n83[12];
    format_83_name(name, n83);

    /* Scan the current directory for a matching directory entry. */
    u32 clus = cwd_active();
    while (clus < 0x0FFFFFF8) {
        u32 lba = cluster_to_lba(clus);

        for (u8 s = 0; s < fat32_spc; s++) {
            ata_read_sector(lba + s, dir_buf);

            for (int e = 0; e < 16; e++) {
                u8 *de = dir_buf + e * 32;

                if (de[0] == 0x00) goto cd_notfound;
                if (de[0] == 0xE5) continue;
                if (!(de[11] & 0x10)) continue;   /* must be a directory */
                    if (de[0] == '.') continue;

                    /* Compare the 8.3 name. */
                    int match = 1;
                for (int k = 0; k < 11; k++) {
                    if (de[k] != (u8)n83[k]) { match = 0; break; }
                }
                if (!match) continue;

                /* Found — update cwd_clus and cwd_path. */
                u32 hi  = *(u16 *)(de + 20);
                u32 lo  = *(u16 *)(de + 26);
                cwd_clus = (hi << 16) | lo;
                if (cwd_path[1] != '\0') strlcat(cwd_path, "/", sizeof(cwd_path));
                strlcat(cwd_path, name, sizeof(cwd_path));
                return;
            }
        }
        clus = fat32_next_cluster(clus);
    }

    cd_notfound:
    print_str("cd: no such directory\r\n");
}


/* ---- stat ------------------------------------------------------- */
static void cmd_stat(const char *name)
{
    char n83[12];
    format_83_name(name, n83);

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
                for (int k = 0; k < 11; k++) {
                    if (de[k] != (u8)n83[k]) { match = 0; break; }
                }
                if (!match) continue;

                /* Found — print the entry details. */
                print_str("File : "); print_str(name); print_str("\r\n");
                print_str("Type : "); print_str((attr & 0x10) ? "Directory" : "File"); print_str("\r\n");

                if (!(attr & 0x10)) {
                    u32 sz = *(u32 *)(de + 28);
                    print_str("Size : "); print_u32(sz); print_str(" B (");
                    print_size_human(sz); print_str(")\r\n");
                }

                u32 hi = *(u16 *)(de + 20);
                u32 lo = *(u16 *)(de + 26);
                print_str("Clus : "); print_u32((hi << 16) | lo); print_str("\r\n");

                /* Decode the FAT creation date/time fields. */
                u16 cdate = *(u16 *)(de + 16);
                u16 ctime = *(u16 *)(de + 14);
                u32 year  = 1980 + ((cdate >> 9) & 0x7F);
                u32 mon   = (cdate >> 5) & 0xF;
                u32 day   = cdate & 0x1F;
                u32 hour  = (ctime >> 11) & 0x1F;
                u32 min   = (ctime >> 5)  & 0x3F;

                char dbuf[32];
                ksnprintf(dbuf, sizeof(dbuf), "Date : %u-%02u-%02u  %02u:%02u\r\n",
                          year, mon, day, hour, min);
                print_str(dbuf);

                u8 ro = (attr & 0x01) ? 1 : 0;
                print_str("Attr : "); print_str(ro ? "read-only" : "read-write"); print_str("\r\n");
                return;
            }
        }
        clus = fat32_next_cluster(clus);
    }

    stat_notfound:
    print_str("stat: not found\r\n");
}


/* ---- df --------------------------------------------------------- */
static void cmd_df(void)
{
    u32 total_clus = fat32_total_clus;
    if (total_clus == 0) { print_str("df: filesystem not mounted\r\n"); return; }

    /* Count free clusters using the FAT sector cache to avoid hammering disk. */
    u32 free_clus = 0;
    for (u32 c = 2; c < total_clus + 2; c++) {
        u32 byte_off = c * 4;
        u32 fat_sec  = byte_off / 512 + fat32_fat_start;
        u32 fat_off  = byte_off % 512;
        fat_read_cached(fat_sec);
        u32 entry = *(u32 *)(fat_buf + fat_off) & 0x0FFFFFFF;
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


/* ---- find ------------------------------------------------------- */
static void cmd_find(const char *pattern)
{
    u32 clus  = cwd_active();
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

                /* Build the display name. */
                char name[13];
                int  ni = 0;
                for (int k = 0; k < 8 && de[k] != ' '; k++) name[ni++] = (char)de[k];
                if (de[8] != ' ') {
                    name[ni++] = '.';
                    for (int k = 8; k < 11 && de[k] != ' '; k++) name[ni++] = (char)de[k];
                }
                name[ni] = 0;

                /* Case-insensitive substring search. */
                if (pattern) {
                    int pi = 0, match = 0;
                    for (int k = 0; k <= ni && !match; k++) {
                        pi = 0;
                        while (pattern[pi] && k + pi <= ni) {
                            char a = name[k + pi];
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
                if (cwd_path[1] != 0) vga_putchar('/');   /* not root */
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


/* ---- head ------------------------------------------------------- */
static void cmd_head(const char *arg)
{
    const char *a2   = get_arg(arg);
    int         lines = 10;

    if (a2) {
        lines = 0;
        for (; *a2 >= '0' && *a2 <= '9'; a2++) lines = lines * 10 + (*a2 - '0');
        if (!lines) lines = 10;
    }

    i64 fd = sh_open(arg);
    if (fd < 0) { print_str("head: file not found\r\n"); return; }

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
    print_str("\r\n");
}


/* ---- tail ------------------------------------------------------- */
static void cmd_tail(const char *arg)
{
    const char *a2   = get_arg(arg);
    int         lines = 10;

    if (a2) {
        lines = 0;
        for (; *a2 >= '0' && *a2 <= '9'; a2++) lines = lines * 10 + (*a2 - '0');
        if (!lines) lines = 10;
    }

    i64 fd = sh_open(arg);
    if (fd < 0) { print_str("tail: file not found\r\n"); return; }

    /* Load the full file into a heap buffer. */
    u8   *buf  = (u8 *)heap_malloc(32768);
    if (!buf) { sh_close(fd); print_str("OOM\r\n"); return; }

    usize total = 0;
    for (;;) {
        i64 n = sh_read(fd, buf + total, 512);
        if (n <= 0) break;
        total += (usize)n;
        if (total >= 32768 - 512) break;
    }
    sh_close(fd);

    /* Walk backwards to find the start of the last N lines. */
    int   lc    = 0;
    usize start = total;
    while (start > 0 && lc < lines) {
        start--;
        if (buf[start] == '\n' && start < total - 1) lc++;
    }
    if (start > 0) start++;   /* skip the newline we stopped on */

        for (usize i = start; i < total; i++) vga_putchar(buf[i]);

        heap_free(buf);
    print_str("\r\n");
}


/* ---- append ----------------------------------------------------- */
static void cmd_append(const char *arg)
{
    const char *text = get_arg(arg);
    if (!text) { print_str("Usage: append <file> <text>\r\n"); return; }

    i64 fd = sh_open(arg);
    if (fd < 0) fd = sh_create(arg, 0644);
    if (fd < 0) { print_str("Error.\r\n"); return; }

    i64 sz = vfs_seek((u64)fd, 0, 2);   /* SEEK_END */
    (void)sz;

    usize tlen    = strlen(text);
    usize written = 0;
    while (written < tlen) {
        i64 n = sh_write(fd, text + written, tlen - written);
        if (n <= 0) break;
        written += (usize)n;
    }
    sh_write(fd, "\r\n", 2);
    sh_close(fd);
    print_str("Appended.\r\n");
}


/* ---- mkdir / rmdir ---------------------------------------------- */
static void cmd_mkdir(const char *name)
{
    if (sh_mkdir(name, 0755) == 0)
        print_str("Directory created.\r\n");
    else
        print_str("mkdir: failed\r\n");
}

static void cmd_rmdir(const char *name)
{
    char n83[12];
    format_83_name(name, n83);

    /* Find the directory cluster. */
    u32 clus     = cwd_active();
    u32 dir_clus = (u32)-1;

    while (clus < 0x0FFFFFF8) {
        u32 lba = cluster_to_lba(clus);

        for (u8 s = 0; s < fat32_spc; s++) {
            ata_read_sector(lba + s, dir_buf);

            for (int e = 0; e < 16; e++) {
                u8 *de = dir_buf + e * 32;

                if (de[0] == 0x00) goto rmdir_search_done;
                if (de[0] == 0xE5) continue;
                if (!(de[11] & 0x10)) continue;   /* must be a directory */
                    if (de[0] == '.') continue;

                    int match = 1;
                for (int k = 0; k < 11; k++) {
                    if (de[k] != (u8)n83[k]) { match = 0; break; }
                }
                if (!match) continue;

                u32 hi   = *(u16 *)(de + 20);
                u32 lo   = *(u16 *)(de + 26);
                dir_clus = (hi << 16) | lo;
                goto rmdir_search_done;
            }
        }
        clus = fat32_next_cluster(clus);
    }

    rmdir_search_done:
    if (dir_clus == (u32)-1) { print_str("rmdir: not found\r\n"); return; }

    /* Verify the directory is empty (no non-dot entries). */
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
    if (sh_unlink(name) == 0)
        print_str("Directory removed.\r\n");
    else
        print_str("rmdir: failed\r\n");
}


/* ---- wc --------------------------------------------------------- */
static void cmd_wc(const char *name)
{
    i64 fd = sh_open(name);
    if (fd < 0) { print_str("wc: file not found\r\n"); return; }

    u32 lines = 0, words = 0, bytes = 0;
    int in_word = 0;

    for (;;) {
        i64 n = sh_read(fd, cat_buf, 512);
        if (n <= 0) break;

        for (i64 i = 0; i < n; i++) {
            u8 c = cat_buf[i];
            bytes++;
            if (c == '\n') lines++;
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                in_word = 0;
            } else if (!in_word) {
                in_word = 1;
                words++;
            }
        }
    }
    sh_close(fd);

    char wbuf[48];
    ksnprintf(wbuf, sizeof(wbuf), "%u lines  %u words  %u bytes\r\n",
              lines, words, bytes);
    print_str(wbuf);
}


/* ---- cp --------------------------------------------------------- */
static void cmd_cp(const char *arg)
{
    const char *dst = get_arg(arg);
    if (!dst) { print_str("Usage: cp <src> <dst>\r\n"); return; }

    i64 fd_in = sh_open(arg);
    if (fd_in < 0) { print_str("cp: source not found\r\n"); return; }

    i64 fd_out = sh_open(dst);
    if (fd_out < 0) fd_out = sh_create(dst, 0644);
    if (fd_out < 0) { sh_close(fd_in); print_str("cp: cannot create dest\r\n"); return; }

    static u8 cp_buf[512];
    u32 total = 0;
    i64 n;
    while ((n = sh_read(fd_in, cp_buf, sizeof(cp_buf))) > 0) {
        sh_write(fd_out, cp_buf, (usize)n);
        total += (u32)n;
    }
    sh_close(fd_in);
    sh_close(fd_out);
    print_str("Copied "); print_size_human(total); print_str("\r\n");
}


/* ---- cat / hexdump / touch / rm / rename / write ---------------- */

static void cmd_cat(const char *name)
{
    i64 fd = sh_open(name);
    if (fd < 0) { print_str("File not found.\r\n"); return; }

    for (;;) {
        i64 n = sh_read(fd, cat_buf, 512);
        if (n <= 0) break;
        cat_buf[n] = 0;
        print_str((char *)cat_buf);
    }
    sh_close(fd);
    print_str("\r\n");
}

static void cmd_hexdump(const char *name)
{
    i64 fd = sh_open(name);
    if (fd < 0) { print_str("File not found.\r\n"); return; }

    int col = 0;
    for (;;) {
        i64 n = sh_read(fd, cat_buf, 512);
        if (n <= 0) break;
        for (i64 i = 0; i < n; i++) {
            print_hex_byte(cat_buf[i]);
            vga_putchar(' ');
            if (++col == 16) { col = 0; print_str("\r\n"); }
        }
    }
    if (col) print_str("\r\n");
    sh_close(fd);
}

static void cmd_touch(const char *name)
{
    i64 fd = sh_open(name);
    if (fd >= 0) { sh_close(fd); print_str("(exists)\r\n"); return; }
    fd = sh_create(name, 0644);
    if (fd >= 0) sh_close(fd);
    print_str("File created.\r\n");
}

static void cmd_rm(const char *name)
{
    if (sh_unlink(name) == 0)
        print_str("File deleted.\r\n");
    else
        print_str("File not found.\r\n");
}

static void cmd_rename(const char *arg)
{
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

static void cmd_write(const char *arg)
{
    const char *text = get_arg(arg);
    if (!text) { print_str("Usage: write <file> <text>\r\n"); return; }

    i64 fd = sh_open(arg);
    if (fd < 0) fd = sh_create(arg, 0644);
    if (fd < 0) { print_str("Error.\r\n"); return; }

    usize tlen    = strlen(text);
    usize written = 0;
    while (written < tlen) {
        i64 n = sh_write(fd, text + written, tlen - written);
        if (n <= 0) break;
        written += (usize)n;
    }
    sh_write(fd, "\r\n", 2);
    sh_close(fd);
    print_str("Written.\r\n");
}


/* ================================================================
 *  NETWORK COMMANDS
 * ================================================================ */

/* Parse a dotted-decimal IP string ("a.b.c.d") into a packed u32. */
static u32 parse_ip(const char *s)
{
    u8  a = 0, b = 0, c = 0, d = 0;
    int n = 0;

    for (; *s; s++) {
        if (*s == '.') {
            n++;
        } else if (*s >= '0' && *s <= '9') {
            u8 *oc = (n == 0) ? &a : (n == 1) ? &b : (n == 2) ? &c : &d;
            *oc = (u8)(*oc * 10 + (*s - '0'));
        }
    }
    return net_make_ip(a, b, c, d);
}

static void print_int(int v)
{
    char buf[12];
    ksnprintf(buf, sizeof(buf), "%d", v);
    print_str(buf);
}

static void cmd_ifconfig(void)
{
    if (!net_ready) { print_str("eth0: not ready\r\n"); return; }

    print_str("eth0  MAC=");
    for (int i = 0; i < 6; i++) {
        print_hex_byte(net_mac[i]);
        if (i < 5) print_str(":");
    }
    print_str("\r\n      IP=");
    if (net_ip) net_print_ip(net_ip); else print_str("(none)");
    print_str("  GW=");
    if (net_gateway) net_print_ip(net_gateway); else print_str("(none)");
    print_str("  DNS=10.0.2.3\r\n");
}

static void cmd_ping(const char *arg)
{
    if (!arg) { print_str("Usage: ping <ip or hostname>\r\n"); return; }

    print_str("Resolving "); print_str(arg); print_str("...\r\n");
    u32 ip = net_dns_resolve(arg);
    if (!ip) { print_str("DNS failed\r\n"); return; }

    print_str("PING "); net_print_ip(ip); print_str(" ...\r\n");
    if (net_ping(ip)) print_str("Reply received!\r\n");
    else              print_str("Request timed out.\r\n");
}

static void cmd_wget(const char *arg)
{
    if (!arg) { print_str("Usage: wget <ip> <path> <outfile>\r\n"); return; }

    char ip_s[32]   = {0};
    char path_s[128]= {0};
    char out_s[16]  = {0};

    /* Split "ip path outfile" by spaces. */
    const char *p1 = strchr(arg, ' ');
    if (!p1) { print_str("Usage: wget <ip> <path> <outfile>\r\n"); return; }

    strlcpy(ip_s, arg, MIN((usize)(p1 - arg) + 1, sizeof(ip_s)));
    while (*p1 == ' ') p1++;

    const char *p2 = strchr(p1, ' ');
    if (p2) {
        strlcpy(path_s, p1, MIN((usize)(p2 - p1) + 1, sizeof(path_s)));
        while (*p2 == ' ') p2++;
        strlcpy(out_s, p2, sizeof(out_s));
    } else {
        strlcpy(path_s, p1, sizeof(path_s));
    }

    if (!ip_s[0] || !path_s[0]) {
        print_str("Usage: wget <ip> <path> <outfile>\r\n");
        return;
    }

    print_str("Resolving "); print_str(ip_s); print_str("...\r\n");
    u32 ip = net_dns_resolve(ip_s);
    if (!ip) { print_str("DNS failed\r\n"); return; }

    print_str("Connecting to "); net_print_ip(ip); print_str(path_s); print_str("\r\n");

    static u8 dl_buf[32768];
    int n = net_http_get(ip, 80, path_s, dl_buf, sizeof(dl_buf));
    if (n < 0) { print_str("wget: connection failed\r\n"); return; }

    print_str("Downloaded "); print_int(n); print_str(" bytes\r\n");

    if (out_s[0]) {
        i64 fd = sh_open(out_s);
        if (fd < 0) fd = sh_create(out_s, 0644);
        if (fd >= 0) {
            sh_write(fd, dl_buf, (usize)n);
            sh_close(fd);
            print_str("Saved to "); print_str(out_s); print_str("\r\n");
        }
    }
}


/* ================================================================
 *  PROCESS COMMANDS
 * ================================================================ */

static void cmd_run(const char *name)
{
    i64 fd = sh_open(name);
    if (fd < 0) { print_str("File not found.\r\n"); return; }

    void *buf = heap_malloc(524288);
    if (!buf) { sh_close(fd); print_str("OOM.\r\n"); return; }

    usize total = 0;
    for (;;) {
        i64 n = sh_read(fd, (u8 *)buf + total, 512);
        if (n <= 0) break;
        total += (usize)n;
    }
    sh_close(fd);

    i64 pid = process_create((u64)buf, name);
    heap_free(buf);
    if (pid < 0) { print_str("Process create failed.\r\n"); return; }

    /* Map a user stack for the new process. */
    u8 *pp = (u8 *)PROC_TABLE;
    for (int i = 0; i < PROC_MAX; i++, pp += PROC_PCB_SIZE) {
        PCB *t = (PCB *)pp;
        if (t->pid == (u64)pid) {
            u64 stack_page = PROC_STACK_TOP - PAGE_SIZE;
            u64 phys       = pmm_alloc();
            if (phys) {
                memset((void *)phys, 0, PAGE_SIZE);
                vmm_map(t->cr3, stack_page, phys, PTE_USER_RW);
            }
            break;
        }
    }
    process_run((u64)pid);
}

static void cmd_elf(const char *name)
{
    i64 fd = sh_open(name);
    if (fd < 0) { print_str("File not found.\r\n"); return; }

    void *buf = heap_malloc(524288);
    if (!buf) { sh_close(fd); print_str("OOM.\r\n"); return; }

    usize total = 0;
    for (;;) {
        i64 n = sh_read(fd, (u8 *)buf + total, 512);
        if (n <= 0) break;
        total += (usize)n;
    }
    sh_close(fd);

    i64 pid = elf_load(buf, total, name);
    heap_free(buf);
    if (pid < 0) { print_str("Bad ELF.\r\n"); return; }

    process_run((u64)pid);
}


/* ================================================================
 *  GUI COMMAND
 * ================================================================ */

static void cmd_gui(void)
{
    print_str("Launching Retro GUI...\r\n");
    gui_init();

    /* gui_init() sets up the full desktop:
     *   - File Manager window (left half)
     *   - Terminal window     (right half)
     *   - About modal dialog  (centred, on top)
     *   - Menu bar + Dock bar
     */

    print_str("GUI running! Press ESC to exit...\r\n");

    /* Drain any stale keyboard/mouse bytes before entering the event loop. */
    for (int d = 0; d < 500; d++) {
        if (inb(0x64) & 1) inb(0x60);
    }

    gui_redraw();

    /* Mouse packet accumulator for the GUI event loop. */
    int gui_mcycle = 0;
    u8  gui_mpkt[3];
    int lbtn_prev  = 0;
    int lbtn_curr  = 0;

    for (;;) {
        /* Drain all pending i8042 bytes before sleeping.
         *
         * i8042 status byte (port 0x64):
         *   bit 0 = output buffer full (data ready at 0x60)
         *   bit 5 = 1 -> byte is from mouse; 0 -> from keyboard
         *
         * Previously, reading 0x60 without checking bit 5 caused mouse
         * movement bytes (which fall in 0x01–0x7F) to be mistaken for
         * keyboard make-codes, closing the GUI on any mouse movement.
         */
        int did_work = 0;

        while (1) {
            u8 st = inb(0x64);
            if (!(st & 1)) break;   /* no data ready */
                did_work = 1;

            u8 b = inb(0x60);

            if (st & 0x20) {
                /* ---- Mouse byte ---- */

                /* Byte 0 must have bit 3 set — resync if not. */
                if (gui_mcycle == 0 && !(b & 0x08)) continue;

                gui_mpkt[gui_mcycle++] = b;
                if (gui_mcycle < 3) continue;
                gui_mcycle = 0;

                u8 flags = gui_mpkt[0];
                if (flags & 0xC0) continue;   /* overflow — discard */

                    /* Sign-extend the 9-bit X/Y deltas. */
                    int dx =  (int)gui_mpkt[1] - ((flags & 0x10) ? 256 : 0);
                int dy = -((int)gui_mpkt[2] - ((flags & 0x20) ? 256 : 0));

                /* Scale down for a smoother feel at 1024×768. */
                dx /= 2;
                dy /= 2;

                if (dx || dy) gui_cursor_move(dx, dy);

                lbtn_curr = flags & 0x01;

                /* Left button click (rising edge). */
                if (lbtn_curr && !lbtn_prev) {
                    gui_handle_click(-1, -1);
                }

                /* Right button click (rising edge). */
                if ((flags & 0x02) && !(lbtn_prev & 0x02)) {
                    int cx, cy;
                    gui_cursor_get(&cx, &cy);
                    gui_handle_right_click(cx, cy);
                }

                /* Button released. */
                if (!lbtn_curr && lbtn_prev) {
                    gui_stop_drag();
                }

                lbtn_prev = lbtn_curr;

                /* Forward button state to the input ring buffer. */
                u8 ibtns = 0;
                if (flags & 0x01) ibtns |= INPUT_BTN_LEFT;
                if (flags & 0x02) ibtns |= INPUT_BTN_RIGHT;
                if (flags & 0x04) ibtns |= INPUT_BTN_MIDDLE;
                input_push_mouse(dx, dy, ibtns);

            } else {
                /* ---- Keyboard byte ---- */
                if (b == 0x01) goto gui_exit;   /* ESC make   */
                    if (b == 0x81) continue;         /* ESC break  */

                        /* Shared modifier tracking for the GUI keyboard handler. */
                        static u8 gui_shift = 0, gui_caps = 0, gui_ctrl = 0, gui_e0 = 0;

                    if (b == 0xE0) { gui_e0 = 1; continue; }
                    if (b == 0x2A || b == 0x36) { gui_shift = 1; gui_e0 = 0; input_mod_update(gui_shift, gui_ctrl, gui_caps); continue; }
                    if (b == 0xAA || b == 0xB6) { gui_shift = 0; gui_e0 = 0; input_mod_update(gui_shift, gui_ctrl, gui_caps); continue; }
                    if (b == 0x1D) { gui_ctrl = 1; gui_e0 = 0; input_mod_update(gui_shift, gui_ctrl, gui_caps); continue; }
                    if (b == 0x9D) { gui_ctrl = 0; gui_e0 = 0; input_mod_update(gui_shift, gui_ctrl, gui_caps); continue; }
                    if (b == 0x3A) { gui_caps ^= 1; gui_e0 = 0; input_mod_update(gui_shift, gui_ctrl, gui_caps); continue; }
                    if (b == 0xBA) { gui_e0 = 0; continue; }
                    if (b & 0x80)  { input_key_release(); gui_e0 = 0; continue; }

                    if (gui_e0) {
                        gui_e0 = 0;
                        if (b == 0x48) input_push_key(0x80 | 0x48, KEY_UP);
                        if (b == 0x50) input_push_key(0x80 | 0x50, KEY_DOWN);
                        continue;
                    }

                    if (b >= 0x3A) continue;

                    u8 ch = kb_translate(b, gui_shift, gui_caps);
                if (!ch) continue;

                input_push_key(b, (u8)ch);
                gui_shell_keypress((u8)ch);
            }
        }

        watchdog_pet();   /* GUI is alive */
        ps2_poll();
        usb_full_poll();
        __asm__ volatile("hlt");
    }

    gui_exit:
    print_str("Exiting GUI, returning to shell...\r\n");
    gui_shutdown();

    /* Restore the VGA text-mode cursor shape. */
    outb(0x3D4, 0x0A); outb(0x3D5, (inb(0x3D5) & 0xC0) | 14);
    outb(0x3D4, 0x0B); outb(0x3D5, (inb(0x3D5) & 0xE0) | 15);
    vga_clear();
}


/* ---- Systrix Compiler (SHC) info -------------------------------- */
static void cmd_shc_info(void)
{
    print_str("Systrix Compiler (SHC) is installed on this disk.\r\n\r\n");
    print_str("Usage:\r\n");
    print_str("  elf SHC              launch the compiler\r\n");
    print_str("  (enter filename)     e.g. PROG.SHA\r\n\r\n");
    print_str("Systrix language syntax:\r\n");
    print_str("  set x to 10         set y to x + 5\r\n");
    print_str("  if x greater y then ... else ... end\r\n");
    print_str("  repeat while x greater 0 ... end\r\n");
    print_str("  return 0\r\n\r\n");
    print_str("Comparisons: greater less equals notequals greaterequals lessequals\r\n");
    print_str("Operators:   + - * / %%\r\n");
}


/* ================================================================
 *  COMMAND DISPATCHER
 * ================================================================ */

/* Parse a port number from the string 'p'; return the parsed value. */
static u16 parse_port(const char *p, u16 fallback)
{
    if (!p) return fallback;
    u16 port = 0;
    for (; *p >= '0' && *p <= '9'; p++)
        port = (u16)(port * 10 + (*p - '0'));
    return port ? port : fallback;
}

static void exec_cmd(void)
{
    const char *s = input_buf;
    while (*s == ' ') s++;
    if (!*s) return;

    /* Helper macro: match a command name and execute body, then return. */
    #define CMD(name, body) \
    if (strncmp(s, name, sizeof(name) - 1) == 0 && \
        (s[sizeof(name) - 1] == ' ' || s[sizeof(name) - 1] == 0)) { body; return; }

        /* ---- system commands ---- */
        CMD("clear",   vga_clear())
        CMD("reboot",  { print_str("Rebooting...\r\n"); outb(0x64, 0xFE); cli(); hlt(); })
        CMD("halt",    { print_str("System halted.\r\n"); cli(); hlt(); })
        CMD("ps",      ps_list())
        CMD("meminfo", cmd_meminfo())
        CMD("uptime",  cmd_uptime())
        CMD("uname",   cmd_uname())
        CMD("sysinfo", cmd_sysinfo())
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
            "System:    meminfo  uname  uptime  sysinfo  df  clear  reboot  halt\r\n"
            "GUI:       gui                launch retro GUI demo\r\n"
            "Display:   720p  1080p        switch resolution (while in GUI)\r\n"
            "           browser            web browser (coming soon)\r\n"))

        /* ---- file management ---- */
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

        /* ---- processes ---- */
        CMD("run",     { if (arg) cmd_run(arg);     else print_str("Usage: run <file>\r\n"); })
        CMD("elf",     { if (arg) cmd_elf(arg);     else print_str("Usage: elf <file>\r\n"); })

        #undef CMD

        /* ---- networking (manual dispatch for multi-word arg parsing) ---- */
        if (strncmp(s, "ifconfig", 8) == 0 && (s[8] == ' ' || s[8] == 0)) { cmd_ifconfig(); return; }
        if (strncmp(s, "gui",      3) == 0 && (s[3] == ' ' || s[3] == 0)) { cmd_gui();      return; }

        /* Resolution switching (requires GUI to be active). */
        if (strncmp(s, "720p",  4) == 0 && (s[4] == ' ' || s[4] == 0)) {
            if (!fb_is_enabled()) { print_str("GUI not active. Run 'gui' first.\r\n"); return; }
            if (fb_set_resolution(1280, 720)) print_str("Resolution set to 1280x720 (720p).\r\n");
            else                              print_str("720p not supported by display.\r\n");
            return;
        }
        if (strncmp(s, "1080p", 5) == 0 && (s[5] == ' ' || s[5] == 0)) {
            if (!fb_is_enabled()) { print_str("GUI not active. Run 'gui' first.\r\n"); return; }
            if (fb_set_resolution(1920, 1080)) print_str("Resolution set to 1920x1080 (1080p).\r\n");
            else                               print_str("1080p not supported by display.\r\n");
            return;
        }

        /* Browser (not yet implemented). */
        if (strncmp(s, "browser", 7) == 0 && (s[7] == ' ' || s[7] == 0)) {
            print_str("Browser: not yet implemented.\r\n");
            print_str("Run 'gui' first, then use browser from GUI when available.\r\n");
            return;
        }

        if (strncmp(s, "ping",   4) == 0 && (s[4] == ' ' || s[4] == 0)) { cmd_ping(arg); return; }
        if (strncmp(s, "wget",   4) == 0 && (s[4] == ' ' || s[4] == 0)) { cmd_wget(arg); return; }

        if (strncmp(s, "serve", 5) == 0 && (s[5] == ' ' || s[5] == 0)) {
            u16 port = parse_port(arg, 80);
            net_http_serve(port);
            return;
        }

        if (strncmp(s, "netcat", 6) == 0 && (s[6] == ' ' || s[6] == 0)) {
            u16 port = parse_port(arg, 4444);

            if (!net_tcp_listen(port)) { print_str("netcat: bind failed\r\n"); return; }

            char pb[40];
            ksnprintf(pb, sizeof(pb), "netcat: waiting on port %u (any key to cancel)\r\n", (unsigned)port);
            print_str(pb);

            /* Wait for an incoming connection, checking for keypress to cancel. */
            TcpConn *conn = NULL;
            for (;;) {
                net_poll();
                if (inb(0x64) & 1) { inb(0x60); net_tcp_unlisten(port); return; }
                conn = net_tcp_accept_nb(port);
                if (conn) break;
                for (volatile int d = 0; d < 5000; d++) __asm__ volatile("pause");
            }
            print_str("netcat: connected\r\n");

            static u8 nc_buf[512];
            for (;;) {
                int n = net_tcp_recv(conn, nc_buf, sizeof(nc_buf) - 1);
                if (n <= 0) break;
                nc_buf[n] = 0;
                print_str((char *)nc_buf);
                if (inb(0x64) & 1) { inb(0x60); break; }
            }
            net_tcp_close(conn);
            net_tcp_unlisten(port);
            print_str("\r\nnetcat: done.\r\n");
            return;
        }

        print_str("Unknown command. Try 'help'.\r\n");
}


/* ================================================================
 *  KERNEL ENTRY POINT  (called from entry.S after boot)
 * ================================================================ */

/* Polled I/O requires no keyboard initialisation at boot. */
static void kbd_init_dummy(void) {}

void kernel_main(void)
{
    /* Enable the VGA hardware cursor (scanlines 14–15 = underline style). */
    outb(0x3D4, 0x0A); outb(0x3D5, (inb(0x3D5) & 0xC0) | 14);
    outb(0x3D4, 0x0B); outb(0x3D5, (inb(0x3D5) & 0xE0) | 15);
    vga_clear();

    /* Initialise kernel subsystems in dependency order. */
    heap_init();
    pmm_init();
    vmm_init_kernel();
    tss_init();        /* must run before STI — sets RSP0 for ring-3 exceptions */
    syscall_init();
    ipc_init();

    memset((void *)PROC_TABLE, 0, PROC_PCB_SIZE * PROC_MAX);
    process_init();
    scheduler_init();

    vfs_init();
    /* NOTE: fat32_init() and VFS mounts happen AFTER AHCI/PCI init below,
     * so the disk backend is configured before we read the BPB.
     * Only vfs_init() (which zeroes the tables) runs here. */
    vfs_core_init();

    pci_scan_all();    /* enumerate all PCI devices — must run before any driver */

    net_start();       /* init e1000 (needs PCI scan), configure IP */

    /* Drain any stale i8042 bytes that accumulated during network init.
     * Without this, DHCP broadcast packets can cause phantom keypresses. */
    for (int t = 1000; (inb(0x64) & 1) && --t;) inb(0x60);

    /* ── AHCI SATA (class 0x01, subclass 0x06) ── */
    {
        /* Try strict prog-if 0x01 first; fall back to any subclass-0x06 device
         * because some QEMU/VirtualBox builds expose AHCI with prog-if 0x00. */
        void *ahci_dev = pci_find_class_progif(0x01, 0x06, 0x01);
        if (!ahci_dev) ahci_dev = pci_find_class(0x01, 0x06);

        if (ahci_dev) {
            pci_power_on(ahci_dev);           /* wake from D3 if sleeping    */
            pci_enable_device(ahci_dev);      /* enable MMIO + I/O decoding  */
            pci_enable_bus_master(ahci_dev);  /* allow DMA                   */

            u64 bar5 = pci_bar_base(ahci_dev, 5);
            if (!bar5) bar5 = pci_bar_base(ahci_dev, 0);   /* fallback BAR0 */

                if (bar5) {
                    ahci_init(bar5);
                    if (ahci_get_port_count() > 0) {
                        disk_use_ahci();
                        print_str("[disk] using AHCI backend\r\n");
                    }
                } else {
                    print_str("[AHCI] SATA controller found but BAR invalid\r\n");
                }
        } else {
            print_str("[AHCI] no SATA controller found\r\n");
        }
    }

    /* ── NVMe (class 0x01, subclass 0x08, prog-if 0x02) ── */
    {
        void *nvme_dev = pci_find_class_progif(0x01, 0x08, 0x02);
        if (!nvme_dev) nvme_dev = pci_find_class(0x01, 0x08);

        if (nvme_dev) {
            pci_power_on(nvme_dev);
            pci_enable_device(nvme_dev);
            pci_enable_bus_master(nvme_dev);

            u64 bar0 = pci_bar_base(nvme_dev, 0);
            if (bar0) {
                nvme_init(bar0);
            } else {
                print_str("[NVMe] NVMe controller found but BAR invalid\r\n");
            }
        } else {
            print_str("[NVMe] no NVMe controller found\r\n");
        }
    }

    /* ── Mount filesystems now that the disk backend is configured ── */
    fat32_init();
    jfs_init();
    vfs_register_fat32();   /* mount FAT32 at /     via inode_ops_t */
    vfs_register_jfs();     /* mount JFS   at /jfs  via inode_ops_t */

    ps2_init();
    kbd_init_dummy();
    mouse_ready = ps2_mouse_ok();
    if (mouse_ready) ps2_mouse_refresh();

    scheduler_start();   /* STI — timer fires from this point on  */
    usb_full_init();     /* EHCI + XHCI enumeration + mass storage */
    smp_init();
    watchdog_init();

    print_str("========================================\r\n");
    print_str("  Systrix v0.1\r\n");
    print_str("  x86-64 | preemptive | FAT32 | ring-3\r\n");
    print_str("  Type 'help' for available commands.\r\n");
    print_str("  Type 'gui'  to launch the GUI desktop.\r\n");
    print_str("  Type 'sysinfo' to verify framebuffer.\r\n");
    print_str("========================================\r\n");

    /* Main shell loop. */
    for (;;) {
        watchdog_pet();
        print_str("systrix:");
        print_str(cwd_path);
        print_str("$ ");
        read_line(input_buf, sizeof(input_buf));
        watchdog_pet();
        exec_cmd();
        watchdog_pet();
    }
}
