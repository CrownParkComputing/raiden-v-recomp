/* NGS2 rendering and PS4 voice events. The decoder is deterministic so these checks need no
 * game files and can distinguish duplicated, skipped and out-of-order audio grains. */
#undef NDEBUG
#include "../src/runtime_ngs2.c"
#include <assert.h>
#include <math.h>
#include <unistd.h>

#ifdef TEMPEST_FEX
static unsigned guest_callback_calls;
uintptr_t runtime_fex_call(uintptr_t entry, const uintptr_t *arguments, size_t count) {
    if (count==4)
        return (uintptr_t)((ParseReadHandler)entry)(arguments[0],(uint32_t)arguments[1],
                                                    (void *)arguments[2],arguments[3]);
    assert(count == 1);
    ++guest_callback_calls;
    ((VoiceCallback)entry)((const VoiceCallbackInfo *)arguments[0]);
    return 0;
}
#endif

void *runtime_low_alloc(size_t size) { return calloc(1,size); }
void runtime_low_free(void *pointer) { free(pointer); }
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *name) {
    (void)table; (void)count; (void)name;
    return 0;
}

/* Each byte names four stereo samples in a four-frame superframe. Like LibAtrac9,
 * InitDecoder configures the codec without clearing a partially consumed superframe. */
#ifndef NGS2_REAL_ATRAC9
typedef struct { unsigned frame; } TestDecoder;
void *Atrac9GetHandle(void) { return calloc(1,sizeof(TestDecoder)); }
void Atrac9ReleaseHandle(void *handle) { free(handle); }
int Atrac9InitDecoder(void *handle, unsigned char *config) {
    (void)config;
    return handle ? 0 : -1;
}
int Atrac9GetCodecInfo(void *handle, Atrac9CodecInfo *info) {
    if (!handle) return -1;
    *info=(Atrac9CodecInfo){.channels=2,.samplingRate=48000,.superframeSize=4,
                           .framesInSuperframe=4,.frameSamples=4};
    return 0;
}
int Atrac9DecodeF32(void *handle, const unsigned char *data, int size, float *pcm,
                    int *used, int nointerleave) {
    (void)nointerleave;
    if (!handle || size<1) return -1;
    TestDecoder *decoder=handle;
    if ((data[0]&3)!=decoder->frame) return -1;
    decoder->frame=(decoder->frame+1)&3;
    for (int i=0;i<4;++i)
        pcm[2*i]=pcm[2*i+1]=(float)(4*data[0]+i+1)/128.0f;
    *used=1;
    return 0;
}
#endif

typedef struct {
    void *system, *samplers, *masters, *submixers;
    void *source, *bus, *master[2];
    unsigned char data[64];
    float output[2][8];
    RenderBufferInfo buffers[2];
} Fixture;

static void event(void *voice, uint32_t value) {
    struct { VoiceParamHeader header; uint32_t value; } param={{12,0,6},value};
    assert(voice_control(voice,&param)==0);
}
static void patch(void *voice, uint32_t port, void *destination) {
    struct { VoiceParamHeader header; uint32_t port, input; void *destination; }
        param={{24,0,5},port,0,destination};
    assert(voice_control(voice,&param)==0);
}
static void volume(void *voice, uint32_t port, float level) {
    struct { VoiceParamHeader header; uint32_t port; float level; }
        param={{16,0,2},port,level};
    assert(voice_control(voice,&param)==0);
}
static void *make_rack(void *system, uint32_t id, uint32_t count) {
    unsigned char option[40]={0};
    uint64_t size=sizeof(option);
    memcpy(option,&size,sizeof(size));
    memcpy(option+32,&count,sizeof(count));
    void *rack=NULL;
    assert(rack_create(system,id,option,NULL,&rack)==0);
    return rack;
}
static void fixture_init(Fixture *f) {
    memset(f,0,sizeof(*f));
    assert(system_create(NULL,NULL,&f->system)==0);
    /* Tiny synthetic codec grains make sequence assertions readable. Production defaults
     * are checked separately; buffer capacity must never determine even this test clock. */
    ((System *)f->system)->grain=((System *)f->system)->max_grain=4;
    f->samplers=make_rack(f->system,0x4001,1);
    f->submixers=make_rack(f->system,0x2000,1);
    f->masters=make_rack(f->system,0x3000,2);
    assert(rack_voice_handle(f->samplers,0,&f->source)==0);
    assert(rack_voice_handle(f->submixers,0,&f->bus)==0);
    for (uint32_t i=0;i<2;++i) {
        assert(rack_voice_handle(f->masters,i,&f->master[i])==0);
        struct { VoiceParamHeader header; uint32_t output, reserved; }
            param={{16,0,0x30000005},i,0};
        assert(voice_control(f->master[i],&param)==0);
        event(f->master[i],0); /* PS4 PLAY is zero, not a bit mask. */
        f->buffers[i]=(RenderBufferInfo){f->output[i],sizeof(f->output[i]),0x18,2};
    }
    patch(f->source,0,f->bus);
    patch(f->bus,0,f->master[1]);
    event(f->bus,0);
    struct { VoiceParamHeader header; WaveformFormat format; uint32_t flags, reserved; }
        setup={{40,0,0x40010000},{0x40,2,48000,0xfe740bf0,0,0},0,0};
    assert(voice_control(f->source,&setup)==0);
    for (unsigned i=0;i<sizeof(f->data);++i) f->data[i]=(unsigned char)i;
}
static void fixture_destroy(Fixture *f) {
    if (f->samplers) assert(rack_destroy(f->samplers,NULL)==0);
    assert(rack_destroy(f->submixers,NULL)==0);
    assert(rack_destroy(f->masters,NULL)==0);
    assert(system_destroy(f->system,NULL)==0);
}
static void submit(Fixture *f, const WaveformBlock *block) {
    struct { VoiceParamHeader header; const void *data; uint32_t flags, count;
             const WaveformBlock *blocks; }
        param={{32,0,0x40010001},f->data,1,1,block};
    assert(voice_control(f->source,&param)==0);
}
static void waveform(Fixture *f) {
    WaveformBlock block={.data_size=sizeof(f->data),.samples=sizeof(f->data)*4};
    submit(f,&block);
    /* The caller can reuse its descriptor as soon as VoiceControl returns. */
    memset(&block,0,sizeof(block));
}
static void render(Fixture *f) {
    memset(f->output,0xcc,sizeof(f->output));
    assert(system_render(f->system,f->buffers,2)==0);
}
static void silent(const float *output) {
    for (unsigned i=0;i<8;++i) assert(output[i]==0.0f);
}
static void samples(const float *output, unsigned first, float gain) {
    for (unsigned i=0;i<4;++i) {
        float expected=(float)(first+i)/128.0f*gain*(32767.0f/32768.0f);
        assert(fabsf(output[2*i]-expected)<0.000001f);
        assert(fabsf(output[2*i+1]-expected)<0.000001f);
    }
}
static void sample_values(const float *output, const unsigned values[4]) {
    for (unsigned i=0;i<4;++i) {
        float expected=(float)values[i]/128.0f*(32767.0f/32768.0f);
        assert(fabsf(output[2*i]-expected)<0.000001f);
        assert(fabsf(output[2*i+1]-expected)<0.000001f);
    }
}

