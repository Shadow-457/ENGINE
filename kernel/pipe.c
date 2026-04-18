#include "../include/kernel.h"

#define PIPE_BUF_SIZE 4096

typedef struct {
    u8 buf[PIPE_BUF_SIZE];
    usize head;
    usize tail;
    usize count;
    int read_open;
    int write_open;
} pipe_t;

void *pipe_create(void) {
    pipe_t *p = (pipe_t*)heap_malloc(sizeof(pipe_t));
    if (!p) return NULL;
    memset(p, 0, sizeof(pipe_t));
    p->read_open = 1;
    p->write_open = 1;
    return p;
}

i64 pipe_read(void *pipe, void *buf, usize count) {
    pipe_t *p = (pipe_t*)pipe;
    if (!p || !p->read_open) return (i64)EBADF;
    if (count == 0) return 0;
    usize total = 0;
    u8 *out = (u8*)buf;
    while (total < count) {
        if (p->count == 0) {
            if (!p->write_open) return total ? (i64)total : 0;
            return total ? (i64)total : (i64)EAGAIN;
        }
        out[total++] = p->buf[p->head];
        p->head = (p->head + 1) % PIPE_BUF_SIZE;
        p->count--;
    }
    return (i64)total;
}

i64 pipe_write(void *pipe, const void *buf, usize count) {
    pipe_t *p = (pipe_t*)pipe;
    if (!p || !p->write_open) return (i64)EBADF;
    if (count == 0) return 0;
    usize total = 0;
    const u8 *in = (const u8*)buf;
    while (total < count) {
        if (p->count == PIPE_BUF_SIZE) {
            return total ? (i64)total : (i64)EAGAIN;
        }
        p->buf[p->tail] = in[total++];
        p->tail = (p->tail + 1) % PIPE_BUF_SIZE;
        p->count++;
    }
    return (i64)total;
}

void pipe_destroy(void *pipe) {
    if (pipe) heap_free(pipe);
}
