/* ================================================================
 *  Systrix OS — user/pthread.h
 *  POSIX thread API built on top of sys_clone (#56) + sys_futex (#202)
 *
 *  Implements:
 *    pthread_create / pthread_join / pthread_exit
 *    pthread_mutex_init / lock / trylock / unlock / destroy
 *    pthread_cond_init / wait / signal / broadcast / destroy
 *
 *  Usage:
 *    #include "pthread.h"   (no linking needed — header-only)
 *
 *  Internal design:
 *    Threads are clones sharing VM (CLONE_VM | CLONE_FS | CLONE_FILES
 *    | CLONE_SIGHAND | CLONE_THREAD).  Each thread gets a 64 KB stack
 *    allocated from mmap.  Mutexes are futex words (4 bytes).
 *    Condition variables are futex words used as sequence counters.
 * ================================================================ */

#pragma once
#include "libc.h"

/* ── Types ───────────────────────────────────────────────────── */

typedef uint64_t pthread_t;

typedef struct {
    volatile uint32_t lock;   /* 0=unlocked, 1=locked, 2=contended */
} pthread_mutex_t;

typedef struct {
    volatile uint32_t seq;    /* sequence counter — bumped on signal */
} pthread_cond_t;

typedef struct {
    void *(*start_routine)(void *);
    void *arg;
    void *retval;
    volatile uint32_t done;   /* futex: 0=running, 1=finished */
} _pthread_state_t;

/* Stub attr types — we ignore attributes for now */
typedef struct { int _unused; } pthread_attr_t;
typedef struct { int _unused; } pthread_mutexattr_t;
typedef struct { int _unused; } pthread_condattr_t;

#define PTHREAD_MUTEX_INITIALIZER  { 0 }
#define PTHREAD_COND_INITIALIZER   { 0 }

/* ── Thread stack size ───────────────────────────────────────── */
#define _PTHREAD_STACK_SZ  (64 * 1024)   /* 64 KB per thread */

/* ── CLONE flags (match Linux ABI) ──────────────────────────── */
#define CLONE_VM        0x00000100
#define CLONE_FS        0x00000200
#define CLONE_FILES     0x00000400
#define CLONE_SIGHAND   0x00000800
#define CLONE_THREAD    0x00010000
#define CLONE_SETTLS    0x00080000

/* ── Raw syscall helpers ─────────────────────────────────────── */

static inline long _futex(volatile uint32_t *addr, long op, uint32_t val,
                           uint64_t timeout_ms) {
    long r;
    __asm__ volatile("syscall"
        : "=a"(r)
        : "0"(202LL),
          "D"((long)addr),
          "S"(op),
          "d"((long)val),
          "r"(timeout_ms)
        : "rcx", "r11", "memory");
    return r;
}

/* clone(flags, stack, ptid, ctid, newtls) — Linux x86-64 ABI */
static inline long _clone(unsigned long flags, void *stack,
                           int *ptid, int *ctid, void *newtls) {
    long r;
    register void *r8  __asm__("r8")  = newtls;
    register int  *r10 __asm__("r10") = ctid;
    __asm__ volatile("syscall"
        : "=a"(r)
        : "0"(56LL),
          "D"((long)flags),
          "S"(stack),
          "d"(ptid),
          "r"(r10),
          "r"(r8)
        : "rcx", "r11", "memory");
    return r;
}

/* ── Thread trampoline ───────────────────────────────────────── */

/* Each new thread starts here.  The state pointer is passed via
 * the top of the new stack (written by pthread_create before clone). */
static void _pthread_trampoline(void) {
    /* Retrieve state pointer from top of our stack.
     * pthread_create placed it at stack_top - 8 before the clone. */
    _pthread_state_t *st;
    __asm__ volatile("mov 8(%%rsp), %0" : "=r"(st));

    st->retval = st->start_routine(st->arg);
    st->done   = 1;
    /* Wake any pthread_join waiters */
    _futex(&st->done, 1 /* FUTEX_WAKE */, 0x7fffffff, 0);

    /* Exit this thread — use exit_group(0) */
    __asm__ volatile("syscall" :: "a"(231LL), "D"(0LL) : "rcx","r11");
    __builtin_unreachable();
}

/* ── pthread_create ──────────────────────────────────────────── */

static inline int pthread_create(pthread_t *thread,
                                  const pthread_attr_t *attr,
                                  void *(*start)(void *), void *arg) {
    (void)attr;

    /* Allocate stack + state in one mmap */
    uint8_t *mem = (uint8_t *)mmap(NULL,
        _PTHREAD_STACK_SZ + sizeof(_pthread_state_t),
        3 /*PROT_READ|PROT_WRITE*/,
        0x22 /*MAP_PRIVATE|MAP_ANONYMOUS*/,
        -1, 0);
    if (!mem) return (int)ENOMEM;

    /* State lives just above the stack */
    _pthread_state_t *st = (_pthread_state_t *)(mem + _PTHREAD_STACK_SZ);
    st->start_routine = start;
    st->arg           = arg;
    st->retval        = NULL;
    st->done          = 0;

    /* Stack grows down; top of stack = mem + _PTHREAD_STACK_SZ.
     * Place state pointer at (stack_top - 8) so trampoline can find it.
     * Also need a fake return address below that — use 0 (thread exits). */
    uint64_t *stktop = (uint64_t *)(mem + _PTHREAD_STACK_SZ);
    stktop[-1] = (uint64_t)st;     /* state ptr  */
    stktop[-2] = 0;                /* return addr (never reached) */

    /* New stack pointer points at the fake return address */
    void *new_rsp = (void *)(stktop - 2);

    long pid = _clone(
        CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD,
        new_rsp, NULL, NULL, NULL);

    if (pid < 0) {
        munmap(mem, _PTHREAD_STACK_SZ + sizeof(_pthread_state_t));
        return (int)ENOMEM;
    }

    /* In parent: store the state pointer as the thread handle */
    *thread = (pthread_t)st;

    /* Child starts at _pthread_trampoline — but clone copies RIP too.
     * We need the child to run our trampoline, not continue here.
     * Standard trick: push trampoline addr as the "return address" on
     * the child stack so its first ret goes there. We already wrote 0
     * at stktop[-2].  Replace with trampoline address. */
    stktop[-2] = (uint64_t)_pthread_trampoline;

    return 0;
}

