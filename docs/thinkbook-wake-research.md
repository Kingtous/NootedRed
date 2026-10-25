# ThinkBook 14p video / lid wake research

Reproduction reported by the device owner: Tahoe 26.7.1 (25G241), Ryzen 7 5800H,
Renoir 1002:1638, Microsoft Edge foreground video playback; close lid and reopen
after approximately three seconds. Display and pointer freeze while audio
continues, followed by black/green-line output and forced shutdown.

This does not establish that full system sleep completed. Compare pmset's
Sleep/Wake/Display events before calling it a system resume failure.

## Source findings

v1.0.0 routes X5000 enableGfxOff to a no-op when `-NRedVCNNoGfxOff` is enabled;
the hook is global and its hold state is not reset on a power cycle in this
module. This makes it a candidate for a power-transition interaction, not a
proven cause. First compare the same release with only this argument removed,
retaining `-NRedGLFix -NRedVDA` and all other EFI settings.

Upstream commit `5a941329d2b5299c176ec38a7fc75e22a5b141f7` removes the hold and
adds VCN client/accelerator teardown draining plus HEVC Main 10 fallback policy.
It addresses real code paths relevant to video teardown, but is not a general
sleep/wake implementation or a guarantee for this machine.

## Research candidate

This branch starts at v1.0.0 and cherry-picks that upstream commit, preserving
its attribution. It adds bounded framebuffer receiver/controller power logs,
VCN initialization and stop/drain boundary logs, and
`NRedWakeResearchRevision=1`. `NRedVCNDrainBuild=2` identifies the upstream drain.
The framebuffer flags, offsets, original calls and 250-ms delay are unchanged.

The candidate also changes HEVC behavior through the upstream commit, so a
successful candidate test cannot be attributed only to GFXOFF removal or drain.
Use the unchanged-release parameter comparison first.

The production drain is exercised with simulated idle, delayed, repeated
timeout and missing-engine/query cases. The upstream polling loop can spend
255 ms when continuously busy (250-ms limit plus a final 5-ms polling step);
tests reflect that behavior. No GPU is exercised by these tests. Native
waitForIdle's internal timeout semantics remain an assumption of the upstream
implementation, not verified here.

## Build and acceptance

Wake Research builds Release/Debug with dSYMs on an Intel macOS runner, using
pinned Lilu/SDK revisions. The candidate reports version 1.0.0 because it is a
research backport; identify it by commit, binary UUID and the research marker.
Only `-NRedGLFix -NRedVDA` is needed for comparison. Do not carry the retired
`-NRedVCNNoGfxOff` argument into this candidate or disable the drain.

Test the same video and three-second lid cycle, then an idle desktop cycle.
Record browser version, video codec and power source. If the graphics freezes,
record whether audio responds and whether the keyboard/network remains usable.
Collect the pre-test stream, pmset logs, gpuRestart/panic reports and matching
NRed properties. Logs captured after a reboot describe the new boot unless
their timestamps explicitly belong to the failed test. No device fix is
claimed until the owner tests the candidate.