static void test_output_routing_and_clock(void) {
    Fixture f; fixture_init(&f);
    waveform(&f);
    render(&f);
    silent(f.output[0]); silent(f.output[1]); /* Submitting data is not PLAY. */
    event(f.source,0);
    render(&f);
    silent(f.output[0]); samples(f.output[1],1,1.0f);
    assert(system_render(f.system,NULL,0)==0); /* No output buffers must not end playback. */

    /* Two output destinations receive the same time slice. The second output must not consume
     * the next four samples, and the next render must continue exactly where this one ended. */
    patch(f.source,1,f.master[0]); volume(f.source,1,0.5f);
    render(&f);
    samples(f.output[0],5,0.5f); samples(f.output[1],5,1.0f);
    render(&f);
    samples(f.output[0],9,0.5f); samples(f.output[1],9,1.0f);

    event(f.source,4); /* PAUSE */
    render(&f); silent(f.output[0]); silent(f.output[1]);
    event(f.source,5); /* RESUME */
    render(&f);
    samples(f.output[0],13,0.5f); samples(f.output[1],13,1.0f);
    event(f.source,1); /* STOP */
    render(&f); silent(f.output[0]); silent(f.output[1]);
    fixture_destroy(&f);
}

static void test_play_before_waveform(void) {
    Fixture f; fixture_init(&f);
    event(f.source,0);
    waveform(&f);
    render(&f);
    silent(f.output[0]); samples(f.output[1],1,1.0f);
    event(f.source,3); /* KILL */
    render(&f); silent(f.output[0]); silent(f.output[1]);
    fixture_destroy(&f);
}

static void test_repeat_sample_windows(void) {
    Fixture f; fixture_init(&f);
    WaveformBlock block={.data_size=3,.repeats=1,.skip_samples=1,.samples=6};
    submit(&f,&block);
    /* The loop window and repeat count must survive reuse of Scream's temporary descriptors. */
    memset(&block,0,sizeof(block));
    event(f.source,0);
    const unsigned expected[3][4]={{2,3,4,5},{6,7,2,3},{4,5,6,7}};
    for (unsigned i=0;i<3;++i) {
        render(&f); silent(f.output[0]); sample_values(f.output[1],expected[i]);
    }
    render(&f); silent(f.output[0]); silent(f.output[1]);

    block=(WaveformBlock){.data_size=2,.repeats=UINT32_MAX,.skip_samples=1,.samples=3};
    submit(&f,&block);
    event(f.source,0);
    render(&f); sample_values(f.output[1],(const unsigned[4]){2,3,4,2});
    render(&f); sample_values(f.output[1],(const unsigned[4]){3,4,2,3});
    fixture_destroy(&f);
}

