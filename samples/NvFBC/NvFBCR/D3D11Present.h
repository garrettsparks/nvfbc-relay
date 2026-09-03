#pragma once

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_3.h>

#include "CaptureRing.h"
#include "IFrameCompositor.h"
#include "FrameMarker.h"
#include "TemporalPolicy.h"

// D3D11 PRESENT BACKEND: decides, composites and presents straight onto a DXGI flip-model
// swapchain, replacing the D3D9 compose-and-PresentEx path for one present.
//
// WHY THIS EXISTS. The D3D9 present is a windowed INTERVAL_ONE, which does not wait on any
// monitor's vblank: it throttles on DWM's compose clock, whose rate follows whatever the
// SOURCE display is doing (the base rate under a fullscreen game, the displayed rate when
// that game runs in-game frame generation, the desktop rate with no game). The relay
// therefore inherits a present cadence, and a present jitter, that belong to another
// display entirely, and the sink samples one, two or three of those presents per scan.
//
// A flip-model swapchain whose window covers an output can be promoted by Windows to
// INDEPENDENT FLIP, where DWM leaves the present path and the display controller scans
// these buffers directly. The swapchain's frame-latency waitable object then signals on the
// SINK's vblank: one present per sink scan, phase-locked by construction, on every title
// regardless of source regime. That is the goal. It is NOT exclusive fullscreen: nothing here
// calls SetFullscreenState, and failing to be promoted degrades to being composed by DWM,
// which is what the relay does today.
//
// THE WAIT COMES BEFORE THE DECISION, not inside Present after the draw. A present that
// blocks after rendering settles into waiting almost a full extra frame between drawing and
// display; waiting on the swapchain's own object first, then deciding, drawing and queueing
// immediately, decides each frame as late as possible and shows it one sink period later
// rather than two.
//
// WHAT IS CERTAIN EVEN WITHOUT PROMOTION: flip model shares buffers with DWM instead of
// copying them into a redirection surface (one full-frame read+write removed per present),
// and GetFrameStatistics reports real data in windowed mode where a bitblt swapchain returns
// zeroes, giving an in-process missed-refresh counter: downstream dupes measurable from the
// log without a marked video and an offline decode.
//
// FEEDS OFF THE EXISTING RING. Ring slots are already shared render targets whose handles
// CaptureRing hands out for opening the same texture on another API's device, and
// FrameBracket already carries beforeSlot/afterSlot/genSlot so a cross-API consumer can pick
// its own per-device aliases. Nothing about capture changes. The capture device drains an
// event query before publishing a slot, so a published slot is GPU-complete for this device
// exactly as it is for the D3D9 present device.
//
// OWNS ITS OWN COMPOSITE STATE. DecideComposite mutates the state it is handed (both Schmitt
// bands, the last output, target and generated stamps), so the D3D9 compositor and this
// backend must never both decide the same present; a mode runs exactly one of them.
class D3D11PresentBackend {
public:
    D3D11PresentBackend();
    ~D3D11PresentBackend();

    D3D11PresentBackend(const D3D11PresentBackend&) = delete;
    D3D11PresentBackend& operator=(const D3D11PresentBackend&) = delete;

    // Call AFTER CaptureRing::Start (slot shared handles must exist). cfg is borrowed from
    // the owning mode and must outlive the backend. subGen arms generated-frame substitution;
    // mark/markFrames arm the frame marker as FrameMarker::Init does. Failure is loud and
    // leaves the backend disabled so the caller can refuse the mode rather than run degraded.
    bool Setup(HWND hwnd, CaptureRing* ring, int width, int height,
               const policy::PolicyConfig* cfg, bool subGen, bool mark, unsigned int markFrames);

    // Decide this present and draw it onto the current back buffer. Fills out exactly as
    // the D3D9 synthesizing compositor would, so the temporal log line reads the same.
    void Compose(const FrameBracket& bracket, CompositeOutcome* out);

    // Burn the frame marker over the composed back buffer. Returns the counter burned
    // (mark= on the temporal line), or -1 when the marker is off.
    long long BurnMarker(const CompositeOutcome& out);

    // THE FRAME-PACING WAIT. Blocks until the swapchain has room for the next frame, which
    // with a latency of one is the moment the previous frame was consumed: the sink's vblank
    // under independent flip, DWM's compose otherwise. Call at the top of every present,
    // before the decision. Returns false on the bounded timeout, which the caller treats as
    // a wait that did not pace anything.
    bool WaitForFrame();

