# 15.8.1 AppleGVA decoder-selection patch data

Status: derived statically (read-only) on macOS 15.8.1 (24H32, Darwin 24.6.0) on the AMD Renoir
`0x1638` Hackintosh. No kext was loaded or installed, no system file or dyld cache was modified, and
no GPU/VideoToolbox API was called. Evidence labels: **[F]** fact read from an input file, **[O]** our
own read-only observation on this machine, **[I]** inference from disassembly, **[C]** claim from the
prior-art report (not independently reproduced here), **UNKNOWN** no reliable evidence.

Deliverables:

- [`tools/gen_vda_gva_data.py`](../tools/gen_vda_gva_data.py) - derives everything below from the live
  dyld cache and emits [`NootedRed/VDAGVAData.hpp`](../NootedRed/VDAGVAData.hpp); `--check` re-derives
  and compares the header against the live pages.
- [`NootedRed/VDAGVAData.hpp`](../NootedRed/VDAGVAData.hpp) - one `UserPatches::PagePatch` entry with an
  exact 4 KiB cache page, a 6-byte find and a 6-byte replacement.

The patch is only appended by `UserPatches` when `-NRedVDA` is set (the VDA tables are gated by
`UserPatches::vdaEnabled`), and it is a whole-page match, so it fails safe (no match, no write) when
the OS build changes.

## 1. Verdict

- The 15.8.1 AppleGVA decoder-selection function is the local `AVDGetGPUAccelerators` implementation at
  `0x7FFA06692198..0x7FFA06692840` in the shared-cache AppleGVA image (AppleGVA 113.56). **[F/O]**
- On a non-special board-id it only inserts the `IntelAccelerator` slot and, when no Intel accelerator
  exists, returns an empty accelerator list; the AMD accelerator that `AVDLoadAllAccelerators` already
  collected sits in a separate array and is never returned. That empty list is why hardware decode is
  not offered on an AMD-only machine. **[F/I]**
- One branch is the fallback: `je` at `0x7FFA0669241B` jumping to the empty-return path
  `0x7FFA066927F7`. Redirecting it to the AMD insertion tail `0x7FFA0669244B` (disp `+0x2A`) returns
  the already-collected AMD accelerator instead. **[F/I]**
- Derived patch: page `0x7FFA06692000`, page offset `0x41B`,
  `0F 84 D6 03 00 00` -> `0F 84 2A 00 00 00` (6 bytes, within the 32-byte limit). Confidence in the
  site and semantics: high. Confidence in the end-to-end result: medium, because the kernel VCN2
  bridge and the `AMDRadeonVADriver2` identify/family patches (separate tasks) are still required and
  nothing has been run on hardware yet. **[I]**
- The UnfairGVA-style patches (`board-id` -> `hwgva-id`, VideoToolbox DRM model, CoreLSKD CPUID) are
  **not** part of this path on 15.8.1: `board-id` is read successfully and this patch handles the
  no-Intel fallback directly. They remain relevant only for DRM/Apple TV playback, which is out of
  scope here. **[I]**

## 2. Inputs and identification

| item | value | source |
| --- | --- | --- |
| macOS | 15.8.1, build 24H32, Darwin 24.6.0 | `/System/Library/CoreServices/SystemVersion.plist` **[F]** |
| dyld cache | UUID `42EC2890-BC40-317E-BB27-45142BFF7F26`, base `0x7FF800000000`, 28 mappings across `dyld_shared_cache_x86_64h`..`.06` | cache header and mapping tables **[F]** |
| AppleGVA | `/System/Library/PrivateFrameworks/AppleGVA.framework/Versions/A/AppleGVA`, 113.56 | cache image table + on-disk `Resources/Info.plist` **[F]** |
| AppleGVA image | address `0x7FFA0666E000`, image UUID `53DD765D087D322EA410BD22D586C08C`, `cputype 0x01000007` | Mach-O header/`LC_UUID` **[F]** |
| AppleGVA `__text` | `0x7FFA0666EF40`, size `0xC814C` | `LC_SEGMENT_64`/`__TEXT,__text` **[F]** |
| target page | `0x7FFA06692000` = `dyld_shared_cache_x86_64h.02` file offset `0x6692000`, SHA-256 `d8a48edff484b8fe0b3a3d429e08f264f00fcb9f4e68304b9a87e5ad20eca474` | cache mapping + direct subcache file read, verified identical **[F]** |