static void pcm_setup(Fixture *f, uint32_t type, uint32_t channels, uint32_t rate) {
    struct { VoiceParamHeader header; WaveformFormat format; uint32_t flags, reserved; }
        setup={{40,0,0x40010000},{type,channels,rate,0,0,0},0,0};
    assert(voice_control(f->source,&setup)==0);
}
static void pitch(void *voice, float ratio) {
    struct { VoiceParamHeader header; float ratio; uint32_t reserved; }
        param={{16,0,0x40010005},ratio,0};
    assert(voice_control(voice,&param)==0);
}
static void put_pcm(Fixture *f, uint32_t type, const float *values, unsigned count) {
    int short_sample=type==WAVEFORM_PCM_I16_LE || type==WAVEFORM_PCM_I16_BE;
    int big_endian=type==WAVEFORM_PCM_I16_BE || type==WAVEFORM_PCM_F32_BE;
    unsigned bytes=short_sample ? 2 : 4;
    assert(count*bytes<=sizeof(f->data));
    for (unsigned i=0;i<count;++i) {
        uint32_t bits;
        if (short_sample) bits=(uint16_t)(int16_t)(values[i]*32768.0f);
        else memcpy(&bits,&values[i],sizeof(bits));
        for (unsigned b=0;b<bytes;++b)
            f->data[i*bytes+b]=(unsigned char)(bits>>((big_endian ? bytes-1-b : b)*8));
    }
}
static void check_pcm(const float *output, const float *expected, unsigned count) {
    for (unsigned i=0;i<count;++i)
        assert(fabsf(output[i]-expected[i]*(32767.0f/32768.0f))<0.000002f);
}

static void test_pcm_formats_and_routing(void) {
    const uint32_t types[]={WAVEFORM_PCM_I16_LE,WAVEFORM_PCM_I16_BE,
                            WAVEFORM_PCM_F32_LE,WAVEFORM_PCM_F32_BE};
    const float stereo[]={0.125f,-0.25f,0.375f,-0.5f,0.625f,-0.75f,0.875f,-1.0f};
    for (unsigned t=0;t<4;++t) {
        Fixture f; fixture_init(&f);
        pcm_setup(&f,types[t],2,48000);
        put_pcm(&f,types[t],stereo,8);
        WaveformBlock block={.data_size=t<2 ? 16 : 32,.samples=4};
        submit(&f,&block); event(f.source,0);
        render(&f); silent(f.output[0]); check_pcm(f.output[1],stereo,8);
        render(&f); silent(f.output[1]);
        fixture_destroy(&f);
    }
    Fixture f; fixture_init(&f);
    pcm_setup(&f,WAVEFORM_PCM_I16_LE,1,48000);
    const float mono[]={0.125f,0.25f,0.375f,0.5f};
    put_pcm(&f,WAVEFORM_PCM_I16_LE,mono,4);
    WaveformBlock block={.data_size=8,.samples=4};
    submit(&f,&block); event(f.source,0);
    patch(f.source,1,f.master[0]); volume(f.source,1,0.5f);
    render(&f);
    for (unsigned i=0;i<4;++i) {
        float expected[]={mono[i],mono[i]};
        check_pcm(f.output[1]+2*i,expected,2);
        expected[0]*=0.5f; expected[1]*=0.5f;
        check_pcm(f.output[0]+2*i,expected,2);
    }
    fixture_destroy(&f);
}

static void test_pcm_windows_pause_and_partial_frames(void) {
    Fixture f; fixture_init(&f);
    pcm_setup(&f,WAVEFORM_PCM_I16_LE,1,48000);
    const float mono[]={0.125f,0.25f,0.375f,0.5f,0.625f};
    put_pcm(&f,WAVEFORM_PCM_I16_LE,mono,5);
    WaveformBlock block={.data_offset=2,.data_size=8,.skip_samples=1,.samples=2,.repeats=1};
    submit(&f,&block); event(f.source,0);
    event(f.source,4); render(&f); silent(f.output[1]);
    event(f.source,5); render(&f);
    const float expected[]={0.375f,0.375f,0.5f,0.5f,0.375f,0.375f,0.5f,0.5f};
    check_pcm(f.output[1],expected,8);
    render(&f); silent(f.output[1]);

    pcm_setup(&f,WAVEFORM_PCM_I16_LE,2,48000);
    block=(WaveformBlock){.data_size=3,.repeats=UINT32_MAX};
    submit(&f,&block); event(f.source,0);
    render(&f); silent(f.output[1]); /* No complete stereo frame, including an infinite loop. */
    uint32_t flags; assert(voice_get_state_flags(f.source,&flags)==0);
    assert(flags&VOICE_STATE_EMPTY);
    pcm_setup(&f,WAVEFORM_PCM_I16_LE,1,48000);
    block=(WaveformBlock){.data_size=8,.skip_samples=4,.repeats=UINT32_MAX};
    submit(&f,&block); event(f.source,0);
    render(&f); silent(f.output[1]); /* An empty skip window cannot repeat forever either. */
    fixture_destroy(&f);
}

