/* Per-thread cleanup ordering, pop semantics, stack arguments and TLS-destructor order. */
#undef NDEBUG
#include "../src/runtime_thread.c"
#include <assert.h>

void *runtime_low_alloc(size_t size) { return calloc(1,size); }
void runtime_low_free(void *pointer) { free(pointer); }
void *runtime_low_map(size_t size,int prot) { (void)size; (void)prot; return NULL; }
int runtime_verbose(void) { return 0; }
typedef struct { int values[8]; unsigned count; } Log;
typedef struct { Log *log; int value; uint64_t magic; } Action;
static _Thread_local Log *owned_log;
static void ABI record(void *argument) {
    Action *action=argument;
    assert(action->magic==UINT64_C(0x123456789abcdef0));
    assert(action->log==owned_log && owned_log->count<8);
    owned_log->values[owned_log->count++]=action->value;
}
void runtime_thread_keys_cleanup(void) {
    if (owned_log) owned_log->values[owned_log->count++]=99;
}
static void ABI unlock_native(void *mutex) { assert(pthread_mutex_unlock(mutex)==0); }
typedef struct {
    Log log;
    Action first,second;
    pthread_barrier_t *barrier;
    int explicit_exit;
    int cancel_at_checkpoint;
} Worker;
static void *ABI worker(void *argument) {
    Worker *w=argument;
    owned_log=&w->log;
    /* Explicit exit must invoke these before its longjmp discards this frame. */
    Action first=w->first,second=w->second;
    thread_cleanup_push(record,w->explicit_exit || w->cancel_at_checkpoint ? &first : &w->first);
    thread_cleanup_push(record,w->explicit_exit || w->cancel_at_checkpoint ? &second : &w->second);
    int result=pthread_barrier_wait(w->barrier);
    assert(result==0 || result==PTHREAD_BARRIER_SERIAL_THREAD);
    if (w->cancel_at_checkpoint) {
        for (;;) { runtime_thread_testcancel(); sched_yield(); }
    }
    if (w->explicit_exit) thread_exit(w);
    return w;
}
#ifdef TEMPEST_FEX
int runtime_exception_prepare_thread(void) { return 0; }
/* Exercise the actual GUEST_CALL1 path without an x86 translator: the fixture's
 * sole entry returns a pointer, and its cleanup routines return void. */
uintptr_t runtime_fex_call(uintptr_t entry,const uintptr_t *arguments,size_t count) {
    assert(count==1);
    if (entry==(uintptr_t)worker) return (uintptr_t)worker((void *)arguments[0]);
    ((CleanupRoutine)entry)((void *)arguments[0]);
    return 0;
}
#endif

int main(void) {
    Log log={0}; owned_log=&log;
    Action actions[3]={{&log,1,UINT64_C(0x123456789abcdef0)},
                       {&log,2,UINT64_C(0x123456789abcdef0)},
                       {&log,3,UINT64_C(0x123456789abcdef0)}};
    thread_cleanup_push(record,&actions[0]);
    thread_cleanup_push(record,&actions[1]);
    thread_cleanup_pop(0); assert(log.count==0);
    thread_cleanup_push(record,&actions[2]);
    thread_cleanup_pop(1); thread_cleanup_pop(1); thread_cleanup_pop(1);
    assert(log.count==2 && log.values[0]==3 && log.values[1]==1);
    struct { uint64_t before; GuestCleanupBuffer buffer; uint64_t after; }
        slot={UINT64_C(0xabcdef1234567890),{0},UINT64_C(0x9876543210fedcba)};
    thread_cleanup_push_imp(record,&actions[1],&slot.buffer);
    assert(slot.buffer.routine==record && slot.buffer.argument==&actions[1] && !slot.buffer.onheap);
    thread_cleanup_pop(1);
    assert(log.count==3 && log.values[2]==2);
    assert(slot.before==UINT64_C(0xabcdef1234567890) && slot.after==UINT64_C(0x9876543210fedcba));
    pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
    assert(pthread_mutex_lock(&mutex)==0);
    thread_cleanup_push(unlock_native,&mutex); thread_cleanup_pop(1);
    assert(pthread_mutex_trylock(&mutex)==0 && pthread_mutex_unlock(&mutex)==0);
    assert(pthread_mutex_destroy(&mutex)==0);

    pthread_barrier_t barrier; assert(pthread_barrier_init(&barrier,NULL,2)==0);
    Worker workers[2]={0}; GuestThread *threads[2];
    for (int i=0;i<2;++i) {
        workers[i].first=(Action){&workers[i].log,10*i+1,UINT64_C(0x123456789abcdef0)};
        workers[i].second=(Action){&workers[i].log,10*i+2,UINT64_C(0x123456789abcdef0)};
        workers[i].barrier=&barrier; workers[i].explicit_exit=i==0;
        assert(thread_create(&threads[i],NULL,worker,&workers[i],"cleanup-test")==0);
    }
    for (int i=0;i<2;++i) {
        void *result=NULL;
        assert(thread_join(threads[i],&result)==0 && result==&workers[i]);
        Log *events=&workers[i].log;
        assert(events->count==3);
        assert(events->values[0]==10*i+2 && events->values[1]==10*i+1 && events->values[2]==99);
    }
    /* A live detached record remains cancellable; completed records never
     * reach a recycled host pthread_t. Cleanup precedes TLS on both paths. */
    for (int detached=0;detached<2;++detached) {
        Worker cancelled={.barrier=&barrier,.cancel_at_checkpoint=1};
        cancelled.first=(Action){&cancelled.log,21,UINT64_C(0x123456789abcdef0)};
        cancelled.second=(Action){&cancelled.log,22,UINT64_C(0x123456789abcdef0)};
        GuestThread *target=NULL;
        assert(thread_create(&target,NULL,worker,&cancelled,"cancel-test")==0);
        int result=pthread_barrier_wait(&barrier);
        assert(result==0 || result==PTHREAD_BARRIER_SERIAL_THREAD);
        if (detached) assert(thread_detach(target)==0);
        assert(thread_cancel(target)==0);
        if (!detached) {
            void *value=NULL;
            assert(thread_join(target,&value)==0 && value==(void *)(intptr_t)-1);
        } else {
            struct timespec pause={0,1000000}; unsigned tries=0;
            while (!__atomic_load_n(&target->finished,__ATOMIC_ACQUIRE) && ++tries<1000)
                nanosleep(&pause,NULL);
            assert(__atomic_load_n(&target->finished,__ATOMIC_ACQUIRE));
        }
        assert(cancelled.log.count==3 && cancelled.log.values[0]==22 &&
               cancelled.log.values[1]==21 && cancelled.log.values[2]==99);
        assert(thread_cancel(target)==ERR(3));
    }
    assert(thread_cancel(NULL)==ERR(3) && thread_cancel(runtime_thread_current())==ERR(3));
    assert(pthread_barrier_destroy(&barrier)==0);
    puts("Thread cleanup LIFO, pop, exit/return and TLS ordering passed");
    return 0;
}
