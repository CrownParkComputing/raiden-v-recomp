/* Real compressed ATRAC9 input, not a mocked decoder. The small original test
 * stream encodes constant scale factors and one nonzero MDCT coefficient per
 * mono block. Four codec frames share a fixed-size superframe with end padding.
 * Build this file with the bundled LibAtrac9 C sources, pthreads and libm. */
#undef NDEBUG
#include "../src/runtime_audiodec.c"
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#ifndef _WIN32
#include <sys/mman.h>
#include <unistd.h>
#endif

typedef int32_t (ABI *AdLibraryFn)(uint32_t);
typedef int32_t (ABI *AdCreateFn)(void *,uint32_t);
typedef int32_t (ABI *AdHandleFn)(int32_t);
typedef int32_t (ABI *AdDecodeFn)(int32_t,void *);
static AdLibraryFn init_api,term_api;
static AdCreateFn create_api;
static AdHandleFn clear_api,delete_api;
static AdDecodeFn decode_api;

static void put_bits(uint8_t *data,unsigned *bit,unsigned value,unsigned width) {
    while (width--) {
        data[*bit/8]|=((value>>width)&1u)<<(7-*bit%8);
        ++*bit;
    }
}
static unsigned encoded_block(uint8_t *data,int first,int coefficient) {
    unsigned bit=0;
    put_bits(data,&bit,!first,1); /* first-in-superframe is active low */
    put_bits(data,&bit,0,1);     /* independent band parameters */
    put_bits(data,&bit,0,4);     /* three bands / ten quantization units */
    put_bits(data,&bit,0,1);     /* no band extension */
    put_bits(data,&bit,0,2);     /* explicit gradient */
    put_bits(data,&bit,0,6); put_bits(data,&bit,0,6);
    put_bits(data,&bit,15,5); put_bits(data,&bit,15,5);
    put_bits(data,&bit,0,4); put_bits(data,&bit,0,1);
    put_bits(data,&bit,1,2); put_bits(data,&bit,3,2); /* five-bit constant factors */
    for (unsigned q=0;q<10;++q) put_bits(data,&bit,30,5);
    for (unsigned q=0;q<24;++q)
        put_bits(data,&bit,q ? 0u : (uint16_t)coefficient,16);
    assert((bit+7)/8==60);
    return (bit+7)/8;
}
typedef struct {
    AdParam param;
    AdInfo info;
    AdBuffer input,output;
    AdControl ctrl;
    uint8_t stream[1024];
    union { int16_t s16[2048]; float f32[2048]; } pcm;
    uint32_t superframe_bytes,frame_input_bytes,frame_output_bytes;
} Fixture;
static void fixture(Fixture *f,int channels,int word_length) {
    memset(f,0,sizeof(*f));
    const unsigned frame_bytes=64u*(unsigned)channels;
    const uint32_t config=0xfe700000u|((unsigned)(channels-1)<<17)|
                          ((frame_bytes-1)<<5)|(2u<<3);
    f->param=(AdParam){12,word_length,{config>>24,config>>16,config>>8,config}};
    f->info.size=36;
    f->superframe_bytes=frame_bytes*4;
    f->frame_input_bytes=60u*(unsigned)channels;
    f->frame_output_bytes=256u*(unsigned)channels*(word_length==1 ? 2u : 4u);
    for (unsigned sf=0;sf<2;++sf) {
        unsigned offset=sf*f->superframe_bytes;
        for (unsigned frame=0;frame<4;++frame)
            for (int ch=0;ch<channels;++ch)
                offset+=encoded_block(f->stream+offset,frame==0,ch ? -600 : 1000);
        memset(f->stream+offset,0xa5,(sf+1)*f->superframe_bytes-offset);
    }
    f->input=(AdBuffer){24,f->stream,f->superframe_bytes*2};
    f->output=(AdBuffer){24,&f->pcm,f->frame_output_bytes};
    f->ctrl=(AdControl){&f->param,&f->info,&f->input,&f->output};
}
static void buffers(Fixture *f,uint32_t offset,uint32_t size) {
    f->input.data=f->stream+offset;
    f->input.bytes=size;
    f->output.bytes=f->frame_output_bytes;
    memset(&f->pcm,0x5a,sizeof(f->pcm));
}

