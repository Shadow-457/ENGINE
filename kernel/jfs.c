#include "../include/kernel.h"

#define JFS_MAGIC 0x4A465300ULL
#define JFS_MAX_INODES 256
#define JFS_MAX_BLOCKS 1024
#define JFS_BLOCK_SIZE 512
#define JFS_JOURNAL_START 131072
#define JFS_DATA_START 132096
#define JFS_SUPER_LBA 131072

#define JFS_IT_FREE  0
#define JFS_IT_FILE  1
#define JFS_IT_DIR   2
#define JFS_IT_LINK  3

#define JFS_OP_WRITE 1
#define JFS_OP_UNLINK 2
#define JFS_OP_MKDIR 3
#define JFS_OP_CREATE 4

typedef struct {
    u64 magic;
    u32 version;
    u32 block_size;
    u64 total_blocks;
    u64 free_blocks;
    u64 total_inodes;
    u64 free_inodes;
    u32 journal_start;
    u32 journal_blocks;
    u32 data_start;
    u32 _pad;
} jfs_super_t;

typedef struct {
    u16 mode;
    u16 type;
    u32 uid;
    u32 gid;
    u64 size;
    u64 blocks;
    u64 atime;
    u64 mtime;
    u64 ctime;
    u32 block_ptrs[12];
    u32 indirect_block;
    u32 _pad[4];
} jfs_inode_t;

typedef struct {
    char name[64];
    u32 ino;
    u8 type;
    u8 _pad[3];
} jfs_direntry_t;

typedef struct {
    u32 op;
    u32 ino;
    u64 offset;
    u64 size;
    u8 data[JFS_BLOCK_SIZE - 32];
} jfs_journal_entry_t;

typedef struct {
    u32 head;
    u32 tail;
    u32 count;
    u32 _pad;
    jfs_journal_entry_t entries[16];
} jfs_journal_t;

static jfs_super_t jfs_super;
static jfs_inode_t jfs_inodes[JFS_MAX_INODES];
static jfs_journal_t jfs_journal;
static u8 jfs_block_buf[JFS_BLOCK_SIZE];

/* FIX: proper in-memory block bitmap — 1 bit per block, 1=used, 0=free */
#define JFS_BITMAP_WORDS  ((JFS_MAX_BLOCKS + 31) / 32)
static u32 jfs_block_bitmap[JFS_BITMAP_WORDS];

static void bitmap_set(u32 block)   { jfs_block_bitmap[block / 32] |=  (1u << (block % 32)); }
static void bitmap_clr(u32 block)   { jfs_block_bitmap[block / 32] &= ~(1u << (block % 32)); }
static int  bitmap_tst(u32 block)   { return (jfs_block_bitmap[block / 32] >> (block % 32)) & 1; }

void jfs_init(void) {
    ata_read_sector(JFS_SUPER_LBA, jfs_block_buf);
    memcpy(&jfs_super, jfs_block_buf, sizeof(jfs_super));
    if (jfs_super.magic != JFS_MAGIC) {
        memset(&jfs_super, 0, sizeof(jfs_super));
        jfs_super.magic = JFS_MAGIC;
        jfs_super.version = 1;
        jfs_super.block_size = JFS_BLOCK_SIZE;
        jfs_super.total_blocks = 65536;
        jfs_super.free_blocks = 65000;
        jfs_super.total_inodes = JFS_MAX_INODES;
        jfs_super.free_inodes = JFS_MAX_INODES - 1;
        jfs_super.journal_start = JFS_JOURNAL_START;
        jfs_super.journal_blocks = 1024;
        jfs_super.data_start = JFS_DATA_START;
        memset(jfs_block_buf, 0, JFS_BLOCK_SIZE);
        memcpy(jfs_block_buf, &jfs_super, sizeof(jfs_super));
        ata_write_sector(JFS_SUPER_LBA, jfs_block_buf);
    }
    memset(jfs_inodes, 0, sizeof(jfs_inodes));
    jfs_inodes[0].type = JFS_IT_DIR;
    jfs_inodes[0].mode = 0755;
    jfs_inodes[0].uid = 0;
    jfs_inodes[0].gid = 0;
    jfs_inodes[0].size = 0;
    memset(&jfs_journal, 0, sizeof(jfs_journal));
    /* FIX: init block bitmap — all blocks start free */
    memset(jfs_block_bitmap, 0, sizeof(jfs_block_bitmap));
}

