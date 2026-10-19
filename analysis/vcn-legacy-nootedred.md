# Legacy (removed) VCN/UVD work in NootedRed — reconstruction and assessment

Status: static research only. No kext was installed, no GPU/VideoToolbox/reboot test was run, no system
file was modified. Repo: `ChefKissInc/NootedRed` (GitHub; the older `NootInc/NootedRed` name redirects
to it). The analysis is written against this clone, branch `feature/vda-hardware-decode`, tip
`1e43a4e` (Phase 1 `VDA` skeleton); the removed code is read back at its last pre-removal commit,
`4523553^` = `adfc907` ("Updates for 14.4", 2024-03-13).

Primary question: what exactly was the abandoned VCN hardware-decode attempt, what worked, what did not,
why was it deleted, what is left, and is it a usable base for VDA Phases 2–4 on macOS 15.8.1 (Darwin
24.6) Renoir `0x1638`?

---

## 0. Verdict (TL;DR)

1. **The legacy VCN attempt is much older than the visible history.** The in-repo root commit `1444841`
   ("Add Catalina support (#130)", 2023-08-07) is a squashed import; the real VCN work started on
   **2022-08-22** in the predecessor project *WhateverRed* (`b2c114a`, "Replace UVD/VCE with VCN2 from
   X6000"), and the pre-import history is still recoverable from the GitHub PR refs (`refs/pull/130/head`
   and `refs/pull/35/head`). See §1.
2. **It was a "borrow X6000 and patch VADriver2" design.** `AMDRadeonX5000` stayed the active
   accelerator; `AMDRadeonX6000` was force-loaded only as a symbol donor for `AMDVCN2HWEngine`,
   `newVideoContext`, `createSMLInterface`, `AMDAccelShared`/`AMDAccelSharedUserClient` and
   `SurfaceCopy`. Engine objects were allocated inside X5000's `allocateHWEngines` and cross-driver
   `OSMetaClass` identity was papered over by a `safeMetaCast` hook plus a `gMetaClass` swap table.
   See §3.
3. **User-space was patched through Lilu's `_cs_validate_page` hook**, not through kext patches: pages
   of the dyld shared cache were modified after code-signing validation to relax VideoToolbox/AppleGVA
   checks and to force `AMDRadeonVADriver2` onto GFX9/VCN2 code paths, plus a VCN2→VCN1 command
   translation layer for Raven/Picasso/Raven2. Gated first by `-nredvcn` (2023-05-28), later by
   `-ChefKissInternal` (2023-10-15); Catalina was explicitly excluded from 2023-10-15 on. See §4.
4. **It partially worked.** PR #35 (merged 2023-04-14) claims "proper hardware acceleration for decoding
   now"; a user report (#163, 2023-09-07, Renoir 5800H, macOS 13.5.1, `-nredvcn`) says HEVC hardware
   decode worked while H.264 did not. It was never production-quality and was always behind a
   developer-only boot arg. See §6.
5. **The removal commit `4523553` ("Remove the mostly broken VCN code", 2024-03-13) deleted only
   code.** 8 files, 549 deletions: `DYLDPatches.cpp` (132 lines), `DYLDPatches.hpp` (307), VCN routes and
   forwarders in `X5000.cpp/.hpp` and `X6000.cpp/.hpp`, the `DYLDPatches` wiring in `NRed.cpp`, the
   Xcode project entries, and `HasVCN0 = true → false`. **It did not delete any firmware blob** (the
   starting note that it did is incorrect; the VCN blobs had churned earlier and independently — see
   §5), and it did not touch `HWLibs.cpp`, the personalities, or `PSP.hpp`.
6. **Large VCN remnants survive**: VCN firmware embedding/injection (`ativvaxy_rv.dat`/`ativvaxy_nv.dat`
   via `HWLibs`) is still live; the injected X5000 accelerator personality still advertises
   `IOGVACodec = AMDVCN2`, H.264/HEVC decode/encode capabilities and `SWIP_properties`; the per-OS
   `HasVCN0` capability offsets are still present (now written from `DeviceProfile::MediaPolicy` behind
   the inert `-NRedVDA` gate). The Phase-1 `DeviceProfile`/`VDA` layer is new work (2026), not a legacy
   remnant. See §7.
7. **Revivability on 15.8.1/0x1638: kernel side is plausible, user-space is the hard part.** The kext
   already has the firmware, the capability offsets, native PM4/SDMA metaclass handling and a working
   `_cs_validate_page`/`_cs_validate_range` patching framework (`UserPatches`). What is missing is a VCN2
   engine class (X6000 no longer loaded; the project deliberately removed that dependency in 2025-11),
   ring/doorbell/wait logic in the kernel, and a complete set of **macOS 15.8.1-specific user-space page
   patches** (the legacy patterns are macOS 12/13-era byte strings). The maintainer's own 2025 statement
   is that the kext side is "straightforward" but user-space patching is the blocker. See §8.
8. **Recommendation:** use the legacy code as a map and as proof of the required boundaries (firmware
   names, engine slots, metaclass bridges, power/GFXOFF interactions), **not** as a base to resurrect
   verbatim. Rebuilding the user-space half against the 15.8.1 binaries is a research project of its own.

---

## 1. How the history was reconstructed (method and limits)

