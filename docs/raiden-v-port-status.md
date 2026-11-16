# Raiden V: Director's Cut port status

Supported input identity: CUSA07673 base APP_VER 01.00 plus matching update
01.02, content ID `UP2035-CUSA07673_00-RAIDEN5ENCA70707`. Updated executable
SHA-256: `c8fcfe9abd75c1e10c5a6506a1bf1e1548a20f75d115ad5e4aa3b39a1638c005`.

## Input and preparation

The original archive passed extraction/CRC checks. Base extraction produced
2,654 files; overlaying the 12 update files gives 2,657 files and 5,504,872,091
bytes. Update hashes match the applied tree. Original packages and manifests
remain private.

The main executable has 413 imports and 54,763 relocations. libc and Fios2 link
at boot; the bundled NpToolkit2 module remains deferred. Preparing the SELF
reconstructs executable loadable bytes; unavailable non-loadable metadata is
reported explicitly, so the derived ELF is not a byte-exact original ELF.

The measured process SDK is `0x04508101` from its 80-byte procparam. SFO
ATTRIBUTE `0x800020` gives CPU mode 0. Earlier games' fixed-address frame/aspect
patches remain disabled. Draw recording/preparation retains serial defaults.

## Execution and runtime work

Linux reaches the original menu, fighter selection and Stage 1 with clear text
and the original side panels. Steady sections run at 60 FPS, with pauses during
initial resource/shader loading. The user confirmed gameplay working. This is
an initial stage check, not a complete playthrough or a save-reload check.

The save-memory API persists updates atomically in the separate user profile;
focused checks cover partial offsets, reopening, metadata/icons, failed writes
and range/user errors. The five PNG decoder APIs use the existing FFmpeg build,
with real pixels, stride, transparency, Adam7 and failure guards checked.

The game uses caller-owned NGS2 system/rack pools and callback-based RIFF/ATRAC9
parsing. Its music includes six-channel ATRAC9; the adapter preserves those
channels for the existing AudioOut downmix. Independent real encoded music
matches a decoder reference. A live Linux run decoded 32,522 ATRAC9
superframes and produced 17,193 audible output buffers, with no missing audio
data reported. The game also completed two save-memory writes.

A live hardware watchpoint proved that clearing the whole advertised NGS2
output capacity erased adjacent audio handles and a mesh scratch pointer. The
corrected render uses the configured grain: the NULL system option means 256
samples, or 8 KiB per eight-channel float output. Larger capacity tails remain
untouched. A regression checks both tails and all-output validation before any
writes or voice advancement. Callback dispatch also rejects destroyed/reused
caller-owned pools before dereferencing their storage.

The bundled network toolkit reaches local-user and network callback setup.
Those paths use the existing offline state and replay the pending local login
outside the registry lock. HTTP and unused online services remain unsupported;
an actual unknown call traps with its caller. Import presence alone does not
make an API a boot blocker.

## Checks and remaining work

The inherited native/runtime and renderer checks pass in this checkout.
Focused save-memory, PNG, NGS2 and local-user callback checks pass, including
native calls and FEX callback-dispatch fixtures. Python checks pass 92 tests
with 17 optional skips, and driver ZIP fixtures pass.
Android has a separate app identity, the shared L3+R3 settings panel and the
bundled/system/imported driver choices. Android main, boot-check and FEX-check targets build. The signed version
0.3/code 3 APK passes identity, ARM64/API, original-icon and bundled-driver
provenance checks. It is installed on the Retroid and its installed hash matches
the build. The interrupted SD transfer initially left required boot assets absent.
After resuming, all 2,657 files and their sizes matched the host, along with
five critical file hashes. The Retroid Pocket Flip2 now reaches Stage 1 with
music and save-memory writes. Stage 1 currently measures about 31–32 FPS,
compared with about 50–55 FPS in menus; Android performance work remains open.
The reported cloud streaks are under investigation. This is an initial stage
check, not a complete playthrough.

The shared FEX wait wrapper now returns a permitted spurious wake after an
artificial 10 ms condition-wait slice, allowing the guest to recheck its
predicate. Real deadlines still report timeout. A bounded single-notification
regression fails on the previous FEX wrapper and passes after this correction.
Native/FEX deadline and recursive-lock checks and sanitizer runs pass.

GPU resource reuse now observes completion refreshed during its search rather
than a captured older tick. Optional deferred counter polling remains disabled
by default. Focused tests cover concurrent completion, timeout, ordered
retirement and real pool selection; native and Android builds pass. Frame
capture also records raw alpha-mask state for the cloud investigation.

The source was published at commit
`5d8cbe63da8c709af3a634801e40d9846d32347b` and the fresh Ubuntu
[CI build and runtime/Python/driver checks passed](https://github.com/CrownParkComputing/raiden-v-recomp/actions/runs/43778487064).
The subsequent wait and GPU-polling changes have focused local verification;
the earlier CI result describes its linked commit.

Private evidence is retained under `out/` and in the CUSA07673 input review
directory. No game assets, packages or drivers belong in the public source.
