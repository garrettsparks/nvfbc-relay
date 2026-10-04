# Sink-Paced Present via DXGI Flip Model — Design Spec

Status: **design.** The pacing half is gated on a measurement that has not been taken (see
"Prerequisite"): whether DWM will promote the relay's window to independent flip is unknown and
unknowable by argument, and is estimated at roughly even odds. The *measurement* half is not
gated — flip model yields present statistics in windowed mode where the current bitblt swapchain
yields zeroes, so the port pays for itself with an in-process downstream-dupe counter even if
promotion never happens. Build order should reflect that: flip-model swapchain first, read the
statistics, and treat independent flip as the upside rather than the premise.
Companion documents: `dual-device-capture-present-spec.md` (the two-device ring this builds on,
and the `WaitForVBlank` / exclusive-fullscreen failures that constrain it),
`etw-frame-timing-spec.md`, `stage7-generated-frame-spec.md`.

## Purpose

Present on the **capture card's own vblank** instead of on DWM's compose clock, by getting the
relay's output window promoted to *independent flip* on the target output.

Today the relay presents with a windowed `D3DPRESENT_INTERVAL_ONE`, which does not wait on any
monitor's vblank: it throttles on DWM's compose clock. That clock is **regime-dependent** — it
follows whatever the source display is doing — and the relay inherits both its rate and its
jitter. The result is a per-title pacing lottery that no amount of policy work fixes, because
the problem is downstream of every decision the policy makes.

This is the fix that removes the regime dependence entirely rather than adapting to it.

## The problem, measured

Present cadence follows DWM, and DWM follows the source:

| source regime | compose rate | presents per source frame | measured downstream judder |
|---|---|---|---|
| KCD2 fullscreen, Smooth Motion x2 | 60 Hz | 1 | 0.12 repeat/s, 0.18 skip/s |
| Avatar, in-game DLSS FG x2 | 120 Hz | 2 | 2.07 repeat/s, 2.08 skip/s |
| desktop, no game | 240 Hz | 4 | (not streamed) |

KCD2 is smooth **by luck**: the fullscreen game takes the source display via independent flip,
which locks DWM's compose to 60 Hz, which happens to equal the content rate. One present per
frame, phase-locked to the content, and the 60 Hz sink samples one present per scan.

In-game frame generation breaks the coincidence. DLSS FG presents through the game's own
swapchain at the *displayed* rate, so compose runs at 120 Hz over a 60 fps content timeline. The
relay emits two presents per frame — a pass and its identical `hold-comb` twin — with 1.23 ms of
spacing jitter, and the sink's 16.67 ms sampling window then contains 1 present 15.4% of the
time, 2 presents 69.6%, and 3 presents 15.0%. Every "1" can show a frame twice; every "3" can
skip one. That is the measured 2/s repeat-and-skip pairing, and it is visible as stutter during
smooth camera pans.

**Everything upstream is already correct when this happens.** The game reports 120.00 fps with
one frame over 17 ms in an entire benchmark run; ETW shows a clean 120 flips/s with zero gaps
over 16.7 ms; NvFBC delivers; the tooth guard holds synthesis at ~0/s. The relay is doing
everything right and the output still stutters, because the last hop is paced by a clock that
belongs to someone else.

## Why not the alternatives

Each of these was tried or modelled, and each is refused for a specific measured reason.

- **`WaitForVBlank` on the target output.** Tried, failed definitively: pacing a loop on a clock
  that does not deliver the flips produced a two-crystal beat and 6→102 dupes
  (`dual-device-capture-present-spec.md`). The rule it produced — *never pace a loop with a
  clock other than the one delivering the flips* — is why the present spec below uses
  independent flip rather than a vblank wait. Independent flip does not violate that rule, it
  **makes the sink's vblank the flip-delivering clock**.
- **Exclusive fullscreen on the capture card.** Not honored by the card's driver
  (`INTERVAL_ONE` returns in ~200 µs) and dies on `S_PRESENT_OCCLUDED` the moment the relay
  loses foreground. Forbidden for a background relay.
