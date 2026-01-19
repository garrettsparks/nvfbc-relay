#pragma once

#include "IFrameCaptureMode.h"
#include <cuda.h>
#include <cuda_runtime.h>

// FRUC and Optical Flow SDK headers
#include "NvOFFRUC.h"
#include "nvOpticalFlowCuda.h"
#include "nvOpticalFlowCommon.h"

// Forward declarations - avoids circular includes
extern IDirect3D9Ex* g_pD3DEx;
extern IDirect3DDevice9Ex* g_pD3D9Device;
extern IDirect3DSurface9* g_backbuffer;
extern int BUF_WIDTH;
extern int BUF_HEIGHT;

// FRUC (Frame Rate Up-Conversion) based optical flow interpolation mode
// Phase 2 STUB - verifies CUDA build infrastructure
class FrucCaptureMode : public IFrameCaptureMode {
private:
    float m_targetFramerate;
    bool m_isVsyncMode;

public:
    // framerate: target framerate (0.0f = vsync mode)
    FrucCaptureMode(float framerate);
    virtual ~FrucCaptureMode();

    virtual UINT GetPresentationInterval() const override;
    virtual bool Setup() override;
    virtual void Run(
        NvFBCToDx9Vid* nvfbcDx9,
        NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams,
        IDirect3DDevice9Ex* device,
        HWND hwnd) override;
    virtual const char* GetModeName() const override;
};