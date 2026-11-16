/* Retained offline WebApi objects, filter ownership and cleanup contracts. */
#define _GNU_SOURCE
#undef NDEBUG
#include "../src/runtime_np.c"
#include <assert.h>

uintptr_t runtime_lookup(const RuntimeExport *table,size_t count,const char *name) {
    for (size_t i=0;i<count;++i) if (!strcmp(table[i].name,name)) return (uintptr_t)table[i].function;
    return 0;
}
int main(void) {
    WebApiPushParameter parameters[2]={{{"np:friend:online"}},{{"np:message"}}};
    assert(webapi_push_filter(1,parameters,2)==WEBAPI_LIB_NOT_FOUND);
    assert(webapi_push_filter(1,NULL,1)==WEBAPI_INVALID_ARGUMENT);
    int32_t first=webapi_initialize(7,4096),second=webapi_initialize(8,8192);
    assert(first>0 && second>first && webapi_find_library(first)->http==7);
    int32_t handle=webapi_handle(first),filter=webapi_push_filter(first,parameters,2);
    assert(handle>0 && filter>0);
    memset(parameters,0,sizeof(parameters)); /* the caller can immediately reuse its buffer */
    WebApiFilter *copy=webapi_find_filter(webapi_find_library(first),filter);
    assert(copy && copy->count==2 && !strcmp(copy->parameters[0].data_type,"np:friend:online") && !strcmp(copy->parameters[1].data_type,"np:message"));
    assert(webapi_delete_push_filter(second,filter)==WEBAPI_PUSH_FILTER_NOT_FOUND);
    assert(webapi_context(first,NULL)==WEBAPI_INVALID_ARGUMENT);
    int32_t user=webapi_context(first,"Player");
    assert(user>0 && ((uint32_t)user>>16)==(uint32_t)first && webapi_context_a(first,1)==WEBAPI_USER_EXISTS);
    assert(webapi_register_push_callback(user,filter,NULL,NULL)==WEBAPI_INVALID_ARGUMENT);
    assert(webapi_register_push_callback(user,filter+100,(void *)0x1234,NULL)==WEBAPI_PUSH_FILTER_NOT_FOUND);
    int32_t callback=webapi_register_push_callback(user,filter,(void *)0x1234,(void *)0x5678);
    assert(callback>0);
    WebApiCallback *record=webapi_find_user(webapi_find_library(first),user)->callbacks;
    assert(record && record->id==callback && record->function==(void *)0x1234 && record->userdata==(void *)0x5678);
    assert(webapi_unregister_service_callback(user,callback)==WEBAPI_SERVICE_CALLBACK_NOT_FOUND);
    assert(webapi_unregister_push_callback(user,callback)==0 && webapi_unregister_push_callback(user,callback)==WEBAPI_PUSH_CALLBACK_NOT_FOUND);
    assert(webapi_service_filter(first,handle,"service",0,parameters,1)==WEBAPI_SIGNED_IN_USER_NOT_FOUND);
    assert(webapi_service_filter(first,handle,"service",INVALID_SERVICE_LABEL,parameters,1)==WEBAPI_INVALID_ARGUMENT);
    assert(webapi_service_filter(first,-1,"service",0,parameters,1)==WEBAPI_HANDLE_NOT_FOUND);
    assert(webapi_delete_service_filter(first,filter)==WEBAPI_SERVICE_FILTER_NOT_FOUND);
    assert(webapi_register_service_callback(user,filter,(void *)0x1234,NULL)==WEBAPI_SERVICE_FILTER_NOT_FOUND);
    assert(webapi_register_push_callback(user,filter,(void *)0x1234,NULL)>0);
    assert(webapi_delete_context(user)==0 && webapi_delete_context(user)==WEBAPI_USER_NOT_FOUND);
    assert(webapi_unregister_push_callback(user,callback)==WEBAPI_USER_NOT_FOUND);
    assert(webapi_delete_push_filter(first,filter)==0 && webapi_delete_push_filter(first,filter)==WEBAPI_PUSH_FILTER_NOT_FOUND);
    assert(webapi_delete_handle(first,handle)==0 && webapi_delete_handle(first,handle)==WEBAPI_HANDLE_NOT_FOUND);
    /* Termination owns all remaining library objects and invalidates old IDs. */
    assert(webapi_push_filter(first,parameters,1)>0 && webapi_handle(first)>0 && webapi_context_a(first,1)>0);
    assert(webapi_terminate(first)==0 && !webapi_find_library(first));
    assert(webapi_handle(first)==WEBAPI_LIB_NOT_FOUND && webapi_terminate(first)==WEBAPI_LIB_NOT_FOUND);
    assert(webapi_terminate(second)==0 && !webapi_libraries);
    assert(runtime_np_resolve("sceNpWebApiCreatePushEventFilter")== (uintptr_t)webapi_push_filter);
    puts("Offline WebApi push filters, registration and cleanup passed");
    return 0;
}
