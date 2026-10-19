// User-Space Driver Patches
//
// Copyright (C) 2026  ChefKiss.
//
// Licensed under the ChefKiss Open Knowledge License version 1.
//
// This work is licensed to you under the terms of the ChefKiss Open Knowledge
// License, either version 1 of the License, or (at your option) any later
// version published by ChefKiss.
//
// This work may not be used to train or develop artificial intelligence,
// and may not be used with artificial intelligence in any other way. See
// the ChefKiss Open Knowledge License for details.
//
// This work is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// ChefKiss Open Knowledge License for more details.
//
// You should have received a copy of the ChefKiss Open Knowledge License
// along with this work.  If not, see https://git.chefkiss.dev/ChefKiss/OpenKnowledge/raw/branch/master/LICENSE-v1.md.

#include <Headers/kern_api.hpp>
#include <Headers/kern_mach.hpp>
#include <Headers/kern_util.hpp>
#include <DeviceProfile.hpp>
#include <PenguinWizardry/KernelVersion.hpp>
#include <UserPatches.hpp>
#include <libkern/OSTypes.h>
#include <mach/i386/vm_types.h>

static UserPatches moduleInstance;

namespace
{

    const char* const kBootArgGLFix    = "-NRedGLFix";
    const char* const kBootArgGLFixOff = "-NRedGLFixOff";

    // The patch bytes and page images below are specific to one driver build, and the native backend hooks
    // code signing page validation, so this is opt-in until the table has been filled in and tested.
    constexpr bool kGLFixDefaultEnabled = false;

    // Arbitrary non-zero section id for `UserPatcher::BinaryModPatch`; Lilu's ABI treats zero as disabled.
    constexpr UInt32 kLiluPatchSection = 1;

    // Exact patches for
    // `/System/Library/Extensions/AMDRadeonX5000GLDriver.bundle/Contents/MacOS/AMDRadeonX5000GLDriver`.
    //
    // TODO: Populate this table from analysis/gl-hwinfo.md, or call `UserPatches::singleton().registerGLDriverFix`
    // with a table in `NRed::init`.  Each entry needs:
    //  - `find`/`replace`/`size`: the bytes to replace (both backends),
    //  - `skip`/`count`: which occurrences to replace; `count` must be non-zero (both backends),
    //  - `segment`: the Mach segment/section holding the patch (Lilu backend only, `SegmentTextText` for `__text`),
    //  - `page`: the exact 4 KiB file page containing the first `find` (native backend only, macOS 11 and newer).
    //
    // A page image can be extracted with, for example:
    //  - `dd if="$kGLDriverPath" of=page.bin bs=4096 skip=$((0xdf000 / 0x1000)) count=1` and then `xxd -i page.bin`.
    //
    // Common GL Driver patch patterns (shared across macOS 14, 15, 26)
    // A1: `_gfx9_HwInfoInitData` hardcodes four shader engines in the Vega10 branch. Renoir has one
    // shader engine, so `rec+0x64 = rec+0x60 * rec+0x78 * rec+0x80` becomes 32 instead of 8 compute
    // units. Site: file offset 0xE081 = page 0xE000 + 0x81.
    static const UInt8 kGLDriverHwInfoFind[]    = {0xC7, 0x47, 0x78, 0x04, 0x00, 0x00, 0x00};
    static const UInt8 kGLDriverHwInfoReplace[] = {0xC7, 0x47, 0x78, 0x01, 0x00, 0x00, 0x00};

    // A2: `_glrAMD_GFX9_UpdateHwCbDualQuadMode` must set `CB_COLOR_CONTROL.DISABLE_DUAL_QUAD` on
    // the iGPU. The original code computes the bit from `rec+0xA0` bit 1, which is set only for the
    // Vega12 branch (0x4F); Renoir takes the Vega10 branch (0x4D), so dual-source blending drops the
    // draw or resets the GPU. Force `or eax, 1` instead of `and eax, ~1`.
    // Site: file offset 0x8160 = page 0x8000 + 0x160; the same 3-byte sequence also occurs at
    // page offset 0x68, hence skip = 1.
    static const UInt8 kGLDriverDualQuadFind[]    = {0x83, 0xE0, 0xFE};
    static const UInt8 kGLDriverDualQuadReplace[] = {0x83, 0xC8, 0x01};