static void test_pcm_resampling_across_grains(void) {
    Fixture f; fixture_init(&f);
    /* Scream submits 44.1 kHz effects as 48 kHz PCM with a 44100/48000 pitch ratio. */
    pcm_setup(&f,WAVEFORM_PCM_I16_LE,1,48000);
    float mono[32];
    for (unsigned i=0;i<32;++i) mono[i]=(float)i/64.0f;
    put_pcm(&f,WAVEFORM_PCM_I16_LE,mono,32);
    WaveformBlock block={.data_size=64,.samples=32};
    submit(&f,&block); pitch(f.source,0.91875f); event(f.source,0);
    for (unsigned grain=0;grain<4;++grain) {
        render(&f);
        for (unsigned i=0;i<4;++i) {
            float value=(float)((grain*4+i)*(double)0.91875f/64.0);
            float expected[]={value,value}; check_pcm(f.output[1]+2*i,expected,2);
        }
        if (grain==1) {
            event(f.source,4); render(&f); silent(f.output[1]); event(f.source,5);
        }
    }
    /* A non-default system clock uses the same conversion without a pitch parameter. */
    ((System *)f.system)->sample_rate=32000;
    event(f.source,1); pitch(f.source,1.0f); submit(&f,&block); event(f.source,0);
    render(&f);
    for (unsigned i=0;i<4;++i) {
        float value=(float)i*1.5f/64.0f;
        float expected[]={value,value}; check_pcm(f.output[1]+2*i,expected,2);
    }
    fixture_destroy(&f);
}

static void test_stopped_effects_return_to_pool(void) {
    Fixture f; fixture_init(&f);
    pcm_setup(&f,WAVEFORM_PCM_I16_LE,1,48000);
    const float mono[]={0.125f,0.25f,0.375f,0.5f};
    put_pcm(&f,WAVEFORM_PCM_I16_LE,mono,4);
    WaveformBlock block={.data_size=8,.samples=4,.repeats=UINT32_MAX};
    uint32_t flags;
    for (unsigned i=0;i<120;++i) {
        /* Scream's pool scans for state zero or EMPTY before starting another effect. */
        if (i) {
            assert(voice_get_state_flags(f.source,&flags)==0);
            assert(flags==0 || (flags&VOICE_STATE_EMPTY));
        }
        submit(&f,&block); event(f.source,0);
        render(&f); assert(f.output[1][0]>0.0f);
        event(f.source,1+i%3); /* STOP, STOP_IMMEDIATE, KILL */
        render(&f); silent(f.output[1]);
    }
    fixture_destroy(&f);
}

/* RFD supplies a logical sample window and then APPENDs its payload. Its block-end callback
 * writes new PCM into the completed staging slot and resubmits it through VoiceControl. */
typedef struct {
    Fixture f;
    int16_t staging[3][4];
    unsigned next[3], callbacks, limit, frames;
    int action;
} Stream;
static void stream_register(Stream *s);

static int32_t stream_blocks(Stream *s, const void *data, uint32_t flags,
                              uint32_t count, const WaveformBlock *blocks) {
    struct { VoiceParamHeader header; const void *data; uint32_t flags, count;
             const WaveformBlock *blocks; }
        param={{32,0,0x10000001},data,flags,count,blocks};
    return voice_control(s->f.source,&param);
}
static void stream_submit(Stream *s, unsigned slot, unsigned sequence) {
    for (unsigned i=0;i<s->frames;++i) s->staging[slot][i]=(int16_t)((sequence+1)*128);
    WaveformBlock window={.samples=s->frames};
    assert(stream_blocks(s,s->staging[slot],BLOCK_CONTINUE,1,&window)==0);
    WaveformBlock bytes={.data_size=s->frames*2,.user_data=slot};
    assert(stream_blocks(s,s->staging[slot],BLOCK_CONTINUE|BLOCK_APPEND|BLOCK_SILENCE,1,&bytes)==0);
    /* Neither descriptor may be retained by address. */
    memset(&window,0xcc,sizeof(window)); memset(&bytes,0xdd,sizeof(bytes));
}
static ABI void stream_callback(const VoiceCallbackInfo *info) {
    Stream *s=(Stream *)info->callback_data;
    assert(info->voice==s->f.source && info->flag==CALLBACK_BLOCK_END);
    assert(info->data_size==s->frames*2 && info->reserved==0 && info->reserved2==0);
    assert(info->attributes==0 && info->repeated==0 && info->user_data<3);
    unsigned slot=(unsigned)info->user_data;
    assert(info->data==s->staging[slot]);
    assert(slot==s->callbacks%3);
    ++s->callbacks;
    if (s->action==1) { /* Stop must invalidate the remaining events captured in this grain. */
        event(s->f.source,EVENT_STOP); return;
    }
    if (s->action==2) {
        s->action=0;
        WaveformBlock reset={.data_size=s->frames*2,.samples=s->frames,.user_data=0};
        assert(stream_blocks(s,s->staging[0],BLOCK_RESET,1,&reset)==0);
        return;
    }
    if (s->action==3 || s->action==4) {
        void *pool=s->f.samplers;
        assert(rack_destroy(pool,NULL)==0);
        if (s->action==3) {
            free(pool); s->f.samplers=NULL;
        } else {
            unsigned char option[128]={0}; uint64_t size=128;
            uint32_t count=1,grain=512,ports=8;
            memcpy(option,&size,8); memcpy(option+28,&grain,4);
            memcpy(option+32,&count,4); memcpy(option+44,&ports,4);
            ContextBufferInfo info={.host_buffer=pool,.host_buffer_size=sizeof(Rack)+sizeof(Voice)};
            assert(rack_pool_create(s->f.system,RACK_CUSTOM_SAMPLER,option,&info,&s->f.samplers)==0);
            assert(rack_voice_handle(s->f.samplers,0,&s->f.source)==0);
            pcm_setup(&s->f,WAVEFORM_PCM_I16_LE,1,48000);
            pcm_setup(&s->f,WAVEFORM_PCM_I16_LE,1,48000);
            stream_register(s); /* Deliberately matches the old voice's generation. */
            s->action=0;
        }
        return;
    }
    if (s->callbacks<s->limit) {
        stream_submit(s,slot,s->next[slot]);
        s->next[slot]+=3;
    }
}
static void stream_register(Stream *s) {
    struct { VoiceParamHeader header; VoiceCallback callback; uintptr_t data;
             uint32_t flags, reserved; }
        param={{32,0,7},stream_callback,(uintptr_t)s,CALLBACK_BLOCK_END,0};
    assert(voice_control(s->f.source,&param)==0);
}
static void stream_init(Stream *s, unsigned frames) {
    memset(s,0,sizeof(*s)); fixture_init(&s->f); s->frames=frames;
    pcm_setup(&s->f,WAVEFORM_PCM_I16_LE,1,48000);
    stream_register(s);
    for (unsigned i=0;i<3;++i) { stream_submit(s,i,i); s->next[i]=i+3; }
}

