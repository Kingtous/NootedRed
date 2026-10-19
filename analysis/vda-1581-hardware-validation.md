# VDA/VCN2 hardware decode — macOS 15.8.1 Renoir 0x1638 validation

Status: **hardware validation passed** on 2026-10-19 for the Option A bridge in
`feature/vda-hardware-decode`. This document records the evidence and the remaining caveats.

## Environment

| item | value |
| --- | --- |
| macOS / Darwin | 15.8.1 (24H32) / Darwin 24.6.0 |
| iGPU | Renoir `0x1638`, revision C1 (Ryzen 7 5800U class) |
| boot args | `keepsyms=1 debug=0x144 itlwm_he=1 -NRedGLFix -NRedVDA` |
| NootedRed | release build, loaded UUID `111D01CB-A485-3EDD-9594-DF8944C41D7E`, EFI kext SHA-256 `457d02f601680e35b1a2b9ac4ef233ec815b6c5415a478cdf866727679e9209b` |
| AMDRadeonX6000 | loaded, `7.0.0`, UUID `4F6A4BF7-2365-3D73-8001-ADA280ABA018` |
| generated data | `gen_vcn_kernel_data.py`, `gen_vda_va_data.py`, `gen_vda_gva_data.py` all `--check` pass |

## Kernel / user-space markers

`ioreg -l -w0 -r -n IGPU` after the test:

```text
NRedVDARequested = 1
NRedVDAMediaEnabled = 1
NRedVCNGeneration = 514 (0x0202)
NRedVCNStage = 11
NRedVCNX5000Seen = 1
NRedVCNX6000Seen = 1
NRedVCNAccelStartSeen = 1
NRedVCNAcceleratorStarted = 1
NRedVCNX5000ABIValidated = 1
NRedVCNX6000ABIValidated = 1
NRedVCNNativeLoadStatus = 0
NRedVCNNativeLoadReady = 1
NRedVCNEngineAllocated = 1
NRedVCNLastHardwareInit = 1
NRedVCNUserApplied = 4
```

This means: both 15.8.1 drivers passed the generated ABI guards, X6000 was loaded by the bridge, the
`AMDVCN2HWEngine` was allocated in the X5000 engine array, the accelerator start succeeded, VCN hardware
init succeeded, and the VA/AppleGVA user-space pages were patched.

## Decode tests

`tools/vda_hwtest.sh` generated 8-bit H.264 (720p and 64x64) and HEVC (720p) clips, decoded them with
VideoToolbox and with software, and compared the raw YUV output byte-for-byte:

```text
h264-720p:                         command OK, VideoToolbox, videotoolbox_vld, output matches software
h264-64x64 (known-adverse):        command OK, VideoToolbox, videotoolbox_vld, output matches software
hevc-720p:                         command OK, VideoToolbox, output matches software
```

`VTDecoderXPCService` logs show the AppleGVA hardware decoder path:

```text
(AppleGVA) [HEVC] : Decoded: 1, useHighPerformanceDecode = 1
(AppleGVA) [HEVC] GVA info: Successfully disposed the main physical accelerator
(AppleGVA) [HEVC] GVA info: Successfully disposed the main virtual accelerator
```

Additional stress run (outside `vda_hwtest.sh`):

- generated 1080p H.264 and HEVC clips (software encode);
- 5 consecutive hardware decodes of each (10 decoder create/destroy cycles);
- every hardware YUV output matched the software decode byte-for-byte;
- no `.gpuRestart` report or kernel panic in the boot log; the only log matches for "watchdog" were
  unrelated RunningBoard/WebKit assertions (`XPCConnectionTerminationWatchdog`), not WindowServer
  watchdogs.

## Idle test

Five minutes of idle followed by a 1080p H.264 hardware decode **passed**: the hardware YUV output matched
the software decode byte-for-byte (`idle_decode_match=0`). This covers the "idle then start" acceptance
step; a repeated long-duration playback test was not run.

## Caveats / not yet validated

- HEVC ffmpeg verbose logs do not expose `videotoolbox_vld`; hardware evidence there is the AppleGVA
  `useHighPerformanceDecode`/physical-accelerator log, not the ffmpeg pixel format line.
- No Chromium/Electron or Safari playback test was run; only ffmpeg/VideoToolbox decode paths.
- No 10-bit HEVC, DRM/Apple TV, or encode acceptance was run at the time of the first pass. A later
  follow-up found that VideoToolbox hardware *encode* fails (`h264_videotoolbox`/`hevc_videotoolbox`
  return -12704) and that VideoProc's hardware-info dialog consequently reports both codecs as unavailable.
  Encode feedback-stride patches were added in a follow-up build (`cb98ebac…`) and are pending a reboot
  test; decode was not changed by them.
- The default-off path (`-NRedVDA` absent) with the final build has not been booted yet; the new code is
  gated at compile/run time and the earlier default boot before the final build was clean.
- Only Renoir `0x1638` is enabled; other VCN 2.2 APUs and VCN 1.x remain unsupported.
- Long-duration/desktop-interactivity and Hackintool create/destroy tests remain for a follow-up pass.
