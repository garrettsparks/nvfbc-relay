# Optional motion blur on the output

Status (2026-10-23): **design, not built.** Every choice here is a default for the user to
change. It follows the optical-flow interpolation work (`docs/optical-flow-interpolation-spec.md`)
and reuses its flow engine and planner where it needs motion at all. Sources at the end.

## 1. What it is for

The relay shows the game at 60 Hz. When the game runs faster than that, each output frame is
one instant of a scene that moved through several; at 60 Hz a fast pan reads as a series of
sharp steps (strobing) rather than continuous motion. A camera avoids this by keeping its
shutter open for part of the frame interval, so motion inside that time smears into a blur along
its path. Film's convention is a shutter open half the interval (180 degrees). This spec adds the
same, optionally, to the relay's output.

What the research says about whether people want it, in short: frame rate dominates perceived
judder and preference in controlled studies, and shutter angle has little measured effect at 48
and 60 fps (Daly et al.; Allison, Wilcox et al. 2016). A study in a racing game found blur changed
neither performance nor enjoyment though players could see it (Disney Research 2013). Players turn
off games' camera blur mostly for smeared targets on turns. No study of stream viewers was found;
the demand that exists is creators blending high-frame-rate recordings for their uploads. So it
ships **off by default**, with 180 degrees as the setting a user who turns it on gets.

## 2. Three ways, chosen by the source's rate

The output instant is the present's target `t` (the same target the policy brackets today). The
shutter is a window of `S` = angle / 360 x one output period, centred on `t`, so the blur's middle
is the instant the relay would have shown sharp and the blur adds no delay. Every frame the
window needs is already in the ring: the relay presents about 95 ms behind capture.

### 2.1 Accumulation: the source runs at least about three times the output

Each captured frame shows the game's picture from its stamp until the next frame's. The output is
that held signal averaged over the window: each ring frame weighted by how much of the window it
covers. At 240 into 60 with 180 degrees, about two frames, each half; with 360 degrees, about four.
This is how true shutter blur is measured from 240 fps footage (Brooks and Barron 2019 build their
ground truth exactly so), and it needs **no motion estimation at all**.

Two properties make it the cheap, safe case:

- **An opaque static HUD stays sharp for free**: it is the same in every frame averaged, so its
  average is itself. Nothing has to detect it.
- **Nothing can be wrong in the occlusion sense**: there are no vectors to misjudge, only real
  frames mixed in proportion to the time they were on screen.

Below about 180 fps a 180-degree window holds one frame (at 120 fps it is exactly one), so
accumulation alone does nothing there.

### 2.2 Flow blur: the source runs at or near the output rate

At 60 fps the window lies inside one frame's display, so the blur has to be made from motion: each
pixel averaged along its own path through the window. The path comes from the optical flow of the
bracket pair (the same engine and fields the `o` mode uses), scaled to the window: a pixel moving
`F` pixels per source period moves `F x S / source period` inside the window.

The gather is a reconstruction filter of the velocity-buffer family (McGuire et al. 2012; Guertin
et al. 2014; the forms Unity and KinoMotion ship):

1. **Tile maximum** of the window motion over tiles of the maximum blur radius (`r`, about 32 px),
   then its 3x3 neighbourhood maximum, so a fast object's blur can reach into a slow tile next to
   it.
2. **Gather** along the tile's dominant motion: an odd number of jittered taps (about 15), each
   weighted so that a sample blurs over the centre only if the sample's own motion reaches it
   (the "spread" or "cone" terms) and only if its motion runs the same way (the direction terms).
3. **Without depth.** The one thing these filters use depth for is which of two overlapping
   moving things is in front. Most of a game stream's motion is a camera pan, where a tile's
   motion is nearly uniform and the question does not arise; Unity's filter skips its depth test
   on such tiles on purpose (its "fast path"). For the rest, the substitute is the round-trip test
   the flow warp already has: content that is covered in the other frame is behind. Where the flow
   fails the round trip on both sides, the pixel is left sharp. A sharp pixel among blurred ones
   is a lesser error than a smear across an edge.
4. **HUD.** Zero flow means no blur, so a HUD the flow sees as static stays sharp. Where the flow
   follows the scene under an opaque HUD, the flow warp's static guard applies unchanged: pixels
   whose before and after frames agree around them are left sharp.

### 2.3 Hybrid: in between

Between about 90 and 180 fps, accumulate the ring frames the window holds and flow-blur each one
over its own share of the window, which closes the gaps between them (the arrangement Blender's
renderer ships). At 120 fps and 180 degrees it reduces to flow blur on one frame; at 240, to
accumulation with a tiny per-frame blur that can be left out.

## 3. Where it goes in the pipeline

It replaces the present's draw, after the decision, on the D3D11 present (`b:vsync`, and `o:vsync`
once rebuilt). The policy is unchanged: it still decides the target, the bracket and whether the
present holds. A synthesized present (a hole) is blurred around its synthesized frame the same
way. The frame marker is burned after the blur, so it stays sharp and decodable.

## 4. What it costs

