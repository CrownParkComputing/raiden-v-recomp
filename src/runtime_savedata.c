/* libSceSaveData on host directories:
 *   <user>/savedata/<user id>/<title id>/<dir name>/        files the game writes
 *   <user>/savedata/<user id>/<title id>/<dir name>.sce_sys/ param.bin, icon0.png
 *   <user>/savedata/<user id>/<title id>/<dir name>.sce_backup/  backup: data/ and sce_sys/
 *   <user>/savedata/<user id>/<title id>.memory/memory.dat  SaveDataMemory (slot 0; slot n:
 *                                                 <title id>.memory<n>/), with param.bin, icon0.png
 * A mounted directory appears to the guest as /savedata0../savedata15.
 * Metadata lives beside the directory so the guest's own files are untouched. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#ifndef _WIN32
#include <ftw.h>
#endif

#define ERR_PARAMETER ((int32_t)0x809F0000)
#define ERR_NOT_INITIALIZED ((int32_t)0x809F0001)
#define ERR_BUSY ((int32_t)0x809F0003)
#define ERR_NOT_MOUNTED ((int32_t)0x809F0004)
#define ERR_EXISTS ((int32_t)0x809F0007)
#define ERR_NOT_FOUND ((int32_t)0x809F0008)
#define ERR_INTERNAL ((int32_t)0x809F000B)
#define ERR_MOUNT_FULL ((int32_t)0x809F000C)
#define ERR_BAD_MOUNTED ((int32_t)0x809F000D)
#define ERR_INVALID_USER ((int32_t)0x809F0011)
#define ERR_MEMORY_NOT_READY ((int32_t)0x809F0012)
#define BLOCK_SIZE 32768
#define DEFAULT_BLOCKS 32768
#define MODE_RDONLY 1
#define MODE_CREATE 4
#define MODE_COPY_ICON 16
#define MODE_CREATE2 32
#define SLOTS 16

typedef struct { char data[10]; char pad[6]; } TitleId;
typedef struct { char data[32]; } DirName;
typedef struct { char data[16]; } MountPoint;
typedef struct {
    char title[128], subtitle[128], detail[1024];
    uint32_t user_param; int32_t pad;
    int64_t mtime;
    uint8_t reserved[32];
} Param;
typedef struct { int32_t user; int32_t pad; const TitleId *title; const DirName *dir; const void *fingerprint;
                 uint64_t blocks; uint32_t mode; uint8_t reserved[32]; } Mount1;
typedef struct { int32_t user; int32_t pad; const DirName *dir; uint64_t blocks; uint32_t mode;
                 uint8_t reserved[32]; int32_t pad2; } Mount2;
typedef struct { MountPoint point; uint64_t required_blocks; uint32_t unused, status; uint8_t reserved[28]; int32_t pad; } MountResult;
typedef struct { int32_t user; int32_t pad; const TitleId *title; const DirName *dir; uint32_t unused;
                 uint8_t reserved[32]; int32_t pad2; } Delete;
typedef struct { int32_t user; int32_t pad; const TitleId *title; const DirName *dir; uint32_t key, order;
                 uint8_t reserved[32]; } SearchCond;
typedef struct { uint64_t blocks, free_blocks; uint8_t reserved[32]; } SearchInfo;
typedef struct { uint32_t hits; int32_t pad; DirName *names; uint32_t names_capacity, set_count;
                 Param *params; SearchInfo *infos; uint8_t reserved[12]; int32_t pad2; } SearchResult;
typedef struct { const void *buffer; uint64_t buffer_size, data_size; uint8_t reserved[32]; } Icon;
_Static_assert(sizeof(Param)==1328,"OrbisSaveDataParam layout");
_Static_assert(sizeof(Mount1)==80 && sizeof(Mount2)==64,"OrbisSaveDataMount layouts");
_Static_assert(sizeof(MountResult)==64,"OrbisSaveDataMountResult layout");
_Static_assert(sizeof(SearchCond)==64 && sizeof(SearchResult)==56,"dir name search layouts");

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static int initialized;
static char title_id[16]="UNKNOWN";
static struct { int used; int32_t user; uint64_t blocks; char host[700], meta[700]; } slots[SLOTS];
static size_t mounts_done, memory_writes, backups_done;
static struct { unsigned char *data; uint64_t size, icon_size; int32_t user; char path[700]; } memory;

void runtime_savedata_configure(const char *title) {
    if (title && *title) snprintf(title_id,sizeof(title_id),"%s",title);
}

static int make_dirs(const char *path) {
    char buffer[700];
    snprintf(buffer,sizeof(buffer),"%s",path);
    for (char *p=buffer+1;*p;++p) if (*p=='/') { *p=0; mkdir(buffer,0755); *p='/'; }
    return mkdir(buffer,0755) && errno!=EEXIST ? -1 : 0;
}
static void root(int32_t user, const char *title, char *out, size_t size) {
    snprintf(out,size,"%s/savedata/%d/%s",runtime_file_user_dir(),user,title && *title ? title : title_id);
}
static int valid_name(const char *name, size_t max) {
    size_t n=strnlen(name,max);
    if (!n || n==max) return 0;
    for (size_t i=0;i<n;++i) if (name[i]=='/' || name[i]=='\\' || (name[i]=='.' && (i==0 || name[i-1]=='.'))) return 0;
    return 1;
}
static void write_param(const char *meta, const Param *p) {
    char path[700]; snprintf(path,sizeof(path),"%s/param.bin",meta);
    FILE *f=fopen(path,"wb");
    if (f) { Param copy=*p; copy.mtime=time(NULL); fwrite(&copy,sizeof(copy),1,f); fclose(f); }
}
static int read_param(const char *meta, Param *p) {
    char path[700]; snprintf(path,sizeof(path),"%s/param.bin",meta);
    memset(p,0,sizeof(*p));
    FILE *f=fopen(path,"rb");
    if (!f) return -1;
    size_t n=fread(p,sizeof(*p),1,f); fclose(f);
    struct stat st;
    if (!stat(path,&st)) p->mtime=st.st_mtime;
    return n==1 ? 0 : -1;
}
#ifdef _WIN32
static void remove_tree(const char *path) { compat_remove_tree(path); }
#else
static int remove_entry(const char *path, const struct stat *st, int flag, struct FTW *ftw) {
    (void)st; (void)flag; (void)ftw; return remove(path);
}
static void remove_tree(const char *path) { nftw(path,remove_entry,16,FTW_DEPTH|FTW_PHYS); }
#endif

/* A save's param (title, subtitle, date) for the save dialog; 0 when it exists. */
int runtime_savedata_param(int32_t user, const char *dir, void *param) {
    if (!valid_name(dir,32)) return -1;
    char base[600], meta[680];
    root(user,NULL,base,sizeof(base));
    snprintf(meta,sizeof(meta),"%s/%s.sce_sys",base,dir);
    return read_param(meta,(Param *)param);
}
/* Host path of a save's icon0.png (the location picture in the save dialog); 0 when named. */
int runtime_savedata_icon(int32_t user, const char *dir, char *out, size_t size) {
    if (!valid_name(dir,32)) return -1;
    char base[600];
    root(user,NULL,base,sizeof(base));
    snprintf(out,size,"%s/%s.sce_sys/icon0.png",base,dir);
    return 0;
}
/* Newest modification time (seconds) of a save: its files and its param.bin; 0 when absent. */
static int64_t newest(const char *path, int depth) {
    struct stat st;
#ifndef _WIN32
    if (lstat(path,&st)) return 0;
    if (S_ISLNK(st.st_mode)) return 0; /* do not follow symlinks outside save */
#else
    if (stat(path,&st)) return 0;
#endif
    if (!S_ISDIR(st.st_mode)) return (int64_t)(st.st_mtime>0 ? st.st_mtime : 0);
    if (depth>4) return 0;
    int64_t t=0;
    DIR *d=opendir(path);
    for (struct dirent *e; d && (e=readdir(d));) {
        if (!strcmp(e->d_name,".") || !strcmp(e->d_name,"..")) continue;
        char child[1024];
        if (snprintf(child,sizeof(child),"%s/%s",path,e->d_name)>=(int)sizeof(child)) continue;
        int64_t c=newest(child,depth+1);
        if (c>t) t=c;
    }
    if (d) closedir(d);
    return t;
}
/* When a save was last written (the save list's order and focus); 0 when absent. */
int64_t runtime_savedata_mtime(int32_t user, const char *dir) {
    if (!valid_name(dir,32)) return 0;
    char base[600], path[700];
    root(user,NULL,base,sizeof(base));
    snprintf(path,sizeof(path),"%s/%s",base,dir);
    struct stat st;
    if (stat(path,&st) || !S_ISDIR(st.st_mode)) return 0;
    int64_t t=newest(path,0);
    snprintf(path,sizeof(path),"%s/%s.sce_sys/param.bin",base,dir);
    if (!stat(path,&st) && (int64_t)st.st_mtime>t) t=(int64_t)st.st_mtime;
    return t>0 ? t : 0;
}
static ABI int32_t save_initialize(const void *param) { (void)param; initialized=1; return 0; }
static ABI int32_t save_terminate(void) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    pthread_mutex_lock(&lock);
    free(memory.data); memset(&memory,0,sizeof(memory));
    initialized=0;
    pthread_mutex_unlock(&lock);
    return 0;
}
static int32_t mount(int32_t user, const char *title, const DirName *dir, uint32_t mode, uint64_t blocks, MountResult *result) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (user<0) return ERR_INVALID_USER;
    if (!dir || !result || !valid_name(dir->data,sizeof(dir->data))) return ERR_PARAMETER;
    char base[600], host[640], meta[680];
    root(user,title,base,sizeof(base));
    snprintf(host,sizeof(host),"%s/%s",base,dir->data);
    snprintf(meta,sizeof(meta),"%s/%s.sce_sys",base,dir->data);
    struct stat st;
    int exists=!stat(host,&st) && S_ISDIR(st.st_mode);
    if ((mode & MODE_CREATE) && exists) return ERR_EXISTS;
    if (!(mode & (MODE_CREATE|MODE_CREATE2)) && !exists) return ERR_NOT_FOUND;
    pthread_mutex_lock(&lock);
    int slot=-1;
    for (int i=0;i<SLOTS;++i) {
        if (slots[i].used && !strcmp(slots[i].host,host)) { pthread_mutex_unlock(&lock); return ERR_BAD_MOUNTED; }
        if (!slots[i].used && slot<0) slot=i;
    }
    if (slot<0) { pthread_mutex_unlock(&lock); return ERR_MOUNT_FULL; }
    if (!exists && (make_dirs(host) || make_dirs(meta))) { pthread_mutex_unlock(&lock); return ERR_INTERNAL; }
    if (!exists) { Param empty={0}; write_param(meta,&empty); }
    slots[slot].used=1;
    slots[slot].blocks=blocks ? blocks : DEFAULT_BLOCKS;
    slots[slot].user=user;
    snprintf(slots[slot].host,sizeof(slots[slot].host),"%s",host);
    snprintf(slots[slot].meta,sizeof(slots[slot].meta),"%s",meta);
    memset(result,0,sizeof(*result));
    char point[32];
    snprintf(point,sizeof(point),"/savedata%d",slot&15);
    memcpy(result->point.data,point,strlen(point)+1);
    result->status=exists ? 0 : 1; /* CREATED */
    runtime_file_mount(result->point.data,host);
    ++mounts_done;
    pthread_mutex_unlock(&lock);
    printf("Runtime: save data '%s' mounted at %s (%s%s)\n",dir->data,result->point.data,
           exists ? "existing" : "created",(mode & MODE_RDONLY) ? ", read-only" : "");
    return 0;
}
static ABI int32_t save_mount2(const Mount2 *m, MountResult *result) {
    if (!m) return ERR_PARAMETER;
    return mount(m->user,NULL,m->dir,m->mode,m->blocks,result);
}
static int slot_of(const MountPoint *point) {
    if (!point || strncmp(point->data,"/savedata",9)) return -1;
    int slot=atoi(point->data+9);
    return slot>=0 && slot<SLOTS && slots[slot].used ? slot : -1;
}
static ABI int32_t save_umount(const MountPoint *point) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    pthread_mutex_lock(&lock);
    int slot=slot_of(point);
    if (slot>=0) { runtime_file_unmount(point->data); slots[slot].used=0; }
    pthread_mutex_unlock(&lock);
    return slot>=0 ? 0 : ERR_NOT_FOUND;
}
static ABI int32_t save_set_param(const MountPoint *point, uint32_t type, const void *buffer, uint64_t size) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!buffer) return ERR_PARAMETER;
    pthread_mutex_lock(&lock);
    int slot=slot_of(point);
    if (slot<0) { pthread_mutex_unlock(&lock); return ERR_NOT_MOUNTED; }
    Param p; read_param(slots[slot].meta,&p);
    switch (type) {
    case 0: if (size<sizeof(Param)) goto bad; memcpy(&p,buffer,sizeof(p)); break;     /* ALL */
    case 1: snprintf(p.title,sizeof(p.title),"%.*s",(int)size,(const char *)buffer); break;
    case 2: snprintf(p.subtitle,sizeof(p.subtitle),"%.*s",(int)size,(const char *)buffer); break;
    case 3: snprintf(p.detail,sizeof(p.detail),"%.*s",(int)size,(const char *)buffer); break;
    case 4: if (size<4) goto bad; memcpy(&p.user_param,buffer,4); break;
    default: goto bad;
    }
    write_param(slots[slot].meta,&p);
    pthread_mutex_unlock(&lock);
    return 0;
