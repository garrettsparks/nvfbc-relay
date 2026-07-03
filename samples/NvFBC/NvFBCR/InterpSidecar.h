#pragma once

#include <windows.h>
#include <d3d9.h>
#include <d3d11.h>
#include "CaptureRing.h"

// D3D11 interpolation sidecar: reads ring slots via their shared handles, converts the
// bracket frames to 8-bit BGRA (alpha forced to 1.0 — the prior FRUC attempt's dimming had
// the alpha byte as prime suspect), feeds them to the NvOFFRUC engine, and exposes the
// interpolated result back to the D3D9 present device as a shared surface.
//
// The present stack stays D3D9 (T8 validation is tied to it); this device is a third
// participant in the existing multi-device design, using the same manual coherency
// discipline (event query + flush) at every cross-API hand-off — D3D9Ex shared surfaces are
// unsynchronized by specification.
//
// Failure policy: Setup failures are loud and leave the sidecar disabled (caller falls back
// to blend); runtime Process failures fall back per-frame, and kMaxConsecutiveFailures in a
// row disables the sidecar for the session (LOGERR once).
class InterpSidecar {
public:
    InterpSidecar();
    ~InterpSidecar();

    InterpSidecar(const InterpSidecar&) = delete;
    InterpSidecar& operator=(const InterpSidecar&) = delete;

    // Call AFTER CaptureRing::Start (slot shared handles must exist).
    bool Setup(IDirect3DDevice9Ex* presentDevice, CaptureRing* ring, int width, int height,
               LARGE_INTEGER baseQpc, LONGLONG freqQpc);

    // Interpolate the bracket at targetQpc. On success the frame is in OutputSurface9().
    // false -> caller falls back (blend/nearest). Never throws, never blocks unboundedly.
    bool Interpolate(const FrameBracket& bracket, LONGLONG targetQpc);

    IDirect3DSurface9* OutputSurface9() const { return m_outSurface9; }
    bool Enabled() const { return m_enabled; }
    LONGLONG LastProcessUs() const { return m_lastProcessUs; }

private:
    bool CreateDeviceAndRingAliases(CaptureRing* ring);
    bool CreateConversionPipeline();
    bool CreateOutputShare(IDirect3DDevice9Ex* presentDevice);
    bool CreateFruc();
    bool ConvertSlotToBgra(int ringSlot, int inputIdx);   // ring alias -> m_frucInput[inputIdx]
    void FlushD3D11();
    double QpcToSeconds(LONGLONG qpc) const;

    // D3D11 infra
    ID3D11Device* m_dev11;
    ID3D11DeviceContext* m_ctx11;
    ID3D11Texture2D* m_ringAlias[CaptureRing::RING_SIZE];   // opened from ring shared handles
    ID3D11ShaderResourceView* m_ringSrv[CaptureRing::RING_SIZE];
    ID3D11VertexShader* m_convVs;
    ID3D11PixelShader* m_convPs;
    ID3D11SamplerState* m_convSampler;
    ID3D11Query* m_flushQuery;

    // FRUC-registered resources: 2 input ping/pong + 1 output (NvOFFRUC_MIN_RESOURCE = 3)
    ID3D11Texture2D* m_frucInput[2];
    ID3D11RenderTargetView* m_frucInputRtv[2];
    ID3D11Texture2D* m_frucOutput;

    // Cross-API output path: FRUC output -> CopyResource -> shared -> opened on D3D9
    ID3D11Texture2D* m_sharedOut11;
    IDirect3DTexture9* m_outTexture9;
    IDirect3DSurface9* m_outSurface9;

    // FRUC engine (loaded dynamically; see NvOFFRUC.h proc-name contract)
    HMODULE m_frucLib;
    void* m_frucHandle;             // NvOFFRUCHandle
    void* m_fnCreate;
    void* m_fnRegister;
    void* m_fnUnregister;
    void* m_fnProcess;
    void* m_fnDestroy;

    int m_width;
    int m_height;
    LARGE_INTEGER m_baseQpc;
    LONGLONG m_freqQpc;
    int m_inputIdx;                 // ping/pong cursor
    LONGLONG m_lastFedTs;           // newest ring timestamp already fed to FRUC
    int m_consecutiveFailures;
    bool m_enabled;
    LONGLONG m_lastProcessUs;       // wall time of the last NvOFFRUCProcess (telemetry)
};
