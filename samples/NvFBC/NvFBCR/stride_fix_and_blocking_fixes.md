# NvOFA FRUC Mode: Stride Fix & Lower-Half Blocking Investigation

## Context

Branch: `garrett-nvofa-optical-flow-interpolation`
Base commit: `00eca167f843bb53eb81574976901dc9b5ae2b75`
Key files: `FrucCaptureMode.cpp`, `FrucCaptureMode.h`, `InterpolateKernel.cu`

## What We're Building

A real-time frame rate conversion (FRUC) capture mode for NvFBC screen capture that produces smooth, locked-framerate output (e.g. 60fps) from variable-rate input (>60fps) using NVIDIA Optical Flow (NvOFA) for motion-compensated interpolation.

### Pipeline
1. **Capture**: NvFBC grabs desktop frames into a 2-slot ring buffer, each timestamped with `QueryPerformanceCounter`
2. **Optical Flow**: NvOFA computes per-block motion vectors between the two most recent captures (`NV_OF_MODE_OPTICALFLOW`, `NV_OF_PERF_LEVEL_FAST`, grid size 4)
3. **Interpolation**: Custom CUDA kernel (`InterpolateKernel.cu`) does backward-warping with bilinear sampling of both the flow field and pixel data
4. **Timing**: A monotonically advancing `targetPresentTime` ticks at exactly `1/targetFramerate`. For each tick, `weight = (tickTime - t0) / (t1 - t0)` places the output precisely between the two bounding captured frames
5. **Present**: Interpolated frame goes through D3D9 output surface to backbuffer

## Issue 1: Horizontal Streaking (FIXED)

### Root Cause
NvOF output (flow vector) buffers have per-row padding. The interpolation kernel was accessing flow vectors with tight-packed indexing: `flowVectors[y * flowWidth + x]`. This caused each row to drift further out of alignment.

### Fix (in working tree diff, not yet committed)
- Changed kernel to accept `flowStrideBytes` parameter from `strideInfo.strideInfo[0].strideXInBytes`
- Added stride-aware accessor: `(FlowVector*)(flowData + y * flowStrideBytes)` then `row[x]`
- `FrucCaptureMode.cpp`: retrieves stride info from `NvOFBufferCudaDevicePtr::getStrideInfo()` and passes it to the kernel
- Files changed: `InterpolateKernel.cu` (kernel signature + flow access), `FrucCaptureMode.cpp` (forward decl + InterpolateFrame)

## Issue 2: Off-Color Blocking in Lower Half (UNDER INVESTIGATION)

### Symptoms
- Top ~60% of image renders correctly
- Bottom ~40% shows large rectangular off-color blocks
- Green bar becomes yellow-green, blue bar becomes lavender
- Block boundaries visible, suggesting flow-grid-aligned artifacts
- Very visible with RGB test pattern (`test_pattern_good.png`), much less noticeable in games
- Screenshot of issue: `stride_fix_bad_2.png` in `C:\Users\garrett\Pictures\Screenshots\`

### What Was Ruled Out
After thorough code review of the full pipeline:

1. **NvOF input upload** (`NvOFCuda.cpp:205-229`): `UploadData` correctly handles stride via `cuMemcpy2D` with `srcPitch = width * elementSize` (tight-packed host) and `dstPitch = strideInfo` (padded GPU buffer). NOT the issue.

2. **Channel swaps**: Both BGRA->RGBA (`CopyFrameToHost`) and RGBA->BGRA (output write-back) trace correctly for all channels. NOT the issue.

3. **CUDA frame buffers** (`m_cudaFrame0/1`): Tight-packed allocation, filled from same host buffers, accessed correctly in kernel. NOT the issue.

4. **D3D9 surface pitch handling**: Both `CopyFrameToHost` and output write-back correctly use `lockedRect.Pitch` for D3D9 surfaces. NOT the issue.

5. **Flow output stride**: The kernel fix correctly uses `flowStrideBytes` from `strideInfo[0].strideXInBytes`. Structurally correct.

### Leading Theories

**Theory A: Optical flow noise on textureless regions (most likely)**
Pure R/G/B bars are pathological for optical flow - zero texture within each bar means the flow estimator has no signal to track. It produces noisy/unreliable vectors. Any warp displacement near the sharp color borders shows as dramatically wrong colors. This explains why the artifact is much less visible in games (rich texture gives flow better signal). The lower-half concentration could be due to NvOF's internal processing order or temporal hint accumulation.

**Theory B: D3D9 GPU sync issue (less likely but possible)**
`D3DPOOL_DEFAULT` ring buffer surfaces are in VRAM. After `StretchRect` copies to them, `CopyFrameToHost` immediately calls `LockRect`. If the GPU hasn't finished the blit, the upper portion (already complete) would be correct and the lower portion would have stale data. However, D3D9Ex `LockRect` should implicitly synchronize.

### Current Diagnostic: Bypass Mode

A `bypassFlow` flag was added to `FrucCaptureMode::Run()` (around line 680):

```cpp
bool bypassFlow = true;  // DIAGNOSTIC: skip optical flow, pass frames through directly
```

When `bypassFlow = true`:
- Skips `ComputeOpticalFlow()` and `InterpolateFrame()` entirely
- Copies the most recent captured frame's host buffer directly to output via `memcpy`
- The rest of the present path (RGBA->BGRA swap, LockRect, StretchRect, PresentEx) runs identically
- Also added bottom-pixel diagnostic logging (row `m_height - 10`) to check for data corruption

**How to interpret results:**
- If bypass looks clean (correct R|G|B all the way down): data path is fine, artifacts are purely from optical flow noise on textureless input. Consider the issue acceptable for real content.
- If bypass also shows corruption in lower half: problem is in the D3D9 capture/copy pipeline. Investigate `D3DPOOL_DEFAULT` locking behavior, consider `D3DPOOL_SYSTEMMEM` staging surfaces.

## Known Inefficiency: GPU->CPU->GPU Round-Trip

The current pipeline has an expensive data path:
1. NvFBC captures to D3D9 surface (GPU)
2. `CopyFrameToHost`: LockRect + pixel-by-pixel channel swap to host buffer (GPU->CPU)
3. `UploadData`: host buffer to NvOF input buffer (CPU->GPU)
4. `cuMemcpyHtoD`: host buffer to CUDA frame buffer (CPU->GPU, second copy!)
5. Interpolation kernel output downloaded to host buffer (GPU->CPU)
6. Host buffer written to D3D9 output surface via LockRect (CPU->GPU)

This should eventually be replaced with GPU-resident operations:
- Use CUDA/D3D9 interop to access NvFBC surfaces directly as CUDA memory
- Run channel conversion as a simple CUDA kernel on GPU
- Keep frame data on GPU throughout the pipeline
- Only touch CPU for timing/control logic

## Other Notes

- The main loop's catch-up `while` loop (line ~737) can rapid-fire multiple presents if it falls behind. Ideally there should be exactly one output frame per tick; if behind, skip forward rather than blasting out frames. Not the root cause of blocking but may need revisiting.
- `FRAME_HISTORY_SIZE = 2` (just the two most recent frames). Sufficient for single-pair interpolation.
- NvOF SDK source is included locally in `NvOFSDK/` subdirectory (NvOF.cpp, NvOFCuda.cpp, headers).
- The `o:60` command-line mode activates FRUC at 60fps; `o` or `o:vsync` uses vsync-driven timing.
- Commit `1e9b3e4` fixed an earlier R/G channel swap issue in the BGRA<->RGBA conversions.