    // Metal counterpart: Gate pattern (jnz -> jmp) for macOS 14 & 15
    static const UInt8 kMTLGateFind[]    = {0x75, 0x0B};
    static const UInt8 kMTLGateReplace[] = {0xEB, 0x0B};

    // macOS 14 (Sonoma) Allow pattern: 0F 93 C0 (setae al) -> 31 C0 90 (xor eax,eax; nop)
    static const UInt8 kMTLAllowFind_Sonoma[]    = {0x0F, 0x93, 0xC0};
    static const UInt8 kMTLAllowReplace_Sonoma[] = {0x31, 0xC0, 0x90};

    // macOS 15 (Sequoia) Allow pattern: 0F 94 C0 (setz al) -> 31 C0 90 (xor eax,eax; nop)
    static const UInt8 kMTLAllowFind_Sequoia[]    = {0x0F, 0x94, 0xC0};
    static const UInt8 kMTLAllowReplace_Sequoia[] = {0x31, 0xC0, 0x90};

    // macOS 26 (Tahoe) Gate: 74 28 (je +0x28) -> EB 00 (jmp +0x00, fallthrough / no-op jump)
    static const UInt8 kMTLGateFind_Tahoe[]    = {0x74, 0x28};
    static const UInt8 kMTLGateReplace_Tahoe[] = {0xEB, 0x00};

    // macOS 26 (Tahoe) Allow pattern: 0F 94 C1 (sete cl) -> 31 C9 90 (xor ecx,ecx; nop)
    static const UInt8 kMTLAllowFind_Tahoe[]    = {0x0F, 0x94, 0xC1};
    static const UInt8 kMTLAllowReplace_Tahoe[] = {0x31, 0xC9, 0x90};

    #include "UserPatchesSonoma.inc"
    #include "UserPatchesSequoia.inc"
    #include "UserPatchesTahoe.inc"

    static const UserPatches::PagePatch kGLDriverPatches_Sonoma[] = {
        {UserPatcher::FileSegment::SegmentTextText, kGLDriverHwInfoPage_Sonoma,    kGLDriverHwInfoFind,
         kGLDriverHwInfoReplace,                    sizeof(kGLDriverHwInfoFind),   0, 1},
        {UserPatcher::FileSegment::SegmentTextText, kGLDriverDualQuadPage_Sonoma,  kGLDriverDualQuadFind,
         kGLDriverDualQuadReplace,                  sizeof(kGLDriverDualQuadFind), 1, 1},
        {UserPatcher::FileSegment::SegmentTextText, kMTLControlPage_Sonoma,        kMTLGateFind,
         kMTLGateReplace,                           sizeof(kMTLGateFind),          0, 1},
        {UserPatcher::FileSegment::SegmentTextText, kMTLControlPage_Sonoma,        kMTLAllowFind_Sonoma,
         kMTLAllowReplace_Sonoma,                   sizeof(kMTLAllowFind_Sonoma),  0, 1},
    };
    constexpr size_t kGLDriverPatchCount_Sonoma = arrsize(kGLDriverPatches_Sonoma);

    static const UserPatches::PagePatch kGLDriverPatches_Sequoia[] = {
        {UserPatcher::FileSegment::SegmentTextText, kGLDriverHwInfoPage_Sequoia,    kGLDriverHwInfoFind,
         kGLDriverHwInfoReplace,                    sizeof(kGLDriverHwInfoFind),   0, 1},
        {UserPatcher::FileSegment::SegmentTextText, kGLDriverDualQuadPage_Sequoia,  kGLDriverDualQuadFind,
         kGLDriverDualQuadReplace,                  sizeof(kGLDriverDualQuadFind), 1, 1},
        {UserPatcher::FileSegment::SegmentTextText, kMTLControlPage_Sequoia,        kMTLGateFind,
         kMTLGateReplace,                           sizeof(kMTLGateFind),          0, 1},
        {UserPatcher::FileSegment::SegmentTextText, kMTLControlPage_Sequoia,        kMTLAllowFind_Sequoia,
         kMTLAllowReplace_Sequoia,                  sizeof(kMTLAllowFind_Sequoia), 0, 1},
    };
    constexpr size_t kGLDriverPatchCount_Sequoia = arrsize(kGLDriverPatches_Sequoia);

