/* libScePngDec ABI: shadPS4 core/libraries/libpng/pngdec.h and Raiden V's
 * original callers at 0x183270/0x183330. Decode uses the existing bundled
 * FFmpeg PNG codec; no guest callbacks or additional decoder dependency. */
#define _GNU_SOURCE
#include "runtime.h"
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <zlib.h>

#define PNG_INVALID_ADDR ((int32_t)UINT32_C(0x80690001))
#define PNG_INVALID_SIZE ((int32_t)UINT32_C(0x80690002))
#define PNG_INVALID_PARAM ((int32_t)UINT32_C(0x80690003))
#define PNG_INVALID_HANDLE ((int32_t)UINT32_C(0x80690004))
#define PNG_INVALID_WORK ((int32_t)UINT32_C(0x80690005))
#define PNG_INVALID_DATA ((int32_t)UINT32_C(0x80690010))
#define PNG_UNSUPPORTED ((int32_t)UINT32_C(0x80690011))
#define PNG_DECODE_ERROR ((int32_t)UINT32_C(0x80690012))
#define PNG_FATAL ((int32_t)UINT32_C(0x80690020))

typedef struct { const uint8_t *png; uint32_t png_size,reserved; } PngParseParam;
typedef struct {
    uint32_t width,height;
    uint16_t color_space,bit_depth;
    uint32_t flags;
} PngImageInfo;
typedef struct { uint32_t size,attribute,max_width; } PngCreateParam;
typedef struct {
    const uint8_t *png;
    uint8_t *image;
    uint32_t png_size,image_size;
    uint16_t pixel_format,alpha;
    uint32_t pitch;
} PngDecodeParam;
typedef struct { uint64_t magic; uint32_t max_width,attribute; } PngWork;
_Static_assert(sizeof(PngParseParam)==16,"PNG parse ABI");
_Static_assert(sizeof(PngImageInfo)==16,"PNG info ABI");
_Static_assert(sizeof(PngCreateParam)==12,"PNG create ABI");
_Static_assert(sizeof(PngDecodeParam)==32 && offsetof(PngDecodeParam,pitch)==28,"PNG decode ABI");

/* Work memory belongs to the caller. Host bookkeeping validates handles without
 * dereferencing arbitrary guest handles. Decode copies immutable settings under
 * the lock, then owns its FFmpeg objects independently of other handles/deletion. */
typedef struct PngDecoder {
    void *work;
    uint32_t max_width,attribute;
    struct PngDecoder *next;
} PngDecoder;
static PngDecoder *png_decoders;
static pthread_mutex_t png_lock=PTHREAD_MUTEX_INITIALIZER;

static uint32_t png_be32(const uint8_t *p) {
    return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 | (uint32_t)p[2]<<8 | p[3];
}
/* Header-only parsing stops at IDAT. Decode additionally requires complete,
 * CRC-valid chunks through IEND before allocating or touching the output. */
