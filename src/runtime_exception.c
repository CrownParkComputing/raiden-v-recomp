/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Unity's kernel exception API. Synchronous faults remain behind GPU/ISA fault
 * recovery; HUP, FPE and the guest suspend signal use a host trampoline. */
#define _GNU_SOURCE
#include "runtime_exception.h"
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#if defined(__linux__) && defined(__x86_64__) && !defined(TEMPEST_FEX)
#include <ucontext.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <asm/prctl.h>
#endif
#define EXERR(n) ((int32_t)(UINT32_C(0x80020000)|(n)))
static RuntimeExceptionHandler exception_handlers[32];
static pthread_mutex_t exception_lock=PTHREAD_MUTEX_INITIALIZER;
#ifndef _WIN32
static struct sigaction previous_actions[32];
static unsigned char owns_action[32];
static int suspend_signal(void) {
#if defined(__ANDROID__) && defined(TEMPEST_FEX)
    /* ART owns SIGUSR1 and zygote-created threads inherit it blocked. Keep its
     * action/mask intact. FEX's full Linux frontend reserves 63 for pause;
     * use 64, outside Bionic/ART's low real-time signals, for guest suspend. */
    return SIGRTMAX;
#else
    return SIGUSR1;
#endif
}
static int native_signal(int signal) {
    switch (signal) {
    case 1:return SIGHUP; case 4:return SIGILL; case 8:return SIGFPE;
    case 10:return SIGBUS; case 11:return SIGSEGV; case 30:return suspend_signal();
    default:return 0;
    }
}
static int guest_signal(int signal) {
    if (signal==SIGHUP) return 1;
    if (signal==SIGILL) return 4;
    if (signal==SIGFPE) return 8;
    if (signal==SIGBUS) return 10;
    if (signal==SIGSEGV) return 11;
    if (signal==suspend_signal()) return 30;
    return 0;
}
#if defined(__linux__) && defined(__x86_64__) && !defined(TEMPEST_FEX)
static size_t saved_fp_bytes(const void *state) {
    const unsigned char *fp=state;
    uint32_t magic,extended,xstate;
    memcpy(&magic,fp+464,4); memcpy(&extended,fp+468,4); memcpy(&xstate,fp+480,4);
    return magic==UINT32_C(0x46505853) && extended>=832 && xstate>=832 ? 832 : 512;
}
static void snapshot_context(RuntimeExceptionContext *out,int signal,const siginfo_t *info,const ucontext_t *host) {
    memset(out,0,sizeof(*out));
    RuntimeExceptionMcontext *m=&out->machine;
    const greg_t *g=host->uc_mcontext.gregs;
#define REG(field,index) m->field=(uint64_t)g[index]
    REG(r8,REG_R8); REG(r9,REG_R9); REG(r10,REG_R10); REG(r11,REG_R11);
    REG(r12,REG_R12); REG(r13,REG_R13); REG(r14,REG_R14); REG(r15,REG_R15);
    REG(rdi,REG_RDI); REG(rsi,REG_RSI); REG(rbp,REG_RBP); REG(rbx,REG_RBX);
    REG(rdx,REG_RDX); REG(rax,REG_RAX); REG(rcx,REG_RCX); REG(rsp,REG_RSP);
    REG(rip,REG_RIP); REG(rflags,REG_EFL); REG(error,REG_ERR);
#undef REG
    m->trapno=(int32_t)g[REG_TRAPNO];
    m->cs=(uint64_t)g[REG_CSGSFS]&0xffffu;
    m->gs=(uint16_t)((uint64_t)g[REG_CSGSFS]>>16);
    m->fs=(uint16_t)((uint64_t)g[REG_CSGSFS]>>32);
    m->ss=(uint64_t)g[REG_CSGSFS]>>48;
    m->flags=1; /* _MC_HASSEGS */
    if (!syscall(SYS_arch_prctl,ARCH_GET_FS,&m->fsbase) &&
        !syscall(SYS_arch_prctl,ARCH_GET_GS,&m->gsbase)) m->flags|=2; /* _MC_HASBASES */
    if (info && signal!=SIGUSR1 && signal!=SIGHUP) m->address=(uint64_t)(uintptr_t)info->si_addr;
    m->length=sizeof(*m);
    m->onstack=(host->uc_stack.ss_flags&SS_ONSTACK)!=0;
    out->stack.address=(uint64_t)(uintptr_t)host->uc_stack.ss_sp;
    out->stack.size=host->uc_stack.ss_size; out->stack.flags=host->uc_stack.ss_flags;
    for (int guest=1;guest<32;++guest) {
        int native=native_signal(guest);
        if (native && sigismember(&host->uc_sigmask,native)==1)
            out->sigmask[(guest-1)/32]|=UINT32_C(1)<<((guest-1)%32);
    }
    if (host->uc_mcontext.fpregs) {
        const unsigned char *fp=(const unsigned char *)host->uc_mcontext.fpregs;
        memcpy(m->fpstate,fp,saved_fp_bytes(fp));
        m->fpformat=0x10002; m->ownedfp=0x20001; /* FreeBSD XMM / FPU */
    }
}
/* rt_sigreturn consumes this frame after our callback returns. Apply the
 * editable register state while retaining the host's TLS selectors/bases,
 * signal mask, alternate stack and Linux XSAVE bookkeeping. */
