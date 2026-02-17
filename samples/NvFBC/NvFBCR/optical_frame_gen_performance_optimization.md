# GPU-Resident Pipeline: Performance Optimization Plan

## Project Context

**Repo:** `C:\Users\garrett\RiderProjects\nvfbc-relay`
**Working dir:** `samples\NvFBC\NvFBCR\`
**Branch:** `garrett-nvofa-optical-flow-interpolation`
**Base commit:** `00eca167f843bb53eb81574976901dc9b5ae2b75`

### What This Is

NvFBCR is an NvFBC-based screen capture relay. We added a FRUC (Frame Rate Up-Conversion)
capture mode that produces smooth, locked-framerate output (e.g. 60fps) from variable-rate
input (>60fps) using NVIDIA Optical Flow Accelerator (NvOFA) for motion-compensated
interpolation. Activated via `o:60` command-line mode.

### Key Files

| File | Purpose |
|------|---------|
| `FrucCaptureMode.cpp` | Main FRUC implementation: capture loop, timing, flow, present |
| `FrucCaptureMode.h` | Class definition, ring buffer, CUDA/NvOF members |
| `InterpolateKernel.cu` | CUDA kernel: backward-warping interpolation using flow vectors |
| `NvOFSDK/NvOFCuda.cpp` | NvOF SDK wrapper: buffer creation, UploadData, Execute |
| `NvOFSDK/NvOFCuda.h` | NvOFBufferCudaDevicePtr class (getCudaDevicePtr, getStrideInfo) |
| `NvOFSDK/NvOF.cpp` | NvOF base: Init, CreateBuffers, output buffer dimensions |
| `NvFBCR.cpp` | Main app: D3D9Ex device creation, mode parsing, CLI |
| `NvFBCR_2013.vcxproj` | Build config: CUDA nvcc compilation, linked libs |

### Pipeline Architecture (GPU-Resident — current implementation)

All frame data stays on the GPU. Zero PCIe bus crossings. Zero host staging buffers.

1. **Capture**: NvFBC grabs desktop frame → `m_captureTarget` (D3D9 surface)
2. **Latch**: `StretchRect` (GPU→GPU) → `m_interopCaptureSurface` (D3D9 sync point)
3. **CUDA Interop Capture**: Map interop surface → `cuMemcpy2D` (CUarray→linear) →
   CUDA ring buffer (`m_cudaFrame0`/`m_cudaFrame1`) → unmap. Timestamped with QPC.
4. **Optical Flow Upload**: `cuMemcpy2D` device-to-device from ring buffer → NvOF input
   buffers (respecting NvOF stride via `getStrideInfo().strideInfo[0].strideXInBytes`)
5. **NvOF Execute**: Computes per-block motion vectors (grid size 4, PERF_LEVEL_FAST)
6. **Interpolation**: CUDA kernel backward-warps with bilinear sampling, weighted by
   `(tickTime - t0) / (t1 - t0)` where tickTime advances at `1/targetFramerate`
7. **CUDA Interop Present**: Map `m_outputSurface` → `cuMemcpy2D` (linear→CUarray) →
   unmap → `StretchRect` → backbuffer → `PresentEx`

CUDA context created via `cuD3D9CtxCreate` (not primary context) for D3D9 interop.
`#include <cudaD3D9.h>` for Driver API D3D9 interop functions.

### Build Setup

- D3D9Ex device (`IDirect3DDevice9Ex`), user-selected adapter
- CUDA Driver API (`cuda.lib`) + Runtime API (`cudart_static.lib`)
- CUDA targets: sm_75, sm_86, sm_89, sm_120
- `.cu` files compiled via custom nvcc build rule in vcxproj
- NvOF loaded at runtime from `nvofapi64.dll`

### Prior Fixes (committed to branch)

1. **Flow buffer stride fix**: NvOF output buffers have per-row padding. Changed kernel from
   tight-packed `flowVectors[y * flowWidth + x]` to stride-aware
   `(FlowVector*)(flowData + y * flowStrideBytes)[x]` using `strideInfo[0].strideXInBytes`.
   Fixed horizontal streaking.

2. **Off-color blocking in lower half (RGB test pattern)**: Investigated thoroughly.
   Confirmed via bypass diagnostic (`bypassFlow=true`) that the D3D9 data path is clean.
   The artifacts are from optical flow producing unreliable vectors on textureless pure-color
   regions. Much less visible on real content (games). Not a data corruption bug.

3. **Phase 1 channel swap removal**: Eliminated BGRA↔RGBA per-pixel swap loops in
   `CopyFrameToHost()` and the output present loop. Pipeline now stays in native BGRA.
   NvOF declared as ABGR8 but fed BGRA — works because NvOF only cares about consistent
   byte order, not channel semantics. Colors confirmed correct.

4. **Phase 3 GPU-resident pipeline** (in progress): Replaced all host staging buffers and
   PCIe transfers with CUDA-D3D9 interop. See "Pipeline Architecture" above for current
   data path. First attempt directly mapping NvFBC capture target produced black screen;
   fixed by adding a separate interop surface with StretchRect sync. Currently testing
   whether the fix resolves the black screen issue.