    static const UserPatches::PagePatch kGLDriverPatches_Tahoe[] = {
        {UserPatcher::FileSegment::SegmentTextText, kGLDriverHwInfoPage_Tahoe,    kGLDriverHwInfoFind,
         kGLDriverHwInfoReplace,                    sizeof(kGLDriverHwInfoFind),   0, 1},
        {UserPatcher::FileSegment::SegmentTextText, kGLDriverDualQuadPage_Tahoe,  kGLDriverDualQuadFind,
         kGLDriverDualQuadReplace,                  sizeof(kGLDriverDualQuadFind), 1, 1},
        {UserPatcher::FileSegment::SegmentTextText, kMTLControlPage_Tahoe,        kMTLGateFind_Tahoe,
         kMTLGateReplace_Tahoe,                     sizeof(kMTLGateFind_Tahoe),    0, 1},
        {UserPatcher::FileSegment::SegmentTextText, kMTLControlPage_Tahoe,        kMTLAllowFind_Tahoe,
         kMTLAllowReplace_Tahoe,                    sizeof(kMTLAllowFind_Tahoe),   1, 1},
    };
    constexpr size_t kGLDriverPatchCount_Tahoe = arrsize(kGLDriverPatches_Tahoe);

}    // namespace

UserPatches& UserPatches::singleton() { return moduleInstance; }

void UserPatches::init()
{
    // `registerGLDriverFix` may already have been called; do not overwrite a custom table then.
    if (this->patches == nullptr && this->patchCount == 0) {
        if (getKernelVersion() >= KernelVersion::Tahoe) {
            SYSLOG("UserPatch", "Selecting macOS 26 (Tahoe) GL/Metal patch set");
            this->patches    = kGLDriverPatches_Tahoe;
            this->patchCount = kGLDriverPatchCount_Tahoe;
        } else if (getKernelVersion() >= KernelVersion::Sequoia) {
            SYSLOG("UserPatch", "Selecting macOS 15 (Sequoia) GL/Metal patch set");
            this->patches    = kGLDriverPatches_Sequoia;
            this->patchCount = kGLDriverPatchCount_Sequoia;
        } else {
            SYSLOG("UserPatch", "Selecting macOS 14 (Sonoma) GL/Metal patch set");
            this->patches    = kGLDriverPatches_Sonoma;
            this->patchCount = kGLDriverPatchCount_Sonoma;
        }
    }

    this->enabled = kGLFixDefaultEnabled;
    if (checkKernelArgument(kBootArgGLFix)) { this->enabled = true; }
    if (checkKernelArgument(kBootArgGLFixOff)) { this->enabled = false; }

    if (!this->enabled) {
        DBGLOG("UserPatch", "User-space GL driver patches are disabled");
        return;
    }

    SYSLOG("UserPatch", "Enabling user-space GL driver patches (experimental)");

    const auto error =
        lilu.onPatcherLoad([](void* const, KernelPatcher& patcher) { singleton().processPatcher(patcher); }, nullptr);
    SYSLOG_COND(error != LiluAPI::Error::NoError, "UserPatch", "Failed to register patcher load callback (%d)",
                static_cast<int>(error));
}

void UserPatches::registerGLDriverFix(const PagePatch* const patchTable, const size_t count)
{
    this->patches    = patchTable;
    this->patchCount = count;
}

void UserPatches::processPatcher(KernelPatcher& patcher)
{
    const auto* const profile = DeviceProfile::current();
    if (profile != nullptr) {
        SYSLOG("UserPatch", "Device profile: %s (family %s)", profile->name,
               DeviceProfile::familyName(profile->family));
        if (!profile->graphics.conditionalDualQuad) {
            SYSLOG("UserPatch", "Graphics policy disables the conditional dual-quad fix for this profile");
            return;
        }
    }

    if (this->patches == nullptr || this->patchCount == 0) {
        SYSLOG("UserPatch", "No user-space GL driver patches are configured, not installing any hooks");
        return;
    }

    if (currentKernelVersion() >= MACOS_11) {
        SYSLOG_COND(!this->installNativePagePatcher(patcher), "UserPatch",
                    "Failed to install the native page patch backend");
    }
    else {
        SYSLOG_COND(!this->installUserPatcher(), "UserPatch", "Failed to register Lilu's user patches");
    }
}

