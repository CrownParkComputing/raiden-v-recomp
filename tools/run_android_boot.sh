#!/usr/bin/env bash
# Run the headless libc boot check against the user's locally prepared game image.
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
device=${1:?Usage: run_android_boot.sh ADB_SERIAL BOOT_LINKED_BIN}
image=${2:?Usage: run_android_boot.sh ADB_SERIAL BOOT_LINKED_BIN [--entry]}
extra=()
if [[ ${3:-} == --entry ]]; then extra=(--entry); fi
if [[ $# -gt 3 || ( $# == 3 && ${3:-} != --entry ) ]]; then
    echo 'Usage: run_android_boot.sh ADB_SERIAL BOOT_LINKED_BIN [--entry]' >&2; exit 2
fi
build_dir=${TEMPEST_FEX_BUILD:-$PWD/out/android-fex-ndk}
sdk=${ANDROID_SDK_ROOT:-${ANDROID_HOME:-$HOME/Android/Sdk}}
adb=${ADB:-$sdk/platform-tools/adb}
if [[ ! -x $adb ]]; then adb=$(command -v adb); fi
[[ -f $image ]] || { echo "Missing prepared image: $image" >&2; exit 1; }
[[ -f $build_dir/Bin/tempest-boot-check ]] || {
    echo 'Build first: bash tools/build_android_fex.sh' >&2; exit 1;
}
profile=$(dirname -- "$image")/content.bin
[[ -s $profile ]] || { echo 'Need content.bin beside the prepared image.' >&2; exit 1; }
remote=/data/local/tmp/raiden-v-fex
"$adb" -s "$device" shell mkdir -p "$remote/app0" "$remote/user"
"$adb" -s "$device" push "$build_dir/Bin/tempest-boot-check" "$remote/tempest-boot-check"
"$adb" -s "$device" push "$image" "$remote/boot-linked.bin"
"$adb" -s "$device" push "$profile" "$remote/content.bin"
source tools/android_ndk_runtime.sh
android_stage_cpp_runtime "$adb" "$device" "$remote" "$build_dir" "$sdk"
"$adb" -s "$device" shell env "LD_LIBRARY_PATH=$remote" "$remote/tempest-boot-check" \
    "$remote/boot-linked.bin" "$remote/app0" "$remote/user" "${extra[@]}"
