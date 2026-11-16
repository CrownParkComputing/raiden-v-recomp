/* Networking for an offline console (the cable is unplugged, as in runtime_services.c): the
 * libSceHttp calls runtime_services.c does not have create objects, and no request ever gets a
 * response (HTTP NETWORK); the Second Screen companion server never sees a client. */
#include "runtime.h"
#include <ctype.h>
#include <stdatomic.h>
#include <stddef.h>
#include <string.h>

/* ---- libSceHttp (shadPS4 http.cpp), offline ---- */
#define HTTP_NETWORK ((int32_t)0x80431063)
#define HTTP_UNKNOWN_SCHEME ((int32_t)0x80431061)
#define HTTP_INVALID_VALUE ((int32_t)0x804311fe)
#define HTTP_INVALID_URL ((int32_t)0x80433060)
#define HTTP_OUT_OF_MEMORY ((int32_t)0x80431022)
static _Atomic int32_t http_next=0x10000;
static int same(const char *a,const char *b) {
    while (*a && tolower((unsigned char)*a)==*b) ++a, ++b;
    return !*a && !*b;
}
/* sceHttpCreateConnection(template, server, scheme, port, keepalive): an id; the server is
 * required and the scheme is http or https. */
static ABI int32_t http_connection(int32_t tmpl,const char *server,const char *scheme,uint16_t port,int32_t keepalive) {
    (void)tmpl; (void)port; (void)keepalive;
    if (!server) return HTTP_INVALID_VALUE;
    if (!scheme || (!same(scheme,"http") && !same(scheme,"https"))) return HTTP_UNKNOWN_SCHEME;
    return atomic_fetch_add(&http_next,1);
}
/* sceHttpCreateRequest2(connection, method, path, content length): an id; the path is required. */
static ABI int32_t http_request2(int32_t conn,const char *method,const char *path,uint64_t length) {
    (void)conn; (void)method; (void)length;
    if (!path) return HTTP_INVALID_VALUE;
    return atomic_fetch_add(&http_next,1);
}
/* sceHttpGetAllResponseHeaders(request, header, size): no request gets a response. */
static ABI int32_t http_headers(int32_t req,char **header,uint64_t *size) {
    (void)req;
    if (!header || !size) return HTTP_INVALID_VALUE;
    return HTTP_NETWORK;
}
/* Timeouts and aborting a request: accepted, as shadPS4 does; nothing is sent. */
static ABI int32_t http_ok(void) { return 0; }

/* http.h OrbisHttpUriElement: opaque at +0, seven string pointers at +8,
 * port at +64. Each string, including absent components, belongs to pool. */
typedef struct HttpUriElement {
    uint8_t opaque;
    char *scheme,*username,*password,*hostname,*path,*query,*fragment;
    uint16_t port;
    uint8_t reserved[10];
} HttpUriElement;
_Static_assert(sizeof(HttpUriElement)==80,"HTTP URI element ABI size");
_Static_assert(offsetof(HttpUriElement,scheme)==8 && offsetof(HttpUriElement,port)==64,"HTTP URI element ABI offsets");
typedef struct { const char *text; size_t length; } UriPart;
static int uri_alpha(unsigned char c) { return (c>='a' && c<='z') || (c>='A' && c<='Z'); }
static int uri_digit(unsigned char c) { return c>='0' && c<='9'; }
static int uri_alnum(unsigned char c) { return uri_alpha(c) || uri_digit(c); }
static int uri_user_character(unsigned char c) {
    return uri_alnum(c) || (c && strchr("!$%&'()*+,-.:;=_~",c));
}
/* sceHttpUriSweepPath's absolute-path rule removes ./ and ../ segments;
 * relative paths and terminal dots are retained. Normalization only shrinks. */
