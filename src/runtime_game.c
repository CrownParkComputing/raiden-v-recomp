/* The game's version, logged first so a bug report's log tells which dump it came from: param.sfo's
 * TITLE_ID and APP_VER, and what scripts/prepare.py records in analysis.json beside the image: the
 * eboot's SHA-256 and the verified build it is (VERIFIED_BUILDS; empty for one prepared with
 * --any-build). */
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <sys/mman.h>
#endif
#include "gpu/tempest_gpu.h"

/* The string value of "key" in analysis.json's head (it is written first), at most size-1 chars of
 * [0-9a-z.]; 0 when absent. */
static int report_string(const char *head, const char *key, char *out, size_t size) {
    char pattern[64];
    snprintf(pattern,sizeof(pattern),"\"%s\": \"",key);
    const char *value=strstr(head,pattern);
    if (!value) return 0;
    value+=strlen(pattern);
    const size_t n=strspn(value,"0123456789abcdefghijklmnopqrstuvwxyz.");
    if (n>=size || value[n]!='"') return 0;
    memcpy(out,value,n); out[n]=0;
    return 1;
}

void runtime_game_identify(const char *title_id, const char *app_version, const char *app0, const char *image_path) {
    /* Support framework paths using Assets when the package stores assets,
     * without changing the original dump or shadowing an authored Assets folder. */
    char assets[4096], authored[4096];
    struct stat entry;
    snprintf(assets,sizeof(assets),"%s/assets",app0);
    snprintf(authored,sizeof(authored),"%s/Assets",app0);
    if (stat(authored,&entry) && !stat(assets,&entry) && S_ISDIR(entry.st_mode)) {
        runtime_file_mount("/app0/Assets",assets);
        runtime_file_mount("/hostapp/Assets",assets);
    }
    char sha[65]="", build[16]="", path[4096], head[4096];
    const char *slash=strrchr(image_path,'/'), *back=strrchr(image_path,'\\');
    if (back>slash) slash=back;
    const int dir=slash ? (int)(slash-image_path)+1 : 0;
    snprintf(path,sizeof(path),"%.*sanalysis.json",dir,image_path);
    FILE *f=fopen(path,"rb");
    if (f) {
        const size_t n=fread(head,1,sizeof(head)-1,f);
        fclose(f);
        head[n]=0;
        if (!report_string(head,"source_sha256",sha,sizeof(sha)) || strlen(sha)!=64) sha[0]=0;
        report_string(head,"verified_build",build,sizeof(build));
    }
    printf("Game: %s version %s, eboot.bin sha256 %s (%s%s)\n",title_id[0] ? title_id : "?",
           app_version[0] ? app_version : "unknown",sha[0] ? sha : "unknown",
           build[0] ? "verified build " : "not a verified build",build);
}

/* Raiden V: Director's Cut owns its frame clock and render targets. Older game-specific
 * addresses must never be inspected or patched in this executable. */
void runtime_game_set_image(unsigned char *image) { (void)image; }
void runtime_game_frame_rate(void) {}
int runtime_game_widen(unsigned char *image,uint32_t width_4k) {
    (void)image;
    if (width_4k!=3840) puts("Display: Raiden V: Director's Cut native aspect ratio; engine resizing is unavailable");
    return width_4k==3840;
}
uint32_t runtime_game_wide_width(const char *name,uint32_t width,uint32_t height,uint32_t width_4k) {
    (void)name; (void)height; (void)width_4k;
    return width;
}

void runtime_game_configure_modules(unsigned char *image,const RuntimeModule *modules,uint64_t count) {
    (void)image; (void)modules; (void)count;
}