static int32_t png_info(const uint8_t *png,uint32_t size,PngImageInfo *out,int complete) {
    static const uint8_t signature[]={137,80,78,71,13,10,26,10};
    if (!png) return PNG_INVALID_ADDR;
    if (size<33 || memcmp(png,signature,8) || png_be32(png+8)!=13 || memcmp(png+12,"IHDR",4))
        return PNG_INVALID_DATA;
    PngImageInfo info={png_be32(png+16),png_be32(png+20),0,png[24],png[28]};
    unsigned color=png[25],depth=png[24];
    if (!info.width || !info.height || info.width>INT_MAX || info.height>INT_MAX ||
        png[26] || png[27] || png[28]>1) return PNG_INVALID_DATA;
    switch (color) {
    case 0:
        if (depth!=1 && depth!=2 && depth!=4 && depth!=8 && depth!=16) return PNG_INVALID_DATA;
        info.color_space=2; break;
    case 2: case 4: case 6:
        if (depth!=8 && depth!=16) return PNG_INVALID_DATA;
        info.color_space=color==2 ? 3 : color==4 ? 18 : 19; break;
    case 3:
        if (depth!=1 && depth!=2 && depth!=4 && depth!=8) return PNG_INVALID_DATA;
        info.color_space=4; break;
    default:return PNG_UNSUPPORTED;
    }
    int idat=0,palette=0;
    for (size_t at=8;at+12<=size;) {
        uint32_t n=png_be32(png+at);
        if (n>size-at-12) return PNG_INVALID_DATA;
        const uint8_t *type=png+at+4;
        uLong crc=crc32(0L,Z_NULL,0);
        crc=crc32(crc,type,4);
        crc=crc32(crc,type+4,n);
        if ((uint32_t)crc!=png_be32(type+4+n)) return PNG_INVALID_DATA;
        if (!memcmp(type,"IHDR",4) && at!=8) return PNG_INVALID_DATA;
        if (!memcmp(type,"PLTE",4)) {
            if (idat || !n || n>768 || n%3) return PNG_INVALID_DATA;
            palette=(int)(n/3);
        } else if (!memcmp(type,"tRNS",4)) {
            if (idat || (color==0 && n!=2) || (color==2 && n!=6) ||
                (color==3 && (!palette || n>(uint32_t)palette)) || color==4 || color==6)
                return PNG_INVALID_DATA;
            info.flags|=2;
        } else if (!memcmp(type,"IDAT",4)) {
            if (color==3 && (!palette || palette>(1<<depth))) return PNG_INVALID_DATA;
            if (!complete) { *out=info; return 0; }
            idat=1;
        } else if (!memcmp(type,"IEND",4)) {
            if (!idat || n) return PNG_INVALID_DATA;
            *out=info; return 0;
        }
        at+=(size_t)n+12;
    }
    return PNG_INVALID_DATA;
}
static int32_t png_validate_create(const PngCreateParam *param) {
    if (!param || param->size!=sizeof(*param) || param->attribute>1) return PNG_INVALID_PARAM;
    if (!param->max_width || param->max_width>1000001) return PNG_INVALID_SIZE;
    return 0;
}
static ABI int32_t png_query_memory(const PngCreateParam *param) {
    int32_t error=png_validate_create(param);
    return error ? error : (int32_t)sizeof(PngWork);
}
static ABI int32_t png_create(const PngCreateParam *param,void *memory,uint32_t size,void **handle) {
    int32_t error=png_validate_create(param);
    if (error) return error;
    if (!memory || !handle) return PNG_INVALID_ADDR;
    if (size<sizeof(PngWork)) return PNG_INVALID_SIZE;
    PngDecoder *decoder=malloc(sizeof(*decoder));
    if (!decoder) return PNG_FATAL;
    *decoder=(PngDecoder){memory,param->max_width,param->attribute,NULL};
    pthread_mutex_lock(&png_lock);
    for (PngDecoder *other=png_decoders;other;other=other->next) if (other->work==memory) {
        pthread_mutex_unlock(&png_lock); free(decoder); return PNG_INVALID_WORK;
    }
    PngWork work={UINT64_C(0x504e474445434f44),param->max_width,param->attribute};
    memcpy(memory,&work,sizeof(work));
    decoder->next=png_decoders; png_decoders=decoder;
    *handle=memory;
    pthread_mutex_unlock(&png_lock);
    return 0;
}
static ABI int32_t png_delete(void *handle) {
    pthread_mutex_lock(&png_lock);
    PngDecoder **link=&png_decoders;
    while (*link && (*link)->work!=handle) link=&(*link)->next;
    PngDecoder *decoder=*link;
    if (decoder) *link=decoder->next;
    pthread_mutex_unlock(&png_lock);
    if (!decoder) return PNG_INVALID_HANDLE;
    free(decoder);
    return 0;
}
static ABI int32_t png_parse_header(const PngParseParam *param,PngImageInfo *info) {
    if (!param || param->reserved) return PNG_INVALID_PARAM;
    if (!info) return PNG_INVALID_ADDR;
    return png_info(param->png,param->png_size,info,0);
}
/* The SDK strips the low byte of16-bit samples. swscale instead rounds them,
 * so preserve the SDK result explicitly for FFmpeg's packed PNG formats. */
