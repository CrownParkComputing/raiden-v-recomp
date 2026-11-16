# Polybius port status

Supported input: CUSA08328 base v1.00 plus matching update v1.03, content ID
`UP4461-CUSA08328_00-LLAMASOFTPLB0002`. The latest executable SHA-256 is
`e02a60780769e1c743ce69870b0580b48a43c6f5a5ee870900ffe517bdb039fb`.
Original packages, before/after overlay hashes and all game data stay local.

## Verified so far

Linux links and starts the real libc and Fios2 modules and reaches playable
level 1 through the original title and level-selection screens. The ship,
track, background and HUD render with legible text. Steady sections run at
60 FPS; first-use shader compilation causes brief pauses. Firing input and
the game-over return to the title were exercised without an unsupported-import
stop in the tested path.

The original opening flow confirms the left and right signs in sequence, with
detail pages between them. It ignores input for approximately 200 screen ticks
after each transition. This is game behavior, not a port patch.

GNF textures render. The original SCREAM initialization and both 8-channel
audio ports start. A 398.56-second stereo PCM capture includes non-silent output
on port 0 (peak 0.980, RMS 0.078); port 1 is silent in this capture. The tested
foreground audio-stat windows report no starvation. This verifies the output
path, not individual music ordering or every effect. SAVEDATA00/gilesdata.bin
is created under the CUSA08328 save namespace.

Native runtime/renderer checks pass. Python checks pass 90 tests with 17 optional
skips in the initialized Git checkout. Driver ZIP fixtures and the actual bundled
Turnip library hash pass. The ARM64 FEX/boot/renderer builds and signed APK
packaging pass, including the native graphics-menu callback.

## Android

Separate application ID `com.crownparkcomputing.polybius`, label Polybius,
version 0.1. The APK and 122 game files are installed separately on the Retroid.
The existing Akka session was preserved. Device launch/testing is pending the
user's availability; Android gameplay/performance is not established yet.
The installed APK SHA-256 matches the packaged build:
`47d5c6e09650a14171c98d51aeadf82a80be391374248bfe226079016777a67b`.

Bundled Standard Turnip 27 November 11 r2 is the default, with archive/library
hashes recorded inside the APK. L3+R3 opens Settings. Graphics driver offers
Bundled, System and Import driver ZIP, applied on the next launch. A launcher
shortcut opens driver recovery independently of game rendering.

Both draw pipelining and speculative draw preparation default off, carrying the
profile verified in Akka. No game-specific address or soundtrack-index patches
are applied. Linux directly executes x86-64; Android uses the pinned FEXCore.

## Limits and evidence

Camera/VR support and optional branches remain unverified. The tested path is
ordinary screen output, not a VR implementation. Longer gameplay, save reload,
music ordering and individual effects remain unverified. The diagnostic run
was ended through the window manager (SIGTERM). A separate focused check opened
Settings with F10 and used the in-game exit confirmation; it produced the
runtime report and exited successfully with status 0.

Private evidence: out/PACKAGE_FINDINGS.md, package-identity.json,
overlay-evidence.json, artifact/, opening-input-contract/, linux-first-boot/,
linux-play-check/, linux-audio-check/, android-install-verification.json and
linux-menu-check/ plus the native/Android build/test logs.