The clone's history is truncated: `git rev-list --max-parents=0 HEAD` is `1444841` ("Add Catalina support
(#130)", 2023-08-07), and both `origin/master` and the old `NootInc/NootedRed` remote root at that same
commit. Everything before that was lost from `master`.

However, GitHub still serves the old PR refs, and their ancestry contains the pre-import commits:

- `refs/pull/35/head` = `ec7dd43` ("Fix VCN (at least partially)", merged as `2eb093a` on 2023-04-14);
- `refs/pull/130/head` = `dd3bf7b`; **its tree hash equals the import root's tree**
  (`a7e643201d12781a7beb344352e02309ec691a7d` for both `pr130^{tree}` and `1444841^{tree}`), i.e. the
  import commit is an exact squash of PR #130's head.

Fetching those refs into `/tmp` (never into the workspace) restores roughly 1,180 pre-import commits
back to 2021-10 and makes the 2022–2023 VCN commits readable. `git show <commit> --stat`, `git log -S`,
`git log --grep`, and the GitHub issue tracker (PR #35, issues #28/#120/#137/#153/#163/#249/#442) were
the main sources. The 2022–2023 code lived under `WhateverRed/` until the 2023-03-13 rebrand
(`f157021`), then under `NootedRed/`.

Limits to keep in mind:

- No runtime experiments were performed, so every statement about *runtime behaviour* is either a code
  reading, a commit message, or a third-party report — never our own measurement.
- On the test machine (macOS 15.8.1, 24H32) the Apple kext binaries and `AMDRadeonVADriver2` Mach-O are
  not on disk (only `Info.plist`/`_CodeSignature` remain; the code lives in the kernelcache and dyld
  shared cache, and this was verified read-only). Therefore no legacy byte pattern can be checked
  against 15.8.1 binaries without extracting those caches — outside this static task.
- The pre-import PR branches are the *merged* state of PR #35/#130, not necessarily every development
  branch; there may be dead VCN experiments on branches whose refs are gone.

---

## 2. Timeline

Date is the commit date. "pre-import" commits are reachable today only through PR refs; "import" is the
visible clone history.

### 2.1 WhateverRed era (pre-import, recovered)

| Date | Commit | Event |
| --- | --- | --- |
| 2021-10-11 | `2f0e031`, `0b32fee` | repository start |
| 2022-07-22 | `335b5a5` | first VCN firmware injection: `ativvaxy_rv.dat` |
| 2022-08-08 | `6e2dd45` | personality `IOGVACodec` changed `AMDVCN1 → AMDU7V4` |
| 2022-08-22 | `b2c114a` | **kernel VCN origin**: X6000 force-loaded via new `AMDNavi10GraphicsAccelerator` personality; `AMDVCN2HWEngine` constructed into X5000's hardware object at `+0x3f8`; X6000 accelerator `start` stubbed to `false` |
| 2022-08-22 | `551dcff` | first X5000/X6000 vtable-offset `find`/`replace` patch set ("Patch VCN2 VTable calls") |
| 2022-11-19…2023-02-27 | `33c1f1c`, `8cc758f`, `3b10b89`, `31ad291` | per-chip `*_vcn.bin` firmware added/updated (picasso, raven2, renoir, green_sardine) |
| 2023-02-07 | `26bb993` | MMHUB/VCN power-gating experiments (`PPSMC_MSG_PowerUpVcn` 0x0C, `PPSMC_MSG_PowerGateMmHub` 0x35) |
| 2023-02-08 | `9ade06d`, `c066001` | X5000 `newVideoContext` and `createSMLInterface` replaced by X6000's implementations |
| 2023-02-12…02-18 | `55fcdd1`, `8bdee64`, `9b85acc` | remove/revert/re-remove manual VCN/MMHUB power-up; "Powering up MMHUB and VCN manually seems to be unnecessary" |
| 2023-02-14 | `68ba3ea` | "Hopefully fixed VCN": `newShared`, `newSharedUserClient`, X5000↔X6000 `AMDAccelSharedUserClient start/stop` bridging added — the user-client path VDA needs |
| 2023-02-24 | `e4c2d40` | VCE capability set false |
| 2023-03-13 | `f157021` | project rebranded WhateverRed → NootedRed |
| 2023-03-16 | `890b617` | "Add UnfairGVA patches": `_cs_validate_page` hook, `hwgva-id`, VideoToolbox DRM-model patch, CoreLSKD CPUID patch (all from WhateverGreen's UnfairGVA) |
| 2023-03-19 / 03-23 | `37d94cd`, `822bd94` | `hwgva-id` patch removed, then reverted back in |
| 2023-03-27 | `addbd85` | "Fix VCN HW Capability": sets `HasVCN0` (Monterey layout `0xAF = true`) |
| 2023-03-30 | `de1f0e0` | "Remove DMCUB fw, use VCN fw from Windows": per-chip `*_vcn.bin` consolidated to `ativvaxy_rv.dat`/`ativvaxy_nv.dat`, injected as device type 0 |
| 2023-04-09…04-14 | `cf235bc`…`ec7dd43`, merged as `2eb093a` | **PR #35 "Fix VCN (at least partially)"**: `VAAcceleratorInfo::identify`, `Vcn2DecCommand` and `Vcn2EncCommand` patches, `VAFactory::create*` patches |
| 2023-05-01 | `884555f` | "VCN Patch and Info.plist fixup" |
| 2023-05-28 | `0a2e5b7` | "Add native Monterey support": adds the **`-nredvcn`** gate around the user-space patch block |
| 2023-05-31 | `3456715`, `d2e1660`, `17624be`, `a9bccd6` | AppleGVAHEVCEncoder spoof; GVA model spoof changed iMacPro1,1 → MacPro7,1; patch-set cleanups |
| 2023-06-10 | `55a03cc`, `47f258f`, `219cf4b` | DYLD patches moved into their own `DYLDPatches` class; device-type spoof fix; Ventura `add*FormatPacket` fix |
| 2023-06-13 | `877d06b` | GVA spoof reverted to iMacPro1,1 |
| 2023-06-30…07-14 | `6f8afb1`, `d0b87b3`, `fecf39e`, `e8a9b7a`, `fb6df04`, `41a62f6`, `cb0e2fd`, `6faccee` | "Navi 12 spoof" experiment (later reverted); "Fix VCN patches. Fixes #120"; "Fix VCN firmware"; Catalina `getIpFw` wrap |

### 2.2 Visible-history era (2023-08 onward)

| Date | Commit | Event |
| --- | --- | --- |
| 2023-08-07 | `1444841` | PR #130 squashed into root commit "Add Catalina support (#130)" (tree identical to `dd3bf7b`) |
| 2023-09-06 | `fb1bb3d` | "Inject all correct firmware. System is now smooth and cool": firmware read directly from `ativvaxy_rv/nv.dat`; `VAAcceleratorInfo::identify` spoof reverted Navi12 → Navi10 (`0xD→0xC`, Ventura `0x6→0x5`) |
| 2023-09-12 | `74bd2d5` | "Make `-nredvcn` not KP on Catalina. **VCN not functional yet**": adds `_cs_validate_range` fallback for Catalina |
| 2023-09-12 | `eae2a4f`, `61c9c98`, `a67c94b` | Apple types, PascalCase file renames (`kern_dyld_patches.*` → `DYLDPatches.*`), debug prints |
| 2023-10-15 | `abbafa9` | "Clean up DYLD patch code": gate renamed **`-nredvcn` → `-ChefKissInternal`**, Catalina support removed entirely (Catalina now returns immediately), dev-only warning banner added |
| 2023-10-22 | `7079e57` | "Compress firmware" (zlib-compressed blobs; reverted 2024-04-18 by `acd58c0`) |
| 2024-03-13 | `adfc907` | "Updates for 14.4" (last parent) |
| 2024-03-13 | `4523553` | **"Remove the mostly broken VCN code"**: deletes DYLDPatches + kernel bridging + `HasVCN0=true` |
| 2024-06-16…2024-11-12 | `a393a03`, `34bb3dd`, `af519ee`, `01b12fa` | refactors; `hasVCN0Field` `ObjectField` introduced (`34bb3dd`, 2024-08-23) — a *later* refactor of the capability write, not legacy engine code |
| 2025-11-15 | `5524cf0` | "Remove dependency on AMDRadeonX6000": X6000 accelerator no longer loaded; DCN classes reimplemented with `RuntimeMC`; `// No VCN? :-(` comment added in `X5000::allocateHWEngines` |
| 2026-05-28 | `f7b3e38` | firmware updated to Adrenaline 26.5.2 (`ativvaxy_rv.dat` 367,360 B, `ativvaxy_nv.dat` 403,904 B) |
| 2026-08-11 | `e000f25` | "Consolidate project structure": sources moved under `NootedRed/iVega/`, firmware now `#embed`ed |
| 2026-10 (branch) | `c21640d`, `1e43a4e` | DeviceProfile layer + Phase 1 `VDA` skeleton (inert, `-NRedVDA` opt-in) |

---

## 3. Kernel-side architecture at removal (state of `4523553^`)

### 3.1 Roles of the two Apple kexts

- **`AMDRadeonX5000`** (Raven driver) is the *active* accelerator for every supported Vega iGPU,
  including Renoir/Green Sardine. NootedRed injects a personality whose `IOClass` is
  `AMDRadeonX5000_AMDVega10GraphicsAccelerator` for all device IDs
  (`NootedRed/Firmware/Drivers.xml@4523553^`, X5000 dict).
- **`AMDRadeonX6000`** (Navi driver) is *force-loaded but never started*. Its personality uses
  `IOClass = AMDRadeonX6000_AMDNavi10GraphicsAccelerator`, `IOMatchCategory = IOAccelerator`,
  `IOPropertyMatch = {LoadAccelerator: true}`, and the same six Vega iGPU PCI IDs
  (`NootedRed/Firmware/Drivers.xml@4523553^`, X6000 dict). `AMDRadeonX6000_AMDGraphicsAccelerator::start`
  is routed and replaced by a function returning `false` (`X6000.cpp@4523553^:201`,
  `bool X6000::wrapAccelStartX6000() { return false; }`). The original 2022 commit comment is explicit:
  "We only use it to force-load `AMDRadeonX6000` and snatch the VCN/DCN symbols"
  (`NootedRed/kern_x6000.cpp@1444841:145-148`).

### 3.2 Cross-driver metaclass bridging

Because X5000 objects must appear as X6000 classes (and vice versa) to `OSMetaClassBase::safeMetaCast`,
NootedRed routes the kernel symbol `__ZN15OSMetaClassBase12safeMetaCastEPKS_PK11OSMetaClass` and keeps a
5×2 table of metaclass pairs (`NRed.cpp@4523553^:141-146, 168-180`;
`NRed.hpp@4523553^:220`):

```cpp
OSMetaClass *metaClassMap[5][2] = {{nullptr}};   // NRed.hpp@4523553^:220
```

`wrapSafeMetaCast` runs the original cast, and on failure retries with the paired metaclass in either
direction (`NRed.cpp@4523553^:168-180`). The pairs are `AMDAccelVideoContext`, `AMDAccelDisplayMachine`,
`AMDAccelDisplayPipe`, `AMDAccelChannel`, `IAMDHWChannel`. Notably `AMDAccelChannel` is resolved
*swapped* (X5000 takes `[3][1]`, X6000 takes `[3][0]`, `X5000.cpp@4523553^:42`,
`X6000.cpp@4523553^:34`), i.e. each driver's `gMetaClass` symbol is repointed at the other driver's
metaclass so that objects created by one driver cast correctly in the other.

### 3.3 Engine allocation and slot offsets

`AMDRadeonX5000_AMDVega10Hardware::allocateHWEngines` is replaced to allocate three engines directly:

```cpp
auto fieldBase = catalina ? 0x348 : 0x3B8;
auto *pm4   = OSObject::operator new(0x340);  callback->orgGFX9PM4EngineConstructor(pm4);
auto *sdma0 = OSObject::operator new(0x250);  callback->orgGFX9SDMAEngineConstructor(sdma0);
auto *vcn0  = OSObject::operator new(0x2D8);  X6000::callback->orgVCN2EngineConstructor(vcn0);
```
(`X5000.cpp@4523553^:169-186`)

Slots relative to the `AMDHardware` object base: PM4 at `fieldBase+0`, SDMA0 at `fieldBase+8`, VCN0 at
`fieldBase + (catalina ? 0x30 : 0x40)` (`X5000.cpp@4523553^:171-183`). The VCN2 constructor is X6000's,
resolved in `X6000::processKext` (`X6000.cpp@4523553^:27`). The PM4/SDMA sizes differ from the later,
post-2024 code, but the slot layout is the important part.

### 3.4 Video-path bridging

Six X5000/X6000 entry points were routed/forwarded (`X5000.cpp@4523553^` and `X6000.cpp@4523553^`):

| X5000 symbol (replaced) | Forwarded to X6000 | Purpose |
| --- | --- | --- |
| `AMDGFX9GraphicsAccelerator::newVideoContext` | `AMDGFX10GraphicsAccelerator::newVideoContext` | create the video (VCN) context object |
| `IAMDSMLInterface::createSMLInterface` | X6000's `createSMLInterface` | system-management layer used by the video engine |
| `AMDGraphicsAccelerator::newShared` | X6000's `newShared` | create `AMDAccelShared` |
| `AMDGraphicsAccelerator::newSharedUserClient` | X6000's `newSharedUserClient` | create the VDA user client |
| `AMDAccelSharedUserClient::start` / `::stop` | X5000's originals were called *from* X6000 wrappers | keep the user-client life cycle on the X5000 object |
| `AMDAccelShared::SurfaceCopy` | X6000's `SurfaceCopy` with HW align-manager vtable swapped | decode surfaces must be copied by the Navi align manager |

Evidence: `X5000.cpp@4523553^:59-60, 75-76, 369-386`; `X6000.cpp@4523553^:56-66, 203-209, 282-288`;
`X5000.hpp@4523553^` (org pointers), `X6000.hpp@4523553^:17-39`. The align-manager swap pattern is the
`HWALIGNMGR_ADJUST`/`HWALIGNMGR_REVERT` macro pair around `wrapAccelSharedSurfaceCopy`
(`X6000.cpp@4523553^:282-288`).

This set is exactly the "missing link" added by `68ba3ea` ("Hopefully fixed VCN", 2023-02-14) after
`newVideoContext`/`createSMLInterface` had been bridged in `9ade06d`/`c066001`.

### 3.5 Hardware capability flags

`AMDGFX9Hardware::setupAndInitializeHWCapabilities` is wrapped and rewrites named fields; the layout is
OS-version dependent (`X5000.cpp@4523553^:188-328`):

| OS | base | HasUVD0 | HasVCE | HasVCN0 |
| --- | --- | --- | --- | --- |
| Catalina 10.15 | `+0x28` | `0x68` | `0x6A` | `0x6B` |
| Big Sur 11 … Monterey 12 | `+0x28` | `0x84` | `0x86` | `0x87` |
| Ventura 13+ | `+0x30` | `0x84` | `0x86` | `0x87` |

At removal the values were `HasUVD0 = false`, `HasVCE = false`, **`HasVCN0 = true`**
(`X5000.cpp@4523553^:333-337`). This is the single most important line of the kernel-side design: the
Vega driver advertised a VCN engine for every APU, including VCN 1.x Raven parts.

### 3.6 Firmware injection at removal

`AMDRadeonX5000HWLibs` firmware directory creation is wrapped once:

- `wrapPopulateFirmwareDirectory` calls the original, then builds one `AMDFirmware` from the embedded
  blob — `ativvaxy_nv.dat` for `chipType >= Renoir` (ipVersion `0x0202`), `ativvaxy_rv.dat` otherwise
  (ipVersion `0x0100`) — and inserts it into firmware-directory slot **6**
  (`HWLibs.cpp@4523553^:267-282`).
- `AmdTtlFwServices::getIpFw` is wrapped so that requests for either `ativvaxy_*.dat` name are answered
  with the embedded blob (`HWLibs.cpp@4523553^:284-293`).
- The device-type table is patched to report **`kAMDDeviceTypeNavi10`** for the running iGPU
  (`HWLibs.cpp@1e43a4e:764`; the same technique existed at removal), which is why the Navi VCN firmware
  directory entry is accepted by the TTL/PSP stack.

No VCN-specific PSP command patch existed: VCN firmware loading was expected to happen through the normal
HWLibs/PSP path once the firmware entry and `HasVCN0` existed. That is a code-reading inference, not a
verified runtime claim.

### 3.7 APU and macOS support at removal

Chip detection (`NRed.cpp@4523553^:192-224`): `0x15D8` → Picasso/Raven2, `0x15DD` → Raven/Raven2,
`0x1636`/`0x164C` → Renoir, `0x1638`/`0x15E7` → Green Sardine. All of these got `HasVCN0 = true` and a
VCN2 engine object. The user-space side split by generation: `chipType < Renoir` (Raven/Picasso/Raven2 =
VCN 1) got the extra command patches; Renoir/Green Sardine (VCN 2.2) did not.

OS support of the kext at the time: Catalina 10.15 through Sonoma 14.x (the `Updates for 14.4` commit is
the direct parent; `HWLibs.cpp`/`X6000.cpp` carry 14.4-specific patterns). The user-space patches had
only two variants, *pre-Ventura* and *Ventura+*; there was no Sonoma-specific pattern set. At removal
Catalina was excluded entirely from the user-space hook (`DYLDPatches.cpp@4523553^:18-19`).

---

## 4. User-space architecture: `DYLDPatches`

### 4.1 Delivery mechanism: `_cs_validate_page`

Lilu's user patcher cannot work on macOS 11+ (the project's own current analysis,
`analysis/lilu-userpatch.md`, documents this and the in-tree `UserPatches` module exists precisely
because of it). The legacy code therefore routed the kernel symbol `_cs_validate_page`
(`DYLDPatches.cpp@4523553^:36`) and, in the wrapper, called the original validation first and then
rewrote the page's bytes (`DYLDPatches.cpp@4523553^:44-131`). Practically:

1. `vn_getpath()` the vnode; if the path is the dyld shared cache, run the shared-cache patch set;
2. otherwise only the two CoreLSKD frameworks are patched (CPUID spoof) and everything else is ignored;
3. pages of the shared cache are modified after validation, so Code Signing is not what is being
   defeated — the pages are simply the in-memory system framework code every process gets.

Catalina has no `_cs_validate_page`; `74bd2d5` (2023-09-12) added a `_cs_validate_range` wrapper for
Catalina, and `abbafa9` (2023-10-15) removed the whole Catalina path again. This matches the current
analysis of the kernel's `_cs_validate_page` (Big Sur+) vs `_cs_validate_range` (Catalina) split.

### 4.2 Boot-argument gate

- 2023-05-28 (`0a2e5b7`): the route + `hwgva-id` property were gated behind **`-nredvcn`**.
- 2023-10-15 (`abbafa9`): renamed to **`-ChefKissInternal`**, and a banner was printed:
  > "Dear end users, do NOT use `-ChefKissInternal`. THIS FLAG ENABLES FEATURES FOR *DEVELOPER*
  > TESTING." (`DYLDPatches.cpp@4523553^:15-17`)
- Gate condition at removal: not Catalina, running normally, and `-ChefKissInternal` present
  (`DYLDPatches.cpp@4523553^:14-19`).

The kernel side (engine object, `HasVCN0=true`, firmware injection) was **not** gated by this boot
argument. Only the user-space patches and the DRM property were. That asymmetry is why "VCN not
functional" and "HasVCN0 advertised" could both be true at once.

### 4.3 Patch inventory

`DYLDPatches` is a small `find`/`find+mask`/`replace(+mask)` engine (`DYLDPatches.hpp@4523553^:8-55`)
with the patch data in the header. All offsets refer to `4523553^`.

Shared-cache (all macOS versions):

| Target binary / function | Decl | Applied at | Intent (from code comments) | Provenance |
| --- | --- | --- | --- | --- |
| VideoToolbox — literal `"MacPro5,1\0MacPro6,1\0IOService"` | `DYLDPatches.hpp:73` | `.cpp:61-62` | replace the first 20 bytes with `BaseDeviceInfo` model identifier, relaxing the DRM model check | WhateverGreen UnfairGVA `RelaxHdcpRequirements` |
| AppleGVA — literal `"board-id\0hw.model"` | `:78-79` | `.cpp:67` | makes AppleGVA read `hwgva-id` instead of `board-id` | WEG UnfairGVA `CustomAppleGvaBoardId` |
| AppleGVAHEVCEncoder — `"vendor8bit\0IOService\0board-id"` | `:88-89` | `.cpp:68` | same spoof for the HEVC encoder path | WEG (NootedRed adds the encoder-specific string) |
| CoreLSKDMSE / CoreLSKD — `mov eax,1; cpuid` | `:81-85` | `.cpp:52-59` | rewrite CPUID leaf to Haswell signature, so hardware-DRM stream decoding is allowed on the "old CPU" path | WEG UnfairGVA `AllowHardwareDrmStreamDecoderOnOldCpuid` |
| device tree `/` property `hwgva-id` | `:75` | `.cpp:29-33` | value `Mac-7BA5B2D9E42DDD94` = iMacPro1,1 board-id | WEG UnfairGVA |

VA/video-driver (`AMDRadeonVADriver2`, shared cache on the test system):

| Target | Decl | Applied at | Intent |
| --- | --- | --- | --- |
| `VAAcceleratorInfo::identify` (pre-Ventura pattern) | `:97-104` | `.cpp:88-90` | fallback enum `0x12` → `0xC` (Navi 10), so the VA driver takes the GFX9/Navi code path instead of "unknown device" |
| `VAAcceleratorInfo::identify` (Ventura+ pattern) | `:107-114` | `.cpp:74-76` | `0xB` → `0x5` (same idea, different immediate) |
| `VAFactory::createGraphicsEngine` | `:121-129` | `.cpp:91-93` | force the GFX9 variant (family value `7 → 4`) |
| `VAFactory::createGraphicsEngine` + `createImageBlt` combined (Ventura+) | `:131-139` | `.cpp:77-81` | same |
| `VAFactory::createImageBlt` | `:173-180` | `.cpp:94-95` | force GFX9 image blitter |
| `VAFactory::create*VP` | `:145-156` / `:159-166` | `.cpp:96-97` / `.cpp:82-83` | force GFX9 video processors |
| `VAAddrLibInterface::init` | `:187-194` | `.cpp:102-103` | remove the `family == 0x8D` (Vega) check so GFX9 AddrLib is used |

`VAAcceleratorInfo`/`VAFactory`/`VAAddrLib` tables were added by PR #35 (April 2023); the model spoof
only applies to Big Sur+ (Catalina excluded), and the "Navi 12 vs Navi 10" enum was briefly flipped
(`d0b87b3`, `cb0e2fd`) and finally set back to Navi 10 by `fb1bb3d` (2023-09-06).

### 4.4 VCN 1 command translation

For `chipType < Renoir` only (`DYLDPatches.cpp@4523553^:107`), 13 additional patches rewrite the
**VCN2 user-space command emitters** into VCN1-compatible opcodes/offsets:
`Vcn2DecCommand::writeUvdNoOp/EngineStart/GpcomVcpuCmd/Data0/Data1` (`:109-116`),
`Vcn2EncCommand::addEncodePacket/SliceHeader/IntraRefresh/ContextBuffer/BitstreamBuffer/FeedbackBuffer`
(`:117-125`), and `addInputFormatPacket`/`addOutputFormatPacket` (`:126-130`) which are turned into
`mov eax, 0; ret` stubs because "VCN 1 does not have these packets"
(`DYLDPatches.hpp@4523553^:293-297`). This is the clearest evidence that the design deliberately used the
**VCN2 (Navi) user-space driver** to drive **VCN1 hardware**.

### 4.5 Development churn visible in the patch data

The 2023 commit log is a sequence of fixups, which is itself evidence of fragility: `Fix VCN patches`
(`fb6df04`, "Fixes #120" — a macOS 12.5 login hang caused by wrong patch arguments), `VCN patch fixup`
(`e8a9b7a`), `Slightly improve VCN patches` (`a9bccd6`), Ventura format-packet fix (`219cf4b`), several
device-spoof reversals, and finally "VCN firmware" rework (`41a62f6`). Issue #120 (2023-07-10) shows the
VCN patch set could break unrelated boot behaviour.

---

## 5. Firmware path: legacy vs current

### 5.1 What actually happened to the blobs (correcting the starting note)

The removal commit `4523553` touched **no firmware file**. Blob history:

| Date | Commit | Firmware state |
| --- | --- | --- |
| 2022-07-22 | `335b5a5` | one `ativvaxy_rv.dat` (early injection) |
| 2022-09-25…2023-02-27 | `4993611`, `33c1f1c`, `8cc758f`, `3b10b89` | ported to per-chip `raven_vcn.bin`, `raven2_vcn.bin`, `picasso_vcn.bin`, `renoir_vcn.bin`, `green_sardine_vcn.bin` (+ `*_dmcub.bin`), sizes 365,536 B (VCN1) / 402,624 B (VCN2) |
| 2023-03-30 | `de1f0e0` | consolidated: `renoir_vcn.bin → ativvaxy_nv.dat`; new `ativvaxy_rv.dat`; per-chip VCN/DMCUB blobs deleted |
| 2023-07-11 | `41a62f6` | reverted to per-chip names (`ativvaxy_rv.dat` deleted; `ativvaxy_nv.dat → green_sardine_vcn.bin`; raven/raven2/picasso/renoir re-added), because the loader again remapped `%s_vcn.bin → ativvaxy_*.dat` |
| 2023-08-07 | `1444841` (import) | same per-chip state as `dd3bf7b` |
| 2023-09-06 | `fb1bb3d` | back to direct `ativvaxy_rv.dat` (366,304 B) / `ativvaxy_nv.dat` (402,752 B); raven/raven2/renoir per-chip blobs deleted, picasso renamed to rv and green_sardine renamed to nv |
| 2024-03-13 | `4523553` | VCN code deleted; **firmware left in place** |
| 2024-04-18 | `acd58c0` | zlib firmware compression removed (added `7079e57`) |
| 2026-05-28 | `f7b3e38` | blobs updated to Adrenaline 26.5.2: `ativvaxy_rv.dat` 367,360 B, `ativvaxy_nv.dat` 403,904 B |
| 2026-08-11 | `e000f25` | firmware `#embed`ed instead of Xcode resources |

Provenance from the commit message of `de1f0e0`: "Remove DMCUB fw, **use VCN fw from Windows**" — the
VCN blobs come from the Windows Adrenalin firmware package (later re-synced several times, latest
`f7b3e38`). The `ipVersion` passed to `createFirmware` was `0x0202` for Renoir/Green Sardine and
`0x0100` for Raven/Picasso/Raven2, i.e. VCN 2.2 vs VCN 1.0.

### 5.2 Mechanism comparison

| Aspect | Legacy (at removal) | Current (`feature/vda-hardware-decode` @ `1e43a4e`) |
| --- | --- | --- |
| Blob source | `NootedRed/Firmware/ativvaxy_{rv,nv}.dat` (Xcode resources) | same files, `#embed`ed in `HWLibs.cpp:55-59` |
| Loader | `wrapPopulateFirmwareDirectory` + `wrapGetIpFw`; `createFirmware(..., ipVersion, name)`; `putFirmware(fwDir, 6, fw)` (`HWLibs.cpp@4523553^:267-293`) | identical logic, `kAMDDeviceTypeNavi10` name, `HWLibs.cpp@1e43a4e:884-926` |
| Device type | patched to Navi 10 (`0x06`) | patched to `kAMDDeviceTypeNavi10` (`HWLibs.cpp@1e43a4e:764`) |
| Name mapping | request `ativvaxy_*.dat` → embedded blob | same |
| Engine use | VCN2 engine object + rings allocated by Apple's VCN code once `HasVCN0=true` | no VCN engine is constructed (`// No VCN? :-(`, `X5000.cpp@1e43a4e:327`) |
| Gate | none for firmware; none for kernel side | none for firmware; `hasVCN0Field` written from profile but always false |
| Provenance | Windows Adrenalin (multiple re-syncs) | same, updated to Adrenaline 26.5.2 |

So today the firmware plumbing is *already there and identical in shape* to what the legacy kernel side
used; only the engine/ring/user-space half is gone.

---

## 6. Why it was removed — facts, inferences, unknowns

### 6.1 Facts

1. Commit message `4523553`: **"Remove the mostly broken VCN code"** (no further text). Parent is
   `adfc907` "Updates for 14.4", same day.
2. Previous VCN commit `74bd2d5` (2023-09-12): **"VCN not functional yet."**
3. The feature was always developer-gated: `-ChefKissInternal` with a banner telling users not to use it
   and not to report issues; earlier `-nredvcn` (`DYLDPatches.cpp@4523553^:15-19`).
4. There is external evidence of *partial* success: PR #35 body says "Helps with #28, but only slightly.
   Well, it works well enough for now. **There's proper hardware acceleration for decoding now** and
   it's generally smoother" (2023-04). Issue #163 (closed as duplicate of #28): on a Renoir 5800H with
   macOS 13.5.1 and `-nredvcn`, **HEVC hardware decode worked, H.264 did not** (2023-09-07).
5. There is evidence of collateral breakage: issue #120 (macOS 12.5 login stuck) was fixed by
   `fb6df04` "Fix VCN patches" (2023-07-10). The commit log around VCN is a chain of fixups/reverts
   (`d0b87b3`/`fecf39e`/`cb0e2fd`, `37d94cd`/`822bd94`, `d2e1660`/`877d06b`).
6. The maintainer's own retrospective, issue #442 (2026-08-10): *"In the past, AMDRadeonX6000 was
   mangled to use some of its code, primarily for the DCN display, and historically for the **removed
   buggy VCN code**. Since the first stable release, that has been replaced with a proper
   reimplementation…"*
7. The tracking issue #28 (opened 2023-03-17) is still **open** (updated 2026-08-13). A 2025-11-16
   maintainer comment there: *"with the latest release, the kext side of the implementation is now
   straight forward. however, the userspace patching is not, due to Lilu's limited 'run find replace on
   shared cache file … every time it's read'."*
8. After removal, a user filed #249 (2024-04-09): "NootedRed does not support hardware decode". This is
   consistent with the removal being a real functional loss, not just cleanup.
9. The user-space patterns were last substantively updated in October 2023 (`abbafa9`); the only
   later touch is `7079e57` (firmware compression, one line). The removal happened five months later,
   immediately after the 14.4 update commit — but **`DYLDPatches` was not updated for 14.4**. Whether
   the 14.4 binaries invalidated the patterns cannot be proven from the repo.

### 6.2 Inferences (explicitly not proven)

- "Mostly broken" most plausibly means: the kernel advertised `HasVCN0` unconditionally, the engine
  object was constructed, and the user-space patches only sometimes produced a working decode path —
  with HEVC working and H.264 failing on at least one Renoir system (issue #163), plus collateral
  regressions (issue #120). The design was also architecturally fragile: two Apple drivers pretending
  to be each other, 13 hard-coded VCN1 opcode translations, and byte patches against three different
  user-space binaries that Apple updates every release.
- The timing suggests the author removed it while doing 14.4 work rather than after a specific test
  failure; there is no commit or issue that documents a single triggering bug.
- The "VCN not functional yet" message (2023-09-12) likely refers to the *Catalina* bring-up that same
  commit was about, or to the feature as a whole being unfinished; it cannot be reconciled with the
  "decode works" claims without specifying APU/OS/codec. We should treat "did hardware decode ever
  work?" as **partially yes, on some Renoir/macOS 13.x systems, for some codecs**, not as a clean
  yes/no.

### 6.3 What would settle the open questions

- The 26 comments on issue #28 (GitHub currently lazy-loads them; only some are retrievable as page 1)
  and the PR #35 review thread.
- Any release notes / Telegram/Discord announcements from 2024-03; the repo's `CHANGELOG.md` does not
  mention VCN.
- A hardware test of an old NootedRed build with `-nredvcn`/`-ChefKissInternal` on a Renoir system
  (out of scope here; would require installing an old kext and rebooting).
- Access to a pre-2024 kernelcache/dyld cache image to check which `DYLDPatches` patterns still matched
  on 14.4.

---

## 7. Remnants in the current tree

Current tree here = `feature/vda-hardware-decode` @ `1e43a4e` (includes Phase 1 `VDA` skeleton).
`master` @ `4b4f453` differs only by not having the Phase-1 files.

### 7.1 Live remnants (inherited from the VCN era)

| Remnant | Location | Origin | Status |
| --- | --- | --- | --- |
| VCN firmware blobs | `NootedRed/Firmware/ativvaxy_rv.dat`, `ativvaxy_nv.dat` | `de1f0e0` (2023-03) → `fb1bb3d` (2023-09) → `f7b3e38` (2026-05) | embedded + injected every boot |
| VCN firmware load path | `HWLibs.cpp@1e43a4e:55-59, 884-906, 915-926` | legacy `wrapPopulateFirmwareDirectory` + `wrapGetIpFw` | fully functional, but no consumer engine |
| Device-type spoof to Navi 10 | `HWLibs.cpp@1e43a4e:764` | legacy device-type table patch | live |
| `HasVCN0` capability write | `X5000.cpp@1e43a4e:96, 113, 135, 149, 360`; `X5000.hpp@1e43a4e:44` | offsets survived removal (values set false at `4523553`), later refactored into `ObjectField` by `34bb3dd` (2024-08-23) and re-wired to profiles in 2026 | live but always false |
| `HasVCN0` in capability enum | `X5000.cpp@1e43a4e:322-330` (`// No VCN? :-(`), `X5000.hpp:42-53` | comment added `5524cf0` (2025-11-15) when X6000 dependency was dropped | live placeholder |
| VDA capability advertisement | `Personalities/com.apple.kext.AMDRadeonX5000.xml@1e43a4e`: `IODVDBundleName` :15/:149/:253, `IOGVACodec = AMDVCN2` :17/:151/:255, `IOGVAHEVC*` :26/:55 etc., `SWIP_properties` :93/:227/:331 (`EnableSwVCNFWLoading=0`, `EnableVCNDPG=1` on Renoir) | present in import root (`1444841`), predates it (2023-05 personality work) | live: the accelerator still tells VideoToolbox it has AMDVCN2 codecs even though no VCN engine exists |
| X5000 accelerator personality with `IODVDBundleName` | same file | legacy era | live |
| `AMDRadeonX6000Framebuffer` personality | `Personalities/com.apple.kext.AMDRadeonX6000Framebuffer.xml`, `DriverInjector.cpp@1e43a4e:50` | legacy DCN borrowing (not VCN) | live |

### 7.2 What is *not* a remnant (avoid mis-attribution)

- `DeviceProfile`/`MediaPolicy` (`DeviceProfile.hpp:55-90`, `DeviceProfile.cpp:41-65, 102-107`),
  `VDA.hpp`/`VDA.cpp` (gate `mediaEnginesEnabled()`), the `-NRedVDA` boot arg: **new 2026 work**
  (`c21640d`, `1e43a4e`), deliberately inert. The media policy *data* (VCN generation, firmware name,
  ring sizes, H.264/HEVC/AV1 flags) re-states knowledge from the legacy era but is not legacy code.
- `hasVCN0Field` as an `ObjectField`: introduced by the post-removal refactor `34bb3dd` (2024-08-23),
  not by the VCN feature. The legacy code used `getMember()` with raw offsets.
- `PenguinWizardry::RuntimeMC` DCN classes (`AMDGFX9DCN*Display.cpp`) and `pm4EngineMC`/`sdmaEngineMC`
  (`X5000.hpp@1e43a4e:52-53`, `X5000.cpp@1e43a4e:178-179, 324-325`): the 2025 "proper reimplementation"
  that replaced X6000 borrowing (`5524cf0`).
- PSP VCN enum names (`GPUDriversAMD/PSP.hpp@1e43a4e:48, 62-63, 76`: `kPSPFirmwareVCN0`,
  `kPSPFirmwareRLCVCN0DRAM/RLCVCN1DRAM`, `kPSPFirmwareVCN1`): they arrive with the consolidated
  `GPUDriversAMD/PSP.hpp` reimplementation (`5524cf0`/`e000f25`), are **unused** in the tree today, and
  were not part of the removed 2024 code.
- `kAMDHWEngineTypeVCN0/VCN1` in `GPUDriversAMD/Accel/HWEngine.hpp`: ABI enum from the same later
  reimplementation; no engine of those types is allocated.

---

## 8. Revivability on macOS 15.8.1 (Darwin 24.6), Renoir `0x1638`

Target facts (read-only, this machine): macOS 15.8.1 (24H32); `AMDRadeonX5000.kext`,
`AMDRadeonX5000HWServices.kext`, `AMDRadeonX6000.kext` and `AMDRadeonVADriver2.bundle` are all present;
`AMDRadeonX6000`'s `Info.plist` still declares only Navi accelerator personalities; Apple kext Mach-Os and
`AMDRadeonVADriver2` are **not on disk** (kernelcache/dyld shared cache only), so legacy byte patterns
cannot be verified without cache extraction. Renoir/Green Sardine `0x1638` maps to the `kVcn2_2` /
`ativvaxy_nv.dat` profile already (`DeviceProfile.cpp:53-65`).

### 8.1 What already exists and is reusable

1. **Firmware path**: `ativvaxy_nv.dat` embedded and injected exactly as the legacy code did;
   `ipVersion 0x0202`; Navi10 device-type spoof. This is the single biggest piece already in place.
2. **Capability plumbing**: `hasVCN0Field` with correct per-OS offsets (macOS 14+/15 layout `0xB7`,
   `X5000.cpp@1e43a4e:149`), written from `DeviceProfile::MediaPolicy` behind `VDA::mediaEnginesEnabled()`
   (`X5000.cpp:351-365`). Flipping the profile flag and making `VDA` report `ready` is a small change.
3. **Engine-slot knowledge**: `pm4EngineField`/`sdma0EngineField` and the `AMDHardware` base (`0x3B8` for
   Big Sur+, `pm4 +0`, `sdma0 +8`; legacy VCN was `+0x40` = `0x3F8`) is documented by the legacy code and
   still matches the current object layout used for PM4/SDMA.
4. **Metaclass machinery**: `PenguinWizardry::RuntimeMC` can build OSMetaClasses at runtime, and X5000
   already uses native metaclass allocation for PM4/SDMA (`pm4EngineMC->alloc()`), so the kernel side no
   longer *needs* X6000 as a donor — a VCN engine class could in principle be reimplemented the same way
   the DCN classes were (this is the project's current direction, issue #442).
5. **User-patch infrastructure**: `NootedRed/UserPatches.cpp/.hpp` has a current, macOS-15-specific
   `_cs_validate_page` + `_cs_validate_range` backend with exact-page matching (routes at
   `UserPatches.cpp@1e43a4e:1002, 1011`; mechanism documented in `UserPatches.hpp:30-44` and
   `docs/CONTRIBUTING-USERPATCHES.md`). The legacy hook was a simpler version of the same idea and can
   be extended; but see 8.3.
6. **Power/GFXOFF groundwork**: the legacy power-gating experiments (`26bb993`, `55fcdd1`, `8bdee64`,
   `9b85acc`) and later GFX CG PG / GFXOFF work in the tree show the coupling between VCN, MMHUB and
   GFXOFF. Any revivability plan must route through the existing SMU/PSP bring-up rather than fight it
   (the 2023 notes literally concluded manual VCN/MMHUB power-up "seems to be unnecessary").

### 8.2 What must be rebuilt

1. **A VCN2 engine class.** The removed code instantiated `AMDRadeonX6000_AMDVCN2HWEngine` (constructor
   size `0x2D8`) after force-loading X6000. On 15.8.1 X6000 exists but the current kext deliberately
   does not load its accelerator (no injected personality, no `kextRadeonX6000` entry in `Kexts.cpp`).
   Options: (a) reintroduce the X6000 force-load and the 2022–2024 bridging (conflicts with the current
   architecture and issue #442 direction); or (b) reimplement the VCN2 engine class with `RuntimeMC`
   and reimplement `newVideoContext`/`createSMLInterface`/`AMDAccelShared*`/`SurfaceCopy` natively —
   significantly more work and requires reimplementing Apple's VCN/PSP/ring logic from observation.
2. **Ring/doorbell/wait/submit logic.** The legacy code relied entirely on Apple's X6000 VCN code for
   ring allocation and submission; nothing in NootedRed allocated VCN rings. Phase 3 cannot be satisfied
   by "flip `hasVCN0`" alone unless option (a) is taken.
3. **macOS 15.8.1 user-space patch data.** Every legacy byte pattern (`VAAcceleratorInfo::identify`,
   `VAFactory::create*`, `VAAddrLibInterface::init`, `Vcn2Dec/EncCommand`, the AppleGVA/VideoToolbox
   literals) was derived from macOS 12/13-era binaries. `AMDRadeonVADriver2` and the VideoToolbox/AppleGVA
   code on 15.8.1 are different builds, and they are in caches. All data must be re-derived (from the
   dyld shared cache, with exact page images as the current `UserPatches` design requires).
4. **A user-space patch *strategy*, not just data.** The maintainer's 2025 comment (issue #28) is the
   key: the kext side is now easy, the user-space side is blocked by Lilu's "find/replace on the shared
   cache every time it's read" approach. The current in-tree `UserPatches` uses whole-page exact-match
   images (safer, but every OS update invalidates them), so even with data, maintaining VDA across
   macOS updates is expensive.
5. **`VideoToolbox`/capability negotiation.** The personality still advertises `IOGVACodec = AMDVCN2`
   and HEVC capabilities, but there is no kernel engine; if a kernel engine is enabled without the
   user-space patches, VideoToolbox may select AMD's decoder and fail (the 2023-era failures, e.g.
   #153 `err -12473`, are in this space). A safe Phase-2/3 bring-up must decide whether to keep or
   suppress this advertisement while testing.
6. **Validation matrix.** Legacy evidence covers: Renoir 5800H / Ventura 13.5.1 (HEVC yes, H.264 no),
   Renoir 5700U / Monterey 12.5 (boot regression), Picasso / Ventura (reset hang reports). Nothing for
   Sequoia 15.x, Tahoe 26, Cezanne/Barcelo, or 0x1638 specifically.

### 8.3 Kernel-first vs user-space-first

- **Kernel side is the tractable part**: firmware injection, capability field, gating and engine-slot
  layout are already present or documented, and the current tree has native metaclass tooling. The
  missing piece (an engine object) is either a re-introduced X6000 dependency or a real reimplementation.
- **User-space is the dominant risk and the likely reason the legacy attempt died.** It is also where
  the maintainer says the blocker is. Therefore Phases 2–4 should not depend on resurrecting the legacy
  `DYLDPatches` path; they should treat VDA user-space patching as an independent research track (as the
  current `docs/VDA-Implementation-Plan.md` already does, with `-NRedVDA` default-off and software
  fallback).
- **Do not resurrect the legacy gating model.** The legacy kernel side ran unconditionally while the
  user-space side was behind a dev flag; that produced an accelerator advertising VCN with no working
  decode. The current Phase-1 rule (`hasVCN0` only true when the bring-up is validated, `-NRedVDA`
  opt-in) is strictly better and should be preserved.

### 8.4 Concrete reuse map for Phases 2–4

| Phase | Legacy asset to reuse | Caveat |
| --- | --- | --- |
| 2 (firmware/PSP) | firmware names + `ipVersion` (`0x0202` NV / `0x0100` RV), Navi10 device-type spoof, `wrapGetIpFw` semantics | already implemented; only read-back/status validation is new |
| 3 (rings/engine) | engine slot `+0x40`, engine size `0x2D8`, `HasVCN0` field, `newVideoContext`/SML/SurfaceCopy boundary list, power-gating lessons | engine class itself must be rebuilt; rings were never in NootedRed |
| 4 (VideoToolbox/VDA) | AppleGVA/VideoToolbox DRM + `hwgva-id` + `VAAcceleratorInfo`/`VAFactory`/`VAAddrLib` rationale; current personality already advertises capabilities | all patch bytes are stale; needs 15.8.1-cache-derived exact pages; maintainer flags this as the blocker |

### 8.5 Evidence that would make revivability decidable

1. Extract `AMDRadeonVADriver2` and the relevant framework pages from the 15.8.1 dyld shared cache and
   check whether any legacy `find` sequences still exist (static, no execution).
2. Extract the 15.8.1 kernelcache and check `AMDRadeonX6000` class symbols/method layout: is
   `AMDVCN2HWEngine` still present and reachable, and do the 2022-era vtable offsets still hold? (The
   legacy cross-driver patching depends on exact vtable slots.)
3. Decide architecture first: re-introduce X6000 vs `RuntimeMC` reimplementation. Issue #442 shows the
   maintainer considers the X6000 borrowing a temporary hack; a new VCN design should not silently
   revive it without that decision.
4. A single bring-up experiment that only loads firmware, reads engine status and *does not* advertise
   `HasVCN0`, to confirm the PSP/RLC VCN path on 15.8.1 without touching VideoToolbox.

---

## 9. Open questions / unknowns

1. Did hardware decode ever work on **Raven/Picasso/Raven2** (the VCN1 command-translation path)? No
   user report found; the only positive report (#163) is Renoir/HEVC.
2. Was `-nredvcn` intended for end users at any point, or always for debugging? It was gated in code and
   never documented in `README.md` (the 2023 README only credits WhateverGreen's UnfairGVA); it was
   renamed to `-ChefKissInternal` with an explicit "do not use" banner, which suggests it was always
   experimental.
3. Why exactly "mostly broken" and at which point: no single triggering issue/commit exists. The removal
   is adjacent to the 14.4 update but `DYLDPatches` was not updated for 14.4 — whether the patterns
   broke on 14.4 is untested here.
4. What the deleted code's *actual* success rate was on Sonoma 14.x: unknown; no logs or issue matrix.
5. Whether the current kernel's X5000/X6000/vtable layouts on 15.8.1 still accommodate the old engine
   construction: unknown without kernelcache extraction.

---

## 10. References

### 10.1 Commits (all hashes are in this repository unless marked "PR ref")

**Pre-import VCN origin (reachable via `refs/pull/130/head` = `dd3bf7b`, `refs/pull/35/head` = `ec7dd43`):**

- `b2c114a` 2022-08-22 "Replace UVD/VCE with VCN2 from X6000" — X6000 force-load personality, engine
  construction at `+0x3f8`, `wrapGFX10AcceleratorStart`.
- `551dcff` 2022-08-22 "Patch VCN2 VTable calls".
- `26bb993` 2023-02-07 "Fix MMHUB and VCN power gating".
- `9ade06d` 2023-02-08 "Replace video context in Accelerator with VCN".
- `c066001` 2023-02-08 "Replace SML Interface with VCN".
- `55fcdd1`/`8bdee64`/`9b85acc` 2023-02-12…02-18 VCN/MMHUB power-up add/remove experiments.
- `68ba3ea` 2023-02-14 "Hopefully fixed VCN" — `newShared`, `newSharedUserClient`,
  `AMDAccelSharedUserClient start/stop` bridging.
- `e4c2d40` 2023-02-24 "Set VCE-related cap 0x86 to false".
- `31ad291`/`3b10b89` per-chip VCN firmware naming/Green Sardine.
- `890b617` 2023-03-16 "Add UnfairGVA patches".
- `37d94cd`/`822bd94` 2023-03-19/23 hwgva-id patch remove/revert.
- `addbd85` 2023-03-27 "Fix VCN HW Capability".
- `de1f0e0` 2023-03-30 "Remove DMCUB fw, use VCN fw from Windows".
- `2eb093a` 2023-04-14 "Fix VCN (at least partially) (#35)".
- `0a2e5b7` 2023-05-28 "Add native Monterey support" — adds `-nredvcn`.
- `3456715`, `d2e1660`, `17624be`, `a9bccd6`, `877d06b` May–Jun 2023 GVA spoof and patch cleanups.
- `55a03cc` 2023-06-10 "Move DYLD patches to separate class".
- `47f258f`, `219cf4b` 2023-06-10 device-type/Ventura format-packet fixes.
- `d0b87b3`, `fecf39e`, `cb0e2fd` Jun–Jul 2023 "Navi 12 spoof" experiment/reverts.
- `e8a9b7a`, `fb6df04` 2023-07-10 "VCN patch fixup" / "Fix VCN patches. Fixes #120".
- `41a62f6` 2023-07-11 "Fix VCN firmware"; `6faccee` 2023-07-14 Catalina `getIpFw` wrap.
- `f157021` 2023-03-13 rebrand WhateverRed→NootedRed.

**Visible history:**

- `1444841` 2023-08-07 `Add Catalina support (#130)` (root; tree == `dd3bf7b^{tree}`).
- `fb1bb3d` 2023-09-06 `Inject all correct firmware...` (direct `ativvaxy_*.dat`, Navi10 spoof).
- `74bd2d5` 2023-09-12 `Make -nredvcn not KP on Catalina. VCN not functional yet`.
- `abbafa9` 2023-10-15 `Clean up DYLD patch code` (`-ChefKissInternal`, Catalina excluded).
- `7079e57` 2023-10-22 `Compress firmware`; `acd58c0` 2024-04-18 `Remove firmware compression`.
- `adfc907` 2024-03-13 `Updates for 14.4` (parent of removal).
- `4523553` 2024-03-13 `Remove the mostly broken VCN code`.
- `34bb3dd` 2024-08-23 `refactor: Store check results and clean up code` (`ObjectField` era).
- `5524cf0` 2025-11-15 `feat: Remove dependency on AMDRadeonX6000 and other improvements.`
- `e000f25` 2026-08-11 `Consolidate project structure` (`#embed`, `iVega/`).
- `f7b3e38` 2026-05-28 `Update firmware to AMD Adrenaline 26.5.2`.
- `c21640d`, `1e43a4e` 2026-10 (branch) `DeviceProfile` + Phase 1 `VDA` skeleton.

### 10.2 Files cited (line numbers are from the stated commit)

- `NootedRed/DYLDPatches.cpp@4523553^` (132 lines): gate :14-19, banner :15-17, `hwgva-id` :29-33,
  route :36, VT DRM :61-62, GVA spoofs :67-68, Ventura VA patches :72-84, pre-Ventura :88-97,
  AddrLib :102-103, VCN1 gate :107, VCN1 patches :109-131.
- `NootedRed/DYLDPatches.hpp@4523553^` (307 lines): `DYLDPatch` :8-55, `DYLDPatches` :57-70,
  VideoToolbox DRM literal :73, `kHwGvaId` :75, GVA literals :78-79, CoreLSKD :81-85,
  HEVC encoder :88-89, VA patches :97-194, Vcn2Dec :200-233, Vcn2Enc :239-307.
- `NootedRed/X5000.cpp@4523553^` (437 lines): routes :37-38, :59-60, :75-76; `allocateHWEngines`
  :169-186; capability tables :188-236; `setHWCapability` :238-328; `wrapSetup...` :330-338
  (`HasVCN0=true` :336); `wrapGetHWChannel` :356-358; bridging forwarders :369-386.
- `NootedRed/X5000.hpp@4523553^`, `NootedRed/X6000.cpp@4523553^` (:27-30, :56-66, :201-209, :282-288),
  `NootedRed/X6000.hpp@4523553^` (:17-39).
- `NootedRed/NRed.cpp@4523553^`: DYLD hook-up :146, `safeMetaCast` route :141-146, wrapper :168-180.
- `NootedRed/NRed.hpp@4523553^`: `ChipType` :13-20, `getChipName` :78-82, `metaClassMap` :220.
- `NootedRed/HWLibs.cpp@4523553^`: firmware :267-282, `getIpFw` :284-293.
- `NootedRed/Firmware/Drivers.xml@4523553^`: X6000 personality, X5000 personality with `IODVDBundleName`/
  `IOGVACodec = AMDVCN2`/`IOGVAHEVC*` (~:85-160), `SWIP_properties` (~:176-186, ~:308-318).
- `NootedRed/kern_hwlibs.cpp@1444841`: :137-158 firmware, :188-200 `getIpFw`.
- `NootedRed/kern_x5000.cpp@1444841`: :143-160 engine allocation, :193-217 capability setup.
- `NootedRed/kern_x6000.cpp@1444841`: :27-65 solve/route, :145-157 X6000 used only as symbol donor.
- Current tree `@1e43a4e`: `NootedRed/HWLibs.cpp:55-59, 764, 884-926`; `NootedRed/X5000.cpp:96, 113,
  135, 149, 322-330, 351-365`; `NootedRed/X5000.hpp:42-53`; `NootedRed/DeviceProfile.hpp:55-90`;
  `NootedRed/DeviceProfile.cpp:41-65, 102-107`; `NootedRed/VDA.hpp:25-52`; `NootedRed/VDA.cpp:34-80`;
  `NootedRed/UserPatches.cpp:1002, 1011, 1135`; `NootedRed/UserPatches.hpp:30-44`;
  `NootedRed/Personalities/com.apple.kext.AMDRadeonX5000.xml` (:15-17, :26-57, :93-101, :149-151,
  :227-229, :253-255, :331-339); `NootedRed/GPUDriversAMD/PSP.hpp:48, 62-63, 76`;
  `NootedRed/Kexts.cpp:26-48`; `NootedRed/DriverInjector.cpp:50-53`.
- `docs/VDA-Implementation-Plan.md`, `docs/Device-Profile.md`, `analysis/lilu-userpatch.md` (current
  state and user-patch mechanism).

### 10.3 External references (GitHub; fetched via `curl`/web, treated as untrusted data)

- PR #35 "Fix VCN (at least partially)" — <https://github.com/ChefKissInc/NootedRed/pull/35>, merged
  2023-04-14 as `2eb093a`; body: "Helps with #28, but only slightly… There's proper hardware acceleration
  for decoding now". Patch series: `pull/35.patch`.
- Issue #28 "Image & video hardware decoding/encoding (hardware DRM, VCN) support" —
  <https://github.com/ChefKissInc/NootedRed/issues/28> (open, 2023-03-17 -> 2026-08-13, 26 comments;
  2023-05-05 comment: "the DRM-unlocking code is just UnfairGVA from WhateverGreen"; 2025-11-16
  maintainer comment on the user-space patching being the blocker).
- Issue #120 "Latest version of NootedRed unable to enter login screen on macOS 12.5" —
  <https://github.com/ChefKissInc/NootedRed/issues/120> (fixed by `fb6df04` "Fix VCN patches").
- Issue #137 "Heavy compute activities hang the system" — <https://github.com/ChefKissInc/NootedRed/issues/137>
  (mentions VdA decoding problems, 2023-07).
- Issue #153 "Hardware Acceleration Problem" — `VDADecoderChecker` error `-12473`, Ventura, 5600G.
- Issue #163 "H.265 can be hardware decoded successfully， but H.264 cannot be hardware decoded" —
  Renoir 5800H, macOS 13.5.1, `-nredvcn`; closed as duplicate of #28.
- Issue #208 "picasso reset hang"; issue #235 "Sonoma+ compute tasks may hang" (context for
  power/GFXOFF risk).
- Issue #249 "Adding Hardware Decode to AMD Ryzen 5 5600G's iGPU" (2024-04-09): "NootedRed does not
  support hardware decode" — post-removal status.
- Issue #442 "Fully remove conflict with RDNA dGPUs" — <https://github.com/ChefKissInc/NootedRed/issues/442>
  (maintainer, 2026-08-10): X6000 was mangled "historically for the removed buggy VCN code", since
  replaced by a proper reimplementation.
- WhateverGreen `WhateverGreen/kern_unfair.cpp` (Acidanthera) — provenance of the UnfairGVA patches
  (`UnfairAllowHardwareDrmStreamDecoderOnOldCpuid`, `UnfairRelaxHdcpRequirements`,
  `UnfairCustomAppleGvaBoardId`), fetched from <https://github.com/acidanthera/WhateverGreen>.

### 10.4 Read-only local observations (macOS 15.8.1, 24H32, test machine)

- `sw_vers` = macOS 15.8.1 / build 24H32.
- `/System/Library/Extensions/AMDRadeonX5000.kext`, `.../AMDRadeonX5000HWServices.kext`,
  `.../AMDRadeonX6000.kext`, `.../AMDRadeonVADriver2.bundle` all exist.
- `AMDRadeonX6000.kext/Contents/` contains `Info.plist`, `_CodeSignature`, `version.plist` but **no
  `MacOS/` binary**; `AMDRadeonVADriver2.bundle/Contents/MacOS/` is empty — binaries are in the
  kernelcache/dyld shared cache. `AMDRadeonX6000` Info.plist declares only `AMDNavi10/12/14/21/23`
  accelerator classes (no Renoir/Vega class), confirming Renoir is driven by the injected X5000
  personality.
- No system file was modified; these checks were `ls`/`PlistBuddy`/`grep` only.
