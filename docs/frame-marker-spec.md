# Frame Marker Spec — Machine-Readable Per-Frame Provenance Burn-In

Status: DESIGN (not yet implemented). Goal: burn a robust, machine-readable marker into every
presented frame so recorded video aligns to the relay log exactly (no offset-hunting), and so
interpolated frames are ground-truth-labeled once blend/NVOFA exist. Debug/test builds only —
never in production output.

## Motivation

Every video↔log correlation this campaign has done (the 2026-07-14 doorway glitch, the July
VOD excursion hunt, all of it) required inferring a time offset by matching capture stalls or
scene events, then trusting it. A burned-in per-frame counter turns that into `read the number
→ look up the log line` — exact, zero inference. And once blend exists, the marker labels each
frame's provenance (real passthrough vs synthesized, the weight, which compositor), replacing
the inferential hf-fingerprint guess (blend frames read ~3x lower high-frequency content — real
but scope-limited: it missed sharp warp frames) with deterministic ground truth.

## Scope, phased

- **v1 (buildable TODAY, nearest mode, no blend):** frame counter + pick code + quantized
  bracket weight (all present-side data that exists today). Delivers exact video↔log
  alignment immediately, plus video-only repeat attribution: a downstream duplicate copies
  the whole frame INCLUDING the burned pick cells, while a relay repeat burns a fresh
  marker with pick=repeat. This is the highest-value near-term test-infra piece.
- **v2 (blend era):** interp flag, compositor ID, source provenance light up; the weight
  field's meaning shifts from bracket w to blend w (same encoding). Same marker format.

## Marker design

A row of high-contrast binary cells in a fixed corner (top-left recommended), read at known
positions. NOT a QR code: the payload is a fixed 36 bits, we control both encoder and decoder,
and coarse binary cells survive video compression far better per-bit than QR's small modules
(which fight macroblock quantization and downscale). QR's error-correction idea is folded in as
a checksum + counter monotonicity instead.

### Cells

- **Binary black/white, LUMA** (pure 0 / pure 255). Maximum separation — no quantization level
  confuses them, through a single encode AND multiple (Twitch transcode → download → re-encode).
  Chroma is 4:2:0-subsampled and lossy; luma is full-res and best-preserved, so encode in luma.
- **>= 32x32 px per cell** at the OUTPUT resolution (1080p card). H.264/HEVC operate on 16x16
  macroblocks; a flat luma block spanning several macroblocks is essentially just a DC
  coefficient, which survives even high QP. Size up if OBS downscales (the user's canvas is
  1440p; a 1080p marker cell becomes ~43px there — still fine, but never go below 32 at output).
- **Position as a fixed FRACTION of the frame**, not absolute pixels, so the reader survives
  crop/scale between the card output and the analysis file. Top-left origin (0,0) is simplest;
  reserve the whole top-left region and keep game HUD elements clear of it in test captures.
- **Black quiet-zone border** (a >= 16px constant-black margin) around the whole cell strip.
  ColorFill overwrites the backbuffer, so content never bleeds THROUGH the marker, but
  compression can smear bright adjacent content into the OUTER edge of an edge cell; the
  quiet zone absorbs that. Combined with center-sampling (read the middle ~50% of each cell,
  ignore edges) this isolates the marker from content and from cell-to-cell bleed. Adjacent
  same-color cells merging visually is fine — the reader samples known positions, not edges.

### Payload layout (44 cells; v1 burns counter + weight + pick, v2 cells reserved black)

| cells | field | encoding |
|---|---|---|
| 0 | sync/always-white | fixed white = "marker present" (distinguishes from black content) |
| 1-24 | frame counter | 24-bit binary, LSB-first; wraps at 2^24 (~74 h at 60 fps) |
| 25 | interp flag (v2) | white = synthesized, black = passthrough real frame |
| 26-29 | weight (LIVE in v1) | 4-bit binary, round(w*15): bracket w now, blend w in v2 |
| 30-31 | compositor ID (v2) | 00 nearest, 01 blend, 10 fruc, 11 flow-warp (HOW the output was made) |
| 32-33 | source provenance (v2+) | 00 real, 01 gen-believed, 10 unknown, 11 mixed (WHAT the input frame was) |
| 34-36 | pick code (LIVE in v1) | 0 none, 1 before, 2 after, 3 after-adv, 4 before-adv, 5 repeat |
| 37-39 | extension schema ID | 0 = core only (v1); nonzero selects a future extension layout |
| 40-43 | checksum | 4-bit XOR of the payload nibbles (cells 1-39; misread detection) |