### Current Known Issues

- **Duplicate frames**: Stepping through recorded video shows many repeated frames (stutter).
  Visible at 180fps input → 60fps output. Root cause not yet measured - could be processing
  budget, timing logic, or the catch-up `while` loop. Phase 2 instrumentation will measure.
- **Text ghosting**: Static text on dark backgrounds shows ghost echoes from optical flow
  noise. Inherent limitation of flow-based interpolation on low-texture content.

---

## Problem

At 180fps input → 60fps output, we see many duplicate frames (visible stutter in recordings).
The root cause hasn't been measured yet - could be processing budget, timing logic, or both.

## Phase 1: Eliminate Channel Swap (DO THIS FIRST)

**Key insight:** The BGRA↔RGBA channel swap is completely unnecessary and is the easiest
performance win.

NvOF computes motion vectors by comparing pixel differences between frames. It doesn't care
about channel semantics - as long as both frames have consistent byte order, the flow vectors
are identical. The interpolation kernel just blends corresponding bytes at each position.
The D3D9 output expects BGRA, which is what we already have from NvFBC capture.

**Result:** The entire pipeline can stay in native BGRA throughout:
- NvFBC captures BGRA → feed BGRA directly to NvOF (declared as ABGR8, doesn't matter)
- Interpolation kernel blends BGRA bytes → output is BGRA
- Write BGRA directly to D3D9 output surface
- **Zero conversion code needed anywhere**

### Changes Required

**`FrucCaptureMode.cpp` - `CopyFrameToHost()`** (~line 400)
Replace per-pixel channel swap loop with straight `memcpy` per row:
```cpp
// BEFORE: per-pixel BGRA→RGBA swap (~8M ops at 1080p)
for (int x = 0; x < m_width; x++) {
    dstRow[x*4+0] = srcRow[x*4+2]; // R
    dstRow[x*4+1] = srcRow[x*4+1]; // G
    dstRow[x*4+2] = srcRow[x*4+0]; // B
    dstRow[x*4+3] = srcRow[x*4+3]; // A
}

// AFTER: simple row copy respecting D3D9 surface pitch
for (int y = 0; y < m_height; y++) {
    memcpy(dst + y * m_width * 4,
           src + y * lockedRect.Pitch,
           m_width * 4);
}
```

**`FrucCaptureMode.cpp` - `ComputeOpticalFlow()`** (~line 468)
Remove the redundant `cuMemcpyHtoD` to `m_cudaFrame0`/`m_cudaFrame1`. The host buffer
data (now BGRA, no swap) is uploaded to NvOF via `UploadData`. For the interpolation
kernel's frame data, either:
- (a) Do a single cuMemcpyHtoD from host to m_cudaFrame (still needed for warp kernel), OR
- (b) Read directly from NvOF input buffer CUdeviceptr in the interpolation kernel
  (requires stride handling - `getCudaDevicePtr()` + `getStrideInfo()` on input buffers)

Option (a) is simpler. Keep the cuMemcpyHtoD but remove the second redundant one.
Actually both cuMemcpyHtoD calls can stay as-is since they copy from host to
m_cudaFrame0 and m_cudaFrame1 respectively. The data is just BGRA now instead of RGBA.

**`FrucCaptureMode.cpp` - Output present loop** (~line 765)
Replace per-pixel RGBA→BGRA swap with straight `memcpy` per row:
```cpp
// BEFORE: per-pixel RGBA→BGRA swap
for (int x = 0; x < m_width; x++) {
    dst[x*4+0] = src[x*4+2]; // B
    dst[x*4+1] = src[x*4+1]; // G
    dst[x*4+2] = src[x*4+0]; // R
    dst[x*4+3] = src[x*4+3]; // A
}

// AFTER: simple row copy respecting D3D9 surface pitch
for (int y = 0; y < m_height; y++) {
    memcpy((uint8_t*)lockedRect.pBits + y * lockedRect.Pitch,
           m_hostOutputBuffer + y * m_width * 4,
           m_width * 4);
}
```

**`InterpolateKernel.cu`** - No changes needed. The kernel blends bytes at each position
regardless of channel interpretation. Comments referencing "ABGR8" can be updated to
note data is actually BGRA but it doesn't matter for blending.

**Impact:** Eliminates both CPU per-pixel loops. At 1080p, each loop does ~8M pixel
read-swap-write operations. This is likely several milliseconds per frame.

## Phase 2: Instrument Timing

After the channel swap removal, add `QueryPerformanceCounter` timestamps around each
pipeline stage to measure where remaining time goes.

**Stages to measure (in the main loop):**
```
[A] NvFBCToDx9VidGrabFrame (capture)
[B] StretchRect to ring buffer + CopyFrameToHost (now just memcpy)
[C] ComputeOpticalFlow (UploadData x2 + cuMemcpyHtoD x2 + NvOF Execute)
[D] InterpolateFrame (kernel + cuMemcpyDtoH)
[E] Output (LockRect + memcpy + StretchRect + PresentEx)
[F] Sleep / idle time until next tick
```

Log format (every 300 frames):
```
TIMING avg(ms): capture=X.XX copyToHost=X.XX optFlow=X.XX interp=X.XX
                output=X.XX sleep=X.XX total=X.XX
```

This tells us:
- Which stage dominates after channel swap removal
- Whether we're exceeding the 16.67ms budget
- How much idle/sleep time exists (headroom indicator)
- Whether duplicate frames are from processing time or timing logic bugs

**File:** `FrucCaptureMode.cpp` Run() main loop

## Phase 3: GPU-Resident Pipeline (CUDA-D3D9 Interop)

Only needed if Phases 1+2 don't resolve the duplicate frame issue.
Eliminates all PCIe bus crossings by keeping frame data on the GPU.

### Current Data Path (after Phase 1)
```
NvFBC → D3D9 captureTarget (GPU)
  │ StretchRect (GPU→GPU)
  ▼
D3D9 ring buffer surface (GPU)
  │ LockRect + memcpy rows              ← PCIe (GPU→CPU)
  ▼
Host staging buffer (CPU, BGRA)
  ├──→ UploadData to NvOF input          ← PCIe (CPU→GPU)
  └──→ cuMemcpyHtoD to m_cudaFrame       ← PCIe (CPU→GPU)
         │
       NvOF Execute + Interpolation (GPU)
         │
       m_cudaOutputFrame (GPU, BGRA)
  │ cuMemcpyDtoH                         ← PCIe (GPU→CPU)
  ▼
Host output buffer (CPU, BGRA)
  │ LockRect + memcpy rows               ← PCIe (CPU→GPU)
  ▼
D3D9 output surface → backbuffer → PresentEx
```

### Target Data Path
```
NvFBC → D3D9 captureTarget (GPU)
  │ CUDA-D3D9 interop: map → cuMemcpy2D (array→linear, GPU→GPU)
  ▼
CUDA linear ring buffer (GPU, BGRA)
  │ cuMemcpy2D device→device to NvOF input buffer (handling stride)
  │ (m_cudaFrame IS the ring buffer - no separate copy needed)
  ▼
NvOF Execute + Interpolation (GPU)
  │
  ▼
m_cudaOutputFrame (GPU, BGRA)
  │ CUDA-D3D9 interop: map → cuMemcpy2D (linear→array, GPU→GPU)
  ▼
D3D9 output surface → backbuffer → PresentEx
```

**Zero PCIe crossings. Zero host buffers. No conversion kernels.**

### Implementation Steps

#### Step 3a: Replace CUDA initialization with D3D9-interop context

**Currently:** `InitCuda()` (called from `Setup()`) does:
```cpp
cuDeviceGet(&m_cuDevice, 0);                          // hardcodes device 0
cuDevicePrimaryCtxRetain(&m_cuContext, m_cuDevice);    // primary context, no D3D9 interop
```

**Problems:**
1. Device 0 may not match the D3D9 adapter (multi-GPU systems)
2. Primary context has no D3D9 interop — `cuGraphicsD3D9RegisterResource` will fail
3. `InitCuda()` is called in `Setup()` before surfaces exist, but `cuD3D9CtxCreate`
   needs `IDirect3DDevice9*` (available via `m_device = g_pD3D9Device` at that point)

**Fix:** Replace the device selection + context creation with `cuD3D9CtxCreate`:
```cpp
// BEFORE:
cuDeviceGet(&m_cuDevice, 0);
cuDevicePrimaryCtxRetain(&m_cuContext, m_cuDevice);
cuCtxSetCurrent(m_cuContext);

// AFTER:
cuD3D9CtxCreate(&m_cuContext, &m_cuDevice, 0, m_device);
// Context is automatically made current. m_cuDevice is populated with the
// CUDA device matching the D3D9 device. Runtime API kernel launches work
// because they use whatever context is current.
```

**Cleanup change:** `cuDevicePrimaryCtxRelease(m_cuDevice)` → `cuCtxDestroy(m_cuContext)`

**Requires:** `m_device` must be set before `InitCuda()` is called. Currently true —
`Setup()` sets `m_device = g_pD3D9Device` at line 339 before calling `InitCuda()` at line 342.

**Header:** `cuD3D9CtxCreate` is in `cudaD3D9.h` (Driver API D3D9 interop header).
Need to add `#include <cudaD3D9.h>` to FrucCaptureMode.h. No new libs — `cuda.lib` already linked.

**File:** `FrucCaptureMode.cpp` `InitCuda()`

#### Step 3b: Register D3D9 surfaces with CUDA

**Files:** `FrucCaptureMode.h`, `FrucCaptureMode.cpp`

New members:
```cpp
CUgraphicsResource m_cudaCaptureResource;  // registered capture target
CUgraphicsResource m_cudaOutputResource;   // registered output surface
```

Registration (in Run(), after surfaces created):
```cpp
cuGraphicsD3D9RegisterResource(&m_cudaCaptureResource, m_captureTarget,
                                CU_GRAPHICS_REGISTER_FLAGS_NONE);
cuGraphicsD3D9RegisterResource(&m_cudaOutputResource, m_outputSurface,
                                CU_GRAPHICS_REGISTER_FLAGS_NONE);
```

**IMPORTANT:** D3D9 interop only supports three registration flags:
`CU_GRAPHICS_REGISTER_FLAGS_NONE`, `CU_GRAPHICS_REGISTER_FLAGS_SURFACE_LDST`,
`CU_GRAPHICS_REGISTER_FLAGS_TEXTURE_GATHER`. The READ_ONLY and WRITE_DISCARD flags
are OpenGL-only and will return `CUDA_ERROR_INVALID_VALUE` for D3D9.
Source: `cudaD3D9.h` lines 310-317.

No new libs needed — `cuda.lib` already linked. Header: `cudaD3D9.h` (added in Step 3a).

#### Step 3c: GPU capture path (replaces CopyFrameToHost + UploadData)

```cpp
bool CaptureFrameToGPU(int ringIndex) {
    // Map D3D9 surface → CUDA array
    cuGraphicsMapResources(1, &m_cudaCaptureResource, m_cudaStream);
    CUarray mappedArray;
    cuGraphicsSubResourceGetMappedArray(&mappedArray, m_cudaCaptureResource, 0, 0);

    // Copy array → linear device memory (BGRA, no conversion)
    CUDA_MEMCPY2D cp = {};
    cp.srcMemoryType = CU_MEMORYTYPE_ARRAY;
    cp.srcArray = mappedArray;
    cp.dstMemoryType = CU_MEMORYTYPE_DEVICE;
    cp.dstDevice = m_cudaFrame[ringIndex];  // reuse existing alloc as ring buffer
    cp.dstPitch = m_width * 4;
    cp.WidthInBytes = m_width * 4;
    cp.Height = m_height;
    cuMemcpy2DAsync(&cp, m_cudaStream);

    cuGraphicsUnmapResources(1, &m_cudaCaptureResource, m_cudaStream);

    // Device-to-device copy to NvOF input buffer (respecting NvOF stride)
    NvOFBufferCudaDevicePtr* nvofBuf = dynamic_cast<NvOFBufferCudaDevicePtr*>(
        m_inputBuffers[ringIndex].get());
    CUDA_MEMCPY2D cp2 = {};
    cp2.srcMemoryType = CU_MEMORYTYPE_DEVICE;
    cp2.srcDevice = m_cudaFrame[ringIndex];
    cp2.srcPitch = m_width * 4;
    cp2.dstMemoryType = CU_MEMORYTYPE_DEVICE;
    cp2.dstDevice = nvofBuf->getCudaDevicePtr();
    cp2.dstPitch = nvofBuf->getStrideInfo().strideInfo[0].strideXInBytes;
    cp2.WidthInBytes = m_width * 4;  // NvOF element size = 4 for ABGR8
    cp2.Height = m_height;
    cuMemcpy2DAsync(&cp2, m_cudaStream);

    return true;
}
```

#### Step 3d: GPU output path (replaces cuMemcpyDtoH + LockRect)

```cpp
bool PresentFromGPU(IDirect3DDevice9Ex* device) {
    // Map D3D9 output surface → CUDA array
    cuGraphicsMapResources(1, &m_cudaOutputResource, m_cudaStream);
    CUarray outputArray;
    cuGraphicsSubResourceGetMappedArray(&outputArray, m_cudaOutputResource, 0, 0);

    // Copy linear → array (BGRA, no conversion)
    CUDA_MEMCPY2D cp = {};
    cp.srcMemoryType = CU_MEMORYTYPE_DEVICE;
    cp.srcDevice = m_cudaOutputFrame;
    cp.srcPitch = m_width * 4;
    cp.dstMemoryType = CU_MEMORYTYPE_ARRAY;
    cp.dstArray = outputArray;
    cp.WidthInBytes = m_width * 4;
    cp.Height = m_height;
    cuMemcpy2DAsync(&cp, m_cudaStream);

    cuStreamSynchronize(m_cudaStream);
    cuGraphicsUnmapResources(1, &m_cudaOutputResource, m_cudaStream);

    device->StretchRect(m_outputSurface, NULL, g_backbuffer, NULL, D3DTEXF_NONE);
    device->PresentEx(NULL, NULL, NULL, NULL, D3DPRESENT_INTERVAL_IMMEDIATE);
    return true;
}
```

#### Step 3e: Clean up dead code

Remove:
- `CopyFrameToHost()` method entirely
- `hostBuffer` / `hostBufferSize` from FrameHistoryEntry
- `m_hostOutputBuffer`
- D3D9 ring buffer surfaces (`m_frameHistory[i].d3dSurface`) - replaced by CUDA ring
- StretchRect from captureTarget → ring buffer
- `bypassFlow` diagnostic code

## Risks and Considerations

### CUDA-D3D9 device matching (resolved)
D3D9 device is on user-selected adapter. CUDA device 0 might not match.
Resolved by Step 3a: `cuD3D9CtxCreate` auto-selects the matching CUDA device.

### CUDA-D3D9 interop context requirement (verified)
`cuGraphicsD3D9RegisterResource` requires a CUDA context created via `cuD3D9CtxCreate`,
not a plain `cuCtxCreate` or primary context. The current `cuDevicePrimaryCtxRetain`
context will NOT work for interop. Step 3a addresses this.
Source: CUDA Driver API docs for `cuD3D9CtxCreate`.

### D3D9 surface type compatibility (verified)
`cuGraphicsD3D9RegisterResource` supports `IDirect3DSurface9` as "stand-alone objects"
(not surfaces obtained from textures). Both `m_captureTarget` and `m_outputSurface` are
created via `CreateOffscreenPlainSurface(D3DPOOL_DEFAULT)` — these are stand-alone,
GPU-resident surfaces. Confirmed compatible.
Restrictions: cannot register primary render targets, shared resources, or depth/stencil.
None of these apply to our surfaces.
Source: CUDA Runtime API docs for `cudaGraphicsD3D9RegisterResource` (same restrictions
apply to Driver API equivalent).

### NvFBC capture target cannot be directly CUDA-mapped (discovered during impl)
**Problem:** Mapping `m_captureTarget` (the NvFBC output surface) directly via
`cuGraphicsD3D9RegisterResource` + `cuGraphicsMapResources` results in black output.
All API calls return success, but the CUDA ring buffer contains zeros.

**Root cause:** NvFBC is a proprietary NVIDIA capture API that writes to D3D9 surfaces
through a path that may bypass the D3D9 command stream. `cuGraphicsMapResources` only
synchronizes with D3D9 API calls — NvFBC's internal GPU writes are not guaranteed to be
visible to CUDA after mapping.

**Fix:** Added a separate `m_interopCaptureSurface` (D3DPOOL_DEFAULT, registered for
CUDA interop). After NvFBC captures to `m_captureTarget`, we `StretchRect` to the
interop surface. StretchRect IS a D3D9 call, so CUDA's map synchronization sees it.
The StretchRect is GPU→GPU and takes microseconds.

**Data path:**
```
NvFBC → m_captureTarget → StretchRect (D3D9 sync point) → m_interopCaptureSurface
  → CUDA map → cuMemcpy2D (array→linear, GPU→GPU) → CUDA ring buffer → unmap
```

### Map/Unmap overhead (Phase 3)
~0.1-0.5ms per call. Input: up to 180/sec, output: 60/sec.
Still far less than LockRect + PCIe transfers.

### NvOF input stride (Phase 3)
When copying device-to-device to NvOF input buffers, use cuMemcpy2D with
NvOF buffer stride from `getStrideInfo().strideInfo[0].strideXInBytes`.
Same approach used for the flow output stride fix.

### NvOF format declaration
We declare `NV_OF_BUFFER_FORMAT_ABGR8` but feed BGRA data. This is fine because
NvOF only cares about consistent byte patterns between frames for flow computation.
The channel semantic doesn't affect motion vector quality.

### D3D9 interop registration flags (corrected during impl)
`cuGraphicsD3D9RegisterResource` only supports three flags:
`CU_GRAPHICS_REGISTER_FLAGS_NONE`, `SURFACE_LDST`, `TEXTURE_GATHER`.
The `READ_ONLY` and `WRITE_DISCARD` flags are OpenGL-only and return
`CUDA_ERROR_INVALID_VALUE` for D3D9. Original plan had wrong flags.
Source: `cudaD3D9.h` lines 310-317.

### Fallback
Keep CPU path behind a flag for systems where CUDA-D3D9 interop fails.

## Estimated Impact

| Metric | Before | After Phase 1 | After Phase 3 |
|--------|--------|---------------|---------------|
| PCIe crossings/frame | 4 | 4 | 0 (1 StretchRect GPU→GPU for NvFBC sync) |
| CPU pixel loops/frame | 2 (full res) | 0 | 0 |
| Channel conversions | 2 | 0 | 0 |
| Host buffers | 3 | 3 | 0 |
| Expected frame time | 10-20ms | 5-10ms | 2-4ms |

## Implementation Log

### Phase 1: Eliminate channel swap — DONE, VERIFIED
- Replaced per-pixel BGRA↔RGBA swap loops with `memcpy` per row in both
  `CopyFrameToHost()` and the output present loop.
- Colors confirmed correct. Pipeline stays in native BGRA throughout.
- NvOF declared as ABGR8 but fed BGRA — verified: NvOF only cares about consistent
  byte order, not channel semantics.

### Phase 3: GPU-resident pipeline — DONE, VERIFIED
- `InitCuda()`: `cuDevicePrimaryCtxRetain` → `cuD3D9CtxCreate`. Cleanup:
  `cuDevicePrimaryCtxRelease` → `cuCtxDestroy`. Confirmed working on RTX 5080 sm_120.
- `AllocateBuffers()`: Removed D3D9 ring buffer surfaces and host staging buffers.
  Added `cuGraphicsD3D9RegisterResource` with `CU_GRAPHICS_REGISTER_FLAGS_NONE`.
- `CaptureFrame()`: NvFBC → StretchRect → interop surface → CUDA map → cuMemcpy2D →
  ring buffer → unmap. (Direct mapping of NvFBC target produced black — see risks.)
- `ComputeOpticalFlow()`: Host uploads replaced with device-to-device `cuMemcpy2D`
  from CUDA ring buffer to NvOF input buffers (respecting NvOF stride).
- `InterpolateFrame()`: Removed `cuMemcpyDtoH` — output stays on GPU.
- `PresentFromGPU()` (new): Maps output surface → `cuMemcpy2D` → unmap → StretchRect →
  PresentEx. Zero PCIe.
- Removed: `CopyFrameToHost()`, host buffers, `bypassFlow` diagnostic.
- **First attempt (direct NvFBC target mapping):** Black screen. All API calls returned
  success but ring buffer contained zeros. Root cause: NvFBC writes may bypass D3D9
  command stream, so `cuGraphicsMapResources` sync guarantee doesn't apply.
- **Second attempt (with StretchRect sync):** Working. Colors correct.
- **Post-review fixes:**
  - `PresentFromGPU`: Fixed `cuGraphicsUnmapResources` passing stream `0` instead of
    `m_cudaStream` (inconsistent with capture path; harmless due to prior sync but wrong).
  - `CaptureFrame`: `cuMemcpy2DAsync` failure now unmaps and returns false instead of
    falling through and marking the frame valid with potentially stale ring buffer data.
  - Added missing trailing newlines to `.cpp` and `.h` files.
  - `InterpolateFrame` signature takes `CUdeviceptr framePrev, frameCurr` params — cleaner
    than plan's `m_cudaFrame[ringIndex]` approach since callers can pass any pair.

#### StretchRect overhead analysis
The StretchRect from `m_captureTarget` → `m_interopCaptureSurface` is GPU→GPU:
- At 1280x720x4 = 3.5MB, with RTX 5080 bandwidth (~700+ GB/s), the copy takes ~5μs
- Even at 180fps capture rate, total overhead < 1ms/second (0.03% of frame budget)
- Extra VRAM for the interop surface: 3.5MB (negligible)
- **Verdict:** Not worth optimizing further. Keep the StretchRect.

#### Possible alternative: D3D9 event query sync (not yet tested)
Instead of StretchRect to a separate surface, could try registering `m_captureTarget`
directly and using an `IDirect3DQuery9` event query after NvFBC grab to force a D3D9
pipeline flush before CUDA mapping:
```cpp
// After NvFBC grab, before CUDA map:
IDirect3DQuery9* query;
device->CreateQuery(D3DQUERYTYPE_EVENT, &query);
query->Issue(D3DISSUE_END);
while (query->GetData(NULL, 0, D3DGETDATA_FLUSH) == S_FALSE) { /* spin */ }
query->Release();
// Then map m_captureTarget directly
```
This would eliminate the extra surface + copy if NvFBC's writes are visible after a D3D9
flush. However, it might NOT work if NvFBC uses a GPU engine that D3D9 event queries
don't track. Easy to test — worth trying to see if it eliminates the black screen.
If it works: removes 1 surface allocation + 1 StretchRect per capture.
If it doesn't: confirms NvFBC truly bypasses D3D9, and StretchRect is the right fix.

### Phase 2: Instrument timing — DONE, RESULTS BELOW

Added `QueryPerformanceCounter` timestamps around each pipeline stage in `Run()`.
Also added a **dropped frame counter** (incremented when a target present time falls
before the current frame pair — the catch-up skip path).

#### What each metric measures

- **flow**: D2D copies to NvOF input buffers + NvOF Execute (once per frame pair, amortized per present)
- **interp**: Kernel launch only (async — actual execution time is in `present`)
- **present**: `cuStreamSynchronize` (waits for interp kernel) + CUDA→D3D9 copy + unmap + StretchRect + PresentEx
- **work**: flow + interp + present (actual per-present GPU work)
- **headroom**: budget - work (positive = time to spare, negative = dropping frames)
- **capture**: NvFBC grab + StretchRect + CUDA interop, shown as per-grab average
- **sleep**: Idle `Sleep()` time between present targets
- **dropped**: Target present times skipped because they fell behind

Log format (every 300 presents / ~5 seconds at 60fps):
```
TIMING(ms): flow=X.XX interp=X.XX present=X.XX work=X.XX headroom=X.XX | capture=X.XX/grab sleep=X.XX | N presents, M captures, D dropped
```

Note on `interp` vs `present`: The interpolation kernel is launched async, so `interp`
only measures launch overhead (~0.02ms). The actual kernel execution time is included
in `present` because `PresentFromGPU` calls `cuStreamSynchronize` which waits for
the kernel to complete before copying the output to D3D9. So effectively:
`present` = kernel execution + CUDA→D3D9 interop + D3D9 present.

#### Timing results (RTX 5080, 1280x720, grid size 4, 60fps target)

Initial metrics used a tautological `total` that always equaled the budget (~16.67ms)
because it summed ALL accumulated time (including sleep) divided by presents — which
just measures wall-clock time per frame. Fixed to report `work` and `headroom` instead.

Raw data from first run (grid size 4, before metrics fix — `total` is tautological):
```
capture=0.79  flow=12.01 interp=0.01 present=3.36  sleep=0.61 | 300 presents, 1210 captures
capture=0.80  flow=11.89 interp=0.02 present=3.85  sleep=0.04 | 300 presents, 1297 captures
capture=1.75  flow=9.43  interp=0.02 present=2.66  sleep=2.95 | 300 presents, 1500 captures
capture=0.96  flow=10.07 interp=0.03 present=4.74  sleep=0.89 | 300 presents, 848 captures
capture=0.47  flow=8.46  interp=0.04 present=7.54  sleep=0.07 | 300 presents, 256 captures
capture=0.33  flow=7.09  interp=0.05 present=9.14  sleep=0.00 | 300 presents, 134 captures
capture=0.66  flow=8.88  interp=0.04 present=7.06  sleep=0.00 | 300 presents, 209 captures
capture=0.69  flow=9.96  interp=0.04 present=5.99  sleep=0.00 | 300 presents, 242 captures
capture=0.61  flow=9.19  interp=0.03 present=6.83  sleep=0.00 | 300 presents, 246 captures
capture=0.60  flow=8.17  interp=0.04 present=7.71  sleep=0.04 | 300 presents, 191 captures
capture=0.92  flow=8.55  interp=0.04 present=6.24  sleep=0.93 | 300 presents, 438 captures
capture=0.76  flow=9.81  interp=0.02 present=5.02  sleep=1.10 | 300 presents, 655 captures
capture=0.78  flow=10.05 interp=0.03 present=5.22  sleep=0.70 | 300 presents, 604 captures
```

#### Analysis

| Stage | Avg ms | Range | Notes |
|-------|--------|-------|-------|
| **flow** | **~9.5** | 7-12 | **DOMINANT BOTTLENECK.** NvOF Execute. |
| present | ~6.0 | 3-9 | Includes interp kernel execution via stream sync |
| capture | ~0.7 | 0.3-1.8 | Cheap. NvFBC + CUDA interop. |
| interp | ~0.03 | 0.01-0.05 | Just async launch overhead |
| work | ~15.5 | 12-16.3 | flow + interp + present |
| headroom | ~1.2 | -0.3 to 4.6 | Many intervals near zero or negative → drops |

**Key findings:**
1. **NvOF Execute is ~57% of the per-present budget.** This is the optical flow hardware
   accelerator itself — there's no software optimization to make it faster.
2. **`present` is ~36% of budget.** This includes the interpolation kernel execution
   (hidden behind async launch) + CUDA→D3D9 interop + D3D9 StretchRect + PresentEx.
3. **Capture is only ~4% of budget.** The GPU-resident pipeline (Phase 3) made this cheap.
4. **Headroom is often near zero or negative.** Intervals with `sleep=0.00` are at 100%
   utilization and will drop frames on any GPU load spike.
5. **Capture count varies wildly (134-1500).** Correlates with GPU load — when the game
   pushes the GPU harder, NvFBC captures slow down and everything gets more expensive.

#### Grid size 8 attempt — FAILED
NvOF on RTX 5080 only supports grid sizes 1, 2, and 4. There is no grid size 8.
The `NV_OF_OUTPUT_VECTOR_GRID_SIZE` enum in `nvOpticalFlowCommon.h` defines only:
`UNDEFINED`, `1`, `2`, `4`, `MAX`. Grid size 4 is already the coarsest available.
This means we cannot reduce NvOF execution time by using a coarser grid.

## Phase 4: Reduce NvOF Bottleneck — NOT YET STARTED

NvOF Execute at ~9.5ms/frame is the dominant bottleneck. Grid size 4 is already the
coarsest available. The following options could reduce the effective flow cost:

### Option A: Downscale input for flow computation
Feed NvOF a lower-resolution version of the frames (e.g. 640x360 instead of 1280x720).
Use the full-resolution frames for the interpolation kernel, scaling up the flow vectors.

**Pros:** NvOF time scales roughly with pixel count — 4x fewer pixels ≈ 4x faster (~2.5ms).
**Cons:** Coarser flow vectors, especially at edges. Needs a GPU downscale step (cheap
via StretchRect or CUDA kernel) and flow vector upscaling in the interpolation kernel.
**Complexity:** Medium. New downscale surface + NvOF at half res + kernel changes to
scale flow vectors by 2x. Flow grid would be 160x90 at grid size 4, half-res input.

### Option B: Skip flow on similar frames
If consecutive frames are nearly identical (static scene, pause menu), reuse the
previous flow vectors instead of recomputing. Detect similarity via a cheap metric
(e.g. sum of absolute differences on a small sample of pixels via CUDA kernel).

**Pros:** Zero flow cost for static/near-static content. Common in menus, loading screens.
**Cons:** Needs a threshold that works across content types. Doesn't help during motion
(which is when flow matters most). Risk of stale vectors causing artifacts if threshold
is wrong.
**Complexity:** Low. Small CUDA kernel for SAD on sampled pixels + conditional skip.

### Option C: Async/pipelined flow computation
Overlap NvOF execution with the previous frame's present. Instead of the current
serial pipeline (capture → flow → interp → present), start flow computation for the
next frame pair while presenting the current interpolated frame.

Current pipeline (serial, ~16ms):
```
|--capture--|----flow(9.5ms)----|--interp+present(6ms)--|
```

Pipelined (flow overlaps with previous present):
```
Frame N:   |--capture--|----flow(9.5ms)----|
Frame N:                                    |--interp+present(6ms)--|
Frame N+1: |--capture--|----flow(9.5ms)------------|
                        ↑ overlaps with N's present
```

**Pros:** Could effectively hide flow latency behind present, reducing per-frame wall
time to ~max(flow, present) instead of flow + present. Potentially ~10ms instead of ~16ms.
**Cons:** Requires two CUDA streams (one for flow, one for present). Adds pipeline
latency (flow is one frame behind). More complex synchronization. NvOF may not support
concurrent execution with other CUDA work on the same GPU.
**Complexity:** High. Dual-stream architecture, double-buffered flow results, careful
synchronization between streams. Need to verify NvOF can run on a separate stream
concurrently with CUDA kernels on another.

### Option D: Reduce interpolation kernel + present cost
The `present` timing (~6ms) includes both the interpolation kernel execution and the
D3D9 present path. Potential sub-optimizations:
- Profile the kernel vs D3D9 overhead separately (add a stream sync between interp and present)
- Optimize the interpolation kernel (memory access patterns, shared memory for flow vectors)
- Check if `PresentEx` with `D3DPRESENT_INTERVAL_IMMEDIATE` is blocking unexpectedly

**Pros:** Could shave 1-3ms off the second-largest cost.
**Cons:** Smaller impact than reducing flow. Kernel optimization needs profiling first.
**Complexity:** Low-Medium. Splitting the timing is trivial. Kernel optimization depends
on what profiling reveals.

### Option E: Eliminate intermediate ring buffer copies
Currently, captured frames go through two copies before NvOF sees them:
```
interop surface → cuMemcpy2D → m_cudaFrame0/1 (ring buffer) → cuMemcpy2D → NvOF input buffer
```
The second copy exists because NvOF input buffers may have different stride. But the
timing logs show NvOF input stride = 5120 bytes and our frame stride = 1280×4 = 5120
bytes — **they match at the current resolution.** Even if they don't match, we could
copy directly from the mapped D3D9 array into the NvOF input buffer (respecting stride),
skipping the intermediate ring buffer entirely:
```
interop surface → cuMemcpy2D → NvOF input buffer (with stride)
```
The interpolation kernel currently reads from `m_cudaFrame0`/`m_cudaFrame1`. It would
need to read from the NvOF input buffer's CUdeviceptr instead (using
`getCudaDevicePtr()` + stride from `getStrideInfo()`). The kernel already handles
stride for flow vectors — same pattern for frame data.