- **Timer present (`b:60`, `b:120`).** `b:60` was captured and is worse in the field; modelling
  reproduces it (21 repeat/s) because a free-running 60 Hz timer beats against both the content
  clock and the sink. `b:120` models at roughly half today's judder on Avatar but **ten times
  worse on KCD2** (0.3/s → 3.0/s), because it introduces the two-presents-per-frame doubling
  that Avatar suffers from and KCD2 does not. A mode that is right for one title and wrong for
  another is a per-game profile, which is explicitly out of scope.
- **Phase-locking the present clock to head-1 ETW flips.** Modelled *worse* than free-running
  (3.90 vs 1.72 repeat/s). The sensor is the problem: head-1 flip timestamps carry ~1.5 ms of
  noise the real scanout does not have, and a control loop chasing a noisy reference injects
  that noise into the output.

## Mechanism

A **flip-model swapchain** whose window covers an output can be promoted by Windows to
*independent flip*: DWM is removed from the present path and the display controller scans the
swapchain's buffers directly. In that state `Present(1, 0)` blocks on **that output's vblank**.

**This is not exclusive fullscreen, and must not be confused with it.** Nothing here calls
`SetFullscreenState`, performs a mode set, or takes ownership of the display. Independent flip
is a DWM optimization applied to an ordinary borderless window; it is granted silently, can be
revoked at any moment (a notification drawn over the window is enough), and the app is not told
either way — it must read `GetFrameStatistics` or an external tool to know. That asymmetry is
what makes the risk acceptable: exclusive fullscreen fails LOUDLY and fatally for a background
relay (`S_PRESENT_OCCLUDED` on every focus loss — see `dual-device-capture-present-spec.md`),
whereas failing to be promoted to independent flip degrades to being composed by DWM, which is
exactly the behavior the relay has today. The downside of trying is that nothing improves.

Independent flip is also the mechanism modern borderless-fullscreen games rely on, precisely
because exclusive fullscreen is hostile to anything that is not the sole foreground application.
It is the well-trodden path, not an exotic one.

The consequences are what make this worth building:

- Present rate becomes the XR1's refresh, **always** — 60/s, whatever the source display, the
  game, or the frame-generation technology is doing.
- Present phase is locked to the sink by construction, with no control loop, no estimator, and
  no noisy sensor.
- One present per sink scan means the sampling ambiguity that produces repeat/skip pairs cannot
  arise: there is no second present for the scan to choose between.
- KCD2 and every other title that works today is **unchanged by construction** (it is already
  effectively one present per 60 Hz scan).

## Prerequisite — the gate

**Do not build any of this until PresentMon says the relay's window is a candidate.**

PresentMon reports the presentation mode by name. `--process_name` repeats, so one run captures
the relay and the game together — which also confirms the compose-regime story from the source
side:

```
PresentMon.exe --process_name NvFBCR.exe --process_name <game>.exe \
               --output_file pm_<label>.csv --timed 120 --terminate_after_timed
```

Display tracking is on by default (`--no_track_display` disables it; do not pass it). Requires
membership in **Performance Log Users**, or run as administrator — without admin, PresentMon
does not get complete information for short-lived or cross-account processes.

Read the `PresentMode` column. Its documented values are `Hardware: Legacy Flip`,
`Hardware: Legacy Copy to front buffer`, `Hardware: Independent Flip`, `Composed: Flip`,
`Hardware Composed: Independent Flip`, `Composed: Copy with GPU GDI`, and
`Composed: Copy with CPU GDI`.

| observed for the RELAY window | meaning | action |
|---|---|---|
| `Composed: Flip` on a *flip-model* swapchain | eligible, DWM simply owns it — promotion is plausible | build |
| `Hardware: Independent Flip` or `Hardware Composed: Independent Flip` already | premise is wrong, the jitter is coming from somewhere else | stop, re-diagnose |
| `Composed: Copy with *` even after moving to a flip-model swapchain | the card refuses promotion, as it refused exclusive fullscreen | stop, direction dead |

The current build uses `D3DSWAPEFFECT_DISCARD` (bitblt model), which **cannot** be promoted to
independent flip, so today's reading establishes the baseline rather than the answer. The real
gate is whether a flip-model swapchain on this HWND and this card gets promoted — which needs
the smallest possible spike (a flip-model swapchain that presents black), not the full port.

