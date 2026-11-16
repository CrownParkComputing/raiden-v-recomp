/* PS4 Audiodec ATRAC9 adapter. The shipped RF caller uses codec 1, 12-byte
 * configuration, 36-byte BSI and a 32-byte four-pointer control record.
 * Evidence: out/audiodec-contract; OpenOrbis include/orbis/Audiodec.h.
 * One call produces one codec frame, bounded by the caller's output capacity.
 * ClearContext replaces the decoder: LibAtrac9InitDecoder alone does not clear
 * its overlap samples or position within a partially consumed superframe. */
#include "runtime.h"
#include "libatrac9.h"
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define AD_ERROR(n) ((int32_t)(0x807f0000u | (n)))
#define AD_API_FAIL AD_ERROR(0)
#define AD_INVALID_TYPE AD_ERROR(1)
#define AD_ARGUMENT AD_ERROR(2)
#define AD_PARAM_SIZE AD_ERROR(4)
#define AD_INFO_SIZE AD_ERROR(5)
#define AD_INPUT_SIZE AD_ERROR(6)
#define AD_OUTPUT_SIZE AD_ERROR(7)
#define AD_CONTROL_POINTER AD_ERROR(8)
#define AD_PARAM_POINTER AD_ERROR(9)
#define AD_INFO_POINTER AD_ERROR(10)
#define AD_INPUT_POINTER AD_ERROR(11)
#define AD_OUTPUT_POINTER AD_ERROR(12)
#define AD_INPUT_BUFFER AD_ERROR(13)
#define AD_OUTPUT_BUFFER AD_ERROR(14)
#define AD_HANDLE AD_ERROR(15)
#define AD_WORD_LENGTH AD_ERROR(16)
#define AD_INPUT_BYTES AD_ERROR(17)
#define AD_OUTPUT_BYTES AD_ERROR(18)
#define AD_CONFIG AD_ERROR(0x1000)
#define AD_ATRAC9 1u
#define AD_MAX_DECODERS 64

typedef struct { uint32_t size; int32_t word_length; uint8_t config[4]; } AdParam;
typedef struct {
    uint32_t size, channels, bitrate, sample_rate, superframe_bytes;
    uint32_t frames_in_superframe, next_frame_bytes, frame_samples;
    int32_t result;
} AdInfo;
typedef struct { uint32_t size; void *data; uint32_t bytes; } AdBuffer;
typedef struct { AdParam *param; AdInfo *info; AdBuffer *input, *output; } AdControl;
_Static_assert(sizeof(AdParam)==12 && sizeof(AdInfo)==36, "PS4 Audiodec metadata ABI");
_Static_assert(sizeof(AdBuffer)==24 && offsetof(AdBuffer,data)==8 &&
               offsetof(AdBuffer,bytes)==16 && sizeof(AdControl)==32, "PS4 Audiodec buffer ABI");

typedef struct {
    int32_t id, word_length;
    void *codec;
    uint8_t config[4];
    Atrac9CodecInfo geometry;
    uint32_t frame_index, superframe_read;
} AdDecoder;
static pthread_mutex_t ad_mutex=PTHREAD_MUTEX_INITIALIZER;
static AdDecoder ad_decoders[AD_MAX_DECODERS];
static uint32_t ad_references;
static int32_t ad_next_id;
static uint64_t ad_created, ad_decoded, ad_cleared, ad_failed, ad_input, ad_output;

static AdDecoder *ad_find(int32_t id) {
    if (id<=0) return NULL;
    for (size_t i=0;i<AD_MAX_DECODERS;++i)
        if (ad_decoders[i].id==id && ad_decoders[i].codec) return &ad_decoders[i];
    return NULL;
}

static int ad_new_codec(AdDecoder *d) {
    void *codec=Atrac9GetHandle();
    Atrac9CodecInfo g;
    if (!codec) return 0;
    if (Atrac9InitDecoder(codec,d->config) || Atrac9GetCodecInfo(codec,&g) ||
        g.channels<1 || g.channels>8 || g.frameSamples<1 || g.frameSamples>256 ||
        g.framesInSuperframe<1 || g.framesInSuperframe>8 || g.superframeSize<1 ||
        g.superframeSize>16384) {
        Atrac9ReleaseHandle(codec);
        return 0;
    }
    if (d->codec) Atrac9ReleaseHandle(d->codec);
    d->codec=codec;
    d->geometry=g;
    d->frame_index=d->superframe_read=0;
    return 1;
}

static AdInfo ad_info(const AdDecoder *d) {
    const Atrac9CodecInfo *g=&d->geometry;
    const uint32_t samples=(uint32_t)g->frameSamples*(uint32_t)g->framesInSuperframe;
    return (AdInfo){36,(uint32_t)g->channels,
        (uint32_t)((uint64_t)g->superframeSize*8u*(uint32_t)g->samplingRate/samples),
        (uint32_t)g->samplingRate,(uint32_t)g->superframeSize,
        (uint32_t)g->framesInSuperframe,(uint32_t)g->superframeSize-d->superframe_read,
        (uint32_t)g->frameSamples,0};
}

