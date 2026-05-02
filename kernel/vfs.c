#include "../include/kernel.h"

#define MAX_INODE 1024
#define MAX_MOUNT 8
#define MAX_PATH 256
#define FD_MAX 256

#define IT_FILE    1
#define IT_DIR     2
#define IT_PIPE    3
#define IT_SOCKET  4
#define IT_LINK    5

#define S_IRWXU 0700
#define S_IRUSR 0400
#define S_IWUSR 0200
#define S_IXUSR 0100
#define S_IRGRP 0040
#define S_IWGRP 0020
#define S_IXGRP 0010
#define S_IROTH 0004
#define S_IWOTH 0002
#define S_IXOTH 0001
#define S_ISUID 04000
#define S_ISGID 02000
#define S_ISVTX 01000

#define AT_FDCWD (-100)

typedef struct {
    u64 dev, ino, nlink;
    u32 mode, uid, gid, _pad0;
    u64 rdev, size, blksize, blocks;
    u64 atime, atimensec, mtime, mtimensec, ctime, ctimensec;
} StatBuf;

/* inode_attr_t, inode_ops_t, vfs_inode_t, vfs_mount_t, vfs_fd_t
 * are now defined in kernel.h — included via kernel.h above */

typedef struct {
    u8 name[11];
    u8 attr;
    u8 _reserved[8];
    u16 ctime;
    u16 cdate;
    u16 adate;
    u16 eah;
    u16 mtime;
    u16 mdate;
    u16 start_lo;
    u16 start_hi;
    u32 size;
} __attribute__((packed)) fat32_direntry_t;

static vfs_inode_t inode_table[MAX_INODE];
static vfs_mount_t mount_table[MAX_MOUNT];
static vfs_fd_t fd_table[FD_MAX];
static u64 next_ino = 1;
static u32 next_dev = 1;

static u64 get_time_ms(void);

void vfs_core_init(void) {
    memset(inode_table, 0, sizeof(inode_table));
    memset(mount_table, 0, sizeof(mount_table));
    memset(fd_table, 0, sizeof(fd_table));
    for (int i = 0; i < 3; i++) {
        fd_table[i].in_use = 1;
        fd_table[i].type = IT_FILE;
        fd_table[i].offset = 0;
        fd_table[i].flags = 0;
    }
}

static u64 alloc_ino(void) { return next_ino++; }
static u32 alloc_dev(void) { return next_dev++; }

static vfs_inode_t *inode_alloc(void) {
    for (int i = 0; i < MAX_INODE; i++) {
        if (!inode_table[i].valid) {
            inode_table[i].valid = 1;
            inode_table[i].ino = alloc_ino();
            inode_table[i].refcount = 1;
            return &inode_table[i];
        }
    }
    return NULL;
}

void inode_ref(vfs_inode_t *inode) {
    if (inode) inode->refcount++;
}

void inode_deref(vfs_inode_t *inode) {
    if (!inode || --inode->refcount > 0) return;
    inode->valid = 0;
    if (inode->fs_data) heap_free(inode->fs_data);
    inode->fs_data = NULL;
}

static int fd_alloc(void) {
    for (int i = 3; i < FD_MAX; i++) {
        if (!fd_table[i].in_use) {
            fd_table[i].in_use = 1;
            return i;
        }
    }
    return -1;
}

vfs_fd_t *fd_get(u64 fd) {
    if (fd >= FD_MAX || !fd_table[fd].in_use) return NULL;
    return &fd_table[fd];
}

i64 vfs_mount_fs(const char *path, const char *fstype, u32 dev, vfs_inode_t *root) {
    usize len = strlen(path);
    if (len >= MAX_PATH) return (i64)EINVAL;
    for (int i = 0; i < MAX_MOUNT; i++) {
        if (!mount_table[i].valid) {
            mount_table[i].valid = 1;
            memset(mount_table[i].path, 0, MAX_PATH);
            memcpy(mount_table[i].path, path, len);
            mount_table[i].dev = dev;
            mount_table[i].root = *root;
            mount_table[i].fstype = fstype;
            return 0;
        }
    }
    return (i64)ENOMEM;
}

