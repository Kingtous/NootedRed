// Bounded VCN idle observation with the native driver's scoped GFXOFF pairing.
#pragma once
#include <IOKit/IOTypes.h>

namespace VCNDrain
{
    using Query = bool (*)(void*);
    using GfxControl = void (*)(void*);
    using NativeWait = void (*)(void*, UInt32);
    using Clock = UInt64 (*)(); // Monotonic nanoseconds, supplied by the kernel caller.
    using Sleep = void (*)(UInt32);

    enum class Outcome : UInt32 { Idle = 0, TimedOut = 1, NativeUnverified = 2, Unavailable = 3 };
    struct Result
    {
        Outcome outcome{Outcome::Unavailable};
        UInt32 elapsedMs{0};
        UInt32 requestedSleepMs{0};
        bool gfxGuardUsed{false};
    };

    class ScopedGfxAccess
    {
        void* hardware;
        GfxControl enable;
    public:
        ScopedGfxAccess(void* target, GfxControl disable, GfxControl restore) : hardware(target), enable(restore)
        {
            disable(hardware);
        }
        ~ScopedGfxAccess() { enable(hardware); }
        ScopedGfxAccess(const ScopedGfxAccess&) = delete;
        ScopedGfxAccess& operator=(const ScopedGfxAccess&) = delete;
    };

    inline UInt32 milliseconds(UInt64 ns)
    {
        const auto ms = ns / 1000000ULL;
        return ms > 0xFFFFFFFFULL ? 0xFFFFFFFFU : static_cast<UInt32>(ms);
    }

    inline Result wait(void* engine, void* hardware, Query query, GfxControl disable, GfxControl enable,
                       NativeWait nativeWait, Clock now, Sleep sleep, UInt32 timeoutMs, UInt32 stepMs)
    {
        Result result;
        if (engine == nullptr || now == nullptr || sleep == nullptr || stepMs == 0) { return result; }
        const auto start = now();
        if (query != nullptr && hardware != nullptr && disable != nullptr && enable != nullptr) {
            {
                // Pair exactly once and restore before reporting/IORegistry updates. Never hook enableGfxOff.
                ScopedGfxAccess guard(hardware, disable, enable);
                result.gfxGuardUsed = true;
                result.outcome = Outcome::TimedOut;
                while (true) {
                    const auto elapsed = milliseconds(now() - start);
                    if (elapsed >= timeoutMs) { break; }
                    if (query(engine)) { result.outcome = Outcome::Idle; break; }
                    const auto afterQuery = milliseconds(now() - start);
                    if (afterQuery >= timeoutMs) { break; }
                    const auto remaining = timeoutMs - afterQuery;
                    const auto delay = remaining < stepMs ? remaining : stepMs;
                    sleep(delay);
                    result.requestedSleepMs += delay;
                }
            }
        } else if (nativeWait != nullptr) {
            // Its return type is void. Returning cannot prove that this shared engine became idle.
            // The existing native method owns its own GFXOFF pair; do not nest our guard around it.
            nativeWait(engine, timeoutMs);
            result.outcome = Outcome::NativeUnverified;
        }
        result.elapsedMs = milliseconds(now() - start);
        return result;
    }
}
