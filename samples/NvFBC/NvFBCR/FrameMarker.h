#pragma once

#include <windows.h>
#include <d3d9.h>

// Machine-readable per-frame marker burned into the presented output (design:
// docs/frame-marker-spec.md). One row of binary luma cells inside a one-cell black
// quiet-zone border at the top-left corner. PRESENT-SIDE ONLY: drawn onto the
// backbuffer after the content copy, never onto ring/capture surfaces (slots are
// shared - repeats re-present them and blend will read them as before+after pairs).
//
// The marker burns the counter, the weight, the pick code, the interp flag, and the
// compositor ID; source provenance stays reserved and drawn black so the cell layout
// and the decoder never change. In nearest mode the weight is the bracket weight and
// interp/compositor read black/0; in blend mode the weight is the composed blend
// weight and the interp flag marks synthesized output. A downstream duplicate copies
// the whole frame including the burned pick, while a relay repeat burns a fresh
// marker with the repeat code, so repeat attribution is decidable from video alone.
//
// Cell layout (LSB-first within each field). COMPATIBILITY CONTRACT: this core layout
// is frozen - cells never move or change meaning. New data appends as extension
// cells/rows (each with its own sync cell and checksum), declared by the extension
// schema ID below, so decoders of any vintage read the core of any recording;
// unknown schemas degrade to core-only.
//   0      sync, always white ("marker present")
//   1-24   frame counter, 24-bit (wraps at 2^24, ~74 h at 60 fps)
//   25     interp flag: white = synthesized output, black = one real frame's pixels
//   26-29  weight, quantized w*15 (bracket weight in nearest mode; blend weight in blend mode)
//   30-31  compositor ID: 0 nearest, 1 blend
//   32-33  source provenance (reserved black)
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
    // between the content composition and PresentEx. weightQ is the quantized
    // weight (0-15), pickCode the 3-bit pick encoding, interp the synthesized-output
    // flag, compositorId the compositor cell value (see the cell layout).
    // Returns the counter value burned into this frame (the mark= log field).
    unsigned int Burn(IDirect3DSurface9* backbuffer, int pickCode, int weightQ,
                      bool interp, int compositorId);

private:
    // Grid geometry: kTexelsPerCell texels per cell plus a one-cell quiet zone on
    // every side. The strip is blitted at a fixed fraction of the output width
    // (kCellsPerWidth cells per width: 32 px cells at 1920) so the decoder locates
    // cells by frame fraction at any recording resolution.
    //
    // Multiple texels per cell defend against the D3D9 half-texel sampling
    // convention: driver StretchRect paths that anchor on texel centers stretch an
    // N-texel source across N-1 texel spans, which at one texel per cell displaces
    // far cells by a full cell width (measured: a 46x3 grid rendered as 45x2). At
    // kTexelsPerCell the worst displacement is 1/kTexelsPerCell of a cell, inside
    // the decoder's inner-50% center-sampling margin either way.
    static const int kCells = 44;          // sync + 39 payload bits + 4 checksum
    static const int kGridW = kCells + 2;  // quiet zone left/right
    static const int kGridH = 3;           // quiet zone above/below
    static const int kCellsPerWidth = 60;
    static const int kTexelsPerCell = 8;

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