bad:
    pthread_mutex_unlock(&lock);
    return ERR_PARAMETER;
}
static ABI int32_t save_icon(const MountPoint *point, const Icon *icon) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!icon || !icon->buffer) return ERR_PARAMETER;
    pthread_mutex_lock(&lock);
    int slot=slot_of(point);
    int32_t r=ERR_NOT_MOUNTED;
    if (slot>=0) {
        char path[700]; snprintf(path,sizeof(path),"%s/icon0.png",slots[slot].meta);
        FILE *f=fopen(path,"wb");
        r=f && fwrite(icon->buffer,1,icon->data_size,f)==icon->data_size ? 0 : ERR_INTERNAL;
        if (f) fclose(f);
    }
    pthread_mutex_unlock(&lock);
    return r;
}
static ABI int32_t save_delete(const Delete *d) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!d || !d->dir || !valid_name(d->dir->data,sizeof(d->dir->data))) return ERR_PARAMETER;
    char base[600], host[640], meta[680];
    root(d->user,d->title ? d->title->data : NULL,base,sizeof(base));
    snprintf(host,sizeof(host),"%s/%s",base,d->dir->data);
    snprintf(meta,sizeof(meta),"%s/%s.sce_sys",base,d->dir->data);
    struct stat st;
    if (stat(host,&st)) return ERR_NOT_FOUND;
    remove_tree(host);
    remove_tree(meta);
    printf("Runtime: save data '%s' deleted\n",d->dir->data);
    return 0;
}
typedef struct { char name[32]; Param param; } Entry;
typedef struct { void *buffer; uint64_t size; int64_t offset; uint8_t reserved[40]; } MemoryData;
typedef struct { uint32_t option; int32_t user; uint64_t size, icon_size; const Param *param; const Icon *icon;
                 uint32_t slot; uint8_t reserved[20]; } MemorySetup2;
