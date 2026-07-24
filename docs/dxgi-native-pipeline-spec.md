# DXGI-Native Capture + Present Pipeline Spec (D3D11 mode)

Status: DESIGN (not implemented). An ALTERNATIVE capture+present mode, not a swap: keep the NvFBC
D3D9Ex path as-is and add a fully DXGI/D3D11-native path whose defining property is HONEST,
present-aligned timestamps on both ends. Prove the gain with a diagnostic (Phase 0) before building
the pipeline. Companions: `dual-device-capture-present-spec.md` (the timestamp problem, T1/T9, and
the two-device shared-handle model), `frame-marker-spec.md`, `phase-comb-lock-spec.md`.

## Motivation

The whole temporal engine runs on a corrupted timeline. Capture arrivals are stamped with
`QueryPerformanceCounter` at the instant NvFBC's grab returns (`CaptureRing.cpp`), i.e. RECEIVE time,
not the frame's true present time. `NvFBCFrameGrabInfo` (legacy `NvFBCToDx9Vid`) carries no timestamp
and no new-frame flag, so there is nothing to correct it with. NvFBC delivery is bursty (paired
arrivals; occasional ~100 ms pause-then-flush, seen right after map-close), which the engine reads as
phantom stalls and covers with blends that never needed to exist (Get Medieval, 2026-07-23: the
source display's own G-Sync refresh graph stayed steady through those "stalls", so they were
NvFBC-delivery artifacts, not game hitches). NvFBC on Windows is additionally DEPRECATED (Win10
Oct-2019 update); the timestamped modern NvFBC (`ulTimestampUs` + `bIsNewFrame`) is Linux-only.

The dual-device spec already names this: "Timestamps are the product." This spec's answer is a
capture+present path where both the frame-in time and the frame-out time are real, on one QPC clock.

## The core win: one honest clock on both ends

- **Capture-in:** each source frame carries its TRUE present time (DD `LastPresentTime` or WGC
  `SystemRelativeTime`), so bracketing/blend/lock run on real content cadence, not delivery jitter.
- **Present-out:** a flip-model DXGI swap chain reports the actual vblank each present hit
  (`GetFrameStatistics.SyncQPCTime`), replacing today's inferred `pdt`/`jit` around `PresentEx`.

Both are QPC-based, so they share a timeline. That closes the loop the relay has never had.

## Phase 0: diagnostic (build this FIRST; it is the go/no-go gate)

Everything hinges on one empirical question: can a SUPPORTED Windows capture both (a) actually grab
the source game and (b) hand back a present-aligned per-frame timeline? Do not build the pipeline
until this passes. Scope: a small standalone probe, no compositors, no present.

1. Stand up a D3D11 capture session (DD and/or WGC) on the source output. Per acquired frame log:
   the backend timestamp (`LastPresentTime` / `SystemRelativeTime`), the new-frame flag
   (`AccumulatedFrames` / frame arrival), and the acquire-QPC.
2. Run it against the ACTUAL source (KCD, borderless, G-Sync), and deliberately trigger the map-close
   stalls that the NvFBC path shows.
3. Metrics:
   - **Coverage gate:** does capture SUCCEED on the source, or does it lose the surface to
     independent-flip / MPO (see backend choice)? This is the #1 kill criterion.
   - **Timeline gate:** do backend-timestamp deltas stay ~present-paced through the windows where
     NvFBC `arr=` shows a 100 ms gap + catch-up flood? If yes, it proves the stalls are
     NvFBC-delivery artifacts AND that this backend dissolves them.
   - Sanity: on a known-cadence source (UFO at a fixed rate) the backend timeline should be a clean
     line; quantify jitter vs NvFBC.
4. Pass = a monotonic, present-aligned timeline that does NOT reproduce the phantom stalls, on a
   backend that reliably captures the source. Fail on either axis redirects or kills the effort.

## Capture backend: DD vs WGC (Phase 0 decides)

Both are supported on modern Windows, both hand back a D3D11 texture in VRAM (in-GPU, no forced CPU
copy), both carry a per-frame timestamp. They differ where it matters for a game source:

- **DXGI Desktop Duplication (DD).** `IDXGIOutputDuplication::AcquireNextFrame` returns an
  `ID3D11Texture2D` plus `DXGI_OUTDUPL_FRAME_INFO`:
  - `LastPresentTime` (QPC of the present; 0 = mouse-only, no new frame) = the true present time,
    explicitly.
  - `AccumulatedFrames` (0 = only the pointer moved = the new-frame flag NvFBC lacks; >1 = frames
    were coalesced, i.e. some were missed and only the latest is available).
  - RISK, and the reason it may fail Phase 0: DD returns `DXGI_ERROR_ACCESS_LOST` under
    independent-flip / MPO presentation, which fullscreen-borderless games frequently get. So DD is
    not guaranteed even for borderless, and never for exclusive-fullscreen. Simple when it works;
    `LastPresentTime` is the cleanest timestamp.
- **Windows.Graphics.Capture (WGC).** `Direct3D11CaptureFramePool` yields `Direct3D11CaptureFrame`:
  - `Surface` -> `IDirect3DSurface` -> D3D11 texture.
  - `SystemRelativeTime` (a QPC-based per-frame time; validate in Phase 0 that it tracks the present
    cadence rather than pure acquire time).
  - The modern API OBS Game Capture uses; captures fullscreen / independent-flip content DD cannot.
    Costs: a capture border (removable on recent Windows builds) and WinRT interop setup.

Likely outcome: WGC for coverage on real games, DD only if it happens to hold the surface and its
`LastPresentTime` proves cleaner. The rest of the pipeline is written behind a capture interface so
the choice is swappable.

