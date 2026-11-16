/* PSN libraries for the offline console (runtime_services.c holds NpManager, the older NpScore,
 * Matching2 and WebApi calls, and the trophies). The one user is signed out, so (shadPS4 np_*):
 *   - title and matching contexts requiring sign-in fail with NP SIGNED_OUT;
 *   - a call on a context or request that can therefore not exist fails with that library's
 *     INVALID_ID; a request object the old API created fails its transfer with SIGNED_OUT;
 *   - objects that need no sign-in (WebApi handles and push filters, InGameMessage contexts,
 *     NpAuth requests) are created, and their server calls fail with SIGNED_OUT.
 * Dialogs about PSN or the system (sign-in, invitation, error, social screen) draw nothing: an
 * opened one finishes at once, cancelled by the user. */
#include "runtime.h"
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <pthread.h>

#define NP_SIGNED_OUT ((int32_t)0x80550006)
#define COMMUNITY_INVALID_ARGUMENT ((int32_t)0x80550704)
#define COMMUNITY_INVALID_ID ((int32_t)0x8055070E)
#define AUTH_INVALID_ARGUMENT ((int32_t)0x80550301)
#define MATCHING2_INVALID_ARGUMENT ((int32_t)0x80550C0A)
#define MATCHING2_INVALID_CONTEXT_ID ((int32_t)0x80550C0B)
#define WEBAPI_INVALID_ARGUMENT ((int32_t)0x80552902)
#define WEBAPI_INVALID_LIB_CONTEXT_ID ((int32_t)0x80552903)
#define WEBAPI_OUT_OF_MEMORY ((int32_t)0x80552901)
#define WEBAPI_LIB_NOT_FOUND ((int32_t)0x80552904)
#define WEBAPI_USER_NOT_FOUND ((int32_t)0x80552905)
#define WEBAPI_REQUEST_NOT_FOUND ((int32_t)0x80552906)
#define WEBAPI_USER_EXISTS ((int32_t)0x8055290a)
#define WEBAPI_PUSH_FILTER_NOT_FOUND ((int32_t)0x8055290b)
#define WEBAPI_PUSH_CALLBACK_NOT_FOUND ((int32_t)0x8055290c)
#define WEBAPI_HANDLE_NOT_FOUND ((int32_t)0x8055290d)
#define WEBAPI_SERVICE_FILTER_NOT_FOUND ((int32_t)0x8055290e)
#define WEBAPI_SERVICE_CALLBACK_NOT_FOUND ((int32_t)0x8055290f)
#define WEBAPI_SIGNED_IN_USER_NOT_FOUND ((int32_t)0x80552910)
#define INVALID_SERVICE_LABEL 0xFFFFFFFFu
#define USER_INVALID (-1)

static _Atomic int32_t next_object=1;
static int32_t new_object(void) { return atomic_fetch_add(&next_object,1); }
static ABI int32_t ok(void) { return 0; }
static ABI int32_t signed_out(void) { return NP_SIGNED_OUT; }
/* Reachability is a local status query, not an attempted PSN sign-in. */
static ABI int32_t np_reachability(int32_t user,int32_t *state) {
    if (!state || user==USER_INVALID) return (int32_t)UINT32_C(0x80550003);
    *state=0; /* OrbisNpReachabilityState::Unavailable. */
    return 0;
}

/* ---- Tempest 4000 ----
 * sceNpCreateRequest(ctx) hands out a request id the caller then uses; the calls made on it
 * fail SIGNED_OUT like the rest, so a plain id is enough.
 * The sign-in dialog is never shown: it reports FINISHED at once and a result of
 * nobody signed in, so the game carries on offline. */
#define DIALOG_STATUS_FINISHED 4
#define SIGNIN_RESULT_CANCELED 1
static ABI int32_t np_request(void) { return new_object(); }
static ABI int32_t signin_status(void) { return DIALOG_STATUS_FINISHED; }
static ABI int32_t signin_result(void *result) {
    if (result) *(int32_t *)result = SIGNIN_RESULT_CANCELED;
    return 0;
}

