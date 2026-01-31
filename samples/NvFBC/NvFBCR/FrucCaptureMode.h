#pragma once

#include "IFrameCaptureMode.h"
#include <cuda.h>
#include <cuda_runtime.h>
#include <cuda_d3d9_interop.h>

// FRUC SDK header (from NVIDIA Optical Flow SDK)
#include "NvOFFRUC.h"

// Forward declarations - avoids circular includes
extern IDirect3D9Ex* g_pD3DEx;
extern IDirect3DDevice9Ex* g_pD3D9Device;
extern IDirect3DSurface9* g_backbuffer;
extern int BUF_WIDTH;
extern int BUF_HEIGHT;

// FRUC (Frame Rate Up-Conversion) based optical flow interpolation mode
// Phase 4: Full FRUC interpolation with optical flow
class FrucCaptureMode : public IFrameCaptureMode {
private:
    struct FrameBuffer {
        IDirect3DSurface9* d3dSurface;           // D3D9 surface
        struct cudaGraphicsResource* cudaResource; // CUDA Runtime API interop resource
        CUdeviceptr cudaPtr;                     // CUDA device pointer (for FRUC)
        size_t pitch;                            // Buffer pitch in bytes
        bool isMapped;                           // Whether CUDA resource is currently mapped
    };

    // ===== Configuration =====
    float m_targetFramerate;
    bool m_isVsyncMode;
    int m_width;
    int m_height;

    // ===== CUDA Resources =====
    CUcontext m_cuContext;
    CUdevice m_cuDevice;
    bool m_cudaInitialized;

    // ===== Frame Buffers =====
    static const int NUM_INPUT_BUFFERS = 2; // FRUC needs 2 input buffers to compute optical flow
    FrameBuffer m_nvfbcBuffer;              // NvFBC captures here
    FrameBuffer m_inputBuffers[2];          // CUDA buffers for FRUC input (alternating)
    FrameBuffer m_outputBuffer;             // CUDA buffer for FRUC output
    int m_currentInputIndex;                // Which input buffer to write to next (0 or 1)
    int m_capturedFrameCount;               // Total frames captured

    // ===== FRUC Resources =====
    HMODULE m_frucModule;                   // NvOFFRUC.dll handle
    NvOFFRUCHandle m_frucHandle;            // FRUC instance handle
    PtrToFuncNvOFFRUCCreate m_frucCreate;
    PtrToFuncNvOFFRUCRegisterResource m_frucRegisterResource;
    PtrToFuncNvOFFRUCUnregisterResource m_frucUnregisterResource;
    PtrToFuncNvOFFRUCProcess m_frucProcess;
    PtrToFuncNvOFFRUCDestroy m_frucDestroy;
    bool m_frucInitialized;

    // ===== D3D9 Resources =====
    IDirect3DDevice9Ex* m_device;

    // ===== Timing =====
    LARGE_INTEGER m_perfFreq;               // Performance counter frequency
    LARGE_INTEGER m_lastPresentTime;        // Last presentation timestamp
    LARGE_INTEGER m_captureStartTime;       // When we started capturing

    // ===== Statistics =====
    int m_interpolatedFrameCount;           // Frames successfully interpolated
    int m_fallbackFrameCount;               // Frames where we fell back to source

public:
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

private:
    // ===== Initialization =====
    bool InitCuda();
    bool InitFruc();
    bool CreateFrameBuffers();
    void Cleanup();

    // ===== Frame Management =====
    // Returns: -1 = fatal error, 0 = no new frame, 1 = new frame captured
    int CaptureFrame(NvFBCToDx9Vid* nvfbcDx9, NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams);

    // ===== Utility =====
    void LogCudaError(const char* operation, CUresult result);
    void LogFrucError(const char* operation, NvOFFRUC_STATUS status);
};