static void test_stream_refill_order_and_abi(void) {
    Stream s; stream_init(&s,4); s.limit=1000;
    event(s.f.source,EVENT_PLAY);
    for (unsigned grain=0;grain<96;++grain) {
        render(&s.f); silent(s.f.output[0]);
        float expected[8];
        for (unsigned i=0;i<8;++i) expected[i]=(float)(grain+1)/256.0f;
        check_pcm(s.f.output[1],expected,8);
        assert(s.callbacks==grain+1);
    }
    event(s.f.source,EVENT_STOP);
    render(&s.f); silent(s.f.output[1]); assert(s.callbacks==96);
    fixture_destroy(&s.f);
}
static void test_stream_reset_stop_and_invalid_calls(void) {
    Stream s; stream_init(&s,1); s.action=1;
    event(s.f.source,EVENT_PLAY);
    render(&s.f);
    /* All three complete in this grain, but the first callback's STOP suppresses stale events. */
    assert(s.callbacks==1);
    render(&s.f); silent(s.f.output[1]); assert(s.callbacks==1);
    fixture_destroy(&s.f);

    stream_init(&s,1); s.action=3;
    event(s.f.source,EVENT_PLAY); render(&s.f); assert(s.callbacks==1);
    fixture_destroy(&s.f); /* Other two callbacks refer to genuinely freed pool memory. */

    stream_init(&s,1); s.action=4;
    void *reused_pool=s.f.samplers;
    event(s.f.source,EVENT_PLAY); render(&s.f); assert(s.callbacks==1);
    fixture_destroy(&s.f); free(reused_pool); /* Same address/generation, new rack identity. */

    stream_init(&s,1); s.action=2;
    event(s.f.source,EVENT_PLAY); render(&s.f); assert(s.callbacks==1);
    /* The callback reset discarded slots1/2. Its new slot0 has exactly one sample. */
    s.callbacks=0;
    render(&s.f);
    check_pcm(s.f.output[1],(const float[8]){1.0f/256,1.0f/256,0,0,0,0,0,0},8);
    assert(s.callbacks==1);
    render(&s.f); silent(s.f.output[1]);
    fixture_destroy(&s.f);

    stream_init(&s,4);
    assert(voice_control(s.f.source,NULL)==ERR_INVALID_PARAM_ADDRESS);
    assert(stream_blocks(&s,NULL,BLOCK_RESET,1,NULL)==ERR_INVALID_BLOCK_ADDRESS);
    WaveformBlock bytes={.data_size=8,.samples=4};
    assert(stream_blocks(&s,NULL,BLOCK_RESET,1,&bytes)==ERR_INVALID_BUFFER_ADDRESS);
    assert(stream_blocks(&s,s.staging[0],BLOCK_RESET,MAX_BLOCKS+1,&bytes)==ERR_INVALID_BLOCK_COUNT);
    struct { VoiceParamHeader header; VoiceCallback callback; uintptr_t data;
             uint32_t flags, reserved; }
        invalid={{32,0,7},stream_callback,(uintptr_t)&s,0x80,0};
    assert(voice_control(s.f.source,&invalid)==ERR_INVALID_CALLBACK_FLAG);
    invalid.header.size=8;
    assert(voice_control(s.f.source,&invalid)==ERR_INVALID_PARAM_SIZE);
    event(s.f.source,EVENT_PLAY);
    /* Rejected reset requests left the original queue and callback registration intact. */
    for (unsigned i=0;i<3;++i) {
        render(&s.f);
        for (unsigned j=0;j<8;++j) {
            float expected=(float)(i+1)/256*(32767.0f/32768.0f);
            assert(fabsf(s.f.output[1][j]-expected)<0.000002f);
        }
    }
    assert(s.callbacks==3);
    fixture_destroy(&s.f);
}

