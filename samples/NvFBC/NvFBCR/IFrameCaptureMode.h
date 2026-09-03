#pragma once

#include <windows.h>
#include <d3d9.h>
#include <NvFBCLibrary.h>
#include <NvFBC/NvFBCToDx9vid.h>

// Abstract interface for frame capture modes
class IFrameCaptureMode {
public:
    virtual ~IFrameCaptureMode() {}

    // Get the D3DPRESENT_INTERVAL value for device creation
    virtual UINT GetPresentationInterval() const = 0;

    // Whether the PRESENT device should be created on the target display's adapter rather
    // than the source's. D3D9 gives one adapter ordinal per OUTPUT, so a relay that captures
    // DISPLAY1 and presents to a window on DISPLAY2 has a choice to make, and the default is
    // wrong for anything that can move: a back buffer sized for the target can never match a
    // source-adapter display mode, so PresentEx returns S_PRESENT_MODE_CHANGED on every
    // present (measured: 27254 in one 259 s capture) and the runtime converts each frame.
    // It also makes the swapchain ineligible for independent flip, which requires the
    // swapchain to live on the adapter that scans it out.
    //
    // Only modes that DECOUPLE capture from present may move. The legacy single-loop modes
    // have NvFBC write straight into the present device's back buffer
    // (NvFBC_OutBuf[0].pPrimary = g_backbuffer), so their present device must stay on the
    // source adapter; the temporal modes rebind NvFBC to CaptureRing's own capture device and
    // are free.
    //
    // This does NOT change present PACING. That was measured in Rounds 7-8: a present-on-
    // target build still paced at the source's 240 Hz, because the compose clock is DWM's
    // property and not the device adapter's. Do not re-propose it as a pacing fix.
    virtual bool PresentsOnTargetAdapter() const { return false; }

    // Whether the mode presents through its own D3D11 flip-model swapchain on the output
    // window instead of the D3D9 device's swapchain. Flip model allows one swapchain per
    // window and no second API on it, so when this is true the D3D9 devices are created on
    // a hidden host window and the D3D9 swapchain never presents; the D3D9 device still
    // exists because NvFBC, the ring and its aliases are D3D9 objects.
    virtual bool PresentsViaD3D11() const { return false; }

    // Setup mode-specific resources
    virtual bool Setup() = 0;

    // Run the entire capture loop (including message processing)
    virtual void Run(
        NvFBCToDx9Vid* nvfbcDx9,
        NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams,
        IDirect3DDevice9Ex* device,
        HWND hwnd) = 0;

    // Get descriptive name for logging
    virtual const char* GetModeName() const = 0;
};