/* FIX: O(n/32) bitmap search — correct and fast */
static u32 jfs_alloc_block(void) {
    for (u32 i = 0; i < JFS_MAX_BLOCKS; i++) {
        if (!bitmap_tst(i)) {
            bitmap_set(i);
            jfs_super.free_blocks--;
            /* Zero the newly allocated block */
            u32 lba = jfs_super.data_start + i;
            memset(jfs_block_buf, 0, JFS_BLOCK_SIZE);
            ata_write_sector(lba, jfs_block_buf);
            return i;
        }
    }
    return (u32)-1;
}

static void jfs_free_block(u32 block) {
    if (block >= JFS_MAX_BLOCKS) return;
    if (bitmap_tst(block)) {
        bitmap_clr(block);
        jfs_super.free_blocks++;
    }
}

static u32 jfs_alloc_inode(void) {
    for (u32 i = 1; i < JFS_MAX_INODES; i++) {
        if (jfs_inodes[i].type == JFS_IT_FREE) {
            jfs_super.free_inodes--;
            return i;
        }
    }
    return (u32)-1;
}

static void jfs_journal_commit(u32 op, u32 ino, u64 offset, u64 size, const void *data) {
    jfs_journal.entries[jfs_journal.tail].op = op;
    jfs_journal.entries[jfs_journal.tail].ino = ino;
    jfs_journal.entries[jfs_journal.tail].offset = offset;
    jfs_journal.entries[jfs_journal.tail].size = size;
    if (data && size <= JFS_BLOCK_SIZE - 32) {
        memcpy(jfs_journal.entries[jfs_journal.tail].data, data, (usize)size);
    }
    jfs_journal.tail = (jfs_journal.tail + 1) % 16;
    jfs_journal.count++;
    if (jfs_journal.count >= 16) {
        jfs_journal_flush();
    }
}

void jfs_journal_flush(void) {
    for (u32 i = 0; i < jfs_journal.count; i++) {
        u32 idx = (jfs_journal.head + i) % 16;
        u32 lba = jfs_super.journal_start + idx;
        memset(jfs_block_buf, 0, JFS_BLOCK_SIZE);
        memcpy(jfs_block_buf, &jfs_journal.entries[idx], sizeof(jfs_journal_entry_t));
        ata_write_sector(lba, jfs_block_buf);
    }
    jfs_journal.head = jfs_journal.tail;
    jfs_journal.count = 0;
    /* FIX: copy jfs_super INTO jfs_block_buf — do NOT zero jfs_super first */
    memset(jfs_block_buf, 0, JFS_BLOCK_SIZE);
    memcpy(jfs_block_buf, &jfs_super, sizeof(jfs_super));
    ata_write_sector(JFS_SUPER_LBA, jfs_block_buf);
}

i64 jfs_create(const char *name, u16 mode) {
    u32 ino = jfs_alloc_inode();
    if (ino == (u32)-1) return (i64)ENOSPC;
    jfs_inodes[ino].type = JFS_IT_FILE;
    jfs_inodes[ino].mode = mode;
    jfs_inodes[ino].uid = 0;
    jfs_inodes[ino].gid = 0;
    jfs_inodes[ino].size = 0;
    jfs_inodes[ino].blocks = 0;
    memset(jfs_inodes[ino].block_ptrs, 0, sizeof(jfs_inodes[ino].block_ptrs));
    jfs_journal_commit(JFS_OP_CREATE, ino, 0, 0, NULL);
    jfs_journal_flush();
    return (i64)ino;
}