static void test_caller_owned_pools_and_rack_info(void) {
    ContextBufferInfo system_info={.user_data=0x1234},returned;
    assert(system_query_buffer(NULL,&system_info)==0);
    assert(!system_info.host_buffer && system_info.host_buffer_size>=sizeof(System));
    assert(system_info.user_data==0x1234);
    system_info.host_buffer=malloc(system_info.host_buffer_size+16);
    assert(system_info.host_buffer);
    memset(system_info.host_buffer,0xa5,system_info.host_buffer_size+16);
    void *system=NULL,*rack=NULL,*voice=NULL;
    assert(system_pool_create(NULL,&system_info,&system)==0);
    assert(system==system_info.host_buffer);
    unsigned char option[168]={0};
    uint64_t size=sizeof(option); uint32_t grain=1024,count=3,matrices=16,ports=8;
    memcpy(option,&size,8); memcpy(option+8,"owned-pool",11);
    memcpy(option+28,&grain,4); memcpy(option+32,&count,4);
    memcpy(option+40,&matrices,4); memcpy(option+44,&ports,4);
    ContextBufferInfo rack_info={.user_data=0x5678};
    assert(rack_query_buffer(RACK_SAMPLER,option,&rack_info)==0);
    rack_info.host_buffer=malloc(rack_info.host_buffer_size+16); assert(rack_info.host_buffer);
    memset(rack_info.host_buffer,0xa5,rack_info.host_buffer_size+16);
    ContextBufferInfo short_pool=rack_info; --short_pool.host_buffer_size;
    assert(rack_pool_create(system,RACK_SAMPLER,option,&short_pool,&rack)==(int32_t)0x804A0209);
    short_pool=rack_info; short_pool.host_buffer=(char *)rack_info.host_buffer+1;
    assert(rack_pool_create(system,RACK_SAMPLER,option,&short_pool,&rack)==(int32_t)0x804A0208);
    assert(rack_pool_create(system,RACK_SAMPLER,option,&rack_info,&rack)==0);
    assert(rack==rack_info.host_buffer);
    unsigned char result[184]; memset(result,0xa5,sizeof(result));
    assert(rack_get_info(rack,(RackInfo *)result,167)==ERR_INVALID_OUT_SIZE);
    assert(result[0]==0xa5);
    assert(rack_get_info(rack,(RackInfo *)result,sizeof(result))==0);
    RackInfo info; memcpy(&info,result,sizeof(info));
    assert(info.handle==rack && info.system==system && info.voices==3);
    assert(info.max_grain==1024 && info.matrices==16 && info.ports==8 && info.type==0);
    assert(!strcmp(info.name,"owned-pool") && info.context.user_data==0x5678);
    for (unsigned i=168;i<sizeof(result);++i) assert(result[i]==0xa5);
    assert(rack_voice_handle(rack,2,&voice)==0);
    assert(rack_voice_handle(rack,3,&voice)==ERR_INVALID_VOICE_INDEX);
    assert(rack_destroy(rack,&returned)==0);
    assert(returned.host_buffer==rack_info.host_buffer && returned.host_buffer_size==rack_info.host_buffer_size);
    assert(returned.user_data==0x5678);
    for (unsigned i=0;i<16;++i)
        assert(((unsigned char *)rack_info.host_buffer)[rack_info.host_buffer_size+i]==0xa5);
    assert(system_destroy(system,&returned)==0);
    assert(returned.host_buffer==system_info.host_buffer && returned.user_data==0x1234);
    for (unsigned i=0;i<16;++i)
        assert(((unsigned char *)system_info.host_buffer)[system_info.host_buffer_size+i]==0xa5);
    free(rack_info.host_buffer); free(system_info.host_buffer);
}
static void test_render_grain_capacity_and_atomic_rejection(void) {
    void *system=NULL;
    assert(system_create(NULL,NULL,&system)==0);
    unsigned char output[0x8000]; memset(output,0xa5,sizeof(output));
    RenderBufferInfo buffer={output,sizeof(output),WAVEFORM_PCM_F32_LE,8};
    assert(system_render(system,&buffer,1)==0);
    /* This is the shipped RVD capacity/type/channel combination. NULL options configure256
     * samples, not the1024 frames that fit the advertised capacity. The tail holds game data. */
    for (unsigned i=0;i<0x2000;++i) assert(output[i]==0);
    for (unsigned i=0x2000;i<sizeof(output);++i) assert(output[i]==0xa5);
    assert(system_destroy(system,NULL)==0);

    unsigned char option[64]={0}; uint64_t size=64;
    uint32_t max_grain=512,grain=64,rate=48000;
    memcpy(option,&size,8); memcpy(option+28,&max_grain,4);
    memcpy(option+32,&grain,4); memcpy(option+36,&rate,4);
    assert(system_create(option,NULL,&system)==0);
    memset(output,0xa5,sizeof(output));
    buffer=(RenderBufferInfo){output,sizeof(output),WAVEFORM_PCM_I16_LE,2};
    assert(system_render(system,&buffer,1)==0);
    for (unsigned i=0;i<64*2*2;++i) assert(output[i]==0);
    for (unsigned i=64*2*2;i<sizeof(output);++i) assert(output[i]==0xa5);
    assert(system_destroy(system,NULL)==0);

    Fixture f; fixture_init(&f); waveform(&f); event(f.source,EVENT_PLAY);
    float oversized[2][32]; memset(oversized,0xa5,sizeof(oversized));
    f.buffers[0]=(RenderBufferInfo){oversized[0],sizeof(oversized[0]),WAVEFORM_PCM_F32_LE,2};
    f.buffers[1]=(RenderBufferInfo){oversized[1],31,WAVEFORM_PCM_F32_LE,2};
    assert(system_render(f.system,f.buffers,2)==(int32_t)0x804A0209);
    /* The second output failed validation. The first remains untouched and no voice advanced. */
    for (unsigned i=0;i<sizeof(oversized);++i) assert(((unsigned char *)oversized)[i]==0xa5);
    f.buffers[1].buffer_size=sizeof(oversized[1]);
    assert(system_render(f.system,f.buffers,2)==0);
    samples(oversized[1],1,1.0f);
    assert(system_render(f.system,f.buffers,2)==0);
    samples(oversized[1],5,1.0f);
    for (unsigned out=0;out<2;++out)
        for (unsigned i=8*sizeof(float);i<sizeof(oversized[0]);++i)
            assert(((unsigned char *)oversized[out])[i]==0xa5);
    fixture_destroy(&f);
}
typedef struct { const unsigned char *data; size_t size; int error,short_read; unsigned calls; } Reader;
static ABI int32_t read_waveform(uintptr_t user,uint32_t offset,void *data,size_t size) {
    Reader *reader=(Reader *)user; ++reader->calls;
    if (reader->error) return reader->error;
    if (offset>reader->size || size>reader->size-offset) return -123;
    memcpy(data,reader->data+offset,size);
    return (int32_t)size-reader->short_read;
}
static void put32(unsigned char *p,uint32_t value) {
    for (unsigned i=0;i<4;++i) p[i]=(unsigned char)(value>>(8*i));
}
static unsigned add_chunk(unsigned char *data,unsigned offset,const char *tag,
                            const void *payload,unsigned size) {
    memcpy(data+offset,tag,4); put32(data+offset+4,size);
    memcpy(data+offset+8,payload,size); data[offset+8+size]=0;
    return offset+8+size+(size&1);
}
static void test_waveform_reader_loops_and_seek(void) {
    unsigned char data[256]={0},fmt[16]={1,0,2,0},smpl[60]={0},pcm[16]={0};
    put32(fmt+4,48000); fmt[12]=4; fmt[14]=16;
    put32(smpl+28,1); put32(smpl+44,1); put32(smpl+48,2);
    memcpy(data,"RIFF",4); memcpy(data+8,"WAVE",4);
    unsigned end=add_chunk(data,12,"JUNK","x",1);
    unsigned fmt_offset=end+8;
    end=add_chunk(data,end,"fmt ",fmt,sizeof(fmt));
    unsigned smpl_offset=end+8;
    end=add_chunk(data,end,"smpl",smpl,sizeof(smpl));
    unsigned data_offset=end+8;
    end=add_chunk(data,end,"data",pcm,sizeof(pcm)); put32(data+4,end-8);
    Reader reader={data,end,0,0,0}; WaveformInfo info;
    assert(parse_waveform_user(read_waveform,(uintptr_t)&reader,&info)==0);
    assert(reader.calls>1 && info.format.type==WAVEFORM_PCM_I16_LE && info.format.channels==2);
    assert(info.data_offset==data_offset && info.data_size==16 && info.samples==4);
    assert(info.block_count==3 && info.loop_begin==1 && info.loop_end==3);
    assert(info.blocks[0].samples==1 && info.blocks[0].data_offset==data_offset);
    assert(info.blocks[1].samples==2 && info.blocks[1].repeats==UINT32_MAX);
    assert(info.blocks[1].data_offset==data_offset+4 && info.blocks[2].data_offset==data_offset+12);
    WaveformInfo before=info;
    reader.error=-321; assert(parse_waveform_user(read_waveform,(uintptr_t)&reader,&info)==-321);
    assert(!memcmp(&before,&info,sizeof(info))); reader.error=0;
    reader.short_read=1;
    assert(parse_waveform_user(read_waveform,(uintptr_t)&reader,&info)==(int32_t)0x804A0408);
    reader.short_read=0;
    data[fmt_offset]=9;
    assert(parse_waveform_user(read_waveform,(uintptr_t)&reader,&info)==ERR_INVALID_WAVEFORM_TYPE);
    data[fmt_offset]=1; put32(data+smpl_offset+48,99);
    assert(parse_waveform_user(read_waveform,(uintptr_t)&reader,&info)==(int32_t)0x804A0408);
    assert(!memcmp(&before,&info,sizeof(info)));
    reader.size=15;
    assert(parse_waveform_user(read_waveform,(uintptr_t)&reader,&info)==-123);

    WaveformFormat format={WAVEFORM_PCM_I16_LE,2,48000,0,0,0}; WaveformBlock block;
    assert(calc_waveform_block(&format,3,7,&block)==0);
    assert(block.data_offset==12 && block.data_size==28 && block.samples==7 && !block.skip_samples);
    assert(calc_waveform_block(&format,UINT32_MAX,7,&block)==(int32_t)0x804A0407);
    format=(WaveformFormat){WAVEFORM_ATRAC9,2,48000,0xfe740bf0,0,0};
    assert(calc_waveform_block(&format,17,4,&block)==0);
    assert(block.data_offset==4 && block.data_size==4 && block.skip_samples==5 && block.samples==4);
    format.config=0xfe7e0000;
    assert(calc_waveform_block(&format,0,4,&block)==(int32_t)0x804A0403);
}

