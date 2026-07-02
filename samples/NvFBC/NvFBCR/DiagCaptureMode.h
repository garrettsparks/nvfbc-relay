#pragma once

#include "IFrameCaptureMode.h"
#include "PresentScheduler.h"

// Diagnostic mode — measures the candidate phase-lock reference clocks (spec: "Open questions —
// phase-lock reference clock"). No ring, no selection; NOWAIT grab keeps the output image alive
// while every tick logs:
//   - DwmGetCompositionTimingInfo: qpcVBlank / refresh period / refresh count (candidate A —
//     does DWM's compose clock hold steady, and what does it do under G-Sync?)
//   - GetRasterStatus on a private TARGET-adapter device: is the capture card's raster readable
//     from a windowed device, and how does its phase drift vs QPC? (candidate B)
//   - present block time + inter-present delta.
//
// Two variants:
//   diag        — QPC 60Hz timer + IMMEDIATE present (steady probe; raster drift vs QPC visible)
//   diag:vsync  — INTERVAL_ONE present (block time/pdt = DWM's actual delivery cadence; under
//                 G-Sync this directly shows whether windowed INTERVAL_ONE follows the game rate)
class DiagCaptureMode : public IFrameCaptureMode {
private:
    PresentScheduler m_scheduler;
    bool m_vsyncPresent;
    IDirect3DDevice9Ex* m_rasterDevice;   // private device on the TARGET adapter, raster reads only

public:
    DiagCaptureMode(bool vsyncPresent);
    virtual ~DiagCaptureMode();

    virtual UINT GetPresentationInterval() const override;
    virtual bool Setup() override;
    virtual void Run(
        NvFBCToDx9Vid* nvfbcDx9,
        NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams,
        IDirect3DDevice9Ex* device,
        HWND hwnd) override;
    virtual const char* GetModeName() const override;
};