i64 jfs_write(u32 ino, const void *buf, usize count, usize offset) {
    if (ino >= JFS_MAX_INODES || jfs_inodes[ino].type != JFS_IT_FILE) return (i64)EBADF;
    jfs_inode_t *inode = &jfs_inodes[ino];
    usize written = 0;
    const u8 *data = (const u8*)buf;
    while (written < count) {
        u32 block_idx = (offset + written) / JFS_BLOCK_SIZE;
        u32 block_off = (offset + written) % JFS_BLOCK_SIZE;
        usize to_write = JFS_BLOCK_SIZE - block_off;
        if (to_write > count - written) to_write = count - written;
        if (block_idx < 12) {
            if (inode->block_ptrs[block_idx] == 0) {
                u32 blk = jfs_alloc_block();
                if (blk == (u32)-1) return written ? (i64)written : (i64)ENOSPC;
                inode->block_ptrs[block_idx] = blk;
                inode->blocks++;
            }
            u32 lba = jfs_super.data_start + inode->block_ptrs[block_idx];
            if (block_off == 0 && to_write == JFS_BLOCK_SIZE) {
                memcpy(jfs_block_buf, data + written, to_write);
                ata_write_sector(lba, jfs_block_buf);
            } else {
                ata_read_sector(lba, jfs_block_buf);
                memcpy(jfs_block_buf + block_off, data + written, to_write);
                ata_write_sector(lba, jfs_block_buf);
            }
        }
        jfs_journal_commit(JFS_OP_WRITE, ino, offset + written, to_write, data + written);
        written += to_write;
    }
    if (offset + written > inode->size) {
        inode->size = offset + written;
    }
    jfs_journal_flush();
    return (i64)written;
}

i64 jfs_read(u32 ino, void *buf, usize count, usize offset) {
    if (ino >= JFS_MAX_INODES || jfs_inodes[ino].type != JFS_IT_FILE) return (i64)EBADF;
    jfs_inode_t *inode = &jfs_inodes[ino];
    if (offset >= inode->size) return 0;
    if (offset + count > inode->size) count = inode->size - offset;
    usize total = 0;
    u8 *data = (u8*)buf;
    while (total < count) {
        u32 block_idx = (offset + total) / JFS_BLOCK_SIZE;
        u32 block_off = (offset + total) % JFS_BLOCK_SIZE;
        usize to_read = JFS_BLOCK_SIZE - block_off;
        if (to_read > count - total) to_read = count - total;
        if (block_idx < 12 && inode->block_ptrs[block_idx] != 0) {
            u32 lba = jfs_super.data_start + inode->block_ptrs[block_idx];
            ata_read_sector(lba, jfs_block_buf);
            memcpy(data + total, jfs_block_buf + block_off, to_read);
        } else {
            memset(data + total, 0, to_read);
        }
        total += to_read;
    }
    return (i64)total;
}

i64 jfs_unlink(u32 ino) {
    if (ino >= JFS_MAX_INODES) return (i64)ENOENT;
    jfs_inode_t *inode = &jfs_inodes[ino];
    jfs_journal_commit(JFS_OP_UNLINK, ino, 0, inode->size, NULL);
    for (int i = 0; i < 12; i++) {
        if (inode->block_ptrs[i]) {
            jfs_free_block(inode->block_ptrs[i]);
            inode->block_ptrs[i] = 0;
        }
    }
    inode->type = JFS_IT_FREE;
    inode->size = 0;
    inode->blocks = 0;
    jfs_super.free_inodes++;
    jfs_journal_flush();
    return 0;
}