## Architecture

The capture side does not change. `CaptureRing` already runs a private D3D9Ex capture device for
NvFBC and shares its ring slots to the present side by handle, and it already anticipates this:

```
// Shared handle of slot i, for opening the same texture on another API's device.
HANDLE SlotSharedHandle(int i) const { return m_ring[i].sharedHandle; }
```

    NvFBC → D3D9Ex capture device → ring slots (shared RTs)
                                        │  shared handle, SAME VRAM (alias, not a copy)
                                        ▼
                              D3D11 device → DXGI flip-model swapchain → XR1
                                              (independent flip ⇒ blocks on XR1 vblank)

### Why D3D11 and not D3D12

The capture side is D3D9Ex and cannot move: NvFBC's `NvFBCToDx9Vid` writes D3D9 surfaces. So the
present device must open a **D3D9Ex shared handle**, and that handle is a *legacy* shared handle,
not an NT handle. `ID3D11Device::OpenSharedResource` accepts legacy handles;
`ID3D12Device::OpenSharedHandle` is defined against NT handles produced by
`ID3D12Device::CreateSharedHandle`, with no documented path for D3D9Ex handles.

D3D12 also buys nothing for this workload — the entire per-frame render is one full-screen lerp
of two textures. Explicit command lists, fences, and descriptor heaps are cost without benefit.
Independent flip, `GetFrameLatencyWaitableObject`, and `GetFrameStatistics` are identical on
both APIs.

### API maturity and documented constraints

None of this is exotic. Flip model has been the recommended presentation path since Windows 8
and is what every modern borderless game uses; D3D9→D3D11 resource sharing is documented on
`ID3D11Device::OpenSharedResource` with an official code sample. The constraints are published,
and the relay already satisfies the ones that matter:

| documented constraint (D3D9 → D3D11 sharing) | our ring slots |
|---|---|
| 2D, 1 mip level, default usage, no MSAA | ✓ `CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, …, D3DPOOL_DEFAULT)` |
| format must be `R10G10B10A2_UNORM`, `R16G16B16A16_FLOAT`, or `R8G8B8A8_UNORM` | ✓ slots are `D3DFMT_A2B10G10R10` = `R10G10B10A2_UNORM` |
| bind flags must include SHADER_RESOURCE and RENDER_TARGET | ✓ created as render targets, read as SRVs by the blend pass |
| `ID3D11DeviceContext::Flush` required after updating a shared texture | new obligation on the capture side — see risks |

Note the format point is luck we should not squander: the *backbuffer* is `D3DFMT_A2R10G10B10`,
which has no DXGI equivalent and could not be shared. The ring chose `A2B10G10R10`, which can.
Any future change to the ring's format must stay on the allowed list.

Two further documented rules bear on the design: use **one flip-model swapchain per HWND**, and
do not target the same HWND with another API — so the D3D9 swapchain is replaced on this path,
not run alongside.

**Swap effect and presentation interval are orthogonal axes**, and conflating them wastes
experiments. Swap effect (`DISCARD` = bitblt, copy into DWM's redirection surface every present;
`FLIPEX`/flip = buffers shared with DWM, no copy) decides *how content reaches DWM*. Presentation
interval (`INTERVAL_ONE` blocks, `IMMEDIATE` does not) decides *when Present returns*. The relay
is `DISCARD` on every path today and always has been — `b:vsync`, `b:60` and `b:120` differ only
in interval — which is why every capture to date has zeroed present statistics and pays the
redirection copy. This spec changes the swap-effect axis and keeps `INTERVAL_ONE`.

That removed copy is a full-frame read+write per present that the relay currently pays
unconditionally. It is GPU work contending with NvFBC on the same device, which makes it a
candidate explanation for the undiagnosed `b:60` capture loss (20-28 arrivals/s lost against the
ETW flip stream). Not a claim — but the port tests it for free, and if arrival loss disappears
under flip model, that closes a question that has been blocking the timer-present direction.