/* The game places control/AU/PCM records at addresses aligned to four bytes.
 * Copy records rather than dereferencing native eight-byte-aligned structs. */
static int32_t ad_control(const void *guest, AdControl *c, AdParam *p, AdInfo *i) {
    if (!guest) return AD_CONTROL_POINTER;
    memcpy(c,guest,sizeof(*c));
    if (!c->param) return AD_PARAM_POINTER;
    if (!c->info) return AD_INFO_POINTER;
    uint32_t param_size,info_size;
    memcpy(&param_size,c->param,sizeof(param_size));
    memcpy(&info_size,c->info,sizeof(info_size));
    if (param_size!=sizeof(*p)) return AD_PARAM_SIZE;
    if (info_size!=sizeof(*i)) return AD_INFO_SIZE;
    memcpy(p,c->param,sizeof(*p));
    memcpy(i,c->info,sizeof(*i));
    return 0;
}

static int32_t ad_init_impl(uint32_t type) {
    /* Only the codec actually selected by this title is implemented. */
    if (type!=AD_ATRAC9) return AD_INVALID_TYPE;
    pthread_mutex_lock(&ad_mutex);
    int32_t r=ad_references==UINT32_MAX ? AD_API_FAIL : 0;
    if (!r) ++ad_references;
    pthread_mutex_unlock(&ad_mutex);
    return r;
}
static int32_t ad_term_impl(uint32_t type) {
    if (type!=AD_ATRAC9) return AD_INVALID_TYPE;
    pthread_mutex_lock(&ad_mutex);
    if (ad_references) --ad_references;
    pthread_mutex_unlock(&ad_mutex);
    return 0;
}
static int32_t ad_create_impl(void *guest, uint32_t type) {
    if (type!=AD_ATRAC9) return AD_INVALID_TYPE;
    AdControl c; AdParam p; AdInfo info;
    int32_t r=ad_control(guest,&c,&p,&info);
    if (r) return r;
    if (p.word_length!=1 && p.word_length!=2) return AD_WORD_LENGTH;
    /* LibAtrac9's channel configuration table has six entries; reject reserved
     * indices before passing user configuration to the bundled decoder. */
    if (p.config[0]!=0xfe || (p.config[1]&1) || ((p.config[1]>>1)&7)>=6)
        return AD_CONFIG;
    pthread_mutex_lock(&ad_mutex);
    if (!ad_references) r=AD_ARGUMENT;
    else if (ad_next_id==INT32_MAX) r=AD_API_FAIL;
    else {
        AdDecoder *d=NULL;
        for (size_t k=0;k<AD_MAX_DECODERS;++k)
            if (!ad_decoders[k].codec) { d=&ad_decoders[k]; break; }
        if (!d) r=AD_API_FAIL;
        else {
            memset(d,0,sizeof(*d));
            d->word_length=p.word_length;
            memcpy(d->config,p.config,4);
            if (!ad_new_codec(d)) r=AD_CONFIG;
            else {
                d->id=++ad_next_id;
                info=ad_info(d);
                memcpy(c.info,&info,sizeof(info));
                ++ad_created;
                r=d->id;
            }
        }
    }
    pthread_mutex_unlock(&ad_mutex);
    return r;
}
static int32_t ad_delete_impl(int32_t handle) {
    pthread_mutex_lock(&ad_mutex);
    AdDecoder *d=ad_find(handle);
    int32_t r=d ? 0 : AD_HANDLE;
    if (d) {
        Atrac9ReleaseHandle(d->codec);
        memset(d,0,sizeof(*d));
    }
    pthread_mutex_unlock(&ad_mutex);
    return r;
}
static int32_t ad_clear_impl(int32_t handle) {
    pthread_mutex_lock(&ad_mutex);
    AdDecoder *d=ad_find(handle);
    int32_t r=!d ? AD_HANDLE : !ad_new_codec(d) ? AD_API_FAIL : 0;
    if (!r) ++ad_cleared;
    pthread_mutex_unlock(&ad_mutex);
    return r;
}