**Removes:** `m_cudaFrame0`, `m_cudaFrame1` allocations + 2 D2D copies per flow computation.
**Saves:** ~0.5ms per present (estimated — D2D copies are fast but not free).
**Complication:** Kernel needs stride-aware frame reads. NvOF input buffers become the
ring buffer, so we need to ensure NvOF doesn't overwrite them while the kernel reads.
The kernel runs after NvOF Execute completes (same stream), so this should be safe.
**Complexity:** Low. Remove ring buffer allocs, change copy target in `CaptureFrame`,
update kernel to accept frame stride parameter.

### Recommendation
**Start with Option A (downscale)** — it's the highest-impact change targeting the
dominant bottleneck, with moderate complexity. If NvOF at 640x360 runs in ~2.5ms instead
of ~9.5ms, total work drops to ~8.5ms with ~8ms headroom. That's comfortably within
budget even under GPU load.

**Option E (eliminate ring buffer)** is low-hanging fruit that can be done alongside
any other option — it simplifies the pipeline and removes ~0.5ms of copies.

Option C (async pipeline) is the most architecturally interesting but also the most
complex and risky. Save it for if Option A isn't enough.

## Execution Order

1. **Phase 1** (eliminate channel swap) — DONE
2. **Phase 3** (CUDA-D3D9 interop) — DONE
3. **Phase 2** (instrument timing) — DONE
4. **Phase 4** (reduce NvOF bottleneck) — next, see options above

## Test Screenshots

Located in `C:\Users\garrett\Pictures\Screenshots\`:
- `test_pattern_good.png` - clean RGB test pattern (baseline)
- `stride_fix_bad_2.png` - off-color blocking artifact (from flow noise, not data bug)
- `claude\` folder - game screenshots (Avatar: Frontiers of Pandora) showing text ghosting

Located in `C:\Users\garrett\Pictures\Screenshots\claude\`:
- `menu 0.png` / `menu 1.png` - game menu, mostly clean
- `intro text 0.png` / `intro text 1.png` - disclaimer text, visible flow artifacts
- `small text 0.png` / `small text 1.png` - worst case: ghost echoes on text
- `loading 0.png` / `loading 1.png` - loading screen, cursor duplication