static void lifecycle_and_errors(void) {
    Fixture f; fixture(&f,1,1);
    assert(create_api(&f.ctrl,1)==AD_ARGUMENT);
    assert(init_api(99)==AD_INVALID_TYPE);
    assert(init_api(1)==0);
    assert(create_api(NULL,1)==AD_CONTROL_POINTER);
    f.param.size=11; assert(create_api(&f.ctrl,1)==AD_PARAM_SIZE); f.param.size=12;
    f.info.size=35; assert(create_api(&f.ctrl,1)==AD_INFO_SIZE); f.info.size=36;
    f.param.word_length=0; assert(create_api(&f.ctrl,1)==AD_WORD_LENGTH); f.param.word_length=1;
    f.param.config[1]=0x7e; assert(create_api(&f.ctrl,1)==AD_CONFIG); f.param.config[1]=0x70;
    int32_t h=create_api(&f.ctrl,1); assert(h>0);
    assert(f.info.channels==1 && f.info.sample_rate==48000 && f.info.bitrate==96000);
    assert(f.info.superframe_bytes==256 && f.info.frames_in_superframe==4 && f.info.frame_samples==256);
    errno=EDOM;
    assert(clear_api(h)==0 && errno==EDOM);
    assert(delete_api(h)==0);
    assert(clear_api(h)==AD_HANDLE && delete_api(h)==AD_HANDLE);
    int32_t next=create_api(&f.ctrl,1); assert(next>0 && next!=h);
    assert(delete_api(h)==AD_HANDLE); /* stale handle must not delete a new stream */
    assert(delete_api(next)==0);
}

static void real_decode_and_reset(void) {
    Fixture f; fixture(&f,1,1);
    int32_t h=create_api(&f.ctrl,1); assert(h>0);
    uint32_t offset=0;
    int16_t first[256];
    for (unsigned frame=0;frame<8;++frame) {
        buffers(&f,offset,512-offset);
        errno=ERANGE;
        assert(decode_api(h,&f.ctrl)==0 && errno==ERANGE);
        assert(f.output.bytes==512);
        const uint32_t used=(frame&3)==3 ? 76u : 60u;
        assert(f.input.bytes==used);
        offset+=used;
        int peak=0;
        for (unsigned i=0;i<256;++i) {
            int sample=f.pcm.s16[i]; if (sample<0) sample=-sample;
            if (sample>peak) peak=sample;
        }
        assert(peak>=700); /* a successful silence stub cannot satisfy this */
        if (frame==0) memcpy(first,f.pcm.s16,sizeof(first));
        if (frame==1) assert(memcmp(first,f.pcm.s16,sizeof(first))!=0);
    }
    assert(offset==512); /* exactly two padded superframes consumed */
    assert(clear_api(h)==0);
    buffers(&f,0,512); assert(decode_api(h,&f.ctrl)==0);
    assert(memcmp(first,f.pcm.s16,sizeof(first))==0);
    /* Reset after one frame leaves a partially decoded superframe and overlap.
     * Reinitializing configuration alone would reject its first frame or alter PCM. */
    assert(clear_api(h)==0);
    buffers(&f,0,512); assert(decode_api(h,&f.ctrl)==0);
    assert(memcmp(first,f.pcm.s16,sizeof(first))==0);
    assert(clear_api(h)==0);
    buffers(&f,0,255); assert(decode_api(h,&f.ctrl)==AD_API_FAIL);
    assert(f.input.bytes==0 && f.output.bytes==0 && ((uint8_t *)&f.pcm)[0]==0x5a);
    buffers(&f,0,512); assert(decode_api(h,&f.ctrl)==0);
    assert(memcmp(first,f.pcm.s16,sizeof(first))==0);
    assert(clear_api(h)==0);
    buffers(&f,0,512); f.output.bytes=511;
    assert(decode_api(h,&f.ctrl)==AD_OUTPUT_BYTES && ((uint8_t *)&f.pcm)[0]==0x5a);
    buffers(&f,0,512); f.stream[0]=0x40; /* invalid reuse flag in first frame */
    assert(decode_api(h,&f.ctrl)==AD_API_FAIL && f.input.bytes==0 && f.output.bytes==0);
    f.stream[0]=0; buffers(&f,0,512); assert(decode_api(h,&f.ctrl)==0);
    assert(memcmp(first,f.pcm.s16,sizeof(first))==0);
    assert(delete_api(h)==0);
}

