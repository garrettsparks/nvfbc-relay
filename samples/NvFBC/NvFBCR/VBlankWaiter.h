#pragma once

#include <windows.h>
#include <dxgi.h>

// Blocks until the vertical blank of a SPECIFIC display, identified by HMONITOR.
//
// Why this exists: D3D9 Present(INTERVAL_ONE) syncs to the vblank of the *device's adapter*
// (fixed at device creation), which in this relay is the SOURCE display. So plain vsync paced
// to the source's refresh, not the capture-card/target's — on a 240Hz-source / 60Hz-target
// setup that presented at 240/s (windowed DWM composition compounds it). WaitForVBlank against
// the TARGET's IDXGIOutput locks pacing to the named display regardless of which adapter the
// present device lives on or how DWM composites.
//
// This is purely a timing primitive: it neither captures, renders, nor presents. NvFBC capture
// and the D3D9 present path are untouched; DXGI is used only as a vblank clock. (DXGI is version-
// independent of D3D9, so this coexists with the D3D9 device.)
class VBlankWaiter {
public:
    VBlankWaiter();
    ~VBlankWaiter();

    // Owns a COM ref; non-copyable.
    VBlankWaiter(const VBlankWaiter&) = delete;
    VBlankWaiter& operator=(const VBlankWaiter&) = delete;

    // Find the IDXGIOutput whose monitor matches `monitor` and retain it. Returns false if no
    // DXGI output matches (unusual topology / headless) — a vblank-paced mode should treat that
    // as fatal rather than silently free-run.
    bool Setup(HMONITOR monitor);

    // Block until the next vblank of the target output. Returns false if the output was lost
    // (adapter reset / unplug); a paced loop should then break.
    bool Wait();

    bool IsReady() const { return m_output != nullptr; }

private:
    IDXGIOutput* m_output;
};
