/* URI component semantics and the SDK caller-owned pool/size-query contract. */
#define _GNU_SOURCE
#undef NDEBUG
#include "../src/runtime_net.c"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

uintptr_t runtime_lookup(const RuntimeExport *table,size_t count,const char *name) {
    for (size_t i=0;i<count;++i) if (!strcmp(table[i].name,name)) return (uintptr_t)table[i].function;
    return 0;
}
static void check_uri(const char *input,const char *const components[7],int opaque,uint16_t port) {
    uint64_t size=UINT64_MAX;
    assert(http_uri_parse(NULL,input,NULL,&size,0)==0 && size<65536);
    unsigned char *allocation=malloc((size_t)size+2); assert(allocation);
    memset(allocation,0xcc,(size_t)size+2);
    char *pool=(char *)allocation+1;
    HttpUriElement uri;
    memset(&uri,0xa5,sizeof(uri));
    uint64_t unchanged=999;
    assert(http_uri_parse(&uri,input,pool,&unchanged,size-1)==HTTP_OUT_OF_MEMORY);
    assert(unchanged==999 && !uri.scheme && !uri.path);
    for (uint64_t i=0;i<size+2;++i) assert(allocation[i]==0xcc);
    assert(http_uri_parse(&uri,input,pool,NULL,size)==0);
    assert(uri.opaque==opaque && uri.port==port);
    char *strings[]={uri.scheme,uri.username,uri.password,uri.hostname,uri.path,uri.query,uri.fragment};
    for (unsigned i=0;i<7;++i) {
        assert(strings[i]>=pool && strings[i]<pool+size);
        assert(!strcmp(strings[i],components[i]));
    }
    assert(allocation[0]==0xcc && allocation[size+1]==0xcc);
    assert(http_uri_parse(&uri,input,pool,&unchanged,size)==0 && unchanged==size);
    assert(http_uri_parse(&uri,input,NULL,&unchanged,0)==0 && unchanged==size);
    free(allocation);
}
int main(void) {
    const char *http[]={"https","user","pa:ss","example.com","/a/c","?q=a%20b","#frag"};
    check_uri("https://user:pa:ss@example.com:8443/a/./b/../c?q=a%20b#frag",http,0,8443);
    const char *file[]={"file","","","","/app0/Media/level0","",""};
    check_uri("file:///app0/Media/level0",file,0,0);
    const char *absolute[]={"","","","","/app0/Media/file%20name","?","#"};
    check_uri("/app0/Media/file%20name?#",absolute,1,0);
    const char *ipv6[]={"HTTP","","","2001:db8::1","/","",""};
    check_uri("HTTP://[2001:db8::1]/",ipv6,0,80);
    const char *https[]={"HTTPS","name","","host","","?a","#b"};
    check_uri("HTTPS://name@host?a#b",https,0,443);
    const char *relative[]={"","","","","../local","",""};
    check_uri("../local",relative,1,0);
    const char *empty[]={"","","","","","",""};
    check_uri("",empty,1,0);
    HttpUriElement uri; char pool[256]; uint64_t size=999;
    assert(http_uri_parse(NULL,NULL,NULL,&size,0)==HTTP_INVALID_URL && size==999);
    assert(http_uri_parse(NULL,"file:///x",NULL,NULL,0)==HTTP_INVALID_VALUE);
    assert(http_uri_parse(&uri,"file:///x",NULL,NULL,0)==HTTP_INVALID_VALUE);
    assert(http_uri_parse(&uri,"http://[::1/path",pool,&size,sizeof(pool))==HTTP_INVALID_URL);
    assert(http_uri_parse(NULL,"http://host:65537/",NULL,&size,0)==HTTP_INVALID_URL);
    assert(http_uri_parse(NULL,"http://host:12x/",NULL,&size,0)==HTTP_INVALID_URL);
    char host_limit[300]; memset(host_limit,'a',sizeof(host_limit));
    memcpy(host_limit,"http://",7); host_limit[7+255]='\0';
    assert(http_uri_parse(NULL,host_limit,NULL,&size,0)==HTTP_INVALID_URL);
    assert(runtime_net_resolve("sceHttpUriParse")== (uintptr_t)http_uri_parse);
    puts("HTTP URI parsing, caller-pool bounds and error contracts passed");
    return 0;
}
