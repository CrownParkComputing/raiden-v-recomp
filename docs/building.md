# Build Raiden V: Director's Cut

The development targets are Linux x86-64 and Android ARM64. The supported
input is Raiden V: Director's Cut CUSA07673 v1.02. See the current [port status](raiden-v-port-status.md).

## Clone

```bash
git clone https://github.com/CrownParkComputing/raiden-v-recomp.git
cd raiden-v-recomp
```

Renderer dependencies under `gpu/third_party/` are vendored, including their
licences. No Git submodule setup is needed. Game files, prepared images, drivers,
Android signing keys, saves and build outputs stay outside Git.

## Linux prerequisites

Use a Linux x86-64 host with Clang supporting C++23, CMake 3.24+, Ninja, Git,
Python 3.10+, pkg-config, make, curl, xz and nasm. The renderer needs Vulkan
headers including Vulkan-Hpp, Vulkan loader, glslang, SDL3, fmt, Boost, robin-map,
xxHash, Zydis, zlib, X11 and Vulkan Memory Allocator headers.

On Ubuntu 24.04 the same installer used by CI installs system packages and builds
the missing SDL3/Vulkan headers. It uses sudo and installs into `/usr/local`:

```bash
bash .github/scripts/ci-deps.sh
export CC=clang-18 CXX=clang++-18
```

On another distribution install the equivalent development packages first.
A Vulkan-capable GPU/driver and desktop session are needed to play; source
compilation and most runtime checks work without a game or display.

## Build and check Linux

```bash
bash build.sh
```

Outputs are `out/raiden-v` and the inherited `out/tempest` runtime name. The
`out/tempest-setup` helper supplies the extractor's native XTS operation. The
build links minimal FFmpeg 9.0.2, unpacking the inherited Linux archive when its
options match, or downloading hash-checked source otherwise. Set
`TEMPEST_FFMPEG_PREBUILT=0` to build FFmpeg from source.

```bash
bash build.sh --test
(cd tests && python3 -m unittest discover -p 'test_*.py')
```

Tests requiring a missing game, graphics device or optional dependency may skip.

## Supply your game

Use your own decrypted CUSA07673 v1.02 dump under ignored `game/`:

```text
game/eboot.bin
game/sce_module/libc.prx
game/sce_module/libSceFios2.prx
game/sce_sys/param.sfo
game/sce_module/libSceNpToolkit2.prx
game/rom/sc_boot.mma
game/rom/sc_menu.mma
```

Extract a matching base and update separately, then overlay update files onto a
copy of the base dump. Keep the original packages and extraction folders.
The supported pair is CUSA07673 base 01.00 plus update 01.02 with content ID
`UP2035-CUSA07673_00-RAIDEN5ENCA70707`:

```bash
python3 scripts/pkg_extract.py /path/to/base.pkg --out out/base --xts-tool out/tempest-setup
python3 scripts/pkg_extract.py /path/to/update.pkg --out out/update --xts-tool out/tempest-setup
cp -a out/base/. game/
cp -a out/update/. game/
```

Only overlay an update after checking its title/content ID matches the base.
The current source does not redistribute game data or automate package selection.

The verified executable SHA-256 is:
`c8fcfe9abd75c1e10c5a6506a1bf1e1548a20f75d115ad5e4aa3b39a1638c005`.
Other builds require investigation; `--any-build` is not a compatibility promise.
The original dump is never rewritten.

## Prepare and play

```bash
python3 run.py prepare --game game
./play-raiden-v.sh
```

Use `--game /path/to/dump`, `--user /path/to/profile` or `--timeout 60` as needed.
Prepared images stay under `out/game/`. Saves are under
`user/savedata/1/CUSA07673.memory/` for the save-memory API; Android has a separate profile.
Caches default to `~/.cache/raiden-v/` and `out/shaders/`.

The shared environment names remain `TEMPEST_*`. Raiden V: Director's Cut uses its own frame clock
and aspect ratio; the fixed addresses from earlier games are disabled.
Both draw pipelining and speculative draw preparation default to off on Linux
and Android, carrying the safe profile established by Akka Arrh. Explicit `TEMPEST_DRAW_PIPE`
and `TEMPEST_PREP_WORKERS` overrides are available for diagnostics.
L3 + R3 opens Settings; F10 and holding Select for two seconds also work.

## First start

Use the original title screen and menus. Options/Start begins or pauses,
Cross/A confirms and fires, and the direction controls move the ship. L3+R3
opens the port Settings panel; F10 also opens it on Linux. Keep a separate
profile while testing a new runtime. The online side panels report offline
network state; local Stage 1 gameplay has been verified.

## Android

Follow [android/README.md](../android/README.md) for dependencies, ARM64/FEX,
Turnip packaging, signing, installation and hardware checks. A fresh checkout
can build without paths to another port's workspace.

## Automation and release scope

CI builds Linux and runs runtime, Python and driver-import checks on main/PRs.
Game files, original launcher artwork, driver ZIPs, signing keys and prepared
images remain local. The inherited GUI setup and release packager are not the
Raiden V: Director's Cut distribution flow. Historical notes are retained under `docs/legacy/`.
See [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md) for source origins.