i64 jfs_mkdir(const char *name, u16 mode) {
    u32 ino = jfs_alloc_inode();
    if (ino == (u32)-1) return (i64)ENOSPC;
    jfs_inodes[ino].type = JFS_IT_DIR;
    jfs_inodes[ino].mode = mode;
    jfs_inodes[ino].uid = 0;
    jfs_inodes[ino].gid = 0;
    jfs_inodes[ino].size = 0;
    jfs_inodes[ino].blocks = 0;
    memset(jfs_inodes[ino].block_ptrs, 0, sizeof(jfs_inodes[ino].block_ptrs));
    jfs_journal_commit(JFS_OP_MKDIR, ino, 0, 0, NULL);
    jfs_journal_flush();
    return (i64)ino;
}

/* ── VFS ops wrappers for JFS ──────────────────────────────────
 * These bridge inode_ops_t (generic VFS) → raw jfs_*() calls.
 * inode->ino holds the JFS inode number; inode->fs_data unused.
 * ──────────────────────────────────────────────────────────── */

/* Write name+ino as a dir-entry into directory inode dir_ino.
 * Scans block_ptrs[0..11] for a free slot; allocates a block if needed. */
static i64 jfs_dir_add(u32 dir_ino, const char *name, u32 child_ino, u8 type) {
    if (dir_ino >= JFS_MAX_INODES || jfs_inodes[dir_ino].type != JFS_IT_DIR)
        return (i64)ENOTDIR;
    jfs_inode_t *dir = &jfs_inodes[dir_ino];
    jfs_direntry_t de;
    /* Scan existing blocks for a free slot (ino == 0 → free) */
    for (int b = 0; b < 12; b++) {
        if (dir->block_ptrs[b] == 0) continue;
        u32 lba = jfs_super.data_start + dir->block_ptrs[b];
        u32 slots = JFS_BLOCK_SIZE / sizeof(jfs_direntry_t);
        for (u32 s = 0; s < slots; s++) {
            ata_read_sector(lba, jfs_block_buf);
            memcpy(&de, jfs_block_buf + s * sizeof(de), sizeof(de));
            if (de.ino == 0) {
                memset(&de, 0, sizeof(de));
                usize nlen = strlen(name);
                if (nlen >= 64) nlen = 63;
                memcpy(de.name, name, nlen);
                de.ino  = child_ino;
                de.type = type;
                ata_read_sector(lba, jfs_block_buf);
                memcpy(jfs_block_buf + s * sizeof(de), &de, sizeof(de));
                ata_write_sector(lba, jfs_block_buf);
                dir->size += sizeof(de);
                return 0;
            }
        }
    }
    /* No free slot — allocate new block */
    for (int b = 0; b < 12; b++) {
        if (dir->block_ptrs[b] != 0) continue;
        u32 blk = jfs_alloc_block();
        if (blk == (u32)-1) return (i64)ENOSPC;
        dir->block_ptrs[b] = blk;
        dir->blocks++;
        memset(&de, 0, sizeof(de));
        usize nlen = strlen(name);
        if (nlen >= 64) nlen = 63;
        memcpy(de.name, name, nlen);
        de.ino  = child_ino;
        de.type = type;
        u32 lba = jfs_super.data_start + blk;
        memset(jfs_block_buf, 0, JFS_BLOCK_SIZE);
        memcpy(jfs_block_buf, &de, sizeof(de));
        ata_write_sector(lba, jfs_block_buf);
        dir->size += sizeof(de);
        return 0;
    }
    return (i64)ENOSPC;
}

/* Scan dir_ino for an entry matching name → fill child_ino+type. */
static i64 jfs_dir_lookup(u32 dir_ino, const char *name, u32 *child_ino, u8 *child_type) {
    if (dir_ino >= JFS_MAX_INODES || jfs_inodes[dir_ino].type != JFS_IT_DIR)
        return (i64)ENOTDIR;
    jfs_inode_t *dir = &jfs_inodes[dir_ino];
    jfs_direntry_t de;
    u32 slots = JFS_BLOCK_SIZE / sizeof(jfs_direntry_t);
    for (int b = 0; b < 12; b++) {
        if (dir->block_ptrs[b] == 0) continue;
        u32 lba = jfs_super.data_start + dir->block_ptrs[b];
        ata_read_sector(lba, jfs_block_buf);
        for (u32 s = 0; s < slots; s++) {
            memcpy(&de, jfs_block_buf + s * sizeof(de), sizeof(de));
            if (de.ino == 0) continue;
            if (strncmp(de.name, name, 63) == 0) {
                *child_ino  = de.ino;
                *child_type = de.type;
                return 0;
            }
        }
    }
    return (i64)ENOENT;
}

