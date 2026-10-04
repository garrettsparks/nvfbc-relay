#pragma once

#include "IPresentPath.h"
#include "FrameMarker.h"

// THE D3D9 PRESENT PATH: the compositor draws onto the D3D9 device's back buffer and
// PresentEx shows it through the swapchain the shell created on the output window.
//
// A windowed INTERVAL_ONE present does not wait on any monitor's vblank: it blocks on DWM's
// compose clock, whose rate follows whatever the SOURCE display is doing (the base rate
// under a fullscreen game, the displayed rate under in-game frame generation, the desktop
// rate with no game). The relay therefore inherits a present cadence, and a present jitter,
// that belong to another display, and the sink samples one, two or three of those presents
// per scan. The flip-model path exists to replace that; this one stays as the path that
// needs nothing from the window manager and carries every compositor.
//
// Hosts any IFrameCompositor (nearest, blend, interp): the compositor decides and draws,
// this path acquires the back buffer, burns the marker, presents and keeps the statistics.
class D3D9PresentPath : public IPresentPath {
public:
    // Takes ownership of the compositor.
    explicit D3D9PresentPath(IFrameCompositor* compositor);
    ~D3D9PresentPath();

    D3D9PresentPath(const D3D9PresentPath&) = delete;
    D3D9PresentPath& operator=(const D3D9PresentPath&) = delete;

    bool Setup(const RelayContext& ctx, CaptureRing* ring, const policy::PolicyConfig* cfg,
               bool mark, unsigned int markFrames, LARGE_INTEGER baseQpc,
               LONGLONG freqQpc) override;

    // Nothing to wait on here: the INTERVAL_ONE PresentEx is this path's pacing wait, and
    // Present reports how long it blocked.
    bool WaitForFrame() override { return true; }

    void Compose(const FrameBracket& bracket, CompositeOutcome* out) override;
    long long BurnMarker(const CompositeOutcome& out) override;
    LONGLONG Present(bool vsync) override;

    // A D3D9 swapchain reports its trouble through the present status, which Present logs;
    // there is no silent stall to detect.
    bool SwapChainStalled() const override { return false; }

    void LogSummary() const override;
    const char* Name() const override { return "D3D9 present"; }
    const char* RefusalAdvice() const override { return ""; }
    bool OwnsOutputWindow() const override { return false; }

private:
    void SamplePresentStats();

    IFrameCompositor* m_compositor;     // owned
    IDirect3DDevice9Ex* m_device;       // borrowed: the present device the shell created
    HWND m_hwnd;
    bool m_mark;
    FrameMarker m_marker;               // per-present provenance burn-in (inert unless -mark)

    // The back buffer of the present in flight: Compose acquires it, BurnMarker draws on
    // it, Present releases it after PresentEx. m_presentTarget is the surface the
    // compositor actually drew on, which is the shell's cached back buffer when acquisition
    // failed.
    IDirect3DSurface9* m_backbuffer;
    IDirect3DSurface9* m_fallbackBackbuffer;   // borrowed: RelayContext::backBuffer
    IDirect3DSurface9* m_presentTarget;
    long long m_presentFailures;
    long long m_backbufferFailures;

    // Present statistics, flip mode (-flipex) only: a bitblt swapchain reports zeroes.
    // lastSyncRefresh is the sink refresh count the previous present landed on; a gap wider
    // than one means refreshes went by showing no new frame of ours.
    IDirect3DSwapChain9Ex* m_presentStatsSwapChain;
    UINT m_lastSyncRefresh;
    long long m_missedRefreshes;
    long long m_statsSamples;
    LARGE_INTEGER m_baseQpc;
    double m_usPerTick;
};
