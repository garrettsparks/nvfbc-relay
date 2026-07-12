claude --resume 5ebc07ac-8490-4e5b-bce7-16b4881e7250

# Kickoff — nvfbc-relay branch-validation campaign

Paste this to launch each testing session. It is self-contained; per-branch detail lives in
that branch's `docs/*-spec.md` (committed on the branch — read the spec FIRST each session).

## What this project is

`nvfbc-relay` (/Users/gsparks/dev/nvfbc-relay): Windows relay capturing a display with NvFBC
(NvFBCToDx9Vid, D3D9Ex) and re-presenting it fullscreen-borderless on an HDMI capture card
(EVGA XR1 Pro, 60Hz, D3D9 adapter 1) for low-overhead game streaming. Source = ROG PG279QM,
240Hz G-Sync, adapter 0, primary. The validated core (v0.0.11, tag on dev): dual-device
capture ring (event-driven blocking grab, QPC timestamps, keep-real frame-gen collapse,
coherency flush) + present loop (nearest-pick, monotonic hysteresis, Schmitt stickiness band,
absolute-QPC or DWM-blocking pacing). Modes: `vsync`, `<rate>`, `t:<rate>`, `t`/`t:vsync`,
`diag`/`diag:vsync`. User's daily driver: `t:vsync`.

## Working agreement (strict)

- User does all merges/tags/Windows builds/captures. Claude: edits code, triggers CI
  (`gh workflow run dev-build.yml --ref <branch>` → `gh run watch <id> --exit-status
  --interval 20`), downloads artifacts (`gh run download <id>`), analyzes logs + videos,
  writes analysis tooling. Claude MAY commit/push on `claude/*` branches (established this
  campaign); never merges, never touches dev without instruction. Tags on explicit request
  (convention `v0.0.N`).
- macOS clangd diagnostics on this repo are noise (windows.h cascades). CI is the only
  compile check. Never "fix" them.
- `NvFBCR.cpp` and the `.vcxproj` are CRLF (vcxproj + BOM): edit via byte-level python
  patches preserving \r\n, never whole-file rewrites. Other sources are LF.
- Verify, don't guess. Every mystery this project hit fell to log analysis, not speculation.

## State: 12 branches, all CI-green, all spec'd, awaiting validation

```
dev (v0.0.11 = validated baseline)
├── direct-write-capture-ring          NvFBC writes ring slots via dwBufferIdx; copy deleted
│   └── claude/no-mt-lock              D3DCREATE_MULTITHREADED removed from all 3 devices
├── claude/init-cleanup                backbuffer leak fix, log order, dead code
├── claude/ring-capacity-config        -ring N (3..32); sizing table for FG multipliers
└── claude/adaptive-bracketing-delay   lag = max(period, 1.25×P̂ EMA), slew 100µs/present
    ├── claude/blend-mode              b/b:vsync/b:<rate> — ps_3_0 lerp compositor
    │   ├── claude/nvofa-fruc-probe    standalone interop probe exe (not in .sln)
    │   └── claude/phase-pull          w-lock onto real frames at integer ratios (blend/interp only)
    │       ├── claude/phase-pull-snap + snap-anchor passthrough (A/B partner; diff = snap alone)
    │       └── claude/fruc-compositor o:* modes — NvOFFRUC via D3D11 sidecar, blend fallback
    │           └── claude/nvof-warp   -interp flow — raw NVOFA + our warp (favored bet)
    └── claude/fg-passthrough          -fg keep — Path B: publish gen frames re-stamped
```

Each branch: own spec in `docs/` with design rationale, constants, risks→instruments, and a
capture rubric. Suggested test order: direct-write → no-mt-lock → adaptive-delay → blend →
phase-pull vs phase-pull-snap A/B → fg-passthrough → probe + fruc/flow three-way →
init-cleanup + ring-capacity spot checks. Gates: perf branches must not regress v0.0.11
numbers; interp branches are characterization (expectation of FRUC failure is on record).

## Settled findings — do not re-litigate without new measured evidence

- Baseline spec T1–T10 (`docs/dual-device-capture-present-spec.md`, LOCAL-ONLY — user keeps
  docs out of dev; if absent, memory summarizes): event-driven capture only; private capture
  device; 100ms timeout only; flush-before-publish (T4); absolute-QPC scheduling; hysteresis;
  bWithHWCursor=0 in ring session; windowed INTERVAL_ONE = DWM's clock (desktop → source
  display, fullscreen game → card-locked 60 — the production regime); keep-REAL collapse
  ([gen, real] wake order, <3ms = pair); constant pipeline offset free, variable latency = stutter.
