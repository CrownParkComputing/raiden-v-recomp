# Development

Raiden V: Director's Cut uses the shared PS4 runtime and shadPS4 Vulkan renderer. Linux directly
executes x86-64 code; Android uses FEXCore. The supported input is CUSA07673 v1.02,
assembled from a matching base and update. Package metadata and original bytes
are retained privately under out/ and game/.

See [building.md](building.md), [Android](../android/README.md) and the current
[port status](raiden-v-port-status.md). Runtime findings for earlier games are
historical evidence, not Raiden V: Director's Cut verification. Keep frame/aspect address hooks
inert until this executable has independently validated signatures.

Use isolated profiles with `--user out/check/user` and
`TEMPEST_GPU_USER_DIR=out/check/gpu-user`. `TEMPEST_FRAME_STATS=1` and
`TEMPEST_AUDIO_STATS=1` enable targeted diagnostics. An unsupported import traps
with the guest caller so its real contract can be implemented.
