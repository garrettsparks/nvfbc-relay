#include "PresentScheduler.h"
#include <SimpleLogger.h>

PresentScheduler::PresentScheduler()
    : m_timer(NULL)
    , m_periodQpc(0)
{
    m_freq.QuadPart = 0;
    m_nextPresent.QuadPart = 0;
}

PresentScheduler::~PresentScheduler() {
    if (m_timer) {
        CloseHandle(m_timer);
        m_timer = NULL;
    }
}

bool PresentScheduler::Setup(float framerate) {
    m_timer = CreateWaitableTimerEx(NULL, NULL,
        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION | CREATE_WAITABLE_TIMER_MANUAL_RESET,
        TIMER_ALL_ACCESS);
    if (NULL == m_timer) {
        LOGERR("PresentScheduler: CreateWaitableTimerEx failed (error: %d)", GetLastError());
        return false;
    }

    QueryPerformanceFrequency(&m_freq);
    // Round to nearest tick to minimize accumulated rounding error over many frames.
    m_periodQpc = (LONGLONG)((double)m_freq.QuadPart / framerate + 0.5);
    return true;
}

void PresentScheduler::Seed() {
    QueryPerformanceCounter(&m_nextPresent);
    m_nextPresent.QuadPart += m_periodQpc;
}

void PresentScheduler::WaitUntilDeadline() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    // Catch-up clamp: if a transient stall left the schedule more than one full period behind
    // real time, re-anchor to now instead of presenting back-to-back to catch up.
    if (m_nextPresent.QuadPart < now.QuadPart - m_periodQpc) {
        m_nextPresent.QuadPart = now.QuadPart;
    }

    LONGLONG ticksUntilPresent = m_nextPresent.QuadPart - now.QuadPart;
    if (ticksUntilPresent > 0) {
        LARGE_INTEGER due;
        due.QuadPart = -(ticksUntilPresent * 10000000 / m_freq.QuadPart);  // 100ns units, relative
        SetWaitableTimer(m_timer, &due, 0, NULL, NULL, FALSE);
        WaitForSingleObject(m_timer, INFINITE);
    }
}

void PresentScheduler::Advance() {
    m_nextPresent.QuadPart += m_periodQpc;
}
