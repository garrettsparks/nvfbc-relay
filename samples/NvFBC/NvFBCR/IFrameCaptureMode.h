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

    // Return true if the mode creates its own NvFBC capture instance
    // (e.g. NvFBCCuda instead of NvFBCToDx9Vid)
    virtual bool ManagesOwnCapture() const { return false; }
};