    // Queue the drawn back buffer. Does not block: WaitForFrame already made room. vsync
    // selects sync interval 1 so the flip lands on a vblank rather than tearing.
    void Present(bool vsync);

    // Whole-run present and substitution statistics; safe to call when disabled.
    void LogSummary() const;

    bool Enabled() const { return m_enabled; }

    struct GenSubStats {
        long long substituted = 0;
        long long offered = 0;            // placement said yes
        long long skippedUnscreened = 0;  // offered, but the ring had no change map to screen it
    };
    const GenSubStats& GeneratedSubstitutionStats() const { return m_genSub; }

private:
    bool CreateDeviceAndSwapChain(HWND hwnd, int width, int height);
    bool OpenRingAliases(CaptureRing* ring);
    bool CreatePipeline();
    // Re-acquire back buffer 0 as the render target. Under flip model the buffer behind
    // index 0 changes after every Present, so this runs once per present.
    bool AcquireBackBuffer();
    // Draw lerp(a, b, w) over the whole back buffer. Passthrough binds the same slot to both
    // inputs with w = 0, so there is ONE code path and the swapchain format is free to differ
    // from the ring's (a CopyResource would demand they match exactly).
    bool Draw(int slotA, int slotB, float w);
    bool DrawMarker(const bool cells[FrameMarker::kCells]);
    void SampleStats();
    void SamplePresentationPath();

    // Marker compositor-ID cell: this backend is the blend pipeline.
    static const int kCompositorIdBlend = 1;
    // Synthesis executor code for the lerp, matching the D3D9 blend compositor's.
    static const int kExecBlend = 1;

    bool m_enabled;
    int m_width, m_height;
    int m_ringSlots;

    const policy::PolicyConfig* m_cfg;
    policy::CompositeState m_compState;
    bool m_subGen;
    GenSubStats m_genSub;

    ID3D11Device* m_dev;
    ID3D11DeviceContext* m_ctx;
    IDXGISwapChain1* m_swapChain;
    // Reports the swapchain's composition mode, an overlay plane against a composition
    // surface. Related to whether the present path is promoted but not equivalent to it, so it
    // informs that question rather than settling it. Purely diagnostic: NULL leaves the backend
    // fully functional with only the mode unreported.
    IDXGISwapChainMedia* m_swapChainMedia;
    HANDLE m_frameWait;                 // the swapchain's frame-latency waitable object
    ID3D11RenderTargetView* m_rtv;      // the current back buffer; recreated per present

    ID3D11Texture2D* m_ringAlias[CaptureRing::RING_SIZE];
    ID3D11ShaderResourceView* m_ringSrv[CaptureRing::RING_SIZE];

    ID3D11VertexShader* m_vs;
    ID3D11PixelShader* m_ps;
    ID3D11PixelShader* m_markerPs;
    ID3D11Buffer* m_cb;                 // one float4 for the lerp, one uint4 of marker cells
    ID3D11SamplerState* m_sampler;

    // What the last present drew, so a hold re-draws the same thing. Kept as inputs rather
    // than as a copied-out texture on purpose: snapshotting the output every present would
    // reintroduce exactly the full-frame copy this backend exists to remove. The slots are
    // still live at hold time because a hold is issued the present after its content was
    // drawn, and the ring keeps many presents of history.
    int m_lastSlotA, m_lastSlotB;
    float m_lastWeight;
    int m_lastExec;                     // executor code of the held pixels (0 = real)
    bool m_haveLast;

    bool m_mark;
    FrameMarker m_marker;               // counter and cell layout only; drawn here
    RECT m_markerRect;

    // Present statistics. lastSyncRefresh is the sink refresh count our previous present
    // landed on; a gap wider than one means refreshes went by showing no new frame of ours.
    UINT m_lastSyncRefresh;
    long long m_missedRefreshes;
    long long m_statsSamples;
    long long m_statsRebases;           // refresh-counter base changes: presentation path changed
    int m_compMode;                     // last DXGI_FRAME_PRESENTATION_MODE seen; -1 = none yet
    long long m_compModeChanges;
    long long m_samplesOverlay;         // presents scanned from an overlay plane
    long long m_samplesComposed;        // presents composed by DWM
    long long m_presentFailures;
    long long m_drawFailures;
    long long m_waitTimeouts;
    long long m_presents;
};
