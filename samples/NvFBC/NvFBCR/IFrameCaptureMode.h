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