# Per-display vsync on Windows/DWM — research findings (D3D9Ex relay)

Standalone reference for: **how (and whether) you can make a present vsync-lock to a SPECIFIC
display's vertical blank — particularly a secondary / non-primary display such as an HDMI capture
card — on modern Windows 10/11 with the Desktop Window Manager (DWM) always on.**

Produced by a fan-out deep-research pass (run `wf_3b2deab9`): 5 search angles, 19 sources fetched,
84 candidate claims extracted, top 25 verified by 3-vote adversarial verification (2/3 refutes
kills a claim) → 20 confirmed, 5 killed. Sourced to Microsoft Learn / DirectX docs where possible;
folklore is called out explicitly.

## The concrete problem this answered

A D3D9Ex relay captures a **240Hz primary** monitor and re-presents into a **borderless
pseudo-fullscreen window** (`WS_EX_TOPMOST | WS_POPUP`, `d3dpp.Windowed = TRUE`,
`SwapEffect = D3DSWAPEFFECT_DISCARD`) sitting on a **secondary 60Hz** display (an EVGA XR1 Pro HDMI
capture card that enumerates as a Windows display). With `Present(D3DPRESENT_INTERVAL_ONE)` the
present loop empirically runs at **240Hz (the source/primary refresh), not the 60Hz display the
window is on** — vsync locks to the wrong display. Creating the present device on the target
display's adapter ordinal made **no difference**.

## TL;DR / bottom line

- **You cannot, from a windowed D3D9 app, vsync-lock the present to a secondary display.** Windowed
  presents are paced by **DWM at the *primary* monitor's** vsync clock, regardless of window
  position or device adapter ordinal. This is documented behavior, not a bug in our code.
- **Exclusive fullscreen is *not* the documented escape hatch** it's reputed to be. Under
  DWM-always-on, the docs do not support "FSE grants complete display ownership / DWM doesn't
  mediate." That belief is largely **pre-Windows-8 folklore**. (It remains worth a *cheap empirical*
  test specifically for a capture-card pseudo-display, which is under-documented — but expectation
  should be low.)
- **`IDXGIOutput::WaitForVBlank` is the only documented per-output vblank primitive**, but it **only
  paces a CPU loop — it does not gate the flip.** Using it as a present gate decouples the present
  from the actual flip and *introduces* a beat (we tried this; it regressed — see the VBlankWaiter
  experiment in `temporal-capture-mode-spec.md` Round 6/7). Its correct use is as a **phase
  reference for a free-running timer** (the phase-locked-timer approach).
- **DXGI flip-model migration (D3D11/D3D12 interop) does not solve it either** — the composed cadence
  still follows DWM/primary, and it adds manual cross-API sync. High cost, no payoff for this goal.
- **Net:** the capture card's display controller does the final 60Hz scanout no matter what; the app
  feeds DWM, and DWM runs at the primary's rate. The best achievable is **well-paced, well-phased**
  frames via a **phase-locked timer**, not a true per-target vsync lock.

## Confirmed findings (with citations)

### 1. Windowed presents follow the PRIMARY monitor's vsync clock (confirmed, high)

In multi-monitor setups with DWM on, a windowed (or borderless) present's flip cadence is governed
by **DWM's composition clock, which runs at the primary monitor's refresh** — *not* the monitor the
window is on, and *not* the device's adapter ordinal.

