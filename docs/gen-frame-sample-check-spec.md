# Generated-frame sample check: a referee for the driver change map

## Why this exists

Under DLSS frame generation the relay obtains exactly the base rate of distinct frames.
The driver's change map (`-diffmap`, 16x16 blocks) reports zero changed blocks between the
real member of a batch and the generated member it was paired with on 99.3 to 100% of
batches across every Avatar capture (60x2, 59x2, 30x2), against 7.5% under Smooth Motion
on KCD. The ring refuses those members as duplicates before the policy ever sees them, so
`-subgen` substitutes single digits per six minutes against thousands of blends, however
favourable the geometry (30x2 places every blend at w=0.5, exactly where the generated
member sits, and fired 7 times).

That leaves one question the logs cannot answer: is the map TRUE (the generated frame never
reaches NvFBC, and the second grab really is the same frontbuffer twice), or does NvFBC
report zero blocks on any repeat grab inside a burst regardless of content? Retracted
members never reach the video, so the recording cannot check them. The map was validated
against a pixel instrument once (65619 batches, 98.80% of duplicates caught, 0 false
positives on 60711 genuine frames) but on a Smooth Motion capture, which is the regime where
it agrees with everything anyway.

Answering it decides whether `-subgen` is dead on the NvFBC path under DLSS-G (leave it off
and stop spending on it) or whether the relay needs its own content check on the D3D11
present path, where today there is none.

## What exists today

`SynthCompositorBase::GeneratedContentUsable` (D3D9 present path only) is a graded pixel
check: three `StretchRect` downscales of gen / before / after to 64x36 tiles, one
`GetRenderTargetData`, luma conversion, and an RMS ratio `gdiff / motion` against a fixed
floor. It runs on the PRESENT thread, per offered candidate, and only when the change map is
unavailable (`changedBlocks == -1`); with the map present it is skipped as redundant.

Measured in the field on two Smooth Motion runs:

| run | offered | mean | worst |
|---|---|---|---|
| subgen_kcd_improved_gen_selection | 2930 | 776 us | 24.3 ms |
| subgen_kcd_diffmap | | 864 us | 27.0 ms |

The 27 KB copy is not the cost. `GetRenderTargetData` drains the GPU pipeline, including the
previous present's blend, on the thread that has to reach the next vblank. The worst case is
a missed refresh. It is a good instrument for a graded question and the wrong one for a
per-grab equality question.

`-subgen` implies `-diffmap` unconditionally, so there is currently no command-line path
that runs the pixel check under DLSS-G at all.

The driver change map is free for the relay's threads and NOT free for the source. Measured
on Avatar 90x2, three benchmark runs with the flag off against three with it on, each set
within six score points of itself:

| | score | displayed fps | GPU time per frame |
|---|---|---|---|
| without `-diffmap` | 5021 to 5032 | 177.3 to 177.6 | 11.26 to 11.28 ms |
| with `-diffmap` | 4568 to 4581 | 161.8 to 162.2 | 12.37 to 12.40 ms |

The game was GPU-bound at 87% either way; the 8160-block difference runs on the GPU the game
renders on and took 1.1 ms of every frame, a 9% loss. A second-order effect follows: the grab
itself gets slower (intra-pair spacing 275 us without the map, 868 us with it), so the second
grab lands after the next flip more often and more distinct members reach the ring. Any
instrument that replaces the map has to be measured on this axis too. 256 texel reads from a
texture already on the capture device should cost the source nothing the benchmark can see;
the same three-plus-three comparison is how that gets confirmed.

## The instrument

An equality test on a sparse, fixed set of pixel positions, run on the CAPTURE thread
beside the `diff=` count, logging its verdict next to the driver's on every second member.

### Where it hooks in

`CaptureRing::CaptureLoop`: the grab lands in `m_captureTarget` and is copied into the ring
slot's render-target texture with `StretchRect`. The instrument reads the SLOT, on the GPU,
after that copy. Nothing new touches the frame on the CPU and nothing is added between the
grab returning and the next grab being issued.