auto UserPatches::installUserPatcher() -> bool
{
    this->liluPatchCount = 0;

    for (size_t patchIndex = 0; patchIndex < this->patchCount; patchIndex++) {
        const auto& patch = this->patches[patchIndex];

        if (patch.find == nullptr || patch.replace == nullptr || patch.size == 0 || patch.count == 0) { continue; }

        if (this->liluPatchCount >= kMaxLiluPatches) {
            SYSLOG("UserPatch", "Too many patches, only the first %zu are used", kMaxLiluPatches);
            break;
        }

        auto& liluPatch   = this->liluPatches[this->liluPatchCount++];
        liluPatch.cpu     = CPU_TYPE_X86_64;
        liluPatch.flags   = 0;
        liluPatch.find    = patch.find;
        liluPatch.replace = patch.replace;
        liluPatch.size    = patch.size;
        liluPatch.skip    = patch.skip;
        liluPatch.count   = patch.count;
        liluPatch.segment = patch.segment;
        liluPatch.section = kLiluPatchSection;
    }

    if (this->liluPatchCount == 0) {
        SYSLOG("UserPatch", "No valid GL driver patches for Lilu's user patcher");
        return false;
    }

    this->liluModInfo.path      = kGLDriverPath;
    this->liluModInfo.patches   = this->liluPatches;
    this->liluModInfo.count     = this->liluPatchCount;
    this->liluModInfo.startTEXT = 0;
    this->liluModInfo.endTEXT   = 0;
    this->liluModInfo.startDATA = 0;
    this->liluModInfo.endDATA   = 0;

    // No process filter is registered: the patch is a page-content patch, and Lilu applies those from the
    // code signing validation hooks for every image that matches, while a process filter would additionally
    // inject the `__RESTRICT` segment into that process (see analysis/lilu-userpatch.md).  Lilu tolerates a
    // zero-sized process list: `onPath` only iterates over `procInfoSize` entries, and `registerPatches` only
    // derives its minimum path length from the list when it is not empty.
    const auto error = lilu.onProcLoad(nullptr, 0, nullptr, nullptr, &this->liluModInfo, 1);
    if (error != LiluAPI::Error::NoError) {
        SYSLOG("UserPatch", "Failed to register user patches with Lilu (%d)", static_cast<int>(error));
        return false;
    }

    DBGLOG("UserPatch", "Registered %zu page patch(es) for %s", this->liluPatchCount, kGLDriverPath);

    return true;
}

auto UserPatches::installNativePagePatcher(KernelPatcher& patcher) -> bool
{
    if (!this->buildNativePatches()) { return false; }

    KernelPatcher::RouteRequest pageRequest{"_cs_validate_page", wrapCsValidatePage, this->orgCsValidatePage};
    if (!patcher.routeMultipleLong(KernelPatcher::KernelID, &pageRequest, 1)) {
        SYSLOG("UserPatch", "Failed to route _cs_validate_page (%d)", static_cast<int>(patcher.getError()));
        patcher.clearError();
        return false;
    }

    // The dyld shared cache and Apple protected pagers validate through the range variant.  The GL driver is
    // not part of the shared cache, so this is only registered when available and never treated as fatal.
    KernelPatcher::RouteRequest rangeRequest{"_cs_validate_range", wrapCsValidateRange, this->orgCsValidateRange};
    if (!patcher.routeMultipleLong(KernelPatcher::KernelID, &rangeRequest, 1)) {
        DBGLOG("UserPatch", "Failed to route _cs_validate_range, continuing without it (%d)",
               static_cast<int>(patcher.getError()));
        patcher.clearError();
    }

    DBGLOG("UserPatch", "Installed the native page patch backend for %zu page(s)", this->nativePageCount);

    return true;
}