**Format translation is not required.** D3D9 names packed formats MSB→LSB and DXGI names them
LSB→MSB, so `D3DFMT_A2B10G10R10` and `DXGI_FORMAT_R10G10B10A2_UNORM` describe the identical bit
layout (R in bits 0-9, A in 30-31) — the same relationship as `D3DFMT_A8R8G8B8` ≡
`DXGI_FORMAT_B8G8R8A8_UNORM`. No swizzle, no conversion pass, no shader cost. The reversed names
are a naming-convention artifact, not a channel-order difference.

### Present statistics — a payoff independent of promotion

This is what makes the port worth doing even at coin-flip odds on independent flip:

> "A flip model swap chain provides present statistics information in both windowed and
> full-screen modes. For bitblt model swap chains in windowed mode, all DXGI_FRAME_STATISTICS
> values are zeroes."

The relay is bitblt today, which is exactly why sink timing has had to be reconstructed from
head-1 ETW flips carrying ~1.5 ms of noise. On a flip-model swapchain, `GetFrameStatistics`
yields `PresentRefreshCount`, `SyncRefreshCount` and `SyncQPCTime` in windowed mode, and
Microsoft documents the detection rule directly: `PresentRefreshCount` equals `SyncRefreshCount`
when the app presents on every vsync, and *"if the actual PresentRefreshCount is later than the
expected PresentRefreshCount, a glitch has occurred."*

That is a **downstream-dupe counter measurable in-process**, replacing a workflow that currently
costs a marked video capture plus a marker decode plus content-step analysis. It arrives with
flip model whether or not DWM ever promotes us, and it would let every future pacing question be
answered from a log line instead of a 20-minute offline pass.

`GetFrameStatistics` returns `DXGI_ERROR_FRAME_STATISTICS_DISJOINT` on the first call and across
mode changes; that is a sequence break to handle, not an error.

### Present loop

Blocking `Present(1, 0)` is the pacing wait, exactly as `INTERVAL_ONE` is today — but the clock
it blocks on is the sink's rather than DWM's. `DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT`
plus `GetFrameLatencyWaitableObject()` is available as a cleaner alternative that yields a real
waitable `HANDLE` instead of borrowing a blocking call's side effect; it is preferred if the
straightforward form shows queueing latency. `SetMaximumFrameLatency(1)` should be set either
way — the default of 3 puts our frame behind two others.

### What the policy sees

Nothing changes. The bracketing lag, comb lock, dejitter, tooth guard, and generated-frame
substitution all operate on the same ring and the same target arithmetic. One consequence is
worth stating: with presents at the sink rate and the source at 60, the **comb lock should
engage again** (it cannot at 2× compose, where alternating targets produce alternating
±half-comb errors and the stability gate correctly refuses — measured `lk=0` throughout every
Avatar capture). The tooth guard remains armed and correct; it simply stops having mid-tooth
targets to demote.

## Scope

**In scope:**
- D3D11 device on the target adapter + DXGI flip-model swapchain on the existing HWND.
- Opening the 32 ring slots as `ID3D11Texture2D` via the existing `SlotSharedHandle`.
- Passthrough (`CopyResource`) and the blend compositor as an HLSL full-screen pass.
- A new mode string, `b:flip`, alongside `b:vsync` / `b:60`.
- Logging: presentation-mode evidence, `GetFrameStatistics` (`SyncQPCTime`,
  `SyncRefreshCount` vs `PresentRefreshCount`).

**Explicitly deferred** — these stay D3D9-only and are unavailable in `b:flip` at first:
- The interp / flow-warp compositor (NvOFFRUC is bound to the D3D9/CUDA path).
- The frame marker burn. **This matters**: `-mark` is the relay-vs-downstream discriminator, so
  until it is ported, `b:flip` captures cannot be attributed the way `b:vsync` captures can.
  Porting the marker is therefore the first follow-up, not an afterthought.
- `-fgphase` (instrument only, never the shipping path).

**Out of scope:** any per-title configuration. If this needs a per-game profile it has failed.

## Performance

A shared handle is an **alias**, not a transfer: the D3D11 texture is the same VRAM NvFBC wrote.
Per present:

| path | today (D3D9) | with `b:flip` (D3D11) |
|---|---|---|
| passthrough | `StretchRect` slot → backbuffer (1 copy) | `CopyResource` alias → backbuffer (1 copy) |
| blend | 1 shader pass → backbuffer | 1 HLSL pass → backbuffer |

No additional per-present copy, **provided compositing moves to D3D11 with the present**. The
tempting shortcut — keep compositing in D3D9, render to a shared surface, copy that into the
D3D11 backbuffer — costs a real extra full-frame copy every present (~8 MB, ~500 MB/s at 60 Hz).
That is small on this GPU but it is pure waste, and it is the version to refuse in review.

The relay-side cost question (GPU time taken from the game) is unchanged by this work and is
measured by `relay-cost-spec.md`.

## Risks, and how each is detected

| risk | detection |
|---|---|
| Independent flip never engages on this card (it already refused exclusive fullscreen) | PresentMon `PresentMode` never reads `Hardware: Independent Flip` — the prerequisite gate |
| Backbuffer format mismatch silently blocks promotion (current backbuffer is `D3DFMT_A2R10G10B10`; swapchain must match the output mode) | same gate; try `B8G8R8A8_UNORM` matched to the XR1's actual mode |
| Flip model may not accept a 10-bit swapchain format. The flip-model doc lists only `R16G16B16A16_FLOAT`, `B8G8R8A8_UNORM`, `R8G8B8A8_UNORM` — that text is Windows 8 era and `R10G10B10A2_UNORM` is believed supported on Win10+ (it is how HDR10 output works), but this is **unverified** | `CreateSwapChainForHwnd` fails or promotion never happens. If 10-bit is refused, dropping the output path to 8-bit is a quality decision the user owns — note the XR1 captures 1080p60 over HDMI and may be 8-bit downstream regardless |
| Shared-texture updates now require `Flush` on the writing device | missing flushes show as torn or stale content inside a slot, the same signature as the existing D3D9↔D3D9 sync risk |
| Overlays / notifications demote to composed mid-run | `PresentMode` changes during a capture; present rate jumps off the sink rate |
| D3D9Ex↔D3D11 shared surfaces have no cross-API sync primitive | already a known risk between the two D3D9Ex devices today (`CaptureRing.h`); shows as tearing or partial content *inside* a slot |
| Losing `-mark` removes the downstream discriminator | known and accepted for the first build; port immediately after |

## Validation

Gates, in order. Each is a stop-the-line failure.

1. **Promotion.** PresentMon reports `Hardware: Independent Flip` for the relay window in
   `b:flip`, under both a KCD2-style and an Avatar-style source regime.
2. **Sink pacing.** Present rate reads the XR1's refresh (~60/s) under Avatar with DLSS FG,
   where `b:vsync` reads 120/s. This is the single number that proves the mechanism.
3. **KCD2 non-regression.** `b:flip` vs `b:vsync` on the same content: repeat and skip rates
   within noise of 0.12/s and 0.18/s. Any regression here kills the change — the working case
   is not negotiable.
4. **Avatar improvement.** Repeat and skip rates fall from ~2/s toward the KCD2 floor, measured
   in FG-engaged windows only.
5. **Policy unchanged.** `PolicyTests` and the replay corpus are untouched by this work and must
   stay green; `lk=1` should now appear in Avatar captures.
6. **Cost.** No regression in the `relay-cost-spec.md` ladder.

Gates 3 and 4 need `-mark` for clean attribution, which is deferred — so until the marker is
ported they are measured by content-step analysis against the video (the method that produced
the numbers in this spec), not by counter repeats.

## Open questions

- Does the XR1's driver permit independent flip at all? Gate 1. Everything depends on it.
- Does a 10-bit backbuffer survive promotion, or must the relay drop to 8-bit on this path? If
  the latter, that is a quality decision the user owns, not a default to assume.
- Does `GetFrameStatistics` return usable data through this path? If so it supersedes the
  head-1 ETW flip stream as the sink clock and gives an **in-process missed-refresh counter** —
  downstream judder measurable from the log alone, without video or marker decode.
- Is the waitable object meaningfully better than blocking `Present`, or is it the same wait
  with more code?