/* ── pthread_join ────────────────────────────────────────────── */

static inline int pthread_join(pthread_t thread, void **retval) {
    _pthread_state_t *st = (_pthread_state_t *)thread;

    /* Wait until done == 1 */
    while (st->done == 0)
        _futex(&st->done, 0 /* FUTEX_WAIT */, 0, 0);

    if (retval) *retval = st->retval;
    return 0;
}

/* ── pthread_exit ────────────────────────────────────────────── */

static inline void pthread_exit(void *retval) __attribute__((noreturn));
static inline void pthread_exit(void *retval) {
    (void)retval;
    /* Just exit the process — for now we don't track retval on self-exit */
    __asm__ volatile("syscall" :: "a"(231LL), "D"(0LL) : "rcx","r11");
    __builtin_unreachable();
}

/* ── pthread_self ────────────────────────────────────────────── */

static inline pthread_t pthread_self(void) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "0"(39LL) : "rcx","r11","memory");
    return (pthread_t)r;
}

/* ================================================================
 *  Mutex — futex-based
 *
 *  lock word:
 *    0 = unlocked
 *    1 = locked, no waiters
 *    2 = locked, waiters sleeping
 * ================================================================ */

static inline int pthread_mutex_init(pthread_mutex_t *m,
                                      const pthread_mutexattr_t *a) {
    (void)a;
    m->lock = 0;
    return 0;
}

static inline int pthread_mutex_destroy(pthread_mutex_t *m) {
    (void)m;
    return 0;
}

static inline int pthread_mutex_trylock(pthread_mutex_t *m) {
    uint32_t expected = 0;
    /* Try CAS: 0 → 1 */
    uint32_t old;
    __asm__ volatile(
        "lock cmpxchgl %2, %1"
        : "=a"(old), "+m"(m->lock)
        : "r"((uint32_t)1), "0"(expected)
        : "memory");
    return (old == 0) ? 0 : (int)EBUSY;
}

static inline int pthread_mutex_lock(pthread_mutex_t *m) {
    uint32_t c;

    /* Fast path: uncontended lock */
    __asm__ volatile(
        "lock cmpxchgl %2, %1"
        : "=a"(c), "+m"(m->lock)
        : "r"((uint32_t)1), "0"((uint32_t)0)
        : "memory");
    if (c == 0) return 0;   /* got it */

    /* Slow path: mark contended and sleep */
    do {
        if (c == 2 || pthread_mutex_trylock(m) != 0) {
            /* Set to 2 (contended) and sleep */
            __asm__ volatile(
                "lock xchgl %0, %1"
                : "=r"(c), "+m"(m->lock)
                : "0"((uint32_t)2)
                : "memory");
            if (c != 0)
                _futex(&m->lock, 0 /* FUTEX_WAIT */, 2, 0);
        }
        /* Try again: CAS 0→2 */
        __asm__ volatile(
            "lock cmpxchgl %2, %1"
            : "=a"(c), "+m"(m->lock)
            : "r"((uint32_t)2), "0"((uint32_t)0)
            : "memory");
    } while (c != 0);

    return 0;
}

static inline int pthread_mutex_unlock(pthread_mutex_t *m) {
    uint32_t prev;
    __asm__ volatile(
        "lock xaddl %0, %1"
        : "=r"(prev), "+m"(m->lock)
        : "0"((uint32_t)-1)
        : "memory");

    /* If there were waiters (lock was 2), wake one */
    if (prev != 1) {
        m->lock = 0;
        _futex(&m->lock, 1 /* FUTEX_WAKE */, 1, 0);
    }
    return 0;
}

/* ================================================================
 *  Condition variable — futex sequence counter
 * ================================================================ */

static inline int pthread_cond_init(pthread_cond_t *c,
                                     const pthread_condattr_t *a) {
    (void)a;
    c->seq = 0;
    return 0;
}

static inline int pthread_cond_destroy(pthread_cond_t *c) {
    (void)c;
    return 0;
}

static inline int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) {
    uint32_t seq = c->seq;
    pthread_mutex_unlock(m);
    /* Sleep until seq changes */
    _futex(&c->seq, 0 /* FUTEX_WAIT */, seq, 0);
    pthread_mutex_lock(m);
    return 0;
}

static inline int pthread_cond_signal(pthread_cond_t *c) {
    __asm__ volatile("lock addl $1, %0" : "+m"(c->seq) :: "memory");
    _futex(&c->seq, 1 /* FUTEX_WAKE */, 1, 0);
    return 0;
}

static inline int pthread_cond_broadcast(pthread_cond_t *c) {
    __asm__ volatile("lock addl $1, %0" : "+m"(c->seq) :: "memory");
    _futex(&c->seq, 1 /* FUTEX_WAKE */, 0x7fffffff, 0);
    return 0;
}