The generator refuses to run unless the OS build, AppleGVA version, cache UUID/base and AppleGVA image
UUID all match the constants it was written for. That keeps 15.8.1 data from being mixed with another
build.

### 2.1 Target machine relevant properties [O]

Read with `ioreg -l -w0` (read-only):

| property | value |
| --- | --- |
| `hw.model` | `MacBookPro16,2` |
| `board-id` | `Mac-5F9802EFE386AA28` (CFData) |
| AMD IGPU `IOGVACodec` | `AMDVCN2` |
| AMD IGPU `IODVDBundleName` | `AMDRadeonVADriver2` |
| AMD IGPU `IOVARendererID` | `16908288` = `0x1020000` |

An offline IOKit probe replicating the call shape of the AppleGVA `board-id` helper
(`IORegistryGetRootEntry` + `IORegistryEntrySearchCFProperty` on the `IOService` plane, see §4.1)
returned the same CFData `Mac-5F9802EFE386AA28`. So the board-id lookup succeeds on this machine and
the non-special fallback path is reachable. **[O/I]**

## 3. How the selection function was located

AppleGVA keeps no local symbols in the dyld cache (only 384 exported functions), so the function was
resolved from its behaviour and its own strings, not from a name:

1. The three diagnostic strings below are C strings in `__cstring` and each is referenced by exactly one
   RIP-relative `lea` in `__text` **[F]**:

   | string | address | reference |
   | --- | --- | --- |
   | `GVA error: AVDGetGPUAccelerators, no plugin ` | `0x7FFA06741D61` | `0x7FFA06692309` |
   | `GVA error: AVDGetGPUAccelerators, invalid boardId ` | `0x7FFA06741E0A` | `0x7FFA06692468` |
   | `GVA info: AVDGetGPUAccelerators, force AMD for flow_id %d ` | `0x7FFA06741D8F` | `0x7FFA06692435` |

2. `LC_FUNCTION_STARTS` (`dataoff 0x23CC8130`, `datasize 0x1280`, ULEB128 deltas from `__TEXT`) places
   all three references inside the function `0x7FFA06692198..0x7FFA06692840` (size `0x6A8`). The next
   function starts at `0x7FFA06692840`. **[F]**
3. The cross-references are part of Apple's own log messages for `AVDGetGPUAccelerators`, so the
   function is the decoder-selection entry point even though the symbol is stripped. The same function
   contains all three messages and none of the other AppleGVA functions reference them. **[F/I]**

## 4. What the function does (and why this branch is the AMD fallback)

`AVDGetGPUAccelerators` receives the accelerator list built by `AVDLoadAllAccelerators`, classifies the
entries and returns the subset to be used for the requested flow. The interesting part for VDA:

### 4.1 Accelerator classification and the "no GPU at all" check

`r15` is the single `IntelAccelerator` slot (`-0x158(%rbp)`) and `-0x130(%rbp)` is the array of AMD
accelerators; the count is kept in `edi`/`-0x138(%rbp)`. Classification is driven by
`IOVARendererID` at struct offset `0x428`:

```text
7FFA066922A9  movl  0x428(%r12), %edx          ; IOVARendererID
7FFA066922B1  leal  -0x1080001(%rdx), %esi     ; Intel generation range 0x1080001..0x1080020
...
7FFA066922C1  movq  %r12, -0x158(%rbp)         ; Intel accelerator slot
...
7FFA066922CD  leal  -0x1020000(%rdx), %esi     ; AMD range 0x1020000..0x1020004
7FFA066922D3  cmpl  $0x4, %esi
7FFA066922D6  ja    ...
7FFA066922D8  btl   %esi, %eax                ; eax = 0x15 -> families 0, 2, 4
7FFA066922DD  movq  %r12, -0x130(%rbp,%rdx,8)  ; AMD accelerator array
```

`0x1020000` is exactly the AMD IGPU's `IOVARendererID` on this machine, so the iGPU lands in the AMD
array. **[F/O]**

Before any board-id handling, the function rejects the "no useful GPU" combination:

```text
7FFA066922F7  testq %r15, %r15                 ; Intel accelerator present?
7FFA066922FA  sete  %al
7FFA066922FD  testl %edi, %edi                 ; AMD accelerator count non-zero?
7FFA066922FF  sete  %cl
7FFA06692304  cmpb  $1, %cl
7FFA06692307  jne   ...
7FFA06692309  ... "AVDGetGPUAccelerators, no plugin" ...
7FFA0669231C  jmp   0x7FFA066927FF             ; return empty
```