static int png_strip16(const AVFrame *source,AVFrame *output,int bgra) {
    unsigned channels=0,high=0;
    switch ((enum AVPixelFormat)source->format) {
    case AV_PIX_FMT_GRAY16LE:high=1; /* fall through */
    case AV_PIX_FMT_GRAY16BE:channels=1; break;
    case AV_PIX_FMT_YA16LE:high=1; /* fall through */
    case AV_PIX_FMT_YA16BE:channels=2; break;
    case AV_PIX_FMT_RGB48LE:high=1; /* fall through */
    case AV_PIX_FMT_RGB48BE:channels=3; break;
    case AV_PIX_FMT_RGBA64LE:high=1; /* fall through */
    case AV_PIX_FMT_RGBA64BE:channels=4; break;
    default:return 0;
    }
    for (int y=0;y<source->height;++y) {
        const uint8_t *src=source->data[0]+(ptrdiff_t)y*source->linesize[0];
        uint8_t *dst=output->data[0]+(ptrdiff_t)y*output->linesize[0];
        for (int x=0;x<source->width;++x,src+=channels*2,dst+=4) {
            uint8_t r=src[high],g=channels<3 ? r : src[high+2],b=channels<3 ? r : src[high+4];
            dst[0]=bgra ? b : r; dst[1]=g; dst[2]=bgra ? r : b;
            dst[3]=channels==2 ? src[high+2] : channels==4 ? src[high+6] : 255;
        }
    }
    return 1;
}
static ABI int32_t png_decode(void *handle,const PngDecodeParam *param,PngImageInfo *out) {
    uint32_t max_width=0;
    pthread_mutex_lock(&png_lock);
    for (PngDecoder *decoder=png_decoders;decoder;decoder=decoder->next)
        if (decoder->work==handle) { max_width=decoder->max_width; break; }
    pthread_mutex_unlock(&png_lock);
    if (!max_width) return PNG_INVALID_HANDLE;
    if (!param || param->pixel_format>1) return PNG_INVALID_PARAM;
    if (!param->png || !param->image) return PNG_INVALID_ADDR;
    if (param->png_size>INT_MAX) return PNG_INVALID_SIZE;
    PngImageInfo info;
    int32_t error=png_info(param->png,param->png_size,&info,1);
    if (error) return error;
    uint64_t row=(uint64_t)info.width*4;
    uint64_t pitch=param->pitch ? param->pitch : row;
    if (info.width>max_width || pitch<row || pitch>INT_MAX ||
        (uint64_t)(info.height-1)*pitch+row>param->image_size) return PNG_INVALID_SIZE;

    const AVCodec *codec=avcodec_find_decoder(AV_CODEC_ID_PNG);
    AVCodecContext *context=codec ? avcodec_alloc_context3(codec) : NULL;
    AVPacket *packet=av_packet_alloc();
    AVFrame *source=av_frame_alloc(),*rgba=av_frame_alloc();
    struct SwsContext *sws=NULL;
    error=PNG_FATAL;
    if (!context || !packet || !source || !rgba) goto done;
    /* FFmpeg includes SIMD padding in its max_pixels checks. The validated
     * IHDR/output bounds above already limit actual decoded image dimensions. */
    context->err_recognition=AV_EF_CRCCHECK|AV_EF_EXPLODE;
    if (avcodec_open2(context,codec,NULL)<0 || av_new_packet(packet,(int)param->png_size)<0) goto done;
    memcpy(packet->data,param->png,param->png_size);
    error=PNG_DECODE_ERROR;
    if (avcodec_send_packet(context,packet)<0 || avcodec_receive_frame(context,source)<0 ||
        source->width!=(int)info.width || source->height!=(int)info.height) goto done;
    rgba->format=param->pixel_format ? AV_PIX_FMT_BGRA : AV_PIX_FMT_RGBA;
    rgba->width=source->width; rgba->height=source->height;
    if (av_frame_get_buffer(rgba,32)<0) { error=PNG_FATAL; goto done; }
    if (info.bit_depth==16) {
        if (!png_strip16(source,rgba,param->pixel_format)) { error=PNG_UNSUPPORTED; goto done; }
    } else {
        sws=sws_getContext(source->width,source->height,(enum AVPixelFormat)source->format,
            source->width,source->height,(enum AVPixelFormat)rgba->format,SWS_POINT,NULL,NULL,NULL);
        if (!sws || sws_scale(sws,(const uint8_t * const *)source->data,source->linesize,
            0,source->height,rgba->data,rgba->linesize)!=source->height) goto done;
    }
    for (uint32_t y=0;y<info.height;++y) {
        uint8_t *pixels=rgba->data[0]+(size_t)y*rgba->linesize[0];
        if ((info.color_space==2 || info.color_space==3 || info.color_space==4) && !(info.flags&2))
            for (uint32_t x=0;x<info.width;++x) pixels[(size_t)x*4+3]=(uint8_t)param->alpha;
        memcpy(param->image+(size_t)y*pitch,pixels,(size_t)row);
    }
    if (out) *out=info;
    error=info.width>32767 || info.height>32767 ? 0 : (int32_t)(info.width<<16 | info.height);
done:
    sws_freeContext(sws);
    av_frame_free(&rgba); av_frame_free(&source);
    av_packet_free(&packet); avcodec_free_context(&context);
    return error;
}
static const RuntimeExport exports[]={
    {"scePngDecCreate",png_create}, {"scePngDecDecode",png_decode},
    {"scePngDecDelete",png_delete}, {"scePngDecParseHeader",png_parse_header},
    {"scePngDecQueryMemorySize",png_query_memory},
};
uintptr_t runtime_pngdec_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
