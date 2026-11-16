# Raiden V: Director's Cut on Android

Target input is Raiden V: Director's Cut, CUSA07673 base 01.00 with update
01.02 applied, content ID `UP2035-CUSA07673_00-RAIDEN5ENCA70707`.
The latest overlaid `eboot.bin` SHA-256 is
`c8fcfe9abd75c1e10c5a6506a1bf1e1548a20f75d115ad5e4aa3b39a1638c005`.

The application ID is `com.crownparkcomputing.raidenv`, activity
`.RaidenVActivity`, label **Raiden V: Director's Cut**, version 0.1/code 1.
It installs separately from the other ports. PS4 executables and game data
stay outside the APK. Stage the complete game tree with the update applied.

The Android target is Retroid Pocket Flip2, Snapdragon 865/Adreno 650, ARM64,
API 31 or later. Device boot, rendering, audio and performance are not yet
verified for this title. This guide describes the build and staging procedure.

## Host prerequisites

Use JDK 17 or newer, `zip`, Android SDK platform 33 or newer, build-tools,
platform-tools, and an ARM64 NDK. The shared toolchain uses NDK
`30.0.14904198`; native code targets API 31.

```bash
export ANDROID_SDK_ROOT="$HOME/Android/Sdk"
export ANDROID_NDK_HOME="$ANDROID_SDK_ROOT/ndk/30.0.14904198"
export PATH="$ANDROID_SDK_ROOT/platform-tools:$PATH"
```

## Dependencies and build

To prepare dependencies entirely under this checkout:

```bash
bash tools/setup_android_deps.sh
source out/android-env.sh
```

This stages the required renderer headers and builds SDL3, pinned libadrenotools
and Android FFmpeg. It does not copy host libc headers into the NDK include path.
See [the build guide](../docs/building.md) for host packages.

For an existing local toolchain, sibling dependency trees can be reused without
building inside them. These paths are the local development layout; change them
for another machine:

```bash
export TEMPEST_ANDROID_RENDERER=1
export TEMPEST_ANDROID_SDL=/home/jon/nativ/toolchain-android/SDL3
export TEMPEST_ANDROID_SDL_SOURCE=/home/jon/nativ/toolchain-android/sdl-src
export TEMPEST_ANDROID_HEADERS=/home/jon/tempest4000-recomp/out/android-headers
export TEMPEST_FFMPEG_DIR=/home/jon/tempest4000-recomp/out/android-ffmpeg
export TEMPEST_ADRENOTOOLS_SOURCE=/home/jon/tempest4000-recomp/out/android-deps/adrenotools
export TEMPEST_FEX_SOURCE=/home/jon/tempest4000-recomp/out/android-deps/fex
export TEMPEST_FEX_SOURCE_READONLY=1
export TEMPEST_FEX_BUILD="$PWD/out/android-fex-ndk"
export TEMPEST_BUILD_JOBS=4
```

The reused FEX source must be revision
`f2b679f6028ce1c38875233aecfcf5d3f8ebecec`, already patched with
`android/fexcore-only.patch`, with its selected submodules initialized. The build
script checks those conditions and refuses to modify a read-only checkout.
Build output belongs to this port's own `out/android-fex-ndk`.

After the title's runtime changes are frozen, build the runtime and headless
check targets:

```bash
bash tools/build_android_fex.sh --ndk
```

This builds `main`, `tempest-fex-check` and `tempest-boot-check`. Generated FEX
adapters must include newly added runtime modules. Do not reuse another port's
`libmain.so` or build directory.

## Driver and APK

