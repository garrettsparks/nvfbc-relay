#pragma once

#include <windows.h>
#include <dxgi.h>

// Blocks until the vertical blank of a SPECIFIC display, identified by HMONITOR, via
// IDXGIOutput::WaitForVBlank. Used by the DxgiLock (t:dlock) present mode to self-gate the flip
// to the capture-card's vblank in exclusive fullscreen — the capture-card driver does not honor
// INTERVAL_ONE, so we wait on the target output's vblank ourselves, then PresentEx(IMMEDIATE).
//
// Pure timing primitive: it does not capture, render, or present. DXGI is used only as a vblank
// clock and coexists with the D3D9 present device.
class VBlankWaiter {
public:
    VBlankWaiter();
    ~VBlankWaiter();

    VBlankWaiter(const VBlankWaiter&) = delete;
    VBlankWaiter& operator=(const VBlankWaiter&) = delete;

    // Find the IDXGIOutput whose monitor matches `monitor` and retain it. Returns false if no
    // DXGI output matches (unusual topology / headless).
    bool Setup(HMONITOR monitor);

    // Block until the next vblank of the target output. Returns false if the output was lost
    // (adapter reset / unplug) — a paced loop should then break.
    bool Wait();

    bool IsReady() const { return m_output != nullptr; }

private:
    IDXGIOutput* m_output;
};
