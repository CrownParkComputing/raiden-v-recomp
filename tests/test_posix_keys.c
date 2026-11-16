/* POSIX TLS key errors, per-thread values, reuse generations and destructor passes. */
#undef NDEBUG
#include "../src/runtime_kernel.c"
#include <assert.h>
#include <stdatomic.h>

int32_t *runtime_errno(void) { static _Thread_local int32_t value; return &value; }
void runtime_report(void) {}
void runtime_thread_count_yield(int zero_sleep) { (void)zero_sleep; }
uintptr_t runtime_lookup(const RuntimeExport *table,size_t count,const char *name) {
    (void)table; (void)count; (void)name; return 0;
}
typedef struct { uint32_t key; int rearm; atomic_int calls; } Value;
static atomic_int old_destructors;
static void ABI old_destructor(void *value) { (void)value; atomic_fetch_add(&old_destructors,1); }
static void ABI new_destructor(void *argument) {
    Value *value=argument;
    int calls=atomic_fetch_add(&value->calls,1)+1;
    if (value->rearm && calls<2) assert(posix_key_set(value->key,value)==0);
}
#ifdef TEMPEST_FEX
uintptr_t runtime_fex_call(uintptr_t entry,const uintptr_t *arguments,size_t count) {
    assert(count==1); ((KeyDestructor)entry)((void *)arguments[0]); return 0;
}
#endif
typedef struct {
    uint32_t key;
    pthread_barrier_t barrier;
    Value old_value,new_value;
} Shared;
static void rendezvous(pthread_barrier_t *barrier) {
    int result=pthread_barrier_wait(barrier);
    assert(result==0 || result==PTHREAD_BARRIER_SERIAL_THREAD);
}
static void *worker(void *argument) {
    Shared *s=argument;
    assert(key_get(s->key)==NULL);
    assert(posix_key_set(s->key,&s->old_value)==0);
    rendezvous(&s->barrier); rendezvous(&s->barrier);
    assert(key_get(s->key)==NULL); /* this slot now denotes another key generation */
    runtime_thread_keys_cleanup();
    assert(posix_key_set(s->key,&s->new_value)==0);
    assert(key_get(s->key)==&s->new_value);
    runtime_thread_keys_cleanup();
    assert(key_get(s->key)==NULL);
    return NULL;
}

int main(void) {
    Shared shared={0};
    assert(posix_key_create(NULL,NULL)==22);
    assert(posix_key_create(&shared.key,old_destructor)==0);
    Value main_old={.key=shared.key},main_new={.key=shared.key,.rearm=1};
    shared.old_value.key=shared.new_value.key=shared.key;
    assert(posix_key_set(shared.key,&main_old)==0 && key_get(shared.key)==&main_old);
    assert(pthread_barrier_init(&shared.barrier,NULL,2)==0);
    pthread_t thread; assert(pthread_create(&thread,NULL,worker,&shared)==0);
    rendezvous(&shared.barrier);
    uint32_t original=shared.key,reused=0;
    assert(posix_key_delete(original)==0 && key_get(original)==NULL);
    assert(posix_key_delete(original)==22 && posix_key_set(original,&main_old)==22);
    assert(posix_key_create(&reused,new_destructor)==0 && reused==original);
    assert(key_get(reused)==NULL && posix_key_set(reused,&main_new)==0);
    rendezvous(&shared.barrier);
    assert(pthread_join(thread,NULL)==0);
    assert(atomic_load(&shared.new_value.calls)==1 && !atomic_load(&old_destructors));
    assert(key_get(reused)==&main_new);
    runtime_thread_keys_cleanup();
    assert(atomic_load(&main_new.calls)==2 && key_get(reused)==NULL);
    assert(posix_key_delete(reused)==0);
    assert(posix_key_delete(KEYS)==22 && posix_key_set(KEYS,NULL)==22 && !key_get(KEYS));
    assert(pthread_barrier_destroy(&shared.barrier)==0);
    puts("POSIX TLS key isolation, reuse and destructor passes passed");
    return 0;
}