/* ---- NpTus / NpTss (shadPS4 np_tus.cpp) ----
 * sceNpTusCreateNpTitleCtxA(label, user) and sceNpTssCreateNpTitleCtxA need the user's NpId:
 * SIGNED_OUT. With no title context, sceNpTusCreateRequest(ctx), every Get/Set/Add/TryAndSet/
 * Delete call on a request, sceNpTusAbortRequest, sceNpTusDeleteRequest and
 * sceNpTusDeleteNpTitleCtx find no such id: COMMUNITY INVALID_ID. */
static ABI int32_t community_no_id(void) { return COMMUNITY_INVALID_ID; }

/* ---- NpScore A variants (shadPS4 np_score.cpp) ----
 * sceNpScoreCreateNpTitleCtxA(label, user): SIGNED_OUT first, then the label is checked. The
 * ranking and game-data calls take a request from sceNpScoreCreateRequest and fail SIGNED_OUT,
 * as the older calls in runtime_services.c do. */
static ABI int32_t score_ctx_a(uint32_t label,int32_t user) { (void)label; (void)user; return NP_SIGNED_OUT; }

/* ---- NpMatching2 (shadPS4 np_matching2.cpp) ----
 * sceNpMatching2CreateContextA(param{userId, serviceLabel, size=0x10}, ctxId): INVALID_ARGUMENT
 * for a bad param, else SIGNED_OUT. Every call that names a context then fails with
 * INVALID_CONTEXT_ID. sceNpMatching2GetMemoryInfo / GetSslMemoryInfo: OK, as shadPS4. */
static ABI int32_t matching2_context_a(const uint8_t *param,uint16_t *ctx) {
    if (!param || !ctx) return MATCHING2_INVALID_ARGUMENT;
    uint64_t size; memcpy(&size,param+8,8);
    return size!=0x10 ? MATCHING2_INVALID_ARGUMENT : NP_SIGNED_OUT;
}
static ABI int32_t matching2_no_context(void) { return MATCHING2_INVALID_CONTEXT_ID; }

/* ---- NpWebApi, local objects with no PSN transport ----
 * A library/user context is a local registration, not a signed-in account.
 * Named service filters need PSN; ordinary push filters and callbacks can be
 * retained locally but receive no events while the console stays offline. */
