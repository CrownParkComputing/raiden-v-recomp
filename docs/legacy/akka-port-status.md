# Akka Arrh port status

Input: CUSA36609 v1.00, executable SHA-256
`d6a1c0a179b4d9a4a9884c3f2e172db9d262bc8f33107335b9d03bb799075cfb`.
The original game remains a private local input. This is a native PS4 runtime
port, using direct x86 execution on Linux and FEXCore on Android ARM64.

## Verified on Linux

- libc and Fios2 initialize; the original main entry runs.
- Title, score table and introductory gameplay render. Measured stable intervals
  run at 60.0 FPS on the host RTX3060, with median frame time 16.67 ms.
- Both audio ports submit approximately 469 buffers per 5 seconds, with zero
  starvation in the measured runs. Actual output contains nonzero PCM; this
  does not independently verify every music cue or effect.
- The SaveData thread creates SAVEDATA00 and writes TEST_SAVEDATA.bin.
- Original fonts, logo, starfield and gameplay geometry are visible in captures.
- User reported title text flashing over other text with the inherited draw
  pipeline enabled. A controlled `TEMPEST_DRAW_PIPE=0` run clears the issue,
  confirmed by the user, while retaining 60 FPS. Serial draw recording is now the
  default. `TEMPEST_DRAW_PIPE=1` remains a diagnostic override; the exact race
  in the parallel recording path is not proven.

## Boot contracts corrected

The package stores lower-case `assets`, while the executable requests
`/app0/Assets/Localisation/Akka_Strings_PS4_Eng.csv`. A mount alias resolves
this without changing the dump. The missing UI resources caused the initial
null dereference at guest0x3e717; it was not an audio-handle fault.

SDK libc uses POSIX pthread condition-variable broadcasts while releasing C++
guards. The shared runtime's real condition-variable implementation handles
its NID, including zero-initialized static conditions. SFO ATTRIBUTE0x810002
selects SDK CPU mode5. No fixed addresses from Tempest or CE2 are patched.

Audio bank inspection identifies 190 stereo 44,100 Hz PCM16 waves. The resource
XML explicitly says Akka has no standalone soundtracks. The game controls
sequencing; Tempest's fixed track mapping is not used.

## Android structure

Separate package `com.crownparkcomputing.akkaarrh`, label Akka Arrh. ARM64 main,
headless boot and FEX checks compile. The bundled driver is Standard Turnip 27,
November11 build r2, containing the KGSL zero-timeout fix verified for CE2.

L3+R3 toggles Settings. Graphics driver opens the Android selector with Bundled,
System and Import driver ZIP. The importer validates metadata, ARM64 ELF and
Android API, stores immutable hash-named libraries and applies changes at the
next launch. The launcher shortcut provides recovery independently of rendering.

The signed APK and all 96 game files are installed on the Retroid. The current
Android build is v0.2 (versionCode 2). Its installed APK and bundled driver hashes
are verified, and installation preserves the existing save files.

The first hardware run exposed jumping text and very large, misplaced geometry
on the title screen. It also occurs after a cold start, independently of opening
the graphics-driver activity. `TEMPEST_PREP_WORKERS=0`, with other settings
unchanged, clears the corruption: before/after video frames show fixed text and
normal model size. Speculative draw preparation is disabled by default on
both platforms; resource selection runs on the GPU thread. The precise resource
whose snapshot changes has not been traced. Android additionally sets this
application default explicitly, while the shared renderer now defaults to zero
workers for direct Linux launches too.

Measured Retroid title intervals are approximately 59–60 FPS, median 16.67 ms;
compilation and occasional scheduling stalls still occur. Both audio ports feed
without starvation in the foreground measurements. Returning from the graphics
driver activity resumes a clear frame. Surface-lost/skipped-frame logs while the
activity is in the background do not establish the cause of the original
geometry corruption. The introductory level also renders with normal geometry and legible HUD text;
a longer gameplay/performance run and exhaustive music/effect verification
remain separate checks. The user confirms the Retroid display is clear.

## Checks

The native C/C++ runtime/renderer suite passes, including pad menu shortcut tests.
Python: 90 checks, 18 skipped for unavailable optional inputs. Driver ZIP fixtures
and the actual bundled Turnip library hash pass. Android packaging checks its
signature. Game files, driver ZIPs and signing keys are excluded from Git.

Private retained evidence lives under `out/`: package/artifact inventories,
audio-audit, kernel-contract, linux-boot-4, linux-play-check and linux-title-serial.
Historical docs in legacy describe other games and do not establish Akka status.

Installed APK SHA-256: `221cdea99397b9d34d5bdf4e9e987a5dacea104b517d417286761d2335323447`.
The native menu callback is defined in libmain and the Java activity exposes its
matching JNI entry. The ARM64 SRT check links successfully.

Android flicker comparison evidence: `out/android-flicker/cold-video-09.png`
and `cold-video-22.png` show corrupt geometry/text; `prep0-title-01.png`,
`prep0-title-06.png` and `prep0-title-12.png` show the same title with stable
layout. The full clips and timing logs remain local beside these captures.

Linux parity check after this change: a fresh native build launches with
`TEMPEST_PREP_WORKERS` and `TEMPEST_DRAW_PIPE` unset and reports zero preparation
workers. Its test log is `out/linux-flicker-parity/boot.log`. Initial frames run
at 60 FPS, followed by one-second presentation stalls in this desktop session;
this run does not establish sustained performance for the updated Linux build.

A subsequent unchanged Linux launch (`out/linux-flicker-focused/boot.log`) holds
60 FPS through repeated five-second windows with no override variables. This
confirms the common zero-worker default runs at normal title speed; the prior
one-second stalls were not reproduced in this run. No additional renderer change
was made between these checks. Android v0.2's introductory-level screenshot also
has legible HUD text and normal geometry (`out/android-flicker/v02-intro.png`).
