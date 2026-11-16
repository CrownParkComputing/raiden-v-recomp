/* Unity exception registration and real target-thread signal/context delivery. */
#undef NDEBUG
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
static atomic_int ready,delivered;
static pid_t expected_tid,callback_tid;
static volatile sig_atomic_t recovered_fault;
static void ABI callback(int32_t signal,void *argument) {
    RuntimeExceptionContext *context=argument;
    if (signal==11) {
        /* Skip the actual three-byte faulting load and change its result. */
        const unsigned char *instruction=(const unsigned char *)(uintptr_t)context->machine.rip;
        assert(instruction[0]==0x48 && instruction[1]==0x8b && instruction[2]==0x00);
        context->machine.rip+=3; context->machine.rax=0x12345678;
        context->machine.fsbase=0; context->machine.gsbase=0;
        context->sigmask[0]=UINT32_MAX; /* must not replace the host mask */
        recovered_fault=1;
        return;
    }
    assert(signal==30 && context->machine.rip && context->machine.rsp);
    assert(context->machine.length==sizeof(RuntimeExceptionMcontext));
    callback_tid=(pid_t)syscall(SYS_gettid);
    *runtime_errno()=999; errno=ERANGE;
    atomic_store_explicit(&delivered,1,memory_order_release);
}
static void fault_chain(int signal,siginfo_t *info,void *context) {
    assert(runtime_exception_dispatch(signal,info,context)==1);
}
static __attribute__((noinline)) uint64_t cause_fault(void) {
    uint64_t result;
    __asm__ volatile("xor %%eax,%%eax\n\tmov (%%rax),%%rax" : "=a"(result) : : "memory");
    return result;
}
static void *ABI target(void *argument) {
    (void)argument;
    expected_tid=(pid_t)syscall(SYS_gettid);
    *runtime_errno()=123; errno=E2BIG;
    atomic_store_explicit(&ready,1,memory_order_release);
    while (!atomic_load_explicit(&delivered,memory_order_acquire)) __asm__ volatile("pause");
    assert(*runtime_errno()==123 && errno==E2BIG);
    return (void *)0x1234;
}
static void wait_for(atomic_int *value) {
    for (int i=0;i<2000 && !atomic_load_explicit(value,memory_order_acquire);++i) {
        struct timespec delay={0,1000000}; nanosleep(&delay,NULL);
    }
    assert(atomic_load_explicit(value,memory_order_acquire));
}
static void *host_worker(void *argument) {
    GuestThread **record=argument;
    *record=runtime_thread_current();
    return NULL;
}

