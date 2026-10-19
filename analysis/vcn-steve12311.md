# steve12311/NootedRed — limited VCN hardware decode: factual analysis

Status: static research (task-2). Fork: <https://github.com/steve12311/NootedRed>, branch `master`,
target revision `1983f326ebedda9e5596ee6e8ba47af52420ec11` ("VCN: Fix hardware encode feedback and
source pitch", 2026-10-07). Cloned to `/tmp/nr-fork-steve` with full history. No kext was installed or
loaded, no GPU / VideoToolbox / render test was run, and no system file was modified. In addition to
reading the fork, I did read-only parsing of this machine's kernel collection and dyld shared cache with
throw-away scripts under `/tmp` to make the macOS 15.8.1 section concrete (see §7.2).

Evidence labels used throughout:

- **[F] fact** — visible in fork code/data, commit message, or repository metadata.
- **[C] claim** — author's documentation / commit-message assertion, not independently reproduced here.
- **[O] observation** — produced by this analysis (read-only, on this machine); method noted.
- **[I] inference** — reasoned from code but not proven by a test or hardware run.
- **UNKNOWN** — no reliable evidence.

---

## 1. Overview

### 1.1 What the fork is

The fork is a derivative of ChefKissInc/NootedRed. Its master is 592 commits: upstream history through
`8e93e82` ("HWLibs: Fix SMU 12 reimplementation on Catalina", 2026-09-29), then fork-only work
`be1545a` ("X5000: Fix Cezanne accelerated rendering"), `08126d3` (surface-sync / Tahoe cache support),
and the VCN series analysed here. The VCN series is exactly five commits:

| commit | date | subject |
| --- | --- | --- |
| `02d3789` | 2026-10-03 | VCN: Add experimental Cezanne hardware decoding |
| `ae8f6bc` | 2026-10-04 | VCN: Generalize VCN2 APU capability matching |
| `6c7b080` | 2026-10-04 | VCN: Match Tahoe with native kernel version constants |
| `338fff9` | 2026-10-05 | UserPatches: Share dynamic cache resolution |
| `1983f32` | 2026-10-07 | VCN: Fix hardware encode feedback and source pitch |

The fork uses the NootedRed Xcode project (`NootedRed.xcodeproj`) and TSNPL 1.5; it predates the
AppleForge/xmake switch, so it is not based on the current upstream `master` tree of our checkout. [F]

### 1.2 What it implements

The VCN feature is a **bridge**, not a VCN driver. It reuses Apple's native
`AMDRadeonX6000.kext` VCN2 engine, video-context, channel, SML and AddrLib code inside an
`AMDRadeonX5000` accelerator that NootedRed already drives, plus user-space byte patches to
`AppleGVA` and `AMDRadeonVADriver2` so VideoToolbox selects the AMD VCN2 decode path instead of
falling back to "unsupported". No new firmware loader, ring allocator or command submission code is
added. [F: `VCN.cpp`, `UserVideoDecode.cpp`; C: docs/vcn.md:30-47]

Headline: a limited, opt-in (`-NRedVCN`) VCN 2.2 decode path for PCI `0x15E7/0x1636/0x1638/0x164C`,
hard-gated to macOS 26 / Darwin 25.x, with VCN1 (Raven/Picasso) explicitly not bridged. The final
commit also fixes hardware **encode** feedback/pitch, so the branch is not decode-only even though
`docs/vcn.md` is. [F]

---

## 2. Gates and capabilities

### 2.1 Device / generation table

`NootedRed/VCNCapabilities.hpp:9-44` [F]:

| PCI ID | generation | `vaFamily` | accepted? | NRed/doc family label |
| --- | --- | --- | --- | --- |
| `0x15D8` | VCN1 (`0x0100`) | 0 | no, reason 5 | Picasso / Raven2 |
| `0x15DD` | VCN1 (`0x0100`) | 0 | no, reason 5 | Raven |
| `0x15E7` | VCN22 (`0x0202`) | 5 | yes, subject to gates | Barcelo / Cezanne family (docs) |
| `0x1636` | VCN22 (`0x0202`) | 5 | yes | Renoir |
| `0x1638` | VCN22 (`0x0202`) | 5 | yes | "Renoir / Green Sardine" in NRed; "Cezanne" in fork docs |
| `0x164C` | VCN22 (`0x0202`) | 5 | yes | Renoir/Cezanne family (docs) |

- The table must match NRed's existing `NRed.cpp` `switch (deviceID)` device set; the generator
  `tools/vcn_capabilities.py:8-21` refuses duplicates, omissions or an unverified `vaFamily`, and
  `tools/test_vcn_candidate.py` asserts `[(0x15E7,5),(0x1636,5),(0x1638,5),(0x164C,5)]`. [F]
- `vaFamily` is the VA driver's decode capability class, not the GPU GFX/AddrLib family
  (`VCNCapabilities.hpp:21-22`). Family 5 is the class natively used by Navi10 `0x731f`; the native VA
  identify function returns enum 11 ("unsupported") for Renoir/Cezanne-family IDs, so the user-space
  patch remaps only the running PCI ID to 5. [F: `tools/test_video_driver_identify.py` CPU fixture;
  I: what the enum means to Apple GVA]
- **VCN1 is not implemented**: the code comment says VCN1 cannot reuse the current VCN2 bridge
  (`VCNCapabilities.hpp:1`), the docs table says "拒绝，尚未实现 VCN1 桥接" (rejected, VCN1 bridge not
  implemented, `docs/vcn.md:12`), and no VCN1 engine class/paths exist in the fork. The choice is
  consistent with the bridge depending on `AMDRadeonX6000_AMDVCN2HWEngine`; Raven/Picasso would need a
  different engine/queue path (the legacy NootedRed VCN effort removed in upstream `4523553` is outside
  this task's scope). [F/I]

### 2.2 OS gate and rejection codes

`VCNCapabilities::rejection()` returns 0 only when all of these hold
(`VCNCapabilities.hpp:36-44`, `VCN.cpp:44-47`):

1. device present in the table (else 3);
2. generation is `VCN22` and `vaFamily != 0` (else 5);
3. `currentKernelVersion().majorMatches(MACOS_26)`, i.e. Darwin major 25 / macOS 26.x (else 4).

`02d3789` initially required exactly `major()==25 && minor()==6` and only `0x1638`; `6c7b080`
generalised to any Darwin 25.x via `KernelVersion`/`MACOS_26`, and `ae8f6bc` generalised the device
set. [F: commit diffs/messages; docs/vcn.md:19-28]

Device-independent user-space and kernel gates:

- boot argument `-NRedVCN` is required; default is off (`VCN.cpp:247-251`, `UserVideoDecode.cpp:387-389`);
- the X5000 accelerator start checks the real provider's PCI vendor `0x1002` and device ID against
  NRed's detected iGPU (a different GPU cannot trigger the bridge even if listed);
- optional native load of `com.apple.kext.AMDRadeonX6000` must succeed, both drivers' hooks must be
  prepared, and (for the user-space half) `VCN::allocated()` must be true;
- both hardware vtables and the six guarded getter/method slots must verify before any kernel patch is
  installed.

Rejection reason meanings: 3 unknown device, 4 non-Tahoe, 5 unimplemented video generation; user-space
adds 1 = missing kernel symbols, 2 = hook failure. These are published as `NRedVCNRejected` /
`NRedVCNUserRejected`. [F: docs/vcn.md:103-104]

### 2.3 Package dependencies and boot args

- The Info.plist is unchanged: `OSBundleLibraries` still contains only Lilu, IOPCIFamily and the KPI
  libraries (`NootedRed/Info.plist:43-61`); `AMDRadeonX6000` is **not** a load-time dependency. [F]
- Instead `VCN.cpp:18-20` declares a local `KernelPatcher::KextInfo` for
  `com.apple.kext.AMDRadeonX6000` with the on-disk path and `KextInfo::Unloaded`, registered with
  `lilu.onKextLoadForce(&x6000)` only when `-NRedVCN` is present (`VCN.cpp:247-251`). The load is then
  requested from inside the X5000 accelerator-start hook with
  `OSKextLoadKextWithIdentifier("com.apple.kext.AMDRadeonX6000")` (`VCN.cpp:58-67`). [F]
- `tools/test_vcn_package_dependencies.py:10-17` compares a built candidate's `OSBundleLibraries`
  against the baseline and fails if the optional driver became a hard dependency. [F]
- The docs promise the plugin-library dependency set is unchanged and the feature is default-off
  (`docs/vcn.md:27-28,32-36`). [F]

The optional-load ordering matters: the X6000 hooks must be installed before the X5000 start path
allocates engines; the code assumes the `OSKextLoadKextWithIdentifier` call completes synchronously
inside the start hook and marks readiness only if `has5000 && has6000 && hooks` are all true. This is
code intent, not independently demonstrated. [F/I]

### 2.4 Diagnostics relevant to gates

`NRedVCNRequested`, `NRedVCNCapabilityVersion=2`, `NRedVCNDeviceID`, `NRedVCNGeneration`
(0x0202 VCN 2.2; 0x0100 only the VCN1 family), `NRedVCNRejected`, `NRedVCNUserRejected`,
`NRedVCNKernelABIRejected`, `NRedVCNX5000ABIValidated`, `NRedVCNX6000ABIValidated`,
`NRedVCNNativeLoadStatus/Ready`, `NRedVCNEngineAllocated`, `NRedVCNLastHardwareInit`,
`NRedVCNUserApplied`, `NRedVCNDecoderPID`, plus the dynamic-cache keys documented in
`docs/vcn.md:98-114`. Diagnostics are published on the iGPU. [F]

---

## 3. Kernel architecture

All kernel VCN code is in `NootedRed/VCN.cpp` (339 lines) and `VCN.hpp`, `VCNCapabilities.hpp`,
`VCNKernelData.hpp`; the only changes to existing kernel modules are in `NRed.cpp` (include + three
calls) and `X5000.cpp` (engine allocation + `hasVCN0`), plus generated data. `Kexts.*`, `X5000.hpp` and
`Info.plist` are untouched by all five VCN commits. [F: `git diff --stat 8e93e82..1983f32 -- ...`]

### 3.1 Wiring (`NRed.cpp`)

- `NRed::init()` calls `VCN::init()` after `Backlight::init()` (`NRed.cpp:54-55`). [F]
- `onPatcherLoadForce` calls `UserVideoDecode::init(patcher)` after the process patcher and surface-sync
  init (`NRed.cpp:68`). [F]
- The global `onKextLoadForce(nullptr, 0, ...)` callback calls `VCN::processKext(...)` before the
  normal `X5000::processKext(...)` (`NRed.cpp:84-85`). [F]

### 3.2 Optional X6000 load and accelerator start

`acceleratorStart()` (`VCN.cpp:49-72`) wraps
`__ZN37AMDRadeonX5000_AMDGraphicsAccelerator5startEP9IOService` and:

1. if not supported / provider is not the expected AMD GPU, calls the original;
2. one-shot CAS on `loadAttempted`, then `OSKextLoadKextWithIdentifier` for X6000;
3. records `NRedVCNNativeLoadStatus/Ready`, calls the original accelerator start, records
   `NRedVCNAcceleratorStarted`;
4. if start failed before an engine was allocated, clears `loadCompleted`, so the bridge cannot be used
   later against a half-initialised accelerator.

This avoids an OSBundleLibraries dependency and is meant to be a fully synchronous "load now, bridge
now, otherwise stay native" decision. [F/I]

### 3.3 Engine allocation into the X5000 PM4 engine array

`X5000::allocateHWEngines()` is already a NootedRed hook (`X5000.cpp:179`). The fork adds
(`X5000.cpp:310-323`):

```cpp
if (VCN::ready()) {
    auto* engine = VCN::allocateEngine();
    (singleton().pm4EngineField + kAMDHWEngineTypeVCN0 * sizeof(void*))(self) = engine;
    if (!engine) { singleton().hasVCN0Field(self) = false; }
}
```

- `kAMDHWEngineTypeVCN0` is enum value 8 in NootedRed's reimplementation
  (`GPUDriversAMD/Accel/HWEngine.hpp:8-22`); on macOS 12+/15.8.1 `pm4EngineField` is `0x3B8`
  (`X5000.cpp:123`), so the slot is `0x3B8 + 0x40 = 0x3F8`. The first version hard-coded `0x3F8`
  (`02d3789`); `6c7b080` replaced it with the field+enum expression. [F]
- `VCN::allocateEngine()` allocates from the X6000 `AMDVCN2HWEngine` metaclass
  (`__ZN30AMDRadeonX6000_AMDVCN2HWEngine10gMetaClassE`, `VCN.cpp:257-268,326`), records
  `NRedVCNEngineAllocated`, and disables the bridge if allocation fails. [F]
- `X5000::wrapSetupAndInitializeHWCapabilities()` then sets `hasVCN0Field = VCN::ready()` and leaves
  `hasUVD0`/`hasVCE` false (`X5000.cpp:344-350`). `hasVCN0` is the native capability flag the AMD
  accelerator uses to expose VCN channels; setting it is the fork's way to make the existing X5000
  native channel setup drive the inserted X6000 engine. [F: code; I: exact native consumers]
- The fork adds **no** ring, doorbell, firmware or command-submission code. Native X6000
  `AMDVCN2HWEngine::initHardware`/`allocateHWRings`/`allocateHWChannels` are reused; `initHardware` is
  routed only to observe/log the result (`VCN.cpp:179-186`). [F]

### 3.4 Cross-driver metaclass bridging

X5000 and X6000 have distinct `OSMetaClass` instances for the same logical classes. The fork resolves
the four class pairs and hooks the kernel's `OSMetaClassBase::safeMetaCast`
(`VCN.cpp:86-101,288-323,326-332`):

- class names: `AMDGraphicsAccelerator`, `AMDAccelVideoContext`, `AMDAccelChannel`, `IAMDHWChannel`
  with mangled-name length prefixes 37/35/30/28 (`VCN.cpp:39-41,288-294`);
- pairs are resolved as `__ZN...AMDRadeonX5000_<class>10gMetaClassE` vs the X6000 equivalent;
- on a failed normal `safeMetaCast`, the hook retries the alternative class from the pair, accepting
  the result only when `getClassSize() >= target->getClassSize()` (so an X6000 object can satisfy an
  X5000 cast or vice versa only if the layout is at least as large);
- the hook is installed on the kernel itself with `routeMultipleLong(KernelPatcher::KernelID, ...)`
  (`VCN.cpp:318-321`).

This lets an accelerator/hardware object of one driver be passed to the other driver's
`newVideoContext` / channel / client code. It is a structural cast bridge, not a vtable bridge. [F/I]

### 3.5 Per-method hooks (`VCN.cpp`)

| hook | target | behaviour when `VCN::allocated()` |
| --- | --- | --- |
| `newShared` | X5000 `AMDGraphicsAccelerator::newShared` | allocate X6000 `AMDAccelShared` metaclass |
| `newClient` | X5000 `AMDGraphicsAccelerator::newSharedUserClient` | allocate X6000 `AMDAccelSharedUserClient` metaclass |
| `newVideo` | X5000 `AMDGFX9GraphicsAccelerator::newVideoContext` | call X6000 `AMDGFX10GraphicsAccelerator::newVideoContext(self)` |
| `clientStart`/`clientStop` | X6000 `AMDAccelSharedUserClient::start/stop` | call the X5000 start/stop implementation |
| `surfaceCopy` | X6000 `AMDAccelShared::SurfaceCopy` | call the X5000 implementation |
| `createSML` | X5000 `IAMDSMLInterface::createSMLInterface` | for non-GFX9 configs, call X6000 `createSMLInterface(config \| 1)`; GFX9-only configs keep X5000 native |
| `signalWork` | X6000 `AMDHardware::signalGPUWorkSubmitted` | call X5000 `AMDGFX9Hardware::notifyGfxAccess(hardware)`; else original |
| `initHardware` | X6000 `AMDVCN2HWEngine::initHardware` | just record `NRedVCNLastHardwareInit` and return native result |

Source: `VCN.cpp:133-186,224-231,296-311`; class symbol names at `VCN.cpp:296-333`. [F]

The `signalWork` hook is the GFXOFF/work-submission pairing: video work submitted by the X6000 code
must go through the X5000 hardware's GFX-access notification so GFXOFF/clocks are handled by the
driver that actually owns the Renoir hardware; otherwise the X6000 hardware object would not have the
X5000 state. The docs describe this as "包含 GFXOFF 等待配对" (`docs/vcn.md:36`). [F/I]

### 3.6 Vtable/getter ABI verification

Before installing anything, `verifyKernelABI()` (`VCN.cpp:103-131`) reads the hardware vtable symbol
(`__ZTV32AMDRadeonX5000_AMDVega10Hardware` / `__ZTV32AMDRadeonX6000_AMDNavi10Hardware`), skips the
Itanium ABI offset-to-top/RTTI header (`2 * sizeof(void*)`), and checks six slots at
`tableAddressPoint + slot`:

| X5000 slot | guard | X6000 slot | guard |
| --- | --- | --- | --- |
| `0x298` | named `AMDHardware::isDeviceValid` identity | `0x2A0` | same for X6000 |
| `0x2A8` | anonymous getter bytes `55 48 89 E5 8A 87 0E 03 00 00 24 01 5D C3` (get byte at `+0x30E`, `and 1`) | `0x2B0` | same getter shape for X6000 |
| `0x3B8` | getter bytes `55 48 89 E5 48 8B 87 40 06 02 00 5D C3` (load qword `+0x20640`) | `0x3B0` | `... 48 06 02 00 ...` (`+0x20648`) |
| `0x3C8` | getter bytes `55 48 89 E5 8B 87 50 05 02 00 5D C3` (load dword `+0x20550`) | `0x3C0` | `... 48 05 02 00 ...` (`+0x20548`) |
| `0x3E8` | named `AMDRTHardware::disableGfxOff` identity | `0x3E0` | same for X6000 |
| `0x3F0` | named `AMDRTHardware::enableGfxOff` identity | `0x3E8` | same for X6000 |

`VCNKernelData.hpp:28-64` holds the tables, getter byte arrays and guards; `VCN.cpp:112-130` checks
bounds (`slide`/`size`), target-in-image, symbol identity, and `memcmp` of the complete getter bytes.
Any mismatch sets `NRedVCNKernelABIRejected=1` and prevents any route/patch on that side. [F]

The guards exist because the bridge calls X5000 hardware methods through X6000 code paths that expect
X6000 vtable numbering; the guard proves the slots that will be used are the intended methods, not just
"some pointer at an offset". The docs explicitly say the checks cannot auto-migrate arbitrary new
driver layouts (`docs/vcn.md:20-23`). [F]

### 3.7 VCNKernelData patch list (cross-driver vtable adapter)

`VCNKernelData.hpp:6-27` contains 20 six-byte call-site patches, generated from the local X6000
binary by `tools/build_vcn_kernel_patch.py`. Each entry is
`{symbol, offset, original[6], newSlot}` and each original is a
`callq *imm32(%rax)` (`FF 90 <slot32>`); `install()` verifies all 20 originals before writing anything
(`VCN.cpp:188-243`). Replacement rule: if `newSlot != 0`, overwrite the slot immediate with the X5000
slot number; if `newSlot == 0`, replace the six bytes with `E8 <rel32>` to the resolved
`AMDHardware::signalGPUWorkSubmitted` plus a NOP (`VCN.cpp:209-223`). [F]

Patch categories and why they exist:

- **X6000 interface/hardware calls must use X5000 vtable numbering.** X6000 code was compiled against
  X6000 `AMDHardware`/interface vtable slots; at runtime the object is the X5000 accelerator's hardware
  object. The generator rewrites the call-site immediate:
  `0x2A0→0x298` (device validity/isIdle class of calls; `AMDVCN2HWEngine::isIdle`, `waitForHwStamp`,
  `enableTimestampInterrupt`), `0x2B0→0x2A8` (`AMDNavi10VideoContext::setSuspendResumeState`),
  `0x3B0→0x3B8` (channel init/timestamp/reset/diagnosis calls), `0x3C0→0x3C8` (channel init/diagnosis
  calls). [F: `VCNKernelData.hpp:6-27`]
- **GFXOFF wait pairing.** `AMDHWChannel::waitForIdle` and `AMDHWEngine::waitForIdle` call X6000
  hardware slots `0x3E0` (disableGfxOff) and `0x3E8` (enableGfxOff) around the wait; X5000's
  corresponding methods are at `0x3E8`/`0x3F0`, so the generator rewrites those to the X5000 slots.
  The generator demands exactly four such wait/idle patches (two channel + two engine) and checks that
  the call receiver was loaded from the channel/engine hardware field (`offset 0x20`/`0x18`) rather
  than from an unrelated object (`tools/build_vcn_kernel_patch.py:86-100,104-105`). [F]
- **Work submission signalling.** `AMDHWChannel::submitCommandBuffer` calls slot `0x230`; the fork
  turns that into a direct call to X6000 `AMDHardware::signalGPUWorkSubmitted`, which is itself routed
  to `signalWork` so the X5000 `notifyGfxAccess` runs (newSlot 0 entry, `VCNKernelData.hpp:17`,
  `VCN.cpp:216-220`). [F/I]

The generator's evidence requirements are strict: it only accepts call sites in named X6000
channel/engine/video-context functions, verifies the current bytes are the expected `ff 90` call, and
requires a preceding `movq (%rdi), %rax` (hardware vtable load) plus, for idle calls, a receiver loaded
from the hardware field (`build_vcn_kernel_patch.py:73-105`). This "same offset on a different object"
check is exactly the class of bug the patch list is designed not to introduce. [F]

### 3.8 Firmware and PSP

The VCN commits add no firmware/PSP code. The VCN firmware is supplied by NootedRed's pre-existing
`HWLibs.cpp`: it embeds `Firmware/ativvaxy_nv.dat` for `ipVersion 0x0202` and `ativvaxy_rv.dat` for
`0x0100`, places them in the AMD firmware directory under the Navi10 device type, and hooks
`getIpFw`/`createFirmware` to return them (`HWLibs.cpp:875-907`; comment there calls VCN 2.1, but the
ipVersion argument is `0x0202`). The bridge therefore depends on Apple's native X6000 VCN2 engine
loading the firmware through that supplied firmware directory; if that path fails, the engine's
`initHardware` returns false and `NRedVCNLastHardwareInit` records it, but the fork has no separate
firmware fallback of its own. [F: code; I: exact call chain from native engine to HWLibs]

### 3.9 Error/fallback paths

- ABI mismatch → `has5000`/`has6000`/`hooks` cleared, `loadCompleted=false`, no bridge; diagnostics only.
- Native X6000 load fails or is late → `loadCompleted=false`; no mid-life engine insertion.
- Engine allocation fails → `hasVCN0=false`, X5000 continues native.
- Patch-site bytes differ → `install()` returns before writing; no partial kernel code patch.
- `routeMultiple` failure after byte patching → the original six-byte sequences are restored
  (`VCN.cpp:232-241`); hooks are marked failed.
- Accelerator start failure without engine → load state cleared.
- Kernel/user-space diagnostics are capped (log counts, stage bits) to avoid unbounded logging.

A successful `routeMultiple` does not itself validate the behaviour; the docs stress that diagnostic
keys only prove hooks/registrations, not hardware decode (`docs/vcn.md:105-114`). [F/C]

---

## 4. User-space architecture

### 4.1 Why `_shared_region_check_np` and COW pages

`AppleGVA` and `AMDRadeonVADriver2` are dyld-shared-cache images, not vnode-backed kexts, so
NootedRed's existing `_cs_validate_page` user-patch path (and Lilu's user patcher) does not reach them.
The fork instead hooks the kernel function `_shared_region_check_np` with
`routeMultipleLong(KernelPatcher::KernelID, ...)` (`UserVideoDecode.cpp:408-428`) and, after calling
the original successfully, reads a user-space variable through `args->startAddress` to obtain the
process's cache base, reads 104 bytes at that base, checks the 16-byte header magic
(`dyld_v1 x86_64h`), and reads the 128-bit cache UUID at header offset 88. It then patches the
process-private COW copies of the AppleGVA/VADriver2 code pages.
[F: `UserVideoDecode.cpp:219-259,387-429`]

The hook only acts when `-NRedVCN` was accepted **and** `VCN::allocated()` is true, i.e. the kernel
bridge actually owns the engine; otherwise it is a pure pass-through. The original return value and
out-parameters are preserved (`UserVideoDecode.cpp:219-230`). [F]

### 4.2 Static fast path vs dynamic cache resolution

- `UserVideoDecodeData.hpp:6-13` contains the generated `CacheMagic`, reference `CacheUUID`
  (`31374756-89B4-34E8-A34F-56C5D57216A1`), `CacheBase = 0x7FF800000000`, and the patch table.
- If the actual cache UUID equals the reference UUID, the wrapper uses the static `Patches[]` offsets
  directly (offset relative to cache base). Otherwise it calls
  `UserVideoDecodeResolver::resolve(readUser, base, devicePatches, plan)` to build a dynamic plan
  (`UserVideoDecode.cpp:259-274`; `UserVideoDecodeResolver.hpp:8-27`). [F]
- The dynamic resolver (`UserCacheResolver.hpp:237-336`, supported by `UserSharedCache.hpp:121-260`):
  - parses the dyld cache header, mapping table and image table, then each image's Mach-O load
    commands to find `__text`/`__const` and segments;
  - finds the complete reference function bytes in the image text (32-byte prefix scan, full-byte
    compare, skipping 4-byte relative-reference fields), requiring a unique match;
  - re-resolves rel32 calls/RIP displacements and GOT/call-stub bindings against the current cache;
  - rebuilds both the original and patched payloads;
  - rejects duplicate matches, metadata overruns, read failures, binding mismatches, rel32 overflow,
    incomplete plans. Limits: 32,768 reads / 32 MiB per resolution, 8 MiB text per modified image,
    128 candidates (`UserSharedCache.hpp:26-49`; docs/user-cache-dynamic.md:29-31).
- The docs are explicit that this only supports **same implementations laid out differently**; it
  "cannot automatically adapt arbitrary new function implementations, external symbol RVAs or driver
  ABI" (`docs/user-cache-dynamic.md:14-22`; `docs/vcn.md:25-26`). UUID is not used as a compatibility
  whitelist for the dynamic path. [C]

### 4.3 What is patched

`UserVideoDecodeData.hpp:222-245` has 22 patch records; `DevicePatches[]` at `:409-417` selects one of
four per-device tables (the `0x1638` table is the base `Patches`; the other three substitute a
device-specific identify payload). The categories (from `tools/build_video_decode_patch.py` and the
generated data):

1. **AppleGVA decoder-selection fallback** (`AppleGVA_AMDOnlyFallback`), one 6-byte branch at
   reference `+0x287` of the selection function: the native fallback for non-special AMD machines only
   selects Intel; the patch redirects it to the existing AMD insertion path. Mixed-GPU, known-machine,
   explicit-device-request and no-GPU paths are left as they were. [F: build script lines 143-150;
   C: docs/vcn.md:46-47]
2. **VA identify full-function replacement** (`VAAcceleratorInfo::identify`, 448 bytes): rewrites the
   function so the running PCI ID returns video family 5 (VCN2 decode capability) while all other PCI
   classifications and IOKit error/type paths keep native results. Per-device variants are generated
   for `0x15E7/0x1636/0x1638/0x164C`; the `0x1638` variant is the base table. [F: build script
   lines 69-104,152-155; CPU test in `tools/test_vcn_candidate.py`]
3. **VAFactory engine selection**: `createGraphicsEngine` (+10) and `createImageBlt` (+11) have the
   10-byte `mov 0x460(%rsi),%rax; mov 0xc(%rax),%eax` replaced with `mov $2,%eax; nops`, forcing the
   native VCN2 engine type instead of the value derived from the wrong (Navi/other) hardware info.
   [F: build script lines 156-160]
4. **VPE factory selection**: each of the VPE factory functions that contains the GFX9 branch
   `8d 46 ff 83 f8 02 72` gets an 8-byte replacement forcing the GFX9/`%esi=2` path; functions without
   that branch are left native (`build_video_decode_patch.py:161-171`). The generated data shows three
   such functions; the docs call this "GFX9 factories". [F/I]
5. **AddrLib family/AddresLib fixes**:
   - `VAAddrLibInterface::init` one byte at +0x8E: `0x8D` (AI) → `0x8E` (RV) next to
     `cmp $0x8d,%r12d` (`build_video_decode_patch.py:172-175`);
   - `Addr::Lib::Create` one byte at +0xB6: `cmp $0x8d` → `cmp $0x8e` (`:176-179`);
   - full replacement of `Addr2::V2::Gfx9Lib::HwlConvertChipFamily` with `tools/video_addr_convert.s`
     (160 bytes), which preserves the native 0x8D/AI configuration and adds the 0x8E/RV chip-settings
     configuration, returning 8 (`video_addr_convert.s:1-41`).
   This mirrors NootedRed's existing X5000 `Addr::Lib::Create` patch (0x8D→0x8E) for the same
   Renoir/Navi-labelling problem; the VA driver's family branch has to be taught the same thing. [F;
   I: semantic intent]
