/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <SDL3/SDL.h>
#include <adrenotools/driver.h>
#include <android/asset_manager_jni.h>
#include <android/log.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vulkan/vulkan.h>
#include "graphics_driver.h"

int tempest_android_boot(const char *image, const char *app0, const char *user);
namespace {
PFN_vkGetInstanceProcAddr vulkan_proc;
void launch_settings(const std::string& directory) {
    FILE* file=fopen((directory+"/launch.ini").c_str(),"r");
    if (!file) return;
    char line[4096];
    while (fgets(line,sizeof(line),file)) {
        char* equals=strchr(line,'=');
        if (!equals || strncmp(line,"TEMPEST_",8)) continue;
        *equals++=0;
        bool valid=true;
        for (const char* p=line; *p; ++p)
            if (!(*p=='_' || (*p>='A' && *p<='Z') || (*p>='0' && *p<='9'))) valid=false;
        if (!valid) continue;
        equals[strcspn(equals,"\r\n")]=0;
        setenv(line,equals,1);
    }
    fclose(file);
}
std::string libraries(JNIEnv *env, jobject activity) {
    jclass type=env->GetObjectClass(activity);
    jmethodID method=env->GetMethodID(type,"getApplicationInfo","()Landroid/content/pm/ApplicationInfo;");
    jobject info=env->CallObjectMethod(activity,method);
    jclass info_type=env->GetObjectClass(info);
    jfieldID field=env->GetFieldID(info_type,"nativeLibraryDir","Ljava/lang/String;");
    auto value=static_cast<jstring>(env->GetObjectField(info,field));
    const char *text=env->GetStringUTFChars(value,nullptr);
    std::string result(text);
    env->ReleaseStringUTFChars(value,text);
    env->DeleteLocalRef(value); env->DeleteLocalRef(info_type);
    env->DeleteLocalRef(info); env->DeleteLocalRef(type);
    return result;
}
std::string game_directory(const std::string& fallback) {
    auto *env=static_cast<JNIEnv *>(SDL_GetAndroidJNIEnv());
    auto activity=static_cast<jobject>(SDL_GetAndroidActivity());
    if (!env || !activity) throw std::runtime_error("Android activity unavailable");
    jclass type=env->GetObjectClass(activity);
    jmethodID method=env->GetMethodID(type,"getGameDataDirectory","()Ljava/lang/String;");
    if (!method) {
        env->ExceptionClear(); env->DeleteLocalRef(type); env->DeleteLocalRef(activity);
        throw std::runtime_error("Android game-directory selection unavailable");
    }
    auto value=static_cast<jstring>(env->CallObjectMethod(activity,method));
    if (env->ExceptionCheck()) {
        env->ExceptionClear(); env->DeleteLocalRef(type); env->DeleteLocalRef(activity);
        throw std::runtime_error("Android game-directory selection failed");
    }
    std::string result=fallback;
    if (value) {
        const char *text=env->GetStringUTFChars(value,nullptr);
        if (!text) {
            env->ExceptionClear(); env->DeleteLocalRef(value);
            env->DeleteLocalRef(type); env->DeleteLocalRef(activity);
            throw std::runtime_error("Android game-directory string unavailable");
        }
        result=text; env->ReleaseStringUTFChars(value,text);
        env->DeleteLocalRef(value);
    }
    env->DeleteLocalRef(type); env->DeleteLocalRef(activity);
    return result;
}
void driver() {
    auto *env=static_cast<JNIEnv *>(SDL_GetAndroidJNIEnv());
    auto activity=static_cast<jobject>(SDL_GetAndroidActivity());
    if (!env || !activity) throw std::runtime_error("Android activity unavailable");
    const std::string directory=SDL_GetAndroidInternalStoragePath();
    vulkan_proc=Ps4Android::LoadGraphicsDriver(env,activity,libraries(env,activity),directory);
    env->DeleteLocalRef(activity);
    puts("Android: selected Vulkan driver ready");
}
}
extern "C" int ps4_android_open_driver_settings(void) {
    return Ps4Android::OpenGraphicsDriverSettings() ? 1 : 0;
}
extern "C" PFN_vkGetInstanceProcAddr tempest_android_vulkan_proc() { return vulkan_proc; }
extern "C" SDL_DECLSPEC int SDL_main(int, char **) {
    int result=1;
    try {
        const char *external=SDL_GetAndroidExternalStoragePath();
        if (!external) throw std::runtime_error("Android game directory unavailable");
        std::string directory(external);
        auto *log=fopen((directory+"/raiden-v.log").c_str(),"w");
        if (!log) throw std::runtime_error("cannot open Android game log");
        if (dup2(fileno(log),STDOUT_FILENO)<0 || dup2(fileno(log),STDERR_FILENO)<0)
            throw std::runtime_error("cannot redirect Android game log");
        fclose(log);
        setvbuf(stdout,nullptr,_IOLBF,0); setvbuf(stderr,nullptr,_IOLBF,0);
        puts("Raiden V: Director's Cut Android startup");
        launch_settings(directory);
        std::filesystem::create_directories(directory+"/user");
        setenv("TEMPEST_FEX_MAX_INST","500",0);
        // Resolve draw resources serially while this title is validated.
        setenv("TEMPEST_DRAW_PIPE","0",0);
        setenv("TEMPEST_PREP_WORKERS","0",0);
        setenv("TEMPEST_RENDER_RESOLUTION","1080p",0);
        // Restored Android pipelines can produce black frames; compile on demand for now.
        setenv("TEMPEST_PIPELINE_CACHE","0",0);
        setenv("TEMPEST_DISK_SHADERS","0",0);
        setenv("TEMPEST_DISPLAY_MODE","borderless",0);
        setenv("TEMPEST_OUTPUT_RESOLUTION","1280x720",0);
        setenv("TEMPEST_GPU_USER_DIR",(directory+"/user").c_str(),1);
        const std::string game=game_directory(directory);
        printf("Android: game data directory %s\n",game.c_str());
        driver();
        result=tempest_android_boot((game+"/boot-linked.bin").c_str(),
            (game+"/app0").c_str(),(directory+"/user").c_str());
    } catch (const std::exception &e) {
        fprintf(stderr,"STOP: Android: %s\n",e.what());
        __android_log_print(ANDROID_LOG_ERROR,"RaidenV","%s",e.what());
    }
    fflush(nullptr);
    _exit(result);
}
