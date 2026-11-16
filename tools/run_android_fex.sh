#!/usr/bin/env bash
# Stage and run the shared FEX runtime check in Raiden V's isolated device directory.
# Usage: bash tools/run_android_fex.sh ADB_SERIAL
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
device=${1:?Usage: bash tools/run_android_fex.sh ADB_SERIAL}
build_dir=${TEMPEST_FEX_BUILD:-$PWD/out/android-fex-ndk}
bundle=$PWD/out/android-runtime
sdk=${ANDROID_SDK_ROOT:-${ANDROID_HOME:-$HOME/Android/Sdk}}
adb=${ADB:-$sdk/platform-tools/adb}
if [[ ! -x $adb ]]; then adb=$(command -v adb); fi
remote=/data/local/tmp/raiden-v-fex
[[ -s $build_dir/Bin/tempest-fex-check ]] || {
    echo 'Build first: bash tools/build_android_fex.sh' >&2; exit 1;
}
"$adb" -s "$device" shell mkdir -p "$remote"
if readelf -l "$build_dir/Bin/tempest-fex-check" | grep -q '/system/bin/linker64'; then
    "$adb" -s "$device" push "$build_dir/Bin/tempest-fex-check" "$remote/tempest-fex-check"
    source tools/android_ndk_runtime.sh
    android_stage_cpp_runtime "$adb" "$device" "$remote" "$build_dir" "$sdk"
    "$adb" -s "$device" shell env "LD_LIBRARY_PATH=$remote" "$remote/tempest-fex-check"
    exit
fi
python3 tools/stage_android_runtime.py "$build_dir/Bin/tempest-fex-check" --out "$bundle"
"$adb" -s "$device" push "$bundle"/* "$remote/"
"$adb" -s "$device" shell env GLIBC_TUNABLES=glibc.pthread.rseq=0 \
    "$remote/ld-linux-aarch64.so.1" --inhibit-cache --library-path "$remote" "$remote/tempest-fex-check"
