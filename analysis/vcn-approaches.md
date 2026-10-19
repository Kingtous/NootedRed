# VCN prior art: removed legacy NootedRed code vs steve12311/NootedRed — synthesis and direction

Status: research synthesis for the VDA/VCN workstream (`feature/vda-hardware-decode`, Phase 1 done).
Read-only: no kext was installed or loaded, no GPU/VideoToolbox/render test was run, no system file was
modified. This document is a synthesis of two deeper reports and adds a small number of Lead spot checks:

- [`analysis/vcn-legacy-nootedred.md`](vcn-legacy-nootedred.md) — reconstruction of the removed
  NootedRed VCN/UVD work from git history (including pre-import PR refs).
- [`analysis/vcn-steve12311.md`](vcn-steve12311.md) — analysis of the community fork
  <https://github.com/steve12311/NootedRed> (master `1983f32`, 2026-10-07).
- Spot checks by the Lead: PR refs exist (`refs/pull/35/head = ec7dd43`, `refs/pull/130/head = dd3bf7b`);
  current X5000 personality advertises `IOGVACodec = AMDVCN2`; `com.apple.kext.AMDRadeonX6000` is present
  in `SystemKernelExtensions.kc`; independent rerun of the fork ABI comparison script (see §3.1).

Evidence labels: **[F]** fact visible in code/history/commit; **[C]** someone's claim, not reproduced;
**[O]** our own read-only observation; **[I]** inference; **UNKNOWN** no reliable evidence.

---

## 0. TL;DR

1. **Both attempts are the same architecture family**: keep `AMDRadeonX5000` as the active accelerator,
   borrow Apple's native `AMDRadeonX6000` VCN2 engine (`AMDVCN2HWEngine`), advertise `HasVCN0`, and patch
   Apple's user-space VideoToolbox/AppleGVA/`AMDRadeonVADriver2` code so it selects the AMD decoder.
   Neither attempt is a from-scratch VCN driver; neither allocates VCN rings itself. [F both reports]
