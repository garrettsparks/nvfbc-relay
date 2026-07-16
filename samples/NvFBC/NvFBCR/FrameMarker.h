#pragma once

#include <windows.h>
#include <d3d9.h>

// Machine-readable per-frame marker burned into the presented output (design:
// docs/frame-marker-spec.md). One row of binary luma cells inside a one-cell black
// quiet-zone border at the top-left corner. PRESENT-SIDE ONLY: drawn onto the
// backbuffer after the content copy, never onto ring/capture surfaces (slots are
// shared - repeats re-present them and blend will read them as before+after pairs).
//
// v1 burns the counter, the bracket weight, and the pick code (all present-side data
// that exists today); the remaining v2 fields (interp flag, compositor ID, source
// provenance) are reserved and drawn black so the cell layout and the decoder never
// change. A downstream duplicate copies the whole frame including the burned pick,
// while a relay repeat burns a fresh marker with the repeat code, so repeat
// attribution is decidable from video alone.
//
// Cell layout (LSB-first within each field). COMPATIBILITY CONTRACT: this core layout
// is frozen once real captures exist - cells never move or change meaning. New data
// appends as extension cells/rows (each with its own sync cell and checksum), declared
// by the extension schema ID below, so decoders of any vintage read the core of any
// recording; unknown schemas degrade to core-only.
//   0      sync, always white ("marker present")
//   1-24   frame counter, 24-bit (wraps at 2^24, ~74 h at 60 fps)
//   25     interp flag (v2, reserved black)
//   26-29  weight, quantized w*15 (bracket weight; blend w once blend exists)
//   30-31  compositor ID (v2, reserved black = nearest)
//   32-33  source provenance (v2, reserved black)
//   34-36  pick code: 0 none, 1 before, 2 after, 3 after-adv, 4 before-adv, 5 repeat
//   37-39  extension schema ID: 0 = no extensions; nonzero defined by future schemas
//   40-43  checksum, 4-bit XOR of the payload nibbles (cells 1-39)
class FrameMarker {
public:
    FrameMarker();
    ~FrameMarker();

    // Creates the cell-strip surfaces on the PRESENT device. On failure the marker
    // disables itself (Burn keeps counting but draws nothing): the relay runs
    // unmarked rather than not at all.
    bool Init(IDirect3DDevice9Ex* device, int bufWidth, int bufHeight);

    // Draws the marker into the backbuffer corner and advances the counter. Call
    // between the content StretchRect and PresentEx. weightQ is the quantized
    // weight (0-15), pickCode the 3-bit pick encoding (see the cell layout).
    // Returns the counter value burned into this frame (the mark= log field).
    unsigned int Burn(IDirect3DSurface9* backbuffer, int pickCode, int weightQ);

private:
    // Grid geometry: one texel per cell plus a one-cell quiet zone on every side.
    // The strip is blitted at a fixed fraction of the output width (kCellsPerWidth
    // cells per width: 32 px cells at 1920) so the decoder locates cells by frame
    // fraction at any recording resolution.
    static const int kCells = 44;          // sync + 39 payload bits + 4 checksum
    static const int kGridW = kCells + 2;  // quiet zone left/right
    static const int kGridH = 3;           // quiet zone above/below
    static const int kCellsPerWidth = 60;

    // Primary draw path: write the grid into the sysmem strip, UpdateSurface it to
    // the default-pool copy, one point-sampled StretchRect into the marker region.
    // Constant ops per present regardless of cell count.
    bool BurnBlit(IDirect3DSurface9* backbuffer, const bool cells[kCells]);

    // Fallback if the blit path hits a driver constraint: quiet zone plus one
    // ColorFill per white cell, straight onto the backbuffer.
    bool BurnColorFill(IDirect3DSurface9* backbuffer, const bool cells[kCells]);

    IDirect3DDevice9Ex* m_device;
    IDirect3DSurface9* m_sysmem;   // kGridW x kGridH staging strip, one texel per cell
    IDirect3DSurface9* m_vram;     // default-pool copy StretchRect can source from
    RECT m_destRect;               // marker region on the backbuffer
    unsigned int m_counter;        // 24-bit present counter, wraps
    bool m_active;                 // false: Init failed or both draw paths failed
    bool m_blitPathOk;             // false: fell back to per-cell ColorFill
};