typedef struct { uint64_t existed; uint8_t reserved[16]; } MemorySetupResult;
typedef struct { int32_t user; int32_t pad; MemoryData *data; Param *param; Icon *icon; uint32_t slot;
                 uint8_t reserved[28]; } MemoryGet2;
typedef struct { int32_t user; int32_t pad; const MemoryData *data; const Param *param; const Icon *icon;
                 uint32_t count, slot; uint8_t reserved[32]; } MemorySet2;
typedef struct { int32_t user; uint32_t slot, option; uint8_t reserved[28]; } MemorySync;
_Static_assert(sizeof(MemorySetup2)==64 && sizeof(MemoryGet2)==64 && sizeof(MemorySet2)==72,"SaveDataMemory v2 layouts");

/* CUSA07673 uses the pre-5.00 SDK ABI: slot/data-count fields are reserved,
 * so these calls operate on slot zero and exactly one data descriptor. The
 * measured setup caller (0x19d740) zeroes 64 bytes and requests 0xeb78 bytes.
 * Persist each update before reporting success; an interrupted write keeps
 * the previous complete memory file. Metadata is stored beside that file. */
#define MEMORY_LIMIT (64u << 20)
static int memory_write_file(const char *path, const void *data, size_t size) {
    char temporary[760];
    if (snprintf(temporary,sizeof(temporary),"%s.tmp",path)>=(int)sizeof(temporary)) return -1;
    FILE *f=fopen(temporary,"wb");
    if (!f) return -1;
    int failed=fwrite(data,1,size,f)!=size || fflush(f);
#ifndef _WIN32
    if (!failed && fsync(fileno(f))) failed=1;
#endif
    if (fclose(f)) failed=1;
    if (!failed && rename(temporary,path)) failed=1;
    if (failed) unlink(temporary);
    return failed ? -1 : 0;
}
static int memory_put_icon(const Icon *icon, uint64_t capacity, const char *folder) {
    if (!icon) return 0;
    uint64_t size=icon->data_size ? icon->data_size : icon->buffer_size;
    if ((size && !icon->buffer) || size>icon->buffer_size || size>capacity) return -1;
    char path[760]; snprintf(path,sizeof(path),"%s/icon0.png",folder);
    return memory_write_file(path,icon->buffer,(size_t)size);
}
static int memory_put_param(const Param *param, const char *folder) {
    if (!param) return 0;
    Param copy=*param; copy.mtime=time(NULL);
    char path[760]; snprintf(path,sizeof(path),"%s/param.bin",folder);
    return memory_write_file(path,&copy,sizeof(copy));
}
static ABI int32_t memory_setup(const MemorySetup2 *setup, MemorySetupResult *result) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!setup || !setup->size || setup->size>MEMORY_LIMIT || setup->icon_size>MEMORY_LIMIT || (setup->option&~7u)) return ERR_PARAMETER;
    if (setup->user<0) return ERR_INVALID_USER;
    char base[600], folder[700], path[760];
    root(setup->user,NULL,base,sizeof(base));
    snprintf(folder,sizeof(folder),"%s.memory",base);
    snprintf(path,sizeof(path),"%s/memory.dat",folder);
    pthread_mutex_lock(&lock);
    unsigned char *data=calloc(1,(size_t)setup->size);
    int32_t error=0;
    uint64_t existed=0;
    if (!data || make_dirs(folder)) { error=ERR_INTERNAL; goto done; }
    struct stat st;
    if (!stat(path,&st)) {
        if (!S_ISREG(st.st_mode) || st.st_size<0 || (uint64_t)st.st_size>MEMORY_LIMIT) { error=ERR_INTERNAL; goto done; }
        existed=(uint64_t)st.st_size;
        FILE *f=fopen(path,"rb");
        size_t size=(size_t)(existed<setup->size ? existed : setup->size);
        if (!f) { error=ERR_INTERNAL; goto done; }
        int failed=fread(data,1,size,f)!=size;
        if (fclose(f)) failed=1;
        if (failed) { error=ERR_INTERNAL; goto done; }
    } else {
        if (errno!=ENOENT || memory_write_file(path,data,(size_t)setup->size)) { error=ERR_INTERNAL; goto done; }
    }
    if (!existed && (memory_put_param(setup->param,folder) || memory_put_icon(setup->icon,setup->icon_size,folder))) { error=ERR_INTERNAL; goto done; }
    free(memory.data);
    memory.data=data; data=NULL;
    memory.size=setup->size; memory.icon_size=setup->icon_size; memory.user=setup->user;
    snprintf(memory.path,sizeof(memory.path),"%s",folder);
    if (result) result->existed=existed;
