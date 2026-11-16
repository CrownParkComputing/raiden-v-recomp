/* Real libScePngDec ABI, pixels, pitch/buffer bounds, transparency and errors. */
#undef NDEBUG
#include "../src/runtime_pngdec.c"
#include <assert.h>
#include <stdio.h>

uintptr_t runtime_lookup(const RuntimeExport *table,size_t count,const char *name) {
    for (size_t i=0;i<count;++i) if (!strcmp(table[i].name,name)) return (uintptr_t)table[i].function;
    return 0;
}
typedef struct { uint8_t bytes[4096]; uint32_t size; } Fixture;
static void be32(uint8_t *p,uint32_t value) {
    p[0]=(uint8_t)(value>>24); p[1]=(uint8_t)(value>>16); p[2]=(uint8_t)(value>>8); p[3]=(uint8_t)value;
}
static void chunk(Fixture *png,const char *type,const uint8_t *data,uint32_t size) {
    assert(png->size+12+size<=sizeof(png->bytes));
    uint8_t *p=png->bytes+png->size;
    be32(p,size); memcpy(p+4,type,4);
    if (size) memcpy(p+8,data,size);
    uLong crc=crc32(0L,Z_NULL,0);
    crc=crc32(crc,p+4,size+4);
    be32(p+size+8,(uint32_t)crc);
    png->size+=size+12;
}
static Fixture fixture(uint32_t width,uint32_t height,uint8_t depth,uint8_t color,int interlace,
    const uint8_t *scanlines,uint32_t bytes,const uint8_t *palette,uint32_t palette_size,
    const uint8_t *alpha,uint32_t alpha_size) {
    Fixture png={{137,80,78,71,13,10,26,10},8};
    uint8_t ihdr[13]={0};
    be32(ihdr,width); be32(ihdr+4,height); ihdr[8]=depth; ihdr[9]=color; ihdr[12]=(uint8_t)interlace;
    chunk(&png,"IHDR",ihdr,sizeof(ihdr));
    if (palette_size) chunk(&png,"PLTE",palette,palette_size);
    if (alpha_size) chunk(&png,"tRNS",alpha,alpha_size);
    uint8_t compressed[1024]; uLongf compressed_size=sizeof(compressed);
    assert(compress2(compressed,&compressed_size,scanlines,bytes,Z_BEST_COMPRESSION)==Z_OK);
    chunk(&png,"IDAT",compressed,(uint32_t)compressed_size);
    chunk(&png,"IEND",NULL,0);
    return png;
}
static void expect_pixels(const Fixture *png,uint32_t width,uint32_t height,uint16_t color,
    uint16_t depth,uint32_t flags,const uint8_t *expected,uint16_t alpha,uint16_t format) {
    PngParseParam parse={png->bytes,png->size,0};
    struct { uint64_t before; PngImageInfo info; uint64_t after; }
        header={UINT64_C(0x12345678abcdef00),{0},UINT64_C(0xfedcba9876543210)};
    assert(png_parse_header(&parse,&header.info)==0);
    assert(header.before==UINT64_C(0x12345678abcdef00) && header.after==UINT64_C(0xfedcba9876543210));
    assert(header.info.width==width && header.info.height==height && header.info.color_space==color);
    assert(header.info.bit_depth==depth && header.info.flags==flags);
    PngCreateParam create={sizeof(create),depth==16,width};
    int32_t size=png_query_memory(&create);
    assert(size>0 && size<=64);
    uint8_t work[80]; memset(work,0xa5,sizeof(work));
    void *handle=NULL;
    assert(png_create(&create,work+8,(uint32_t)size,&handle)==0 && handle);
    for (int i=0;i<8;++i) assert(work[i]==0xa5 && work[size+8+i]==0xa5);
    uint8_t output[256]; memset(output,0xcd,sizeof(output));
    uint32_t pitch=width*4+5,required=(height-1)*pitch+width*4;
    assert(required+16<=sizeof(output));
    PngDecodeParam decode={png->bytes,output+8,png->size,required,format,alpha,pitch};
    PngImageInfo decoded={0};
    assert(png_decode(handle,&decode,&decoded)==(int32_t)(width<<16|height));
    assert(!memcmp(&decoded,&header.info,sizeof(decoded)));
    for (uint32_t y=0;y<height;++y) {
        if (memcmp(output+8+y*pitch,expected+y*width*4,width*4)) {
            fprintf(stderr,"pixel mismatch color%u/depth%u/format%u row%u:",color,depth,format,y);
            for (uint32_t x=0;x<width*4;++x) fprintf(stderr," %u/%u",output[8+y*pitch+x],expected[y*width*4+x]);
            fputc('\n',stderr);
        }
        assert(!memcmp(output+8+y*pitch,expected+y*width*4,width*4));
        if (y+1<height) for (uint32_t x=width*4;x<pitch;++x) assert(output[8+y*pitch+x]==0xcd);
    }
    for (int i=0;i<8;++i) assert(output[i]==0xcd && output[8+required+i]==0xcd);
    /* A handle supports subsequent decode, including tightly packed pitch0. */
    memset(output,0xcd,sizeof(output));
    decode.pitch=0; decode.image_size=width*height*4;
    assert(png_decode(handle,&decode,NULL)==(int32_t)(width<<16|height));
    assert(!memcmp(output+8,expected,width*height*4));
    assert(png_delete(handle)==0 && png_delete(handle)==PNG_INVALID_HANDLE);
    assert(png_decode(handle,&decode,NULL)==PNG_INVALID_HANDLE);
}