static vfs_mount_t *find_mount(const char *path, const char **rel_path) {
    vfs_mount_t *best = NULL;
    usize best_len = 0;
    usize path_len = strlen(path);
    for (int i = 0; i < MAX_MOUNT; i++) {
        if (!mount_table[i].valid) continue;
        usize mlen = strlen(mount_table[i].path);
        if (mlen > path_len) continue;
        if (mlen > 1 && path[mlen] != '/') continue;
        if (strncmp(mount_table[i].path, path, mlen) == 0) {
            if (mlen > best_len) {
                best = &mount_table[i];
                best_len = mlen;
            }
        }
    }
    if (!best) return NULL;
    usize mlen = strlen(best->path);
    *rel_path = (mlen < path_len) ? (path + mlen) : "/";
    if (**rel_path == '/') (*rel_path)++;
    return best;
}

static i64 path_walk(vfs_inode_t *start, const char *path, vfs_inode_t *out) {
    if (!start || !start->valid || !start->ops->lookup) return (i64)ENOENT;
    if (!path || !*path) { *out = *start; return 0; }
    char tmp[MAX_PATH];
    /* strlcpy returns source length; if >= sizeof(tmp) the path was truncated */
    if (strlcpy(tmp, path, sizeof(tmp)) >= sizeof(tmp))
        return (i64)ENAMETOOLONG;
    vfs_inode_t cur = *start;
    char *p = tmp;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        char *slash = p;
        while (*slash && *slash != '/') slash++;
        char saved = *slash;
        *slash = '\0';
        vfs_inode_t next;
        i64 r = cur.ops->lookup(&cur, p, &next);
        *slash = saved;
        if (r < 0) return r;
        cur = next;
        p = slash;
    }
    *out = cur;
    return 0;
}

i64 vfs_open(const char *path) {
    if (!path) return (i64)EFAULT;
    const char *rel;
    vfs_mount_t *mnt = find_mount(path, &rel);
    if (!mnt) return (i64)ENOENT;
    vfs_inode_t inode;
    i64 r = path_walk(&mnt->root, rel, &inode);
    if (r < 0) return r;
    int fd = fd_alloc();
    if (fd < 0) return (i64)ENOMEM;
    fd_table[fd].inode = inode_alloc();
    if (!fd_table[fd].inode) { fd_table[fd].in_use = 0; return (i64)ENOMEM; }
    *fd_table[fd].inode = inode;
    fd_table[fd].offset = 0;
    fd_table[fd].flags = 0;
    fd_table[fd].type = inode.attr.type;
    fd_table[fd].pipe_data = NULL;
    return fd;
}

i64 vfs_read(u64 fd, void *buf, usize n) {
    vfs_fd_t *f = fd_get(fd);
    if (!f || !f->inode) return (i64)EBADF;
    if (f->type == IT_PIPE) {
        return pipe_read(f->pipe_data, buf, n);
    }
    if (!f->inode->ops->read) return (i64)EINVAL;
    i64 r = f->inode->ops->read(f->inode, buf, n, f->offset);
    if (r > 0) f->offset += (usize)r;
    return r;
}

i64 vfs_write(u64 fd, const void *buf, usize n) {
    vfs_fd_t *f = fd_get(fd);
    if (!f || !f->inode) return (i64)EBADF;
    if (f->type == IT_PIPE) {
        return pipe_write(f->pipe_data, buf, n);
    }
    if (!f->inode->ops->write) return (i64)EINVAL;
    i64 r = f->inode->ops->write(f->inode, buf, n, f->offset);
    if (r > 0) f->offset += (usize)r;
    return r;
}

i64 vfs_close(u64 fd) {
    vfs_fd_t *f = fd_get(fd);
    if (!f) return (i64)EBADF;
    if (f->inode) inode_deref(f->inode);
    if (f->type == IT_PIPE && f->pipe_data) pipe_destroy(f->pipe_data);
    f->in_use = 0;
    f->inode = NULL;
    f->pipe_data = NULL;
    return 0;
}

i64 vfs_seek(u64 fd, i64 offset, u64 whence) {
    vfs_fd_t *f = fd_get(fd);
    if (!f || !f->inode) return (i64)EBADF;
    i64 sz = f->inode->ops->getsize ? f->inode->ops->getsize(f->inode) : 0;
    switch (whence) {
        case 0: f->offset = (usize)offset; break;
        case 1: f->offset += (usize)offset; break;
        case 2: f->offset = (usize)(sz + offset); break;
        default: return (i64)EINVAL;
    }
    return (i64)f->offset;
}

