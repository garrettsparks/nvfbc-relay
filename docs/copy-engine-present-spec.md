# Presenting from the copy engine

Status (2026-10-21): **the default with `-wgc`**, turned off by `-nocopyengine`. Built on the
user's word ("yeah do it", 2026-10-20) behind `-xcopyengine`, measured (Results, below), then made
the default with the frame marker on the copy path (the user: "it's either a good change that
everyone should use, or it's not"). Results before that change name the switch as it was then.
The measurements behind the design are in `docs/game-gpu-interruption-research.md` ("Measured
with the card display on the 5080"). Still owed: a stream and its streamcheck.

## Why

Every present of the relay's `-wgc` path takes the game's graphics engine once: the draw of the
captured frame into the back buffer goes in with the present, and the game is switched out and
back for it. In a benchmark run that is 60 visits a second costing the game about 14 ms a second
(sitting `loadpa1`, 2026-10-20), priced at about 62 to 71 points in 2026-10-15's traces.

What takes the visit is the GPU work, not the present. Under the benchmark, with the card display
on the 5080:

- a present with no work behind it (a DXGI flip present, or a composition swapchain present) takes
  no visit;
- a clear or a D3D11 copy before each present takes one visit per present, as the relay's draw
  does;
- a copy on a D3D12 copy queue into a displayable buffer, presented through the composition
  swapchain, takes **no visit**: 4,507 copies and presents in the traced run, the game holding the
  engine 983.6 ms a second as with nothing beside it.

A copy queue cannot write a DXGI swapchain's own back buffers (the device is removed with
`DXGI_ERROR_ACCESS_DENIED`), so the composition swapchain is what makes the copy engine usable for
the present.

## What changes

On by default with `-wgc` (`b:vsync`; nothing here depends on NvFBC); `-nocopyengine` turns it
off and the D3D11 present draws every frame onto its DXGI swapchain as before. A target that cannot
be built refuses the mode with a popup naming `-nocopyengine` (no automatic fallback: the D3D12
runtime and the composition swapchain are part of the Windows the relay supports, so a failure is
a fault to see).

1. **The present target.** The output window's content becomes a composition swapchain surface
   (presentation manager, bound to the window through DirectComposition), with two displayable
   buffers (`SHARED | SHARED_NTHANDLE | SHARED_DISPLAYABLE`) at the output's size and format, in
   place of the DXGI flip-model swapchain. Two buffers keep today's depth: the buffer shown before
   the current one comes back at each refresh and is the next one written, as the frame-latency
   wait does now.
2. **A device for presenting.** The presentation manager needs a D3D11 device made with
   `SINGLETHREADED | PREVENT_INTERNAL_THREADING_OPTIMIZATIONS` (the factory fails otherwise). The
   present's device is used from two threads (the capture thread opens WGC pool textures on it),
   so the presentation manager gets a device of its own, used only by the present thread. It owns
   the two buffers and does no drawing.
3. **A present of one frame as it is** (passthrough, the frame at the output's size and in the
   buffer's format): a D3D12 device's copy queue copies the WGC pool texture into the next buffer
   (both opened on D3D12 through NT shared handles), signals a shared fence, the presenting device
   waits on it, and the presentation manager presents. No graphics-engine work. **With the frame
   marker** the strip is written on the CPU into an upload buffer, in the pixels the marker shader
   draws, and the copy queue copies the frame around the strip's rectangle and the strip into it
   (three copies of separate pixels, so no barrier): a marked run takes the same path as an
   unmarked one. (The first build drew every marked present, which took the marked video away as
   a check of the copy path; fixed 2026-10-21.)
4. **Every other present** (a blend, a scaled frame, a frame from the ring's own surface): drawn as
   now by the present's device, its marker drawn by the shader, into the next buffer opened on that
   device, then a shared fence the presenting device waits on, then the present. These keep their
   visit.
5. **A hold** re-does the previous present's operation into the next buffer (a copy stays a copy).
6. **Pacing** stays the refresh of the card's display: the present thread waits for the next
   buffer to come back (its available event) where it waits on the frame-latency object now. No
   target times.
7. **Statistics.** The present-statistics summary (missed refreshes, presentation path) reads the
   presentation manager's statistics (present status, independent-flip and composition frames) in
   place of `GetFrameStatistics` and `IDXGISwapChainMedia`, with the same summary lines.
8. **Imports.** `d3d12.dll` and the dcomp functions are loaded at run time, so the import list
   stays the baseline's.

## Risks to check

- **The WGC frame's readiness on D3D12.** D3D11 synchronises a shared texture between devices for
  the relay today; a D3D12 copy gets no such help. If Windows hands over a frame before its
  compositor's copy into it has finished, the copy could read a picture part written. The desktop
  check below compares pictures byte for byte, and a marked run checks every frame.
- **The format.** A copy needs the pool texture's format and size to equal the buffer's. On an
  8-bit output (the card's on the 5080, 2026-10-20) both are `B8G8R8A8`. On a 10-bit output the
  relay's buffer is `R10G10B10A2`; there it draws, as the switch's log line says.
- **What took the relay's no-draw visit on 2026-10-15** is not known (the probe's bare present
  takes none, and a render target view made each present does not explain it). The traced row
  below shows whether the new path really leaves the game alone.
- **Pacing through buffer availability** is new to the relay; the desktop check counts presents,
  skipped presents and missed refreshes against the DXGI path.

## Tests

1. The policy suite (no change expected; this is the present backend) and the zig build.
2. Desktop, no game: `-wgc` against `-wgc -xcopyengine`, the levels pattern through the card with
   the picture compared byte for byte, presents a second, skipped presents, missed refreshes, and
   the log line saying how many presents were copies and how many draws.
3. One traced benchmark row each way in one launch: the relay's holds a second on the graphics
   engine should fall from 60 to about the share of presents that are not passthrough (34% of
   `loadpa1`'s), and its copies should not appear.
4. A sitting, `-wgc` against `-wgc -xcopyengine`, one launch, minutes said first. Before it, the
   rig's 2026-10-20 evening slowdown (every pass about a third slower than that morning) has to be
   understood or gone, so the points compare with the earlier sittings.

## Results (2026-10-20)

As built (`src/relay/CopyEnginePresent.cpp`, zig build `copyengine-1`), the card display on the
5080 at 60.0002 Hz:

- **Suite:** equal to the baseline byte for byte under C++17 and C++20, with `-xcopyengine` in the
  relaunch round trip and the defaults check.
- **Desktop, a still picture** (the test image on the game display, `-wgc` against `-wgc
  -xcopyengine`): every WGC pool texture opened on the D3D12 device; the card pictures equal byte
  for byte outside the test image's own moving 16x16 marker; 0 present failures, 0 frame-wait
  timeouts, 1 present skipped, 4 refreshes with no new frame (as the drawn path). 166 of 822
  presents were copies: the picture changed at a rate unrelated to the card's, so most presents
  were blends.
- **Desktop, a moving source** (the game display flipping between two greys at about 238 a
  second, `-nowgccursor` both ways): 1,228 of 1,240 presents copied, every one of 20 card frames a
  single grey (64 or 192), so no copy read a frame Windows had not finished; 2 refreshes with no
  new frame against the drawn path's 4.
- **Traced benchmark run** (sitting `ce1trace`, one launch, one run each): the relay's holds of
  the graphics engine 60.1 a second with `-wgc`, **19.0** with `-wgc -xcopyengine` (67% of
  presents copied, the visits left are the draws); the game's time off the engine 49.8 against
  44.5 ms a second, of it with the relay 14.8 against 8.6. Present failures 0, skipped 0, 24
  refreshes with no new frame either way. Both runs read about 137 fps, under the sitting's
  150 floor, from the evening's slowdown (research doc).
- **Cost** (sitting `ce1cost`, one launch, no relay at both ends, `-wgc` and `-wgc -xcopyengine`
  alternating five times, one run each, all checks passed with the fps floor at 125):
  `-wgc` cost 134.8 points (3.3%), `-wgc -xcopyengine` 98.2 (2.4%): **36.7 +- 2.2 points less
  (paired, n=5; fitted 36.9 +- 2.1)**, about 27% of the relay's cost. Each copy-engine run copied
  4,978 to 5,020 of about 7,350 presents (68%), 0 failed, 1 skipped, 5 or 6 refreshes with no new
  frame (the drawn path 5 or 6). The no-relay level was 4,039 and 4,047, the evening's slowdown;
  the cost is in points against rows of the same launch.
- **Cost at normal speed** (sitting `ce2cost`, 2026-10-21 after a reboot cleared the slowdown,
  the same design): no relay 5,403 and 5,429; `-wgc` cost 191.2 points (3.5%), `-wgc
  -xcopyengine` 118.6 (2.2%): **72.6 +- 3.8 points less (paired, n=5; fitted 72.3 +- 3.9)**, 38%
  of the relay's cost. Twice the slowed reading; why the slowed GPU read less is not measured (a
  guess: with every pass a third slower, each interruption was a smaller share of a frame). All
  checks passed at the normal fps floor.
- **Against OBS** (sitting `ceobs1`, 2026-10-21, one launch: no relay, then OBS's projector with
  game capture on a 2560x1440 canvas, `-wgc -xcopyengine` and `-wgc` three times each, then no
  relay; OBS's hook took each time, its projector on the card display): OBS 190.2 points (3.5%),
  `-wgc` 180.5, `-wgc -xcopyengine` 109.7 (2.0%). **The copy-engine relay costs the game 80.6
  +- 4.2 points less than OBS's game capture** (paired, n=3; fitted 82.5 +- 6.1), about 42% less;
  `-wgc` alone 9.8 +- 3.5 less, as on 2026-10-15 (17.1 +- 2.3). The copy engine against `-wgc`
  in this launch: 70.8 +- 1.8, as `ce2cost`.

- **Default build with the marker on the copy path** (zig build `copyengine-2`, 2026-10-21):
  suites equal to the baseline under C++17 and C++20 (the defaults check now expects the copy
  engine on; `-nocopyengine` round-trips through the relaunch and clears only its own field;
  `-copyengine` is unknown). Desktop, recorded from the card and decoded with the marker decoder
  against each log: drawn and marked (`-nocopyengine -mark`) 910 of 910 readable frames valid;
  copy engine and marked on a still picture 480/480 valid, every counter +1, all in the log,
  provenance 100%; on the 240 Hz two-grey source 480/480 valid, every frame a copied real frame,
  892 of 905 presents copied, all 892 with the marker. Forced-failure control (`copyengine-2ctl`,
  never committed, setup made to fail): the mode refused and the popup read "To start with the
  older output method, add -nocopyengine to the launch options."

- **The marker's cost on the copy path** (sitting `cemark1`, 2026-10-21, `copyengine-2`, one
  launch, `-wgc` and `-wgc -mark` twice each between no-relay rows): marked 119.2 points against
  122.8 unmarked, a difference inside the bench's noise: no measurable cost. Every copied present
  of the marked runs carried the marker (6,906 and 6,944), 0 failed.

## Expected effect (written before the build; measured above at 36.7 points)

An estimate, not a measurement: with about two thirds of a benchmark run's presents passthrough,
about 40 of the 60 visits a second go, roughly 40 to 47 points if each costs what 2026-10-15's
traces priced. In a game whose frames come at a steady rate more presents are passthrough, and
more would go.