done:
    free(data);
    pthread_mutex_unlock(&lock);
    return error;
}
static int memory_valid_data(const MemoryData *data) {
    return !data || (data->offset>=0 && (uint64_t)data->offset<=memory.size &&
        data->size<=memory.size-(uint64_t)data->offset && (!data->size || data->buffer));
}
static ABI int32_t memory_get(MemoryGet2 *get) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!get) return ERR_PARAMETER;
    if (get->user<0) return ERR_INVALID_USER;
    pthread_mutex_lock(&lock);
    int32_t error=0;
    if (!memory.data || memory.user!=get->user) { error=ERR_MEMORY_NOT_READY; goto done; }
    if (!memory_valid_data(get->data) || (get->icon && get->icon->buffer_size && !get->icon->buffer)) { error=ERR_PARAMETER; goto done; }
    if (get->data && get->data->size) memcpy(get->data->buffer,memory.data+get->data->offset,(size_t)get->data->size);
    if (get->param) read_param(memory.path,get->param);
    if (get->icon) {
        char path[760]; snprintf(path,sizeof(path),"%s/icon0.png",memory.path);
        FILE *f=fopen(path,"rb");
        get->icon->data_size=0;
        if (f) {
            get->icon->data_size=fread((void *)get->icon->buffer,1,(size_t)get->icon->buffer_size,f);
            if (ferror(f)) error=ERR_INTERNAL;
            fclose(f);
        } else if (errno!=ENOENT) error=ERR_INTERNAL;
    }
