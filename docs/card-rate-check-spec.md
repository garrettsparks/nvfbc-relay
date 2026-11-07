# The capture card's refresh rate check

2026-11-06. Status: approved by the user 2026-11-06; built the same day, the rig test owed.

## 1. Why

The relay shows the game on the capture card's display, one picture per refresh of that display.
A game drawing a whole number of frames a second (60) and a card display refreshing at a rate a
little off it drift apart, and every time they drift by a whole frame the card shows one picture
twice or skips one. How often that happens is one over the difference:

| card display rate | difference from 60 | one repeated or skipped frame every |
|---|---|---|
| 59.950550 Hz (a common 2560x1440 "60 Hz" timing: 241.50 MHz pixel clock, 2720x1481 total) | 0.0494 | 20 s |
| 60.000199 Hz (the same totals at 241.70 MHz, a custom resolution) | 0.0002 | 84 min |
| 60.000000 Hz | 0 | never |

The 59.95 Hz timing is what Windows picks for a card that offers 2560x1440 at about 60 unless a
custom resolution is made and selected, and the custom resolution has been lost without a sign
more than once: after a display rearrangement (2026-09-24), with the custom entry still shown as
selected (2026-10-16), and after a capture card swap (2026-11-05). Each time it was found only
afterwards, from a log or a check run on purpose. The user asked for the relay to notice it at
start (2026-10-16: "relay should have the capability to read the exact timing and throw up a
warning if it's off"; again 2026-11-05).

OBS's capture source shows "FPS: 60.0002" for such a card. That figure is the interval of the
card's USB video format (16.6666 ms, 60.00024 frames a second), not the rate of the HDMI signal
the relay sends, which is what this check reads. They round to the same number by coincidence.

## 2. What it reads

Windows' display configuration (`QueryDisplayConfig`, already called at start by
`AddFriendlyNames` in `src/relay/Displays.cpp`) lists each active display's target mode. Its
signal information holds the pixel clock and the total size, blanking included, and the exact
rate is

    pixel clock / (total width x total height)

The rounded figure next to it (`vSyncFreq`, 59951/1000 for the 59.95 timing) and
`EnumDisplaySettings`' whole hertz (59 or 60) are not exact enough. Read 2026-11-05 on the rig:
241,500,000 / (2720 x 1481) = 59.950550 Hz and 241,700,000 / (2720 x 1481) = 60.000199 Hz, for the
same card before and after its custom resolution.

`DisplayInfo` gains the exact rate (0 when it could not be read), filled in the same pass that
gives each display its name: the path's target mode, found through `targetInfo.modeInfoIdx`.

## 3. The rule

Take the whole number nearest the card display's exact rate, n (at least 1), and the difference
d = |rate - n|. A frame repeats or skips every 1/d seconds. **Warn when that is under 10
minutes**, that is when d is over 1/600 Hz (about 0.0017).

| rate | warns | why |
|---|---|---|
| 59.950550 | yes | every 20 s |
| 59.940060 (59.94, broadcast video) | yes | every 17 s |
| 60.000199 | no | every 84 min |
| 60.000000, 120.000000, 144.000000 | no | never |
| 0 (not read) | no | logged as not read |

The rule lives in `src/core` as a function of the rate alone, so the policy suite tests it
without Windows.

## 4. Where it shows

- **The display list** at the prompts gives each display's exact rate to two decimals, more when
  two would read as a whole number it is not: `[1] Capture Card, 2560x1440 at 59.95 Hz, at
  (2560,-1440)`; the custom 60.000199 reads `60.0002`. A rate not read shows no figure.
- **Right above the mode prompt**, after the usage list, the warning text in the console in
  yellow when the rule fires, wrapped at spaces to 79 columns by the usage list's own wrapper;
  the console's own colors come back after it. The run goes on to the mode prompt. It first came
  right after the card display was picked, in one unwrapped line, and the usage list printed
  under it pushed it out of view: on the rig 2026-11-06 the user found it "easy to miss with all
  of the options text below" and asked for it moved, colored and wrapped "the same way we wrap
  the option descriptions". Plain yellow, not bright: Windows' default color scheme makes bright
  yellow a pale cream (the user: "a bit pale").
- **A launch from the command line or a shortcut** (no prompts) shows nothing; the log line below
  says it. The user, 2026-11-06: "maybe we should skip the warning if you're launching from like
  a shortcut. it can be in the log, but don't like throw up an extra popup".
- **The log**, always: a new line after `Target display refresh:`, which stays as it is (the
  tooth guard's line):
  `Target display timing: 59.950550 Hz (pixel clock 241.500 MHz, total 2720x1481); a frame repeats or skips about every 20 s against a whole-number rate`
  or `Target display timing: not read (...)`. Analysis tools read the new line; no existing line
  changes.

Placements considered: one check at start after every prompt for both launch paths (one place,
but at the prompts it comes after the mode is typed); the rate in the display list alone (easy
to miss); a yes or no question before going on (one more prompt on every start for a problem
that is rare); a popup on a command-line launch (dropped by the user). The user chose a note at
the prompts (2026-11-06: "let's stick with the note"), then moved it above the mode prompt and
colored it.

## 5. The words

Plain, no jargon (memory: UI text without jargon):

> The capture card's display is running at 59.95 Hz, not 60. The stream will repeat or skip a
> frame about every 20 seconds. To fix it, set the capture card's display to exactly 60 Hz with a
> custom resolution.

The user's wording (2026-11-06), "will" kept: a stream runs a whole number of frames a second
(OBS at 60), so a card display off a whole number repeats or skips on that schedule whatever
rate the game draws at; a game at 59.95 would only move the hitch from the relay to OBS.

The rate is the read one to two decimals and the whole number is n from the rule; the interval is
rounded to whole seconds under two minutes and to whole minutes above. The rate gets more
decimals when two would read as the whole number (59.995 shows as 59.995, not 60.00).

## 6. Not in this change

- Switching the display mode or making the custom resolution (the 2026-10-16 note: possible with
  `ChangeDisplaySettingsEx` when Windows offers a 60 Hz entry; making one is driver-specific).
- Comparing against the game's own rate, which is not known at start.
- The game display's rate.

## 7. Tests

- **Suite:** the rule on the table in section 3, each row; the interval text (20 s, 17 s, 84 min);
  a rate of 0 gives no warning; the note at 59.95 wrapped to its three lines at 79 columns. The
  usage list's wrapper moved into a function the note shares; the usage list is byte-identical
  before and after at every width from 20 to 200.
- **Builds:** zig on the Mac, then CI.
- **Rig, two launches** of `o:vsync -wgc`, about 2 minutes in all: the card at its custom
  60.000199 Hz (no note, the log line reads 60.000199), and the card switched to the standard
  59.95 Hz entry in Windows' display settings (the log line reads 59.950550). Each launch from the
  command line (a rig job: nothing shown) and from the prompts (the user, at the rig: the display
  list's rates, and the yellow note above the mode prompt only at 59.95). The user switches the card
  back and the display check reads 60.000199 again. A launch that cannot read the rate is not reproducible on the rig;
  the suite covers its rule and the log covers its text.
- One change per stream: a stream on the build before it is merged.
