/* Linux pthread-backed opaque PS4 mutex handles. No guest threads yet. */
#define _GNU_SOURCE
#include "runtime.h"
#include "guest_cpu.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>
typedef struct { int type; } GuestAttr;
typedef struct { pthread_mutex_t native; } GuestMutex;
static size_t created, locks, unlocks;
static int32_t orbis_error(int e) {
    if (!e) return 0;
    unsigned code;
    switch (e) {
    case EPERM: code=1; break;
    case ENOMEM: code=12; break;
    case EBUSY: code=16; break;
    case EINVAL: code=22; break;
    case EAGAIN: code=35; break;
    case EDEADLK: code=11; break;
    default: fprintf(stderr, "STOP: unmapped host pthread error %d\n", e); exit(21);
    }
    return (int32_t)(UINT32_C(0x80020000) | code);
}
static ABI int32_t attr_init(GuestAttr **out) {
    if (!out) return orbis_error(EINVAL);
    GuestAttr *attr = malloc(sizeof(*attr));
    if (!attr) return orbis_error(ENOMEM);
    attr->type = 1; *out = attr; return 0;
}
static ABI int32_t attr_type(GuestAttr **attr, int type) {
    if (!attr || !*attr || type < 1 || type > 4) return orbis_error(EINVAL);
    (*attr)->type = type; return 0;
}
/* Priority protocols need a PS4 scheduler; the value is validated and recorded only. */
static ABI int32_t attr_protocol(GuestAttr **attr, int protocol) {
    if (!attr || !*attr || protocol < 0 || protocol > 2) return orbis_error(EINVAL);
    return 0;
}
static ABI int32_t attr_destroy(GuestAttr **attr) {
    if (!attr || !*attr) return orbis_error(EINVAL);
    free(*attr); *attr = NULL; return 0;
}
static ABI int32_t mutex_init(GuestMutex **out, GuestAttr **attr, const char *name) {
    (void)name;
    if (!out || (attr && !*attr)) return orbis_error(EINVAL);
    int type = attr ? (*attr)->type : 1;
    if (type < 1 || type > 4) return orbis_error(EINVAL);
    GuestMutex *mutex = malloc(sizeof(*mutex));
    if (!mutex) return orbis_error(ENOMEM);
    pthread_mutexattr_t native_attr;
    int e = pthread_mutexattr_init(&native_attr);
    if (e) { free(mutex); return orbis_error(e); }
    int native_type = type == 2 ? PTHREAD_MUTEX_RECURSIVE : type == 3 ? PTHREAD_MUTEX_NORMAL : PTHREAD_MUTEX_ERRORCHECK;
    e = pthread_mutexattr_settype(&native_attr, native_type);
    if (!e) e = pthread_mutex_init(&mutex->native, &native_attr);
    pthread_mutexattr_destroy(&native_attr);
    if (e) { free(mutex); return orbis_error(e); }
    *out = mutex; ++created; return 0;
}
static pthread_mutex_t static_init = PTHREAD_MUTEX_INITIALIZER;
/* Statically initialized handles (0/1) are created once, even under contention. */
static int32_t ensure_mutex(GuestMutex **mutex) {
    if (!mutex) return orbis_error(EINVAL);
    uintptr_t value = __atomic_load_n((uintptr_t *)mutex, __ATOMIC_ACQUIRE);
    if (value == 2) return orbis_error(EINVAL);
    if (value >= 2) return 0;
    pthread_mutex_lock(&static_init);
    int32_t e = 0;
    if ((uintptr_t)*mutex < 2) {
        GuestMutex *created_mutex = NULL;
        e = mutex_init(&created_mutex, NULL, NULL);
        if (!e) __atomic_store_n(mutex, created_mutex, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&static_init);
    return e;
}
/* uncharted_android: contended guest mutexes spin before they sleep, as FreeBSD's libthr does on
 * the PS4. Bionic's pthread_mutex_lock goes to the futex at the first collision and every
 * contended unlock then makes a wake syscall; Naughty Dog's six job workers take one lock tens of
 * thousands of times a second, and on the phone they spent 86% of their samples in those two
 * syscalls (3.5 cores, half of it kernel time). GOW3_MUTEX_SPIN=N tries the lock N times with a
 * CPU pause before blocking; 0 (the default) blocks at once. Measured on 2026-10-23 with 200
 * (~5 us): kernel time 2.2 -> 0.3 cores but user time 2.0 -> 4.9, the process a core busier,
 * because the lock is contended without pause and the futex sleeps were what held the workers
 * back. Kept for experiments. */
static size_t spin_hits, spin_misses;
static int mutex_spins(void) {
    static int spins = -1;
    int n = __atomic_load_n(&spins, __ATOMIC_RELAXED);
    if (n < 0) {
        const char *value = getenv("GOW3_MUTEX_SPIN");
        char *end = NULL;
        long parsed = value && value[0] ? strtol(value, &end, 10) : 0;
        n = (end && *end) || parsed < 0 || parsed > 100000 ? 0 : (int)parsed;
        __atomic_store_n(&spins, n, __ATOMIC_RELAXED);
    }
    return n;
}
static inline void cpu_pause(void) {
#if defined(__aarch64__)
    __asm__ volatile("isb" ::: "memory");
#elif defined(__x86_64__)
    __builtin_ia32_pause();
#endif
}
/* uncharted_android: per-window counts for the 5-second statistics (runtime_mutex_stats, called
 * from the GPU library's frame statistics): how often the guest locks, collides, waits and wakes,
 * to see what keeps the job workers on their lock. Relaxed atomics; reset by each report. */
static struct {
    size_t locks, contended, cond_waits, cond_timeouts, signals, broadcasts;
} window;
#define COUNT(field) __atomic_fetch_add(&window.field, 1, __ATOMIC_RELAXED)
/* Who takes the guest locks: one guest mutex_lock in CALLER_SAMPLE records its guest return
 * address and the next CALLER_FRAMES - 1 image addresses found on the guest stack (the callers of
 * the game's lock wrapper; a heuristic: stale return addresses can show up), with the mutex. As
 * image offsets (probe.c hands over the range), so they read against the executable directly. */
#define CALLER_SAMPLE 64
#define CALLER_FRAMES 3
#define CALLER_SLOTS 128
typedef struct {
    uint64_t frames[CALLER_FRAMES]; /* image offsets + 1 (0: empty slot) */
    uintptr_t mutex;
    size_t samples, contended;
} CallerSlot;
static CallerSlot callers[CALLER_SLOTS];
static uintptr_t image_base, image_end;
void runtime_mutex_image(uintptr_t base, size_t size) { image_base = base; image_end = base + size; }
static void sample_caller(GuestMutex **mutex, int contended) {
    uintptr_t sp = guest_cpu_stack_pointer();
    if (!image_base || !sp) return;
    uint64_t frames[CALLER_FRAMES] = {0};
    unsigned found = 0;
    /* [sp] is the return address of the guest's call of the import: the lock's caller. */
    for (unsigned i = 0; i < 32 && found < CALLER_FRAMES; ++i) {
        uintptr_t word = ((const uintptr_t *)sp)[i];
        if (word > image_base && word < image_end) frames[found++] = word - image_base + 1;
        else if (i == 0) return; /* not called from the image (a trap or a host path) */
    }
    unsigned h = (unsigned)((frames[0] ^ frames[1] * 31u) % CALLER_SLOTS);
    for (unsigned probe = 0; probe < CALLER_SLOTS; ++probe, h = (h + 1) % CALLER_SLOTS) {
        CallerSlot *slot = &callers[h];
        uint64_t key = __atomic_load_n(&slot->frames[0], __ATOMIC_ACQUIRE);
        if (!key) {
            uint64_t empty = 0;
            if (!__atomic_compare_exchange_n(&slot->frames[0], &empty, frames[0], 0, __ATOMIC_ACQ_REL,
                                             __ATOMIC_ACQUIRE)) {
                key = empty;
            } else {
                for (unsigned f = 1; f < CALLER_FRAMES; ++f)
                    __atomic_store_n(&slot->frames[f], frames[f], __ATOMIC_RELAXED);
                __atomic_store_n(&slot->mutex, (uintptr_t)*mutex, __ATOMIC_RELAXED);
                key = frames[0];
            }
        }
        if (key == frames[0] && __atomic_load_n(&slot->frames[1], __ATOMIC_RELAXED) == frames[1]) {
            __atomic_fetch_add(&slot->samples, 1, __ATOMIC_RELAXED);
            if (contended) __atomic_fetch_add(&slot->contended, 1, __ATOMIC_RELAXED);
            return;
        }
    }
}
static void report_callers(void) {
    CallerSlot top[5];
    unsigned shown = 0;
    size_t total = 0;
    CallerSlot snapshot[CALLER_SLOTS];
    for (unsigned i = 0; i < CALLER_SLOTS; ++i) {
        CallerSlot *slot = &callers[i];
        snapshot[i].samples = __atomic_exchange_n(&slot->samples, 0, __ATOMIC_RELAXED);
        snapshot[i].contended = __atomic_exchange_n(&slot->contended, 0, __ATOMIC_RELAXED);
        for (unsigned f = 0; f < CALLER_FRAMES; ++f) snapshot[i].frames[f] = __atomic_load_n(&slot->frames[f], __ATOMIC_RELAXED);
        snapshot[i].mutex = __atomic_load_n(&slot->mutex, __ATOMIC_RELAXED);
        total += snapshot[i].samples;
    }
    for (unsigned n = 0; n < 5; ++n) {
        unsigned best = CALLER_SLOTS;
        for (unsigned i = 0; i < CALLER_SLOTS; ++i)
            if (snapshot[i].samples && (best == CALLER_SLOTS || snapshot[i].samples > snapshot[best].samples)) best = i;
        if (best == CALLER_SLOTS) break;
        top[shown++] = snapshot[best];
        snapshot[best].samples = 0;
    }
    if (!total) return;
    printf("Guest lock callers (1 in %d sampled, image offsets, caller <- its callers):", CALLER_SAMPLE);
    for (unsigned n = 0; n < shown; ++n) {
        printf("%s %.0f%% (%.0f%% contended) +%#" PRIx64, n ? ";" : "", 100.0 * top[n].samples / total,
               100.0 * top[n].contended / top[n].samples, top[n].frames[0] - 1);
        for (unsigned f = 1; f < CALLER_FRAMES && top[n].frames[f]; ++f) printf(" <- +%#" PRIx64, top[n].frames[f] - 1);
        printf(" mutex %#" PRIxPTR, top[n].mutex);
    }
    printf("\n");
}
/* A mutex the caller already holds keeps its type's answer: trylock fails with EBUSY on an
 * error-checking one and the blocking lock below reports EDEADLK; a recursive one succeeds. */
static int lock_native(pthread_mutex_t *native, int *contended) {
    *contended = 0;
    COUNT(locks);
    int e = pthread_mutex_trylock(native);
    if (e != EBUSY) return e;
    COUNT(contended);
    *contended = 1;
    int spins = mutex_spins();
    if (spins) {
        for (int i = 1; i < spins; ++i) {
            cpu_pause();
            e = pthread_mutex_trylock(native);
            if (e != EBUSY) { ++spin_hits; return e; }
        }
        ++spin_misses;
    }
    return pthread_mutex_lock(native);
}
void runtime_mutex_stats(double seconds) {
    if (seconds <= 0) return;
    size_t locks = __atomic_exchange_n(&window.locks, 0, __ATOMIC_RELAXED);
    size_t contended = __atomic_exchange_n(&window.contended, 0, __ATOMIC_RELAXED);
    size_t waits_ = __atomic_exchange_n(&window.cond_waits, 0, __ATOMIC_RELAXED);
    size_t timeouts = __atomic_exchange_n(&window.cond_timeouts, 0, __ATOMIC_RELAXED);
    size_t signals = __atomic_exchange_n(&window.signals, 0, __ATOMIC_RELAXED);
    size_t broadcasts = __atomic_exchange_n(&window.broadcasts, 0, __ATOMIC_RELAXED);
    printf("Guest sync/s: %.0f mutex locks, %.0f contended (%.1f%%); %.0f cond waits, %.0f timed out; "
           "%.0f signals, %.0f broadcasts\n",
           locks / seconds, contended / seconds, locks ? 100.0 * contended / locks : 0.0,
           waits_ / seconds, timeouts / seconds, signals / seconds, broadcasts / seconds);
    report_callers();
}
static ABI int32_t mutex_lock(GuestMutex **mutex) {
    static __thread unsigned sample_tick;
    int32_t e = ensure_mutex(mutex);
    if (e) return e;
    int contended;
    e = orbis_error(lock_native(&(*mutex)->native, &contended));
    if (!e) ++locks;
    if (++sample_tick % CALLER_SAMPLE == 0) sample_caller(mutex, contended);
    return e;
}
static ABI int32_t mutex_trylock(GuestMutex **mutex) {
    int32_t e = ensure_mutex(mutex);
    if (e) return e;
    e = orbis_error(pthread_mutex_trylock(&(*mutex)->native));
    if (!e) ++locks;
    return e;
}
static ABI int32_t mutex_unlock(GuestMutex **mutex) {
    if (!mutex || (uintptr_t)*mutex == 2) return orbis_error(EINVAL);
    if ((uintptr_t)*mutex < 2) return orbis_error(EPERM);
    int32_t e = orbis_error(pthread_mutex_unlock(&(*mutex)->native));
    if (!e) ++unlocks;
    return e;
}
static ABI int32_t mutex_destroy(GuestMutex **mutex) {
    if (!mutex || (uintptr_t)*mutex == 2) return orbis_error(EINVAL);
    if ((uintptr_t)*mutex < 2) return 0;
    int e = pthread_mutex_destroy(&(*mutex)->native);
    if (!e) { free(*mutex); *mutex = (GuestMutex *)(uintptr_t)2; }
    return orbis_error(e);
}
static int deadline_after(struct timespec *end, uint64_t usec) {
    if (clock_gettime(CLOCK_REALTIME, end)) return errno;
    uint64_t ns = (uint64_t)end->tv_nsec + (usec % 1000000) * 1000;
    end->tv_sec += (time_t)(usec / 1000000 + ns / 1000000000);
    end->tv_nsec = (long)(ns % 1000000000);
    return 0;
}
static int32_t timed_error(int e) { return e == ETIMEDOUT ? (int32_t)UINT32_C(0x8002003c) : orbis_error(e); }
static ABI int32_t mutex_timedlock(GuestMutex **mutex, uint32_t usec) {
    int32_t e = ensure_mutex(mutex);
    if (e) return e;
    struct timespec end;
    if (deadline_after(&end, usec)) return orbis_error(EINVAL);
    e = timed_error(pthread_mutex_timedlock(&(*mutex)->native, &end));
    if (!e) ++locks;
    return e;
}
/* Condition variables: opaque handles over host pthread_cond (CLOCK_REALTIME,
 * like FreeBSD's default), sharing the host mutex behind GuestMutex. */
typedef struct { pthread_cond_t native; } GuestCond;
static size_t conds, waits, wakeups;
static ABI int32_t cond_init(GuestCond **out, void **attr, const char *name) {
    (void)attr; (void)name;
    if (!out) return orbis_error(EINVAL);
    GuestCond *c = malloc(sizeof(*c));
    if (!c) return orbis_error(ENOMEM);
    int e = pthread_cond_init(&c->native, NULL);
    if (e) { free(c); return orbis_error(e); }
    *out = c; ++conds; return 0;
}
static int32_t ensure_cond(GuestCond **cond) {
    if (!cond) return orbis_error(EINVAL);
    if (__atomic_load_n((uintptr_t *)cond, __ATOMIC_ACQUIRE) >= 2) return 0;
    pthread_mutex_lock(&static_init);
    int32_t e = 0;
    if ((uintptr_t)*cond < 2) {
        GuestCond *c = NULL;
        e = cond_init(&c, NULL, NULL);
        if (!e) __atomic_store_n(cond, c, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&static_init);
    return e;
}
static ABI int32_t cond_destroy(GuestCond **cond) {
    if (!cond) return orbis_error(EINVAL);
    if ((uintptr_t)*cond < 2) return 0;
    int e = pthread_cond_destroy(&(*cond)->native);
    if (!e) { free(*cond); *cond = NULL; }
    return orbis_error(e);
}
static ABI int32_t cond_wait(GuestCond **cond, GuestMutex **mutex) {
    int32_t e = ensure_cond(cond);
    if (e) return e;
    if (!mutex || (uintptr_t)*mutex < 3) return orbis_error(EINVAL);
    ++waits; COUNT(cond_waits);
    return orbis_error(pthread_cond_wait(&(*cond)->native, &(*mutex)->native));
}
static int32_t cond_wait_until(GuestCond **cond, GuestMutex **mutex, const struct timespec *end) {
    int32_t e = ensure_cond(cond);
    if (e) return e;
    if (!mutex || (uintptr_t)*mutex < 3 || !end) return orbis_error(EINVAL);
    ++waits; COUNT(cond_waits);
    int r = pthread_cond_timedwait(&(*cond)->native, &(*mutex)->native, end);
    if (r == ETIMEDOUT) COUNT(cond_timeouts);
    return timed_error(r);
}
static ABI int32_t cond_timedwait(GuestCond **cond, GuestMutex **mutex, uint32_t usec) {
    struct timespec end;
    if (deadline_after(&end, usec)) return orbis_error(EINVAL);
    return cond_wait_until(cond, mutex, &end);
}
static ABI int32_t cond_signal(GuestCond **cond) {
    int32_t e = ensure_cond(cond);
    if (e) return e;
    ++wakeups; COUNT(signals); return orbis_error(pthread_cond_signal(&(*cond)->native));
}
static ABI int32_t cond_broadcast(GuestCond **cond) {
    int32_t e = ensure_cond(cond);
    if (e) return e;
    ++wakeups; COUNT(broadcasts); return orbis_error(pthread_cond_broadcast(&(*cond)->native));
}
/* PS4 struct timespec is {int64 sec, int64 nsec}, identical to Linux x86-64. */
typedef struct { int64_t sec, nsec; } GuestTimespec;
static ABI int32_t cond_abs_timedwait(GuestCond **cond, GuestMutex **mutex, const GuestTimespec *abs) {
    if (!abs || abs->nsec < 0 || abs->nsec >= 1000000000) return orbis_error(EINVAL);
    struct timespec end = {.tv_sec = (time_t)abs->sec, .tv_nsec = (long)abs->nsec};
    return cond_wait_until(cond, mutex, &end);
}
/* POSIX pthread APIs return positive guest errno values, unlike scePthread. */
static int32_t posix_result(int32_t result) { return result ? (int32_t)((uint32_t)result & 0xffff) : 0; }
static ABI int32_t posix_attr_init(GuestAttr **out) { return posix_result(attr_init(out)); }
static ABI int32_t posix_attr_type(GuestAttr **attr,int type) { return posix_result(attr_type(attr,type)); }
static ABI int32_t posix_attr_destroy(GuestAttr **attr) { return posix_result(attr_destroy(attr)); }
static ABI int32_t posix_mutex_init(GuestMutex **out,GuestAttr **attr) { return posix_result(mutex_init(out,attr,NULL)); }
static ABI int32_t posix_mutex_destroy(GuestMutex **m) { return posix_result(mutex_destroy(m)); }
static ABI int32_t posix_mutex_lock(GuestMutex **m) { return posix_result(mutex_lock(m)); }
static ABI int32_t posix_mutex_trylock(GuestMutex **m) { return posix_result(mutex_trylock(m)); }
static ABI int32_t posix_mutex_unlock(GuestMutex **m) { return posix_result(mutex_unlock(m)); }
static ABI int32_t posix_cond_init(GuestCond **c, void **a) { return posix_result(cond_init(c, a, NULL)); }
static ABI int32_t posix_cond_destroy(GuestCond **c) { return posix_result(cond_destroy(c)); }
static ABI int32_t posix_cond_wait(GuestCond **c, GuestMutex **m) { return posix_result(cond_wait(c, m)); }
static ABI int32_t posix_cond_timedwait(GuestCond **c, GuestMutex **m, const GuestTimespec *t) { return posix_result(cond_abs_timedwait(c, m, t)); }
static ABI int32_t posix_cond_signal(GuestCond **c) { return posix_result(cond_signal(c)); }
static ABI int32_t posix_cond_broadcast(GuestCond **c) { return posix_result(cond_broadcast(c)); }
static ABI int32_t condattr_init(void **attr) {
    if (!attr) return (int32_t)0x80020016; /* EINVAL */
    *attr=calloc(1,16);
    return *attr ? 0 : (int32_t)0x8002000C; /* ENOMEM */
}
static ABI int32_t condattr_destroy(void **attr) {
    if (!attr || !*attr) return (int32_t)0x80020016;
    free(*attr); *attr=NULL; return 0;
}
/* uc3: libScePosix's pthread_condattr_init (Uncharted 3): POSIX error numbers. */
static ABI int32_t posix_condattr_init(void **attr) { return posix_result(condattr_init(attr)); }
uintptr_t runtime_mutex_resolve(const char *name) {
    if (!strcmp(name,"mKoTx03HRWA#libkernel")) return (uintptr_t)posix_condattr_init;
    if (!strcmp(name,"m5-2bsNfv7s#libkernel")) return (uintptr_t)condattr_init;
    if (!strcmp(name,"waPcxYiR3WA#libkernel")) return (uintptr_t)condattr_destroy;
    if (!strcmp(name,"0TyVk4MSLt0#libkernel")) return (uintptr_t)posix_cond_init;
    if (!strcmp(name,"RXXqi4CtF8w#libkernel")) return (uintptr_t)posix_cond_destroy;
    if (!strcmp(name,"Op8TBGY5KHg#libkernel")) return (uintptr_t)posix_cond_wait;
    if (!strcmp(name,"27bAgiJmOh0#libkernel")) return (uintptr_t)posix_cond_timedwait;
    if (!strcmp(name,"2MOy+rUfuhQ#libkernel")) return (uintptr_t)posix_cond_signal;
    if (!strcmp(name,"mkx2fVhNMsg#libkernel")) return (uintptr_t)posix_cond_broadcast;
    if (!strcmp(name,"2Tb92quprl0#libkernel")) return (uintptr_t)cond_init;
    if (!strcmp(name,"g+PZd2hiacg#libkernel")) return (uintptr_t)cond_destroy;
    if (!strcmp(name,"WKAXJ4XBPQ4#libkernel")) return (uintptr_t)cond_wait;
    if (!strcmp(name,"BmMjYxmew1w#libkernel")) return (uintptr_t)cond_timedwait;
    if (!strcmp(name,"kDh-NfxgMtE#libkernel")) return (uintptr_t)cond_signal;
    if (!strcmp(name,"JGgj7Uvrl+A#libkernel")) return (uintptr_t)cond_broadcast;
    if (!strcmp(name,"IafI2PxcPnQ#libkernel")) return (uintptr_t)mutex_timedlock;
    if (!strcmp(name,"dQHWEsJtoE4#libkernel")) return (uintptr_t)posix_attr_init;
    if (!strcmp(name,"mDmgMOGVUqg#libkernel")) return (uintptr_t)posix_attr_type;
    if (!strcmp(name,"HF7lK46xzjY#libkernel")) return (uintptr_t)posix_attr_destroy;
    if (!strcmp(name,"ttHNfU+qDBU#libkernel")) return (uintptr_t)posix_mutex_init;
    if (!strcmp(name,"ltCfaGr2JGE#libkernel")) return (uintptr_t)posix_mutex_destroy;
    if (!strcmp(name,"7H0iTOciTLo#libkernel")) return (uintptr_t)posix_mutex_lock;
    if (!strcmp(name,"K-jXhbt2gn4#libkernel")) return (uintptr_t)posix_mutex_trylock;
    if (!strcmp(name,"2Z+PpY6CaJg#libkernel")) return (uintptr_t)posix_mutex_unlock;
    if (!strcmp(name, "F8bUHwAG284#libkernel")) return (uintptr_t)attr_init;
    if (!strcmp(name, "iMp8QpE+XO4#libkernel")) return (uintptr_t)attr_type;
    if (!strcmp(name, "smWEktiyyG0#libkernel")) return (uintptr_t)attr_destroy;
    if (!strcmp(name, "1FGvU0i9saQ#libkernel")) return (uintptr_t)attr_protocol;
    if (!strcmp(name, "cmo1RIYva9o#libkernel")) return (uintptr_t)mutex_init;
    if (!strcmp(name, "9UK1vLZQft4#libkernel")) return (uintptr_t)mutex_lock;
    if (!strcmp(name, "upoVrzMHFeE#libkernel")) return (uintptr_t)mutex_trylock;
    if (!strcmp(name, "tn3VlD0hG60#libkernel")) return (uintptr_t)mutex_unlock;
    if (!strcmp(name, "2Of0f+3mhhE#libkernel")) return (uintptr_t)mutex_destroy;
    return 0;
}
void runtime_mutex_report(void) {
    printf("Runtime: mutexes created=%zu, locks=%zu, unlocks=%zu; contended locks taken spinning=%zu, slept=%zu (GOW3_MUTEX_SPIN=%d)\n",
           created, locks, unlocks, spin_hits, spin_misses, mutex_spins());
    printf("Runtime: condition variables created=%zu, waits=%zu, wakeups=%zu\n", conds, waits, wakeups);
}