#ifdef NGS2_REAL_ATRAC9
static void test_real_six_channel_atrac9(const char *path) {
    FILE *file=fopen(path,"rb"); assert(file);
    assert(fseek(file,0,SEEK_END)==0); long length=ftell(file); assert(length>0);
    rewind(file); unsigned char *data=malloc((size_t)length); assert(data);
    assert(fread(data,1,(size_t)length,file)==(size_t)length); fclose(file);
    Reader reader={data,(size_t)length,0,0,0}; WaveformInfo info;
    assert(parse_waveform_user(read_waveform,(uintptr_t)&reader,&info)==0);
    assert(info.format.type==WAVEFORM_ATRAC9 && info.format.channels==6 && info.format.sample_rate==48000);
    assert(info.format.config==0xfe7618f0 && info.block_count==1 && info.blocks[0].skip_samples==256);
    WaveformBlock seek;
    assert(calc_waveform_block(&info.format,1025,1000,&seek)==0);
    assert(seek.data_offset==800 && seek.data_size==1600 && seek.skip_samples==257);
    Fixture f; fixture_init(&f);
    ((System *)f.system)->grain=((System *)f.system)->max_grain=1024;
    struct { VoiceParamHeader header; WaveformFormat format; uint32_t flags,reserved; }
        setup={{40,0,0x40010000},info.format,0,0};
    assert(voice_control(f.source,&setup)==0);
    struct { VoiceParamHeader header; const void *data; uint32_t flags,count; const WaveformBlock *blocks; }
        blocks={{32,0,0x40010001},data,0,1,info.blocks};
    assert(voice_control(f.source,&blocks)==0); event(f.source,EVENT_PLAY);
    float output[2][1024*8];
    for (unsigned i=0;i<2;++i) f.buffers[i]=(RenderBufferInfo){output[i],sizeof(output[i]),WAVEFORM_PCM_F32_LE,8};
    /* Independent direct decoder reference checks actual six-channel values and delay, beyond
     * just observing a PLAY flag or a nonzero buffer. No encoded game bytes enter the source. */
    void *decoder=Atrac9GetHandle(); assert(decoder);
    unsigned char config[]={0xfe,0x76,0x18,0xf0}; Atrac9CodecInfo codec;
    assert(Atrac9InitDecoder(decoder,config)==0 && Atrac9GetCodecInfo(decoder,&codec)==0);
    float reference[1536*6]; unsigned offset=info.data_offset,frame=0,sf_bytes=0,samples=0;
    while (samples<1280) {
        int used=0;
        assert(Atrac9DecodeF32(decoder,data+offset,(int)(length-offset),reference+samples*6,&used,0)==0);
        assert(used>0); offset+=(unsigned)used; sf_bytes+=(unsigned)used; samples+=256;
        if (++frame==4) { offset+=800-sf_bytes; frame=sf_bytes=0; }
    }
    assert(system_render(f.system,f.buffers,2)==0);
    for (unsigned i=0;i<1024;++i) for (unsigned c=0;c<8;++c) {
        float expected=c<6 ? reference[(i+256)*6+c]*(32767.0f/32768.0f) : 0;
        assert(fabsf(output[1][i*8+c]-expected)<0.000002f);
        assert(output[0][i*8+c]==0);
    }
    double energy=0; float peak=0;
    for (unsigned grain=0;grain<64;++grain) {
        if (grain) assert(system_render(f.system,f.buffers,2)==0);
        for (unsigned i=0;i<1024*8;++i) {
            float value=output[1][i]; assert(isfinite(value)); energy+=(double)value*value;
            if (fabsf(value)>peak) peak=fabsf(value);
        }
    }
    assert(peak>0.001f && energy>0.001);
    printf("Real 6ch ATRAC9: peak=%.7f RMS=%.7f, metadata/read/seek/sample reference passed\n",
           peak,sqrt(energy/(64*1024*8)));
    Atrac9ReleaseHandle(decoder); fixture_destroy(&f); free(data);
}
#endif

int main(int argc,char **argv) {
    alarm(20); /* A callback accidentally invoked under the mixer lock must fail boundedly. */
#ifdef NGS2_REAL_ATRAC9
    if (argc!=2) { fputs("Supply the private six-channel BGM AT9 fixture path\n",stderr); return 2; }
    test_real_six_channel_atrac9(argv[1]); alarm(0); return 0;
#else
    (void)argc; (void)argv;
#endif
    test_output_routing_and_clock();
    test_play_before_waveform();
    test_repeat_sample_windows();
    test_pcm_formats_and_routing();
    test_pcm_windows_pause_and_partial_frames();
    test_pcm_resampling_across_grains();
    test_stopped_effects_return_to_pool();
    test_stream_refill_order_and_abi();
    test_stream_reset_stop_and_invalid_calls();
    test_caller_owned_pools_and_rack_info();
    test_render_grain_capacity_and_atomic_rejection();
    test_waveform_reader_loops_and_seek();
#ifdef TEMPEST_FEX
    assert(guest_callback_calls>=96);
#endif
    alarm(0);
    puts("NGS2 routing, clock, PCM/ATRAC9, streaming order and guest callbacks passed");
    return 0;
}