6. **Swizzle-mode masks**: eight u32 edits, four occurrences each of `0x02220221→0x02020201` and
   `0x06660661→0x06060601`, inside four verified AddrLib consumer functions
   (`build_video_decode_patch.py:204-218`). The docs call this "swizzle 修正"
   (`docs/vcn.md:42`); the exact consumer semantics are not documented in the fork. [F: bytes; I:
   purpose]
7. **Hardware-encode fixes** (added by `1983f32`, not decode):
   - `Vcn2EncCommand::getFeedbackSize`: `mov $0xA4` (164) → `mov $0x30` (48), matching the firmware's
     48-byte feedback stride;
   - `AVDVcn2AvcEncDataProcessor::ProcessEncodeStatus` and the HEVC counterpart: `imul $0xA4` →
     `imul $0x30` (two sites);
   - `Vcn2Encoder::setEncodeParams`: replaces a 37-byte block that reconstructs source pitch from the
     encoded width with one that reads the source texture pitch fields (`+0x80`, `+0x84`) and keeps
     the native element/chroma conversions.
   [F: build script lines 183-203; commit `1983f32` message]

### 4.4 COW transaction and rollback

For both static and dynamic plans (`UserVideoDecode.cpp:276-382`):

1. read each patch site's current bytes; require either **all** native originals or **all** already
   patched (idempotent re-entry); any mix is rejected (`Stage::Mixed`, D08);
