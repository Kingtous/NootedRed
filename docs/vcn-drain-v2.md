# VCN drain v2: scoped power access and honest wait outcomes

The first wake research build (`bef765a`) resumed short lid cycles, but reported
repeated `client stop` waits with `idle=0`. The old 255-ms value was a sum of
requested IOSleep durations; it was not a measurement of elapsed time. The
native driver's waitForIdle code brackets its queries with disableGfxOff and
enableGfxOff (see analysis/vcn-1581-kernel-data.md, section 4.4). The new direct
polling path had no equivalent pair. This is a concrete implementation gap,
not proof that it caused every timeout on this machine.

The candidate resolves the X5000 owner's two named GFXOFF methods and calls
them in a scoped RAII guard around direct isIdle polling. It does not route or
replace either method, does not reinstate the global NoGfxOff behavior, and
restores normal power management before log/IORegistry reporting. If the pair
or hardware is unavailable, it avoids direct unprotected MMIO queries and can
use the existing native wait method, which owns its own pairing.

Polling uses kernel monotonic nanoseconds rather than adding requested sleeps.
When the deadline is reached it stops issuing further polls/sleeps. It cannot
preempt an already running driver method, a scheduling delay or a power-control
call, so this is not a hard real-time 250-ms guarantee. Reported elapsed time
includes those calls; requested sleep time is published separately.

The native fallback has void return type and cannot certify an idle engine.
It is now recorded as unverified, rather than a successful drain. No success
is fabricated and no remaining busy condition is hidden.

## Runtime evidence

- `NRedWakeResearchRevision=2`, `NRedVCNDrainWaitRevision=2`: candidate identity.
- `NRedVCNIdleLastOutcome`: 0 observed idle, 1 deadline reached, 2 native wait
  returned but idle unverified, 3 missing observation path.
- `NRedVCNIdleGfxGuard`: whether the direct poll used the scoped pair.
- `NRedVCNIdleWaitedMs` / `...MaxMs`: actual monotonic elapsed milliseconds.
- `NRedVCNIdleRequestedSleepMs`: requested polling sleep total.
- `NRedVCNIdleUnverified` / `...Unavailable`: separate non-success counters.
- Existing `NRedVCNIdleTimeouts` continues to count actual polling timeouts.

The original global engine-idle criterion is retained. Other decoder clients
can keep that engine busy; v2 does not implement per-client fences or prove
that all of a departing client's submissions have completed. The existing
engine/object lifetime and stop serialization assumptions are unchanged.
These limitations need device/binary investigation before claiming a complete
buffer-lifetime or concurrency fix.

## Verification and acceptance

The production bridge and policy tests check normal/delayed idle, persistent
busy conditions, scheduler oversleep, slow setup/query calls, every guarded
exit, missing symbol pairs, disabled/missing engines and unverified fallback.
Simulation does not exercise the GPU. The Release and Debug workflow builds
are separate from real-device acceptance.

Keep the known-good `bef765a` kext/config backup. For a candidate test, retain
`-NRedGLFix -NRedVDA`, change only the kext, and run the same sleep-wake capture.
Compare outcomes and actual elapsed time with video paused versus playing,
including multiple decoder clients. If no idle is observed despite a valid
guard, do not simply lengthen the wait or call the timeout resolved.