2. **The legacy attempt (WhateverRed/NootedRed, 2022–2024) partially worked and was removed as
   "mostly broken".** A user report on Renoir + macOS 13.5.1 says HEVC hardware decode worked while
   H.264 did not; it was always hidden behind a developer boot arg (`-nredvcn`, later
   `-ChefKissInternal`), caused at least one unrelated boot regression (issue #120), and was deleted in
   `4523553` without a single documented triggering bug. [C in issues; F in history]
3. **The fork is a 2026 Tahoe reimplementation of the same idea with much better engineering**: optional
   X6000 load, six-slot vtable/getter ABI guards on both drivers, generated call-site patches, a
   `_shared_region_check_np` COW transaction for user-space, and explicit diagnostics. It is credible
   prior art, but it is **single-author self-reported** and its hardware acceptance claims are not
   independently reproduced. [F architecture; C/C hardware results]
4. **On our target (macOS 15.8.1 / Darwin 24.6, Renoir `0x1638`) the fork is inert by design and not
   reusable as data.** The OS gate rejects Darwin 24; the kernel ABI guard fails on the X5000 `0x2A8` /
   X6000 `0x2B0` getter (13 bytes on 15.8.1 vs 14 expected); 4 of 20 generated call sites are shifted;
   2 of 18 user-space reference functions (the AppleGVA decoder-selection function and
   `VAAddrLibInterface::init`) do not match 15.8.1; the generators read on-disk kext binaries that do not
   exist here (they live in `SystemKernelExtensions.kc`) and hard-assert the author's cache UUID/base.
   [O, forked resolver logic reproduced read-only; see §3.1]
5. **Direction: adopt the concept, not the code.** Keep Phase 1's inert `MediaPolicy`/`-NRedVDA` layer.
   For Phase 2/3, prototype the *native X6000 engine bridge* on 15.8.1 with freshly generated ABI data,
   optional X6000 load and engine allocation, but keep `hasVCN0` false and video decode unexposed until
   the engine path and GFXOFF behaviour are validated. Treat 15.8.1 user-space patch generation as a
   separate research track (the fork's own docs and the maintainer both identify it as the blocker).
   [I/recommendation]
6. **Licence/process:** the fork is TSNPL 1.5 and its README forbids AI use/analysis; our `LICENSE.md`
   also forbids derivatives and AI use. **No fork code or generated data may be copied into this tree**
   without a human licence decision by the project owner; this synthesis is descriptive only. [F]

---

## 1. Comparison at a glance

| Aspect | Legacy NootedRed (removed) | steve12311 fork | Our Phase 1 (current tree) |
| --- | --- | --- | --- |
| Period / status | 2022-08 … 2024-03, removed by `4523553` | 2026-10, active experiment, 5 VCN commits | 2026-10, implemented, inert |
| Target OS | Catalina 10.15 … Sonoma 14.x (legacy patterns last updated Oct 2023, no 14.4 update) | macOS 26 / Darwin 25.x only (`majorMatches(MACOS_26)`) | all supported (10.4–26.x), media disabled |
| APUs | all supported Vega APUs; `HasVCN0=true` for every one | VCN 2.2 only: `0x15E7/0x1636/0x1638/0x164C`; VCN1 explicitly rejected | all profiles record data; no engine enabled |
| Kernel engine path | X5000 active; X6000 force-loaded via injected `AMDNavi10GraphicsAccelerator` personality and `start→false`; VCN2 engine allocated in X5000 `allocateHWEngines` at `fieldBase+0x40` (`0x3F8` Big Sur+) | X5000 active; `OSKextLoadKextWithIdentifier("com.apple.kext.AMDRadeonX6000")` inside X5000 accelerator start; VCN2 engine allocated into X5000 PM4 engine array at `pm4EngineField + kAMDHWEngineTypeVCN0*8` | no engine; `allocateHWEngines` only PM4/SDMA (`// No VCN? :-(`) |
| Cross-driver glue | `safeMetaCast` hook + 5×2 `gMetaClass` swap; forwarding wrappers for `newVideoContext`, `createSMLInterface`, `newShared`, `newSharedUserClient`, `AccelSharedUserClient start/stop`, `SurfaceCopy` | `safeMetaCast` pair bridge for 4 classes + per-method hooks + 20 generated 6-byte call-site patches; GFXOFF wait pairing; ABI guards on six vtable slots per driver | none |
| Firmware | `ativvaxy_rv.dat`/`ativvaxy_nv.dat`, `ipVersion 0x0100/0x0202`, Navi10 device-type spoof, `fwDir` slot 6 | same mechanism (pre-existing upstream code, unchanged by the VCN commits); no new firmware code | same mechanism, already present |
| Capability exposure | `HasVCN0=true` unconditionally; personality advertises `IOGVACodec=AMDVCN2`, H.264/HEVC caps | `hasVCN0 = VCN::ready()` (gated by OS/device/ABI/engine); same personality advertisement inherited from upstream | `hasVCN0` from `MediaPolicy`, always false behind `VDA::mediaEnginesEnabled()` |
| User-space path | `_cs_validate_page` hook; patches VideoToolbox DRM model, AppleGVA board-id/`hwgva-id`, CoreLSKD CPUID, `VAAcceleratorInfo::identify`, `VAFactory::create*`, `VAAddrLibInterface::init`, and 13 VCN2→VCN1 command translations | `_shared_region_check_np` hook + private COW pages + all-or-nothing transaction; 22 patches over AppleGVA / `AMDRadeonVADriver2` / `libsystem_c`: GVA AMD-only fallback, per-device VA identify (`vaFamily=5`), VAFactory/VPE, AddrLib family fixes, swizzle masks, encode feedback/pitch | `UserPatches` (`_cs_validate_page` + `_cs_validate_range`) exists for GL/Metal; not extended to VDA |
| Gate | kernel side ungated; user-space gate `-nredvcn` → `-ChefKissInternal` (dev-only) | `-NRedVCN`, default off; device+OS+ABI+load+allocation gates | `-NRedVDA`, default off; `mediaEnginesEnabled = requested && ready`, `ready` never set |
| Evidence | commit history, PR #35, issues #28/#120/#163/#249/#442; HEVC works/H.264 fails on one Renoir + 13.5.1; never production | author reports Cezanne `0x1638` H.264/HEVC shorts matching software pixels; 64×64 H.264 fails; Hackintool VCN0Dec + WindowServer watchdog; GFXOFF fix offline-only | build green; no hardware decode claim |
| Main blockers | removed as mostly broken; stale user-space patterns; fragile cross-driver patching; X6000 dependency removed upstream 2025-11 | all generated data is Tahoe-specific; no 15.8.1 data; 2 user-space functions differ on 15.8.1; hardware risks (GFXOFF/watchdog); licence | user-space half; engine architecture decision; validation |

---

## 2. What the legacy attempt teaches

Facts and evidence are in [`analysis/vcn-legacy-nootedred.md`](vcn-legacy-nootedred.md); the lessons that
matter for Phases 2–4 are:

1. **The bridge concept is old and was exercised on real hardware.** The 2022-08-22 commit
   "Replace UVD/VCE with VCN2 from X6000" already force-loaded X6000 as a symbol donor, and the 2023
   PR #35 added the user-space VA patches. This is not a new idea; it is a known path with known
   failure modes. [F]
2. **It partially worked, but not uniformly.** PR #35 claimed "proper hardware acceleration for decoding";
   issue #163 (Renoir 5800H, macOS 13.5.1, `-nredvcn`) says HEVC hardware decode worked and H.264 did
   not; issue #153 reports `VDADecoderChecker` error `-12473`. No VCN1 (Raven/Picasso) success report was
   found. [C]
3. **Advertising `HasVCN0` unconditionally while gating only the user-space patches was unsafe by
   construction.** The kernel side ran for every APU; only `DYLDPatches` was behind the developer flag.
   That is exactly the asymmetry the current Phase 1 avoids (`hasVCN0` only when the bring-up is ready,
   `-NRedVDA` opt-in). [F; I]
4. **The user-space half is the fragile part.** The legacy patch table was a chain of fixups/reverts over
   three user-space binaries, updated only to Ventura, then abandoned; one patch mistake caused an
   unrelated login hang (issue #120). This is consistent with the maintainer's 2025 comment that the
   kext side is "straightforward" while user-space patching is the blocker (issue #28). [F/C]
5. **The removal did not delete the firmware or the capability plumbing.** `4523553` deleted code only;
   `ativvaxy_rv/nv.dat` firmware injection, the X5000 personality's `IOGVACodec = AMDVCN2` advertisement,
   and the per-OS `hasVCN0` offsets survive today. The firmware path is therefore already in place for
   Phase 2. [F]

**Do not repeat:** unconditional capability advertisement, hard-coded VCN1 command translation with no
hardware matrix, and a kernel path that is not gated by the same feature switch as the user-space path.

---

## 3. What the fork adds (and what it does not)

The fork is best understood as a **modern, Tahoe-targeted reimplementation of the legacy bridge** with
substantially better safety engineering:

- **Optional native load instead of a personality.** X6000 is loaded from inside X5000 accelerator start
  (`OSKextLoadKextWithIdentifier`), not injected as an accelerator personality; its `start` is never
  allowed to create a competing accelerator. [F]
- **Generated ABI guards.** Six vtable slots per driver are verified by method identity or full getter
  bytes (`isDeviceValid`, anonymous getters at `+0x2A8/+0x3B8/+0x3C8`, `disableGfxOff`, `enableGfxOff`);
  any mismatch disables the whole bridge. This is the right pattern to copy conceptually. [F]
- **Generated call-site patches instead of blind vtable rewriting.** 20 six-byte patches rewrite X6000
  `callq *slot(%rax)` to X5000 slots, and pair GFXOFF waits; every site is verified before writing, with
  rollback. [F]
- **User-space transaction.** `_shared_region_check_np` + private COW pages, static UUID fast path vs
  dynamic same-implementation relocation, exact matching, RX protection checks, all-or-nothing writes,
  rollback, idempotency, bounded reads. This is a stronger safety envelope than the legacy
  `_cs_validate_page`-writes-after-validation approach. [F]
- **One driver-set, generated per host.** All kernel and user-space data is generated from the author's
  macOS 26 drivers/cache; there is no 15.8.1 data and the generators refuse mismatched inputs. [F]

It does **not** add: VCN1 support, its own firmware/ring/submit code, an OS-independent data format, or
independent hardware evidence. Its acceptance results are single-author prose; the repo's tests are CPU
simulations (the author says so in `docs/vcn.md`). Treat the architecture as credible and the hardware
claims as plausible-but-unverified.

### 3.1 On our macOS 15.8.1 Renoir `0x1638` (Lead spot checks)

Method: the fork's own read-only parsing logic was reproduced against this machine's
`SystemKernelExtensions.kc` and `dyld_shared_cache_x86_64h` by `vcn-fork-researcher`; the ABI check was
independently rerun by the Lead. No kext was loaded and no GPU API was called. [O]

| Check | Result |
| --- | --- |
| OS gate | returns 4 (non-Tahoe); feature inert by design on 15.8.1 |
| X6000 kext availability | `com.apple.kext.AMDRadeonX6000` is in `SystemKernelExtensions.kc`; on-disk `Contents/MacOS` is empty. Whether `OSKextLoadKextWithIdentifier` succeeds on Renoir-only 15.8.1 is **untested** |
| Kernel vtable guards | 10/12 slots match; X5000 `0x2A8` and X6000 `0x2B0` getters differ (15.8.1: `55 48 89 E5 8A 87 0E 03 00 00 5D C3`, 13 bytes, no `and $1`; reference expects 14). `verifyKernelABI()` would reject both drivers |
| Kernel call-site patches | 16/20 match; 4 are shifted: `setSuspendResumeState +0x28` (ref `+0x27`), `writeDiagnosisReport +0x52B/+0x7AD/+0x9C3` (ref `+0x524/+0x7A5/+0x9B8`) |
| User-space images | AppleGVA, `AMDRadeonVADriver2`, `libsystem_c` are all found in the 15.8.1 cache (UUID `42EC2890-…`, base `0x7FF800000000`) |
| User-space reference functions | 16/18 match; the AppleGVA decoder-selection function (1136 B reference) and `VAAddrLibInterface::init` (528 vs 530 B) do not. The AddrLib family patch bytes themselves still exist; the GVA branch needs fresh analysis |
| Generators | `build_vcn_kernel_patch.py` reads on-disk X5000/X6000 Mach-Os that do not exist here (need kernel collection/KDK extraction); `metal_cache.py` hard-asserts the author's cache UUID/base |

Conclusion: the fork's data is not portable to 15.8.1; the fork's **design** is portable. `[I]`

---

## 4. Recommendation: chosen direction for Phases 2–4

### 4.1 Architecture choice (must be made before Phase 3 code)

Two ways to get a VCN2 engine into the X5000 accelerator:

- **Option A — native X6000 engine bridge (fork/legacy concept).** Load X6000 on demand, allocate
  `AMDVCN2HWEngine`, insert it into the X5000 engine array, bridge metaclasses/vtable calls. Least new
  driver code, proven concept, but it reintroduces the X6000 dependency that upstream deliberately
  removed in 2025-11 (issue #442 direction) and is ABI-fragile.
- **Option B — reimplement the VCN engine with `RuntimeMC`.** Follow the 2025 DCN reimplementation
  direction; no X6000 dependency, but it means reimplementing Apple's VCN/PSP/ring/submit logic from
  observation — a much larger, unproven project.

**Recommendation:** prototype Option A for Phase 2/3 because it is the only known community path to
hardware decode on these APUs and the kernel side is the tractable part. Record the choice explicitly as
an experiment, keep it behind `-NRedVDA`, and do not let it leak into the default graphics path. If the
project owner rejects reviving the X6000 dependency, Phase 3 must switch to Option B before any code is
written. `[I]`

### 4.2 Phase 2 — data extraction and safe kernel validation (no decode exposure)

1. Extract 15.8.1 `AMDRadeonX5000`, `AMDRadeonX6000` and `AMDRadeonX5000HWLibs` Mach-Os from
   `SystemKernelExtensions.kc` (e.g. `kmutil emit-macho` or a KDK), plus their version/UUID/hashes. Do not
   ship or commit the extracted binaries.
2. Generate 15.8.1-specific ABI data: both hardware vtables, the six guard slots, the getter change at
   `0x2A8/0x2B0`, and the 20 call-site patches (expect the four shifted offsets). Prefer an in-tree
   generator with an input-hash manifest over hand-edited tables.
3. Prove the optional X6000 load on Renoir-only 15.8.1 and that `AMDVCN2HWEngine`/`AMDAccel*`
   metaclasses are usable; keep engine allocation disabled for this step.
4. Verify the existing firmware injection path is exercised only when the engine is used; no new
   firmware code expected. Publish read-only diagnostics (loaded/validated/allocated/readback).
5. Keep `hasVCN0` false and `VDA::isReady()` false. Exit criterion: all guards validate, load succeeds,
   no engine write, no graphics regression.

### 4.3 Phase 3 — engine insertion and ring/submit validation

1. Allocate the engine and insert it into the X5000 PM4 engine array at
   `kAMDHWEngineTypeVCN0`; validate `allocateHWEngines` success and engine idleness/GFXOFF pairing.
2. Note that **the native engine owns ring allocation/submission**; NootedRed does not allocate VCN rings
   in this design. Phase 3's original "allocate VCN rings and doorbells" should be restated as "validate
   the native engine's ring/doorbell setup and command submission" unless Option B is chosen.
3. Flip `hasVCN0` only for the tested profile (`0x1638`) **after** ring/submit validation and with
   `-NRedVDA`; any failure must leave the accelerator in the current graphics-only state, never PANIC.
4. Reproduce all diagnostics before/after; watch for Hackintool `VCN0Dec` stalls, WindowServer watchdog,
   `.gpuRestart`; GFXOFF hang recovery must be planned (bootable EFI / remove arg / reboot).

### 4.4 Phase 4 — VideoToolbox/VDA user space (the long pole)

1. Treat 15.8.1 user-space patch generation as its own task: locate the AppleGVA decoder-selection
   function in AppleGVA 113.56 and re-derive `VAAddrLibInterface::init`; revalidate the 16 matching
   reference functions and all 22 patch sites; regenerate identify payloads (`vaFamily=5`), AddrLib
   family fixes, VAFactory/VPE and swizzle patches.
2. Delivery mechanism: our existing `UserPatches` (`_cs_validate_page`/`_cs_validate_range`, already
   validated on 15.8.1 for GL/Metal shared-cache pages) is the natural host; adopt the fork's
   transaction semantics conceptually (all-or-nothing, RX check, rollback, idempotency). Whether it can
   reach `AMDRadeonVADriver2`/AppleGVA in `VTDecoderXPCService` must be proven; otherwise a
   `_shared_region_check_np` COW path is the fallback idea (new code, not copied).
3. Keep `-NRedVDA` default off; no VideoToolbox exposure until the kernel side works; add
   hardware acceptance: H.264/HEVC short clips vs software pixel digests, 64×64, Hackintool
   create/destroy, ≥5 min idle then start, continuous playback, desktop responsiveness, 10-bit HEVC,
   DRM/Apple TV, `.gpuRestart`/watchdog checks.
4. Consider the personality mismatch: the current X5000 personality already advertises
   `IOGVACodec = AMDVCN2` and HEVC capabilities even though no engine exists. Before exposing the
   engine, decide whether to keep or suppress this advertisement while testing; do not change stable
   graphics behaviour without validation.

### 4.5 Required data collection (per target machine)

- OS/build/Darwin, iGPU PCI ID/revision, ioreg media properties, boot args.
- X5000/X6000/HWLibs binaries from kernel collection or KDK, with versions/hashes; dyld shared cache
  UUID/base and the AppleGVA/`AMDRadeonVADriver2`/`libsystem_c` images and symbols.
- Software-vs-hardware decode pixel digests, decoder `UsingHardwareAcceleratedVideoDecoder`, `.gpuRestart`
  and WindowServer reports if anything hangs.
- Version→data table keyed by OS build and driver version; never mix Tahoe-generated data into 15.8.1.

### 4.6 Licence/process

The fork is TSNPL 1.5 and its README forbids AI use/analysis; our `LICENSE.md` forbids derivatives and AI
use. No fork code or generated table may be copied into this tree without the project owner's explicit
decision. The direction above is conceptual; any implementation must be independently written (and the
project's AI-use policy applies to this analysis itself — record and resolve with the owner).

---

## 5. Open questions

1. Did the legacy path ever work on VCN1 (Raven/Picasso/Raven2)? No report found; the 13 VCN2→VCN1
   command patches are unvalidated. [UNKNOWN]
2. Why exactly was the legacy code removed in 2024-03, and did it ever work on Sonoma 14.x? No triggering
   issue/commit found; `DYLDPatches` was not updated for 14.4. [UNKNOWN]
3. Can `OSKextLoadKextWithIdentifier("com.apple.kext.AMDRadeonX6000")` load the kext and expose
   `AMDVCN2HWEngine` on Renoir-only 15.8.1? [UNKNOWN, testable in Phase 2]
4. Is the 13-byte getter at X5000 `0x2A8` / X6000 `0x2B0` semantically equivalent to the reference's
   14-byte `and $1` version? What should the 15.8.1 guard assert? [UNKNOWN]
5. What is the 15.8.1 AppleGVA 113.56 decoder-selection control flow, and where is the AMD-only fallback
   branch? [UNKNOWN, Phase 4]
6. Option A vs Option B: does the project accept reviving the X6000 dependency, or must the VCN engine be
   reimplemented with `RuntimeMC`? [decision]
7. Does the existing `IOGVACodec = AMDVCN2` personality advertisement affect VideoToolbox selection in
   the current graphics-only build, and should Phase 2 suppress it while experimenting? [UNKNOWN]
8. Hardware risks on 15.8.1 (GFXOFF hang, Hackintool `VCN0Dec` watchdog, 64×64 H.264) are inherited
   from the fork's Tahoe observations; nothing on 15.8.1 has been measured. [UNKNOWN]

---

## 6. References

- Deep reports: [`analysis/vcn-legacy-nootedred.md`](vcn-legacy-nootedred.md),
  [`analysis/vcn-steve12311.md`](vcn-steve12311.md).
- Legacy removal: `4523553` "Remove the mostly broken VCN code" (2024-03-13);
  pre-import PR refs `refs/pull/35/head` (`ec7dd43`), `refs/pull/130/head` (`dd3bf7b`).
- Fork: `https://github.com/steve12311/NootedRed`, master `1983f326ebedda9e5596ee6e8ba47af52420ec11`;
  VCN commits `02d3789`, `ae8f6bc`, `6c7b080`, `338fff9`, `1983f32`; `docs/vcn.md`,
  `docs/user-cache-dynamic.md`, `NootedRed/VCN.cpp`, `NootedRed/VCNKernelData.hpp`,
  `NootedRed/UserVideoDecode.cpp`, `NootedRed/UserVideoDecodeData.hpp`, `tools/build_*_patch.py`.
- Current tree: `NootedRed/DeviceProfile.hpp/.cpp`, `NootedRed/VDA.hpp/.cpp`,
  `NootedRed/X5000.cpp`, `NootedRed/HWLibs.cpp`, `NootedRed/Personalities/com.apple.kext.AMDRadeonX5000.xml`,
  `NootedRed/UserPatches.cpp/.hpp`, `NootedRed/GPUDriversAMD/PSP.hpp`.
- Issues referenced by the legacy report: #28 (open feature tracker), #35 (PR), #120, #163, #249, #442.
- Linux AMDGPU hardware list (VCN generation mapping):
  <https://www.kernel.org/doc/html/v6.17/gpu/amdgpu/amd-hardware-list-info.html>.
