# Scaling on WGC

Status (2026-10-18): built as `-wgcscale bilinear|bicubic|lanczos` (with `-wgc`, not in the help
text), bilinear by default; with `-wgc` a game display of another size than the output is
always captured and scaled. Benchmarked as built on 2026-10-18 ("The shipped form's
benchmark": equal sizes cost nothing; bilinear and the experiment `xcomposite` the same, so
`xcomposite` was removed); not yet committed or through CI. Measured on 2026-10-16 as the experiment `-xwgcscale`; results in "Results" at the
end, under the experiment's names.

## Why

Capture through Windows.Graphics.Capture (`-wgc`) is used only when the game's display and the
capture card's display are the same size. Otherwise the relay stays on NvFBC, which can scale
inside its grab (`SOURCEMODE_SCALE`). A 4K game display into a 1440p card, or a 1440p one into a
1080p card, is common, so WGC has to scale before it can be the default capture anywhere (the
fork for the Steam app plans WGC as its default).

## Where the scaling goes

In the draw the present already makes. Each present samples a captured frame (one pool texture,
or two for a blend) and writes the back buffer. Scaling is only a different way of sampling in
that same draw: no extra pass, no extra texture, no extra visit to the graphics engine.

That placement matters more than the filter. GPU traces of the relay (2026-10-15 and -16) show
each visit the relay makes to the game's graphics engine costs the game about 0.17 ms, about two
thirds of it the context switching on either side, whatever the visit does. Anything that adds a
visit pays that: NV12 conversion added about 90 visits a second and cost 91 to 160 points.
NvFBC's scaling looks free for the same reason: it happens inside the grab's own pass, a write
NvFBC makes anyway.

## Candidates

