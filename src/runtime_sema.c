/* Kernel semaphores: 32-bit IDs, counted tokens and FIFO waiters.
 * Priority-ordered semaphores are approximated by FIFO order. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>
#define ERR(n) ((int32_t)(UINT32_C(0x80020000)|(n)))
typedef struct Waiter {
    int32_t need, result;
    int done;
    pthread_cond_t event;
    struct Waiter *next;
} Waiter;
typedef struct Sema {
    uint32_t id, attr;
    int32_t count, initial, maximum;
    unsigned active;
    int deleted;
    Waiter *first;
    struct Sema *next;
} Sema;
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static Sema *registry;
static uint32_t next_id=1;
static size_t created, deleted, acquired, signaled, timed_out;
/* A found semaphore moves to the front. New ones went in front, so the ones made at boot
 * sat behind every one made since, each wait and signal walking past them under the one lock
 * (from Awakenchan 0.9.34 beta 9.9). Not for the diagnostic snapshot, which leaves the order
 * alone. TEMPEST_SEMA_FRONT=0: the list keeps its order, as before. */
static Sema *find_ordered(uint32_t id,int reorder) {
    static int front=-1;
    if (front<0) { const char *e=getenv("TEMPEST_SEMA_FRONT"); front=!(e && e[0]=='0'); }
    Sema **link=&registry;
    for (Sema *s=registry;s;link=&s->next,s=s->next) {
        if (s->id!=id) continue;
        if (reorder && front && link!=&registry) { *link=s->next; s->next=registry; registry=s; }
        return s;
    }
    return NULL;
}
static Sema *find(uint32_t id) { return find_ordered(id,1); }
static void host_check(int e) {
    if (e) { fprintf(stderr,"STOP: semaphore host error %d\n",e); exit(21); }
}
/* Waits use this clock for their deadlines. winpthreads condition variables accept only
 * CLOCK_REALTIME (pthread_condattr_setclock(CLOCK_MONOTONIC) fails with EINVAL). */