### Compatibility contract (append-only, same philosophy as the temporal log line)

The 44-cell CORE above is FROZEN the moment real captures exist: cells never move or
change meaning (the weight field's v1-bracket-w to v2-blend-w shift is pre-declared
above, disambiguated by the interp flag / compositor ID). New data NEVER edits the
core - it appends, declared by the extension schema ID (checksum-protected, so the
declaration itself is reliable):

- **Extension space:** rightward on the same row first (~13 cells before the frame's
  right margin at 60 cells/width), then whole rows below the strip (~50 net bits per
  row at one cell height each). Each extension block carries its OWN always-white sync
  cell and its OWN checksum, so it is self-validating exactly like the core.
- **Old recording, new decoder:** core says schema 0 - the decoder never samples
  extension positions (which hold quiet zone or game content). One code path decodes
  every vintage; no per-version layout tables.
- **New recording, old decoder:** core cells are untouched by appends, so old decoders
  read the core fields correctly and report "extension schema N present, core-only
  decode" for schemas they don't know.
- PROVEN by synthetic test: a simulated schema-1 recording (4 appended cells) decodes
  100% checksum-valid core-only on the schema-0 decoder with the extension reported.

Reserve the v2 cells in v1 too (fill black) so the format is stable and the reader never changes
layout. **Compositor ID vs source provenance are distinct and both useful:** compositor ID says
how the OUTPUT frame was produced (passthrough / blend / warp); source provenance says what the
INPUT frame was — but IT IS A TIMING-DERIVED BELIEF, NOT content-verified ground truth. The
relay does NOT know gen vs real; it knows BURST POSITION (intra-batch vs batch-start) from
arrival timing. Under the x2 closed model this maps to [gen (batch-start, first),
real (intra-batch, ε later)]: collapse KEEPS the intra-batch real, RETRACTS the batch-start gen
(valid=false), so in steady state only reals are selectable and a SELECTED frame tagged "gen"
means an ε-window leak. **That mapping is itself EMPIRICAL, not inherent** — it comes from the
`-collapse first/second` A/B (keep-second = crisp / no gen artifacts ⟹ member 2 = real;
keep-first = ghosting ⟹ member 1 = gen), run on one GPU/driver/SM version. It could DRIFT
silently (a driver update or different FG mode could flip the order, and the relay would keep
mislabeling). **Trustworthy ONLY for x2 as characterized** — and the marker+video cross-check is
precisely how you RE-VALIDATE it: if the order ever flips, "kept/real"-tagged frames start
showing gen artifacts in the recording, catching the regression instead of silently shipping
gen frames. For x3 the
structure is hybrid (ε-pair + a paced sharp single), frame accounting does not close, and some
gens are hf-SHARP (warp/extrapolation) — so burst-position does NOT cleanly map to gen/real AND
the hf fingerprint (soft=gen) MISSES the sharp gens. In-game DLSS-G is uncharacterized. So the
tag records the relay's COLLAPSE BELIEF; the marker + video together VALIDATE x2 (soft gens,
mapping holds) but DO NOT resolve x3 or unknown FG modes. Encode a confidence/regime bit if
useful (x2-model vs uncertain). The ring `Slot` already carries metadata (timestamp, valid), so
adding the tag is trivial infra; the plumbing making it v2+ is the "provisional → resolved"
belief (a batch-start member is only presumed-gen once an intra-batch member retracts it;
non-FG batch-start members are plain reals) and propagating it through selection. The tag also
enables a LOG-SIDE leak counter today (log when a presumed-gen is selected), same x2-only scope. Two robustness layers: the checksum DETECTS a misread; the counter's MONOTONICITY
(should be prev+1) both detects misreads and flags genuine dropped/repeated frames (useful data,
not just error handling).

## Implementation (D3D9, in the present path)

