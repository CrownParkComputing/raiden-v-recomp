/* Guest-pointer POSIX semaphores: counted tokens, blocking wakeups and absolute deadlines. */
#undef NDEBUG
#include "../src/runtime_sema.c"
#include <assert.h>
#include <stdatomic.h>

static _Thread_local int32_t guest_errno;
int32_t *runtime_errno(void) { return &guest_errno; }
int32_t runtime_guest_errno(int error) {
    switch (error) {
    case EINVAL:return 22; case ENOMEM:return 12; case EBUSY:return 16;
    case EAGAIN:return 35; case ETIMEDOUT:return 60; case EOVERFLOW:return 84;
    default:assert(!"unexpected host error");return 5;
    }
}
void *runtime_low_alloc(size_t size) { return calloc(1,size); }
void runtime_low_free(void *pointer) { free(pointer); }
int runtime_verbose(void) { return 0; }
uint64_t runtime_wait_begin(void) { return 0; }
void runtime_wait_end(const char *what,uint64_t start,const char *detail,long long arg) {
    (void)what; (void)start; (void)detail; (void)arg;
}
uintptr_t runtime_lookup(const RuntimeExport *table,size_t count,const char *name) {
    for (size_t i=0;i<count;++i) if (!strcmp(table[i].name,name)) return (uintptr_t)table[i].function;
    return 0;
}
#ifdef TEMPEST_FEX
static _Thread_local int pending_exception;
static void (*exception_action)(void);
int runtime_fex_exception_pending(void) { return pending_exception; }
void runtime_exception_poll(void) {
    assert(pending_exception && exception_action);
    pending_exception=0; exception_action();
}
static PosixSem *checkpoint_sem;
static uint32_t checkpoint_id;
static void checkpoint_post(void) {
    assert(posix_sem_destroy(&checkpoint_sem)==-1 && guest_errno==16);
    assert(posix_sem_post(&checkpoint_sem)==0); /* callback must not hold s->lock */
}
static void checkpoint_signal(void) {
    assert(sem_signal(checkpoint_id,1)==0); /* callback must not hold registry lock */
}
static void test_exception_checkpoints(void) {
    assert(posix_sem_init(&checkpoint_sem,0,0)==0);
    exception_action=checkpoint_post; pending_exception=1;
    assert(posix_sem_wait(&checkpoint_sem)==0 && !pending_exception);
    assert(posix_sem_destroy(&checkpoint_sem)==0);
    assert(sem_create(&checkpoint_id,"exception-checkpoint",0,0,1,NULL)==0);
    exception_action=checkpoint_signal; pending_exception=1;
    assert(sem_wait(checkpoint_id,1,NULL)==0 && !pending_exception);
    assert(sem_delete(checkpoint_id)==0);
}
#endif

static void pause_ms(unsigned ms) {
    struct timespec delay={ms/1000,(long)(ms%1000)*1000000};
    while (nanosleep(&delay,&delay)) assert(errno==EINTR);
}
static uint64_t monotonic_ns(void) {
    struct timespec t; assert(clock_gettime(CLOCK_MONOTONIC,&t)==0);
    return (uint64_t)t.tv_sec*1000000000+(uint64_t)t.tv_nsec;
}
static PosixSemTimespec deadline_ms(unsigned ms) {
    struct timespec now; assert(clock_gettime(CLOCK_REALTIME,&now)==0);
    int64_t ns=now.tv_nsec+(int64_t)ms*1000000;
    return (PosixSemTimespec){now.tv_sec+ns/1000000000,ns%1000000000};
}
typedef struct {
    PosixSem **sem;
    atomic_int entered,done;
    int timed,result,error;
} Worker;
static void *take_one(void *argument) {
    Worker *w=argument;
    PosixSemTimespec until=deadline_ms(1000);
    atomic_store(&w->entered,1);
    w->result=w->timed ? posix_sem_timedwait(w->sem,&until) : posix_sem_wait(w->sem);
    w->error=guest_errno;
    atomic_store(&w->done,1);
    return NULL;
}
static void started(Worker *w) {
    uint64_t limit=monotonic_ns()+UINT64_C(2000000000);
    while (!atomic_load(&w->entered) && monotonic_ns()<limit) pause_ms(1);
    assert(atomic_load(&w->entered));
}

