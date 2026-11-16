/* SPDX-License-Identifier: GPL-2.0-or-later */
/* PS4 exception context ABI, matching shadPS4 kernel/threads/exception.h. */
#ifndef TEMPEST_RUNTIME_EXCEPTION_H
#define TEMPEST_RUNTIME_EXCEPTION_H
#include "runtime.h"
typedef void (ABI *RuntimeExceptionHandler)(int32_t,void *);
typedef struct {
    uint64_t onstack,rdi,rsi,rdx,rcx,r8,r9,rax,rbx,rbp,r10,r11,r12,r13,r14,r15;
    int32_t trapno; uint16_t fs,gs;
    uint64_t address;
    int32_t flags; uint16_t es,ds;
    uint64_t error,rip,cs,rflags,rsp,ss,length,fpformat,ownedfp,lbrfrom,lbrto,aux1,aux2;
    uint64_t fpstate[104],fsbase,gsbase,spare[6];
} RuntimeExceptionMcontext;
typedef struct {
    uint32_t sigmask[4];
    int32_t reserved0[12];
    RuntimeExceptionMcontext machine;
    uint64_t link;
    struct { uint64_t address,size; int32_t flags,padding; } stack;
    int32_t flags,spare[4],reserved1[3];
} RuntimeExceptionContext;
_Static_assert(sizeof(RuntimeExceptionMcontext)==0x480,"PS4 mcontext size");
_Static_assert(sizeof(RuntimeExceptionContext)==0x500,"PS4 ucontext size");
_Static_assert(offsetof(RuntimeExceptionContext,machine.rsp)==0xf8,"PS4 saved RSP offset");
#endif