2. compute the set of touched pages (patch may cross a page); reject > `PatchCount * 2` pages;
3. for every page, `mach_vm_region_recurse` short info and require protection exactly
   `VM_PROT_READ | VM_PROT_EXECUTE` (RX) at the leaf (`leafProtection`, short-query only, depth loop);
4. only after all checks pass, `vm_protect(RW|COPY)` each page (private COW, removes execute), write
   with `copyout`, then restore the original protection (RX);
5. if a write, protect or restore fails, roll back all written bytes in reverse order, restore pages
   and report `Rollback`/`NRedVCNUserLastError`; never leave a partially patched image;
6. on success mark `Stage::Applied`, set `NRedVCNGVASelectionFix=1`, count `NRedVCNUserApplied`
   (capped at 16), and if the process name starts with `VTDecoderXPC`, set `NRedVCNDecoderPID`.

The write is not to the on-disk cache; the transaction only mutates the process-private COW pages.
[F: `UserVideoDecode.cpp:90-118,127-204,276-382`; C: docs/vcn.md:44-45]

### 4.5 Diagnostics and failure codes

`NRedVCNUserD00..D15` stage bits (Called, OriginalError, Argument, CacheRead, CacheMismatch, CodeRead,
CodeMismatch, Already, Mixed, NoMap, Region, Protection, Write, Restore, Rollback, Applied),
`NRedVCNUserLastError`, `NRedVCNUserCacheUUID0..3`, `NRedVCNUserDynamicMatched/Applied/Rejected/Reads`,
`NRedVCNUserEnabled/Rejected`, `NRedVCNUserApplied`, `NRedVCNDecoderPID`,
`NRedVCNGVASelectionFix`, `NRedVCNRegionQueryVersion`. The docs warn that `Enabled` only means a hook
was registered and cannot be used as proof of hardware decode (`docs/vcn.md:100-117`). [F/C]