Use the Standard Adreno 6xx/7xx [Turnip 27.0 November 11 r2 build](https://github.com/The412Banner/Banners-Turnip/releases/tag/v27.0.0-20261111-r2),
`Turnip-v27.0.0-20261111-r2.zip`. ZIP SHA-256:
`bcc118d8a8bb4206f2012c1941252f46eb92b3da5cc579af368ac56fb2769271`.
Library SHA-256:
`472c1d0b4b415229facc3015e73dd4f5c8298cdbc7207d5df8b60edfda64039e`.
Its KGSL zero-timeout fix avoids the blocking completion checks observed with
older R5 builds in the earlier ports. The driver is staged from a local ZIP;
`assets/driver-source.json` records exact APK provenance.

```bash
bash tools/package_android.sh /path/to/Turnip-v27.0.0-20261111-r2.zip
```

Output is `out/android-app/raiden-v.apk`. The script checks APK signing.
When available, the local dump's `game/sce_sys/icon0.png` supplies launcher art;
an explicit second argument or `TEMPEST_ANDROID_ICON` overrides it. Without local
artwork, packaging uses the project-owned vector icon. Do not commit copied PS4
artwork or signing keys. Packaging stages only the runtime, driver and launcher
resources, with no PS4 executables or gameplay resources.

The usual Android debug key is `$HOME/.android/debug.keystore`. Keep the same key
for subsequent `install -r` updates. Packaging requires an existing key.

## Prepare and stage the game

Prepare the owned full game directory with the matching update applied:

```bash
python3 run.py prepare --game game
bash tools/install_android.sh ADB_SERIAL out/game/boot-linked.bin game
adb -s ADB_SERIAL shell am start -n com.crownparkcomputing.raidenv/.RaidenVActivity
```

The installer uses `install -r`, stages the image and `content.bin`, and copies the
entire game tree separately. An optional fourth argument stages those files in
this package's SD-card files directory when primary storage cannot hold the game:

```bash
bash tools/install_android.sh ADB_SERIAL out/game/boot-linked.bin game \
  /storage/FEDD-B1FF/Android/data/com.crownparkcomputing.raidenv/files
```

The application chooses among its own `getExternalFilesDirs(null)` directories.
A directory is usable when it contains both `boot-linked.bin` and
`app0/eboot.bin`; a complete primary copy takes precedence. Otherwise a complete
SD copy is selected, falling back to primary when neither copy is complete.
`content.bin` and optional `analysis.json` travel with the prepared image. No
broad storage permission or arbitrary-directory picker is used.

For a runtime-only update after staging:

```bash
adb -s ADB_SERIAL install -r out/android-app/raiden-v.apk
```

Settings, `launch.ini`, logs, user saves and caches remain under the primary
external-files directory even when game files are on the SD card:
`/sdcard/Android/data/com.crownparkcomputing.raidenv/files/`.
The main log is `raiden-v.log`; `launch.ini` accepts literal
`TEMPEST_NAME=value` lines. The save title ID is CUSA07673, in this app's own
`user/` profile. Coordinate device tests before interrupting a running game.

## Controls and graphics driver selection

SDL supplies the gamepad input. L3+R3 opens the shared internal Settings panel.
Choose **Graphics driver** to select bundled or
system Vulkan, or import a compatible ARM64 driver ZIP. The launcher also exposes
the **Graphics driver** shortcut. Changes apply after closing and reopening this
app. See [driver settings and recovery](../docs/android-driver-selection.md).

## Initial profile and execution checks

Defaults are serial draw recording (`TEMPEST_DRAW_PIPE=0`), preparation on the
GPU thread (`TEMPEST_PREP_WORKERS=0`), FEX MaxInst 500, the shared 1080p render
setting with 1280x720 host output, and on-demand shader compilation. Device
captures show 1920x1080 guest display buffers and an 896x1024 scene color target;
the host output size does not determine those guest targets. No per-title
intro-skip override is inherited. `launch.ini` can override the shared settings;
normal play has no diagnostic timeout.

`TEMPEST_VK_DEFER_COUNTER_REFRESH=1` enables experimental on-demand GPU
completion polling for a fresh process; its default is off. Completed ticks
advance only after a successful wait/poll or a real counter query. The resource
pool rereads the refreshed completion value before reusing an allocation.
These synchronization changes have focused regressions; this option is not a
claim of improved frame rate on every device.

The latest executable's 80-byte procparam at guest offset `0x2d4000` supplies SDK
`0x04508101` at offset +16; Android reads it from the prepared image. GPU setup
receives the SFO ATTRIBUTE `0x800020`; CPU-mode bits 15 and 16 are both clear,
giving mode 0. Package-tool SDK timestamps are not used as the process SDK.

The separate headless checks require an explicit device serial and stage into
`/data/local/tmp/raiden-v-fex`, preserving other ports' test directories:

```bash
bash tools/run_android_fex.sh ADB_SERIAL
bash tools/run_android_boot.sh ADB_SERIAL out/game/boot-linked.bin
```

The scripts stage the matching NDK `libc++_shared.so`. For a full headless entry
probe, stage the complete game into that directory's `app0/` first, then add
`--entry` to the boot-check command. Headless checks and a successful APK build do
not establish Android compatibility or gameplay performance.