i64 vfs_stat(const char *path, void *statbuf) {
    const char *rel;
    vfs_mount_t *mnt = find_mount(path, &rel);
    if (!mnt) return (i64)ENOENT;
    vfs_inode_t inode;
    i64 r = path_walk(&mnt->root, rel, &inode);
    if (r < 0) return r;
    StatBuf *sb = (StatBuf*)statbuf;
    memset(sb, 0, sizeof(*sb));
    sb->dev = inode.dev;
    sb->ino = inode.ino;
    sb->nlink = inode.attr.nlink;
    sb->mode = inode.attr.mode;
    sb->uid = inode.attr.uid;
    sb->gid = inode.attr.gid;
    sb->rdev = inode.rdev;
    sb->size = inode.attr.size;
    sb->blksize = inode.attr.blksize;
    sb->blocks = inode.attr.blocks;
    sb->atime = inode.attr.atime;
    sb->mtime = inode.attr.mtime;
    sb->ctime = inode.attr.ctime;
    return 0;
}

i64 vfs_fstat(u64 fd, void *statbuf) {
    vfs_fd_t *f = fd_get(fd);
    if (!f || !f->inode) return (i64)EBADF;
    StatBuf *sb = (StatBuf*)statbuf;
    memset(sb, 0, sizeof(*sb));
    sb->dev = f->inode->dev;
    sb->ino = f->inode->ino;
    sb->nlink = f->inode->attr.nlink;
    sb->mode = f->inode->attr.mode;
    sb->uid = f->inode->attr.uid;
    sb->gid = f->inode->attr.gid;
    sb->rdev = f->inode->rdev;
    sb->size = f->inode->attr.size;
    sb->blksize = f->inode->attr.blksize;
    sb->blocks = f->inode->attr.blocks;
    sb->atime = f->inode->attr.atime;
    sb->mtime = f->inode->attr.mtime;
    sb->ctime = f->inode->attr.ctime;
    return 0;
}

i64 vfs_mkdir(const char *path, u16 mode) {
    const char *rel;
    vfs_mount_t *mnt = find_mount(path, &rel);
    if (!mnt) return (i64)ENOENT;
    char tmp[MAX_PATH];
    if (strlcpy(tmp, rel, sizeof(tmp)) >= sizeof(tmp)) return (i64)ENAMETOOLONG;
    /* strrchr finds the last '/' so we can split parent dir from base name */
    char *last_slash = strrchr(tmp, '/');
    vfs_inode_t parent;
    if (last_slash) {
        *last_slash = '\0';
        i64 r = path_walk(&mnt->root, tmp, &parent);
        if (r < 0) return r;
        return parent.ops->mkdir(&parent, last_slash + 1, mode);
    }
    return mnt->root.ops->mkdir(&mnt->root, tmp, mode);
}

i64 vfs_unlink(const char *path) {
    const char *rel;
    vfs_mount_t *mnt = find_mount(path, &rel);
    if (!mnt) return (i64)ENOENT;
    char tmp[MAX_PATH];
    if (strlcpy(tmp, rel, sizeof(tmp)) >= sizeof(tmp)) return (i64)ENAMETOOLONG;
    char *last_slash = strrchr(tmp, '/');
    vfs_inode_t parent;
    if (last_slash) {
        *last_slash = '\0';
        i64 r = path_walk(&mnt->root, tmp, &parent);
        if (r < 0) return r;
        return parent.ops->unlink(&parent, last_slash + 1);
    }
    return mnt->root.ops->unlink(&mnt->root, tmp);
}