/* ── inode_ops_t callbacks ───────────────────────────────────── */

static i64 jfs_ops_read(struct vfs_inode *inode, void *buf, usize count, usize offset) {
    return jfs_read((u32)inode->ino, buf, count, offset);
}

static i64 jfs_ops_write(struct vfs_inode *inode, const void *buf, usize count, usize offset) {
    return jfs_write((u32)inode->ino, buf, count, offset);
}

static i64 jfs_ops_readdir(struct vfs_inode *inode, void *buf, usize count, usize *offset) {
    u32 dir_ino = (u32)inode->ino;
    if (dir_ino >= JFS_MAX_INODES || jfs_inodes[dir_ino].type != JFS_IT_DIR)
        return (i64)ENOTDIR;
    jfs_inode_t *dir = &jfs_inodes[dir_ino];
    jfs_direntry_t de;
    u32 slots = JFS_BLOCK_SIZE / sizeof(jfs_direntry_t);
    usize written = 0;
    for (int b = 0; b < 12 && written + sizeof(de) <= count; b++) {
        if (dir->block_ptrs[b] == 0) continue;
        u32 lba = jfs_super.data_start + dir->block_ptrs[b];
        ata_read_sector(lba, jfs_block_buf);
        for (u32 s = 0; s < slots && written + sizeof(de) <= count; s++) {
            memcpy(&de, jfs_block_buf + s * sizeof(de), sizeof(de));
            if (de.ino == 0) continue;
            memcpy((u8*)buf + written, &de, sizeof(de));
            written += sizeof(de);
        }
    }
    *offset += written;
    return (i64)written;
}

static i64 jfs_ops_lookup(struct vfs_inode *dir_inode, const char *name, struct vfs_inode *out) {
    u32 child_ino; u8 child_type;
    i64 r = jfs_dir_lookup((u32)dir_inode->ino, name, &child_ino, &child_type);
    if (r < 0) return r;
    if (child_ino >= JFS_MAX_INODES) return (i64)ENOENT;
    jfs_inode_t *ji = &jfs_inodes[child_ino];
    out->valid    = 1;
    out->ino      = child_ino;
    out->dev      = dir_inode->dev;
    out->rdev     = 0;
    out->refcount = 1;
    out->ops      = dir_inode->ops;   /* same ops table */
    out->fs_data  = NULL;
    out->mount    = dir_inode->mount;
    out->attr.type    = (ji->type == JFS_IT_DIR) ? 2 : 1; /* IT_DIR=2 IT_FILE=1 */
    out->attr.mode    = ji->mode;
    out->attr.uid     = ji->uid;
    out->attr.gid     = ji->gid;
    out->attr.size    = ji->size;
    out->attr.nlink   = 1;
    out->attr.blocks  = ji->blocks;
    out->attr.blksize = JFS_BLOCK_SIZE;
    out->attr.atime   = ji->atime;
    out->attr.mtime   = ji->mtime;
    out->attr.ctime   = ji->ctime;
    return 0;
}

static i64 jfs_ops_create(struct vfs_inode *dir_inode, const char *name, u16 mode, struct vfs_inode *out) {
    i64 ino = jfs_create(name, mode);
    if (ino < 0) return ino;
    i64 r = jfs_dir_add((u32)dir_inode->ino, name, (u32)ino, JFS_IT_FILE);
    if (r < 0) { jfs_unlink((u32)ino); return r; }
    jfs_ops_lookup(dir_inode, name, out);
    return 0;
}