typedef struct { char data_type[65]; } WebApiPushParameter;
_Static_assert(sizeof(WebApiPushParameter)==65,"WebApi push filter parameter ABI");
typedef struct WebApiFilter {
    int32_t id;
    uint64_t count;
    WebApiPushParameter *parameters;
    struct WebApiFilter *next;
} WebApiFilter;
typedef struct WebApiHandle { int32_t id; struct WebApiHandle *next; } WebApiHandle;
typedef struct WebApiCallback {
    int32_t id,filter;
    void *function,*userdata;
    struct WebApiCallback *next;
} WebApiCallback;
typedef struct WebApiUser {
    int32_t id,user;
    WebApiCallback *callbacks;
    struct WebApiUser *next;
} WebApiUser;
typedef struct WebApiLibrary {
    int32_t id,http;
    uint64_t pool_size;
    WebApiHandle *handles;
    WebApiFilter *filters;
    WebApiUser *users;
    struct WebApiLibrary *next;
} WebApiLibrary;
static pthread_mutex_t webapi_lock=PTHREAD_MUTEX_INITIALIZER;
static WebApiLibrary *webapi_libraries;
static uint32_t webapi_next_library,webapi_next_user;
static WebApiLibrary *webapi_find_library(int32_t id) {
    for (WebApiLibrary *lib=webapi_libraries;lib;lib=lib->next) if (lib->id==id) return lib;
    return NULL;
}
static WebApiHandle *webapi_find_handle(WebApiLibrary *lib,int32_t id) {
    for (WebApiHandle *handle=lib->handles;handle;handle=handle->next) if (handle->id==id) return handle;
    return NULL;
}
static WebApiFilter *webapi_find_filter(WebApiLibrary *lib,int32_t id) {
    for (WebApiFilter *filter=lib->filters;filter;filter=filter->next) if (filter->id==id) return filter;
    return NULL;
}
static WebApiUser *webapi_find_user(WebApiLibrary *lib,int32_t id) {
    for (WebApiUser *user=lib->users;user;user=user->next) if (user->id==id) return user;
    return NULL;
}
static void webapi_free_user(WebApiUser *user) {
    while (user->callbacks) { WebApiCallback *cb=user->callbacks; user->callbacks=cb->next; free(cb); }
    free(user);
}
static ABI int32_t webapi_initialize(int32_t http,uint64_t pool_size) {
    WebApiLibrary *lib=calloc(1,sizeof(*lib));
    if (!lib) return WEBAPI_OUT_OF_MEMORY;
    pthread_mutex_lock(&webapi_lock);
    if (++webapi_next_library>=0x8000) { --webapi_next_library; pthread_mutex_unlock(&webapi_lock); free(lib); return (int32_t)0x8055291a; }
    lib->id=(int32_t)webapi_next_library; lib->http=http; lib->pool_size=pool_size;
    lib->next=webapi_libraries; webapi_libraries=lib;
    pthread_mutex_unlock(&webapi_lock);
    return lib->id;
}
static ABI int32_t webapi_terminate(int32_t id) {
    pthread_mutex_lock(&webapi_lock);
    WebApiLibrary **slot=&webapi_libraries;
    while (*slot && (*slot)->id!=id) slot=&(*slot)->next;
    if (!*slot) { pthread_mutex_unlock(&webapi_lock); return WEBAPI_LIB_NOT_FOUND; }
    WebApiLibrary *lib=*slot; *slot=lib->next;
    while (lib->users) { WebApiUser *user=lib->users; lib->users=user->next; webapi_free_user(user); }
    while (lib->filters) { WebApiFilter *filter=lib->filters; lib->filters=filter->next; free(filter->parameters); free(filter); }
    while (lib->handles) { WebApiHandle *handle=lib->handles; lib->handles=handle->next; free(handle); }
    free(lib); pthread_mutex_unlock(&webapi_lock);
    return 0;
}
static ABI int32_t webapi_context_a(int32_t id,int32_t user_id) {
    if (id>=0x8000) return WEBAPI_INVALID_LIB_CONTEXT_ID;
    if (user_id==USER_INVALID) return WEBAPI_INVALID_ARGUMENT;
    pthread_mutex_lock(&webapi_lock);
    WebApiLibrary *lib=webapi_find_library(id);
    if (!lib) { pthread_mutex_unlock(&webapi_lock); return WEBAPI_LIB_NOT_FOUND; }
    for (WebApiUser *user=lib->users;user;user=user->next) if (user->user==user_id) {
        pthread_mutex_unlock(&webapi_lock); return WEBAPI_USER_EXISTS;
    }
    WebApiUser *user=calloc(1,sizeof(*user));
    if (!user) { pthread_mutex_unlock(&webapi_lock); return WEBAPI_OUT_OF_MEMORY; }
    if (++webapi_next_user>=0x10000) webapi_next_user=1;
    user->id=(int32_t)(((uint32_t)id<<16)|webapi_next_user); user->user=user_id;
    user->next=lib->users; lib->users=user;
    pthread_mutex_unlock(&webapi_lock);
    return user->id;
}
static ABI int32_t webapi_context(int32_t lib,const void *online_id) {
    if (lib>=0x8000) return WEBAPI_INVALID_LIB_CONTEXT_ID;
    return online_id ? webapi_context_a(lib,1) : WEBAPI_INVALID_ARGUMENT;
}
static ABI int32_t webapi_delete_context(int32_t id) {
    pthread_mutex_lock(&webapi_lock);
    WebApiLibrary *lib=webapi_find_library((int32_t)((uint32_t)id>>16));
    if (!lib) { pthread_mutex_unlock(&webapi_lock); return WEBAPI_LIB_NOT_FOUND; }
    WebApiUser **slot=&lib->users;
    while (*slot && (*slot)->id!=id) slot=&(*slot)->next;
    if (!*slot) { pthread_mutex_unlock(&webapi_lock); return WEBAPI_USER_NOT_FOUND; }
    WebApiUser *user=*slot; *slot=user->next; webapi_free_user(user);
    pthread_mutex_unlock(&webapi_lock);
    return 0;
}
static ABI int32_t webapi_handle(int32_t id) {
    pthread_mutex_lock(&webapi_lock);
    WebApiLibrary *lib=webapi_find_library(id);
    if (!lib) { pthread_mutex_unlock(&webapi_lock); return WEBAPI_LIB_NOT_FOUND; }
    WebApiHandle *handle=malloc(sizeof(*handle));
    if (!handle) { pthread_mutex_unlock(&webapi_lock); return WEBAPI_OUT_OF_MEMORY; }
    handle->id=new_object(); handle->next=lib->handles; lib->handles=handle;
    pthread_mutex_unlock(&webapi_lock);
    return handle->id;
}
static ABI int32_t webapi_delete_handle(int32_t id,int32_t handle_id) {
    pthread_mutex_lock(&webapi_lock);
    WebApiLibrary *lib=webapi_find_library(id);
    if (!lib) { pthread_mutex_unlock(&webapi_lock); return WEBAPI_LIB_NOT_FOUND; }
    WebApiHandle **slot=&lib->handles;
    while (*slot && (*slot)->id!=handle_id) slot=&(*slot)->next;
    if (!*slot) { pthread_mutex_unlock(&webapi_lock); return WEBAPI_HANDLE_NOT_FOUND; }
    WebApiHandle *handle=*slot; *slot=handle->next; free(handle);
    pthread_mutex_unlock(&webapi_lock);
    return 0;
}
static ABI int32_t webapi_push_filter(int32_t id,const WebApiPushParameter *parameters,uint64_t count) {
    if (!parameters || !count) return WEBAPI_INVALID_ARGUMENT;
    if (count>SIZE_MAX/sizeof(*parameters)) return WEBAPI_OUT_OF_MEMORY;
    pthread_mutex_lock(&webapi_lock);
    WebApiLibrary *lib=webapi_find_library(id);
    if (!lib) { pthread_mutex_unlock(&webapi_lock); return WEBAPI_LIB_NOT_FOUND; }
    WebApiFilter *filter=calloc(1,sizeof(*filter));
    if (filter) filter->parameters=malloc((size_t)count*sizeof(*parameters));
    if (!filter || !filter->parameters) { free(filter); pthread_mutex_unlock(&webapi_lock); return WEBAPI_OUT_OF_MEMORY; }
    memcpy(filter->parameters,parameters,(size_t)count*sizeof(*parameters)); filter->count=count;
    filter->id=new_object(); filter->next=lib->filters; lib->filters=filter;
    pthread_mutex_unlock(&webapi_lock);
    return filter->id;
}
static ABI int32_t webapi_delete_push_filter(int32_t id,int32_t filter_id) {
    pthread_mutex_lock(&webapi_lock);
    WebApiLibrary *lib=webapi_find_library(id);
    if (!lib) { pthread_mutex_unlock(&webapi_lock); return WEBAPI_LIB_NOT_FOUND; }
    WebApiFilter **slot=&lib->filters;
    while (*slot && (*slot)->id!=filter_id) slot=&(*slot)->next;
    if (!*slot) { pthread_mutex_unlock(&webapi_lock); return WEBAPI_PUSH_FILTER_NOT_FOUND; }
    WebApiFilter *filter=*slot; *slot=filter->next; free(filter->parameters); free(filter);
    pthread_mutex_unlock(&webapi_lock);
    return 0;
}
static ABI int32_t webapi_service_filter(int32_t id,int32_t handle,const char *service,uint32_t label,const WebApiPushParameter *parameters,uint64_t count) {
    if (!service || label==INVALID_SERVICE_LABEL || !parameters || !count) return WEBAPI_INVALID_ARGUMENT;
    pthread_mutex_lock(&webapi_lock);
    WebApiLibrary *lib=webapi_find_library(id);
    int32_t result=!lib ? WEBAPI_LIB_NOT_FOUND : !webapi_find_handle(lib,handle) ? WEBAPI_HANDLE_NOT_FOUND : WEBAPI_SIGNED_IN_USER_NOT_FOUND;
    pthread_mutex_unlock(&webapi_lock);
    return result;
}
static ABI int32_t webapi_delete_service_filter(int32_t id,int32_t filter) {
    (void)filter;
    pthread_mutex_lock(&webapi_lock);
    int32_t result=webapi_find_library(id) ? WEBAPI_SERVICE_FILTER_NOT_FOUND : WEBAPI_LIB_NOT_FOUND;
    pthread_mutex_unlock(&webapi_lock); return result;
}
static int32_t webapi_push_callback(int32_t id,int32_t filter,void *function,void *userdata,int service) {
    if (!function) return WEBAPI_INVALID_ARGUMENT;
    pthread_mutex_lock(&webapi_lock);
    WebApiLibrary *lib=webapi_find_library((int32_t)((uint32_t)id>>16));
    WebApiUser *user=lib ? webapi_find_user(lib,id) : NULL;
    int32_t result=!lib ? WEBAPI_LIB_NOT_FOUND : !user ? WEBAPI_USER_NOT_FOUND : service ? WEBAPI_SERVICE_FILTER_NOT_FOUND :
        !webapi_find_filter(lib,filter) ? WEBAPI_PUSH_FILTER_NOT_FOUND : 0;
    if (!result) {
        WebApiCallback *callback=malloc(sizeof(*callback));
        if (!callback) result=WEBAPI_OUT_OF_MEMORY;
        else { callback->id=new_object(); callback->filter=filter; callback->function=function; callback->userdata=userdata;
            callback->next=user->callbacks; user->callbacks=callback; result=callback->id; }
    }
    pthread_mutex_unlock(&webapi_lock); return result;
}
static ABI int32_t webapi_register_push_callback(int32_t user,int32_t filter,void *callback,void *argument) {
    return webapi_push_callback(user,filter,callback,argument,0);
}
static ABI int32_t webapi_register_service_callback(int32_t user,int32_t filter,void *callback,void *argument) {
    return webapi_push_callback(user,filter,callback,argument,1);
}
static int32_t webapi_unregister_callback(int32_t id,int32_t callback_id,int service) {
    pthread_mutex_lock(&webapi_lock);
    WebApiLibrary *lib=webapi_find_library((int32_t)((uint32_t)id>>16));
    WebApiUser *user=lib ? webapi_find_user(lib,id) : NULL;
    int32_t result=!lib ? WEBAPI_LIB_NOT_FOUND : !user ? WEBAPI_USER_NOT_FOUND : 0;
    if (!result) {
        WebApiCallback **slot=&user->callbacks;
        while (*slot && (*slot)->id!=callback_id) slot=&(*slot)->next;
        if (service || !*slot) result=service ? WEBAPI_SERVICE_CALLBACK_NOT_FOUND : WEBAPI_PUSH_CALLBACK_NOT_FOUND;
        else { WebApiCallback *callback=*slot; *slot=callback->next; free(callback); }
    }
    pthread_mutex_unlock(&webapi_lock); return result;
}
static ABI int32_t webapi_unregister_push_callback(int32_t user,int32_t callback) { return webapi_unregister_callback(user,callback,0); }
static ABI int32_t webapi_unregister_service_callback(int32_t user,int32_t callback) { return webapi_unregister_callback(user,callback,1); }
/* sceNpWebApiCreateExtdPushEventFilter(lib, handle, service name, label, params, count). */
static ABI int32_t webapi_filter(int32_t lib,int32_t handle,const char *service,uint32_t label,const void *params,uint64_t count) {
    (void)handle;
    if ((service && label==INVALID_SERVICE_LABEL) || !params || !count) return WEBAPI_INVALID_ARGUMENT;
    return lib>=0x8000 ? WEBAPI_INVALID_LIB_CONTEXT_ID : new_object();
}
/* sceNpWebApiRegisterExtdPushEventCallbackA(userCtx, filter, callback, arg). */
static ABI int32_t webapi_callback_a(int32_t ctx,int32_t filter,void *callback,void *arg) {
    (void)ctx; (void)filter; (void)arg; return callback ? NP_SIGNED_OUT : WEBAPI_INVALID_ARGUMENT;
}

