/* Move library lifecycle and disconnected-device result contracts. */
#undef NDEBUG
#include "../src/runtime_devices.c"
#include <assert.h>

uintptr_t runtime_lookup(const RuntimeExport *table,size_t count,const char *name) {
    for (size_t i=0;i<count;++i) if (!strcmp(table[i].name,name)) return (uintptr_t)table[i].function;
    return 0;
}
int main(void) {
    unsigned char sample[80],original[80];
    memset(sample,0xa5,sizeof(sample)); memcpy(original,sample,sizeof(sample));
    int32_t count=123;
    assert(move_open(1,0,0)==MOVE_NOT_INIT && move_term()==MOVE_NOT_INIT);
    assert(move_read(0,NULL)==MOVE_NOT_INIT && move_vibration(0,255)==MOVE_NOT_INIT);
    assert(move_init()==0 && move_init()==MOVE_ALREADY_INIT);
    int32_t first=move_open(1,0,0),second=move_open(2,0,0);
    assert(first==0x30b0100 && second==first+0x100);
    assert(move_read(first,NULL)==MOVE_INVALID_ARG);
    assert(move_read(first,sample)==MOVE_NO_CONTROLLER);
    assert(move_read_recent(first,-1,sample,&count)==MOVE_INVALID_ARG);
    assert(move_read_recent(first,0,NULL,&count)==MOVE_INVALID_ARG);
    assert(move_read_recent(first,0,sample,NULL)==MOVE_INVALID_ARG);
    assert(move_read_recent(first,0,sample,&count)==MOVE_NO_CONTROLLER);
    assert(count==123 && !memcmp(sample,original,sizeof(sample)));
    assert(move_vibration(first,255)==MOVE_NO_CONTROLLER && move_light(first,1,2,3)==MOVE_NO_CONTROLLER);
    assert(move_close_or_reset(first)==0);
    assert(move_term()==0 && move_read(second,sample)==MOVE_NOT_INIT);
    assert(move_close_or_reset(second)==MOVE_NOT_INIT && move_term()==MOVE_NOT_INIT);
    assert(move_init()==0 && move_open(1,0,0)==second+0x100 && move_term()==0);
    assert(runtime_devices_resolve("sceMoveInit")== (uintptr_t)move_init);
    puts("Move lifecycle and disconnected-device contracts passed");
    return 0;
}