---

## 5. Patch generation

### 5.1 Generator chain

All generated headers say "do not edit by hand": `VCNKernelData.hpp:1-2`,
`UserVideoDecodeData.hpp:1`. [F]

- **`tools/audit_video_driver.py`** — read-only audit of the host's X5000/X6000 (on-disk Mach-O) and
  the VADriver2 image in the x86_64h dyld cache: vtable dump, metaclass names, `VAAcceleratorInfo`
  code/hash. Writes `build/vcn/video-driver-audit.json`.
- **`tools/build_vcn_kernel_patch.py`** — parses the host's
  `/System/Library/Extensions/AMDRadeonX6000.kext/.../AMDRadeonX6000` with `llvm-objdump`, finds the
  accepted `callq *slot(%rax)` sites, verifies the hardware-vtable receiver and idle-call receiver,
  maps X6000 slots to X5000 slots, verifies the X5000/X6000 hardware vtable guards
  (`abi_guards()` lines 21-62, using `nm`, `llvm-objdump`, `disk_image`, `vtable`), and emits
  `VCNKernelData.hpp` plus `build/vcn/kernel-patches.json` (with the driver SHA-256).
- **`tools/build_video_decode_patch.py`** — requires the reference dyld cache through
  `tools/metal_cache.py`, whose `CacheReader` hard-asserts base `0x7FF800000000` and UUID
  `31374756-89B4-34E8-A34F-56C5D57216A1`; verifies the AppleGVA selection function at hard-coded
  address `0x7FFA0C59BBAC` (size 1136, SHA-256 `b7c7...`), verifies per-symbol SHA-256 for the
  VADriver2 functions (`HASHES`, lines 19-46), compiles the identify and AddrConvert payloads, applies
  the byte patches at verified sites, checks cross-page/no-overlap, and emits `UserVideoDecodeData.hpp`
  plus `build/vcn/user-patches.json`.
