/* PS4 pointer-sized pthread handles, ownership and condition-variable deadlines. */
#undef NDEBUG
#include "../src/runtime_mutex.c"
#include <assert.h>
#include <stdatomic.h>
#include <unistd.h>

void *runtime_low_alloc(size_t size) { return calloc(1,size); }
void runtime_low_free(void *pointer) { free(pointer); }
uint64_t runtime_wait_begin(void) { return 0; }
void runtime_wait_end(const char *what,uint64_t start,const char *detail,long long arg) {
    (void)what; (void)start; (void)detail; (void)arg;
}
uintptr_t runtime_lookup(const RuntimeExport *table,size_t count,const char *name) {
    for (size_t i=0;i<count;++i)
        if (!strcmp(table[i].name,name)) return (uintptr_t)table[i].function;
    return 0;
}
#ifdef TEMPEST_FEX
static _Thread_local int pending_exception;
static void (*exception_action)(void);
int runtime_fex_exception_pending(void) { return pending_exception; }
void runtime_exception_poll(void) {
    assert(pending_exception && exception_action);
    pending_exception=0;
    exception_action();
}
#endif
static uint64_t monotonic_ns(void) {
    struct timespec now; assert(clock_gettime(CLOCK_MONOTONIC,&now)==0);
    return (uint64_t)now.tv_sec*1000000000+(uint64_t)now.tv_nsec;
}
static GuestTimespec deadline_ms(unsigned ms) {
    struct timespec now; assert(clock_gettime(CLOCK_REALTIME,&now)==0);
    int64_t ns=now.tv_nsec+(int64_t)ms*1000000;
    return (GuestTimespec){now.tv_sec+ns/1000000000,ns%1000000000};
}
typedef struct { GuestMutex **mutex; int unlock,trylock; } Intruder;
static void *try_foreign(void *argument) {
    Intruder *other=argument;
    other->unlock=posix_mutex_unlock(other->mutex);
    other->trylock=posix_mutex_trylock(other->mutex);
    return NULL;
}
static void test_mutex_ownership(void) {
    struct { uint64_t before; GuestMutex *mutex; uint64_t after; }
        box={UINT64_C(0x0123456789abcdef),NULL,UINT64_C(0xfedcba9876543210)};
    GuestAttr *attr=NULL;
    assert(posix_attr_init(&attr)==0);
    assert(posix_attr_type(&attr,0)==22 && posix_attr_type(&attr,5)==22);
    assert(posix_attr_protocol(&attr,-1)==22 && posix_attr_protocol(&attr,3)==22);
    assert(posix_attr_type(&attr,1)==0 && posix_attr_protocol(&attr,0)==0);
    assert(posix_mutex_init(&box.mutex,&attr)==0);
    assert(box.before==UINT64_C(0x0123456789abcdef) && box.after==UINT64_C(0xfedcba9876543210));
    assert(posix_mutex_lock(&box.mutex)==0);
    assert(posix_mutex_lock(&box.mutex)==11); /* guest EDEADLK, not host EDEADLK or SCE */
    assert(posix_mutex_trylock(&box.mutex)==16 && posix_mutex_destroy(&box.mutex)==16);
    Intruder other={.mutex=&box.mutex}; pthread_t thread;
    assert(pthread_create(&thread,NULL,try_foreign,&other)==0);
    assert(pthread_join(thread,NULL)==0 && other.unlock==1 && other.trylock==16);
    assert(posix_mutex_unlock(&box.mutex)==0 && posix_mutex_unlock(&box.mutex)==1);
    assert(posix_mutex_destroy(&box.mutex)==0);
    assert(posix_mutex_destroy(&box.mutex)==22 && posix_mutex_trylock(&box.mutex)==22);

    assert(posix_attr_type(&attr,2)==0);
    assert(posix_mutex_init(&box.mutex,&attr)==0);
    assert(posix_mutex_lock(&box.mutex)==0 && posix_mutex_lock(&box.mutex)==0);
    assert(posix_mutex_unlock(&box.mutex)==0 && posix_mutex_destroy(&box.mutex)==16);
    assert(posix_mutex_unlock(&box.mutex)==0 && posix_mutex_destroy(&box.mutex)==0);
    assert(posix_attr_type(&attr,3)==0 && posix_attr_protocol(&attr,1)==0);
    assert(posix_mutex_init(&box.mutex,&attr)==0); /* real host priority-inheritance attribute */
    assert(posix_mutex_lock(&box.mutex)==0 && posix_mutex_unlock(&box.mutex)==0);
    assert(posix_mutex_destroy(&box.mutex)==0 && posix_attr_destroy(&attr)==0);
    assert(posix_attr_destroy(&attr)==22 && posix_attr_init(NULL)==22);
}
typedef struct {
    GuestMutex *mutex;
    GuestCond *cond;
    atomic_uint entered;
    int release;
    unsigned awoken;
} WakeState;
static void *wait_for_signal(void *argument) {
    WakeState *s=argument;
    assert(posix_mutex_lock(&s->mutex)==0);
    atomic_fetch_add(&s->entered,1);
    while (!s->release) assert(posix_cond_wait(&s->cond,&s->mutex)==0);
    ++s->awoken;
    assert(posix_mutex_unlock(&s->mutex)==0);
    return NULL;
}
static void test_condition_wakeup(unsigned count) {
    WakeState s={0}; void *attr=NULL; pthread_t threads[2];
    assert(posix_condattr_init(&attr)==0 && posix_cond_init(&s.cond,&attr)==0);
    assert(posix_condattr_destroy(&attr)==0 && posix_condattr_destroy(&attr)==22);
    assert(posix_mutex_init(&s.mutex,NULL)==0);
    for (unsigned i=0;i<count;++i) assert(pthread_create(&threads[i],NULL,wait_for_signal,&s)==0);
    uint64_t limit=monotonic_ns()+UINT64_C(2000000000);
    while (atomic_load(&s.entered)!=count && monotonic_ns()<limit) {
        struct timespec pause={0,1000000}; nanosleep(&pause,NULL);
    }
    assert(atomic_load(&s.entered)==count);
    /* Acquiring this lock proves the last worker has released it inside cond_wait. */
    assert(posix_mutex_lock(&s.mutex)==0);
    assert(posix_cond_destroy(&s.cond)==16 && posix_mutex_destroy(&s.mutex)==16);
    s.release=1;
    assert((count==1 ? posix_cond_signal(&s.cond) : posix_cond_broadcast(&s.cond))==0);
    assert(posix_mutex_unlock(&s.mutex)==0);
    for (unsigned i=0;i<count;++i) assert(pthread_join(threads[i],NULL)==0);
    assert(s.awoken==count);
    assert(posix_cond_destroy(&s.cond)==0 && posix_mutex_destroy(&s.mutex)==0);
    assert(posix_cond_destroy(&s.cond)==22 && posix_cond_signal(&s.cond)==22);
}
static void test_absolute_deadline(void) {
    GuestMutex *mutex=NULL; GuestCond *cond=NULL;
    assert(posix_mutex_init(&mutex,NULL)==0);
    assert(posix_cond_init(&cond,NULL)==0 && posix_mutex_lock(&mutex)==0);
    GuestTimespec until=deadline_ms(40);
    uint64_t before=monotonic_ns();
    assert(posix_cond_timedwait(&cond,&mutex,&until)==60);
    uint64_t elapsed=monotonic_ns()-before;
    assert(elapsed>=UINT64_C(1000000) && elapsed<UINT64_C(2000000000));
    assert(posix_mutex_trylock(&mutex)==16); /* timed wait returned with the lock reacquired */
    assert(posix_mutex_destroy(&mutex)==16);
    until=(GuestTimespec){0,0};
    assert(posix_cond_timedwait(&cond,&mutex,&until)==60); /* already expired */
    until=(GuestTimespec){-1,0}; assert(posix_cond_timedwait(&cond,&mutex,&until)==22);
    until=(GuestTimespec){0,-1}; assert(posix_cond_timedwait(&cond,&mutex,&until)==22);
    until=(GuestTimespec){0,1000000000}; assert(posix_cond_timedwait(&cond,&mutex,&until)==22);
    assert(posix_cond_timedwait(&cond,&mutex,NULL)==22);
    assert(posix_mutex_trylock(&mutex)==16 && posix_mutex_unlock(&mutex)==0);
    assert(posix_cond_wait(&cond,&mutex)==1); /* ERRORCHECK mutex is unowned */
    assert(posix_mutex_trylock(&mutex)==0 && posix_mutex_unlock(&mutex)==0);
    assert(posix_mutex_destroy(&mutex)==0 && posix_cond_destroy(&cond)==0);
    assert(posix_condattr_init(NULL)==22 && posix_cond_init(NULL,NULL)==22);
}
#ifdef TEMPEST_FEX
static void pause_ms(unsigned ms) {
    struct timespec pause={ms/1000,(long)(ms%1000)*1000000};
    while (nanosleep(&pause,&pause)) assert(errno==EINTR);
}
static int await_flag(atomic_int *flag,unsigned ms) {
    uint64_t limit=monotonic_ns()+(uint64_t)ms*1000000;
    while (!atomic_load(flag) && monotonic_ns()<limit) pause_ms(1);
    return atomic_load(flag);
}
typedef struct {
    GuestMutex *mutex;
    atomic_int callback_started,callback_release,callback_done,done;
    uint32_t timeout_us;
    int timed,result,pending_after;
    uint64_t callback_finished_ns,returned_ns;
} MutexCheckpoint;
static _Thread_local MutexCheckpoint *mutex_checkpoint;
static void mutex_checkpoint_handler(void) {
    MutexCheckpoint *s=mutex_checkpoint;
    assert(s && posix_mutex_trylock(&s->mutex)==16);
    assert(posix_mutex_destroy(&s->mutex)==16);
    atomic_store(&s->callback_started,1);
    assert(await_flag(&s->callback_release,2000));
    s->callback_finished_ns=monotonic_ns();
    atomic_store(&s->callback_done,1);
}
static void *lock_at_checkpoint(void *argument) {
    MutexCheckpoint *s=argument;
    mutex_checkpoint=s;
    pending_exception=1;
    s->result=s->timed ? posix_result(mutex_timedlock(&s->mutex,s->timeout_us))
                      : posix_mutex_lock(&s->mutex);
    s->pending_after=pending_exception;
    s->returned_ns=monotonic_ns();
    if (!s->result) assert(posix_mutex_unlock(&s->mutex)==0);
    atomic_store(&s->done,1);
    return NULL;
}
static void test_mutex_exception_checkpoint(int timed,int protocol) {
    MutexCheckpoint s={.timed=timed,.timeout_us=2000000};
    atomic_store(&s.callback_release,1);
    GuestAttr *attr=NULL;
    assert(posix_attr_init(&attr)==0 && posix_attr_protocol(&attr,protocol)==0);
    assert(posix_mutex_init(&s.mutex,&attr)==0 && posix_attr_destroy(&attr)==0);
    assert(posix_mutex_lock(&s.mutex)==0);
    exception_action=mutex_checkpoint_handler;
    pthread_t thread;
    assert(pthread_create(&thread,NULL,lock_at_checkpoint,&s)==0);
    int delivered_while_locked=await_flag(&s.callback_done,500);
    int blocked_after_callback=!atomic_load(&s.done);
    /* Always release and finish the worker before asserting delivery: an old
     * blocking host lock fails this regression without leaving the test hung. */
    assert(posix_mutex_unlock(&s.mutex)==0);
    int finished=await_flag(&s.done,2000);
    assert(finished && pthread_join(thread,NULL)==0);
    assert(posix_mutex_destroy(&s.mutex)==0);
    assert(delivered_while_locked && blocked_after_callback);
    assert(s.result==0 && !s.pending_after);
}
static void test_mutex_exception_deadline(int protocol) {
    MutexCheckpoint s={.timed=1,.timeout_us=200000};
    GuestAttr *attr=NULL;
    assert(posix_attr_init(&attr)==0 && posix_attr_protocol(&attr,protocol)==0);
    assert(posix_mutex_init(&s.mutex,&attr)==0 && posix_attr_destroy(&attr)==0);
    assert(posix_mutex_lock(&s.mutex)==0);
    exception_action=mutex_checkpoint_handler;
    pthread_t thread;
    assert(pthread_create(&thread,NULL,lock_at_checkpoint,&s)==0);
    int delivered_while_locked=await_flag(&s.callback_started,500);
    /* Hold the callback beyond the lock's original 200 ms deadline. Restarting
     * that deadline after delivery would wait another 200 ms instead of return. */
    if (delivered_while_locked) pause_ms(300);
    atomic_store(&s.callback_release,1);
    int finished_before_release=await_flag(&s.done,100);
    assert(posix_mutex_unlock(&s.mutex)==0);
    int finished=await_flag(&s.done,2000);
    assert(finished && pthread_join(thread,NULL)==0);
    assert(posix_mutex_destroy(&s.mutex)==0);
    assert(delivered_while_locked && atomic_load(&s.callback_done));
    assert(finished_before_release && s.result==60 && !s.pending_after);
    assert(s.returned_ns>=s.callback_finished_ns &&
           s.returned_ns-s.callback_finished_ns<UINT64_C(150000000));
}
static GuestMutex *checkpoint_mutex;
static GuestCond *checkpoint_cond;
static unsigned checkpoint_calls;
static void *checkpoint_probe(void *argument) {
    (void)argument;
    // A different host thread must acquire it: recursive trylock on the handler's
    // own thread would falsely pass if one recursive level were still locked.
    assert(posix_mutex_trylock(&checkpoint_mutex)==0);
    assert(posix_mutex_unlock(&checkpoint_mutex)==0);
    return NULL;
}
static void checkpoint_handler(void) {
    assert(posix_mutex_destroy(&checkpoint_mutex)==16);
    assert(posix_cond_destroy(&checkpoint_cond)==16);
    pthread_t thread;
    assert(pthread_create(&thread,NULL,checkpoint_probe,NULL)==0);
    assert(pthread_join(thread,NULL)==0);
    ++checkpoint_calls;
}
static void test_exception_checkpoint(void) {
    GuestAttr *attr=NULL;
    assert(posix_attr_init(&attr)==0 && posix_attr_type(&attr,2)==0);
    assert(posix_mutex_init(&checkpoint_mutex,&attr)==0);
    assert(posix_attr_destroy(&attr)==0 && posix_cond_init(&checkpoint_cond,NULL)==0);
    assert(posix_mutex_lock(&checkpoint_mutex)==0 && posix_mutex_lock(&checkpoint_mutex)==0);
    exception_action=checkpoint_handler; pending_exception=1;
    assert(posix_cond_wait(&checkpoint_cond,&checkpoint_mutex)==0);
    assert(checkpoint_calls==1 && !pending_exception);
    Intruder other={.mutex=&checkpoint_mutex}; pthread_t thread;
    assert(pthread_create(&thread,NULL,try_foreign,&other)==0 && pthread_join(thread,NULL)==0);
    assert(other.unlock==1 && other.trylock==16); /* original ownership restored */
    assert(posix_mutex_unlock(&checkpoint_mutex)==0 && posix_mutex_destroy(&checkpoint_mutex)==16);
    assert(posix_mutex_unlock(&checkpoint_mutex)==0 && posix_mutex_destroy(&checkpoint_mutex)==0);
    assert(posix_cond_destroy(&checkpoint_cond)==0);
}
#endif
int main(void) {
    alarm(10);
    test_mutex_ownership();
    test_condition_wakeup(1);
    test_condition_wakeup(2);
    test_absolute_deadline();
#ifdef TEMPEST_FEX
    test_exception_checkpoint();
    for (int protocol=0;protocol<=1;++protocol) {
        test_mutex_exception_checkpoint(0,protocol);
        test_mutex_exception_checkpoint(1,protocol);
        test_mutex_exception_deadline(protocol);
    }
#endif
    alarm(0);
    puts("POSIX mutex ownership, recursive locks, signal/broadcast and absolute condition deadlines passed");
#ifdef TEMPEST_FEX
    puts("FEX: blocked mutex callbacks, retained handles and original timedlock deadlines passed");
#endif
    return 0;
}