static void ad_counts(const AdControl *c, uint32_t input, uint32_t output) {
    memcpy((char *)c->input+offsetof(AdBuffer,bytes),&input,sizeof(input));
    memcpy((char *)c->output+offsetof(AdBuffer,bytes),&output,sizeof(output));
}
static int32_t ad_decode_impl(int32_t handle, void *guest) {
    AdControl c; AdParam p; AdInfo info; AdBuffer input,output;
    int32_t r=ad_control(guest,&c,&p,&info);
    if (r) return r;
    if (!c.input) return AD_INPUT_POINTER;
    if (!c.output) return AD_OUTPUT_POINTER;
    memcpy(&input,c.input,sizeof(input));
    memcpy(&output,c.output,sizeof(output));
    if (!input.data) return AD_INPUT_BUFFER;
    if (!output.data) return AD_OUTPUT_BUFFER;
    if (!input.bytes || input.bytes==UINT32_MAX) return AD_INPUT_BYTES;
    if (!output.bytes || output.bytes==UINT32_MAX) return AD_OUTPUT_BYTES;
    if (input.size!=sizeof(input)) return AD_INPUT_SIZE;
    if (output.size!=sizeof(output)) return AD_OUTPUT_SIZE;
    pthread_mutex_lock(&ad_mutex);
    AdDecoder *d=ad_find(handle);
    if (!d) r=AD_HANDLE;
    else {
        const uint32_t remain=(uint32_t)d->geometry.superframeSize-d->superframe_read;
        const uint32_t bytes=(uint32_t)d->geometry.channels*(uint32_t)d->geometry.frameSamples*
                             (d->word_length==1 ? 2u : 4u);
        if (output.bytes<bytes) r=AD_OUTPUT_BYTES;
        else if (input.bytes<remain) {
            /* No incomplete superframe is handed to a zero-extending bit reader.
             * The caller retains/retries these bytes; decoding state is untouched. */
            ad_counts(&c,0,0);
            r=AD_API_FAIL;
        } else {
            union { int16_t s16[256*8]; float f32[256*8]; } pcm;
            int used=0;
            const int status=d->word_length==1 ?
                Atrac9Decode(d->codec,input.data,(int)remain,pcm.s16,&used,0) :
                Atrac9DecodeF32(d->codec,input.data,(int)remain,pcm.f32,&used,0);
            if (status || used<=0 || (uint32_t)used>remain) {
                /* A malformed packet may have changed overlap/band state. Reset
                 * rather than report synthesized silence or reuse corrupt state. */
                (void)ad_new_codec(d);
                ad_counts(&c,0,0);
                r=AD_API_FAIL;
            } else {
                uint32_t consumed=(uint32_t)used;
                d->superframe_read+=consumed;
                if (++d->frame_index==(uint32_t)d->geometry.framesInSuperframe) {
                    /* Fixed-size superframes end with padding not consumed by
                     * LibAtrac9. The next superframe must start after that pad. */
                    consumed=remain;
                    d->frame_index=d->superframe_read=0;
                }
                memcpy(output.data,&pcm,bytes);
                ad_counts(&c,consumed,bytes);
                info=ad_info(d);
                memcpy(c.info,&info,sizeof(info));
                ++ad_decoded; ad_input+=consumed; ad_output+=bytes;
            }
        }
        if (r) ++ad_failed;
    }
    pthread_mutex_unlock(&ad_mutex);
    return r;
}

/* Kernel-style returns do not alter either host errno or guest POSIX errno. */
static ABI int32_t audiodec_init(uint32_t t) { int e=errno; int32_t r=ad_init_impl(t); errno=e; return r; }
static ABI int32_t audiodec_term(uint32_t t) { int e=errno; int32_t r=ad_term_impl(t); errno=e; return r; }
static ABI int32_t audiodec_create(void *c,uint32_t t) { int e=errno; int32_t r=ad_create_impl(c,t); errno=e; return r; }
static ABI int32_t audiodec_delete(int32_t h) { int e=errno; int32_t r=ad_delete_impl(h); errno=e; return r; }
static ABI int32_t audiodec_clear(int32_t h) { int e=errno; int32_t r=ad_clear_impl(h); errno=e; return r; }
static ABI int32_t audiodec_decode(int32_t h,void *c) { int e=errno; int32_t r=ad_decode_impl(h,c); errno=e; return r; }

uintptr_t runtime_audiodec_resolve(const char *name) {
    static const struct { const char *nid; uintptr_t address; } exports[]={
        {"VjhsmxpcezI#libSceAudiodec",(uintptr_t)audiodec_init},
        {"h5jSB2QIDV0#libSceAudiodec",(uintptr_t)audiodec_term},
        {"O3f1sLMWRvs#libSceAudiodec",(uintptr_t)audiodec_create},
        {"Tp+ZEy69mLk#libSceAudiodec",(uintptr_t)audiodec_delete},
        {"6Vf9WTLDoss#libSceAudiodec",(uintptr_t)audiodec_clear},
        {"KHXHMDLkILw#libSceAudiodec",(uintptr_t)audiodec_decode},
    };
    for (size_t i=0;i<sizeof(exports)/sizeof(*exports);++i)
        if (!strcmp(name,exports[i].nid)) return exports[i].address;
    return 0;
}
void runtime_audiodec_report(void) {
    pthread_mutex_lock(&ad_mutex);
    size_t active=0;
    for (size_t i=0;i<AD_MAX_DECODERS;++i) active+=ad_decoders[i].codec!=NULL;
    printf("Runtime: Audiodec ATRAC9 created=%llu active=%zu decoded=%llu reset=%llu "
           "failed=%llu input=%llu PCM=%llu bytes\n",
           (unsigned long long)ad_created,active,(unsigned long long)ad_decoded,
           (unsigned long long)ad_cleared,(unsigned long long)ad_failed,
           (unsigned long long)ad_input,(unsigned long long)ad_output);
    pthread_mutex_unlock(&ad_mutex);
}