static void uri_sweep_path(char *path,size_t length) {
    if (!length || path[0]!='/') return;
    size_t read=1,write=1;
    while (read<length) {
        if (path[read]=='.' && read+1<length && path[read+1]=='/') { read+=2; continue; }
        if (path[read]=='.' && read+2<length && path[read+1]=='.' && path[read+2]=='/') {
            if (write>1) {
                --write;
                while (write>1 && path[write-1]!='/') --write;
            }
            read+=3;
            continue;
        }
        do { path[write++]=path[read++]; } while (read<length && path[read-1]!='/');
    }
    path[write]='\0';
}
static ABI int32_t http_uri_parse(HttpUriElement *out,const char *source,void *pool,uint64_t *required,uint64_t capacity) {
    if (!source) return HTTP_INVALID_URL;
    int write_output=out && pool;
    if (!write_output && !required) return HTTP_INVALID_VALUE;
    if (write_output) memset(out,0,sizeof(*out));
    UriPart parts[7]={{"",0},{"",0},{"",0},{"",0},{"",0},{"",0},{"",0}};
    size_t offset=0,scheme_length=0;
    while (scheme_length<32 && source[scheme_length] && source[scheme_length]!=':') {
        unsigned char c=(unsigned char)source[scheme_length];
        if (!uri_alnum(c) && c!='+' && c!='-' && c!='.') break;
        ++scheme_length;
    }
    if (scheme_length && source[scheme_length]==':' && uri_alpha((unsigned char)source[0])) {
        parts[0]=(UriPart){source,scheme_length}; offset=scheme_length+1;
    }
    uint8_t opaque=1;
    if (source[offset]=='/' && source[offset+1]=='/') { offset+=2; opaque=0; }
    size_t scan=0,colon=0; int seen_colon=0,seen_at=0;
    while (source[offset+scan]) {
        unsigned char c=(unsigned char)source[offset+scan];
        if (c=='@') { seen_at=1; break; }
        if (!seen_colon && c==':') { seen_colon=1; colon=scan; }
        else if (!uri_user_character(c)) break;
        ++scan;
    }
    if (seen_at) {
        parts[1]=(UriPart){source+offset,seen_colon ? colon : scan};
        if (seen_colon) parts[2]=(UriPart){source+offset+colon+1,scan-colon-1};
        offset+=scan+1;
    }
    const char *host=source+offset;
    size_t host_scan=0,host_length=0;
    if (host[0]=='[') {
        host_scan=1;
        while (host[host_scan] && host[host_scan]!=']') {
            unsigned char c=(unsigned char)host[host_scan];
            if (host_scan>=255 || (!uri_alnum(c) && c!='-' && c!='.' && c!='_' && c!=':')) return HTTP_INVALID_URL;
            ++host_scan;
        }
        if (host_scan>=255 || host[host_scan]!=']') return HTTP_INVALID_URL;
        host_length=host_scan-1;
        parts[3]=(UriPart){host+1,host_length}; ++host_scan;
    } else if (host[0]!='.') {
        while (uri_alnum((unsigned char)host[host_scan]) || host[host_scan]=='-' || host[host_scan]=='.' || host[host_scan]=='_') {
            if (++host_scan>=255) return HTTP_INVALID_URL;
        }
        host_length=host_scan;
        parts[3]=(UriPart){host,host_length};
    }
    offset+=host_scan;
    uint16_t port=0; int explicit_port=0;
    if (source[offset]==':') {
        ++offset;
        uint32_t value=0; size_t digits=0;
        while (digits<5 && uri_digit((unsigned char)source[offset+digits])) {
            value=value*10+(uint32_t)(source[offset+digits]-'0'); ++digits;
        }
        if (value>65536 || (source[offset+digits] && source[offset+digits]!='/')) return HTTP_INVALID_URL;
        if (digits) { explicit_port=1; port=(uint16_t)value; }
        offset+=digits;
    }
    if (!explicit_port) {
        const char *scheme=parts[0].text; size_t n=parts[0].length;
        /* The primary library applies these case-insensitive scheme prefixes. */
        if (n>=5 && tolower((unsigned char)scheme[0])=='h' && tolower((unsigned char)scheme[1])=='t' &&
            tolower((unsigned char)scheme[2])=='t' && tolower((unsigned char)scheme[3])=='p' && tolower((unsigned char)scheme[4])=='s') port=443;
        else if ((n>=4 && tolower((unsigned char)scheme[0])=='h' && tolower((unsigned char)scheme[1])=='t' &&
                  tolower((unsigned char)scheme[2])=='t' && tolower((unsigned char)scheme[3])=='p') ||
                 (n>=3 && tolower((unsigned char)scheme[0])=='t' && tolower((unsigned char)scheme[1])=='t' &&
                  tolower((unsigned char)scheme[2])=='p')) port=80;
    }
    size_t length=0;
    while (source[offset+length] && source[offset+length]!='?' && source[offset+length]!='#') {
        if (length>=0x3fff) return HTTP_INVALID_URL;
        ++length;
    }
    parts[4]=(UriPart){source+offset,length}; offset+=length;
    length=0;
    if (source[offset]=='?') {
        length=1;
        while (source[offset+length] && source[offset+length]!='#') {
            if (length>=0x3fff) return HTTP_INVALID_URL;
            ++length;
        }
    }
    parts[5]=(UriPart){source+offset,length}; offset+=length;
    length=0;
    if (source[offset]=='#') {
        length=1;
        while (source[offset+length]) { if (length>=0x3fff) return HTTP_INVALID_URL; ++length; }
    }
    parts[6]=(UriPart){source+offset,length};
    uint64_t total=7;
    for (unsigned i=0;i<7;++i) total+=parts[i].length;
    if (write_output) {
        if (capacity<total) return HTTP_OUT_OF_MEMORY;
        char *position=pool,*strings[7];
        for (unsigned i=0;i<7;++i) {
            strings[i]=position;
            memcpy(position,parts[i].text,parts[i].length);
            position[parts[i].length]='\0'; position+=parts[i].length+1;
        }
        out->opaque=opaque; out->scheme=strings[0]; out->username=strings[1]; out->password=strings[2];
        out->hostname=strings[3]; out->path=strings[4]; out->query=strings[5]; out->fragment=strings[6]; out->port=port;
        uri_sweep_path(out->path,parts[4].length);
    }
    if (required) *required=total;
    return 0;
}