static void unaligned_stereo_float(void) {
    Fixture f; fixture(&f,2,2);
    uint8_t ctrl[40],au[32],pcm[32],info[44];
    memset(ctrl,0xa6,sizeof(ctrl)); memset(au,0xa6,sizeof(au));
    memset(pcm,0xa6,sizeof(pcm)); memset(info,0xa6,sizeof(info));
    AdControl c={&f.param,(AdInfo *)(info+4),(AdBuffer *)(au+4),(AdBuffer *)(pcm+4)};
    memcpy(info+4,&f.info,sizeof(f.info)); memcpy(au+4,&f.input,sizeof(f.input));
    memcpy(pcm+4,&f.output,sizeof(f.output)); memcpy(ctrl+4,&c,sizeof(c));
    int32_t h=create_api(ctrl+4,1); assert(h>0);
    assert(decode_api(h,ctrl+4)==0);
    AdBuffer got_in,got_out; memcpy(&got_in,au+4,24); memcpy(&got_out,pcm+4,24);
    assert(got_in.bytes==120 && got_out.bytes==2048);
    assert(got_in.data==f.input.data && got_out.data==f.output.data);
    assert(ctrl[0]==0xa6 && ctrl[39]==0xa6 && au[0]==0xa6 && au[31]==0xa6);
    assert(pcm[0]==0xa6 && pcm[31]==0xa6 && info[0]==0xa6 && info[43]==0xa6);
    int nonzero=0,different=0;
    for (unsigned i=0;i<256;++i) {
        assert(isfinite(f.pcm.f32[i*2]) && isfinite(f.pcm.f32[i*2+1]));
        nonzero|=fabsf(f.pcm.f32[i*2])>0.001f;
        different|=f.pcm.f32[i*2]!=f.pcm.f32[i*2+1];
    }
    assert(nonzero && different); assert(delete_api(h)==0);
}

static void guarded_size_records(void) {
#ifndef _WIN32
    const long page=sysconf(_SC_PAGESIZE); assert(page>0);
    uint8_t *mapping=mmap(NULL,(size_t)page*2,PROT_READ|PROT_WRITE,
                          MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(mapping!=MAP_FAILED);
    assert(!mprotect(mapping+page,(size_t)page,PROT_NONE));
    uint32_t *short_record=(uint32_t *)(mapping+page-4); *short_record=0;
    Fixture f; fixture(&f,1,1);
    AdControl c=f.ctrl; c.param=(AdParam *)short_record;
    assert(create_api(&c,1)==AD_PARAM_SIZE);
    c=f.ctrl; c.info=(AdInfo *)short_record;
    assert(create_api(&c,1)==AD_INFO_SIZE);
    assert(!munmap(mapping,(size_t)page*2));
#endif
}

enum { THREADS=8, ITERATIONS=8 };
static int32_t thread_handles[THREADS][ITERATIONS];
static void *worker(void *arg) {
    unsigned row=(unsigned)(uintptr_t)arg;
    for (unsigned j=0;j<ITERATIONS;++j) {
        Fixture f; fixture(&f,1,1);
        int32_t h=create_api(&f.ctrl,1); assert(h>0); thread_handles[row][j]=h;
        assert(decode_api(h,&f.ctrl)==0 && f.output.bytes==512);
        assert(clear_api(h)==0 && delete_api(h)==0);
    }
    return NULL;
}
int main(void) {
    init_api=(AdLibraryFn)runtime_audiodec_resolve("VjhsmxpcezI#libSceAudiodec");
    term_api=(AdLibraryFn)runtime_audiodec_resolve("h5jSB2QIDV0#libSceAudiodec");
    create_api=(AdCreateFn)runtime_audiodec_resolve("O3f1sLMWRvs#libSceAudiodec");
    delete_api=(AdHandleFn)runtime_audiodec_resolve("Tp+ZEy69mLk#libSceAudiodec");
    clear_api=(AdHandleFn)runtime_audiodec_resolve("6Vf9WTLDoss#libSceAudiodec");
    decode_api=(AdDecodeFn)runtime_audiodec_resolve("KHXHMDLkILw#libSceAudiodec");
    assert(init_api && term_api && create_api && delete_api && clear_api && decode_api);
    assert(!runtime_audiodec_resolve("unknown#libSceAudiodec"));
    lifecycle_and_errors(); real_decode_and_reset(); unaligned_stereo_float(); guarded_size_records();
    pthread_t threads[THREADS];
    for (unsigned i=0;i<THREADS;++i) assert(!pthread_create(&threads[i],NULL,worker,(void *)(uintptr_t)i));
    for (unsigned i=0;i<THREADS;++i) assert(!pthread_join(threads[i],NULL));
    for (unsigned i=0;i<THREADS*ITERATIONS;++i)
        for (unsigned j=i+1;j<THREADS*ITERATIONS;++j)
            assert(thread_handles[i/ITERATIONS][i%ITERATIONS]!=thread_handles[j/ITERATIONS][j%ITERATIONS]);
    assert(term_api(1)==0);
    Fixture f; fixture(&f,1,1); assert(create_api(&f.ctrl,1)==AD_ARGUMENT);
    runtime_audiodec_report();
    puts("Audiodec real ATRAC9, byte counts, reset, unaligned ABI and lifecycle: PASS");
    return 0;
}