/* ---- NpUtility (shadPS4 np_utility.cpp) ----
 * sceNpWordFilterCreateTitleCtxA(user): SIGNED_OUT; the request and sanitize calls then name a
 * context that does not exist: COMMUNITY INVALID_ID. The bandwidth test measures the PSN
 * connection: sceNpBandwidthTestInitStart / GetStatus / Shutdown fail SIGNED_OUT, Abort is OK. */

/* ---- NpManager InGameMessage ----
 * sceNpInGameMessageInitialize(pool size, option) gives a library context and
 * sceNpInGameMessageCreateHandle(ctx) a handle; PrepareA and SendDataA send to other players:
 * SIGNED_OUT. Delete, Terminate and the pool statistics are OK. */
static ABI int32_t object(void) { return new_object(); }

/* ---- NpAuth (shadPS4 np_auth.cpp) ----
 * sceNpAuthCreateRequest: a synchronous request id (0x10000001 up). sceNpAuthGetAuthorizationCodeA
 * (req, param, code, issuer) and sceNpAuthGetIdTokenA(req, param, token) need param and output,
 * then fail SIGNED_OUT. sceNpAuthAbortRequest: OK. */
static _Atomic int32_t next_auth=0x10000001;
static ABI int32_t auth_request(void) { return atomic_fetch_add(&next_auth,1); }
static ABI int32_t auth_code_a(int32_t req,const void *param,void *code,int32_t *issuer) {
    (void)req; (void)issuer; return !param || !code ? AUTH_INVALID_ARGUMENT : NP_SIGNED_OUT;
}