### The gather: no lock, no drain

1. A pixel shader on the capture device renders an N x 1 render target. Pixel i samples the
   slot texture once, point-filtered, at stored position (u_i, v_i). One draw, N texel
   reads, a few microseconds of GPU time queued behind the copy that just happened.
2. Each slot owns its N x 1 target and a matching system-memory surface. The draw is issued
   when the slot is filled; nothing is read yet.
3. On the NEXT wake, `GetRenderTargetData` copies the previous slot's N x 1 target (N x 4
   bytes) to its system-memory surface and `LockRect` reads it. The GPU finished that draw a
   whole source period ago, so the copy has nothing to wait for; this is a 1 KB transfer of
   completed data, not a pipeline drain.
4. On the second member of a batch, compare its N words against the first member's and log.

The verdict for a pair therefore lands one or two wakes after the pair arrived. For the
referee it is irrelevant when it lands. For a production check it is still early: the policy
does not look at a slot until the bracketing lag has elapsed (96 ms today with -lag 75), so a
slot can carry a pending verdict for a wake and be finalized long before anyone reads it.

### Why not read the frame on the CPU

Two ways exist and both were rejected.

- Lock the ring slot. Impossible: the reference for `IDirect3DTexture9::LockRect` states
  "This method cannot retrieve data from a texture resource created with
  D3DUSAGE_RENDERTARGET because such a texture must be assigned to D3DPOOL_DEFAULT memory
  and is therefore not lockable. In this case, use instead GetRenderTargetData", which is a
  full-surface copy and a sync, worse than any lock.
- Lock `m_captureTarget`, the offscreen plain grab surface, which the reference says is
  always lockable. Possible, but the lock is a GPU sync of driver-defined cost that would sit
  between member 1 and member 2 of a pair, inside the 3 ms keep-real window that the whole
  batch-collapse rule depends on. A content instrument that can perturb the thing it is
  measuring is the wrong instrument. The gather reads the same pixels off the same frame
  with no sync anywhere near the grab loop.

### Sample pattern

- Positions stored as fractions (u, v) in [0, 1), so one pattern serves every resolution.
- Stratified, not uniform random: a 16 x 16 grid of cells with one jittered sample per
  cell, so a localized difference receives coverage proportional to its area.
- The SAME positions on both frames of a comparison. Mandatory; nothing is comparable
  otherwise.
- Fixed for the run. An equality test cannot be gamed by a static pattern.

### Statistics: how many samples

Two frames sampled at N common positions read as identical only if every sample lands
outside the region that differs. If that region is a fraction f of the frame:

    P(miss) = (1 - f)^N        N = ln(P_miss) / ln(1 - f)

| differing fraction f | N for P_miss <= 1e-3 | <= 1e-6 | <= 1e-9 | what f looks like |
|---|---|---|---|---|
| 0.75 | 5 | 10 | 15 | SM generated frame under camera motion (p10 6135 of 8160 blocks) |
| 0.25 | 25 | 49 | 73 | a quarter of the frame |
| 0.06 | 112 | 224 | 335 | the DLSS-G non-duplicates (~500 of 8160 blocks) |
| 0.01 | 688 | 1375 | 2062 | ~80 blocks |
| 0.001 | 6905 | 13809 | 20713 | one HUD counter |

N = 256. It catches any difference over 6% of the frame at P_miss ~ 1e-7 and a genuine
interpolated frame under motion (f >= 0.25) at 1e-32. It cannot see a one-HUD-counter
change; that needs thousands of samples, which is no longer sparse, and belongs to the
driver map (a complete 16x16 census) or a full-frame hash. It does not need to: a
"generated" frame that differs by a HUD counter is not a generated frame, and a driver that
lies about a real interpolated frame lies about the whole moving area.

The hash width is irrelevant to this. The only way two different frames read as equal is
the sampling miss above; a 64-bit hash adds a 2^-64 term. Keeping the 256 raw words (1 KB
per slot) and comparing them directly is simpler and exact.