- **`tools/cache_resolver_data.py`** — generates the complete-function bytes, instruction/data split,
  and relative-reference descriptors used by the dynamic resolver.
- **`tools/vcn_capabilities.py`** — derives the VCN2 device list from `VCNCapabilities.hpp` and
  cross-checks it against `NRed.cpp`'s `switch`; rejects VCN1 or an unverified `vaFamily`.
- **`tools/video_addr_convert.s`** — hand-written 160-byte replacement for `HwlConvertChipFamily`.

The docs require running these on the "currently adapted macOS host" and say mismatched driver/cache
data must not be reused (`docs/vcn.md:51`). [F/C]

### 5.2 What is version/cache/driver specific

| generated input | tied to |
| --- | --- |
| `VCNKernelData::Patches[]` (symbols, offsets, 6-byte originals) | exact X6000 kext build |
| `Table5000/6000`, `Guards*`, `Getter*` | X5000 and X6000 hardware vtable layouts and getter instruction bytes |
| `UserVideoDecodeData::CacheMagic`, `CacheUUID`, `CacheBase` | exact dyld cache format/UUID/base |
| `Patches[]` static offsets | exact reference cache (and cache base) |
| `Identify15E7/1636/164C`, `DevicePatches[]` | device-specific identify payload, built from the reference identify function |
| `ResolverImages/Functions/Locations`, `ResolverOriginal*`, `ResolverRefs*` | reference image paths, function bytes, RVAs and relocations (dynamic matching baseline) |
| `MaxPatchSize` (448), `PageCount` (11) | reference patch set/layout |
| generator constants (`GVA_CHOICE_ADDRESS/SIZE/SHA256`, `HASHES`, `ENCODE_CONTEXTS`, `CacheReader` UUID/base) | reference AppleGVA/VADriver2 build and cache |

The capability table, rejection codes, COW transaction logic, kernel bridge structure and dynamic
resolver algorithm are version-independent code, but every byte table above must be regenerated for a
different macOS build. The fork's own docs say the same (`docs/vcn.md:51`). [F/C]

### 5.3 Test tooling and what it actually proves

The tools directory contains CPU-side regression harnesses. They are valuable but none of them loads a
kext or touches the GPU:

- `test_vcn_native_load.py` extracts the actual `supported()`, `acceleratorStart()` and `ready()`
  functions from `VCN.cpp` and compiles them against a fixture; checks load-before-start, delayed-load
  refusal, return-value preservation.
- `test_vcn_engine_bridge.py` compiles the actual engine allocation/SML/safe-cast/signal code and the
  actual `X5000::allocateHWEngines` slot write; checks four devices, failure states, class sizes.
- `test_vcn_kernel_abi.py` compiles the actual `verifyKernelABI()` and checks all slots/targets/getter
  bytes/bounds, optionally with ASan/UBSan.
- `test_vcn_user_bridge.py` and `test_video_cache_dynamic.py` compile the actual COW wrapper and
  resolver against synthetic caches; cover partial writes, protection failures, mixed/patched
  idempotency, cross-page rollback, UUID/ASLR/function relocation, GOT/stub rebinding, read budgets.
- `test_vcn_candidate.py` executes the generated identify payload and the native identify function on
  the CPU over the complete 16-bit PCI space and checks that only the target returns 5.
- `test_vcn_idle_calls.py` executes native `waitForIdle` code on the CPU with mocked IOKit/trace
  targets, to verify GFXOFF slot pairing and timeout paths.
- `test_vcn_gva_choice.py` executes the native/candidate AppleGVA selection function with mocked
  external calls; it does not create a decoder or touch the GPU.
- `test_vcn_package_dependencies.py` compares a built Info.plist's `OSBundleLibraries` to baseline.
- `run_video_encode_test.py` / `test_video_encode.m` is a real VideoToolbox encode probe, but hardware
  modes are only run with explicit `--hardware`; the default is software-only and each case has a
  timeout. [F: tool sources]

The fork's own docs state these are "CPU simulations, cannot replace hardware initialisation, video
bitstream and long-duration tests" (`docs/vcn.md:81`). [C]

---

## 6. Claimed results, limitations, evidence quality

### 6.1 Author's hardware acceptance claims

`docs/vcn.md:85-96` [C]:

- existing Cezanne `0x1638`: normal-size H.264 and HEVC short clips were observed with hardware decode
  and matching software pixel digests;
- 64×64 H.264 still fails;
- the three newly enabled devices (`0x15E7/0x1636/0x164C`) have no equivalent hardware result yet;
- a Hackintool check once triggered an incomplete `VCN0Dec` and a WindowServer watchdog;
- the GFXOFF call fix passed offline regression; hardware validation that the hang is gone is **not**
  done, so stable/complete decode must not be claimed;
- acceptance is expected to cover Hackintool decoder create/destroy, ≥5 min idle then start,
  continuous playback and desktop responsiveness, with `.gpuRestart`/WindowServer reports;
- Apple TV online-viewing key requests and system network/audio panics need separate acceptance; the
  renderer-selection patch does not prove those are fixed.

`1983f32` commit message [C] additionally claims, on a Ryzen 7 5800H (`1002:1638`), macOS 26.7.1
(`25G241`), installed Release kext, normal VideoToolbox/XPC:

- H.264/HEVC software, automatic and forced-hardware **encoding** at 720p/3 and 1080p/120 colour
  frames with full software decode/readback; final probe 1080p/60 including duplicate-timestamp
  rejection;
- H.264/HEVC **decode** matches software pixel digests at 720p;
- "48 native CPU and 188 dynamic cache bridge cases" pass, including sanitizers; all three builds
  pass; driver bytes identical to the tested candidate;
- unverified: 7430U, long-duration encoding, 10-bit HEVC, application workflows;
- the first 120-frame software HEVC control exceeded 30 s; a 60 s timeout passed.

### 6.2 Evidence quality

- All hardware statements are **single-author self-reports** in prose/commit messages. The repository
  contains no logs, no captured `ioreg` dumps, no pixel-diff artifacts, no test result files, and no
  CI. [F: repository inspection]
