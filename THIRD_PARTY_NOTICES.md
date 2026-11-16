# Source and dependencies

The runner is GPL-2.0-or-later; see [LICENSE](LICENSE). Existing source copyright
and SPDX notices are retained.

- This port starts from [CrownParkComputing/akka-arrh-recomp](https://github.com/CrownParkComputing/akka-arrh-recomp), including its shared Android driver menu and stable draw defaults.
- The runtime builds on [CrownParkComputing/tempest4000-recomp](https://github.com/CrownParkComputing/tempest4000-recomp) and its inherited runtime work.
- The Vulkan renderer and shader compiler derive from [shadPS4](https://github.com/shadps4-emu/shadPS4). Source notices remain under `gpu/shadps4`.
- The FEX embedding bridge comes from [zenithblue-oss/shadps4-arm64](https://github.com/zenithblue-oss/shadps4-arm64), revision `be6bc2e9c60799e071dd2fafa6216e8d80ec619c`. Its MIT and GPL notices are in `third_party/fex_bridge/`.
- FEXCore is downloaded separately at `f2b679f6028ce1c38875233aecfcf5d3f8ebecec`; the embedding patch is `android/fexcore-only.patch`.
- LibAtrac9 and the vendored renderer dependencies retain their licences in `third_party/` and `gpu/third_party/`.
- Dear ImGui, magic_enum, miniz and xbyak are vendored source snapshots, rather than Git submodules. Their inherited revisions are `f1cc2ae15e53a861a874c3034aae6798fde194ab`, `e046b69a3736d314fad813e159b1c192eaef92cd`, `2948eebafc5b9f1c69bebed80333e403a09bd62d` and `cd6bad3cad2e07a3e277c23d2b1f065320024717`, respectively.
- The minimal FFmpeg build uses version 9.0.2; version, source URL, hash and build options are recorded in `tools/build_ffmpeg.sh` and `tools/build_android_ffmpeg.sh`. The inherited Linux archive is under `prebuilt/`.
- SDL3 3.4.16 and libadrenotools `8fae8ce254dfc1344527e05301e43f37dea2df80` are external Android dependencies. The setup script downloads them separately.
- Turnip is supplied as an external driver ZIP. The APK records its archive metadata and SHA-256 hashes; the driver is not committed here.

Game executables, modules, prepared images, media, saves and package files are
local inputs and are excluded from this repository. Game names and artwork identify
the supported title; the game is not licensed by the runtime's GPL licence.

The Raiden Fighters port also derives from the shared runtime in [Polybius](https://github.com/CrownParkComputing/polybius-recomp). Original game resources remain locally supplied.

The Raiden V port derives from [Raiden Fighters](https://github.com/CrownParkComputing/raiden-fighters-recomp), including the PFS inode-boundary fix, platform APIs, mixed-case asset lookup and NGS2 streaming callbacks.