So every later path in this function has at least one GPU; when the Intel slot is empty the AMD array
is non-empty. **[F/I]**

### 4.2 The `gvaForceAMD*` override path and the board-id checks

```text
7FFA06692327  cmpq  $0, -0x130(%rbp)           ; any AMD accelerator?
7FFA0669232F  je    0x7FFA066923A9             ; no: skip the force checks
; read CFPreferences gvaForceAMDAVCDecode/AVCEncode/HEVCDecode/KE, compare flow_id with 0xB/2/0xE
7FFA06692386  jne   0x7FFA06692435             ; force AMD
...
7FFA066923A9  ... call board-id helper (reads IORegistry "board-id") ...
7FFA066923CD  je    0x7FFA06692468             ; -> "invalid boardId", return empty
7FFA066923D3  ... strcmp "Mac-F60DEB81FF30ACF6" -> 0x7FFA06692499
7FFA066923EA  ... strcmp "Mac-7BA5B2D9E42DDD94" -> 0x7FFA066925C3
7FFA06692401  ... strcmp "Mac-27AD2F918AE68F61" -> 0x7FFA066925E5
```

The three board-id strings are Apple's special AMD Macs. On this machine `board-id` is
`Mac-5F9802EFE386AA28`, so all three comparisons fail and control reaches the fallback at
`0x7FFA06692418`. **[F/O]**

The `board-id` helper at `0x7FFA06693A17` takes a CFString key: the `__cfstring` object at
`0x7FFA40A28060` points to the C string `board-id` at `0x7FFA06741E93`, and the result is treated as
CFData (byte pointer + length, then copied). Our IOKit probe confirmed the same lookup returns the
CFData `Mac-5F9802EFE386AA28`. So no UnfairGVA `board-id` -> `hwgva-id` spoof is needed for this
function to get a board-id. **[F/O/I]**

### 4.3 The fallback branch and the patch

```text
7FFA06692418  testq %r15, %r15                 ; Intel accelerator found?
7FFA0669241B  je    0x7FFA066927F7             ; no -> return the list collected so far (empty)
7FFA06692421  leaq  -0x158(%rbp), %rsi         ; yes -> push the Intel accelerator
7FFA06692428  movq  %rbx, %rdi
7FFA0669242B  call  0x7FFA0669392C             ; vector push_back
7FFA06692430  jmp   0x7FFA066927F7
7FFA06692435  ... "force AMD for flow_id" log ...
7FFA0669244B  movq  0x8(%rbx), %r15            ; AMD insertion tail (shared with the force path)
7FFA0669244F  movq  0x10(%rbx), %rax
7FFA06692453  cmpq  %rax, %r15
7FFA06692456  jae   0x7FFA066924F9             ; grow vector
7FFA0669245C  movq  -0x130(%rbp), %rax         ; first AMD accelerator
7FFA06692463  movq  %rax, (%r15)
7FFA06692466  jmp   0x7FFA06692490             ; store end pointer, return
```

The Intel insert path and the conditional entry (`0x7FFA06692430` `jmp 0x7FFA066927F7`) share the
empty-return target, confirming that the `je` target is the "nothing selected" exit. The AMD insertion
tail is the same code reached by the `gvaForceAMD*` overrides. **[F/I]**

Patch:

| item | value |
| --- | --- |
| page | `0x7FFA06692000` (`__TEXT` page; exact cache page image embedded) |
| site | `0x7FFA0669241B`, function offset `+0x283`, page offset `0x41B` |
| original | `0F 84 D6 03 00 00` (`je 0x7FFA066927F7`, disp `+0x3D6`) |
| patched | `0F 84 2A 00 00 00` (`je 0x7FFA0669244B`, disp `+0x2A`, the AMD insertion tail) |
| length | 6 bytes (both) |
| find occurrences in the page | exactly 1 |

Register state at the site: `rbx` = output `std::vector` pointer (set at function entry), `rbp` =
function frame, `r15` = Intel slot (NULL here). The replacement target reloads `r15`/`rax` from the
output vector, so no register from the skipped Intel-insert block is required. `-0x130(%rbp)` is
non-NULL because the function would have returned at `0x7FFA06692309` otherwise. **[F/I]**