done:
    pthread_mutex_unlock(&lock);
    return error;
}
static ABI int32_t memory_set(const MemorySet2 *set) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!set) return ERR_PARAMETER;
    if (set->user<0) return ERR_INVALID_USER;
    pthread_mutex_lock(&lock);
    int32_t error=0;
    unsigned char *copy=NULL;
    if (!memory.data || memory.user!=set->user) { error=ERR_MEMORY_NOT_READY; goto done; }
    if (!memory_valid_data(set->data)) { error=ERR_PARAMETER; goto done; }
    if (set->icon) {
        uint64_t size=set->icon->data_size ? set->icon->data_size : set->icon->buffer_size;
        if ((size && !set->icon->buffer) || size>set->icon->buffer_size || size>memory.icon_size) { error=ERR_PARAMETER; goto done; }
    }
    if (set->data) {
        copy=malloc((size_t)memory.size);
        if (!copy) { error=ERR_INTERNAL; goto done; }
        memcpy(copy,memory.data,(size_t)memory.size);
        if (set->data->size) memcpy(copy+set->data->offset,set->data->buffer,(size_t)set->data->size);
        char path[760]; snprintf(path,sizeof(path),"%s/memory.dat",memory.path);
        if (memory_write_file(path,copy,(size_t)memory.size)) { error=ERR_INTERNAL; goto done; }
        memcpy(memory.data,copy,(size_t)memory.size);
        ++memory_writes;
    }
    if (memory_put_param(set->param,memory.path) || memory_put_icon(set->icon,memory.icon_size,memory.path)) error=ERR_INTERNAL;