/* ---- NpCommerce: sceNpCommerceShowPsStoreIcon(pos) / HidePsStoreIcon: OK, no icon is drawn
 * (the Store is unreachable offline). ---- */

/* ---- Dialogs: status 0 none, 1 initialized, 2 running, 3 finished ---- */
#define DIALOG_NOT_INITIALIZED ((int32_t)0x80B80003)
#define DIALOG_ALREADY_INITIALIZED ((int32_t)0x80B80004)
#define DIALOG_NOT_FINISHED ((int32_t)0x80B80005)
#define DIALOG_ARG_NULL ((int32_t)0x80B8000D)
#define RESULT_USER_CANCELED 1
enum { SIGNIN, INVITATION, ERROR_DIALOG, SOCIAL_SCREEN, FRIEND_LIST, DIALOGS };
static const char *const dialog_names[DIALOGS]={"SigninDialog","InvitationDialog","ErrorDialog","SocialScreenDialog","NpFriendListDialog"};
static _Atomic int dialog_status[DIALOGS];
static int32_t dialog_init(int d) {
    int none=0;
    return atomic_compare_exchange_strong(&dialog_status[d],&none,1) ? 0 : DIALOG_ALREADY_INITIALIZED;
}
static int32_t dialog_open(int d,const char *detail) {
    if (!atomic_load(&dialog_status[d])) return DIALOG_NOT_INITIALIZED;
    printf("Runtime: %s opened%s; finished at once (offline, no dialog UI)\n",dialog_names[d],detail);
    atomic_store(&dialog_status[d],3); return 0;
}
static int32_t dialog_result(int d,void *result) {
    if (atomic_load(&dialog_status[d])!=3) return DIALOG_NOT_FINISHED;
    return result ? 0 : DIALOG_ARG_NULL;
}
static int32_t dialog_term(int d) {
    if (!atomic_load(&dialog_status[d])) return DIALOG_NOT_INITIALIZED;
    atomic_store(&dialog_status[d],0); return 0;
}
/* sceInvitationDialog*: nothing can be sent. OrbisInvitationDialogResultA: callbackArg @0 (kept),
 * errorCode @8, result @12, sentUsers @16, reserved[32]. */
