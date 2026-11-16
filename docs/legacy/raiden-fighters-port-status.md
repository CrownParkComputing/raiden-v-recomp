# Raiden Fighters Remix Collection port status

Supported input: CUSA53688 base v1.00 plus matching update v1.20, content ID
`HP5035-CUSA53688_00-RFPRODUCTION0PS4`. Latest executable SHA-256:
`c2e3a0879fed1f5d1269c2c9d9ad67fc5a0e4debe100be46ab9b93859766e3e4`.

## Input and preparation

The original base and update are preserved separately. The base resource index
contains 5,109 files and 5,210,302,983 bytes. Extracting the base and overlaying
the matching update yields 5,174 files; the final executable hash matches.
The original executable and all twelve bundled modules link successfully.
This establishes input/preparation compatibility, not playable behavior.

The original extractor read inode records continuously across 64 KiB blocks.
This dump packs 390 unsigned 168-byte records into each block, followed by 16
padding bytes. The corrected reader respects that boundary. Regression checks
cover signed and unsigned records and reads across noncontiguous file blocks.

## Runtime work

The initial graphics-enabled import check found ten unprovided functions in
the main executable: the six Audiodec lifecycle/decode calls, PadGetHandle,
RtcGetCurrentClockLocalTime, PthreadCancel and NpGetNpReachabilityState.
The check exits before guest execution. Additional missing imports in deferred
bundled modules do not establish which optional code paths the game will use.

The executable's measured SDK is 0x05050001. ATTRIBUTE=0x800020 requests CPU
mode 0. No earlier game's fixed-address timing/aspect patches are enabled.
Draw recording and preparation retain the serial defaults verified in Akka.

Linux boots the original collection title, game-selection menu and Arcade Mode
menu, and renders the original arcade attract sequence with side artwork.
Steady sections run at 60 FPS, with brief compilation/loading pauses. Player
stages have been reached in Raiden Fighters, Raiden Fighters 2 and Raiden
Fighters Jet. Raiden Fighters also reached game-over/continue flow. These checks
do not cover every regional variant, level or retry path.

The verified start sequence uses separate Options/Start presses: one after
arcade boot reaches INSERT COIN, then another at the credits/player-start
prompt to start player 1. Release between presses; early presses during boot
can be ignored. Cross/A confirms fighter/name selection and shoots. See the
[first-start instructions](building.md#first-start).

The original NGS2 adapter replaced each submitted PCM block and did not deliver
block-end callbacks, leaving only the last short staging block. The corrected
queue preserves the three buffers, combines logical windows with their appended
data, and dispatches real guest callbacks so the game can refill them.
An 80-second run delivered 1,945 callbacks and 4,311 audible buffers out of 7,500,
then exited normally. PCM samples are finite. The independent Audiodec
regression decodes real hand-authored ATRAC9 input into nonzero PCM.

A separate profile creates SaveData/savedata.bin and trophy_stats.bin. Longer
play, save reload and Android performance are pending.
The Android package is `com.crownparkcomputing.raidenfighters`, with
the shared L3+R3 settings menu and bundled/system/imported graphics drivers.

## Private evidence

Input identities, hashes, the original package findings and import checks are
retained locally. `out/` contains extraction, preparation, native/Android build
logs and focused ABI contracts. No original game assets or drivers are included
in the public source tree.

## Build and installation checks

The native runtime/renderer checks pass, including streaming callback order, reset/stop
invalidation, native dispatch and the FEX callback-dispatch fixture, extraction, thread cleanup
and cancellation, case-sensitive save isolation, and decoder error/lifecycle
checks. Python checks pass 92 tests with 17 optional skips. Driver ZIP fixtures
pass. Android main, boot-check and FEX-check targets build, and APK signing,
manifest, ARM64 libraries, launcher icon and bundled-driver provenance pass.

The separate app is installed on the Retroid. Its full 5,174-file game tree is
staged in its own SD-card external-files directory, with all sizes checked and
the prepared-image, executable and SFO hashes matched against the host.
The installed APK is version 0.2/code 2 and its hash matches the signed build.
Settings, logs and saves stay in primary storage. Device launch is awaiting the
user's availability so the existing foreground app can remain undisturbed.

Deferred cancellation is currently consumed at safe sleep checkpoints; other
blocking POSIX wait cancellation semantics are not yet implemented. The optional
exception-trace diagnostic is disabled: its inherited Linux scanner does not
safely cover gaps between this dump's module segments. Normal boot does not use
that diagnostic.