done:
    free(copy);
    pthread_mutex_unlock(&lock);
    return error;
}

/* ---- Backups (shadPS4 save_backup.cpp), done at once on the calling thread ----
 * A backup copies the save's files to <dir>.sce_backup/data and its param.bin/icon0.png to
 * <dir>.sce_backup/sce_sys; each completed one queues an event for sceSaveDataGetEventResult
 * (1 umount backup, 2 backup, 3 memory sync) and sets the progress to 1. A mounted save is BUSY. */
typedef struct { uint32_t type; int32_t error, user; uint8_t pad[4]; TitleId title; DirName dir; uint8_t reserved[40]; } Event;
_Static_assert(sizeof(Event)==104,"OrbisSaveDataEvent layout");
typedef struct { int32_t user; int32_t pad; const TitleId *title; const DirName *dir; const void *fingerprint;
                 uint8_t reserved[32]; } BackupRequest;
typedef struct { int32_t user; int32_t pad; const TitleId *title; const DirName *dir; Param *param; Icon *icon;
                 uint8_t reserved[32]; } CheckBackup;
typedef struct { int32_t user; int32_t pad; const TitleId *title; const DirName *dir; const void *fingerprint;
                 uint32_t unused; uint8_t reserved[32]; int32_t pad2; } RestoreBackup;
typedef struct { int32_t user; int32_t pad; const TitleId *title; const DirName *dir; const void *fingerprint;
                 uint8_t reserved[32]; } TransferringMount;