static void restore_context(ucontext_t *host,const RuntimeExceptionContext *context) {
    const RuntimeExceptionMcontext *m=&context->machine;
    greg_t *g=host->uc_mcontext.gregs;
#define REG(field,index) g[index]=(greg_t)m->field
    REG(r8,REG_R8); REG(r9,REG_R9); REG(r10,REG_R10); REG(r11,REG_R11);
    REG(r12,REG_R12); REG(r13,REG_R13); REG(r14,REG_R14); REG(r15,REG_R15);
    REG(rdi,REG_RDI); REG(rsi,REG_RSI); REG(rbp,REG_RBP); REG(rbx,REG_RBX);
    REG(rdx,REG_RDX); REG(rax,REG_RAX); REG(rcx,REG_RCX); REG(rsp,REG_RSP);
    REG(rip,REG_RIP);
#undef REG
    const uint64_t user_flags=UINT64_C(0x50dd5); /* Linux FIX_EFLAGS */
    g[REG_EFL]=(greg_t)(((uint64_t)g[REG_EFL]&~user_flags)|(m->rflags&user_flags));
    if (host->uc_mcontext.fpregs && m->fpformat==0x10002 && m->ownedfp==0x20001) {
        unsigned char *fp=(unsigned char *)host->uc_mcontext.fpregs;
        size_t bytes=saved_fp_bytes(fp);
        uint32_t mxcsr_mask,mxcsr;
        memcpy(&mxcsr_mask,fp+28,4);
        if (!mxcsr_mask) mxcsr_mask=UINT32_C(0xffbf);
        /* Bytes 464..511 describe Linux's variable-sized signal frame. */
        memcpy(fp,m->fpstate,464);
        memcpy(&mxcsr,fp+24,4); mxcsr&=mxcsr_mask;
        memcpy(fp+24,&mxcsr,4); memcpy(fp+28,&mxcsr_mask,4);
        if (bytes>=832) {
            uint64_t host_features,guest_features;
            memcpy(&host_features,fp+512,8); memcpy(&guest_features,(const unsigned char *)m->fpstate+512,8);
            host_features=(host_features&~UINT64_C(7))|(guest_features&UINT64_C(7));
            memcpy(fp+512,&host_features,8);
            memcpy(fp+576,(const unsigned char *)m->fpstate+576,256); /* YMM high halves */
        }
    }
}
#endif
int runtime_exception_dispatch(int signal,const void *info,void *host_context) {
    int guest=guest_signal(signal);
    if (!guest || !host_context) return 0;
    RuntimeExceptionHandler handler=__atomic_load_n(&exception_handlers[guest],__ATOMIC_ACQUIRE);
    if (!handler) return 0;
    int saved_errno=errno; int32_t *guest_errno=runtime_errno(),saved_guest_errno=*guest_errno;
    int handled=0;
#ifdef TEMPEST_FEX
    handled=runtime_fex_exception_deliver(guest,(uintptr_t)handler,info,host_context);
#elif defined(__linux__) && defined(__x86_64__)
    RuntimeExceptionContext context;
    snapshot_context(&context,signal,info,host_context);
    handler(guest,&context);
    restore_context(host_context,&context);
    handled=1;
#else
    (void)info;
#endif
    *guest_errno=saved_guest_errno; errno=saved_errno;
    return handled;
}
static void exception_trampoline(int signal,siginfo_t *info,void *context) {
    int saved_errno=errno; int32_t *guest_errno=runtime_errno(),saved_guest_errno=*guest_errno;
    if (!runtime_exception_dispatch(signal,info,context)) {
        int guest=guest_signal(signal);
        struct sigaction old=previous_actions[guest];
        if (old.sa_handler==SIG_DFL) {
            sigaction(signal,&old,NULL);
            raise(signal);
        } else if (old.sa_handler!=SIG_IGN) {
            if (old.sa_flags&SA_SIGINFO) old.sa_sigaction(signal,info,context);
            else old.sa_handler(signal);
        }
    }
    *guest_errno=saved_guest_errno; errno=saved_errno;
}
#else
int runtime_exception_dispatch(int signal,const void *info,void *context) { (void)signal; (void)info; (void)context; return 0; }
#endif
int runtime_exception_prepare_thread(void) {
#if defined(__ANDROID__) && defined(TEMPEST_FEX)
    /* Called after guest TLS is attached, before the thread is published or
     * runs guest code. This also covers Fios threads created before Mono
     * installs its handler. Never change any of ART's reserved mask bits. */
    sigset_t set;
    sigemptyset(&set); sigaddset(&set,suspend_signal());
    return pthread_sigmask(SIG_UNBLOCK,&set,NULL);
#else
    return 0;
#endif
}
void runtime_exception_poll(void) {
#ifdef TEMPEST_FEX
    int saved_errno=errno; int32_t *guest_errno=runtime_errno(),saved_guest_errno=*guest_errno;
    runtime_fex_exception_poll();
    *guest_errno=saved_guest_errno; errno=saved_errno;
#endif
}
static ABI int32_t install_exception(int32_t signal,RuntimeExceptionHandler handler) {
#ifdef _WIN32
    (void)signal; (void)handler; return EXERR(78);
#else
    int native=native_signal(signal);
    if (!native) return EXERR(22);
#ifdef TEMPEST_FEX
    /* Cooperative delivery runs at an HLE boundary. A synchronous host fault
     * cannot resume unchanged and reach that boundary, so do not register it
     * as handled. Mono's asynchronous suspend request is guest SIGUSR1 (30). */
    if (signal!=30) return EXERR(45);
#endif
    pthread_mutex_lock(&exception_lock);
    int32_t result=0;
    if (__atomic_load_n(&exception_handlers[signal],__ATOMIC_ACQUIRE)) result=EXERR(35);
    else {
        /* SIGILL/BUS/SEGV belong to the loader/FEX fault chain, never replace it. */
        if (signal==1 || signal==8 || signal==30) {
            struct sigaction action={0},old;
            sigemptyset(&action.sa_mask); action.sa_sigaction=exception_trampoline;
            action.sa_flags=SA_SIGINFO;
#ifndef TEMPEST_FEX
            action.sa_flags|=SA_RESTART;
#endif
            if (sigaction(native,NULL,&old)) result=EXERR(runtime_guest_errno(errno));
#if defined(__ANDROID__) && defined(TEMPEST_FEX)
            /* Do not replace a runtime/vendor handler if this signal acquires
             * another owner. Guest suspend cannot work safely in that case. */
            else if (signal==30 && old.sa_handler!=SIG_DFL) result=EXERR(16);
#endif
            else if (sigaction(native,&action,&old)) result=EXERR(runtime_guest_errno(errno));
            else if (!owns_action[signal]) { previous_actions[signal]=old; owns_action[signal]=1; }
        }
        if (!result) __atomic_store_n(&exception_handlers[signal],handler,__ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&exception_lock);
    return result;
#endif
}
static ABI int32_t remove_exception(int32_t signal) {
#ifdef _WIN32
    (void)signal; return EXERR(78);
#else
    int native=native_signal(signal);
    if (!native) return EXERR(22);
    pthread_mutex_lock(&exception_lock);
    int32_t result=0;
    if (owns_action[signal] && sigaction(native,&previous_actions[signal],NULL))
        result=EXERR(runtime_guest_errno(errno));
    if (!result) {
        __atomic_store_n(&exception_handlers[signal],(RuntimeExceptionHandler)0,__ATOMIC_RELEASE);
        owns_action[signal]=0;
    }
    pthread_mutex_unlock(&exception_lock);
    return result;
#endif
}
static ABI int32_t raise_exception(void *thread,int32_t signal) {
    if (signal!=30) return EXERR(22);
#ifdef _WIN32
    (void)thread; return EXERR(78);
#else
    int error=runtime_thread_raise_signal(thread,native_signal(signal));
    return error ? EXERR(runtime_guest_errno(error)) : 0;
#endif
}
static const RuntimeExport exception_exports[]={
    {"sceKernelInstallExceptionHandler",install_exception},
    {"sceKernelRemoveExceptionHandler",remove_exception},
    {"sceKernelRaiseException",raise_exception},
};
uintptr_t runtime_exception_resolve(const char *name) { return RUNTIME_LOOKUP(exception_exports,name); }
