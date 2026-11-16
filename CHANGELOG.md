# Raiden V: Director's Cut port changes

## 0.1 — 2026-11-16

- Start the separate CUSA07673 v1.02 Linux and Android port from the shared
  Raiden Fighters runtime, retaining L3+R3 Settings, bundled Turnip, system
  driver selection, driver ZIP import and app-scoped SD-card game storage.
- Persist save-memory updates, decode PNGs with the existing FFmpeg build,
  support caller-owned NGS2 pools and RIFF/ATRAC9 parsing, and preserve six-channel
  music for AudioOut downmixing.
- Render the configured audio grain rather than the advertised output capacity,
  preventing adjacent game-state corruption. Guard callback dispatch when a
  caller-owned pool is destroyed or reused.
- Support the bundled toolkit's local-user callback replay and offline network
  callback setup. Leave HTTP and unused online services unsupported.
- Reach Stage 1 on Linux with clear text and 60 FPS in steady sections;
  gameplay confirmed by the user. Further play and Android device checks remain.

Earlier port notes are retained under `docs/legacy/`.