/* TEMPEST_SAVE_TRACE=1: every call above with its result (polls only when the result changes). */
static int save_trace(void) { static int v=-1; if (v<0) { const char *e=getenv("TEMPEST_SAVE_TRACE"); v=e && e[0]=='1'; } return v; }
static int32_t traced(const char *call, const char *detail, int32_t r) {
    if (save_trace()) printf("Save trace: %s(%s) -> 0x%x\n",call,detail ? detail : "",(unsigned)r);
    return r;
}
static const char *point_name(const MountPoint *p) { return p ? p->data : "null"; }
static const char *dir_name(const DirName *d) { return d ? d->data : "null"; }
static ABI int32_t t_initialize(const void *p) { return traced("Initialize",NULL,save_initialize(p)); }
static ABI int32_t t_terminate(void) { return traced("Terminate",NULL,save_terminate()); }
static ABI int32_t t_mount2(const Mount2 *m, MountResult *r) {
    char d[64]; snprintf(d,sizeof(d),"%s, mode 0x%x",m ? dir_name(m->dir) : "null",m ? m->mode : 0);
    return traced("Mount2",d,save_mount2(m,r));
}
static ABI int32_t t_umount(const MountPoint *p) { return traced("Umount",point_name(p),save_umount(p)); }
static ABI int32_t t_set_param(const MountPoint *p, uint32_t type, const void *b, uint64_t n) {
    char d[48]; snprintf(d,sizeof(d),"%s, type %u",point_name(p),type);
    return traced("SetParam",d,save_set_param(p,type,b,n));
}
static ABI int32_t t_icon(const MountPoint *p, const Icon *i) { return traced("SaveIcon",point_name(p),save_icon(p,i)); }
static ABI int32_t t_delete(const Delete *d) { return traced("Delete",d ? dir_name(d->dir) : "null",save_delete(d)); }

/* sceSaveDataGetParam(point, type, buffer, size, out): the mirror of sceSaveDataSetParam,
 * reading back what was written for this mount. `out` receives the bytes produced. */
static ABI int32_t save_get_param(const MountPoint *point, uint32_t type, void *buffer,
                                  uint64_t size, uint64_t *out) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!buffer) return ERR_PARAMETER;
    pthread_mutex_lock(&lock);
    int slot=slot_of(point);
    if (slot<0) { pthread_mutex_unlock(&lock); return ERR_NOT_MOUNTED; }
    Param p; read_param(slots[slot].meta,&p);
    pthread_mutex_unlock(&lock);
    const void *source; uint64_t length;
    switch (type) {
    case 0: source=&p; length=sizeof(p); break;                                     /* ALL */
    case 1: source=p.title; length=strnlen(p.title,sizeof(p.title))+1; break;
    case 2: source=p.subtitle; length=strnlen(p.subtitle,sizeof(p.subtitle))+1; break;
    case 3: source=p.detail; length=strnlen(p.detail,sizeof(p.detail))+1; break;
    case 4: source=&p.user_param; length=sizeof(p.user_param); break;
    default: return ERR_PARAMETER;
    }
    if (size<length) return ERR_PARAMETER;
    memcpy(buffer,source,(size_t)length);
    if (out) *out=length;
    return 0;
}
static ABI int32_t t_get_param(const MountPoint *p, uint32_t type, void *b, uint64_t n, uint64_t *o) {
    char d[48]; snprintf(d,sizeof(d),"%s, type %u",point_name(p),type);
    return traced("GetParam",d,save_get_param(p,type,b,n,o));
}
static ABI int32_t t_memory_setup(const MemorySetup2 *s, MemorySetupResult *r) { return traced("SetupSaveDataMemory2",NULL,memory_setup(s,r)); }
static ABI int32_t t_memory_get(MemoryGet2 *g) { return traced("GetSaveDataMemory2",NULL,memory_get(g)); }
static ABI int32_t t_memory_set(const MemorySet2 *s) { return traced("SetSaveDataMemory2",NULL,memory_set(s)); }
static const RuntimeExport exports[]={
    {"sceSaveDataInitialize3",t_initialize}, {"sceSaveDataTerminate",t_terminate},
    {"sceSaveDataMount2",t_mount2}, {"sceSaveDataUmount",t_umount},
    {"sceSaveDataSetParam",t_set_param}, {"sceSaveDataSaveIcon",t_icon}, {"sceSaveDataDelete",t_delete},
    {"sceSaveDataGetParam",t_get_param},
    {"sceSaveDataSetupSaveDataMemory2",t_memory_setup}, {"sceSaveDataGetSaveDataMemory2",t_memory_get},
    {"sceSaveDataSetSaveDataMemory2",t_memory_set},
};
uintptr_t runtime_savedata_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
void runtime_savedata_report(void) { printf("Runtime: save data mounts=%zu, memory writes=%zu, backups=%zu\n",mounts_done,memory_writes,backups_done); }