static ABI int32_t invitation_init(void) { return dialog_init(INVITATION); }
static ABI int32_t invitation_open(const void *param) { (void)param; return dialog_open(INVITATION,""); }
static ABI int32_t invitation_status(void) { return atomic_load(&dialog_status[INVITATION]); }
static ABI int32_t invitation_result(uint8_t *result) {
    int32_t r=dialog_result(INVITATION,result);
    if (r) return r;
    int32_t codes[2]={NP_SIGNED_OUT,RESULT_USER_CANCELED};
    memcpy(result+8,codes,8); memset(result+16,0,40);
    return 0;
}
static ABI int32_t invitation_term(void) { return dialog_term(INVITATION); }
/* sceErrorDialog*: param {int32 size, int32 errorCode, ...}; the code goes to the log. */
static ABI int32_t error_init(void) { return dialog_init(ERROR_DIALOG); }
static ABI int32_t error_open(const int32_t *param) {
    if (!param) return DIALOG_ARG_NULL;
    char detail[48]; snprintf(detail,sizeof(detail)," (error 0x%08x)",(uint32_t)param[1]);
    return dialog_open(ERROR_DIALOG,detail);
}
static ABI int32_t error_status(void) { return atomic_load(&dialog_status[ERROR_DIALOG]); }
static ABI int32_t error_term(void) { return dialog_term(ERROR_DIALOG); }
/* sceInvitationDialogClose / sceErrorDialogClose: as sceSocialScreenDialogClose. */
static ABI int32_t invitation_close(void) { return atomic_load(&dialog_status[INVITATION]) ? 0 : DIALOG_NOT_INITIALIZED; }
static ABI int32_t error_close(void) { return atomic_load(&dialog_status[ERROR_DIALOG]) ? 0 : DIALOG_NOT_INITIALIZED; }
/* sceNpFriendListDialog*: no friends offline; the player leaves it. The result's first field is
 * the result (USER_CANCELED). */