i64 vfs_create(const char *path, u16 mode) {
    const char *rel;
    vfs_mount_t *mnt = find_mount(path, &rel);
    if (!mnt) return (i64)ENOENT;
    char tmp[MAX_PATH];
    if (strlcpy(tmp, rel, sizeof(tmp)) >= sizeof(tmp)) return (i64)ENAMETOOLONG;
    /* strrchr gives us the last slash cleanly without a manual loop */
    char *last_slash = strrchr(tmp, '/');
    vfs_inode_t parent;
    if (last_slash) {
        *last_slash = '\0';
        *last_slash = 0;
        i64 r = path_walk(&mnt->root, tmp, &parent);
        if (r < 0) return r;
        vfs_inode_t new_inode;
        r = parent.ops->create(&parent, last_slash + 1, mode, &new_inode);
        if (r < 0) return r;
        int fd = fd_alloc();
        if (fd < 0) return (i64)ENOMEM;
        fd_table[fd].inode = inode_alloc();
        if (!fd_table[fd].inode) { fd_table[fd].in_use = 0; return (i64)ENOMEM; }
        *fd_table[fd].inode = new_inode;
        fd_table[fd].offset = 0;
        fd_table[fd].flags = 0;
        fd_table[fd].type = IT_FILE;
        fd_table[fd].pipe_data = NULL;
        return fd;
    }
    vfs_inode_t new_inode;
    i64 r = mnt->root.ops->create(&mnt->root, tmp, mode, &new_inode);
    if (r < 0) return r;
    int fd = fd_alloc();
    if (fd < 0) return (i64)ENOMEM;
    fd_table[fd].inode = inode_alloc();
    if (!fd_table[fd].inode) { fd_table[fd].in_use = 0; return (i64)ENOMEM; }
    *fd_table[fd].inode = new_inode;
    fd_table[fd].offset = 0;
    fd_table[fd].flags = 0;
    fd_table[fd].type = IT_FILE;
    fd_table[fd].pipe_data = NULL;
    return fd;
}

i64 vfs_readdir(u64 fd, void *buf, usize count) {
    vfs_fd_t *f = fd_get(fd);
    if (!f || !f->inode) return (i64)EBADF;
    if (!f->inode->ops->readdir) return (i64)ENOTDIR;
    usize offset = 0;
    return f->inode->ops->readdir(f->inode, buf, count, &offset);
}

i64 vfs_dup(u64 oldfd) {
    vfs_fd_t *old = fd_get(oldfd);
    if (!old) return (i64)EBADF;
    int newfd = fd_alloc();
    if (newfd < 0) return (i64)ENOMEM;
    fd_table[newfd] = *old;
    if (old->inode) inode_ref(old->inode);
    return newfd;
}

i64 vfs_dup2(u64 oldfd, u64 newfd) {
    if (oldfd == newfd) return (i64)newfd;
    vfs_fd_t *old = fd_get(oldfd);
    if (!old) return (i64)EBADF;
    if (newfd >= FD_MAX) return (i64)EBADF;
    if (fd_table[newfd].in_use) vfs_close(newfd);
    fd_table[newfd] = *old;
    fd_table[newfd].in_use = 1;
    if (old->inode) inode_ref(old->inode);
    return (i64)newfd;
}

i64 sys_dup2(u64 oldfd, u64 newfd) {
    return vfs_dup2(oldfd, newfd);
}

static u64 get_time_ms(void) {

    return pit_ticks;
}

i64 sys_pipe(int *pipefd) {
    void *p = pipe_create();
    if (!p) return (i64)ENOMEM;
    int rfd = -1, wfd = -1;
    for (int i = 3; i < FD_MAX; i++) {
        if (!fd_table[i].in_use) { rfd = i; break; }
    }
    if (rfd < 0) { pipe_destroy(p); return (i64)ENOMEM; }
    for (int i = 3; i < FD_MAX; i++) {
        if (!fd_table[i].in_use && i != rfd) { wfd = i; break; }
    }
    if (wfd < 0) { pipe_destroy(p); return (i64)ENOMEM; }
    fd_table[rfd].in_use = 1;
    fd_table[rfd].type = IT_PIPE;
    fd_table[rfd].offset = 0;
    fd_table[rfd].flags = 0;
    fd_table[rfd].inode = NULL;
    fd_table[rfd].pipe_data = p;
    fd_table[wfd].in_use = 1;
    fd_table[wfd].type = IT_PIPE;
    fd_table[wfd].offset = 0;
    fd_table[wfd].flags = 1;
    fd_table[wfd].inode = NULL;
    fd_table[wfd].pipe_data = p;
    pipefd[0] = rfd;
    pipefd[1] = wfd;
    return 0;
}