/* libSceCompanionHttpd: the HTTP server for the PS4 Second Screen companion app. It starts and
 * accepts handlers, but no companion app can reach it, so no event ever arrives: GetEvent reports
 * none (the game only tests for nonzero; it reacts to 0x10000001 connected / 0x10000002 gone). */
#define COMPANION_NO_EVENT ((int32_t)0x80E40008)
static ABI int32_t companion_no_event(void *event) { (void)event; return COMPANION_NO_EVENT; }

/* ---- Tempest 4000 ---- */
static ABI int32_t net_ok(void) { return 0; }
/* sceNetCtlGetResult(eventType, errorCode): no request is outstanding, so the error is 0. */
static ABI int32_t netctl_result(int32_t event,int32_t *error) {
    (void)event;
    if (error) *error = 0;
    return 0;
}

static const RuntimeExport exports[]={
    {"sceCompanionHttpdInitialize2",http_ok}, {"sceCompanionHttpdTerminate",http_ok},
    {"sceCompanionHttpdStart",http_ok}, {"sceCompanionHttpdStop",http_ok},
    {"sceCompanionHttpdRegisterRequestCallback",http_ok}, {"sceCompanionHttpdUnregisterRequestCallback",http_ok},
    {"sceCompanionHttpdAddHeader",http_ok}, {"sceCompanionHttpdSetBody",http_ok},
    {"sceCompanionHttpdSetStatus",http_ok}, {"sceCompanionHttpdGetEvent",companion_no_event},

    {"sceHttpCreateConnection",http_connection}, {"sceHttpCreateRequest2",http_request2},
    {"sceHttpGetAllResponseHeaders",http_headers},
    {"sceHttpUriParse",http_uri_parse},
    {"sceHttpAbortRequest",http_ok},
    {"sceHttpSetSendTimeOut",http_ok}, {"sceHttpSetResolveTimeOut",http_ok},
    {"sceHttpSetRecvTimeOut",http_ok},

    /* Tempest 4000 brings the stack up before asking for a connection. Starting and stopping
     * succeed; the connection state is reported by sceNetCtlGetResult, which has nothing to
     * report because nothing was ever requested. */
    {"sceNetInit",net_ok}, {"sceNetTerm",net_ok},
    {"sceNetCtlInit",net_ok}, {"sceNetCtlTerm",net_ok}, {"sceNetCtlGetResult",netctl_result},
};
uintptr_t runtime_net_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