- DXGI composition uses the primary monitor cadence regardless of window placement —
  [compositor clock](https://learn.microsoft.com/en-us/windows/win32/directcomp/compositor-clock/compositor-clock).
- DWM composition is itself vsync-paced (the compositor is driven by a vblank-aligned clock).
- Windows 10 build **2004** changed DWM to synchronize composition to the **fastest** attached
  monitor — but this **degrades above roughly a 3× refresh mismatch**. Our case is 240/60 = **4×**,
  i.e. past where the mitigation holds —
  [otterbro: DWM mixed-refresh performance](https://blog.otterbro.com/dwm-mixed-refresh-rate-performance/)
  (blog, corroborated).

Consequence: a 240Hz primary + 60Hz secondary ⇒ windowed `INTERVAL_ONE` paces at **240Hz**. Exactly
the observed `pdt ≈ 4168µs`.

### 2. D3D9Ex is fundamentally limited for per-output vsync (confirmed, high)

- D3D9 has **no per-output vsync selection** — `D3DSWAPEFFECT` sync is per-adapter, not per-monitor
  ([d3dswapeffect](https://learn.microsoft.com/en-us/windows/win32/direct3d9/d3dswapeffect)).
- `D3DSWAPEFFECT_FLIPEX` (the D3D9Ex flip path) **hands surfaces to DWM and adds an implicit back
  buffer** (present queue = `BackBufferCount + 1`), and in windowed mode goes through DWM
  composition — ([Direct3D 9Ex improvements](https://learn.microsoft.com/en-us/windows/win32/direct3darticles/direct3d-9ex-improvements)).
  (Matches the relay's own header note that FLIPEX produced an every-3rd-frame-blank artifact on
  this rig.)

### 3. `IDXGIOutput::WaitForVBlank` paces a loop but does NOT gate the flip (confirmed, high)

- `WaitForVBlank` is a method on **`IDXGIOutput`** (a specific monitor), reachable via
  `IDXGIAdapter::EnumOutputs` — so you *can* block on a chosen display's vblank
  ([WaitForVBlank](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgioutput-waitforvblank)).
- **But it only halts the calling thread until that output's vblank — it does not control when the
  swap chain actually flips.** In windowed mode the flip is still DWM's at primary cadence. So
  pacing your present loop with `WaitForVBlank` + an immediate present **decouples** the present
  call from the flip → a beat between your loop and the real flip. (This is precisely what the
  `VBlankWaiter` experiment hit; see `temporal-capture-mode-spec.md` Rounds 6–7.)
- Correct use: **phase reference for a timer-driven present**, not a present gate.

### 4. DXGI flip-model: better, but doesn't pin a secondary vblank (confirmed, high)

- Flip-model swap chains (`FLIP_DISCARD`/`FLIP_SEQUENTIAL`) make **windowed equal or better than
  fullscreen-exclusive** for latency/efficiency, and `SetFullscreenState(TRUE, pTarget)` lets you
  pick the output — ([use DXGI flip model](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/for-best-performance--use-dxgi-flip-model),
  [SetFullscreenState](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-setfullscreenstate)).
- A waitable swap chain (`DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT` +
  `GetFrameLatencyWaitableObject`) bounds *latency*, not which display governs cadence
  ([reduce latency](https://learn.microsoft.com/en-us/windows/uwp/gaming/reduce-latency-with-dxgi-1-3-swap-chains)).
- **None of this is proven to pin the flip cadence to a *secondary* display** — composed windowed
  cadence still follows DWM/primary. And reaching it from D3D9Ex needs **D3D9Ex↔D3D11 interop with
  manual sync** (D3D9Ex shared surfaces are **unsynchronized** — the app must synchronize by hand)
  ([surface sharing](https://learn.microsoft.com/en-us/windows/win32/direct3darticles/surface-sharing-between-windows-graphics-apis)).

## Refuted claims (3-vote adversarial — these are folklore or unsupported)

| Claim | Vote | Why it matters |
|---|---|---|
| "True fullscreen-exclusive grants complete display ownership; DWM doesn't mediate." | **0-3** | The core "FSE bypasses DWM" belief is **not supported** by current docs. |
| "Independent Flip makes DWM stop composing and send frames straight to scanout (the documented bypass)." | **0-3** | Borderless Independent Flip is **not** a documented per-display scanout bypass. |
| "When a present straddles outputs, DXGI syncs to the output with the largest sub-rect — so window position picks the governing monitor." | **0-3** | Window **position does not** choose flip cadence. |
| "In the flip model the app's back buffers are shared directly with DWM which composes without an extra copy (windowed = DWM compose, not scanout)." | **0-3** | Phrased as stated, unsupported; the takeaway (windowed → DWM) is right but the mechanism detail isn't as claimed. |
| "Modern Windows transparently converts FSE games to borderless (Fullscreen Optimizations), so DWM-managed is always the default." | **1-2** | **Not** strongly supported — FSO is more nuanced; do not rely on this as a reason FSE "can't" work. |

Note on "refuted": a kill means the source material did **not substantiate** the claim — it is not
proof of the opposite. For the FSE-on-a-capture-card case specifically (under-documented), this
justifies a *cheap empirical test* rather than a confident "won't work."

## Practical recommendation (ranked by reliability × cost)

1. **Phase-locked timer** — run the QPC present timer and periodically resync its *phase* to the
   target output's vblank via `IDXGIOutput::WaitForVBlank` or `DwmGetCompositionTimingInfo`. Uses
   WaitForVBlank the documented way (phase reference, not flip gate), avoids the decoupling beat,
   and helps **variable** sources. **Lowest risk, moderate cost, documented.** Spec already exists:
   `phase-locked-timer-dxgi-spec.md`. *This is the recommended path.*
2. **Exclusive fullscreen on the target** — the one remaining documented-ish lever. Cheap to try,
   but the evidence says it probably won't bypass DWM on a secondary display; the capture-card case
   is the only reason it's not already ruled out. **Low expectation, low cost — a rule-out.**
3. **Accept source-rate present + card hardware-downsample** — for **uniform high-rate** sources
   (e.g. 240Hz) this is already clean (the card samples a regular stream). Does **nothing** for
   variable sources. Zero cost; not a general solution.
4. **DXGI flip-model migration (D3D11/D3D12 interop)** — large effort, manual cross-API sync, and
   **does not solve** the secondary-vblank problem. **Not recommended.**

## Sources

Primary (Microsoft Learn / DirectX):
- compositor clock — `learn.microsoft.com/en-us/windows/win32/directcomp/compositor-clock/compositor-clock`
- D3DSWAPEFFECT — `learn.microsoft.com/en-us/windows/win32/direct3d9/d3dswapeffect`
- Direct3D 9Ex improvements — `learn.microsoft.com/en-us/windows/win32/direct3darticles/direct3d-9ex-improvements`
- IDXGIOutput::WaitForVBlank — `learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgioutput-waitforvblank`
- For best performance, use DXGI flip model — `learn.microsoft.com/en-us/windows/win32/direct3ddxgi/for-best-performance--use-dxgi-flip-model`
- DXGI flip model — `learn.microsoft.com/en-us/windows/win32/direct3ddxgi/dxgi-flip-model`
- IDXGISwapChain::SetFullscreenState — `learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-setfullscreenstate`
- IDXGISwapChain::Present — `learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-present`
- IDXGISwapChain2::GetFrameLatencyWaitableObject — `learn.microsoft.com/en-us/windows/win32/api/dxgi1_3/nf-dxgi1_3-idxgiswapchain2-getframelatencywaitableobject`
- Reduce latency with DXGI 1.3 swap chains — `learn.microsoft.com/en-us/windows/uwp/gaming/reduce-latency-with-dxgi-1-3-swap-chains`
- Surface sharing between Windows graphics APIs — `learn.microsoft.com/en-us/windows/win32/direct3darticles/surface-sharing-between-windows-graphics-apis`
- Multiple monitor operations (D3D9) — `learn.microsoft.com/en-us/windows/win32/direct3d9/multiple-monitor-operations`
- Variable refresh rate displays — `learn.microsoft.com/en-us/windows/win32/direct3ddxgi/variable-refresh-rate-displays`
- DirectX blog: DXGI flip model — `devblogs.microsoft.com/directx/dxgi-flip-model/`
- DirectX blog: demystifying fullscreen optimizations — `devblogs.microsoft.com/directx/demystifying-full-screen-optimizations/`

Secondary (blog / forum, corroborating):
- otterbro: DWM mixed-refresh-rate performance — `blog.otterbro.com/dwm-mixed-refresh-rate-performance/`
- jackmin: swapchains, present, and present latency — `jackmin.home.blog/2018/12/14/swapchains-present-and-present-latency/`
- Blur Busters forum threads — `forums.blurbusters.com/viewtopic.php?t=7443`, `forums.blurbusters.com/viewtopic.php?t=9138`
- VirtualDub blog: present/latency notes — `virtualdub.org/blog2/entry_339.html`

See `temporal-capture-mode-spec.md` Rounds 5–8 for how these findings drove the relay's present-path
decisions (VBlankWaiter reverted, present-on-target-adapter, exclusive-FS test, phase-locked timer).
