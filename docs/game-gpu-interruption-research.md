# Interrupting the game's GPU less

Status (2026-10-21): a present with no GPU work behind it does not take the game's graphics
engine, and neither does a picture copied on the copy engine and presented through the
composition swapchain (measured with the probe under the benchmark, below). Built in the relay as
`-xcopyengine` (`docs/copy-engine-present-spec.md`): the relay's visits fell from 60 to 19 a
second and its cost by 36.7 +- 2.2 points (134.8 to 98.2). The question (the user):
"is there really no way around this? some way to let the GPU keep going on the game while we
present or something? some way to avoid interrupting it?" and "I want to be certain we've really
gone down all of the paths to reduce the interruption to the game processing."

## What the cost is

GPU traces of the 5080's graphics engine (sittings `xtrace1`, 2026-10-20, and the 2026-10-15
traces in `performance-candidates.md` section 8), in a benchmark run of the game at full load:

- With no relay the game holds the engine about 192 times a second, 1.95 ms at a time, and loses
  11 ms a second to switching alone.
- With `-wgc` the relay holds it 61.6 times a second for 0.065 ms (one hold per present: the draw
  and the present go in together), and Windows' compositor, which does WGC's capture, 111 to 151
  times a second for about 0.1 ms. The game's work is cut into about 345 to 367 pieces a second,
  and it loses 55 to 66 ms a second to others, of which only about 22 is the relay's and the
  compositor's own work; the rest is the switching and the stretches around it.
- A present with nothing drawn still took the engine once in the relay (2026-10-15, `-xnodraw`),
  but a present with no GPU work at all does not (2026-10-20, the probe below). What in the
  relay's no-draw present took the visit is not known.

So the cost is the number of times the game is interrupted, much more than what is done in each
interruption. Every path below is judged by whether it cuts that number on the game's GPU.

## The paths