#ifdef _WIN32
#define WAIT_CLOCK CLOCK_REALTIME
#else
#define WAIT_CLOCK CLOCK_MONOTONIC
#endif
static uint64_t now_ns(void) {
    struct timespec t;
    if (clock_gettime(WAIT_CLOCK,&t)) { perror("clock_gettime"); exit(1); }
    return (uint64_t)t.tv_sec*1000000000+(uint64_t)t.tv_nsec;
}
static ABI int32_t sem_create(uint32_t *out,const char *name,uint32_t attr,
                              int32_t initial,int32_t maximum,const void *options) {
    if (!out || !name || attr>2 || initial<0 || maximum<=0 || initial>maximum) return ERR(22);
    if (options) { fputs("STOP: semaphore options are not implemented\n",stderr); exit(21); }
    Sema *s=calloc(1,sizeof(*s));
    if (!s) return ERR(12);
    pthread_mutex_lock(&lock);
    if (next_id==UINT32_MAX) { pthread_mutex_unlock(&lock); free(s); return ERR(28); }
    s->id=next_id++; s->attr=attr; s->count=s->initial=initial; s->maximum=maximum;
    s->next=registry; registry=s; *out=s->id; ++created;
    if (runtime_verbose()) printf("Runtime: semaphore id=%u, attr=%u, initial=%d, max=%d\n",s->id,attr,initial,maximum);
    pthread_mutex_unlock(&lock);
    return 0;
}
static int32_t wait_count(uint32_t id,int32_t need,uint32_t *timeout,int block) {
    pthread_mutex_lock(&lock);
    Sema *s=find(id);
    int32_t result=0;
    if (!s) result=ERR(3);
    else if (need<=0 || need>s->maximum) result=ERR(22);
    else if (s->count>=need) { s->count-=need; ++acquired; }
    else if (!block) result=ERR(16);
    else if (timeout && !*timeout) { result=ERR(60); ++timed_out; }
    else {
        /* Priority-ordered semaphores (attr 2) are served FIFO: there is no
           PS4 priority scheduler, host threads run at equal priority. */
        Waiter w={.need=need};
        pthread_condattr_t attr;
        host_check(pthread_condattr_init(&attr));
        host_check(pthread_condattr_setclock(&attr,WAIT_CLOCK));
        host_check(pthread_cond_init(&w.event,&attr));
        host_check(pthread_condattr_destroy(&attr));
        Waiter **tail=&s->first;
        while (*tail) tail=&(*tail)->next;
        *tail=&w; ++s->active;
        uint64_t deadline=timeout ? now_ns()+(uint64_t)*timeout*1000 : 0;
#ifndef TEMPEST_FEX
        struct timespec end={.tv_sec=(time_t)(deadline/1000000000),.tv_nsec=(long)(deadline%1000000000)};
#endif
        while (!w.done) {
#ifdef TEMPEST_FEX
            uint64_t slice=now_ns()+UINT64_C(10000000);
            if (timeout && deadline<slice) slice=deadline;
            struct timespec slice_end={(time_t)(slice/1000000000),(long)(slice%1000000000)};
            int e=pthread_cond_timedwait(&w.event,&lock,&slice_end);
            if (runtime_fex_exception_pending()) {
                // active retains s and w while guest code may signal/delete it.
                pthread_mutex_unlock(&lock);
                runtime_exception_poll();
                pthread_mutex_lock(&lock);
            }
            if (e==ETIMEDOUT && !w.done && (!timeout || now_ns()<deadline)) continue;
#else
            int e=timeout ? pthread_cond_timedwait(&w.event,&lock,&end) : pthread_cond_wait(&w.event,&lock);
#endif
            if (e==ETIMEDOUT && !w.done) {
                Waiter **p=&s->first;
                while (*p!=&w) p=&(*p)->next;
                *p=w.next; w.result=ERR(60); w.done=1; ++timed_out;
            } else if (e!=ETIMEDOUT) host_check(e);
        }
        result=w.result;
        if (!result) ++acquired;
        if (timeout) {
            uint64_t now=now_ns();
            *timeout=result ? 0 : (uint32_t)(now>=deadline ? 0 : (deadline-now)/1000);
        }
        host_check(pthread_cond_destroy(&w.event));
        if (!--s->active && s->deleted) free(s);
    }
    pthread_mutex_unlock(&lock);
    return result;
}
static ABI int32_t sem_wait(uint32_t id,int32_t need,uint32_t *timeout) {
    const uint64_t wait_start=runtime_wait_begin();
    const int32_t r=wait_count(id,need,timeout,1);
    runtime_wait_end("semaphore wait",wait_start,"id",id);
    return r;
}
static ABI int32_t sem_signal(uint32_t id,int32_t count) {
    pthread_mutex_lock(&lock);
    Sema *s=find(id);
    int32_t result=0;
    if (!s) result=ERR(3);
    else if (count<=0 || count>s->maximum-s->count) result=ERR(22);
    else {
        s->count+=count; ++signaled;
        Waiter **p=&s->first;
        while (*p) {
            Waiter *w=*p;
            if (w->need>s->count) { p=&w->next; continue; }
            s->count-=w->need; *p=w->next; w->done=1;
            host_check(pthread_cond_signal(&w->event));
        }
    }
    pthread_mutex_unlock(&lock);
    return result;
}
static int32_t wake_all(Sema *s,int32_t result) {
    int32_t count=0;
    while (s->first) {
        Waiter *w=s->first; s->first=w->next;
        w->result=result; w->done=1; ++count;
        host_check(pthread_cond_signal(&w->event));
    }
    return count;
}
static ABI int32_t sem_delete(uint32_t id) {
    pthread_mutex_lock(&lock);
    Sema **p=&registry;
    while (*p && (*p)->id!=id) p=&(*p)->next;
    if (!*p) { pthread_mutex_unlock(&lock); return ERR(3); }
    Sema *s=*p; *p=s->next; s->deleted=1; ++deleted;
    wake_all(s,ERR(13));
    if (!s->active) free(s);
    pthread_mutex_unlock(&lock);
    return 0;
}
/* POSIX semaphores (sem_init/sem_wait/sem_post/sem_destroy, shadPS4 semaphore.cpp): sem_t is a
 * pointer to a record below 1 TiB. Failures return -1 with errno (EINVAL, EOVERFLOW at
 * SEM_VALUE_MAX 0x7fffffff); pshared is ignored, there is one process. */
typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t wake;
    int32_t value;
    unsigned waiters;
} PosixSem;
typedef struct { int64_t sec, nsec; } PosixSemTimespec;
_Static_assert(sizeof(PosixSem *)==8,"PS4 POSIX semaphore slot is one pointer");
static int32_t posix_sem_error(int error) {
    *runtime_errno()=runtime_guest_errno(error);
    return -1;
}
static ABI int32_t posix_sem_init(PosixSem **sem,int32_t pshared,uint32_t value) {
    (void)pshared; /* The runtime has one guest process, as shadPS4 does. */
    if (!sem || value>INT32_MAX) return posix_sem_error(EINVAL);
    PosixSem *s=runtime_low_alloc(sizeof(*s));
    if (!s) return posix_sem_error(ENOMEM);
    memset(s,0,sizeof(*s));
    int error=pthread_mutex_init(&s->lock,NULL);
    if (error) { runtime_low_free(s); return posix_sem_error(error); }
    /* Default condition-variable deadlines are absolute CLOCK_REALTIME, the
     * POSIX sem_timedwait clock, on both Linux and Android. */
    error=pthread_cond_init(&s->wake,NULL);
    if (error) { pthread_mutex_destroy(&s->lock); runtime_low_free(s); return posix_sem_error(error); }
    s->value=(int32_t)value;
    *sem=s;
    return 0;
}
static ABI int32_t posix_sem_destroy(PosixSem **sem) {
    if (!sem || !*sem) return posix_sem_error(EINVAL);
    PosixSem *s=*sem;
    int error=pthread_mutex_lock(&s->lock);
    if (error) return posix_sem_error(error);
    if (s->waiters) { pthread_mutex_unlock(&s->lock); return posix_sem_error(EBUSY); }
    pthread_mutex_unlock(&s->lock);
    error=pthread_cond_destroy(&s->wake);
    if (!error) error=pthread_mutex_destroy(&s->lock);
    if (error) return posix_sem_error(error);
    *sem=NULL;
    runtime_low_free(s);
    return 0;
}
/* mode 0 polls, 1 waits indefinitely, 2 uses a guest absolute real-time deadline.
 * Tokens and wakeups share one lock, so posts cannot be lost between the empty
 * check and the wait; spurious wakeups never consume an absent token. */