auto UserPatches::buildNativePatches() -> bool
{
    this->nativePageCount = 0;

    for (size_t patchIndex = 0; patchIndex < this->patchCount; patchIndex++) {
        const auto& patch = this->patches[patchIndex];

        if (patch.find == nullptr || patch.replace == nullptr || patch.size == 0 || patch.count == 0) { continue; }

        if (patch.size > kMaxPatchSize) {
            SYSLOG("UserPatch", "Patch %zu is larger than %zu bytes, skipping it", patchIndex, kMaxPatchSize);
            continue;
        }

        if (patch.page == nullptr) {
            SYSLOG("UserPatch", "Patch %zu has no page image, the native backend cannot apply it", patchIndex);
            continue;
        }

        NativePage* page{nullptr};
        for (size_t pageIndex = 0; pageIndex < this->nativePageCount; pageIndex++) {
            if (this->nativePages[pageIndex].image != patch.page) { continue; }
            page = &this->nativePages[pageIndex];
            break;
        }

        if (page == nullptr) {
            if (this->nativePageCount >= kMaxNativePages) {
                SYSLOG("UserPatch", "Too many distinct pages, only the first %zu are used", kMaxNativePages);
                break;
            }

            page        = &this->nativePages[this->nativePageCount++];
            page->image = patch.page;
        }

        auto skip  = patch.skip;
        auto count = patch.count;

        for (size_t offset = 0; offset + patch.size <= kPageSize; offset++) {
            if (memcmp(patch.page + offset, patch.find, patch.size) != 0) { continue; }

            if (skip != 0) {
                skip--;
                continue;
            }

            if (page->siteCount >= kMaxSitesPerPage) {
                SYSLOG("UserPatch", "Too many patch sites on a single page, only the first %zu are used",
                       kMaxSitesPerPage);
                break;
            }

            auto& site   = page->sites[page->siteCount++];
            site.offset  = offset;
            site.find    = patch.find;
            site.replace = patch.replace;
            site.size    = patch.size;

            if (--count == 0) { break; }
        }

        SYSLOG_COND(count != 0, "UserPatch", "Failed to locate all occurrences of patch %zu (%zu missing)", patchIndex,
                    count);
    }

    if (this->nativePageCount == 0) {
        SYSLOG("UserPatch", "No usable native page patches, each patch needs a %zu byte page image", kPageSize);
        return false;
    }

    return true;
}

void UserPatches::patchNativePage(const void* const data)
{
    if (data == nullptr) { return; }

    for (size_t pageIndex = 0; pageIndex < this->nativePageCount; pageIndex++) {
        const auto& page = this->nativePages[pageIndex];

        if (memcmp(page.image, data, kPageSize) != 0) { continue; }

        // The page matches, so these checks can only fail when the table is inconsistent; never patch then.
        for (size_t siteIndex = 0; siteIndex < page.siteCount; siteIndex++) {
            const auto& site = page.sites[siteIndex];
            if (memcmp(static_cast<const UInt8*>(data) + site.offset, site.find, site.size) != 0) {
                SYSLOG("UserPatch", "Page matched but a patch site did not, skipping the page");
                return;
            }
        }

        if (MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) != KERN_SUCCESS) {
            SYSLOG("UserPatch", "Failed to disable kernel write protection");
            return;
        }

        for (size_t siteIndex = 0; siteIndex < page.siteCount; siteIndex++) {
            const auto& site = page.sites[siteIndex];
            memcpy(const_cast<UInt8*>(static_cast<const UInt8*>(data)) + site.offset, site.replace, site.size);
        }

        if (MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock) != KERN_SUCCESS) {
            SYSLOG("UserPatch", "Failed to restore kernel write protection");
        }

        DBGLOG("UserPatch", "Patched %zu site(s) in a user-space GL driver page", page.siteCount);

        return;
    }
}

void UserPatches::wrapCsValidatePage(void* const vnode, void* const pager, const UInt64 pageOffset,
                                     const void* const data, int* const validated, int* const tainted, int* const nx)
{
    FunctionCast(wrapCsValidatePage, singleton().orgCsValidatePage)(vnode, pager, pageOffset, data, validated, tainted,
                                                                    nx);

    // Only patch pages that passed validation, so that a rejected page is never turned into a valid-looking one.
    if (validated != nullptr && *validated != 0) { singleton().patchNativePage(data); }
}

boolean_t UserPatches::wrapCsValidateRange(void* const vnode, void* const pager, const UInt64 pageOffset,
                                           const void* const data, const UInt64 dataSize, unsigned* const tainted)
{
    const auto result = FunctionCast(wrapCsValidateRange, singleton().orgCsValidateRange)(vnode, pager, pageOffset,
                                                                                          data, dataSize, tainted);

    if (result == 0 || data == nullptr) { return result; }

    const auto* const bytes = static_cast<const UInt8*>(data);
    for (size_t offset = 0; offset + kPageSize <= dataSize; offset += kPageSize) {
        singleton().patchNativePage(bytes + offset);
    }

    return result;
}