| path | what it would cut | status |
|---|---|---|
| One visit per present | the relay's draw and present in one submission | done: 61.6 holds a second at 60 presents |
| Present without drawing | the draw | the relay's no-draw present took a visit (2026-10-15); a bare present from the probe takes none, under load or not (2026-10-20) |
| NvFBC in place of WGC | the compositor's visits | measured: NvFBC costs more (about 290 points against WGC's 192 to 244) |
| A second GPU for the present | the relay's visits | measured 2026-10-20 (`docs/dual-gpu-spec.md` on the branch `performance-optimization-dual-gpu`): the relay's visits go, but the compositor's triple in length as it carries each frame across, +31.8 points net; the copy-engine design is tabled |
| Work beside the game (async compute) | the switching | not possible from another process: work from different processes takes turns on the engine; only a process's own queues run side by side (from how the GPU schedules work; not measured) |
| The copy engine | the relay's visit on every present that shows one frame as it is | **built as `-xcopyengine`: 36.7 +- 2.2 points less (sitting `ce1cost`), the relay's visits 60.1 to 19.0 a second** (`docs/copy-engine-present-spec.md`); a copy on a D3D12 copy queue into a displayable buffer, presented through the composition swapchain, takes no visit (below); a copy queue cannot write a DXGI swapchain's own buffers (the device is removed), so the composition swapchain is what makes it possible |
| The composition swapchain (presentation manager) | the present's own visit | a present through it takes no visit, but neither does a DXGI flip present with no work; on its own it saves nothing, its use is as the target of a copy-engine copy |
| The relay's GPU scheduling priority | where and how the switches land | untested; used by capture hosts (Looking Glass, Sunshine and its fork Apollo); below |
| Fewer compositor visits | the compositor's visits | the least time between WGC frames has to stay at 0.001 ms or the real frame of a Smooth Motion pair is lost (`wgc-capture-spec.md`); dirty-region reporting does not change what the compositor copies (an API contract 19.0 feature; no cost data found) |
| The pointer left out of the picture | the compositor drawing the pointer | measured 2026-10-14: a moving pointer costs about 145 points with it in, which was accepted; `-nowgccursor` exists |
| Hardware-accelerated GPU scheduling off | the scheduler's behaviour | no reliable evidence either way (forum and blog tests); measurable with two sittings and a reboot between |
| Timing the relay's work into the game's own gaps | the cost of each switch | an idea, not a known mechanism: the trace shows about 170 short gaps a second where nothing holds the engine; the composition swapchain's target times would let the relay draw and present early and be shown on time |
| A hook inside the game (as OBS's game capture) | all of the capture's visits | the cheapest capture measured (4.0% against 5.3%, 2026-10-09), because its copy runs inside the game's own work; out of scope: injection into games, with anti-cheat risk |

## The composition swapchain probe

`PresentProbe` (outside the repo) covers the display that is not the main one with a window and
presents pre-made pictures with no drawing, in three modes that are each other's controls: `flip`
(a DXGI flip-model swapchain as the relay's, nothing drawn), `comp` (the composition swapchain,
its surface bound to the window through DirectComposition, three pictures in turn), `idle` (no
presents), and `flipclear` (alternating greys drawn, a control for the card capture). The
interfaces are declared from the SDK's `Presentation.idl`, since the MinGW headers have none.

Later modes (2026-10-20, evening): `flipcopy` (a D3D11 CopyResource of a ready picture into each
back buffer), `q12copy` (a D3D12 swapchain whose back buffers a copy queue fills; fails, below)
and `q12comp` (the composition swapchain's buffers opened on a D3D12 device and filled by its copy
queue before each present, the D3D11 device waiting on the copy's shared fence).

On the rig (2026-10-20, the card on the AMD output): the composition swapchain is supported,
with independent flip. From the AMD GPU its presents go to scanout as independent flips, 60 a
second; from the 5080 to the AMD output Windows composes them with a copy across. The card shows
every present's picture (the three greys in turn, read from the card capture). Whether it avoids
the present's visit on the game's GPU needs the card display on the 5080 again, a trace of `flip`
against `comp` against `idle`, and the probe's and the compositor's holds counted.

### Measured with the card display on the 5080 (2026-10-20, evening)

The card display back on the RTX 5080 at 60.000199 Hz (a display override made with CRU). Each
row a 10 s GPU trace of the 5080's graphics engine (its context switches), the probe running
beside the game in a benchmark run (one launch per sitting, no relay), or on the idle desktop:

| row | the probe's presents | the probe's holds a second | the game's time lost to the probe |
|---|---|---|---|
| idle (window, no presents) | 0 | 0 | 0 |
| flip, nothing drawn | 60 a second | **0** | 0 |
| comp, nothing drawn | 60 a second | **0** | 0 |
| flip, a clear before each present (control) | 60 a second | 59.9 | 12.9 ms a second (0.177 ms a stretch) |
| flip, a render target view made each present, nothing drawn | 60 a second | 0 | 0 |
| flip, a D3D11 CopyResource into each back buffer | 60 a second | 60.0 | about 19 ms a second |
| comp, a D3D12 copy-queue copy into each buffer, D3D11 waiting on its fence | 60 a second | **0** | **0** |
| the same without the fence wait | 60 a second | 0 | 0 |
| the relay, `b:vsync -src 60 -wgc` on `6d17691` (control) | 60 a second | 60.0 | 14.0 ms a second |

In the copy-queue rows the game held the engine 983.6 and 983.8 ms a second, as in the rows with
nothing beside it (983.4 to 983.8), against 972.5 and 966.2 with a clear or a D3D11 copy. The card
showed every present's picture in every mode that changes it (two greys alternating each refresh,
each frame a single grey: no torn frames). On the idle desktop the compositor's context stays on
the engine about 988 ms a second and hides other work, which is why the question needed the game
running.

A D3D12 copy queue's copy into a D3D12 flip-model swapchain's own back buffer removed the device
on the first present (`DXGI_ERROR_DEVICE_REMOVED`, reason `DXGI_ERROR_ACCESS_DENIED`).

Every benchmark run of 2026-10-20 evening scored about 4,000 (3,974 to 4,008 without the relay),
where the traced runs of the morning scored 5,258 to 5,261: the game's GPU time per frame was
14.1 ms against 10.8, about a third more in every pass, with the same settings file and settings
screens. A reboot the next morning cleared it (no relay 5,397 and 5,398, GPU time 10.5 ms a
frame); it was state left by that evening's display and driver changes, which one is not known.
The visit counts compare inside a launch and do not depend on it.

A slip found on the way: the card captures taken with a frame skip (`select`) and raw output
repeated one frame unless the frame timing is passed through (`-fps_mode passthrough`); the
alternating-grey control showed it. The capture tools were fixed; captures before 2026-10-20 hold
one frame repeated, which none of their findings relied on.

## GPU scheduling priority

Two calls set how the scheduler treats the relay's work against the game's:
`IDXGIDevice::SetGPUThreadPriority` (-7 to 7 for a device) and
`D3DKMTSetProcessSchedulingPriorityClass` (a class for the process: idle, below normal, normal,
above normal, high, realtime). Looking Glass reported that its capture host, raised with the
second call, got its frames in time and cost the foreground application less (no numbers; run as
SYSTEM). Sunshine sets it; its fork Apollo uses high rather than realtime on NVIDIA, after realtime
froze or crashed the driver (also reported on NVIDIA's forum with NVENC under HAGS). Realtime needs
the right to raise scheduling priority, which administrators hold by default; the relay runs
elevated. Either direction could help the game: higher, so the relay's short work is not left
waiting and finishes in one switch; lower, so it waits for a gap the game leaves. Neither changes
the number of visits. A switch and one sitting (both directions against none) would settle it.

## Sources

- [Composition swapchain overview](https://learn.microsoft.com/en-us/windows/win32/comp_swapchain/comp-swapchain) and [examples](https://learn.microsoft.com/en-us/windows/win32/comp_swapchain/comp-swapchain-examples); the interface definitions from [win32metadata's Presentation.idl](https://github.com/microsoft/win32metadata/tree/main/generation/WinSDK/RecompiledIdlHeaders/um)
- [Hardware flip queue (WDDM 3.0)](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/hardware-flip-queue); [DXGI flip model](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/for-best-performance--use-dxgi-flip-model)
- [GPU preemption (Windows drivers)](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/gpu-preemption); measured context switch costs in [The case for limited-preemptive scheduling in GPUs](https://www.repository.cam.ac.uk/bitstreams/ec0fee41-8504-42f5-9898-960a19da02da/download) and [GPU Preemptive Scheduling Made General and Efficient](https://www.usenix.org/system/files/atc25-fan.pdf)
- [IDXGIDevice::SetGPUThreadPriority](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgidevice-setgputhreadpriority); [D3DKMTSetProcessSchedulingPriorityClass](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/nf-d3dkmthk-d3dkmtsetprocessschedulingpriorityclass); [the realtime band's privilege](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgkarg_setcontextschedulingproperties)
- [Looking Glass: DXGI capture performance](https://forum.level1techs.com/t/news-dxgi-capture-performance/160459); [Sunshine's display code](https://docs.lizardbyte.dev/projects/sunshine/latest/classplatf_1_1dxgi_1_1display__base__t.html); [Apollo: high on NVIDIA](https://izackpgit.duckdns.org/izackp/Apollo/commit/6dfe56518cb8295e74e3e4aef0bc771a3c333c1e); [NVENC hangs in realtime mode](https://forums.developer.nvidia.com/t/nvenc-encoder-hangs-on-windows-when-using-d3d11-in-real-time-mode/357465)
- [GraphicsCaptureSession](https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture.graphicscapturesession) (dirty regions, least time between frames)
