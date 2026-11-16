/* Guest mutex handles backed by host pthreads. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>
#include <stddef.h>
typedef struct { int type, protocol; } GuestAttr;
/* Linked libc marks its heap mutex at guest offset 0x20. Guest fields must not
 * overlap the host pthread object, whose size and layout vary by platform. */
typedef struct {
    unsigned char guest_reserved[0x20];
    uint32_t libc_flags;
    pthread_mutex_t native;
    unsigned users; /* holders and waiters: destroy returns EBUSY while any remain */
#ifdef TEMPEST_FEX
    uintptr_t owner;
    unsigned depth;
#endif
} GuestMutex;
#ifdef TEMPEST_FEX
static _Thread_local char owner_key;
static uintptr_t owner_id(void) { return (uintptr_t)&owner_key; }
static void record_lock(GuestMutex *mutex) {
    if (__atomic_load_n(&mutex->owner,__ATOMIC_ACQUIRE)==owner_id()) ++mutex->depth;
    else { mutex->depth=1; __atomic_store_n(&mutex->owner,owner_id(),__ATOMIC_RELEASE); }
}
#endif
_Static_assert(offsetof(GuestMutex,libc_flags)==0x20,"Guest libc mutex flags offset");
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
    case ENOTSUP: code=45; break;
    default: fprintf(stderr, "STOP: unmapped host pthread error %d\n", e); exit(21);
    }
    return (int32_t)(UINT32_C(0x80020000) | code);
}
static ABI int32_t attr_init(GuestAttr **out) {
    if (!out) return orbis_error(EINVAL);
    GuestAttr *attr = runtime_low_alloc(sizeof(*attr));
    if (!attr) return orbis_error(ENOMEM);
    attr->type = 1; attr->protocol = 0; *out = attr; return 0;
}
static ABI int32_t attr_type(GuestAttr **attr, int type) {
    if (!attr || !*attr || type < 1 || type > 4) return orbis_error(EINVAL);
    (*attr)->type = type; return 0;
}
/* PS4 and host POSIX protocol values are NONE=0, INHERIT=1 and PROTECT=2. */
static ABI int32_t attr_protocol(GuestAttr **attr, int protocol) {
    if (!attr || !*attr || protocol < 0 || protocol > 2) return orbis_error(EINVAL);
    (*attr)->protocol = protocol;
    return 0;
}
static ABI int32_t attr_destroy(GuestAttr **attr) {
    if (!attr || !*attr) return orbis_error(EINVAL);
    runtime_low_free(*attr); *attr = NULL; return 0;
}
static ABI int32_t mutex_init(GuestMutex **out, GuestAttr **attr, const char *name) {
    (void)name;
    if (!out || (attr && !*attr)) return orbis_error(EINVAL);
    int type = attr ? (*attr)->type : 1;
    if (type < 1 || type > 4) return orbis_error(EINVAL);
    GuestMutex *mutex = runtime_low_alloc(sizeof(*mutex));
    if (!mutex) return orbis_error(ENOMEM);
    pthread_mutexattr_t native_attr;
    int e = pthread_mutexattr_init(&native_attr);
    if (e) { runtime_low_free(mutex); return orbis_error(e); }
    int native_type = type == 2 ? PTHREAD_MUTEX_RECURSIVE : type == 3 ? PTHREAD_MUTEX_NORMAL : PTHREAD_MUTEX_ERRORCHECK;
    e = pthread_mutexattr_settype(&native_attr, native_type);
    if (!e && attr) e = pthread_mutexattr_setprotocol(&native_attr, (*attr)->protocol);
    if (!e) e = pthread_mutex_init(&mutex->native, &native_attr);
    pthread_mutexattr_destroy(&native_attr);
    if (e) { runtime_low_free(mutex); return orbis_error(e); }
#ifdef TEMPEST_FEX
    mutex->owner=0; mutex->depth=0;
#endif
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
#ifdef TEMPEST_FEX
static int compare_time(const struct timespec *a,const struct timespec *b) {
    return a->tv_sec<b->tv_sec ? -1 : a->tv_sec>b->tv_sec ? 1 :
           a->tv_nsec<b->tv_nsec ? -1 : a->tv_nsec>b->tv_nsec;
}
/* A parked host mutex cannot run queued x86 signal handlers. Use bounded waits
 * so suspension/resume callbacks execute while this thread owns no target lock. */
static int mutex_lock_checkpoint(GuestMutex *mutex,const struct timespec *deadline) {
    int error=pthread_mutex_trylock(&mutex->native);
    if (error!=EBUSY) return error;
    for (;;) {
        struct timespec slice;
        if (clock_gettime(CLOCK_REALTIME,&slice)) return errno;
        if (deadline && compare_time(&slice,deadline)>=0) return ETIMEDOUT;
        slice.tv_nsec+=10000000;
        if (slice.tv_nsec>=1000000000) { ++slice.tv_sec; slice.tv_nsec-=1000000000; }
        if (deadline && compare_time(deadline,&slice)<0) slice=*deadline;
        error=pthread_mutex_timedlock(&mutex->native,&slice);
        if (error!=ETIMEDOUT) return error;
        if (runtime_fex_exception_pending()) runtime_exception_poll();
    }
}
#endif
static ABI int32_t mutex_lock(GuestMutex **mutex) {
    int32_t e = ensure_mutex(mutex);
    if (e) return e;
    GuestMutex *value=*mutex;
    __atomic_add_fetch(&value->users,1,__ATOMIC_ACQ_REL);
    const uint64_t wait_start = runtime_wait_begin();
#ifdef TEMPEST_FEX
    e = orbis_error(mutex_lock_checkpoint(value,NULL));
#else
    e = orbis_error(pthread_mutex_lock(&value->native));
#endif
    runtime_wait_end("mutex lock", wait_start, "mutex", (long long)(uintptr_t)value);
    if (e) __atomic_sub_fetch(&value->users,1,__ATOMIC_RELEASE);
    if (!e) {
#ifdef TEMPEST_FEX
        record_lock(value);
#endif
        ++locks;
    }
    return e;
}
static ABI int32_t mutex_trylock(GuestMutex **mutex) {
    int32_t e = ensure_mutex(mutex);
    if (e) return e;
    GuestMutex *value=*mutex;
    __atomic_add_fetch(&value->users,1,__ATOMIC_ACQ_REL);
    e = orbis_error(pthread_mutex_trylock(&value->native));
    if (e) __atomic_sub_fetch(&value->users,1,__ATOMIC_RELEASE);
    if (!e) {
#ifdef TEMPEST_FEX
        record_lock(value);
#endif
        ++locks;
    }
    return e;
}
static ABI int32_t mutex_unlock(GuestMutex **mutex) {
    if (!mutex || (uintptr_t)*mutex == 2) return orbis_error(EINVAL);
    if ((uintptr_t)*mutex < 2) return orbis_error(EPERM);
    GuestMutex *value=*mutex;
    if (!__atomic_load_n(&value->users,__ATOMIC_ACQUIRE)) return orbis_error(EPERM);
#ifdef TEMPEST_FEX
    if (__atomic_load_n(&value->owner,__ATOMIC_ACQUIRE)!=owner_id()) return orbis_error(EPERM);
    if (!--value->depth) __atomic_store_n(&value->owner,0,__ATOMIC_RELEASE);
#endif
    int32_t e = orbis_error(pthread_mutex_unlock(&value->native));
#ifdef TEMPEST_FEX
    if (e) { ++value->depth; __atomic_store_n(&value->owner,owner_id(),__ATOMIC_RELEASE); }
#endif
    if (!e) {
        __atomic_sub_fetch(&value->users,1,__ATOMIC_RELEASE);
        ++unlocks;
    }
    return e;
}
static ABI int32_t mutex_destroy(GuestMutex **mutex) {
    if (!mutex || (uintptr_t)*mutex == 2) return orbis_error(EINVAL);
    if ((uintptr_t)*mutex < 2) return 0;
    /* Winpthreads permits destroying locked mutexes; retain owners and waiters. */
    if (__atomic_load_n(&(*mutex)->users,__ATOMIC_ACQUIRE)) return orbis_error(EBUSY);
    int e = pthread_mutex_destroy(&(*mutex)->native);
    if (!e) { runtime_low_free(*mutex); *mutex = (GuestMutex *)(uintptr_t)2; }
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
/* Condition variables: opaque handles over host pthread_cond (CLOCK_REALTIME,
 * like FreeBSD's default), sharing the host mutex behind GuestMutex. */
typedef struct { pthread_cond_t native; unsigned waiters; } GuestCond;
static size_t conds, waits, wakeups;
static ABI int32_t cond_init(GuestCond **out, void **attr, const char *name) {
    (void)attr; (void)name;
    if (!out) return orbis_error(EINVAL);
    GuestCond *c = runtime_low_alloc(sizeof(*c));
    if (!c) return orbis_error(ENOMEM);
    c->waiters = 0;
    int e = pthread_cond_init(&c->native, NULL);
    if (e) { runtime_low_free(c); return orbis_error(e); }
    *out = c; ++conds; return 0;
}
static int32_t ensure_cond(GuestCond **cond) {
    if (!cond) return orbis_error(EINVAL);
    uintptr_t value = __atomic_load_n((uintptr_t *)cond, __ATOMIC_ACQUIRE);
    if (value == 1) return orbis_error(EINVAL); /* FreeBSD destroyed condition sentinel */
    if (value >= 2) return 0;
    pthread_mutex_lock(&static_init);
    int32_t e = 0;
    if (!*cond) {
        GuestCond *c = NULL;
        e = cond_init(&c, NULL, NULL);
        if (!e) __atomic_store_n(cond, c, __ATOMIC_RELEASE);
    } else if ((uintptr_t)*cond == 1) e = orbis_error(EINVAL);
    pthread_mutex_unlock(&static_init);
    return e;
}
static ABI int32_t cond_destroy(GuestCond **cond) {
    if (!cond) return orbis_error(EINVAL);
    if ((uintptr_t)*cond == 1) return orbis_error(EINVAL);
    if (!*cond) return 0;
    if (__atomic_load_n(&(*cond)->waiters,__ATOMIC_ACQUIRE)) return orbis_error(EBUSY);
    int e = pthread_cond_destroy(&(*cond)->native);
    if (!e) { runtime_low_free(*cond); *cond = (GuestCond *)(uintptr_t)1; }
    return orbis_error(e);
}
#ifdef TEMPEST_FEX
/* Periodic host waits give deferred guest exceptions a safe point. Keep the guest
 * handle in use, but drop every recursive native lock level before guest reentry. */
static int cond_wait_checkpoint(GuestCond *cond,GuestMutex *mutex,const struct timespec *deadline) {
    if (__atomic_load_n(&mutex->owner,__ATOMIC_ACQUIRE)!=owner_id()) return EPERM;
    const unsigned depth=mutex->depth;
    for (unsigned i=1;i<depth;++i) {
        int error=pthread_mutex_unlock(&mutex->native);
        if (error) return error;
    }
    mutex->depth=0; __atomic_store_n(&mutex->owner,0,__ATOMIC_RELEASE);
    int error=0;
    for (;;) {
        struct timespec slice;
        if (clock_gettime(CLOCK_REALTIME,&slice)) { error=errno; break; }
        slice.tv_nsec+=10000000; /* 10 ms maximum delivery latency while parked */
        if (slice.tv_nsec>=1000000000) { ++slice.tv_sec; slice.tv_nsec-=1000000000; }
        if (deadline && compare_time(deadline,&slice)<0) slice=*deadline;
        error=pthread_cond_timedwait(&cond->native,&mutex->native,&slice);
        if (runtime_fex_exception_pending()) {
            int unlock_error=pthread_mutex_unlock(&mutex->native);
            if (unlock_error) { error=unlock_error; break; }
            runtime_exception_poll();
            int lock_error=mutex_lock_checkpoint(mutex,NULL);
            if (lock_error) return lock_error;
            // A notification may arrive while the guest handler runs. Return an
            // allowed spurious wake so the guest rechecks its protected predicate.
            error=0;
            if (deadline) {
                struct timespec now;
                if (clock_gettime(CLOCK_REALTIME,&now)) error=errno;
                else if (compare_time(&now,deadline)>=0) error=ETIMEDOUT;
            }
            break;
        }
        if (error!=ETIMEDOUT) break;
        if (deadline && compare_time(&slice,deadline)>=0) break;
    }
    for (unsigned i=1;i<depth;++i) {
        int lock_error=pthread_mutex_lock(&mutex->native);
        if (lock_error) return lock_error;
    }
    mutex->depth=depth; __atomic_store_n(&mutex->owner,owner_id(),__ATOMIC_RELEASE);
    return error;
}
#endif
static ABI int32_t cond_wait(GuestCond **cond, GuestMutex **mutex) {
    int32_t e = ensure_cond(cond);
    if (e) return e;
    if (!mutex || (uintptr_t)*mutex < 3) return orbis_error(EINVAL);
    ++waits;
    __atomic_add_fetch(&(*cond)->waiters,1,__ATOMIC_ACQ_REL);
    /* Keep the mutex's users count: cond_wait releases and reacquires the host lock,
     * while the guest handle remains in use throughout the wait. */
    const uint64_t wait_start = runtime_wait_begin();
#ifdef TEMPEST_FEX
    const int r = cond_wait_checkpoint(*cond,*mutex,NULL);
#else
    const int r = pthread_cond_wait(&(*cond)->native, &(*mutex)->native);
#endif
    runtime_wait_end("cond wait", wait_start, "cond", (long long)(uintptr_t)*cond);
    __atomic_sub_fetch(&(*cond)->waiters,1,__ATOMIC_RELEASE);
    return orbis_error(r);
}
static int32_t cond_wait_until(GuestCond **cond, GuestMutex **mutex, const struct timespec *end) {
    int32_t e = ensure_cond(cond);
    if (e) return e;
    if (!mutex || (uintptr_t)*mutex < 3 || !end) return orbis_error(EINVAL);
    ++waits;
    __atomic_add_fetch(&(*cond)->waiters,1,__ATOMIC_ACQ_REL);
    const uint64_t wait_start = runtime_wait_begin();
#ifdef TEMPEST_FEX
    int e_native = cond_wait_checkpoint(*cond,*mutex,end);
#else
    int e_native = pthread_cond_timedwait(&(*cond)->native, &(*mutex)->native, end);
#endif
    runtime_wait_end("cond timedwait", wait_start, "cond", (long long)(uintptr_t)*cond);
    __atomic_sub_fetch(&(*cond)->waiters,1,__ATOMIC_RELEASE);
    return timed_error(e_native);
}
static ABI int32_t cond_timedwait(GuestCond **cond, GuestMutex **mutex, uint32_t usec) {
    struct timespec end;
    if (deadline_after(&end, usec)) return orbis_error(EINVAL);
    return cond_wait_until(cond, mutex, &end);
}
/* scePthreadMutexTimedlock(mutex, usec): a lock that gives up after `usec`, reporting
 * 0x8002003c (ETIMEDOUT) as the other timed waits here do. */
static ABI int32_t mutex_timedlock(GuestMutex **mutex, uint32_t usec) {
    int32_t e = ensure_mutex(mutex);
    if (e) return e;
    struct timespec end;
    if (deadline_after(&end, usec)) return orbis_error(EINVAL);
    GuestMutex *value=*mutex;
    __atomic_add_fetch(&value->users,1,__ATOMIC_ACQ_REL);
    const uint64_t wait_start = runtime_wait_begin();
#ifdef TEMPEST_FEX
    e = timed_error(mutex_lock_checkpoint(value,&end));
#else
    e = timed_error(pthread_mutex_timedlock(&value->native, &end));
#endif
    runtime_wait_end("mutex timedlock", wait_start, "mutex", (long long)(uintptr_t)value);
    if (e) __atomic_sub_fetch(&value->users,1,__ATOMIC_RELEASE);
    if (!e) {
#ifdef TEMPEST_FEX
        record_lock(value);
#endif
        ++locks;
    }
    return e;
}
static ABI int32_t cond_signal(GuestCond **cond) {
    int32_t e = ensure_cond(cond);
    if (e) return e;
    ++wakeups; return orbis_error(pthread_cond_signal(&(*cond)->native));
}
static ABI int32_t cond_broadcast(GuestCond **cond) {
    int32_t e = ensure_cond(cond);
    if (e) return e;
    ++wakeups; return orbis_error(pthread_cond_broadcast(&(*cond)->native));
}
/* PS4 struct timespec is {int64 sec, int64 nsec}, identical to Linux x86-64. */
typedef struct { int64_t sec, nsec; } GuestTimespec;
/* POSIX pthread APIs return positive guest errno values, unlike scePthread. */
static int32_t posix_result(int32_t result) { return result ? (int32_t)((uint32_t)result & 0xffff) : 0; }
/* POSIX init omits scePthread's debug-name argument; guest handles stay one pointer wide. */
static ABI int32_t posix_mutex_init(GuestMutex **m, GuestAttr **a) { return posix_result(mutex_init(m,a,NULL)); }
static ABI int32_t posix_mutex_destroy(GuestMutex **m) { return posix_result(mutex_destroy(m)); }
static ABI int32_t posix_mutex_lock(GuestMutex **m) { return posix_result(mutex_lock(m)); }
static ABI int32_t posix_mutex_trylock(GuestMutex **m) { return posix_result(mutex_trylock(m)); }
static ABI int32_t posix_mutex_unlock(GuestMutex **m) { return posix_result(mutex_unlock(m)); }
static ABI int32_t posix_attr_init(GuestAttr **a) { return posix_result(attr_init(a)); }
static ABI int32_t posix_attr_destroy(GuestAttr **a) { return posix_result(attr_destroy(a)); }
static ABI int32_t posix_attr_type(GuestAttr **a, int type) { return posix_result(attr_type(a,type)); }
static ABI int32_t posix_attr_protocol(GuestAttr **a, int protocol) { return posix_result(attr_protocol(a,protocol)); }
static ABI int32_t posix_cond_init(GuestCond **c, void **a) { return posix_result(cond_init(c,a,NULL)); }
static ABI int32_t posix_cond_destroy(GuestCond **c) { return posix_result(cond_destroy(c)); }
static ABI int32_t posix_cond_wait(GuestCond **c, GuestMutex **m) { return posix_result(cond_wait(c,m)); }
static ABI int32_t posix_cond_signal(GuestCond **c) { return posix_result(cond_signal(c)); }
static ABI int32_t posix_cond_broadcast(GuestCond **c) { return posix_result(cond_broadcast(c)); }
/* pthread_cond_timedwait uses an absolute CLOCK_REALTIME deadline, not scePthread's usec. */
static ABI int32_t posix_cond_timedwait(GuestCond **c, GuestMutex **m, const GuestTimespec *end) {
    if (!end || end->sec < 0 || end->nsec < 0 || end->nsec >= 1000000000) return 22;
    struct timespec deadline={(time_t)end->sec,(long)end->nsec};
    if ((int64_t)deadline.tv_sec != end->sec) return 22;
    return posix_result(cond_wait_until(c,m,&deadline));
}
static ABI int32_t condattr_init(void **attr) {
    if (!attr) return (int32_t)0x80020016; /* EINVAL */
    *attr=runtime_low_alloc(16);
    if (*attr) memset(*attr,0,16);
    return *attr ? 0 : (int32_t)0x8002000C; /* ENOMEM */
}
static ABI int32_t condattr_destroy(void **attr) {
    if (!attr || !*attr) return (int32_t)0x80020016;
    runtime_low_free(*attr); *attr=NULL; return 0;
}
static ABI int32_t posix_condattr_init(void **a) { return posix_result(condattr_init(a)); }
static ABI int32_t posix_condattr_destroy(void **a) { return posix_result(condattr_destroy(a)); }
static const RuntimeExport posix_exports[]={
    {"pthread_mutex_init",posix_mutex_init}, {"pthread_mutex_destroy",posix_mutex_destroy},
    {"pthread_mutex_lock",posix_mutex_lock}, {"pthread_mutex_trylock",posix_mutex_trylock},
    {"pthread_mutex_unlock",posix_mutex_unlock},
    {"pthread_mutexattr_init",posix_attr_init}, {"pthread_mutexattr_destroy",posix_attr_destroy},
    {"pthread_mutexattr_settype",posix_attr_type}, {"pthread_mutexattr_setprotocol",posix_attr_protocol},
    {"pthread_cond_init",posix_cond_init}, {"pthread_cond_destroy",posix_cond_destroy},
    {"pthread_cond_wait",posix_cond_wait}, {"pthread_cond_timedwait",posix_cond_timedwait},
    {"pthread_cond_signal",posix_cond_signal}, {"pthread_cond_broadcast",posix_cond_broadcast},
    {"pthread_condattr_init",posix_condattr_init}, {"pthread_condattr_destroy",posix_condattr_destroy},
};
uintptr_t runtime_mutex_resolve(const char *name) {
    uintptr_t posix=RUNTIME_LOOKUP(posix_exports,name);
    if (posix) return posix;
    if (!strcmp(name,"m5-2bsNfv7s#libkernel")) return (uintptr_t)condattr_init;
    if (!strcmp(name,"waPcxYiR3WA#libkernel")) return (uintptr_t)condattr_destroy;
    if (!strcmp(name,"2Tb92quprl0#libkernel")) return (uintptr_t)cond_init;
    if (!strcmp(name,"g+PZd2hiacg#libkernel")) return (uintptr_t)cond_destroy;
    if (!strcmp(name,"WKAXJ4XBPQ4#libkernel")) return (uintptr_t)cond_wait;
    if (!strcmp(name,"BmMjYxmew1w#libkernel")) return (uintptr_t)cond_timedwait;
    if (!strcmp(name,"kDh-NfxgMtE#libkernel")) return (uintptr_t)cond_signal;
    if (!strcmp(name,"JGgj7Uvrl+A#libkernel")) return (uintptr_t)cond_broadcast;
    if (!strcmp(name,"7H0iTOciTLo#libkernel")) return (uintptr_t)posix_mutex_lock;
    if (!strcmp(name,"2Z+PpY6CaJg#libkernel")) return (uintptr_t)posix_mutex_unlock;
    if (!strcmp(name, "F8bUHwAG284#libkernel")) return (uintptr_t)attr_init;
    if (!strcmp(name, "iMp8QpE+XO4#libkernel")) return (uintptr_t)attr_type;
    if (!strcmp(name, "smWEktiyyG0#libkernel")) return (uintptr_t)attr_destroy;
    if (!strcmp(name, "1FGvU0i9saQ#libkernel")) return (uintptr_t)attr_protocol;
    if (!strcmp(name, "cmo1RIYva9o#libkernel")) return (uintptr_t)mutex_init;
    if (!strcmp(name, "9UK1vLZQft4#libkernel")) return (uintptr_t)mutex_lock;
    if (!strcmp(name, "upoVrzMHFeE#libkernel")) return (uintptr_t)mutex_trylock;
    if (!strcmp(name, "IafI2PxcPnQ#libkernel")) return (uintptr_t)mutex_timedlock;
    if (!strcmp(name, "tn3VlD0hG60#libkernel")) return (uintptr_t)mutex_unlock;
    if (!strcmp(name, "2Of0f+3mhhE#libkernel")) return (uintptr_t)mutex_destroy;
    return 0;
}
void runtime_mutex_report(void) {
    printf("Runtime: mutexes created=%zu, locks=%zu, unlocks=%zu\n", created, locks, unlocks);
    printf("Runtime: condition variables created=%zu, waits=%zu, wakeups=%zu\n", conds, waits, wakeups);
}