- **Every present becomes a draw.** Today a `-wgc` present that shows one frame as it is goes
  through the GPU's copy engine and takes no turn on the game's graphics engine. A blurred present
  mixes frames, which is a draw, so with blur on every present takes a turn, as `-nocopyengine`
  does. On the 2026-10-22 numbers that is roughly the difference between the copy-engine `-wgc`
  (about 127 points) and the drawn one (about 192), a guess until measured with blur itself.
- **Accumulation** reads two to four full frames per present, a bandwidth pass, expected well
  under a millisecond at 1440p (not measured).
- **Flow blur** needs a flow field for every present's bracket, not only for synthesized ones:
  about 60 flows a second at a 60 fps source. The 5080's flow engine runs apart from the game's
  graphics engine (2026-10-23 `ofprobe4`, about 2.7 ms a pair at grid 4); what the game pays is
  turns on its graphics engine, 2 per flow call (2026-10-24 `oftrace2` to `oftrace4`). A draw on
  the flow's D3D11 context rides in those same 2 turns (`oftrace5`: draw alone 1 turn, 1.83%; flow
  alone 3.61%; both together 3.65%, at 60 a second). Since blur already makes every present a draw,
  the flow adds about 1 turn per present on top, roughly 1.8% at 60 a second. The gather
  at 15 taps: Guertin et al. measured 2 to 5 ms at 1080p on a 2013 GPU; today's should be several
  times faster (a guess).

## 5. Interface

`-blur <angle>`: 0 (off, the default) to 360; `-blur` alone means 180. The relay picks
accumulation, hybrid or flow blur from the measured source rate, and logs which on the resolved
options line and when it changes. With a source rate the relay cannot measure, flow blur.

## 6. Tests

- **Suite (pure code in `src/core`):** the window's frame weights for a target, shutter and ring
  stamps: each frame weighted by its overlap with the window, summing to 1; `S` = 0 gives the
  frame on screen at `t` alone, bit for bit; a window wholly inside one frame gives that frame;
  frames that never arrived leave their time to the frame before them (a stall blurs as a hold).
  The flow blur's gather as a CPU reference, held to: zero motion returns the input exactly;
  uniform motion of a known pattern matches its box-filtered truth; a static overlay over moving
  content stays sharp; tile and neighbourhood maxima on hand-made fields.
- **GPU self-test:** the shader against the CPU reference on generated frames, with controls that
  fail loudly (zero angle must equal the sharp frame; a pattern moving one way must not blur the
  other way).
- **Quality against ground truth:** from a 240 fps capture, accumulation over each window IS the
  ground truth for a 60 Hz shutter. Feed every fourth frame to flow blur (a 60 fps source) and
  score it against that truth (PSNR, SSIM; overall, HUD region, fastest-moving tenth), with sharp
  output and accumulation of the 60 fps frames (which is sharp) as controls.
- **Live rows:** the uncapped benchmark with `-wgc` at 180 degrees against `-wgc` sharp (the cost
  of turning it on); the game capped at 60 with flow blur (the expensive case); a marked recording
  of each, decoded, so every blurred frame's provenance is checked; then a stream, one change.

## 7. Order

After the interpolation work: the window weights and accumulation (no flow, the cheap case),
measured and streamed; then flow blur on the interpolation's flow engine; then the hybrid.

## 8. Decisions for the user

1. Whether to build it at all, given that studies find little viewer benefit at 60 fps and every
   blurred present gives up the copy engine's saving.
2. The default angle when turned on (180 recommended) and whether the window is centred on the
   target (recommended: no added delay) or ends at it.
3. Whether a HUD the flow cannot see deserves a hand-drawn mask option, as some offline tools
   offer; recommended no until a stream shows a HUD smearing.

## Sources

- Rosado, "Motion Blur as a Post-Processing Effect", GPU Gems 3 ch. 27:
  https://developer.nvidia.com/gpugems/gpugems3/part-iv-image-effects/chapter-27-motion-blur-post-processing-effect
  (depth plus the previous view-projection give per-pixel velocity; a box gather along it; camera
  motion only. The chapter carries no code licence and its code is not in NVIDIA's archive, so
  nothing of it is copied; what survives without depth is the gather.)
- McGuire, Hennessy, Bukowski, Osman 2012, "A Reconstruction Filter for Plausible Motion Blur";
  Guertin, McGuire, Nowrouzezahrai 2014, "A Fast and Stable Feature-Aware Motion Blur Filter":
  https://casual-effects.com/research/Guertin2014MotionBlur/Guertin2014MotionBlur-small.pdf
- Unity HDRP motion blur (read, not copied; Unity Companion License):
  https://github.com/Unity-Technologies/Graphics ; KinoMotion (MIT):
  https://github.com/keijiro/KinoMotion
- Brooks and Barron 2019, "Learning to Synthesize Motion Blur": https://arxiv.org/abs/1811.11745
- Sundaram, Brox, Keutzer 2010 (forward/backward consistency), as used in Jonschkowski et al.
  2020: https://arxiv.org/abs/2006.04902
- Perception: Daly et al. on judder (https://www.smpte.org/blog/psychophysical-study-isolating-judder);
  Allison, Wilcox et al. 2016, JIST 60(6); Sharan et al. 2013, "Simulated motion blur does not
  improve player experience in racing game" (Disney Research).
- Blender's accumulation plus post-process blur:
  https://docs.blender.org/manual/en/2.90/render/eevee/render_settings/motion_blur.html
