/* Android inherited masks must not park Mono's target-thread suspend signal. */
#undef NDEBUG
#if !defined(__ANDROID__) || !defined(TEMPEST_FEX)
#error Build this regression with the Android NDK and TEMPEST_FEX.
#endif
#include "../src/runtime_thread.c"
#include "../src/runtime_exception.c"
#include <assert.h>
#include <stdatomic.h>

void *runtime_low_alloc(size_t size) { return calloc(1,size); }
void runtime_low_free(void *pointer) { free(pointer); }
void *runtime_low_map(size_t size,int prot) { (void)size; (void)prot; return NULL; }
void runtime_thread_keys_cleanup(void) {}
int runtime_verbose(void) { return 0; }
int32_t runtime_guest_errno(int error) {
    switch (error) { case EINVAL:return 22; case EAGAIN:return 35; case ESRCH:return 3; default:assert(!"unexpected errno");return 5; }
}
uintptr_t runtime_lookup(const RuntimeExport *table,size_t count,const char *name) {
    (void)table; (void)count; (void)name; return 0;
}
typedef struct {
    atomic_int ready,delivered;
    GuestThread *record;
    pid_t target_tid,callback_tid;
    int signal,has_context,has_handler;
} Worker;
static _Thread_local Worker *active_worker;
static void ABI unused_guest_handler(int32_t signal,void *context) { (void)signal; (void)context; }
int runtime_fex_exception_deliver(int signal,uintptr_t handler,const void *info,void *context) {
    (void)info;
    Worker *worker=active_worker;
    if (!worker) return 0;
    worker->callback_tid=(pid_t)syscall(SYS_gettid);
    worker->signal=signal; worker->has_context=context!=NULL;
    worker->has_handler=handler==(uintptr_t)unused_guest_handler;
    *runtime_errno()=999; errno=ERANGE;
    atomic_store_explicit(&worker->delivered,1,memory_order_release);
    return 1;
}
void runtime_fex_exception_poll(void) {}
int runtime_fex_exception_pending(void) { return 0; }
uintptr_t runtime_fex_call(uintptr_t entry,const uintptr_t *arguments,size_t count) {
    assert(count==1);
    return (uintptr_t)((GuestEntry)entry)((void *)arguments[0]);
}
static void wait_for(atomic_int *value) {
    for (int i=0;i<2000 && !atomic_load_explicit(value,memory_order_acquire);++i) {
        struct timespec delay={0,1000000}; nanosleep(&delay,NULL);
    }
    assert(atomic_load_explicit(value,memory_order_acquire));
}
static void check_mask(void) {
    sigset_t mask;
    assert(pthread_sigmask(SIG_SETMASK,NULL,&mask)==0);
    assert(sigismember(&mask,SIGUSR1)==1);
    assert(sigismember(&mask,SIGRTMAX)==0);
}
static void *ABI guest_worker(void *argument) {
    Worker *worker=argument;
    active_worker=worker; worker->record=runtime_thread_current();
    worker->target_tid=(pid_t)syscall(SYS_gettid);
    check_mask();
    *runtime_errno()=123; errno=E2BIG;
    atomic_store_explicit(&worker->ready,1,memory_order_release);
    while (!atomic_load_explicit(&worker->delivered,memory_order_acquire)) atomic_signal_fence(memory_order_seq_cst);
    assert(*runtime_errno()==123 && errno==E2BIG);
    return worker;
}
static void *host_worker(void *argument) { return guest_worker(argument); }
static void external_handler(int signal) { (void)signal; }
static void check_delivery(Worker *worker) {
    assert(raise_exception(worker->record,30)==0);
    wait_for(&worker->delivered);
    assert(worker->callback_tid==worker->target_tid);
    assert(worker->signal==30 && worker->has_context && worker->has_handler);
}
int main(void) {
    sigset_t original_mask,blocked;
    struct sigaction original_usr1,original_rt,external={0},after;
    assert(SIGRTMAX==64 && native_signal(30)==64 && guest_signal(64)==30 && guest_signal(SIGUSR1)==0);
    assert(sigaction(SIGUSR1,NULL,&original_usr1)==0 && sigaction(SIGRTMAX,NULL,&original_rt)==0);
    external.sa_handler=external_handler; sigemptyset(&external.sa_mask);
    assert(sigaction(SIGUSR1,&external,NULL)==0);
    sigemptyset(&blocked); sigaddset(&blocked,SIGUSR1); sigaddset(&blocked,SIGRTMAX);
    assert(pthread_sigmask(SIG_BLOCK,&blocked,&original_mask)==0);
    Worker main_worker={0}; active_worker=&main_worker;
    main_worker.record=runtime_thread_current(); main_worker.target_tid=(pid_t)syscall(SYS_gettid);
    check_mask();

    /* Both kinds of guest thread start with 64 blocked, before registration. */
    assert(pthread_sigmask(SIG_BLOCK,&blocked,NULL)==0);
    Worker before={0},host_before={0}; GuestThread *thread_before; pthread_t host_thread;
    assert(thread_create(&thread_before,NULL,guest_worker,&before,"before-install")==0);
    assert(pthread_create(&host_thread,NULL,host_worker,&host_before)==0);
    wait_for(&before.ready); wait_for(&host_before.ready);
    assert(runtime_exception_prepare_thread()==0);
    assert(install_exception(30,unused_guest_handler)==0);
    check_delivery(&before); check_delivery(&host_before);
    void *result;
    assert(thread_join(thread_before,&result)==0 && result==&before);
    assert(pthread_join(host_thread,&result)==0 && result==&host_before);

    /* A later guest thread also repairs its inherited mask at attach(). */
    assert(pthread_sigmask(SIG_BLOCK,&blocked,NULL)==0);
    Worker after_install={0}; GuestThread *thread_after;
    assert(thread_create(&thread_after,NULL,guest_worker,&after_install,"after-install")==0);
    wait_for(&after_install.ready);
    assert(runtime_exception_prepare_thread()==0);
    check_delivery(&after_install);
    assert(thread_join(thread_after,&result)==0 && result==&after_install);
    *runtime_errno()=123; errno=E2BIG;
    check_delivery(&main_worker);
    assert(*runtime_errno()==123 && errno==E2BIG);
    check_mask();
    assert(sigaction(SIGUSR1,NULL,&after)==0 && after.sa_handler==external_handler);
    assert(remove_exception(30)==0);

    /* A future host owner is an explicit error, never silently replaced. */
    assert(sigaction(SIGRTMAX,&external,NULL)==0);
    assert(install_exception(30,unused_guest_handler)==EXERR(16));
    assert(sigaction(SIGRTMAX,NULL,&after)==0 && after.sa_handler==external_handler);
    assert(sigaction(SIGRTMAX,&original_rt,NULL)==0 && sigaction(SIGUSR1,&original_usr1,NULL)==0);
    assert(pthread_sigmask(SIG_SETMASK,&original_mask,NULL)==0);
    puts("Android suspend signal: main/pre-install/later guest delivery, inherited masks, errno and host ownership passed");
    return 0;
}
