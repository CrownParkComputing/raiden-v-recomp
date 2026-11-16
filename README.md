# Raiden V: Director's Cut native port

Linux x86-64 and Android ARM64 port in development for PS4 CUSA07673 v1.02,
derived from the shared Tempest, PAC-MAN CE2, Akka Arrh, Polybius and Raiden
Fighters runtime.

The matching base and update extract and link successfully. Linux reaches
Stage 1 with the original graphics and clear text, runs steady sections at
60 FPS with sustained audio, and has been confirmed playable by the user. Full playthrough and
Android device verification remain pending. See the
[current status](docs/raiden-v-port-status.md) for the tested scope.

Android uses its own application ID, `com.crownparkcomputing.raidenv`.
The shared L3+R3 Settings panel retains bundled Turnip as the default, the system
driver option and compatible ARM64 driver ZIP import, applied on relaunch.

See [Linux build instructions](docs/building.md), [Android](android/README.md)
and [source origins](THIRD_PARTY_NOTICES.md). Build with `bash build.sh`, prepare
your supported dump with `python3 run.py prepare --game game`, then run
`./play-raiden-v.sh`. Shared configuration names remain `TEMPEST_*`.

Original packages, game files, prepared images, drivers, signing keys and saves
remain local and excluded from Git. The APK contains the runtime, driver and
launcher art; game executables and resources are staged separately.