static int32_t posix_sem_take(PosixSem **sem,int mode,const PosixSemTimespec *until) {
    if (!sem || !*sem || (mode==2 && !until)) return posix_sem_error(EINVAL);
    PosixSem *s=*sem;
    int error=pthread_mutex_lock(&s->lock);
    if (error) return posix_sem_error(error);
    if (!s->value && !mode) error=EAGAIN;
    else if (!s->value) {
        if (mode==2 && (until->nsec<0 || until->nsec>=1000000000)) error=EINVAL;
        else {
            struct timespec end={0};
            if (mode==2) { end.tv_sec=(time_t)until->sec; end.tv_nsec=(long)until->nsec; }
            ++s->waiters;
            while (!s->value) {
#ifdef TEMPEST_FEX
                struct timespec slice;
                if (clock_gettime(CLOCK_REALTIME,&slice)) { error=errno; break; }
                slice.tv_nsec+=10000000;
                if (slice.tv_nsec>=1000000000) { ++slice.tv_sec; slice.tv_nsec-=1000000000; }
                if (mode==2 && (end.tv_sec<slice.tv_sec ||
                    (end.tv_sec==slice.tv_sec && end.tv_nsec<slice.tv_nsec))) slice=end;
                error=pthread_cond_timedwait(&s->wake,&s->lock,&slice);
                if (runtime_fex_exception_pending()) {
                    // waiters keeps destroy from freeing s during guest reentry.
                    pthread_mutex_unlock(&s->lock);
                    runtime_exception_poll();
                    pthread_mutex_lock(&s->lock);
                }
                if (!s->value && error==ETIMEDOUT && (mode!=2 ||
                    slice.tv_sec<end.tv_sec || (slice.tv_sec==end.tv_sec && slice.tv_nsec<end.tv_nsec))) {
                    error=0; continue;
                }
#else
                error=mode==2 ? pthread_cond_timedwait(&s->wake,&s->lock,&end) :
                                pthread_cond_wait(&s->wake,&s->lock);
#endif
                if (s->value) { error=0; break; }
                if (error) break;
            }
            --s->waiters;
        }
    }
    if (!error) --s->value;
    pthread_mutex_unlock(&s->lock);
    return error ? posix_sem_error(error) : 0;
}
static ABI int32_t posix_sem_wait(PosixSem **sem) {
    const uint64_t start=runtime_wait_begin();
    int32_t result=posix_sem_take(sem,1,NULL);
    runtime_wait_end("POSIX semaphore wait",start,"slot",(long long)(uintptr_t)sem);
    return result;
}
static ABI int32_t posix_sem_trywait(PosixSem **sem) { return posix_sem_take(sem,0,NULL); }
static ABI int32_t posix_sem_timedwait(PosixSem **sem,const PosixSemTimespec *until) {
    const uint64_t start=runtime_wait_begin();
    int32_t result=posix_sem_take(sem,2,until);
    runtime_wait_end("POSIX semaphore timed wait",start,"slot",(long long)(uintptr_t)sem);
    return result;
}
static ABI int32_t posix_sem_post(PosixSem **sem) {
    if (!sem || !*sem) return posix_sem_error(EINVAL);
    PosixSem *s=*sem;
    int error=pthread_mutex_lock(&s->lock);
    if (error) return posix_sem_error(error);
    if (s->value==INT32_MAX) error=EOVERFLOW;
    else {
        ++s->value;
        if (s->waiters) error=pthread_cond_signal(&s->wake);
    }
    pthread_mutex_unlock(&s->lock);
    return error ? posix_sem_error(error) : 0;
}
static ABI int32_t posix_sem_getvalue(PosixSem **sem,int32_t *value) {
    if (!sem || !*sem || !value) return posix_sem_error(EINVAL);
    PosixSem *s=*sem;
    int error=pthread_mutex_lock(&s->lock);
    if (error) return posix_sem_error(error);
    *value=s->value;
    pthread_mutex_unlock(&s->lock);
    return 0;
}
static const RuntimeExport posix_exports[]={
    {"sem_init",posix_sem_init}, {"sem_destroy",posix_sem_destroy},
    {"sem_wait",posix_sem_wait}, {"sem_trywait",posix_sem_trywait},
    {"sem_timedwait",posix_sem_timedwait}, {"sem_post",posix_sem_post},
    {"sem_getvalue",posix_sem_getvalue},
};
uintptr_t runtime_sema_resolve(const char *name) {
    if (!strcmp(name,"188x57JYp0g#libkernel")) return (uintptr_t)sem_create;
    if (!strcmp(name,"Zxa0VhQVTsk#libkernel")) return (uintptr_t)sem_wait;
    if (!strcmp(name,"4czppHBiriw#libkernel")) return (uintptr_t)sem_signal;
    if (!strcmp(name,"R1Jvn8bSCW8#libkernel")) return (uintptr_t)sem_delete;
    return RUNTIME_LOOKUP(posix_exports,name);
}
void runtime_sema_report(void) {
    pthread_mutex_lock(&lock);
    printf("Runtime: semaphores created=%zu, deleted=%zu, acquired=%zu, signals=%zu, timeouts=%zu\n",
           created,deleted,acquired,signaled,timed_out);
    pthread_mutex_unlock(&lock);
}