- Campaign-era: anti-flip-flop REQUIRES state (static bias measurably failed; Schmitt with a
  state bit fixed 240→60 judder: windows 6→0, std(Δ) 0.430 beats the vsync gold standard);
  w = present/capture clock phase (never 0/1) so interpolating compositors synthesize every
  frame unless phase-pulled; plain timer `60` mode's ~100 bursty dupes/5min are inherent
  (two-clock NOWAIT aliasing) — its fixed version is `t:60`.
- PRIOR FRUC CONCLUSIONS ("dimmed, basically a lerp") ARE CONTAMINATED — retracted by user
  (no decoupled capture, guessed timestamps, gen frames unknowingly fed). Archive HEADERS are
  facts; archive conclusions are noise. Dimming prime suspect (alpha byte) is neutralized in
  the new integration (alpha forced 1.0) and the IDENTITY TEST arbitrates in 30 seconds.

## Test methodology

**Capture discipline:** empty `NvFBCR.log` beside exe (logging auto-enables); OBS CBR
~6 Mbps, confirm 0 dropped frames; ~3 min UFO runs / ~5 min KCD runs; STOP OBS BEFORE
touching windows or quitting (tail wind-down contaminates dupe counts); for KCD start OBS
after load-in (regime transitions live in the log, not the video); KCD segment order strafe →
sky-swings → map → gameplay (instruments need segment masks: reverse/dupes are meaningless on
map/sky/menus — sky swings ARE reversals, stills ARE dupes).

**Artifact naming:** `<source>_<fg>_<mode>[_<variant>]_<content>.{mp4,log}` — e.g.
`240_x1_t_60_dw_ufo`, `120cap_x2_t_vsync_nomt_kcd`, `180cap_x2_b_vsync_pp_kcd`. Variant tags
this campaign: `dw`, `nomt`, `adl`, `b`(in mode), `pp`, `pps`, `fgkeep`, `o`+`fruc/flow`.

**Analysis (Claude runs, in /Users/gsparks/dev/frame-drop-analysis):**
- `uv run detect.py dupes --mad --max-dupes 0 <file>.mp4` — re-shown frames (NOTE: defeated
  by blend/interp output — only stall-repeats remain detectable there)
- `uv run detect.py pacing <file>.mp4` — period-2/3, delta std, Roughness
- `uv run detect.py reverse <file>.mp4` — stale/out-of-order pixels (THE coherency gate;
  needs directional content: UFO scroll or strafe segments)
- `python3 stride.py <file>.log` — relay-side stride distribution + alternation windows
  (selection ground truth, no OBS in the loop; handles pick=repeat carry-forward)
- Log assertions: correct init lines; `flush` p95 < 500µs; no unexpected LOGERR; `pick=`
  census consistent with rate ratio; bracket depth `d` inside ring; `col` matches FG regime
  (≈47% of wakes at ×2; =0 no-FG except ~0.08% jitter-tail false collapses at 240Hz — known);
  relay overruns (pdt>25ms) only at launch/quit regime boundaries.
- Log fields: `arr` capture arrival µs · `dt` inter-arrival · `flush` coherency wait ·
  `col` collapsed/re-stamped · `g=` gen tag (fg-passthrough) · `dl` deadline · `tgt` target ·
  `before/after(d<n>)` bracket · `w` weight · `pick` outcome
  (before/after/±adv/repeat/blend/stall/snap/interp) · `jit` deadline→present · `pdt`
  inter-present · `lag` bracketing delay (adaptive branches) · `pull` phase-pull (pp branches).

**Reference numbers (v0.0.11, for no-regression comparisons):** V1 vsync 60→60: 4 dupes/3min,
std(Δ) 0.500, reverse 0. t:60 240→60 (Schmitt): 0 dupes, period-2 0%, std 0.430, stride
windows 0, lone slip ~1/33.5s. t:vsync 60→60: ~4-5 dupes, std 0.498. t:vsync KCD 120cap×2:
pdt 16667µs, jit 3µs median/5µs p95, col ≈47%, d ≤ 5, crisp reticle (user eyeball). Plain
`60`: ~100-123 bursty dupes/5min IS the baseline profile (not V1-class — criterion corrected).