### 4.4 Which paths are left alone

- **Intel + AMD** (`r15 != NULL`): the branch is not taken; Intel is inserted exactly as before.
- **Explicit device request** (`needToLoad != 0`, checked at the top of the function): it takes the
  early lookup path and never reaches this branch.
- **Apple's special board-ids**: they branch to their own handlers at `0x7FFA06692499/25C3/25E5`
  before the fallback.
- **No GPU at all**: returns at `0x7FFA06692309` before the board-id block.
- **`gvaForceAMD*` set / matching flow ids**: uses the same AMD tail natively.
- **Intel-only machines**: unchanged.

Only "no Intel accelerator + non-special board-id + AMD accelerator already collected" is changed.

### 4.5 Is the AMD accelerator really collected on this machine?

`AVDLoadAllAccelerators` (`0x7FFA06692D49..0x7FFA066937C4`) scans the `IOService` plane, looks up the
`IOGVACodec` property (CFString `0x7FFA40A27FC0` -> `IOGVACodec`), reads `IODVDBundleName`
(CFString `0x7FFA40A280E0`) and `IOVARendererID`, and creates a global accelerator entry for every
service that has `IOGVACodec`. At the end it returns success iff the global accelerator list is
non-empty:

```text
7FFA06693634  movq  gAcceleratorList(%rip), %rax
7FFA06693640  sete  %al
7FFA06693643  movl  $0xA, %ebx
7FFA06693648  cmovnel %ecx, %ebx               ; ecx = 0 -> return 0 when the list is non-empty
...
7FFA066936F2  movl  %ebx, %eax
```

The AMD IGPU has `IOGVACodec = AMDVCN2`, `IODVDBundleName = AMDRadeonVADriver2` and
`IOVARendererID = 0x1020000`, so it becomes the accelerator that `AVDGetGPUAccelerators` classifies
into the AMD array and that this patch returns. **[F/O/I]**

## 5. Cross-check against the fork's Tahoe report (semantic only)

The steve12311 fork patches a 1136-byte AppleGVA "decoder-selection" function at `+0x287`: it changes
`0F 84 AC 01 00 00` (`je`, disp `+0x1AC`) to `0F 84 2A 00 00 00` (`je`, disp `+0x2A`) and describes it
as "no Intel -> use the existing AMD insertion path". **[C]**

Our independent 15.8.1 derivation lands at function offset `+0x283` and produces the **same
replacement displacement `+0x2A`** (the AMD tail is the same distance after the branch), while the
original displacement differs (`+0x3D6`) because the 15.8.1 function inlines more `std::vector` code
and is larger (`0x6A8` vs 1136 bytes). The structural match of the branch/insert-block distance plus
the identical control-flow interpretation is independent supporting evidence; no fork bytes or
generated data were copied. **[I]**

## 6. What this patch does *not* solve

- **`AMDRadeonVADriver2` identify/family**: after the accelerator is returned it is created through
  the VA driver bundle. The 15.8.1 VA driver still has to identify `0x1638` as a supported VCN2/Navi
  family and use the GFX9 AddrLib paths. That is the separate VA data task (`tools/gen_vda_va_data.py`,
  `NootedRed/VDAVAData.hpp`).
- **Kernel side**: the X5000 accelerator must expose the VCN2 engine and the `IOGVACodec`/HEVC
  capability properties; this patch only chooses the user-space accelerator. Those are the kernel
  bridge and personality tasks.
- **Other AppleGVA call paths** (`AVDLoadAllAccelerators`, plugin dlopen, the QT decode flows) are
  untouched.
- **DRM / Apple TV**: the legacy UnfairGVA table (VideoToolbox DRM model spoof, `board-id` ->
  `hwgva-id`, CoreLSKD CPUID) is for DRM stream playback and old CPUs. It is not needed to make the
  VCN2 decode accelerator selectable and is deliberately not included. If DRM is wanted later it has
  to be derived separately for 15.8.1.

## 7. Confidence, safety and failure behaviour

- **High confidence** in function identification, the branch site, the AMD-tail target and the patch
  bytes: multiple independent log-string cross-references, `LC_FUNCTION_STARTS` bounds, exact
  fall-through/tail byte checks, a unique 6-byte find in the page, and an independent semantic match
  with the fork report. The generator repeats all of these checks on every run and in `--check`.
