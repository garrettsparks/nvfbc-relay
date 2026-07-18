#pragma once

#include <windows.h>
#include <d3d9.h>
#include "CaptureRing.h"

// What the compositor produced this present, for the temporal log line and the frame
// marker. The composition DECISION itself is pure policy (TemporalPolicy.h); a
// compositor only executes it and reports what it did.
struct CompositeOutcome {
    const char* pickLabel;   // the line's pick= field ("none" when selection did not run)
    const char* opLabel;     // op= field label; NULL = no op=/bw= fields on the line
    int pickCode;            // marker pick cells (0 when selection did not run)
    int weightQ;             // marker weight cells, quantized 0-15
    bool synthesized;        // marker interp cell: output pixels are not one real frame
    double opWeight;         // bw= value when opLabel is set
};

// Per-present composition: turn the bracket into backbuffer pixels. One implementation
// per output strategy (nearest copies one real frame; blend lerps the pair); the
// present loop stays strategy-agnostic.
class IFrameCompositor {
public:
    virtual ~IFrameCompositor() {}

    // One-time device resources. Loud failure: the mode refuses to run rather than
    // degrade silently.
    virtual bool Setup(IDirect3DDevice9Ex* device, int width, int height) = 0;

    // Marker compositor-ID cell value (0 nearest, 1 blend).
    virtual int Id() const = 0;

    // Compose this present's output onto the backbuffer and fill the outcome. When
    // nothing is presentable yet (startup, before any frame exists) the backbuffer is
    // left untouched and the outcome still describes the decision.
    virtual void Compose(const FrameBracket& bracket, IDirect3DSurface9* backbuffer,
                         CompositeOutcome* out) = 0;
};