All but the last run in the composite shader, on the frame's own pixels, with the output's size
from the viewport and the frame's from the texture, so a frame captured before WGC took over
(already at the output's size) passes through at 1:1.

| name | what | taps per output pixel at 1440p to 1080p |
|---|---|---|
| point | nearest source pixel | 1 |
| linear | the GPU sampler's bilinear filter, unscaled (what a plain blit gives) | 1 fetch (4 texels) |
| tri | triangle (bilinear) kernel widened by the ratio, so a downscale averages what it drops | 9 |
| cubic | Catmull-Rom bicubic, widened by the ratio | 36 |
| lanczos | Lanczos-3, widened by the ratio | 64 |
| area | each output pixel the exact area-weighted average of the source pixels it covers | 9 |
| vp | the GPU's video processor (D3D11 `VideoProcessorBlt`), fixed-function scaling, for presents of one frame; blends use linear | n/a |

On a downscale a kernel has to widen with the ratio, or detail finer than the output can hold
folds back as moiré (aliasing). Point and plain linear do not widen. Upscales (a 1080p game into
a 1440p card) use the kernels at their own width.

A blend filters both frames and mixes them, so it costs twice the taps; blends are about 7% of
presents in the benchmark and well under 1% in a capped stream.

Prediction (a guess, from the traces): every shader candidate costs about the same, within a few
points of point, since the draw's own work is about a third of a present's cost and texture
fetches at these counts are cheap; vp costs more, because the video processor works in a context
of its own (as DXVA2's did for NV12).

## Measurements

The game display stays at 2560x1440 240 Hz; the card's display goes to 1920x1080 at 60 Hz (the
card's own 1080p mode, measured at 59.9999 Hz). That is a downscale of 1.333, the common case.
The card's display goes back to its 2560x1440 custom 60 Hz mode afterwards.

1. **Correctness.** The policy suite equals its baseline (the switch adds a launch option and
   nothing else to the policy). With the switch off, or with displays of one size, nothing
   changes: the relay with no switch matches the reference build's picture exactly through the
   card.
2. **Quality.** A test image on the game display (greyscale, so the card's 4:2:0 colour does not
   enter): a zone plate (aliasing), fine text at several sizes, 1 px and 2 px lines, a slanted
   edge (sharpness), hard black and white edges (ringing), ramps. The card's output is captured
   uncompressed on the Mac and compared with the same image scaled offline by each kernel's own
   formula (does the shader do what it says) and with an offline Lanczos-3 (the best a scaler
   could do), region by region: error against the reference, residual moiré in the zone plate,
   the edge's 10 to 90% rise in output pixels, the overshoot at a hard edge. NvFBC's scaled grab
   goes through the same, to name what NvFBC uses. Enlarged crops of text and the zone plate for
   the eye.
3. **Cost.** One-launch benchmark sittings (Avatar, 1440p, DLSS frame generation 2X), four rows
   of each setting, a no-relay row at both ends and in the middle, paired against a reference
   setting in the same cycle: the shader candidates against point; NvFBC's scaled grab, the
   chosen shader candidate and vp against each other. A GPU context switch trace of the finalists
   to see the draw's hold per present.

## Choosing

The cheapest candidate whose picture cannot be told from the best at stream quality. If the
shader candidates all cost the same, the better picture wins outright and there is nothing to
configure; a setting is worth having only if quality and cost really trade.

## The switch

The experiment: `-xwgcscale <point|linear|tri|cubic|lanczos|area|vp|lanczos2p|cubic2p>` (the
last two were added after the first sittings showed one-pass Lanczos's cost): with `-wgc`, let
WGC capture a display of another size than the output and picked the filter. Without it nothing
changed.

## The shipped form (2026-10-18)

The user's choices of 2026-10-18: bilinear by default, bicubic and Lanczos as options, a flag
to choose in the real build, scaling only for frames of another size, scaling last in the
present. The names are OBS's own (its scale filter menu: Point, Bilinear, Bicubic, Lanczos,
Area), so a streamer reads them the same way in both.

`-wgcscale <bilinear|bicubic|lanczos>`, with `-wgc`; not in the help text while `-wgc` is not.
With `-wgc`, a game display of another size than the output no longer leaves the relay on
NvFBC: WGC captures it at its own size and the present scales it.

| shipped | the experiment's name | what |
|---|---|---|
| `bilinear` (default) | `linear` | one pass with the GPU's bilinear sampler; the same picture as NvFBC's scaled grab |
| `bicubic` | `cubic2p` | Catmull-Rom bicubic widened by the ratio, in two passes |
| `lanczos` | `lanczos2p` | Lanczos-3 widened by the ratio, in two passes |

All three filters are drawn the same way, by the scaler in place of the present's composite.
The shaders are the experiment's: the two-pass ones unchanged (bicubic's kernel number aside),
bilinear its `linear` path with the lines that path never used taken out. Dropped from the
shipped code, with their results kept below: point, tri, area, vp and the one-pass bicubic and
Lanczos.

For the benchmark the build also had `xcomposite`, bilinear drawn another way: the present's
own composite shader with a bilinear sampler in place of its point sampler, no shader of its
own, but reading both of a present's inputs even when it shows one frame (the user, 2026-10-18:
"simplicity is not worth performance necessarily ... if it's exactly the same performance, we
can take the simpler linear implementation if it's actually simpler. let's bench it all
though"). It cost the same as `bilinear` and gave the same picture, and was removed (below).

A present scales only when one of its frames is a WGC texture of another size than the output.
A frame NvFBC grabbed before WGC took over is at the output's size already. If bicubic's or
Lanczos's shaders cannot be made, the log says so and bilinear is used. The log says the filter
at start and counts the presents at the end:

    D3D11Present: a WGC frame of another size than the output is scaled with bilinear (-wgcscale)
    WGC: the source display is 2560x1440 and the output 1920x1080; the present scales its frames
    WGC scaling summary: N presents scaled with bilinear, M drew frames at the output's size unscaled

Checked: the policy suite (C++20 and C++17) and zig builds; the suite pins the default, the
names, a refused name and the relaunch round trip, and fails when the relaunch drops
`-wgcscale`. Then the benchmark and picture checks of 2026-10-18 below. Owed: CI.

## The shipped form's benchmark (2026-10-18)

Builds: zig builds of `088a5db` and of the shipped form, so both sides share a compiler. Game
display 2560x1440 at 240 Hz; the card's display at 2560x1440 (its custom 60.0002 Hz mode), then
1920x1080, 1280x720 and 3840x2160, each at 60.000 Hz. `b:vsync -src 60`. Every benchmark row was
checked for its build, its capture source and its scaling line, and every row presented about
7,340 to 7,400 times a run with 1 to 7 refreshes showing no new frame.

### Equal sizes cost nothing

With the card at the game display's size, every WGC setting of the shipped form logged 0
presents scaled, and the card's picture through the capture card matched `088a5db`'s byte for
byte (the test image, `-wgc` with each filter and `xcomposite`, and NvFBC on both builds; only
the test program's blinking marker differed, and a one-pixel shift of the reference changed two
million pixels, so the comparison sees a difference). One launch, four rows each, against
`088a5db -wgc` in the same cycle (points of the score; no relay scored 5,390 to 5,424):

| setting | paired | fitted |
|---|---|---|
| `-wgc` (bilinear) | +4.5 +- 2.1 | +4.9 +- 4.3 |
| `-wgcscale bicubic` | -4.8 +- 9.8 | +0.7 +- 4.6 |
| `-wgcscale lanczos` | -5.3 +- 7.5 | +0.6 +- 4.6 |
| `-wgcscale xcomposite` | -6.5 +- 4.6 | -0.7 +- 4.4 |
| NvFBC, shipped form against `088a5db` | -4.2 +- 4.0 | -4.3 +- 4.3 |

At equal sizes the four WGC settings run the same code, and they scatter on both sides of zero.

### What each costs at other sizes

One launch at each size, four rows of each setting, paired against bilinear in the same cycle
(fitted in brackets):

| setting | 1440p to 1080p (1.333) | 1440p to 720p (2) | 1440p to 4K (0.667, an upscale) |
|---|---|---|---|
| `xcomposite` | +2.4 +- 2.9 (+3.5 +- 6.2) | -4.3 +- 3.3 (-3.9 +- 3.7) | -1.9 +- 1.8 (+0.4 +- 2.0) |
| `bicubic` | +6.2 +- 10.0 (+11.3 +- 6.4) | +3.5 +- 3.8 (+4.8 +- 3.8) | +17.0 +- 1.1 (+19.9 +- 2.1) |
| `lanczos` | +11.4 +- 7.3 (+15.4 +- 6.8) | +13.5 +- 4.6 (+13.6 +- 4.0) | +33.1 +- 3.9 (+32.1 +- 2.2) |
| NvFBC's scaled grab | +67.4 +- 5.7 (+71.6 +- 6.6) | +68.2 +- 2.6 (+68.6 +- 3.9) | +147.3 +- 3.1 (+147.6 +- 2.1) |
| bilinear's own cost | about 185 | about 182 | about 194 |

At 1.333 bicubic and Lanczos read lower than the experiment's +17 and +25, within about two
standard errors; that sitting was the noisiest of the day (8.7 points a row against 2.8 to 6.0).
At 4K the two-pass filters cost about twice what they do downscaling, since the output has 2.25
times the pixels of 1440p. NvFBC costs most at 4K because its grab writes frames at the output's
size, so its ring holds and copies 4K frames, where WGC's frames stay at the source's size and
only the present's draw grows.

A GPU context switch trace at 1080p (two traced runs of each): the present's hold per present at
the median 0.051 and 0.051 ms with bilinear, 0.050 and 0.051 with `xcomposite`; the game held the
engine 950.8 to 952.0 ms a second in all four. **Bilinear and `xcomposite` cost the same** at
every size measured, and give the same picture.

### What each does to the picture at other sizes

The main test image and a second one of steps between mid greys (96 to 160, 64 to 192, 112 to
144, 128 to 208, 48 to 112), grabbed uncompressed from the card. "Own" is the mean difference
from the same filter computed offline (the card's noise is about 0.3 of a level), "text" the
mean difference from Lanczos-3 in the text, "ringing" the overshoot past a mid-grey step as a
share of the step, against the computed filter's in brackets:

| ratio | filter | own | text | ringing |
|---|---|---|---|---|
| 1.333 | bilinear, `xcomposite`, NvFBC | 0.30, 0.30, 0.31 | 0.72, 0.78, 0.72 | 0.7% (0.0) |
| 1.333 | bicubic | 0.27 | 0.47 | 0.9% (0.5) |
| 1.333 | lanczos | 0.28 | 0.03 | 1.3% (1.1) |
| 2 | bilinear, `xcomposite`, NvFBC | 0.24, 0.24, 0.24 | 0.99, 1.10, 1.11 | 0.7% (0.0) |
| 2 | bicubic | 0.29 | 0.51 | 1.5% (1.2) |
| 2 | lanczos | 0.27 | 0.13 | 1.6% (1.5) |
| 0.667 | bilinear, `xcomposite`, NvFBC | 0.33, 0.33, 0.34 | 0.93, 0.89, 0.89 | 0.7% (0.0) |
| 0.667 | bicubic | 0.29 | 0.38 | 6.5% (6.2) |
| 0.667 | lanczos | 0.27 | 0.04 | 11.8% (11.1) |

Bilinear's 0.7% is the capture card's own floor. **NvFBC's scaled grab is plain bilinear at
every ratio measured**, upscaling included. **Ringing at mid-grey edges is under 2 levels on a
downscale; on an upscale Lanczos rings by about 12% of a step** (a halo of about 7 levels on a
64-level step) and bicubic by about 6.5%, the kernels' own amounts. At exactly 2:1 bilinear
averages each 2x2 block, so it removes 1 px line pairs entirely there. Not measured: a 4K source
(the rig's game display is 1440p), so 4K into 1080p itself; its 2:1 ratio was measured at 1440p
into 720p.

### Bilinear or xcomposite: bilinear (the user, 2026-10-19)

The rule the user set (2026-10-18): "if it's exactly the same performance, we can take the
simpler linear implementation if it's actually simpler", and "it seems simpler to me to just
have all of the scalers implemented in the same way". The performance and the picture were the
same. `xcomposite` had fewer lines but was a special case in the present (its own sampler and
its own branch in the draw); `bilinear` draws every scaled present through the scaler like the
other two filters, and the present's composite is untouched. The user, 2026-10-19: "bilinear so
all modes apply the same". `xcomposite` was removed from the code and the suite; the suite
equals its baseline after the removal.

## Results (2026-10-16)

Setup: game display 2560x1440 at 240 Hz, the capture card's display 1920x1080 at 60.000 Hz (a
1.333 downscale), WGC with its frame pool on a device of its own, `b:vsync -src 60`.

### What each filter does to the picture

A greyscale test image on the game display, the card's output captured uncompressed. Each
shader's output matched the same filter computed offline from the source to within 0.26 to 0.30
of a level, the card's own noise (point excepted: it lands exactly on pixel boundaries every
third pixel, where rounding picks either neighbour).

| filter | text error against Lanczos-3 (levels) | moire past the output's limit (source swing 90) | 1 px lines, swing left (ideal 0) | detail kept just below the limit |
|---|---|---|---|---|
| point | 2.56 | 90.3 | 127.5 | 1.00 (all of it aliased) |
| linear | 0.79 | 47.8 | 69.2 | 0.79 |
| NvFBC's scaled grab | 0.79 | 47.7 | 69.0 | 0.79 |
| vp (the video processor) | the same as linear, frame for frame | | | |
| area | 0.82 | 37.9 | 51.8 | 0.75 |
| tri | 1.15 | 23.9 | 28.4 | 0.65 |
| cubic, and cubic2p | 0.40 | 33.0 | 29.3 | 0.85 |
| lanczos, and lanczos2p | 0.03 | 34.1 | 10.1 | 0.97 |

**NvFBC's scaled grab is plain bilinear** (it matches linear to 0.3 of a level everywhere), and
so is the GPU's video processor. Lanczos keeps the most detail and removes the most fine-line
moire; cubic is close; tri removes the most moire near the limit but is the softest; area sits
between linear and tri. The two-pass forms give exactly the one-pass picture.

### What each costs the game

One-launch benchmark sittings, three rows of each, paired against a reference in the same cycle
(points of the score; no relay scores about 5,400):

| sitting | setting | against | paired | fitted |
|---|---|---|---|---|
| A | linear | point | -0.1 +- 6.9 | +2.2 +- 4.3 |
| A | area | point | +5.0 +- 6.5 | +14.3 +- 9.1 |
| A | tri | point | +5.5 +- 3.8 | +9.8 +- 5.2 |
| A | cubic | point | +35.8 +- 8.9 | +41.9 +- 6.3 |
| A | lanczos | point | +66.8 +- 5.8 | +74.5 +- 7.6 |
| B | NvFBC's scaled grab | WGC linear | +74.4 +- 6.8 | +76.7 +- 7.0 |
| B | vp | WGC linear | +62.7 +- 5.8 | +67.0 +- 10.2 |
| B | lanczos | WGC linear | +69.1 +- 5.9 | +74.9 +- 14.2 |
| C | cubic2p | WGC linear | +16.6 +- 5.9 | +16.7 +- 15.6 |
| C | lanczos2p | WGC linear | +25.4 +- 3.7 | +25.7 +- 10.7 |
| C | lanczos | WGC linear | +63.7 +- 7.1 | +63.6 +- 21.6 |

WGC with point or linear cost the game about 174 to 191 points in all. **WGC with linear scaling
costs about 74 points less than NvFBC's scaled grab for the same picture**: NvFBC's scaling only
looks free because NvFBC itself costs more. **The video processor costs about 63 points more than
the shader for the same picture**: it works in a GPU context of its own, so each present adds a
visit to the graphics engine. Lanczos in one pass reads 64 source pixels for each output pixel
and costs about 70 points; cubic about 36 to 42. **Done in two passes**, across into a
half-float texture the output's width and the source's height, then down, Lanczos reads about 16
source pixels for each output pixel and costs about 25 points; cubic about 17. Both passes run in
the present's own visit to the graphics engine, so the second pass adds work but no visit.

### Why, by a GPU context switch trace

One launch, one traced run each (10 s from 40 s in; traced scores are diagnostics):

| setting | the relay on the graphics engine | the present's hold per present | the game's time on the engine a second |
|---|---|---|---|
| WGC linear | the present's context, 60 visits a second | 0.051 ms | 951.3 ms |
| WGC lanczos2p | the same | 0.101 ms | 949.5 ms |
| WGC lanczos, one pass | the same | 0.235 ms | 941.3 ms |
| WGC vp | the present's (0.037 ms, blends) and the video processor's own, 55 visits a second at 0.068 ms | | 940.6 ms |
| NvFBC's scaled grab | NvFBC's capture context, 128 visits a second at 0.088 ms, and the present's | 0.050 ms | 943.7 ms |

The filters cost what their draws take: one-pass Lanczos holds the engine 4.6 times as long as
linear, the two-pass form twice as long. The video processor adds a context and its visits, as
DXVA2's conversion did for NV12. NvFBC's capture visits cost the game more than Windows' capture
for WGC does (Windows' compositor, about 115 visits a second at 0.058 ms).

### Equal sizes

In the experiment, without the switch nothing of this ran. Since `scale-3` (2026-10-18), and in
the shipped form, a present scales only when one of its frames is a WGC texture of another size
than the output; a frame at the output's size takes the plain composite, so displays of one
size cost what they did before. The log counts both kinds of present. (Before that gate the
scaler ran on every present once the switch was on; at 1:1 every filter reduces to the frame
itself, so the picture was the same, but the work was spent.) Not yet checked on the rig.

### Where the scaling happens

In the present's composite draw, the last GPU work before `Present`, on the present's device.
WGC's frames stay at the source's size in the frame pool and are scaled as they are drawn: once a
present, a blend filtering both its frames. NvFBC instead scales inside its grab, on the game's
GPU, and its ring holds frames at the output's size. Scaling at the present matters for a second
GPU (cost record 9.3): the present would run there, so the scaling would too, and the game's GPU
would only capture. The price is that the frames crossing to the second GPU are the source's
size (14.7 MB at 2560x1440, 33 MB at 4K) rather than the output's; what that crossing costs is
not measured.

### The choice (Claude's recommendation, a default the user owns)

The user, 2026-10-18: linear by default, since it matches NvFBC's picture, costs least and has
less text error than area and tri; cubic and Lanczos as options (shipped as `bilinear`,
`bicubic` and `lanczos`, "The shipped form"). (Text error is measured against
Lanczos, which favours the sharper filters; linear lands closer to it than area and tri because
those two are softer.) What linear gives up against lanczos2p is the fine-detail moire it keeps
(69 against 10 on 1 px lines), the same moire NvFBC's grab has today.

Claude's earlier recommendation, kept for the record:

**lanczos2p as the default**: the best picture measured (Lanczos-3 widened with the ratio), about
25 points over linear, and still about 50 points cheaper than NvFBC's bilinear grab is today.
**linear as the one alternative** for anyone who wants the last 25 points (it is what NvFBC gives,
and costs nothing over point). The others drop out: point aliases badly; area and tri cost about
what linear does but are softer or barely better; cubic in one pass and Lanczos in one pass cost
more for the same pictures as their two-pass forms; cubic2p saves about 9 points on lanczos2p and
keeps more of the fine-line moire (29 against 10); the video processor costs about 63 points for
linear's picture.

Not measured on 2026-10-16: ringing (halos at hard edges on mid-grey; the test's edges are black
and white, which hide it); upscaling (a smaller game display than the card's); ratios other than
1.333, such as 4K into 1080p, where one-pass kernels read more (a ratio of 2 makes Lanczos 144
reads a pixel in one pass, 24 in two). All three measured on 2026-10-18 ("The shipped form's
benchmark"), 2:1 as 1440p into 720p.