**INVARIANT: the marker is PRESENT-side only — it goes on the backbuffer (the per-present
output), NEVER on the ring/capture surfaces.** Flow: select the frame from the capture ring →
`StretchRect(chosen → g_backbuffer)` → mark the backbuffer → `PresentEx`. Writing into a ring
slot would corrupt a shared source frame (slots are re-selected by repeats and, later, read as
before+after by blend), and would intrude on the capture device / grab timing (the FG race we
protect). All marker payload except source-provenance is present-side data already (counter,
pick, w, lk, pull, jit, pdt, computed in the present loop). The source-provenance field is the
lone exception: its real/gen belief originates capture-side (keep-real collapse tags the slot),
so the ring must carry a real/gen tag per slot that the present loop reads off the selected
slot and encodes — the RENDERING stays present-side; only the tag propagation is capture-side.

After the existing `StretchRect(chosen → g_backbuffer)` and before `PresentEx`, write the cells
into the backbuffer corner. **Build-a-bitmap-then-one-blit is the primary approach, NOT
per-cell ColorFill** — it is a constant handful of ops regardless of cell count, which the
per-cell approach is not. Per-cell ColorFill is tolerable for the ~40-cell minimal marker but
prohibitive for the rich tier (100-200 cells = 100-200 API calls/present); the blit scales to
either. It is also simpler (assemble a tiny bitmap, blit it):

- Keep a small **system-memory surface** sized one texel (or a small block) per cell — e.g. a
  K×1 strip or a W×H grid. Each present: lock it (a few dozen bytes, cheap), write each cell's
  luma (0 or 255), unlock.
- **`UpdateSurface`** it to a matching default-pool texture (the standard sysmem→default upload).
- **`StretchRect`** (point-sampled, `D3DTEXF_NONE`) that texture up to the marker region on the
  backbuffer, scaling each texel to a >= 32px cell. ~1 lock + UpdateSurface + StretchRect per
  present, constant in cell count. VERIFY the exact pool/format path at implementation (D3D9
  StretchRect/UpdateSurface have pool/format/render-target constraints; the ARGB10 backbuffer is
  the dest). ColorFill-per-cell remains a valid fallback for the minimal marker if the blit path
  hits a driver constraint.
- Gate behind a **`-mark` flag** (minimal) / **`-mark:full`** (rich tier) — shares the
  ApplyOption dispatch (one place, cmdline + prompt). Off by default; never ships enabled.
- **Log the counter on the temporal line**: append `mark=<N>` (append-only, parser-safe). Now
  video↔log is `read N off the frame → grep mark=N`. The counter increments once per present.

### Rich tier (`-mark:full`) — self-describing video

Optional larger marker (a grid, ~10% of frame) encoding the full per-present TEMPORAL-LINE data
so the video is analyzable with NO log — insurance against log loss and for sharing clips.
Encode `pick` (3 bits), `w` / `pull` / `lag` / `jit` / `pdt` as deltas-from-nominal or quantized
(the present period and lag are near-constant, so deltas need far fewer bits than absolutes) and
`lk`, alongside the counter and a wider checksum. ~80-120 bits total → a ~10×10 cell grid.
SCOPE LIMIT: this reconstructs the TEMPORAL lines only, not the whole log — capture-side data
(`dt`, `flush`, `col`), estimator telemetry, and error lines are not one-per-output-frame and
have nowhere to hang. "Almost the log" = the temporal lines, which is most of what analysis uses.
The rich grid needs more error correction than the minimal marker (more cells = more misread
surface); use the checksum plus, if measured necessary, a parity row.

## Analysis tool (frame-drop-analysis) — co-designed with the encoder

The decoder is half the system; its geometry, thresholds, and checksum MUST match the encoder's,
so it is specced here alongside. Lives beside `detect.py` / `relaylog.py`, either as a new
`detect.py marker` subcommand or a `relaylog marker` command (it bridges both worlds: it reads a
video AND cross-checks a log).

### Decode pipeline (per frame)

1. **Locate the marker region.** Cell positions are a FRACTION of frame dimensions (encoder
   writes at fixed fractions), so multiply by the analysis file's actual W×H — this survives the
   card-1080p → OBS-1440p-canvas → downloaded-file scale chain without hardcoded pixels. The
   cell-0 "always-white" sync cell is the presence check: if it doesn't read white, there's no
   marker on this frame (untagged clip, or the region was cropped away) — bail cleanly.
