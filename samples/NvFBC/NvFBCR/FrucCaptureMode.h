#pragma once

#include "IFrameCaptureMode.h"
#include <atomic>
#include <cuda.h>
#include <cuda_runtime.h>
#include <cudaD3D9.h>
#include <NvFBC/nvFBCCuda.h>

// NvOF SDK headers for optical flow
#include "NvOFSDK/NvOFCuda.h"

// Forward declarations - avoids circular includes
extern IDirect3D9Ex* g_pD3DEx;
extern IDirect3DDevice9Ex* g_pD3D9Device;
extern IDirect3DSurface9* g_backbuffer;
extern int BUF_WIDTH;
extern int BUF_HEIGHT;
extern NvFBCLibrary* pNVFBCLib;

// Optical Flow based frame interpolation mode
// GPU-resident pipeline: NvFBC → CUDA → NvOF → interpolation → D3D9, zero PCIe crossings
//
// V7 architecture: Direct-to-backbuffer at native resolution. Three threads:
//   Capture thread: NvFBCCuda grab → downscale to backbuffer res → NvOF input ring
//   Flow thread: NvOF Execute directly from ring slots → double-buffered output
//   Present thread: interp kernel → CUDA→backbuffer copy → PresentEx (VSync)
// Working resolution = backbuffer resolution (no StretchRect needed).
// Flow(N+1) runs concurrently with Present(N), so budget = max(flow, present).
class FrucCaptureMode : public IFrameCaptureMode {
private:
    // Frame history entry - timestamp and validity for ring buffer slots
    static const int RING_SIZE = 6;  // 6 slots: 5 completed + 1 write, enough history to bracket prevVBlankTime
    struct FrameHistoryEntry {
        LARGE_INTEGER timestamp;                  // High-precision capture timestamp
        bool valid;                               // Whether entry contains valid data
    };

    // ===== Configuration =====
    float m_targetFramerate;
    bool m_isVsyncMode;
    int m_width;                                // Working resolution (= backbuffer res)
    int m_height;
    int m_captureWidth;                         // Full NvFBCCuda capture resolution
    int m_captureHeight;
    int m_flowWidth;                            // Width of flow vector grid
    int m_flowHeight;                           // Height of flow vector grid

    // ===== CUDA Resources =====
    CUcontext m_cuContext;
    CUdevice m_cuDevice;
    CUstream m_cudaStream;                      // Present thread stream (interp/output)
    CUstream m_captureStream;                   // Capture thread stream (downscale kernel)
    CUstream m_flowStream;                      // Flow thread stream (kept for future use)
    bool m_cudaInitialized;

    // ===== CUDA Events (GPU kernel timing) =====
    CUevent m_interpStartEvent;                 // Recorded before interp kernel launch
    CUevent m_interpEndEvent;                   // Recorded after interp kernel launch

    // ===== CUDA-D3D9 Interop (output only) =====
    CUgraphicsResource m_cudaOutputResource;    // Registered backbuffer for direct output

    // ===== NvOF (Optical Flow) =====
    NvOFObj m_nvOF;                              // NvOF CUDA object
    std::vector<NvOFBufferObj> m_inputBuffers;  // RING_SIZE input buffers (double as ring buffer)
    std::vector<NvOFBufferObj> m_outputBuffers; // 2 output flow vector buffers (double-buffered)
    bool m_nvofInitialized;
    uint32_t m_gridSize;                        // Flow vector grid size (e.g., 4 = 4x4 pixel blocks)

    // ===== Ring Buffer (NvOF input buffers used as ring, written by capture thread) =====
    int m_ringStride;                           // NvOF input buffer stride in bytes
    FrameHistoryEntry m_frameHistory[RING_SIZE]; // Timestamps per slot
    std::atomic<int> m_ringWriteIndex;          // Next slot to write (capture thread advances)
    std::atomic<int> m_capturedFrameCount;      // Total frames captured (atomic, cross-thread)

    // ===== GPU Buffers =====
    CUdeviceptr m_fullResGrabBuffer;            // Single full-res buffer for NvFBCCuda grabs
    IDirect3DSurface9* m_outputSurface;         // D3D9 surface for CUDA interop output
    CUsurfObject m_interpSurfObj;               // Surface object for direct kernel→D3D9 write
    bool m_outputMapped;                        // Whether D3D9 output surface is currently mapped

    // ===== D3D9 Resources =====
    IDirect3DDevice9Ex* m_device;

    // ===== Timing =====
    LARGE_INTEGER m_perfFreq;                   // Performance counter frequency

    // ===== Threading: Capture =====
    HANDLE m_captureThread;                     // Capture thread handle
    std::atomic<bool> m_captureRunning;         // Shutdown signal for capture thread
    std::atomic<bool> m_sessionInvalidated;     // Fatal error from capture thread
    std::atomic<int> m_captureGrabCount;        // For capture rate metric

    // ===== Threading: Flow Worker (pipelining) =====
    HANDLE m_flowThread;                        // Flow worker thread handle
    HANDLE m_flowRequestEvent;                  // Auto-reset: present→flow "start computing"
    HANDLE m_flowDoneEvent;                     // Auto-reset: flow→present "result ready"
    std::atomic<bool> m_flowShutdown;           // Shutdown signal for flow thread

    // Flow thread shared state (written by present thread before signaling, read by flow thread)
    int m_flowPrevSlot;                         // Ring slot index for previous frame
    int m_flowNextSlot;                         // Ring slot index for next frame
    int m_flowOutputIdx;                        // Which NvOF output buffer to write (0 or 1)
    std::atomic<double> m_lastFlowTimeUs;       // Flow thread timing (for present thread logging)

    // ===== NvFBCCuda =====
    NvFBCCuda* m_nvfbcCuda;                     // NvFBCCuda instance (used by capture thread)
    NVFBC_CUDA_GRAB_FRAME_PARAMS m_cudaGrabParams;  // Grab params template
    NvFBCFrameGrabInfo m_grabInfo;              // Filled by grab — stride detection

    // Stride handling: if dwBufferWidth != dwWidth, grab to temp then cuMemcpy2D to ring slot
    bool m_strideChecked;
    bool m_needsStrideCopy;
    int m_grabStride;                           // dwBufferWidth * 4 bytes
    CUdeviceptr m_grabTempBuffer;               // Temp buffer for padded grabs (0 if not needed)

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
    virtual bool ManagesOwnCapture() const override { return true; }

private:
    // ===== Initialization =====
    bool InitCuda();
    bool InitNvOF();
    bool AllocateBuffers();
    bool CreateNvFBCCuda();
    void Cleanup();

    // ===== Threading =====
    static DWORD WINAPI CaptureThreadProc(LPVOID param);
    static DWORD WINAPI FlowWorkerThreadProc(LPVOID param);

    // ===== Optical Flow & Interpolation =====
    bool InterpolateFrame(float weight, CUdeviceptr framePrev, CUdeviceptr frameCurr, int flowOutputIdx);

    // ===== GPU-Resident Present =====
    bool PresentFromGPU(IDirect3DDevice9Ex* device, LARGE_INTEGER* pPresentExStart, float* pInterpGpuMs);

    // ===== Utility =====
    void LogCudaError(const char* operation, CUresult result);
    void LogNvOFError(const char* operation, const std::exception& e);
};