## Architecture (backend-independent behind ICaptureSource)

```
 DD / WGC ─► D3D11 CAPTURE device ─► ring slots (D3D11 textures, stamped PRESENT time)
                                        │ shared (keyed-mutex handles)
                                        ▼
                          D3D11 PRESENT device ─► flip-model swap chain ─► XR1
```

- **`ICaptureSource`** (new seam): `AcquireFrame(out ID3D11Texture2D*, out LONGLONG presentQpc, out
  bool isNewFrame)`. NvFBC (legacy, presentQpc = grab-QPC, isNewFrame always true) and DD/WGC both
  implement it. This is what makes DXGI "an alternative mode, not a swap."
- **Ring:** holds D3D11 textures. Stamp `slot.timestamp` with the backend's PRESENT time, not
  QPC-at-grab. This single change is the entire point; the selection math downstream is unchanged.
- **Compositors:** D3D11 (see reuse map).
- **Present:** flip-model DXGI swap chain on the XR1 output.

## Reuse map (what this actually touches)

| component | disposition |
|---|---|
| `PresentScheduler` | REUSE (pure QPC timing, backend-agnostic already) |
| `TemporalPolicy` / gate / comb lock | REUSE (pure math) |
| `CaptureRing` `FindBracket` / selection | REUSE (timestamp math; now fed honest stamps) |
| `FlowWarpEngine` (NVOFA + warp) | REUSE (already 0 D3D9 refs; NVOFA is D3D11/CUDA-native) |
| `BlendRenderer` (lerp) | RESKIN: `ps_3_0` -> SM5 full-screen-quad pixel shader |
| `FrameMarker` (burn) | RESKIN: `StretchRect`/`ColorFill` -> quad draw / `CopySubresourceRegion` |
| `FrameCompositors` copies | RESKIN: `StretchRect` -> `CopyResource` / blit |
| `IFrameCompositor` / `FrameBracket` types | GENERALIZE: `IDirect3DDevice9Ex*` / `IDirect3DSurface9*` -> an abstract surface/device handle (or a parallel D3D11 compositor hierarchy) |
| present (`PresentEx`) | REPLACE: DXGI swap chain `Present` + `GetFrameStatistics` |

The brain (scheduling, policy, ring selection, flow/NVOFA) is reused; the render primitives are
standard D3D11 quad draws. The one architectural task is the `IFrameCompositor`/`FrameBracket` type
generalization. This is a render-backend reskin, not a rewrite.

## Present side: the second honest clock

- Flip-model swap chain (`DXGI_SWAP_EFFECT_FLIP_DISCARD`) on the XR1 output. `Present(1, 0)` for
  vsync-paced (matches today's DWM-clocked 60), or an `IDXGISwapChain2` frame-latency waitable object
  for tighter scheduling driven by `PresentScheduler`.
- `GetFrameStatistics` -> `DXGI_FRAME_STATISTICS { SyncQPCTime, PresentCount, PresentRefreshCount }`.
  `SyncQPCTime` is the QPC of the vblank the present actually hit: log it as the real present time
  instead of inferring `pdt`/`jit` from QPC around the present call.
- Optional future: `DXGI_PRESENT_ALLOW_TEARING` for a low-latency / VRR XR1 path (today it is
  DWM-clocked 60).

## Two-device model

Keep the capture/present device split from the dual-device spec, now in D3D11: a capture device bound
to the SOURCE output's adapter (DD/WGC require the output's adapter) and a present device on the XR1's
adapter, ring textures shared by keyed-mutex shared handles. The dual-device shared-handle work
carries over; it becomes D3D11 shared textures (`D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX`) instead of
D3D9 shared surfaces.

## Gotchas / open questions

- **Independent-flip access loss (DD):** the Phase-0 coverage gate. Very likely why WGC wins.
- **G-Sync / VRR honesty:** confirm `LastPresentTime` / `SystemRelativeTime` report the VRR present
  cadence truthfully, not a rounded refresh grid.
- **Timestamp semantics:** `LastPresentTime` = present time; `SystemRelativeTime` = capture-available
  time. Confirm the chosen one tracks true content cadence (Phase-0 diff against a known-rate source).
- **Format:** the XR1 present is ARGB10 today; the swap chain format must match
  (`DXGI_FORMAT_R10G10B10A2_UNORM`, or scRGB `R16G16B16A16_FLOAT` if HDR). Marker cells stay luma,
  fraction-positioned; just a D3D11 draw.
- **Cursor / protected content:** DD/WGC cursor handling vs the relay's content-only intent
  (NvFBC `bWithHWCursor = 0`); protected-content passthrough.
- **Backend availability:** WGC needs a recent Windows build; DD needs the output not to be
  independent-flipped. NvFBC mode stays the default until DXGI mode is proven, and remains the only
  option for a true exclusive-fullscreen source.

## Validation

1. **Phase 0 passes:** honest, present-aligned timeline on a backend that reliably captures the source.
2. **A/B the phantom blends (the money metric):** same content, NvFBC mode vs DXGI mode. The
   map-close 100 ms "stall" blends should DISAPPEAR in DXGI mode, because frames stamped with true
   present time show no phantom gap, so the policy passes through instead of blending. Quantify the
   drop in synth-during-stall.
3. **Present-side check:** `GetFrameStatistics.SyncQPCTime` vs `PresentScheduler`'s scheduled
   deadlines: the real present timeline should match the intended cadence within jitter.
4. **Smooth Motion:** the "keep the second (real) member" batch-collapse workaround should become
   unnecessary, because real present times disambiguate the gen/real pair directly rather than by
   arrival-order heuristic. Confirm on an FG source.