static ABI int32_t friends_init(void) { return dialog_init(FRIEND_LIST); }
static ABI int32_t friends_open(const void *param) { (void)param; return dialog_open(FRIEND_LIST,""); }
static ABI int32_t friends_status(void) { return atomic_load(&dialog_status[FRIEND_LIST]); }
static ABI int32_t friends_result(int32_t *result) {
    int32_t r=dialog_result(FRIEND_LIST,result);
    if (!r) *result=RESULT_USER_CANCELED;
    return r;
}
static ABI int32_t friends_close(void) { return atomic_load(&dialog_status[FRIEND_LIST]) ? 0 : DIALOG_NOT_INITIALIZED; }
static ABI int32_t friends_term(void) { return dialog_term(FRIEND_LIST); }

static const RuntimeExport exports[]={
    {"sceNpGetNpReachabilityState",np_reachability},
    {"sceNpTusCreateNpTitleCtxA",signed_out}, {"sceNpTssCreateNpTitleCtxA",signed_out},
    {"sceNpTusCreateRequest",community_no_id}, {"sceNpTusDeleteRequest",community_no_id},
    {"sceNpTusAbortRequest",community_no_id}, {"sceNpTusDeleteNpTitleCtx",community_no_id},

    /* The asynchronous forms name the same requests that cannot exist, and so does polling one. */
    {"sceNpTssGetDataAsync",community_no_id}, {"sceNpTusPollAsync",community_no_id},
    {"sceNpTusAddAndGetVariableAAsync",community_no_id}, {"sceNpTusAddAndGetVariableAVUserAsync",community_no_id},
    {"sceNpTusGetDataAAsync",community_no_id}, {"sceNpTusGetDataAVUserAsync",community_no_id},
    {"sceNpTusGetMultiSlotVariableAAsync",community_no_id}, {"sceNpTusGetMultiSlotVariableAVUserAsync",community_no_id},
    {"sceNpTusSetDataAAsync",community_no_id}, {"sceNpTusSetDataAVUserAsync",community_no_id},
    {"sceNpScoreCreateNpTitleCtxA",score_ctx_a},

    /* Asynchronous ranking and record calls fail as the synchronous ones; nothing is in flight to poll. */
    {"sceNpScoreGetBoardInfoAsync",signed_out}, {"sceNpScoreGetFriendsRankingAAsync",signed_out},
    {"sceNpScoreGetGameDataByAccountIdAsync",signed_out}, {"sceNpScoreGetRankingByAccountIdAsync",signed_out},
    {"sceNpScoreGetRankingByRangeAAsync",signed_out}, {"sceNpScoreRecordGameDataAsync",signed_out},
    {"sceNpScoreRecordScoreAsync",signed_out}, {"sceNpScorePollAsync",community_no_id},
    {"sceNpMatching2CreateContextA",matching2_context_a},
    {"sceNpMatching2CreateJoinRoomA",matching2_no_context}, {"sceNpMatching2JoinRoomA",matching2_no_context},

    {"sceNpMatching2GetRoomDataExternalList",matching2_no_context},

    {"sceNpMatching2GetRoomMemberDataExternalList",matching2_no_context},

    {"sceNpMatching2RegisterRoomMessageCallback",matching2_no_context},

    {"sceNpWebApiInitialize",webapi_initialize}, {"sceNpWebApiTerminate",webapi_terminate},
    {"sceNpWebApiCreateContext",webapi_context}, {"sceNpWebApiCreateContextA",webapi_context_a},
    {"sceNpWebApiDeleteContext",webapi_delete_context},
    {"sceNpWebApiCreateHandle",webapi_handle}, {"sceNpWebApiDeleteHandle",webapi_delete_handle}, {"sceNpWebApiSendRequest2",signed_out},
    {"sceNpWebApiCreatePushEventFilter",webapi_push_filter}, {"sceNpWebApiDeletePushEventFilter",webapi_delete_push_filter},
    {"sceNpWebApiCreateServicePushEventFilter",webapi_service_filter}, {"sceNpWebApiDeleteServicePushEventFilter",webapi_delete_service_filter},
    {"sceNpWebApiRegisterPushEventCallback",webapi_register_push_callback}, {"sceNpWebApiUnregisterPushEventCallback",webapi_unregister_push_callback},
    {"sceNpWebApiRegisterServicePushEventCallback",webapi_register_service_callback}, {"sceNpWebApiUnregisterServicePushEventCallback",webapi_unregister_service_callback},

    {"sceNpWebApiCreateExtdPushEventFilter",webapi_filter}, {"sceNpWebApiDeleteExtdPushEventFilter",ok},
    {"sceNpWebApiRegisterExtdPushEventCallbackA",webapi_callback_a},
    {"sceNpWebApiUnregisterExtdPushEventCallback",ok},
    {"sceNpWordFilterCreateTitleCtxA",signed_out}, {"sceNpWordFilterDeleteTitleCtx",community_no_id},
    {"sceNpWordFilterDeleteRequest",community_no_id},
    {"sceNpWordFilterAbortRequest",community_no_id},
    {"sceNpWordFilterCensorComment",community_no_id}, {"sceNpWordFilterCreateAsyncRequest",community_no_id},
    {"sceNpWordFilterPollAsync",community_no_id},

    {"sceNpInGameMessageInitialize",object}, {"sceNpInGameMessageTerminate",ok},
    {"sceNpInGameMessageCreateHandle",object}, {"sceNpInGameMessageDeleteHandle",ok},
    {"sceNpInGameMessageAbortHandle",ok},
    {"sceNpInGameMessagePrepareA",signed_out}, {"sceNpInGameMessageSendDataA",signed_out},

    {"sceNpAuthCreateRequest",auth_request}, {"sceNpAuthGetAuthorizationCodeA",auth_code_a},
    {"sceNpAuthAbortRequest",ok},

    {"sceInvitationDialogInitialize",invitation_init}, {"sceInvitationDialogOpenA",invitation_open},
    {"sceInvitationDialogUpdateStatus",invitation_status}, {"sceInvitationDialogGetResultA",invitation_result},
    {"sceInvitationDialogTerminate",invitation_term}, {"sceInvitationDialogClose",invitation_close},
    {"sceErrorDialogInitialize",error_init}, {"sceErrorDialogOpen",error_open},
    {"sceErrorDialogUpdateStatus",error_status}, {"sceErrorDialogTerminate",error_term},
    {"sceErrorDialogClose",error_close},
    {"sceNpFriendListDialogInitialize",friends_init}, {"sceNpFriendListDialogOpenA",friends_open},
    {"sceNpFriendListDialogUpdateStatus",friends_status}, {"sceNpFriendListDialogGetResultA",friends_result},
    {"sceNpFriendListDialogClose",friends_close}, {"sceNpFriendListDialogTerminate",friends_term},

    /* ---- Tempest 4000: the synchronous NpManager, NpScore, NpTrophy and SigninDialog calls ----
     * No account is signed in, so identity and leaderboard calls report SIGNED_OUT; the dialog
     * opens and finishes at once with nobody signed in. */
    {"sceNpGetOnlineId",signed_out}, {"sceNpCreateRequest",np_request},
    {"sceNpRegisterNpReachabilityStateCallback",ok},
    {"sceNpScoreGetBoardInfo",signed_out}, {"sceNpScoreGetFriendsRankingA",signed_out},
    {"sceNpScoreGetRankingByAccountId",signed_out}, {"sceNpScoreGetRankingByRangeA",signed_out},
    {"sceNpScoreRecordScore",signed_out},
    {"sceNpTrophyGetTrophyInfo",signed_out},
    {"sceSigninDialogInitialize",ok}, {"sceSigninDialogOpen",ok},
    {"sceSigninDialogUpdateStatus",signin_status}, {"sceSigninDialogGetResult",signin_result},
    {"sceSigninDialogTerminate",ok},
};
uintptr_t runtime_np_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
