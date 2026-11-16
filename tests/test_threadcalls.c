/* Thread creation, scheduling and stack calls (src/runtime_thread.c) against the contracts
 * shadPS4 follows. */
#include "../src/runtime_thread.c"
#include <assert.h>

void *runtime_low_alloc(size_t size) { return calloc(1, size); }
void runtime_low_free(void *pointer) { free(pointer); }
/* Linux guest stacks: none low here, so pthreads places them. */
void *runtime_low_map(size_t size, int prot) { (void)size; (void)prot; return NULL; }
void runtime_thread_keys_cleanup(void) {}
int runtime_verbose(void) { return 0; }

static int32_t own_tid;
static void *ABI inspector(void *arg) {
    (void)arg;
    own_tid = thread_get_id();
    return NULL;
}
/* pthread_exit: the value reaches the joiner. */
static void *ABI leaver(void *arg) {
    thread_exit(arg);
}

int main(void) {
    /* A thread that has finished. */
    GuestThread *t = NULL;
    assert(thread_create(&t, NULL, inspector, NULL, "finished") == 0);
    assert(thread_join(t, NULL) == 0);
    assert(own_tid > 0 && own_tid != thread_get_id());

    /* Scheduling parameters of a live thread; a finished one is ESRCH. */
    GuestThread *self = runtime_thread_current();
    char guest_name[32],host_name[16];
    assert(thread_rename(self,"Mono main thread with full name")==0);
    assert(thread_get_name(self,guest_name)==0 && !strcmp(guest_name,"Mono main thread with full name"));
    assert(pthread_getname_np(pthread_self(),host_name,sizeof(host_name))==0 && !strcmp(host_name,"Mono main threa"));
    assert(thread_rename(self,NULL)==0 && !self->name[0]);
    assert(thread_rename(NULL,"invalid")==ERR(3) && thread_rename(t,"finished")==ERR(3));
    int param = 300, prio = 0;
    assert(thread_set_sched(self, 3, &param) == 0 && self->attr.policy == 3 && self->attr.prio == 300);
    int policy=0;
    assert(thread_get_sched(self,&policy,&prio)==0 && policy==3 && prio==300);
    assert(thread_get_sched(self,NULL,&prio)==ERR(22) && thread_get_sched(t,&policy,&prio)==ERR(3));
    param=500;
    assert(posix_thread_set_sched(self,1,&param)==0);
    assert(posix_thread_get_sched(self,&policy,&prio)==0 && policy==1 && prio==500);
    assert(posix_thread_get_sched(NULL,&policy,&prio)==3 && posix_thread_get_sched(self,&policy,NULL)==22);
    assert(thread_set_sched(self, 3, NULL) == ERR(22) && thread_set_sched(t, 1, &param) == ERR(3));
    assert(thread_set_prio(self, 700) == 0 && thread_get_prio(self, &prio) == 0 && prio == 700);
    assert(thread_get_prio(self, NULL) == ERR(22));

    /* pthread_exit's value, and a detached thread that cannot be joined. */
    void *value = NULL;
    assert(thread_create(&t, NULL, leaver, (void *)0x1234, "leaver") == 0);
    assert(thread_join(t, &value) == 0 && value == (void *)0x1234);
    ThreadAttr *detached = NULL;
    assert(attr_init(&detached) == 0 && attr_set_detach(&detached, 1) == 0);
    assert(thread_create(&t, &detached, inspector, NULL, "detached") == 0);
    const int32_t joined = thread_join(t, NULL); /* EINVAL, or ESRCH once its record is gone */
    assert(joined == ERR(22) || joined == ERR(3));
    assert(attr_destroy(&detached) == 0 && thread_join(self, NULL) == ERR(3)); /* the host-owned main thread */

    /* scePthreadAttrGet reports the stack the thread runs on: it holds this frame's locals. */
    ThreadAttr *attr = NULL;
    assert(attr_init(&attr) == 0 && attr_get(self, &attr) == 0);
    assert(attr->stack_address && attr->stack);
    char local = 0;
    assert((char *)attr->stack_address <= &local && &local < (char *)attr->stack_address + attr->stack);
    void *stack_address=NULL,*separate_address=NULL; uint64_t stack_size=0,separate_size=0;
    assert(attr_get_stack(&attr,&stack_address,&stack_size)==0);
    assert(attr_get_stackaddr(&attr,&separate_address)==0 && separate_address==stack_address);
    assert(attr_get_stacksize(&attr,&separate_size)==0 && separate_size==stack_size);
    assert((char *)stack_address<=&local && &local<(char *)stack_address+stack_size);
    assert(attr_get_stack(NULL,&stack_address,&stack_size)==ERR(22));
    assert(attr_get_stack(&attr,NULL,&stack_size)==ERR(22));
    assert(attr_get_stack(&attr,&stack_address,NULL)==ERR(22));
    assert(attr_get_stackaddr(&attr,NULL)==ERR(22) && attr_get_stacksize(&attr,NULL)==ERR(22));
    assert(posix_attr_get_stack(&attr,&stack_address,&stack_size)==0);
    assert(posix_attr_get_stackaddr(&attr,&separate_address)==0 && separate_address==stack_address);
    assert(posix_attr_get_stacksize(&attr,&separate_size)==0 && separate_size==stack_size);
    assert(posix_attr_get_stack(NULL,&stack_address,&stack_size)==22);
    /* The CPU backend records an x86 stack rather than its ARM host stack. */
    runtime_thread_set_stack((void *)UINT64_C(0x600000000),65536);
    assert(attr_get(self,&attr)==0 && attr_get_stack(&attr,&separate_address,&separate_size)==0);
    assert(separate_address==(void *)UINT64_C(0x600000000) && separate_size==65536);
    runtime_thread_set_stack(stack_address,stack_size);
    assert(attr_destroy(&attr) == 0);
    /* POSIX spellings share real handles but use positive guest error numbers. */
    assert(posix_attr_init(NULL)==22);
    assert(posix_attr_init(&attr)==0 && posix_attr_set_stack(&attr,1)==22);
    param=700;
    assert(posix_attr_set_param(&attr,&param)==0 && attr->prio==700);
    param=255;
    assert(posix_attr_set_param(&attr,&param)==45 && attr->prio==700);
    assert(posix_attr_set_param(&attr,NULL)==45 && posix_attr_set_param(NULL,&param)==22);
    assert(posix_attr_set_stack(&attr,DEFAULT_STACK)==0);
    int state=-1;
    assert(posix_attr_get_detach(&attr,&state)==0 && state==0);
    assert(posix_attr_get_detach(NULL,&state)==22);
    assert(posix_thread_create(NULL,NULL,inspector,NULL)==22);
    assert(posix_thread_create(&t,&attr,inspector,NULL)==0);
    assert(thread_equal(t,t) && !thread_equal(t,self));
    assert(posix_thread_join(t,&value)==0 && !value);
    assert(posix_thread_detach(t)==22);
    assert(posix_thread_join(self,NULL)==11 && posix_thread_join(NULL,NULL)==3);
    assert(posix_thread_detach(NULL)==3);
    assert(posix_attr_set_detach(&attr,1)==0);
    assert(posix_attr_get_detach(&attr,&state)==0 && state==1);
    assert(posix_attr_destroy(&attr)==0 && posix_attr_get_detach(&attr,&state)==22);
    puts("thread calls test: ok");
    return 0;
}
