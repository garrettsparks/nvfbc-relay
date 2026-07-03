# Init-Path Cleanup — Feature Spec

Branch: `claude/init-cleanup` (off dev @ v0.0.11). Independent of all other branches.
Status: implemented. Behavior-affecting change: one leaked allocation removed; everything
else is dead code and a corrected log line.

## Changes (all `NvFBCR.cpp`)

1. **`InitD3D9Surfaces` leak fix.** The function created an offscreen plain surface into
   `g_backbuffer` and on the next line overwrote the pointer with `GetBackBuffer` — leaking
   one full-resolution 10-bit surface per launch and returning an `hr` that described the
   dead allocation, not the pointer actually used. Now: `GetBackBuffer` only, with its own
   error check. (Vestige of the FLIPEX experiments; the FLIPEX findings comment at the top of
   the file is retained deliberately — it documents *why* the presentation model is what it
   is.)
2. **"Buffer size" log fired before `BUF_WIDTH/BUF_HEIGHT` were assigned** — logged
   uninitialized values every run since the DPI-awareness change. Moved after the assignment.
3. **Dead commented-out experiment lines removed**: `X8R8G8B8` backbuffer format, `FLIPEX`
   swap effect, `D3DPRESENTFLAG_VIDEO`, trailing `eMode` format alternatives.
4. **Dead shadowed `int i = 0;`** before the `for (int i = ...)` loop in `InitDisplays`.

## Why this waited

All four sit in the shared init path that `vsync`/`60` use; the baseline's rollback stance
("gold modes byte-untouched") deliberately deferred touching them until V1–V7 had signed off
the baseline. That happened at v0.0.10/v0.0.11.

## Validation

Item 1 changes an allocation; items 2–4 are inert. A V1-class spot check
(`60_x1_vsync_ufo`-style, few minutes) plus confirming the startup log shows the real buffer
size is sufficient. Full suite not required — no timing-path code is touched.
