#!/usr/bin/env bash
# Build the shared FEXCore execution check as native Android ARM64 code.
# --glibc selects the separately tested Linux/glibc embedding build instead.
# TEMPEST_ANDROID_RENDERER=1 also builds main; set the SDL, headers, FFmpeg and
# libadrenotools dependency paths to reuse an existing pinned toolchain.
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
port=$PWD
mode=${1:---ndk}
[[ $mode == --ndk || $mode == --glibc ]] || { echo 'Usage: build_android_fex.sh [--ndk|--glibc]' >&2; exit 2; }
revision=f2b679f6028ce1c38875233aecfcf5d3f8ebecec
source_dir=${TEMPEST_FEX_SOURCE:-$port/out/android-deps/fex}
build_dir=${TEMPEST_FEX_BUILD:-$port/out/android-fex-ndk}
readonly_source=${TEMPEST_FEX_SOURCE_READONLY:-0}
[[ $readonly_source == 0 || $readonly_source == 1 ]] || {
    echo 'TEMPEST_FEX_SOURCE_READONLY must be 0 or 1.' >&2; exit 2;
}
for tool in git cmake ninja clang clang++; do
    command -v "$tool" >/dev/null || { echo "Need $tool for the ARM64/glibc build." >&2; exit 1; }
done
toolchain=()
if [[ $mode == --ndk ]]; then
    sdk=${ANDROID_SDK_ROOT:-${ANDROID_HOME:-$HOME/Android/Sdk}}
    ndk=${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}
    if [[ -z $ndk ]]; then
        mapfile -t installed < <(printf '%s\n' "$sdk"/ndk/*/source.properties | sort -V)
        for ((i=${#installed[@]}-1; i>=0; --i)); do
            if [[ -f ${installed[i]} ]]; then ndk=${installed[i]%/source.properties}; break; fi
        done
    fi
    [[ -f $ndk/build/cmake/android.toolchain.cmake ]] || { echo 'Set ANDROID_NDK_HOME to an installed NDK.' >&2; exit 1; }
    toolchain=(-DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake"
        -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-31)
else
    command -v aarch64-linux-gnu-gcc >/dev/null || { echo 'Need aarch64-linux-gnu-gcc for --glibc.' >&2; exit 1; }
    build_dir=${TEMPEST_FEX_BUILD:-$port/out/android-fex}
    toolchain=(-DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=aarch64
        -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
        -DCMAKE_C_COMPILER_TARGET=aarch64-linux-gnu -DCMAKE_CXX_COMPILER_TARGET=aarch64-linux-gnu
        -DCMAKE_FIND_ROOT_PATH=/usr/aarch64-linux-gnu)
fi
case ${TEMPEST_ANDROID_RENDERER:-0} in
    1|ON|on|TRUE|true|YES|yes) renderer=ON ;;
    0|OFF|off|FALSE|false|NO|no) renderer=OFF ;;
    *) echo 'TEMPEST_ANDROID_RENDERER must be 0 or 1.' >&2; exit 2 ;;
esac
renderer_options=(-DTEMPEST_ANDROID_RENDERER="$renderer")
targets=(tempest-fex-check tempest-boot-check)
if [[ $renderer == ON ]]; then
    [[ $mode == --ndk ]] || { echo 'The Android renderer requires --ndk.' >&2; exit 2; }
    : "${TEMPEST_ANDROID_SDL:?Set TEMPEST_ANDROID_SDL to an Android SDL3 install}"
    : "${TEMPEST_ANDROID_HEADERS:?Set TEMPEST_ANDROID_HEADERS to the staged renderer headers}"
    : "${TEMPEST_FFMPEG_DIR:?Set TEMPEST_FFMPEG_DIR to an Android FFmpeg install}"
    renderer_options+=(-DANDROID_STL=c++_shared -DCMAKE_POLICY_VERSION_MINIMUM=3.5
        -DTEMPEST_ANDROID_SDL="$TEMPEST_ANDROID_SDL"
        -DTEMPEST_ANDROID_HEADERS="$TEMPEST_ANDROID_HEADERS"
        -DTEMPEST_FFMPEG_DIR="$TEMPEST_FFMPEG_DIR"
        -DTEMPEST_ADRENOTOOLS_SOURCE="${TEMPEST_ADRENOTOOLS_SOURCE:-$port/out/android-deps/adrenotools}")
    targets+=(main)
fi
if [[ ! -e $source_dir/.git ]]; then
    [[ $readonly_source == 0 ]] || { echo 'The read-only FEX checkout is missing.' >&2; exit 1; }
    mkdir -p "$(dirname -- "$source_dir")"
    git clone --filter=blob:none --no-checkout --depth 1 https://github.com/FEX-Emu/FEX.git "$source_dir"
    git -C "$source_dir" fetch --depth 1 origin "$revision"
    git -C "$source_dir" checkout --detach "$revision"
fi
[[ $(git -C "$source_dir" rev-parse HEAD) == "$revision" ]] || {
    echo "FEX must be at $revision; refusing to replace an existing checkout." >&2; exit 1;
}
patch=$port/android/fexcore-only.patch
if git -C "$source_dir" apply --reverse --check "$patch" 2>/dev/null; then
    : # Already patched; a shared checkout needs no writes.
elif [[ $readonly_source == 1 ]]; then
    echo 'The read-only FEX checkout must already have the embedding patch.' >&2; exit 1
elif git -C "$source_dir" apply --check "$patch" 2>/dev/null; then
    git -C "$source_dir" apply "$patch"
else
    echo 'The FEX embedding patch does not match this checkout.' >&2; exit 1
fi
submodules=(External/unordered_dense External/rpmalloc External/xxhash External/fmt
    External/range-v3 Source/Common/cpp-optparse)
if [[ $readonly_source == 1 ]]; then
    module_states=$(git -C "$source_dir" submodule status -- "${submodules[@]}")
    while IFS= read -r state; do
        [[ ${state:0:1} == ' ' ]] || {
            echo 'The read-only FEX submodules must already match their pinned revisions.' >&2; exit 1;
        }
    done <<< "$module_states"
else
    git -C "$source_dir" submodule update --init --depth 1 --jobs 8 -- "${submodules[@]}"
fi
cmake -S "$source_dir" -B "$build_dir" -G Ninja \
    "${toolchain[@]}" "${renderer_options[@]}" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_DISABLE_FIND_PACKAGE_fmt=TRUE -DTUNE_CPU=none \
    -DBUILD_FEXCORE_ONLY=ON -DFEXCORE_EMBED_PROJECT="$port/android/FexTarget.cmake" \
    -DBUILD_TESTING=OFF -DBUILD_FEX_LINUX_TESTS=OFF -DBUILD_THUNKS=OFF -DBUILD_FEXCONFIG=OFF \
    -DENABLE_GDB_SYMBOLS=OFF -DENABLE_LTO=OFF -DENABLE_JEMALLOC_GLIBC_ALLOC=OFF \
    -DENABLE_OFFLINE_TELEMETRY=OFF -DENABLE_VIXL_DISASSEMBLER=OFF -DENABLE_VIXL_SIMULATOR=OFF \
    -DENABLE_ZYDIS=OFF -DENABLE_FEXCORE_PROFILER=OFF
cmake --build "$build_dir" --target "${targets[@]}" --parallel "${TEMPEST_BUILD_JOBS:-8}"
echo "Built $build_dir/Bin/tempest-fex-check"
echo "Built $build_dir/Bin/tempest-boot-check"
if [[ $renderer == ON ]]; then echo "Built $build_dir/android-libs/libmain.so"; fi