2. **Sample each cell at its CENTER**, averaging an inner patch (e.g. the middle 50% of the cell)
   to avoid macroblock-edge bleed between adjacent cells. Use the LUMA plane (decode `-pix_fmt
   gray` via ffmpeg, as the existing tools do).
3. **Threshold** each sampled cell: luma > 128 → 1 (white), else 0 (black). The binary design
   makes 128 a safe split with huge margin even after multiple encodes.
4. **Assemble the payload**: counter (24-bit LSB-first), interp flag, weight (4-bit), compositor
   ID (2-bit), checksum (4-bit). **Verify the checksum** (XOR of payload nibbles). Mark the frame
   `checksum_ok = false` on mismatch rather than trusting a corrupt read.
5. **Monotonicity pass** over the counter sequence: a value that isn't prev+1 is either a misread
   (checksum likely also failed → discard) or a genuine dropped/repeated output frame (checksum
   OK, counter skipped/repeated → RECORD it — this is real dupe/skip data, not an error).

### Output

A per-frame provenance table + summary:
- Per frame: `video_frame_index, counter, interp?, weight, compositor, checksum_ok`.
- Summary: counter coverage (min/max/gaps), % checksum-valid, dropped/repeated counters detected,
  and — in v2 — interp-vs-passthrough census and weight distribution.

### Cross-check against the log (the payoff)

Given the video's decoded markers and the run's `NvFBCR.log`:
- Join on the counter: video frame with `counter=N` ↔ the temporal log line with `mark=N`. This
  is the EXACT alignment — no offset-hunting, no stall-matching.
- **v1**: confirms the alignment itself and surfaces the true dropped/repeated-output timeline
  (counter gaps/repeats) cross-referenced with the log's `pick=` (was a counter-repeat a
  `pick=repeat`? a beat slip? a chain-fabricated dupe with no log cause? — the exact question
  that took a whole session by hand).
- **v2**: for each joined pair, assert the video marker's `interp?/weight/compositor` matches the
  log's `pick=`/`w=`. Agreement = end-to-end ground truth (targeting → compositing → scanout →
  capture → encode all faithful). Disagreement localizes a compositor-pipeline bug.

### Robustness-measurement mode

A mode that takes the SAME source run through N encode generations (local → Twitch source VOD →
re-encode) and reports per-generation checksum-valid rate and first-corrupted-cell, establishing
the marker's encode-generation budget before blend depends on it. This is how §Validation step 2
is run.

## Performance

Debug-gated (`-mark`), so production output never pays it. Cost is dominated by the ~36+
separate ColorFill CALLS per present (API/GPU-command overhead), not pixel throughput — the
filled area (~37K px) is ~2% of the StretchRect already on the present path. It is on the
PRESENT device, not the capture path (the FG gen-capture race is untouched). Standing discipline
still applies: measure pdt/jit with `-mark` on vs off during a real (game-running) test session,
since the present device shares the GPU with the game. If the call count registers, batch it:
write the whole cell row into a tiny N×1 system-memory surface with one lock (a few dozen bytes),
then one StretchRect scaling it into the corner — 2 ops/present instead of 36+. Do NOT
pre-optimize; batch only if measured.

## Validation

1. **v1 alignment proof:** record a run with `-mark`, read counters off the video, confirm they
   map 1:1 to `mark=` in the log with a constant frame offset (or exact, if the counter is the
   key). No more stall-matching.
2. **Encode-robustness sweep:** push the SAME `-mark` run through the real pipeline — local
   recording, then stream→Twitch→download (source rendition), then a re-encode — and measure how
   many encode generations the counter survives intact (checksum-valid). Establishes the marker's
   robustness budget BEFORE blend depends on it.
3. **v2 (blend era):** confirm the marker's interp/weight matches the log's pick=/w= per frame.

## Gotchas

- ColorFill format/pool restriction on the ARGB10 backbuffer — verify; have the StretchRect
  fallback ready.
- OBS crop/scale: position the marker as a frame fraction and keep the region clear of HUD in
  test captures; account for the 1080p-card-into-1440p-canvas geometry the user runs.
- Chroma subsampling: luma-only cells (done by design).
- Counter wrap: 24 bits is ~74 h; fine, but the reader must handle the wrap.
- The marker occupies real pixels — it's a DEBUG feature, gated off for any production/stream you
  actually watch.
