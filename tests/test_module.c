/* Loaded-module enumeration, stable handles and the short-array contract. */
#undef NDEBUG
#include "../src/runtime_module.c"
#include <assert.h>

int runtime_verbose(void) { return 0; }
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *name) {
    (void)table; (void)count; (void)name; return 0;
}

static ABI int32_t start(uint64_t args, const void *argp, void *param) {
    assert(args==0 && !argp && !param);
    /* The module is visible during its initializer, without holding the loader lock. */
    int32_t handles[4]; uint64_t count=0;
    assert(module_list(handles,4,&count)==0 && count==4);
    for (int i=0;i<4;++i) assert(handles[i]==i);
    return 17;
}

int main(void) {
    RuntimeModule table[3]={{.name="first.prx",.started=1,.base=0x10000,.size=0x10000},
                            {.name="second.prx",.flags=RUNTIME_MODULE_DEFERRED,.base=0x20000,.size=0x10000},
                            {.name="third.prx",.started=1,.base=0x30000,.size=0x10000}};
    runtime_module_register((unsigned char *)start,table,3);
    RuntimeModule executable={.name="eboot.bin",.size=0x10000};
    runtime_module_set_main(&executable);
    runtime_module_add_segment(0,0x1000,5);
    runtime_module_add_segment(0x4000,0x3000,4);
    runtime_module_add_segment(0x8000,0x1000,6);
    runtime_module_add_segment(0x10000,0x5001,5);
    runtime_module_add_segment(0x20000,0x4000,5);
    ModuleInfo info={.st_size=sizeof(info)};
    assert(module_info(0,&info)==0 && !strcmp(info.name,"eboot.bin"));
    assert(info.num_segments==3 && info.segments[0].size==0x4000);
    assert(info.segments[0].address==(uint64_t)(uintptr_t)start);
    assert(info.segments[1].address==(uint64_t)(uintptr_t)start+0x4000);
    assert(info.segments[1].prot==4 && info.segments[2].prot==6);
    assert(module_info(1,&info)==0 && !strcmp(info.name,"first.prx"));
    assert(info.num_segments==1 && info.segments[0].size==0x8000);
    assert(info.segments[0].address==(uint64_t)(uintptr_t)start+0x10000);
    assert(module_info(2,&info)==KERNEL_ESRCH); /* mapped but not yet loaded */
    assert(module_info(-1,&info)==KERNEL_ESRCH && module_info(4,&info)==KERNEL_ESRCH);
    assert(module_info(0,NULL)==KERNEL_EFAULT);
    info.st_size=sizeof(info)-1;
    assert(module_info(0,&info)==KERNEL_EINVAL);
    info.st_size=sizeof(info);
    int32_t handles[5]={-1,-1,-1,-1,-1}; uint64_t count=99;
    assert(module_list(handles,5,&count)==0 && count==3);
    assert(handles[0]==0 && handles[1]==1 && handles[2]==3 && handles[3]==-1);
    count=99;
    assert(module_list(handles,2,&count)==KERNEL_ENOMEM && count==99);
    assert(handles[0]==0 && handles[1]==1);
    assert(module_list(handles,0,&count)==KERNEL_ENOMEM && count==99);
    assert(module_list(NULL,5,&count)==KERNEL_EFAULT && count==99);
    assert(module_list(handles,5,NULL)==KERNEL_EFAULT);
    int32_t result=0;
    assert(runtime_module_start("second.prx",0,NULL,&result)==2 && result==17);
    assert(module_info(2,&info)==0 && !strcmp(info.name,"second.prx"));
    assert(info.num_segments==1 && info.segments[0].size==0x4000);
    assert(module_list(handles,5,&count)==0 && count==4);
    for (int i=0;i<4;++i) assert(handles[i]==i);
    puts("Module enumeration and deferred-load visibility passed");
    return 0;
}
