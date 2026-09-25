#include "TimerCaptureMode.h"
#include "OutputWindow.h"
#include <SimpleLogger.h>

TimerCaptureMode::TimerCaptureMode(float framerate)
    : m_framerate(framerate)
{
}

UINT TimerCaptureMode::GetPresentationInterval() const {
    return D3DPRESENT_INTERVAL_IMMEDIATE;
}

MaybeFailure TimerCaptureMode::Setup(const RelayContext& /*ctx*/) {
    if (!m_scheduler.Setup(m_framerate)) {
        return ModeCouldNotStart(GetModeName());
    }
    LOG("Timer mode initialized - target framerate: %.2f fps", m_framerate);
    return std::nullopt;
}

MaybeFailure TimerCaptureMode::Run(RelayContext& ctx, NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams) {
    m_scheduler.Seed();
    for (;;) {
        if (ctx.session->NvFBCToDx9VidGrabFrame(grabParams) == NVFBC_ERROR_INVALIDATED_SESSION) {
            LOGERR("NvFBC session invalidated - session needs to be recreated");
            return CaptureLost("timer mode");
        }

        // Present immediately (non-blocking), because the device was CREATED with
        // INTERVAL_IMMEDIATE. dwFlags is 0 and must stay 0: it is not an interval, and
        // D3DPRESENT_INTERVAL_IMMEDIATE (0x80000000) is not even a defined flag bit
        // (see TemporalCaptureMode::Run).
        ctx.presentDevice->PresentEx(NULL, NULL, NULL, NULL, 0);
        if (!PumpMessages()) return std::nullopt;

        // Wait out the rest of this frame on the absolute schedule, then advance.
        m_scheduler.WaitUntilDeadline();
        m_scheduler.Advance();
    }
}

const char* TimerCaptureMode::GetModeName() const {
    return "Timer";
}