- The automated tests are CPU simulations by the author's own statement; they verify code logic, data
  matching and rollback, not firmware load, GPU hangs, VideoToolbox selection or decode correctness.
  [C: docs/vcn.md:81]
- The real hardware probe (`test_video_encode.m`) is a manual tool and is not part of the documented
  regression command list; its hardware mode is opt-in. [F]
- The docs contain an adverse result (64×64 H.264 fails, Hackintool watchdog), which raises confidence
  that the report is not purely promotional, but the claimed working path remains unverified here.
- The final commit's decode claim (720p pixel digests) and encode claims are consistent with the earlier
  docs claim, but both come from the same author; no independent reproduction exists in the repo.

### 6.3 License

- Fork `LICENSE:1-54`: **Thou Shalt Not Profit License version 1.5** (TSNPL). Copyright/patent/source
  grants, but distribution of the original or derivatives must stay under TSNPL (1(c)) and is limited
  to non-commercial purposes (17/50); attribution notices must be retained (6); AMD firmware is under
  the license exemption (52-54). [F]
- Fork `README.md:3-6` explicitly forbids "Use of Artificial Intelligence / Large Language Models on
  this repository, its copies or derived works ... including training, analysis, or modification". [F]
- Our checkout's `LICENSE.md:5-16,18-...` is the ChefKiss Open Knowledge License v1, which forbids
  derivative works and AI use. [F]
- Consequence: **no code from the fork can be copied into our tree without a human licence decision**;
  the more restrictive of the two regimes applies. This also means the current analysis itself is
  contrary to the fork's stated AI prohibition — that is a process/legal risk the Lead should record
  and resolve (e.g. only retain human-written conclusions, or obtain permission). No code was copied
  into this report; excerpts are short identifiers/byte patterns required for citation.

---

## 7. Applicability to macOS 15.8.1 (Darwin 24.6), Renoir `0x1638`

### 7.1 Current behaviour on 15.8.1

With the fork's code as-is on this target:

- `VCNCapabilities::rejection()` returns 4 (`majorMatches(MACOS_26)` is false for Darwin 24), so
  `VCN::processKext()` returns immediately, `UserVideoDecode::init()` returns after publishing
  `NRedVCNRejected=4`, and `VCN::ready()` is false.
- Therefore no X6000 load is requested, no metaclass/vtable hooks are installed, `X5000::allocateHWEngines()`
  does not touch the VCN slot, and `hasVCN0` stays false. The feature is **inert and safe by default**
  on 15.8.1. [F: gate code; I: runtime effect, consistent with gate order]

This is exactly matching our Phase 1 safety requirement (default off, no engine changes); the fork's
gate would need to be widened for our target.

### 7.2 Read-only observations on this machine (target: 15.8.1, 0x1638)

Method: `sw_vers`, `uname -r`, `ioreg`/`system_profiler`, and read-only Python parsing of
`/System/Library/KernelCollections/SystemKernelExtensions.kc` and
`/System/Volumes/Preboot/Cryptexes/OS/System/Library/dyld/dyld_shared_cache_x86_64h*`. No kext was
loaded, no GPU/VideoToolbox API was called, and no system file was written.

Host identity:

| item | value |
| --- | --- |
| macOS | 15.8.1 (`24H32`), Darwin `24.6.0` |
| CPU | AMD Ryzen 7 5800U (Cezanne class) |
| iGPU | `0x1638`, revision `0xC1`, reported as "AMD Radeon RX Renoir Graphics" |
| X5000 kext | `7.0.25 24657` (Info.plist `CFBundleGetInfoString`) |
| X6000 kext | `7.0.25 24657` |
| AMDRadeonVADriver2 | `7.0.25` (version.plist) |
| AppleGVA | `113.56` |
| kernel drivers location | in `SystemKernelExtensions.kc`; `/System/Library/Extensions/.../Contents/MacOS` is empty |
| dyld cache UUID | `42EC2890-BC40-317E-BB27-45142BFF7F26` (not the fork's reference `31374756-...`);
| dyld cache base | `0x7FF800000000`; header mappingOffset 552 / 4 mappings; image table offset 1000 / 3258 images |

**Kernel-side ABI comparison [O]:** I resolved the X5000/X6000 symbols/vtables from the kernel
collection and compared them to `VCNKernelData.hpp`:

- all six guard slots per side were checked; method identities (`isDeviceValid`, `disableGfxOff`,
  `enableGfxOff`) and the getters at X5000 `0x3B8`/`0x3C8` and X6000 `0x3B0`/`0x3C0` match;
- the anonymous getter at X5000 `0x2A8` (and X6000 `0x2B0`) **differs**: on 15.8.1 it is
  `55 48 89 E5 8A 87 0E 03 00 00 5D C3` (13 bytes, returns the raw byte) while the reference guard
  expects 14 bytes with a trailing `24 01` (`and $1`);
- consequently the fork's `verifyKernelABI()` would reject **both** drivers on 15.8.1 (safe fallback,
  `NRedVCNKernelABIRejected=1`), even if the OS gate were changed;
- patch call-sites: 16 of 20 match the reference bytes at the same offsets. The four that differ are
  small offsets, not missing calls:
  - `AMDNavi10VideoContext::setSuspendResumeState`: actual slot-0x2B0 call at `+0x28`, reference `+0x27`;
  - `AMDHWChannel::writeDiagnosisReport`: actual slot-0x3B0/0x3C0 calls at `+0x52B/+0x7AD/+0x9C3`,
    reference `+0x524/+0x7A5/+0x9B8`.
  A generator run against the 15.8.1 X6000 binary would therefore find the same call targets at
  shifted offsets (still requiring regeneration, and review of the getter change).

**User-space comparison [O]:** I replicated the fork's cache/image parsing and function-match logic
read-only on this machine's cache:

- the fork's image parser works on 15.8.1: it finds
  `/System/Library/PrivateFrameworks/AppleGVA.framework/Versions/A/AppleGVA`,
  `/System/Library/Extensions/AMDRadeonVADriver2.bundle/Contents/MacOS/AMDRadeonVADriver2` and
  `/usr/lib/system/libsystem_c.dylib` by path, and their Mach-O text/const sections;
- 16 of the 18 reference functions match uniquely, byte-for-byte except at reference relocations:
  `VAAcceleratorInfo::identify` (448 B), `VAFactory::createGraphicsEngine` (152 B),
  `createImageBlt` (82 B), three VPE factories (130 B each), `Addr::Lib::Create` (672 B),
  `HwlConvertChipFamily` (160 B), the encode functions (36/408/508/292 B), and the swizzle consumers
  (96/848/752/3456 B);
- 2 functions do **not** match:
  - the AppleGVA decoder-selection function (reference size 1136 B): no occurrence of the reference's
    32-byte prefix; only one 24-byte-prefix candidate exists and the full function does not match —
    15.8.1's AppleGVA is a different build (113.56);
  - `VAAddrLibInterface::init`: reference 530 B, actual 15.8.1 function 528 B, full match fails.
    However the specific patch bytes at `+0x8B/+0x8E` (`41 81 FC 8D 00 00 00`) are present, so the
    family patch itself is still applicable after re-deriving the reference function data;
- the swizzle-mask values `0x02220221` / `0x06660661` still occur 4× each in the VADriver2 text,
  matching the generator's expected count;
