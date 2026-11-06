# The relay's GPU work on D3D12

Written 2026-11-04. Not scheduled: no code exists for it. The user keeps it for the day the relay
supports GPUs from vendors other than NVIDIA. Section 6 lists the decisions it leaves to the user,
each with Claude's default.

## 1. Purpose

Two parts of the relay run only on NVIDIA GPUs: NvFBC capture and the optical flow engine behind
`o`. WGC (Windows.Graphics.Capture) already covers capture on any GPU. Flow is the harder part.
The vendor-neutral sources of motion are AMD's FidelityFX Optical Flow (MIT source, compute
shaders, D3D12 and Vulkan backends, no D3D11 backend) and the D3D12 video motion estimator
(`ID3D12VideoMotionEstimator`, a driver feature). Both need D3D12. This spec moves the relay's
drawing and flow onto one D3D12 device so that a flow backend can be picked per vendor, with the
present and its pacing left as they are.

Vulkan was considered and is not planned (section 7). Appendix A has the background: every way a
GPU vendor offers to make an in-between frame, as found on 2026-11-04, and which of them a relay
can use.

## 2. What the relay is today

Checked in the tree at `2044bc4`, 2026-11-04.

| piece | API | where |
|---|---|---|
| NvFBC capture | NvFBC's `ToDX9Vid` into a D3D9 surface, one `StretchRect` into a shared ring slot | `NvFBCSession`, `CaptureRing` |
| WGC capture | frame pool on a D3D11 device of its own (`-nowgcopen` puts it on the present's); a ring slot names the pool texture its frame is in, with no copy | `WgcCaptureSource.h` |
| Present, drawn | D3D11: passthrough, blend, `-blend` shaders, nearest, scaler, flow warp, marker, `-tint` | `D3D11Present.cpp`, `BlendShaders.cpp`, `WgcScaler.h`, `FrameMarker.cpp` |
| Present, copied | a D3D12 copy queue copies the WGC pool texture into a composition swapchain buffer, shared fence, then the presentation manager presents | `CopyEnginePresent.cpp` |
| Optical flow | NVOFA through its D3D11 interface; the luma draw, the warp and the cut gate's means are D3D11 shaders | `OpticalFlowWarp.cpp`, `FlowWarpShader.h` |
| D3D9 present | `t:dwm`, `b:dwm`, `-d3d9` | `D3D9Present.cpp`, `D3D9Setup.cpp` |
| Shaders | HLSL compiled at run time with `D3DCompile`, `vs_5_0` and `ps_5_0` | each compositor |
| Pacing, statistics, ETW | DXGI and the presentation manager; no GPU API of its own | `Pacing.h`, `EtwConsumer.cpp` |

About 8,700 lines in 13 files touch D3D11 directly. The largest are `FlowProbeMode.cpp` (1,911),
`D3D11Present.cpp` (1,250), `CaptureRing.cpp` (1,222), `WgcCaptureSource.h` (702),
`OpticalFlowWarp.cpp` (670), `BlendShaders.cpp` (661) and `CopyEnginePresent.cpp` (607).

D3D12 is already in the process: the copy engine present opens WGC pool textures on a D3D12 device
through NT shared handles, copies on a copy queue and signals a shared fence that the presenting
D3D11 device waits on. `d3d12.dll` is loaded at run time, so the import list does not name it.
The policy step never sees the present path (`docs/present-step-spec.md`), so none of this
touches `src/core`.

## 3. Measurements that bear on it

All on the rig's RTX 5080; nothing here has run on another vendor's GPU.

- **NVOFA through D3D12 costs what it costs through D3D11** (2026-10-24, the flow engine probe,
  60 bidirectional 1440p flows a second beside a GPU-bound benchmark): D3D11 without a completion
  query 3.59% of the game's frames, D3D12 3.61% and 3.68% in two sittings. Each call into the
  engine took two turns on the game's graphics engine on either API, and the game lost about
  0.4 ms of engine time per flow (23.7 ms a second at 60 flows). Through D3D12, a fence said when
  each flow was done: median about 4 ms after the call under load, 2.73 ms idle. The D3D12 loop
  and the D3D12 header were removed afterwards; commit `d1856fd` has them
  (`docs/optical-flow-interpolation-spec.md`, 6.1).
- **What runs NVIDIA's flow on Blackwell is not documented.** NVIDIA's Blackwell whitepaper does
  not mention the optical flow accelerator and says DLSS 4 frame generation replaced hardware
  optical flow with an AI model. The Optical Flow SDK still works. On the 5080 (2026-10-23) the
  probe's own work held the game's graphics engine about 0.25 ms per flow, where a flow on the
  shader cores would hold it for the flow's whole 2.7 ms, and the engine's timings tracked
  NVIDIA's published Ada figures scaled by pixel count. So the flow work does not run on the
  game's graphics engine as the GPU trace sees it. Whether that is a fixed-function unit, or a
  model on hardware the trace does not show, is not known. The reading rests on NVIDIA's model
  that shader work from another process takes a turn on the game's engine.
- **In `o` itself**, a synthesized present cost the game about 0.4 ms of engine time more than the
  blend (capped 90 fps, 2026-10-24): the warp draw and the flow's hand-off together. GPU-bound, `o`
  cost 85 points (1.45%) over the blend (2026-10-25). This is the bar any other backend is judged
  by.
- **A D3D12 copy queue present takes no turn on the game's graphics engine**
  (`docs/game-gpu-interruption-research.md`): 4,507 copies and presents in a traced run, the game
  holding the engine as with no relay beside it. It is the reason the copy engine present exists
  (`docs/copy-engine-present-spec.md`).
- **NvFBC's D3D9 ring slots open on D3D12** (`-xnvcopy`, removed in `268da9a`): about 94% of
  presents copied, but it saved nothing, because a context that exists only in that mode took the
  graphics engine 60 times a second.
- **The GPU timers miss cost.** A compute shader version of the cut gate timed 6 us a pair and
  cost 0.13% of a benchmark's frames, about ten times what its timers showed (2026-11-04). Every
  cost in this spec is judged by an uncapped benchmark score with partnered rows.

## 4. Design

### 4.1 Capture stays D3D11

The WGC frame pool takes a D3D11 device; Microsoft documents no D3D12 device for it. The pool stays
on its D3D11 device of its own, as now, and the D3D12 device opens each pool texture through an NT
shared handle, as the copy engine present does. The copy engine spec's risk applies to every
present then: D3D12 gets no implicit synchronisation of a texture shared with another device. The
copy path's checks of 2026-10-20 found no part-written picture: a still desktop equal byte for
byte to the drawn path, and a source flipping between two greys about 238 times a second with
every card frame a single grey. The same checks gate this move (section 5).

NvFBC stays on its D3D9 ring and the D3D11 present. The D3D12 path takes `-wgc` launches only.
Nothing new depends on NvFBC, and NvFBC through D3D12 has already been tried with no saving.

### 4.2 One D3D12 device, three queues

| queue | work |
|---|---|
| copy | passthrough presents, the marker strip (as today) |
| direct | blend, `-blend` shaders, nearest, scaler, flow warp, `-tint`, the marker on drawn presents |
| compute | FidelityFX flow and the cut gate's means, when that backend runs |

The queues order their work with fences. The relay records every barrier itself. D3D11 decided
these for the relay until now, and the 2026-10-24 ordering bug (a warp reading a stale flow until
the fields carried the render-target flag) shows how an ordering mistake looks: a picture one flow
old with no error. Every barrier gets a probe check of that kind.

The presenting device stays D3D11, made `SINGLETHREADED | PREVENT_INTERNAL_THREADING_OPTIMIZATIONS`
as the presentation manager requires. It owns the composition swapchain's buffers and draws
nothing, as now. D3D12 writes the buffers through shared handles and signals a shared fence the
presenting device waits on before each present. Today only the copy path works this way; the
move extends it to every present. Whether the presentation manager accepts a D3D12 device
directly was not checked; the design does not need it to.

`-nocopyengine` today draws onto a DXGI flip-model swapchain. On the D3D12 path every present goes
to the composition buffers, so the option has nothing left to change there (decision 6.3).

### 4.3 Flow backends

One interface, `IFlowSource`: hand over a pair, get a field of motion vectors with its cell size,
and a fence value that says when it is done. The warp (`FlowWarpMath.h`, the per-cell fixed-point
solve) already works on cells; the cell size becomes a parameter, where today it is NVOFA's grid of
4. The cut gate reads mean luma, which every backend can feed.

| backend | vendors | cells | notes |
|---|---|---|---|
| NVOFA, D3D12 interface | NVIDIA, Turing and later | 4x4 (grid 4) | MIT headers, driver runtime; the header and a working loop are in `d1856fd` |
| FidelityFX Optical Flow | any GPU with SM 6.2 | 8x8 blocks on a luma pyramid | MIT source, ships as compiled shaders; runs on the shader cores the game uses; has its own scene-change detection |
| D3D12 video motion estimator | drivers that report it (`CheckFeatureSupport`, `D3D12_FEATURE_VIDEO_MOTION_ESTIMATOR`) | 8x8 or 16x16 | the video encoder's motion search; Microsoft's 2021 announcement lists NV12 input only, so it needs a conversion pass; forum reports from 2021 and 2022 have it working on NVIDIA and AMD and not on Intel |

Default order: NVOFA on NVIDIA. Elsewhere, FidelityFX or the motion estimator, whichever step 3
finds cheaper for the game at a usable quality. The estimator runs on the video encoder's motion
search, so on a driver that really runs it there it may stay off the game's shader cores, as NVOFA
does; FidelityFX cannot.

Two vendors also ship an interpolator that takes plain frames and returns the frame in between.
These skip the relay's own warp, so they sit behind a second interface, `IFrameSource`: hand over a
pair and a position between them, get a picture and a fence value.

| backend | vendors | API | notes |
|---|---|---|---|
| AMF FRC (frame rate conversion) | AMD | AMF, on D3D11 or D3D12 | FFmpeg's `frc_amf` filter uses it; it doubles the frame rate, and an option trades latency for quality by using a later frame; AMF's own documentation of the component was not read |
| Intel VPL AI frame interpolation | Intel | Intel VPL, spec 2.12 and later | the caller sets the input and output frame rates; Intel names no supported GPUs; built for video, so its delay and whether it can make one frame at a time on demand are not known |

Neither fits the relay's step as it is: the policy picks the position of each synthesized present,
and a frame rate converter picks its own. Each is worth a probe on its vendor's GPU (step 4)
before any design: can it return a frame for a chosen position, in what time, at what cost to the
game. The cut gate and the passthrough rules stay the relay's either way.

### 4.4 Shaders and the build

The relay's own shaders can stay HLSL compiled at run time: D3D12 takes `vs_5_1` and `ps_5_1`
bytecode from the same `D3DCompile` in Windows, with a root signature added to each. FidelityFX
needs shader model 6.2, which only DXC compiles. Its shaders would be compiled at build time and
carried in the exe, which adds DXC to CI. The zig pre-check on the Mac would need a DXC it can run;
not checked.

`d3d12.dll` stays loaded at run time, so the import baseline does not change.

### 4.5 What does not change

The policy step, `Pacing.h` (the wait on the composition buffer's available event, the timer's
tight wait), the present statistics, ETW, the frame marker's layout and the log lines. A refactor
gate here is identical output: the same picture at every present, the same log apart from the
lines that name the device.

### 4.6 Picking the vendor

The relay reads the output adapter's vendor ID at start. On a GPU other than NVIDIA:

- a launch without `-wgc` stops with a message that NvFBC needs an NVIDIA GPU and names `-wgc`
  (or `-wgc` is implied; decision 6.4);
- `o` runs with FidelityFX;
- the log names the adapter, the vendor and the flow backend.

On NVIDIA, `-flow fidelityfx` (hidden) runs the FidelityFX backend for comparison.

## 5. Order, each step with its gate

0. **Prerequisite: the D3D9 present is removed** (planned after a release; spec
   `docs/d3d11-present-unification-spec.md` 7.3 item 6). Moving while D3D9 is still in the process
   would put three graphics APIs in one launch.
1. **The D3D12 present backend for `t`, `b` and the timer modes**, drawing on the direct queue into
   the composition buffers, behind a hidden `-d3d12`. Gate: on the desktop, every present's picture
   equal byte for byte to the D3D11 path's; a marked KCD run with every counter +1 in the video;
   the policy suite identical; cost in a GPU-bound benchmark, three rows a build, balanced, the
   decision rule written before the data. Control: a deliberately missing barrier must show up as
   a stale or torn picture in the byte check.
2. **NVOFA through D3D12 and the warp ported**, with the cut gate. Gate: the flow probe's
   self-test on D3D12 (exact flow on the generated frames, warp equal to the CPU reference, the cut
   gate's cases); the probe's ordering check across the copy, direct and compute queues; ten map
   opens in KCD filmed (every one a fade); cost against the D3D11 `o`, partnered and balanced.
3. **FidelityFX and the motion estimator on NVIDIA first**, behind `-flow fidelityfx` and
   `-flow estimator`. Gate: flow quality next to NVOFA on the same frame pairs (the probe's square
   and recorded game pairs, endpoint error per cell); each one's cost uncapped beside NVOFA's,
   partnered and balanced; the GPU trace showing which engine each runs on. This is where it shows
   whether flow off NVOFA is affordable at all. NVIDIA's own estimator says nothing about AMD's,
   so the estimator is measured again in step 4.
4. **A GPU from another vendor** in a test machine. Gate: the whole mode table on the desktop, a
   game capture with the marker, the copy queue present checked for turns on the graphics engine
   with that vendor's trace tools (the 5080's result says nothing about AMD or Intel). On that GPU,
   the vendor's whole-frame interpolator gets its probe (4.3).
5. **The D3D11 path removed** after a release with the D3D12 path as the default (decision 6.2).

## 6. Decisions for the user

Claude's defaults; the user owns each.

1. **Whether NVIDIA moves to D3D12 too.** Default: yes, once step 2's cost matches D3D11. One
   present path to maintain, as with the D3D11 unification.
2. **The D3D11 path as a fallback.** Default: kept one release as a hidden option, then removed,
   as was done for D3D9.
3. **`-nocopyengine` on the D3D12 path.** Default: it draws passthrough presents on the direct queue
   (the copy queue's work moved to the graphics engine), so it keeps a meaning as a diagnostic.
4. **A non-NVIDIA launch without `-wgc`.** Default: imply `-wgc` and say so in the log, since it is
   the only capture that works there.
5. **The test GPU.** Steps 4 and 5 need an AMD or Intel GPU in a machine the relay can run on.
   Which one, and where, is open.
6. **The flow backend off NVIDIA.** Default: whichever of FidelityFX and the motion estimator step
   3 finds cheaper at a usable quality, with the other dropped.
7. **The vendors' whole-frame interpolators** (AMF FRC, Intel VPL). Default: a probe each on its
   vendor's GPU in step 4, built into the relay only if it beats the relay's own warp there.

## 7. Not in this spec

- **Vulkan.** The relay is Windows-only (WGC, NvFBC, DWM, ETW), so Vulkan's other platforms buy
  nothing. A Vulkan swapchain presents through each vendor's own window-system layer, which hides
  the composition swapchain, independent flip and the buffer events the pacing waits on. Its only
  optical flow extension found is NVIDIA's (`VK_NV_optical_flow`, not ratified).
- **NvFBC through D3D12** (tried, no saving; section 3).
- **The game and the relay sharing one GPU.** D3D12 does not change how the GPU switches between
  the two processes, which is most of the relay's cost on the present (the cost breakdown in
  `docs/performance-candidates.md`). No cost saving is predicted from the API change itself.
- **The second GPU** (`docs/dual-device-capture-present-spec.md`), tabled 2026-10-20.

## 8. Risks

- A barrier or fence missing in one path shows as a stale picture with no error. Each queue
  hand-off needs a probe check with a control that fails when the barrier is taken out.
- FidelityFX runs on the shader cores. Its cost on the game is unknown until step 3, and AMD's own
  figure (the whole frame generation pass, 1.0 ms at 1440p on an RX 7900 XTX) is for a different
  GPU and a different job.
- WGC's 0.001 ms least time between frames, the composition swapchain's independent flip
  (`IsPresentationSupportedWithIndependentFlip`) and the copy queue's free present are checked only
  on NVIDIA. Each is a check in step 4.
- FidelityFX's 8x8 cells are coarser than NVOFA's 4x4. The warp's edges and the residual
  visibility test were tuned on 4x4 cells.

## Appendix A. Where an in-between frame can come from (2026-11-04)

The relay sees only the pictures the game displays. Each vendor's frame generation for games is
built into the game, which hands it data the relay never has. What remains for a relay is a short
list.

**Frame generation inside a game.** None of these can run on captured frames.

| technology | vendor, GPUs | what the game hands it | notes |
|---|---|---|---|
| DLSS frame generation | NVIDIA, RTX 40 and later | depth, motion vectors, camera data, through Streamline | since DLSS 4 an AI model on the tensor cores; NVIDIA says it replaced hardware optical flow |
| FSR 3 frame generation | any GPU with shader model 6.2 | depth, motion vectors | MIT source; its optical flow pass alone takes plain frames (below) |
| FSR frame generation 4 (ML, AMD's "Redstone") | AMD, RDNA 4 only | depth, motion vectors, plus its own flow estimate | older GPUs get FSR 3's analytical version. FSR 4's source went up on GitHub by accident with FidelityFX SDK 2.0 in August 2025 and was taken down; the trained models are a separate question from the code's license, and nothing from it is used here |
| XeSS frame generation | 2x on any vendor since SDK 2.1.1; 3x and 4x on Intel Arc only | depth, motion vectors; XeLL, Intel's latency reduction, built into the game's render loop | the SDK ships as binaries |

**Frame generation in the driver.** NVIDIA's Smooth Motion and AMD's Fluid Motion Frames take any
DirectX 11 or 12 game, with no API for another program. Intel's frame generation override applies
only to games that already ship XeSS frame generation.

**From plain frames.** The relay's candidates:

| source | vendors | runs on | output | status here |
|---|---|---|---|---|
| NVOFA, Optical Flow SDK 5.0 (D3D11, D3D12, CUDA; Vulkan through `VK_NV_optical_flow`) | NVIDIA, Turing and later | off the game's graphics engine on the 5080 (section 3) | motion vectors, 4x4 cells | `o` today |
| NVIDIA's FRUC library | NVIDIA | NVOFA plus CUDA | whole frames | rejected on quality (2026-09-22); its DLL cannot ship in this repository |
| NVIDIA Maxine Video Frame Generation | NVIDIA, Ada and later | shader and tensor cores | whole frames | cannot ship in an MIT repository |
| FidelityFX Optical Flow | any GPU with shader model 6.2 | shader cores | motion vectors, 8x8 blocks | 4.3 |
| D3D12 video motion estimator | drivers that report it | the video encoder's motion search | motion vectors, 8x8 or 16x16 blocks | 4.3 |
| AMF FRC | AMD | not documented in what was read | whole frames | 4.3 |
| Intel VPL AI frame interpolation | Intel | not documented in what was read | whole frames | 4.3 |
| RIFE (a neural interpolator, MIT code and weights) | any, through TensorRT or ncnn | shader cores | whole frames | a quality reference for offline comparison |

**By vendor, for a relay.**

- **NVIDIA:** NVOFA, through a public API, off the game's shader cores. Nothing else is needed.
- **AMD:** no flow engine with a public API was found. AMF FRC gives whole frames on AMD's own
  terms (its frame rate, its positions). FidelityFX Optical Flow is AMD's but runs on any GPU's
  shader cores. FSR 4's ML frame generation needs the game's data and RDNA 4. The D3D12 motion
  estimator worked on AMD in a 2021 report.
- **Intel:** no flow engine with a public API was found either. VPL's AI frame interpolation gives
  whole frames on Intel GPUs, and Intel names none. XeSS frame generation needs the game's data
  and XeLL in the game's loop. The D3D12 motion estimator did not work on Intel in the 2021 and
  2022 reports.

So off NVIDIA, a relay that wants its own warp has two choices that run on any vendor (FidelityFX
on the shader cores, the motion estimator where a driver offers it), and a relay that accepts a
vendor's whole frames has one per vendor, each built for video.

The column that decides cost is where it runs. A source on the shader cores competes with the game
for the same units, so its whole time comes out of the game's frame. NVOFA, and the motion
estimator if a driver runs it on the encoder, work outside them, and the game pays only for the
hand-off. `docs/optical-flow-interpolation-spec.md` (3.1 to 3.4) has the earlier survey these rows
extend.

## Sources

- Windows.Graphics.Capture frame pool:
  https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture.direct3d11captureframepool
- D3D12 video motion estimator:
  https://learn.microsoft.com/en-us/windows/win32/api/d3d12video/ns-d3d12video-d3d12_video_motion_estimator_desc ;
  driver reports: https://community.intel.com/t5/Graphics/DirectX-12-Motion-Estimator-API-support/m-p/1307843
- FidelityFX SDK: https://gpuopen.com/manuals/fidelityfx_sdk/
- NVIDIA Optical Flow SDK 5.0:
  https://docs.nvidia.com/video-technologies/optical-flow-sdk/nvofa-programming-guide/index.html
- `VK_NV_optical_flow`: https://docs.vulkan.org/spec/latest/chapters/VK_NV_optical_flow/optical_flow.html
- RTX Blackwell whitepaper:
  https://images.nvidia.com/aem-dam/Solutions/geforce/blackwell/nvidia-rtx-blackwell-gpu-architecture.pdf
- FFmpeg's AMF frame rate conversion filter: https://www.ffmpeg.org/doxygen/trunk/vf__frc__amf_8c.html
- Intel VPL AI frame interpolation:
  https://community.intel.com/t5/Blogs/Tech-Innovation/Tools/Intel-VPL-Unveils-AI-Powered-Video-Frame-Interpolation/post/1670736
- AMD FSR SDK 2.3 (frame generation 4.0.1, RDNA 4): https://gpuopen.com/learn/amd-fsr-sdk-2-3-blog/
- FSR 4 source published by accident: https://www.guru3d.com/story/amd-accidentally-opensources-fsr-4-code-on-github
- Intel XeSS: https://www.intel.com/content/www/us/en/developer/topic-technology/gamedev/xess.html ;
  the XeSS 3 SDK as binaries:
  https://www.tomshardware.com/pc-components/gpu-drivers/intel-shares-xess-3-0-sdk-for-game-devs-with-3x-and-4x-mfg-modes-but-it-still-hasnt-followed-through-on-its-open-source-promise ;
  the driver override:
  https://www.tomshardware.com/pc-components/gpu-drivers/intel-enables-xess-3-multi-frame-generation-in-latest-drivers-expanding-frame-generation-across-arc-gpus-and-core-ultra-igpus-mfg-can-be-enabled-across-any-title-with-xess-2-support
