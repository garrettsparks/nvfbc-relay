# Direct-Write Capture Ring — Feature Spec

Branch: `direct-write-capture-ring` (off dev @ v0.0.11). Successor item 1 of the dual-device
baseline. Status: implemented, awaiting validation.

## What it does

NvFBC writes captured frames directly into the shared ring slots instead of into an
intermediate capture-target surface that a per-frame `StretchRect` then copies into the ring.

- `NvFBCToDx9VidSetUp`: all `RING_SIZE` ring textures registered as output buffers
  (`dwNumBuffers = RING_SIZE`, `ppBuffer[i] = ring[i].capSurface`).
- Per grab: `dwBufferIdx = writeCount % RING_SIZE` chosen **before** the blocking grab; the
  frame lands in the slot in place.
- Deleted: the capture-target surface and the capture-side `StretchRect`.
- Unchanged: the coherency flush (event query + `D3DGETDATA_FLUSH` drain) still runs after the
  write and before publish — it is the T4 cross-device guarantee regardless of who wrote the
  slot. Batch collapse, retraction, timestamps, publish ordering, logging: all unchanged.

## Why the safety argument still holds

Slot choice moved before the grab, but the geometry is identical: the written slot is the
oldest ring position, one past FindBracket's scan window `[p−7, p−1]`, so the in-place write
can never touch a slot the present thread reads. Publish-then-never-touch is preserved — the
slot is mutated only while unpublished (the entry it evicts left the scan window 1 slot ago).

## Expected win

One full-frame GPU copy per captured frame removed (at 240 Hz source: 240 copies/s of a
1080p+ 10-bit surface). `flush=` now measures the drain of NvFBC's own write rather than our
StretchRect; if it collapses to ~0 that suggests NvFBC synchronizes internally before the grab
returns — the flush stays anyway (removing it would be a separate experiment against T4).

## Risk (spec-documented: either-works-or-doesn't)

1. **Loud**: driver rejects shared-handle render-target textures as NvFBC outputs →
   `NvFBCToDx9VidSetUp` fails at startup, `CaptureRing: NvFBCToDx9VidSetUp on capture device
   failed` — revert branch, finding recorded.
2. **Subtle**: corruption in direct-written slots (partial writes, wrong-slot writes) →
   caught by `detect.py reverse` (stale/out-of-order pixels) and pacing Roughness.

## Validation (light, per baseline spec)

| run | artifact | pass |
|---|---|---|
| `t:60` @240 UFO | `240_x1_t_60_dw_ufo` | reverse 0 (the corruption gate); 0 dupes; stride windows 0; `col` ≈ baseline; `flush` ≤ baseline |
| `t:vsync` KCD 120cap ×2 | `120cap_x2_t_vsync_dw_kcd` | jit ≈3 µs, pdt ≈16667 µs; collapse ≈47%; crisp reticle |

Acceptance: numbers unchanged vs v0.0.11 with lower flush+copy cost.