### Logging

Per second member, on the existing capture line or beside it:

    gencheck: blocks=<driver count> same=<0|1> ndiff=<samples that differ> rb=<us>

where `rb` is the time the deferred readback took, and in the shutdown summary:

    gencheck summary: <n> pairs, agree <n> (<pct>), driver-dupe/samples-differ <n>,
    driver-change/samples-same <n>, readback median <us> p95 <us> worst <us>

`ndiff` is what makes the disagreement cases readable: 3 of 256 differing is a partial or
torn grab; 200 of 256 is a different frame.

### Flag

`-gencheck`, capture-thread only, no effect on any decision. It runs alongside `-diffmap`
(the two are the point of the comparison) and does not alter `-subgen`'s use of the map.

## The runs

| capture | generator | expected driver dupe rate | what it settles |
|---|---|---|---|
| Avatar 60x2, `-subgen -gencheck` | DLSS-G | ~99.3% | whether the map is true under DLSS-G |
| KCD 60x2, `-subgen -gencheck` | Smooth Motion | ~7.5% | that the instrument agrees where the map is already trusted |

## Reading the result

| `blocks=0` and `same=1` | `blocks=0` and `same=0` | verdict |
|---|---|---|
| ~99% of pairs | ~0 | the map is TRUE. The generated frame never reaches NvFBC. `-subgen` is dead on this path under DLSS-G by supply, not tuning; default stays OFF; the only route to generated pixels is a capture that taps the scanout plane (DXGI duplication). Close the question. |
| ~0 | ~99% of pairs | the map LIES on repeat grabs under DLSS-G. The frames are distinct and the ring is throwing them away. Replace the duplicate refusal with this check and re-run 30x2 `-subgen`, which should then substitute on nearly every blend. |
| mixed | mixed | read `ndiff`: small counts are partial grabs (the ~500-block non-duplicates already seen), large counts are real frames. The map is partly right; the check becomes the arbiter and the map an accelerator. |

Whichever way it lands, the `rb` column and the intra-batch `dt` census with the flag on
decide whether the instrument is shippable as-is as a production content check on the D3D11
path. The expectation is that it is: a 1 KB copy of finished data has no reason to cost
anything.

## Risks and how they are watched

- **Pair collapse.** Nothing in the design sits between members of a pair, but the draw and
  the deferred readback are still work on the capture thread. Watch the intra-batch `dt`
  census with the flag on against the same capture without it (median 162 to 187 us today
  on DLSS-G, 1841 us on Smooth Motion). Any movement is a defect in the instrument.
- **Readback cost is driver-defined.** `GetRenderTargetData` on finished data should be a
  1 KB copy; if a driver serializes it behind queued work anyway, `rb=` shows it, and the
  fix is to defer by one more wake, not to change the design.
- **Sampling blind spot.** Differences under ~5% of the frame can read as equal. Accepted
  by design; see the statistics. `ndiff` on the disagreements shows whether anything is
  living in that gap.
- **Format.** The grab surface is A2B10G10R10, four bytes per pixel. The read must use the
  surface format, not assume it; a future format change should fail loudly at setup.

## Performance, side by side

| | `GeneratedContentUsable` | 256-sample gather |
|---|---|---|
| thread | present | capture |
| reads the frame from | ring slot, via 3 full downscales | ring slot, via one 256-texel gather |
| GPU work | 3 downscale blits, then a full drain | one N x 1 draw, queued |
| CPU work | 6912-px luma convert, RMS | 256-word compare |
| sync | pipeline drain, every call | none: readback deferred to finished data |
| locks | none | none |
| measured | 0.8 ms mean, 27 ms worst | to be measured (`rb=`) |
| answers | graded ratio | identical or not |
| blind to | nothing above noise | < ~5% of the frame |

## Not in scope

Replacing `GeneratedContentUsable`'s graded verdict. If the map turns out partly right and
partial grabs matter, the graded check still has a job on the D3D9 path; this instrument
answers the equality question only, which is the one the driver map claims to answer.