int main(void) {
    /* Populate the host snapshot independently and check the guest ABI fields. */
    ucontext_t host={0}; struct _libc_fpstate fp={0};
    host.uc_mcontext.fpregs=&fp;
    for (int i=0;i<NGREG;++i) host.uc_mcontext.gregs[i]=i+101;
    host.uc_mcontext.gregs[REG_CSGSFS]=(greg_t)UINT64_C(0x002b000000000033);
    sigemptyset(&host.uc_sigmask); sigaddset(&host.uc_sigmask,SIGUSR1); sigaddset(&host.uc_sigmask,SIGBUS);
    host.uc_stack.ss_sp=(void *)0x10000; host.uc_stack.ss_size=65536;
    siginfo_t info={0}; info.si_addr=(void *)0x23456;
    RuntimeExceptionContext guest;
    snapshot_context(&guest,SIGSEGV,&info,&host);
#define CHECK(field,reg) assert(guest.machine.field==(uint64_t)host.uc_mcontext.gregs[reg])
    CHECK(r8,REG_R8); CHECK(r9,REG_R9); CHECK(r10,REG_R10); CHECK(r11,REG_R11);
    CHECK(r12,REG_R12); CHECK(r13,REG_R13); CHECK(r14,REG_R14); CHECK(r15,REG_R15);
    CHECK(rdi,REG_RDI); CHECK(rsi,REG_RSI); CHECK(rdx,REG_RDX); CHECK(rcx,REG_RCX);
    CHECK(rax,REG_RAX); CHECK(rbx,REG_RBX); CHECK(rbp,REG_RBP); CHECK(rsp,REG_RSP);
    CHECK(rip,REG_RIP); CHECK(rflags,REG_EFL); CHECK(error,REG_ERR);
#undef CHECK
    assert(guest.machine.address==0x23456 && guest.machine.cs==0x33 && guest.machine.ss==0x2b);
    assert(guest.sigmask[0]&(UINT32_C(1)<<29));
    assert(guest.sigmask[0]&(UINT32_C(1)<<9));
    assert(guest.stack.address==0x10000 && guest.stack.size==65536);
    assert(guest.machine.fpformat==0x10002 && !memcmp(guest.machine.fpstate,&fp,sizeof(fp)));
    /* Copyback changes guest registers/FP state without clobbering host TLS or
     * the signal frame's architecture metadata. */
    guest.machine.rax=999; guest.machine.rip=1234; guest.machine.rsp=5678;
    guest.machine.fs=0xffff; guest.machine.gs=0xffff; guest.machine.cs=0xffff;
    guest.machine.fsbase=0; guest.machine.gsbase=0;
    guest.machine.rflags=UINT64_MAX;
    ((unsigned char *)guest.machine.fpstate)[160]=0x5a;
    restore_context(&host,&guest);
    assert(host.uc_mcontext.gregs[REG_RAX]==999 && host.uc_mcontext.gregs[REG_RIP]==1234 && host.uc_mcontext.gregs[REG_RSP]==5678);
    assert((uint64_t)host.uc_mcontext.gregs[REG_CSGSFS]==UINT64_C(0x002b000000000033));
    assert((((uint64_t)host.uc_mcontext.gregs[REG_EFL])&~UINT64_C(0x50dd5))==(((uint64_t)(REG_EFL+101))&~UINT64_C(0x50dd5)));
    assert(((unsigned char *)&fp)[160]==0x5a && sigismember(&host.uc_sigmask,SIGBUS)==1);

    struct sigaction before,after;
    assert(sigaction(SIGSEGV,NULL,&before)==0);
    assert(install_exception(11,callback)==0);
    assert(sigaction(SIGSEGV,NULL,&after)==0 && before.sa_sigaction==after.sa_sigaction && before.sa_flags==after.sa_flags);
    assert(remove_exception(11)==0);
    struct sigaction chain={0}; chain.sa_flags=SA_SIGINFO; chain.sa_sigaction=fault_chain;
    sigemptyset(&chain.sa_mask);
    assert(sigaction(SIGSEGV,&chain,NULL)==0 && install_exception(11,callback)==0);
    sigset_t mask_before,mask_after;
    assert(pthread_sigmask(SIG_SETMASK,NULL,&mask_before)==0);
    assert(cause_fault()==0x12345678 && recovered_fault);
    assert(pthread_sigmask(SIG_SETMASK,NULL,&mask_after)==0);
    assert(sigismember(&mask_before,SIGUSR1)==sigismember(&mask_after,SIGUSR1));
    assert(remove_exception(11)==0 && sigaction(SIGSEGV,&before,NULL)==0);
    assert(install_exception(2,callback)==EXERR(22));
    assert(install_exception(30,callback)==0 && install_exception(30,callback)==EXERR(35));
    assert(raise_exception(NULL,11)==EXERR(22));
    assert(raise_exception(NULL,30)==EXERR(3));
    GuestThread *thread=NULL; void *result=NULL;
    assert(thread_create(&thread,NULL,target,NULL,"exception-test")==0);
    wait_for(&ready);
    assert(raise_exception(thread,30)==0);
    wait_for(&delivered);
    assert(thread_join(thread,&result)==0 && result==(void *)0x1234);
    assert(callback_tid==expected_tid && callback_tid!=(pid_t)syscall(SYS_gettid));
    assert(raise_exception(thread,30)==EXERR(3));
    /* Externally-created HLE workers also leave retained handles. Their host
     * TLS teardown must invalidate the handle before pthread_t can be reused. */
    pthread_t worker; GuestThread *host_record=NULL;
    assert(pthread_create(&worker,NULL,host_worker,&host_record)==0);
    assert(pthread_join(worker,NULL)==0 && host_record && host_record->finished);
    assert(raise_exception(host_record,30)==EXERR(3));
    assert(remove_exception(30)==0 && remove_exception(30)==0 && remove_exception(31)==EXERR(22));
    puts("Unity exception ABI, registration and target-thread delivery passed");
    return 0;
}
