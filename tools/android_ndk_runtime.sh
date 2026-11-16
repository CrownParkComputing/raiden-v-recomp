#!/usr/bin/env bash
# Shared by the explicitly invoked headless runners; this file never runs ADB by itself.
android_stage_cpp_runtime() {
    local runtime_adb=$1 runtime_device=$2 runtime_remote=$3 runtime_build=$4 runtime_sdk=$5
    local runtime_ndk=${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}
    if [[ -z $runtime_ndk && -f $runtime_build/CMakeCache.txt ]]; then
        runtime_ndk=$(sed -n 's/^CMAKE_ANDROID_NDK:[^=]*=//p' "$runtime_build/CMakeCache.txt")
    fi
    if [[ -z $runtime_ndk ]]; then
        local runtime_installed
        mapfile -t runtime_installed < <(printf '%s\n' "$runtime_sdk"/ndk/*/source.properties | sort -V)
        local runtime_index
        for ((runtime_index=${#runtime_installed[@]}-1; runtime_index>=0; --runtime_index)); do
            if [[ -f ${runtime_installed[runtime_index]} ]]; then
                runtime_ndk=${runtime_installed[runtime_index]%/source.properties}
                break
            fi
        done
    fi
    local runtime_cpp=$runtime_ndk/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so
    [[ -s $runtime_cpp ]] || { echo 'Set ANDROID_NDK_HOME to the NDK used to build the ARM64 checks.' >&2; return 1; }
    "$runtime_adb" -s "$runtime_device" push "$runtime_cpp" "$runtime_remote/libc++_shared.so"
}