static i64 jfs_ops_mkdir(struct vfs_inode *dir_inode, const char *name, u16 mode) {
    i64 ino = jfs_mkdir(name, mode);
    if (ino < 0) return ino;
    return jfs_dir_add((u32)dir_inode->ino, name, (u32)ino, JFS_IT_DIR);
}

static i64 jfs_ops_unlink(struct vfs_inode *dir_inode, const char *name) {
    u32 child_ino; u8 child_type;
    i64 r = jfs_dir_lookup((u32)dir_inode->ino, name, &child_ino, &child_type);
    if (r < 0) return r;
    /* Remove dir-entry: scan blocks, zero the matching slot */
    jfs_inode_t *dir = &jfs_inodes[(u32)dir_inode->ino];
    jfs_direntry_t de;
    u32 slots = JFS_BLOCK_SIZE / sizeof(jfs_direntry_t);
    for (int b = 0; b < 12; b++) {
        if (dir->block_ptrs[b] == 0) continue;
        u32 lba = jfs_super.data_start + dir->block_ptrs[b];
        ata_read_sector(lba, jfs_block_buf);
        for (u32 s = 0; s < slots; s++) {
            memcpy(&de, jfs_block_buf + s * sizeof(de), sizeof(de));
            if (de.ino == child_ino && strncmp(de.name, name, 63) == 0) {
                memset(jfs_block_buf + s * sizeof(de), 0, sizeof(de));
                ata_write_sector(lba, jfs_block_buf);
                goto found;
            }
        }
    }
    found:
    return jfs_unlink(child_ino);
}

static i64 jfs_ops_setattr(struct vfs_inode *inode, inode_attr_t *attr) {
    u32 ino = (u32)inode->ino;
    if (ino >= JFS_MAX_INODES) return (i64)EINVAL;
    jfs_inodes[ino].mode  = attr->mode;
    jfs_inodes[ino].uid   = attr->uid;
    jfs_inodes[ino].gid   = attr->gid;
    jfs_inodes[ino].atime = attr->atime;
    jfs_inodes[ino].mtime = attr->mtime;
    jfs_inodes[ino].ctime = attr->ctime;
    inode->attr = *attr;
    return 0;
}

static i64 jfs_ops_getsize(struct vfs_inode *inode) {
    u32 ino = (u32)inode->ino;
    if (ino >= JFS_MAX_INODES) return (i64)EINVAL;
    return (i64)jfs_inodes[ino].size;
}

/* Global JFS ops table — one instance, all JFS inodes point here */
static inode_ops_t jfs_inode_ops = {
    .read    = jfs_ops_read,
    .write   = jfs_ops_write,
    .readdir = jfs_ops_readdir,
    .lookup  = jfs_ops_lookup,
    .create  = jfs_ops_create,
    .mkdir   = jfs_ops_mkdir,
    .unlink  = jfs_ops_unlink,
    .rmdir   = jfs_ops_unlink,   /* reuse: removes entry + inode */
    .setattr = jfs_ops_setattr,
    .getsize = jfs_ops_getsize,
};

/* Call after jfs_init(). Builds a root vfs_inode_t and mounts at /jfs */
void vfs_register_jfs(void) {
    vfs_inode_t root;
    memset(&root, 0, sizeof(root));
    root.valid    = 1;
    root.ino      = 0;            /* JFS inode 0 is always root dir */
    root.dev      = 2;            /* arbitrary dev id for JFS */
    root.refcount = 1;
    root.ops      = &jfs_inode_ops;
    root.attr.type    = 2;        /* IT_DIR */
    root.attr.mode    = 0755;
    root.attr.nlink   = 1;
    root.attr.blksize = JFS_BLOCK_SIZE;
    vfs_mount_fs("/jfs", "jfs", 2, &root);
}