- consequence: the fork's dynamic resolver would fail on 15.8.1 (`UserDynamicRejected=1`, no writes)
  because the plan requires all reference functions; the existing Tahoe-generated data cannot be
  reused even if the kernel side were ported. The static path is also unusable because the cache UUID
  differs. [O; I: resolver semantics reproduced closely but not via the fork's compiled code]

**Tooling consequence [O]:** `tools/build_vcn_kernel_patch.py` would fail on 15.8.1 before any ABI
check because it hard-codes on-disk kext paths
(`/System/Library/Extensions/AMDRadeonX5000.kext/Contents/MacOS/...`) that are empty on this system;
the drivers must be extracted from the kernel collection (or a KDK). `tools/metal_cache.py` /
`build_video_decode_patch.py` would fail at the cache UUID/base assertion, and the AppleGVA
address/hashes are reference-specific. Both generators need a 15.8.1-specific port plus a human
re-derivation step for the two differing user-space functions.

### 7.3 What would be needed for 15.8.1 (explicit list)

1. **OS gate:** add a Darwin 24 / macOS 15.x path (or a version→data table) instead of
   `majorMatches(MACOS_26)`; keep `-NRedVCN` opt-in and default off. Keep 26.x data separate.
2. **Kernel generated data for 15.8.1:** extract the 15.8.1 X5000 and X6000 Mach-Os from
   `SystemKernelExtensions.kc`/KDK, record SHA-256 and versions (`7.0.25 24657` here), dump
   `__ZTV32AMDRadeonX5000_AMDVega10Hardware` and `__ZTV32AMDRadeonX6000_AMDNavi10Hardware`, regenerate
   the six guards per side, review the getter change (13 vs 14 bytes; determine whether the `and $1`
   difference is semantically irrelevant and how to express the guard), regenerate the 20 call-site
   patches (expect the four shifted offsets), re-verify the exactly-four GFXOFF wait pairs and the
   submit-signal patch. Do not ship with a failing guard silently bypassed.
3. **User-space generated data for 15.8.1:** adapt `metal_cache.py`/generator to the actual cache
   (`42EC2890-...`, base `0x7FF800000000`); re-locate the AppleGVA selection function and re-derive its
   branch patch; re-derive `VAAddrLibInterface::init` reference bytes/size (528 B) and its location;
   revalidate the other 16 matching function references/hashes and all 22 patch sites against 15.8.1;
   regenerate identify payloads and `CacheUUID/CacheBase`, static offsets, page count; re-check the
   encode patch contexts. Run the CPU regressions and the package-dependency test against the new data.
4. **Firmware path:** confirm the existing `HWLibs` VCN firmware injection (`ativvaxy_nv.dat`,
   ipVersion `0x0202`) is still exercised by 15.8.1's native X6000 engine; no new firmware work is
   expected from the fork's design.
5. **Optional X6000 load on a Renoir-only 15.8.1 machine:** prove `OSKextLoadKextWithIdentifier`
   synchronously loads the X6000 kext and its `AMDVCN2HWEngine`/`AMDAccel*` metaclasses on this
   platform, and that the X5000 accelerator start path can allocate the engine before its native start
   consumes `hasVCN0`.
6. **Hardware acceptance, from scratch on 15.8.1:** short H.264/HEVC clips vs software pixel digests;
   64×64; Hackintool decoder create/destroy; ≥5 min idle then start; continuous playback; desktop
   responsiveness; GPU reset/Watchdog absence; GFXOFF hang reproduction; Apple TV DRM; long-duration;
   10-bit HEVC; other APUs (`0x15E7/0x1636/0x164C`) and `7430U` if relevant.
7. **Data collection:** OS build/`sw_vers`/`uname -r`; iGPU PCI ID/revision and ioreg media properties;
   X5000/X6000/HWLibs binaries from KC/KDK with hashes; dyld cache UUID/map and AppleGVA/VADriver2/
   libsystem_c images and symbols; `ioreg -l -w0 -r -n IGPU | rg NRed` fields; decoder
   `UsingHardwareAcceleratedVideoDecoder`; software-vs-hardware decode output; `.gpuRestart`/
   WindowServer reports if a hang occurs.

### 7.4 Unknowns specific to 15.8.1

- Whether 15.8.1's X5000 native accelerator has the same `hasVCN0` semantics and engine-array layout as
  the author's 25.6/26.x reference (the NootedRed field offsets for 13+ are the same in our tree, but
  the Apple binary behaviour is not proven here).
- Whether the X6000 kext can be loaded and its VCN2 engine initialised on a Renoir-only machine on
  15.8.1 (the author reports this only for macOS 26.7.1).
- Whether 15.8.1's `VADriver2`/`AppleGVA` still select a hardware VCN2 decoder after identifying the
  iGPU as family 5 (the identify function matches structurally, but the actual 15.8.1 AppleGVA build
  differs and the end-to-end result is untested).
- Whether the getter change (missing `and $1`) is safe to accept; whether the guard should be relaxed
  or the vtable slot interpreted differently.
- Whether the AppleGVA fallback branch exists with the same control flow in 113.56; the reference
  function does not match, so a fresh analysis is required.
- Whether 15.8.1 hardware has the same 64×64 H.264 failure, Hackintool watchdog, GFXOFF hang and DRM
  behaviour; the fork gives no 15.8.1 data at all.
- Whether the encode fixes (feedback stride 48, pitch) apply to 15.8.1's VADriver2; the reference
  function bytes matched, but no hardware run exists.

---

## 8. Reuse / risk assessment for our Phase 2–4

### 8.1 Conceptually reusable (no code copying)

- **The central design insight:** do not implement VCN firmware/rings ourselves; load the native
  `AMDRadeonX6000` VCN2 engine and place it in the X5000 accelerator's PM4 engine array at
  `kAMDHWEngineTypeVCN0`, set `hasVCN0`, then adapt the cross-driver calls. This is the only known
  community approach that reaches hardware decode on Renoir/Cezanne-class APUs.
- **Hard gates before bridging:** opt-in boot arg, real PCI identity match, optional X6000 load,
  both-sides hook readiness, engine-allocation success, vtable/getter ABI verification. These map
  directly onto our Phase 2/3 safety rules (default off, no PANIC, fall back to software).
- **Cross-driver vtable slot adaptation** with per-build generated data and explicit ABI guards; the
  "call receiver must be the hardware object" verification is the key correctness idea.
- **Metaclass pair bridging via `safeMetaCast`** including class-size checks, to let X5000 objects
  satisfy X6000 class expectations and vice versa.
- **User-space transaction design:** all-or-nothing byte verification, RX-only leaf check, private COW
  pages, restore RX, rollback on any failure, idempotent re-entry, bounded reads/candidates, rich
  diagnostic stages. This is exactly the safety envelope our Phase 4 should adopt (whether via
  `_shared_region_check_np` or another early per-process hook).
- **Dynamic function/relocation matching** for same implementation / different cache layout; a good
  middle ground between "hard-coded cache offsets" and "new OS adaptation".
- **Per-device identify payload** for `vaFamily=5` and the AddrLib family/0x8D→0x8E fixes: these are
  concrete decode-enablement changes we may need to re-derive for our tree.
- **Diagnostics scheme** (`NRedVCN*` props) and the convention of publishing requested/validated/
  applied separately from claimed success.

### 8.2 Must be regenerated / rewritten for our target

- All kernel data (`VCNKernelData`) from 15.8.1's X5000/X6000 binaries, including the getter guard
  difference and shifted call sites; plus extraction of the binaries from the kernel collection/KDK.
- All user-space data (`UserVideoDecodeData`, resolver reference data) from 15.8.1's cache; the
  AppleGVA selection and `VAAddrLibInterface::init` functions need fresh analysis; static fast path
  UUID/base must change or be dropped for 15.8.1.
- OS gate and version-data selection; ideally a version→data table rather than another hard-coded
  `majorMatches`.
- Build-system integration: the fork is Xcode/older base; our tree is AppleForge/xmake (`xmake.lua`)
  and uses `UserPatches` (`_cs_validate_page`) rather than `_shared_region_check_np`. The user-space
  hook mechanism would be new code for ours; we already have a different, validated COW/validation
  framework to compare against.
- Our tree's Phase 1 `DeviceProfile::MediaPolicy` / `VDA` plan is inert and default-off; the fork's
  gating approach is compatible with it, but the engine flags must only flip after our own validation.

### 8.3 Main risks / failure modes

- **GFXOFF / wait-idle / watchdog hang.** This is the biggest hardware risk; the author observed
  VCN0Dec incomplete and a WindowServer watchdog and explicitly says the GFXOFF fix is only
  offline-validated. Our Phase 2/3 should reproduce the wait pairing carefully and plan a recovery
  path (bootable EFI, remove arg, reboot) before any user-visible decode.
