#!/usr/bin/env bash
# Install the APK and stage the user's own game separately.
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
if [[ $# -lt 3 || $# -gt 4 ]]; then
    echo 'Usage: install_android.sh ADB_SERIAL BOOT_LINKED_BIN GAME_DIR [EXTERNAL_FILES_DIR]' >&2
    exit 2
fi
device=$1
image=$2
game=$3
[[ -s $image && -s $game/eboot.bin && -s $(dirname -- "$image")/content.bin ]] || { echo 'Need a prepared image and decrypted PS4 game directory.' >&2; exit 1; }
sdk=${ANDROID_SDK_ROOT:-${ANDROID_HOME:-$HOME/Android/Sdk}}
adb=${ADB:-$sdk/platform-tools/adb}
[[ -x $adb ]] || adb=$(command -v adb)
package=com.crownparkcomputing.raidenv
remote=/sdcard/Android/data/$package/files
data=${4:-$remote}
[[ $data == /*/Android/data/$package/files && $data != *'/../'* ]] || {
    echo "Use this app's absolute Android/data/$package/files directory." >&2; exit 2;
}
"$adb" -s "$device" install -r out/android-app/raiden-v.apk
"$adb" -s "$device" shell mkdir -p "$data/app0" "$remote/user"
"$adb" -s "$device" push "$image" "$data/boot-linked.bin"
"$adb" -s "$device" push "$(dirname -- "$image")/content.bin" "$data/content.bin"
if [[ -s $(dirname -- "$image")/analysis.json ]]; then
    "$adb" -s "$device" push "$(dirname -- "$image")/analysis.json" "$data/analysis.json"
fi
"$adb" -s "$device" push "$game/." "$data/app0/"
echo "Installed Raiden V: Director's Cut and staged the game for $package"
echo "Game data: $data; settings, logs and saves: $remote"