int main(void) {
    struct { uint64_t before; PosixSem *sem; uint64_t after; }
        slot={UINT64_C(0x123456789abcdef0),NULL,UINT64_C(0xfedcba9876543210)};
    PosixSem **sem=&slot.sem;
    assert(posix_sem_init(sem,0,2)==0 && slot.sem);
    assert(slot.before==UINT64_C(0x123456789abcdef0) && slot.after==UINT64_C(0xfedcba9876543210));
    int32_t value=-1;
    assert(posix_sem_getvalue(sem,&value)==0 && value==2);
    assert(posix_sem_wait(sem)==0 && posix_sem_trywait(sem)==0);
    assert(posix_sem_trywait(sem)==-1 && guest_errno==35);
    assert(posix_sem_getvalue(sem,&value)==0 && value==0);

    Worker workers[4]={0}; pthread_t threads[4];
    for (int i=0;i<4;++i) {
        workers[i].sem=sem;
        assert(pthread_create(&threads[i],NULL,take_one,&workers[i])==0);
        started(&workers[i]);
    }
    pause_ms(20);
    for (int i=0;i<4;++i) assert(!atomic_load(&workers[i].done));
    for (int i=0;i<4;++i) assert(posix_sem_post(sem)==0);
    for (int i=0;i<4;++i) {
        assert(pthread_join(threads[i],NULL)==0);
        assert(atomic_load(&workers[i].done) && workers[i].result==0);
    }
    assert(posix_sem_getvalue(sem,&value)==0 && value==0);

    PosixSemTimespec expired={0,0},bad={0,1000000000};
    assert(posix_sem_timedwait(sem,&expired)==-1 && guest_errno==60);
    assert(posix_sem_timedwait(sem,&bad)==-1 && guest_errno==22);
    PosixSemTimespec until=deadline_ms(40);
    uint64_t before=monotonic_ns();
    assert(posix_sem_timedwait(sem,&until)==-1 && guest_errno==60);
    assert(monotonic_ns()-before>=UINT64_C(20000000));
    Worker timed={.sem=sem,.timed=1}; pthread_t thread;
    assert(pthread_create(&thread,NULL,take_one,&timed)==0);
    started(&timed); pause_ms(20); assert(!atomic_load(&timed.done));
    assert(posix_sem_post(sem)==0 && pthread_join(thread,NULL)==0 && timed.result==0);
    assert(posix_sem_destroy(sem)==0 && !slot.sem);
    assert(posix_sem_wait(sem)==-1 && guest_errno==22);
    assert(posix_sem_destroy(sem)==-1 && guest_errno==22);

    assert(posix_sem_init(sem,0,UINT32_MAX)==-1 && guest_errno==22 && !slot.sem);
    assert(posix_sem_init(sem,0,INT32_MAX)==0);
    assert(posix_sem_post(sem)==-1 && guest_errno==84);
    assert(posix_sem_getvalue(sem,&value)==0 && value==INT32_MAX);
    assert(posix_sem_destroy(sem)==0);
    assert(posix_sem_init(NULL,0,0)==-1 && guest_errno==22);
    assert(posix_sem_wait(NULL)==-1 && guest_errno==22);
    assert(runtime_sema_resolve("sem_init")== (uintptr_t)posix_sem_init);

    /* The separate kernel API still uses IDs and counted acquisitions. */
    uint32_t id=0;
    assert(sem_create(&id,"kernel",0,1,4,NULL)==0 && id);
    assert(sem_wait(id,1,NULL)==0 && sem_signal(id,2)==0 && sem_delete(id)==0);
    assert(runtime_sema_resolve("188x57JYp0g#libkernel")== (uintptr_t)sem_create);
#ifdef TEMPEST_FEX
    test_exception_checkpoints();
#endif
    puts("POSIX semaphore tokens, wakeups and absolute deadlines passed");
    return 0;
}