- **User-space patch of a shared-cache driver in VTDecoderXPCService.** COW writes to AppleGVA/
  VADriver2 in every matching process are invasive; a bad pattern/offset or a race between processes
  could crash the decoder or desktop. The fork's transaction guarding is good, but 15.8.1 data must be
  generated and tested, not adapted by hand.
- **Two mismatching reference functions on 15.8.1** (AppleGVA selection and `VAAddrLibInterface::init`)
  mean the user-space half is not a drop-in; if we cannot re-derive them, decode may still fail even
  with a working kernel bridge.
- **Cross-driver ABI fragility:** the bridge patches Apple's X6000 code at exact call sites and depends
  on X5000/X6000 vtable layouts; every macOS update can break it (safe rejection, but work).
- **Optional X6000 load semantics** on a non-Navi machine are unusual; if the kext does not load or
  its classes are not usable on Renoir-only 15.8.1, Phase 2 cannot proceed with this approach.
- **Hardware coverage:** 64×64 H.264 failure, DRM/Apple TV, long-duration, 10-bit HEVC, multiple APUs;
  the author's own list of unverified items is substantial.
- **Legal/AI:** code copying is blocked by both licences; the fork README forbids AI analysis. Any
  implementation must be an independent human-written design/implementation, and the project's own
  policy (AGENTS.md: "the project license forbids AI use") should be explicit about this analysis.
  The Lead should decide how to record/limit that risk.

### 8.4 Recommendation

Treat the fork as a **design reference and evidence that the bridge approach can work**, not as a
source to port. For our Phase 2–4:

1. Phase 2: prototype on 15.8.1 only the safe, non-user-visible pieces: extract 15.8.1 X5000/X6000
   binaries, regenerate ABI guards/patch data, validate the optional X6000 load and engine allocation,
   keep `hasVCN0` gated off and every failure path a no-op.
2. Phase 3: validate VCN firmware/ring init through the native X6000 engine with the engine slot
   inserted but no user-space exposure; log readbacks; no HW decode claim.
3. Phase 4: only after kernel bring-up is stable, consider a user-space `vaFamily=5`/GVA selection
   patch — generated from 15.8.1 data, behind `-NRedVDA`, with rollback and hardware acceptance.
4. Plan a separate 15.8.1 data generation/documentation task; do not mix Tahoe-generated headers with
   our 15.8.1 tree.
5. Resolve licence/AI questions with the project owner before any code or generator reuse; this report
   is analysis only.

---

## 9. References

### Fork commits (all in `/tmp/nr-fork-steve`, branch `master`)

- `8e93e82` — upstream base ("HWLibs: Fix SMU 12 reimplementation on Catalina", 2026-09-29)
- `be1545a` — "X5000: Fix Cezanne accelerated rendering" (fork-only; adds surface-sync, not VCN)
- `02d3789` — "VCN: Add experimental Cezanne hardware decoding" (initial kernel bridge + user-space COW + docs; limited to `0x1638`, Darwin 25.6)
- `ae8f6bc` — "VCN: Generalize VCN2 APU capability matching" (four VCN2 devices, per-device identify)
- `6c7b080` — "VCN: Match Tahoe with native kernel version constants" (Darwin 25.x, ABI guards, engine slot from field+enum)
- `338fff9` — "UserPatches: Share dynamic cache resolution" (shared cache/resolver, dynamic video plan, cross-page rollback)
- `1983f32` — "VCN: Fix hardware encode feedback and source pitch" (encode fixes; HEAD)

### Fork files / docs

- `docs/vcn.md:1-118` (device table, OS gate, implementation, generation/tests, acceptance claims, diagnostics)
- `docs/user-cache-dynamic.md:1-42` (dynamic resolver scope, UUID/static fast path, limits)
- `NootedRed/VCN.cpp:1-339`; `VCN.hpp:1-14`; `VCNCapabilities.hpp:1-46`; `VCNKernelData.hpp:1-65`
- `NootedRed/UserVideoDecode.cpp:1-429`; `UserVideoDecode.hpp:1-4`; `UserVideoDecodeData.hpp:1-1444`;
  `UserVideoDecodeResolver.hpp:1-29`; `UserSharedCache.hpp:1-262`; `UserCacheResolver.hpp:1-336`
- `NootedRed/X5000.cpp:310-350` (engine allocation, `hasVCN0`); `X5000.hpp` (unchanged by VCN commits)
- `NootedRed/NRed.cpp:54-55,68,84`; `NootedRed/Info.plist:43-61`; `NootedRed/Kexts.cpp/.hpp` (unchanged)
- `NootedRed/HWLibs.cpp:875-907` (VCN firmware injection; pre-existing, unchanged by VCN commits)
- `tools/build_vcn_kernel_patch.py:1-140`; `tools/build_video_decode_patch.py:1-295`;
  `tools/audit_video_driver.py:1-116`; `tools/vcn_capabilities.py:1-21`;
  `tools/video_addr_convert.s:1-41`; `tools/metal_cache.py:1-95`; `tools/cache_resolver_data.py`
- tests: `tools/test_vcn_native_load.py`, `test_vcn_engine_bridge.py`, `test_vcn_kernel_abi.py`,
  `test_vcn_user_bridge.py`, `test_video_cache_dynamic.py`, `test_vcn_candidate.py`,
  `test_vcn_idle_calls.py`, `test_vcn_gva_choice.py`, `test_vcn_package_dependencies.py`,
  `test_video_encode.m`, `run_video_encode_test.py`
- `LICENSE:1-54` (TSNPL 1.5); `README.md:3-6` (AI prohibition); `Info.plist` dependencies

### Our tree (context, current state at time of writing)

- `AGENTS.md:54-88` (conventions, licence/AI note; "Do not add AI/assistant attribution")
- `LICENSE.md:5-16` (no derivatives), `:18` (AI prohibition)
- `NootedRed/X5000.cpp:321-329` (empty VCN placeholder), `:350-356` (`hasVCN0` from profile);
  `X5000.hpp` has `pm4EngineField`/`hasVCN0Field`
- `NootedRed/DeviceProfile.hpp:70-95` (`MediaPolicy`: generation, firmware, ring/fence, H.264/HEVC/AV1, `hasVCN0=false`)
- `docs/Device-Profile.md` (media policy, integration points, safety); `NootedRed/UserPatches.hpp:30-38`
  (our user-space path is `_cs_validate_page`, not the fork's `_shared_region_check_np` COW)
- `NootedRed/HWLibs.cpp:55-59,653,884-897` (our tree carries the same VCN firmware blobs/injection)
- `NootedRed/PenguinWizardry/KernelVersion.hpp` (`MACOS_15` = 24.x, `MACOS_26` = 25.x)

### Local system observations (read-only, not repository files)

- macOS 15.8.1 (24H32), Darwin 24.6.0; Ryzen 7 5800U; iGPU `1002:1638` rev `0xC1`.
- `/System/Library/Extensions/AMDRadeonX5000.kext/Contents/Info.plist` and X6000: `7.0.25 24657`;
  `AMDRadeonVADriver2.bundle/Contents/version.plist`: `7.0.25`; AppleGVA `113.56`.
- `/System/Library/KernelCollections/SystemKernelExtensions.kc`: contains `com.apple.kext.AMDRadeonX5000`
  and `com.apple.kext.AMDRadeonX6000`; the on-disk `Contents/MacOS` executables are absent.
- `/System/Volumes/Preboot/Cryptexes/OS/System/Library/dyld/dyld_shared_cache_x86_64h*`: UUID
  `42EC2890-BC40-317E-BB27-45142BFF7F26`, base `0x7FF800000000`, mappingOffset 552 / mappingCount 4,
  imagesOffset 1000 / imageCount 3258; image paths and text/const sections found for AppleGVA,
  AMDRadeonVADriver2 and libsystem_c.

### External background

- Linux AMDGPU hardware list (used by the fork for VCN generations, `docs/vcn.md:16`):
  <https://www.kernel.org/doc/html/v6.17/gpu/amdgpu/amd-hardware-list-info.html>
- Apple dyld cache format (used by the dynamic resolver, `docs/user-cache-dynamic.md:26`):
  <https://github.com/apple-oss-distributions/dyld/blob/main/include/macho/dyld_cache_format.h>
