#pragma once

#include <windows.h>
#include <d3d9.h>
#include <d3d11.h>
#include "CaptureRing.h"
#include "FlowWarpEngine.h"

// D3D11 interpolation sidecar for the interp compositor: reads ring slots via their shared
// handles, converts the bracket frames to 8-bit BGRA for the optical-flow engine's inputs
// (alpha forced to 1.0 - NvFBC's desktop alpha is unspecified), and exposes the warped
// result back to the D3D9 present device as a shared surface. The engine is raw NVOFA flow
// plus our own warp (FlowWarpEngine), whose only NVIDIA runtime dependency is the driver's
// nvofapi64.dll. The warp samples the original 10-bit ring aliases and, driver permitting,
// renders through a 10-bit share, so the 8-bit hop is confined to flow estimation.
//
// The present stack stays D3D9; this device is a third participant in the existing
// multi-device design, using the same manual coherency discipline (event query + flush)
// at every cross-API hand-off - D3D9Ex shared surfaces are unsynchronized by
// specification.
//
// Failure policy: Setup failures are loud and leave the sidecar disabled; runtime
// failures return false per-frame (the caller renders its fallback), and
// kMaxConsecutiveFailures in a row disables the sidecar for the session (LOGERR once).
class InterpSidecar {
public:
    InterpSidecar();
    ~InterpSidecar();

    InterpSidecar(const InterpSidecar&) = delete;
    InterpSidecar& operator=(const InterpSidecar&) = delete;

    // Call AFTER CaptureRing::Start (slot shared handles must exist). freqQpc is the QPC
    // frequency, for the engine-time telemetry.
    bool Setup(IDirect3DDevice9Ex* presentDevice, CaptureRing* ring, int width, int height,
               LONGLONG freqQpc);

    // Interpolate the bracket at its own weight. On success the frame is in OutputSurface9().
    // false -> caller falls back to the lerp. Never throws, never blocks unboundedly.
    bool Interpolate(const FrameBracket& bracket);

    IDirect3DSurface9* OutputSurface9() const { return m_outSurface9; }
    bool Enabled() const { return m_enabled; }
    LONGLONG LastProcessUs() const { return m_lastProcessUs; }

private:
    bool CreateDeviceAndRingAliases(CaptureRing* ring);
    bool CreateConversionPipeline();
    bool CreateOutputShare(IDirect3DDevice9Ex* presentDevice);
    bool TryCreateOutputShare(IDirect3DDevice9Ex* presentDevice, DXGI_FORMAT fmt11,
                              D3DFORMAT fmt9);
    void ReleaseOutputShare();
    bool ConvertSlotToBgra(int ringSlot, int inputIdx);   // ring alias -> m_flowInput[inputIdx]
    void FlushD3D11();

    // D3D11 infra
    ID3D11Device* m_dev11;
    ID3D11DeviceContext* m_ctx11;
    ID3D11Texture2D* m_ringAlias[CaptureRing::RING_SIZE];   // opened from ring shared handles
    ID3D11ShaderResourceView* m_ringSrv[CaptureRing::RING_SIZE];
    // How many of those the ring actually allocated; the tail stays NULL. RING_SIZE is only
    // the array bound now that the lag can grow the ring.
    int m_ringSlots = 0;
    ID3D11VertexShader* m_convVs;
    ID3D11PixelShader* m_convPs;
    ID3D11SamplerState* m_convSampler;
    ID3D11Query* m_flushQuery;

    // The flow engine's 8-bit inputs, one per bracket side.
    ID3D11Texture2D* m_flowInput[2];
    ID3D11RenderTargetView* m_flowInputRtv[2];
    ID3D11RenderTargetView* m_sharedOutRtv;   // the warp renders straight to the share
    FlowWarpEngine m_flow;

    // Cross-API output path: warp output -> shared texture -> opened on D3D9
    ID3D11Texture2D* m_sharedOut11;
    IDirect3DTexture9* m_outTexture9;
    IDirect3DSurface9* m_outSurface9;

    int m_width;
    int m_height;
    LONGLONG m_freqQpc;
    int m_consecutiveFailures;
    bool m_enabled;
    LONGLONG m_lastProcessUs;       // wall time of the last flow + warp (telemetry)
};