int main(void) {
    const uint8_t rgb[]={0,255,0,0,0,255,0, 0,0,0,255,255,255,255};
    Fixture png=fixture(2,2,8,2,0,rgb,sizeof(rgb),NULL,0,NULL,0);
    const uint8_t rgba[]={255,0,0,37,0,255,0,37,0,0,255,37,255,255,255,37};
    expect_pixels(&png,2,2,3,8,0,rgba,37,0);
    const uint8_t bgra[]={0,0,255,37,0,255,0,37,255,0,0,37,255,255,255,37};
    expect_pixels(&png,2,2,3,8,0,bgra,37,1);
    const uint8_t alpha_rgb[]={0,1,2,3,0,4,5,6,127};
    Fixture with_alpha=fixture(2,1,8,6,0,alpha_rgb,sizeof(alpha_rgb),NULL,0,NULL,0);
    expect_pixels(&with_alpha,2,1,19,8,0,alpha_rgb+1,255,0);
    const uint8_t palette[]={255,0,0,0,0,255},transparency[]={0,91},indices[]={0,0,1};
    Fixture indexed=fixture(2,1,8,3,0,indices,sizeof(indices),palette,sizeof(palette),transparency,sizeof(transparency));
    const uint8_t indexed_rgba[]={255,0,0,0,0,0,255,91};
    expect_pixels(&indexed,2,1,4,8,2,indexed_rgba,255,0);
    const uint8_t gray[]={0,0x80},gray_rgba[]={255,255,255,19,0,0,0,19};
    Fixture grayscale=fixture(2,1,1,0,0,gray,sizeof(gray),NULL,0,NULL,0);
    expect_pixels(&grayscale,2,1,2,1,0,gray_rgba,19,0);
    const uint8_t ya[]={0,127,63,200,255},ya_rgba[]={127,127,127,63,200,200,200,255};
    Fixture gray_alpha=fixture(2,1,8,4,0,ya,sizeof(ya),NULL,0,NULL,0);
    expect_pixels(&gray_alpha,2,1,18,8,0,ya_rgba,0,0);
    const uint8_t rgb16[]={0,0x12,0xff,0x56,0xff,0x9a,0xff},rgb16_rgba[]={0x12,0x56,0x9a,255};
    Fixture sixteen=fixture(1,1,16,2,0,rgb16,sizeof(rgb16),NULL,0,NULL,0);
    expect_pixels(&sixteen,1,1,3,16,0,rgb16_rgba,255,0);
    const uint8_t rgba16[]={0,0x12,0xff,0x56,0xff,0x9a,0xff,0x78,0xff};
    const uint8_t rgba16_bgra[]={0x9a,0x56,0x12,0x78};
    Fixture sixteen_alpha=fixture(1,1,16,6,0,rgba16,sizeof(rgba16),NULL,0,NULL,0);
    expect_pixels(&sixteen_alpha,1,1,19,16,0,rgba16_bgra,255,1);
    const uint8_t gray16[]={0,0x12,0xff},gray16_rgba[]={0x12,0x12,0x12,73};
    Fixture sixteen_gray=fixture(1,1,16,0,0,gray16,sizeof(gray16),NULL,0,NULL,0);
    expect_pixels(&sixteen_gray,1,1,2,16,0,gray16_rgba,73,0);
    const uint8_t ya16[]={0,0x12,0xff,0x78,0xff},ya16_rgba[]={0x12,0x12,0x12,0x78};
    Fixture sixteen_ya=fixture(1,1,16,4,0,ya16,sizeof(ya16),NULL,0,NULL,0);
    expect_pixels(&sixteen_ya,1,1,18,16,0,ya16_rgba,255,0);
    const uint8_t keyed_rgb[]={0,255,0,0,0,255,0},red_key[]={0,255,0,0,0,0};
    const uint8_t keyed_rgba[]={255,0,0,0,0,255,0,255};
    Fixture keyed=fixture(2,1,8,2,0,keyed_rgb,sizeof(keyed_rgb),NULL,0,red_key,sizeof(red_key));
    expect_pixels(&keyed,2,1,3,8,2,keyed_rgba,0,0);

    /* A3x3 Adam7 image spans six nonempty passes, not just the first pixel. */
    const uint8_t adam_pixels[]={1,2,3,255, 4,5,6,255, 7,8,9,255,
        10,11,12,255, 13,14,15,255, 16,17,18,255,
        19,20,21,255, 22,23,24,255, 25,26,27,255};
    const unsigned x0[]={0,4,0,2,0,1,0},y0[]={0,0,4,0,2,0,1};
    const unsigned dx[]={8,8,4,4,2,2,1},dy[]={8,8,8,4,4,2,2};
    uint8_t adam_rows[80]; uint32_t count=0;
    for (int pass=0;pass<7;++pass) if (x0[pass]<3)
        for (unsigned y=y0[pass];y<3;y+=dy[pass]) {
            adam_rows[count++]=0;
            for (unsigned x=x0[pass];x<3;x+=dx[pass]) {
                memcpy(adam_rows+count,adam_pixels+(y*3+x)*4,4); count+=4;
            }
        }
    Fixture adam=fixture(3,3,8,6,1,adam_rows,count,NULL,0,NULL,0);
    expect_pixels(&adam,3,3,19,8,1,adam_pixels,0,0);

    PngCreateParam create={sizeof(create),0,2};
    uint8_t work[64],output[32]; void *handle=NULL;
    assert(png_query_memory(NULL)==PNG_INVALID_PARAM);
    create.size=0; assert(png_query_memory(&create)==PNG_INVALID_PARAM); create.size=sizeof(create);
    create.max_width=0; assert(png_query_memory(&create)==PNG_INVALID_SIZE); create.max_width=2;
    assert(png_create(&create,NULL,sizeof(work),&handle)==PNG_INVALID_ADDR);
    assert(png_create(&create,work,0,&handle)==PNG_INVALID_SIZE);
    assert(png_create(&create,work,sizeof(work),&handle)==0);
    void *other=(void *)(uintptr_t)1;
    assert(png_create(&create,work,sizeof(work),&other)==PNG_INVALID_WORK && other==(void *)(uintptr_t)1);
    PngDecodeParam decode={png.bytes,output,png.size,sizeof(output),0,255,0};
    assert(png_decode((void *)(uintptr_t)1,&decode,NULL)==PNG_INVALID_HANDLE);
    assert(png_decode(handle,NULL,NULL)==PNG_INVALID_PARAM);
    decode.pixel_format=2; assert(png_decode(handle,&decode,NULL)==PNG_INVALID_PARAM); decode.pixel_format=0;
    memset(output,0x5a,sizeof(output));
    decode.pitch=7; assert(png_decode(handle,&decode,NULL)==PNG_INVALID_SIZE); decode.pitch=0;
    decode.image_size=15; assert(png_decode(handle,&decode,NULL)==PNG_INVALID_SIZE); decode.image_size=sizeof(output);
    for (size_t i=0;i<sizeof(output);++i) assert(output[i]==0x5a);
    decode.png_size=png.size-1; assert(png_decode(handle,&decode,NULL)==PNG_INVALID_DATA);
    decode.png_size=png.size;
    Fixture corrupt=png; corrupt.bytes[29]^=1; decode.png=corrupt.bytes;
    assert(png_decode(handle,&decode,NULL)==PNG_INVALID_DATA);
    const uint8_t invalid_deflate[]={1,2,3};
    Fixture bad_stream={{137,80,78,71,13,10,26,10},8};
    chunk(&bad_stream,"IHDR",png.bytes+16,13);
    chunk(&bad_stream,"IDAT",invalid_deflate,sizeof(invalid_deflate));
    chunk(&bad_stream,"IEND",NULL,0);
    decode.png=bad_stream.bytes; decode.png_size=bad_stream.size;
    assert(png_decode(handle,&decode,NULL)==PNG_DECODE_ERROR);
    for (size_t i=0;i<sizeof(output);++i) assert(output[i]==0x5a);
    PngParseParam parse={png.bytes,20,0};
    PngImageInfo info={123,456,7,8,9};
    assert(png_parse_header(&parse,&info)==PNG_INVALID_DATA && info.width==123);
    assert(png_parse_header(NULL,&info)==PNG_INVALID_PARAM);
    parse.png_size=png.size; parse.png=NULL; assert(png_parse_header(&parse,&info)==PNG_INVALID_ADDR);
    assert(png_parse_header(&parse,NULL)==PNG_INVALID_ADDR);
    assert(png_delete(handle)==0);
    assert(runtime_pngdec_resolve("scePngDecDecode")== (uintptr_t)png_decode);
    assert(runtime_pngdec_resolve("scePngDecDelete")== (uintptr_t)png_delete);
    puts("PNG decoder ABI, real pixels, pitch, transparency, Adam7 and errors passed");
    return 0;
}
