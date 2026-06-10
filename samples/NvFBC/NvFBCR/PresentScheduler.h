#pragma once

#include <windows.h>

// Absolute-QPC present scheduler shared by capture modes that present on a fixed cadence.
//
// Schedules each present against an absolute QueryPerformanceCounter deadline on a fixed
// timeline (seed + N*period) using a high-resolution waitable timer, so per-frame OS wake
// latency cannot accumulate into drift (a relative timer re-armed each iteration drifts;
// this does not). Extracted verbatim from TimerCaptureMode's scheduling.
//
// Usage:
//   Setup(framerate) once; Seed() at the start of the loop; then per present:
//   WaitUntilDeadline(); <do work + PresentEx>; Advance();
class PresentScheduler {
private:
    HANDLE m_timer;
    LARGE_INTEGER m_freq;         // QPC frequency (ticks/sec)
    LONGLONG m_periodQpc;         // target frame interval in QPC ticks
    LARGE_INTEGER m_nextPresent;  // absolute QPC deadline for the next present

public:
    PresentScheduler();
    ~PresentScheduler();

    // Owns a timer HANDLE, so it's non-copyable (copying would double-close the handle).
    PresentScheduler(const PresentScheduler&) = delete;
    PresentScheduler& operator=(const PresentScheduler&) = delete;

    // Create the high-res timer and compute the period from the target framerate.
    bool Setup(float framerate);

    // Anchor the first deadline one period from now.
    void Seed();

    // Absolute QPC of the next scheduled present, and the timing constants.
    LONGLONG Deadline() const { return m_nextPresent.QuadPart; }
    LONGLONG PeriodQpc() const { return m_periodQpc; }
    LONGLONG Freq() const { return m_freq.QuadPart; }

    // Block until the current deadline. First re-anchors to now if a stall left the schedule
    // more than one period behind (catch-up clamp), so a transient hitch can't fan out into a
    // burst of catch-up presents.
    void WaitUntilDeadline();

    // Advance the deadline forward one period (fixed timeline; jitter does not accumulate).
    void Advance();
};
