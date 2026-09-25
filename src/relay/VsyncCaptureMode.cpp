#include "VsyncCaptureMode.h"
#include "OutputWindow.h"
#include <SimpleLogger.h>

VsyncCaptureMode::VsyncCaptureMode() {}

VsyncCaptureMode::~VsyncCaptureMode() {}

UINT VsyncCaptureMode::GetPresentationInterval() const {
    return D3DPRESENT_INTERVAL_ONE;
}

MaybeFailure VsyncCaptureMode::Setup(const RelayContext& /*ctx*/) {
    LOG("VSync mode initialized - VSync will control frame timing");
    LOG("Output FPS will match target monitor's refresh rate");
    return std::nullopt;
}

MaybeFailure VsyncCaptureMode::Run(RelayContext& ctx, NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams) {
    do {
        // A no-wait grab: it returns the newest frame at once. Any other error (no new frame)
        // leaves the back buffer as it was, and presenting it again is the right answer.
        if (ctx.session->NvFBCToDx9VidGrabFrame(grabParams) == NVFBC_ERROR_INVALIDATED_SESSION) {
            LOGERR("NvFBC session invalidated - session needs to be recreated");
            return CaptureLost("vsync mode");
        }

        // Present and wait for VSync - this blocks until monitor refresh, because the device
        // was CREATED with INTERVAL_ONE. dwFlags is 0 and must stay 0: it is not an interval,
        // and D3DPRESENT_INTERVAL_ONE is numerically D3DPRESENT_DONOTWAIT, which asks the
        // runtime to skip the present rather than wait (see TemporalCaptureMode::Run).
        ctx.presentDevice->PresentEx(NULL, NULL, NULL, NULL, 0);
    } while (PumpMessages());
    return std::nullopt;
}

const char* VsyncCaptureMode::GetModeName() const {
    return "VSync";
}
