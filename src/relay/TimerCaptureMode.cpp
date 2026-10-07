#include "TimerCaptureMode.h"
#include "OutputWindow.h"
#include <SimpleLogger.h>
#include <limits.h>

// How often the loop logs the presents it made, so a log shows the rate the timer really ran at.
static const LONGLONG kRateReportSeconds = 10;

TimerCaptureMode::TimerCaptureMode(float framerate, bool ogTimer, bool ogFlags)
    : m_framerate(framerate)
    , m_ogTimer(ogTimer)
    , m_ogFlags(ogFlags)
{
}

TimerCaptureMode::~TimerCaptureMode() {
    if (m_ogTimerHandle) {
        CloseHandle(m_ogTimerHandle);
        m_ogTimerHandle = NULL;
    }
}

UINT TimerCaptureMode::GetPresentationInterval() const {
    return D3DPRESENT_INTERVAL_IMMEDIATE;
}

MaybeFailure TimerCaptureMode::Setup(const RelayContext& /*ctx*/) {
    if (m_ogTimer) {
        // The original relay's timer: manual reset, default resolution.
        m_ogTimerHandle = CreateWaitableTimer(NULL, TRUE, NULL);
        if (!m_ogTimerHandle) {
            LOGERR("Timer mode: CreateWaitableTimer failed (error: %lu)", GetLastError());
            return ModeCouldNotStart(GetModeName());
        }
    } else if (!m_scheduler.Setup(m_framerate)) {
        return ModeCouldNotStart(GetModeName());
    }
    LOG("Timer mode initialized - target framerate: %.2f fps", m_framerate);
    if (m_ogTimer) {
        LOG("Timer mode: original relay's timer (-ogtimer): default-resolution waitable timer, "
            "re-armed %lld x 100 ns ahead before each grab",
            (long long)(10000000.0 / m_framerate));
    }
    if (m_ogFlags) {
        LOG("Timer mode: original relay's PresentEx flags 0x80000000 (-ogflags)");
    }
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

    // D3DPRESENT_INTERVAL_IMMEDIATE (0x80000000) is an interval, not a PresentEx flag bit; the
    // original relay passed it as the flags, and -ogflags passes it again for comparison.
    const DWORD presentFlags = m_ogFlags ? 0x80000000 : 0;
    const LONGLONG ogDue = -(LONGLONG)(10000000.0 / m_framerate);

    if (!m_ogTimer) m_scheduler.Seed();
    for (;;) {
        if (m_ogTimer) {
            LARGE_INTEGER due;
            due.QuadPart = ogDue;
            SetWaitableTimer(m_ogTimerHandle, &due, 0, NULL, NULL, FALSE);
        }
        if (ctx.session->NvFBCToDx9VidGrabFrame(grabParams) == NVFBC_ERROR_INVALIDATED_SESSION) {
            LOGERR("NvFBC session invalidated - session needs to be recreated");
            return CaptureLost("timer mode");
        }

        // Present immediately (non-blocking), because the device was CREATED with
        // INTERVAL_IMMEDIATE.
        ctx.presentDevice->PresentEx(NULL, NULL, NULL, NULL, presentFlags);

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

        if (m_ogTimer) {
            WaitForSingleObject(m_ogTimerHandle, INFINITE);
        } else {
            // Wait out the rest of this frame on the absolute schedule, then advance.
            m_scheduler.WaitUntilDeadline();
            m_scheduler.Advance();
        }
    }
}

const char* TimerCaptureMode::GetModeName() const {
    return "Timer";
}
