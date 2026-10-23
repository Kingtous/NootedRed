# Shared-region hook stack research (ThinkBook 14p / 5800H)

## Problem and scope

The tested v0.9.0-pre-release.6 build reaches the Tahoe 26.7.1 (25G241) desktop
with either `-NRedDisplayFix` or `-NRedVideoFix`. Enabling both causes an early
boot failure. An earlier dual-feature report contains a launchd Double fault
while mapping the shared cache. The cause is not yet confirmed.

Inspection of that release binary and its dSYM found local stack reservations
of 6,040 bytes in the display wrapper and 1,640 bytes in the video wrapper.
Both route `_shared_region_check_np` and call their original function while
their own stack frame is live, so those reservations can overlap the native
shared-cache and storage call chain. These figures describe the old release,
not measured stack usage of this candidate.

This checkout starts at `27e75c8`, newer than pre-release.6 (`128b980`). Existing
FairPlay and page-helper changes are preserved. Testing this candidate therefore
also includes those intervening changes; it is not an otherwise identical
pre-release.6 build.

## Candidate change

- Keep each entry wrapper small. Run the original hook chain first, then enter
  a separate `noinline` patch helper. The helper cannot be inlined back into
  the wrapper in optimized builds, and its frame is not live during the native
  syscall or the next hook in the chain.
- Move display patch plans, rollback bytes, legacy patch descriptors, context
  bytes and page records into one per-call heap workspace. Free it on all
  return paths; reject the patch attempt on allocation failure while preserving
  the native result. No shared/global scratch buffer is introduced.
- Retain both feature switches, code/cache validation and protection/rollback
  behavior. No patch-content or ABI validation is bypassed.
- Emit four boundary messages for each of the first eight calls per module:
  `original-enter`, `original-return`, `patch-enter`, `patch-return`. Native
  errors return without patching. Each message includes module, call ticket
  and PID. The trace counter saturates; it does not continuously print.

`NRedSharedRegionStackRevision=1` and
`NRedVCNSharedRegionStackRevision=1` identify this implementation in IORegistry.
They identify the code revision, not feature activation or successful decoding.
Existing `Enabled`, `Applied` and error properties retain their meanings.

## Build with the existing workflow

The existing CI now permits deliberate fork builds, matches branch names with
slashes, and uses `macos-15-intel` for the x86_64 harnesses. It builds Debug,
Research Release and Release, including Debug dSYM output. It uploads archives,
dSYMs, commit/submodule/Xcode information and wrapper symbol records as
`NootedRed-research-<commit>`. The inherited tag-release step remains tag-only;
an ordinary branch push produces workflow artifacts, not a release.

For a manually dispatched run, select this branch under Actions → CI → Run
workflow after pushing it. Alternatively, a push to this branch starts CI.
No custom secrets are needed for ordinary artifact builds.

The inherited archive build phase includes a dSYM when present. Use the Release
archive for the first device comparison; keep the matching Debug archive and
dSYM for further instrumentation. Do not mix a binary with another commit's
symbols. These remain research artifacts, pending real-device acceptance.

## Validation

- `tools/test_shared_region_hooks.py`: compiles the production wrappers,
  exercises both chain orders and native-error propagation, checks trace
  saturation, and applies a 512-byte compiler frame warning budget to the
  host harness. This is not a measurement of the final kext's frame sizes.
- `tools/test_surface_sync_bridge.py`: existing cache/code/protection and
  rollback coverage, plus forced workspace allocation failure, no live
  workspace during the original call, and leak checks after every call.
- `tools/test_vcn_user_bridge.py`: VCN/FairPlay patch selection and rollback.
- `tools/test_legacy_bridges.py`: Sonoma/Sequoia and SubmitGuard regression
  checks; CI additionally runs its address/undefined sanitizer mode.
- `tools/test_fix_package.py`: feature-switch checks, also applied to the
  built kexts in CI.

The Windows checks use the installed Clang 16 with C++23 draft spelling
`-std=c++2b` and omit the macOS `xcrun` launcher. They run the C++ simulation
harnesses, not the macOS kext. Xcode builds, Clang analysis, sanitizer checks
and final binary checks must be verified in the workflow.

## Device acceptance

Keep the known-good EFI and original kext. Record the exact workflow commit and
binary UUID, replace only NootedRed for comparison, and retain the existing
logging configuration. Compare no switches, display-only, video-only, then
both. Confirm `Applied` properties and actual H.264/HEVC decoding separately
from reaching the desktop. Collect panic/GPU reset logs and a boot video if
the early fault cannot be persisted. A successful boot would support this
candidate; it would not by itself establish a complete video/stability fix.
