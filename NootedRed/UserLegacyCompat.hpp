// Sonoma/Sequoia 的精确缓存配置；完整页面只参与核验，写入仍限制在补丁范围内。
#pragma once
#include <UserSharedCache.hpp>

namespace UserLegacyCompat
{

    struct Patch
    {
        UInt64       offset;
        const UInt8* original;
        const UInt8* patched;
        UInt32       size;
    };
    struct Page
    {
        UInt64       offset;
        const UInt8* original;
    };
    struct Profile
    {
        UInt32       major;
        const UInt8* uuid;
        const Page*  pages;
        UInt32       pageCount;
        const Patch* patches;
        UInt32       patchCount;
    };

    inline bool matches(UserSharedCache::Reader reader, UInt64 base, const UInt8* uuid, const Profile& profile)
    {
        if (!reader || !uuid || memcmp(uuid, profile.uuid, 16) != 0 || profile.pageCount == 0 || profile.pageCount > 16
            || profile.patchCount == 0 || profile.patchCount > 32)
        {
            return false;
        }
        UInt8 bytes[256];
        for (UInt32 i = 0; i < profile.pageCount; ++i) {
            const auto& page = profile.pages[i];
            if (base >= UserSharedCache::UserLimit || (base & 4095) != 0 || (page.offset & 4095) != 0
                || page.offset >= UserSharedCache::UserLimit - base
                || 4096 > UserSharedCache::UserLimit - base - page.offset)
            {
                return false;
            }
            for (UInt32 offset = 0; offset < 4096; offset += sizeof(bytes)) {
                if (reader(base + page.offset + offset, bytes, sizeof(bytes)) != 0) { return false; }
                for (UInt32 j = 0; j < sizeof(bytes); ++j) {
                    const auto position = page.offset + offset + j;
                    auto       expected = page.original[offset + j];
                    if (bytes[j] == expected) { continue; }
                    bool accepted = false;
                    for (UInt32 k = 0; k < profile.patchCount; ++k) {
                        const auto& patch = profile.patches[k];
                        if (position >= patch.offset && position - patch.offset < patch.size
                            && bytes[j] == patch.patched[position - patch.offset])
                        {
                            accepted = true;
                            break;
                        }
                    }
                    if (!accepted) { return false; }
                }
            }
        }
        return true;
    }

}    // namespace UserLegacyCompat