**Per-session flow:** read branch spec → user builds/downloads CI artifact for THAT branch →
captures per the spec's rubric → Claude runs analysis suite → verdict vs the spec's pass
criteria → user merges (squash, PR # style) or Claude iterates on the branch → on merge:
Claude tags next `v0.0.N` on request → update memory.

## FIRST SESSION: direct-write + no-mt-lock (the perf chain, riskiest first)

1. **direct-write-capture-ring** (`docs/direct-write-spec.md`): NvFBC writes ring slots
   directly (`dwNumBuffers=RING_SIZE`, `dwBufferIdx`); capture-target + StretchRect deleted;
   flush unchanged. EITHER-WORKS-OR-DOESN'T: first launch either runs, or
   `NvFBCToDx9VidSetUp on capture device failed` appears → revert branch, record finding.
   Captures: `240_x1_t_60_dw_ufo` + `120cap_x2_t_vsync_dw_kcd`.
   Pass: **reverse 0 (the corruption gate)**, 0 dupes, stride windows 0, col/dt/pdt/jit ≈
   reference, `flush=` ≤ baseline (if it collapses to ~0, NvFBC synchronizes internally —
   interesting, flush stays anyway per T4).
2. **claude/no-mt-lock** (`docs/no-mt-lock-spec.md`), stacked on 1 — test only after 1
   passes (attribution): MULTITHREADED off all three devices; single-thread ownership audit
   is in the spec. Full-suite class because it touches the shared InitD3D9 path:
   `60_x1_vsync_nomt_ufo` (gold guard) + `240_x1_t_60_nomt_ufo` + `120cap_x2_t_vsync_nomt_kcd`.
   Watch startup for CreateDeviceEx failures/debug-runtime complaints (loud finding).
   Rollback: revert the one commit.

Both validated → user merges chain (direct-write first), tag proposal `v0.0.12`.

## Interp-phase prerequisites (sessions ~5+)

- Copy `NvOFFRUC.dll` (from Optical_Flow_SDK_5.0.7/NvOFFRUC/NvOFFRUCSample/bin/win64/, zip in
  Mac Downloads — must reach the Windows box) beside `NvFBCR.exe`. Loader logs resolved path.
  Raw-flow backend needs NO file (nvofapi64.dll is driver-shipped).
- Run `FrucProbe.exe` (samples/NvFBC/NvFBCFrucProbe, own vcxproj, not in .sln — build it once)
  → TESTs 1-4 answer the cross-API sharing verify points; exit code = failure count.
- FRUC identity test FIRST: static desktop, `o:60 -interp fruc` — output must be
  pixel-identical; any dimming = engine/format bug isolated from motion (the old-claim arbiter).
- Flow smoke test: `o:60 -interp flow` on 240Hz UFO (warp bugs are glaring on the pattern).
  Supported-input-format log lines diagnose byte-order (channel-swap ⇒ flip BGRA8→RGBA8 in
  InterpSidecar::CreateConversionPipeline). Default -interp flips to flow after first clean run.
- The decisive quality artifact: three-way `-interp flow` vs `-interp fruc` vs `b:vsync` on
  `180cap_x2_*_kcd` (1.5:1 — quantization that no picker can smooth; the 180cap sb2 nearest
  capture is the banked "before" video). Expected FRUC failure mode: HUD/reticle halos.

## Merge orchestration cautions

- Two chains touch `CaptureRing.cpp` independently (direct-write chain vs adaptive-delay
  chain + fg-passthrough); merge one chain, rebase the other — conflicts are small
  (slot/EMA regions) but real. `claude/nvofa-fruc-probe` and `claude/fruc-compositor` both
  add shared-handle retention (intentional duplicate — dedupe at merge).
- Specs are committed in `docs/` per-branch per user request; user keeps docs out of dev —
  strip or keep the doc commit at merge, user's call each time.
- Stacked branches: after a parent merges to dev, rebase children (Claude can do this on
  claude/* branches; force-push with lease).

## Memory

Auto-memory (`temporal-capture-project.md` + `defender-false-positive.md` etc.) carries the
full campaign history including retracted-evidence notes. Recalled memories are background —
verify against code/logs when specific. Defender note: builds flag Wacatac.B!ml if the
d3dcompiler import ever disappears again (blend's real D3DCompile now carries it).
