#include "TimerCaptureMode.h"
#include "OutputWindow.h"
#include <SimpleLogger.h>
#include <limits.h>

// How often the loop logs the presents it made, so a log shows the rate the timer really ran at.
static const LONGLONG kRateReportSeconds = 10;

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
    LOG("Timer mode: a 'timer:' line every %lld s gives the presents made and their spacing",
        (long long)kRateReportSeconds);
    return std::nullopt;
}

MaybeFailure TimerCaptureMode::Run(RelayContext& ctx, NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams) {
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    LARGE_INTEGER windowStart;
    QueryPerformanceCounter(&windowStart);
    LONGLONG lastPresent = 0;
    LONGLONG minGap = LLONG_MAX;
    LONGLONG maxGap = 0;
    long long presents = 0;

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

        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        if (lastPresent) {
            const LONGLONG gap = now.QuadPart - lastPresent;
            if (gap < minGap) minGap = gap;
            if (gap > maxGap) maxGap = gap;
        }
        lastPresent = now.QuadPart;
        presents++;
        const LONGLONG elapsed = now.QuadPart - windowStart.QuadPart;
        if (elapsed >= kRateReportSeconds * freq.QuadPart) {
            const double seconds = (double)elapsed / (double)freq.QuadPart;
            LOG("timer: %lld presents in %.2f s (%.2f a second), %.2f to %.2f ms apart",
                presents, seconds, (double)presents / seconds,
                minGap == LLONG_MAX ? 0.0 : (double)minGap * 1000.0 / (double)freq.QuadPart,
                (double)maxGap * 1000.0 / (double)freq.QuadPart);
            windowStart = now;
            presents = 0;
            minGap = LLONG_MAX;
            maxGap = 0;
        }

        if (!PumpMessages()) return std::nullopt;

        // Wait out the rest of this frame on the absolute schedule, then advance.
        m_scheduler.WaitUntilDeadline();
        m_scheduler.Advance();
    }
}

const char* TimerCaptureMode::GetModeName() const {
    return "Timer";
}
