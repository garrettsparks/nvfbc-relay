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

    // Whether the present device should be created on the TARGET (capture-card) adapter rather
    // than the source. Only modes that bind NvFBC to a SEPARATE capture device (the CaptureRing
    // two-device modes) may return true — others write NvFBC straight to the present device, so
    // it must stay on the source adapter. Default: source.
    virtual bool PresentsOnTargetAdapter() const { return false; }

    // Whether to attempt D3D9 EXCLUSIVE FULLSCREEN on the present adapter. This is the documented
    // (pre-DWM) way to try to bypass DWM composition and lock INTERVAL_ONE to the present
    // display's own vblank instead of the primary's composition clock. Only meaningful combined
    // with PresentsOnTargetAdapter + vsync present; device creation fails fast if FS is
    // unavailable (no windowed fallback, so a failed FS isn't silently measured as windowed).
    // Default: no (windowed).
    virtual bool WantsExclusiveFullscreen() const { return false; }

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