- **Medium confidence** in the end-to-end result: no hardware run has been done, and the decode path
  additionally depends on the VA driver patches and the kernel VCN bridge. The decision that the
  15.8.1 VideoToolbox path actually calls this function for H.264/HEVC remains to be observed on
  hardware.
- **Fail-safe**: the native `UserPatches` backend compares the whole 4 KiB page before writing and
  verifies the find bytes at the site; on any mismatch it does nothing. The table is only enabled by
  `-NRedVDA`.
- **No new disablement of the Intel path**: mixed-GPU machines keep native behaviour.
- **Unknowns**:
  - whether the page buffer handed to `_cs_validate_range` for the dyld shared cache is byte-identical
    to the subcache file page embedded here (it should be: the generator verifies the page against the
    `.02` file page, but the kernel-side validation path has not been observed at runtime);
  - whether `VTDecoderXPCService`/VideoToolbox on 15.8.1 uses `AVDGetGPUAccelerators` for the tested
    H.264/HEVC decode flows;
  - whether creating the AMD accelerator without an Intel accelerator succeeds end-to-end once
    `AMDRadeonVADriver2` accepts the device.

If the branch/patch cannot be derived in the future (OS update), the generator refuses to emit data and
the deliverable stays an empty-but-compiling `Patches[]` table. The best next candidate in that case is
the same branch resolved through the three `AVDGetGPUAccelerators` log strings plus
`LC_FUNCTION_STARTS`, as this file documents; if the strings are gone, the next step is a
`VideoToolbox`/`VTDecoderXPCService` trace (`log stream`, `dtrace`) while reproducing a failed
hardware-decode attempt, then re-identify the fallback in a fresh disassembly.

## 8. Reproduction

```sh
python3 tools/gen_vda_gva_data.py          # derive, print evidence, rewrite NootedRed/VDAGVAData.hpp
python3 tools/gen_vda_gva_data.py --check  # re-derive from the live cache and verify the header
```

`--check` output on this machine (abridged):

```text
OS: macOS 15.8.1 (24H32)
dyld cache: UUID 42EC2890BC40317EBB2745142BFF7F26, base 0x7ff800000000
AppleGVA: 113.56, image UUID 53DD765D087D322EA410BD22D586C08C, address 0x7ffa0666e000
target function: 0x7ffa06692198..0x7ffa06692840 (size 0x6a8)
patch site: 0x7ffa0669241b (+0x283), page 0x7ffa06692000 offset 0x41b
  branch: je 0x7ffa066927f7 (disp +0x3d6) -> je 0x7ffa0669244b (disp +0x2a), AMD tail 0x7ffa0669244b
  find:    0f 84 d6 03 00 00
  replace: 0f 84 2a 00 00 00
  page:    d8a48edff484b8fe0b3a3d429e08f264f00fcb9f4e68304b9a87e5ad20eca474
  source:  dyld_shared_cache_x86_64h.02 file offset 0x6692000
OK: NootedRed/VDAGVAData.hpp matches the live cache
```

## 9. References

- Inputs: `/System/Volumes/Preboot/Cryptexes/OS/System/Library/dyld/dyld_shared_cache_x86_64h*`
  (read-only), `/System/Library/PrivateFrameworks/AppleGVA.framework/Versions/A/Resources/Info.plist`,
  `/System/Library/CoreServices/SystemVersion.plist`, `ioreg -l -w0`.
- Tools: `/Library/Developer/CommandLineTools/usr/bin/llvm-objdump`, `llvm-nm`, plus our own cache
  reader in `tools/gen_vda_gva_data.py`; earlier scratch readers `/tmp/cache-inspect.py`,
  `/tmp/extract-image.py`.
- Current tree: [`docs/VDA-Implementation-Plan.md`](../docs/VDA-Implementation-Plan.md) (Phase 4),
  [`analysis/vcn-steve12311.md`](vcn-steve12311.md) §4/§7 (fork semantics, labelled **[C]**),
  [`analysis/vcn-approaches.md`](vcn-approaches.md) §3.1/§4.4,
  [`analysis/lilu-userpatch.md`](lilu-userpatch.md) §4 (page-patch backend),
  [`NootedRed/UserPatches.hpp`](../NootedRed/UserPatches.hpp),
  [`NootedRed/VDAGVAData.hpp`](../NootedRed/VDAGVAData.hpp).
