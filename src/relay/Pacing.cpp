#include "Pacing.h"
#include "IPresentPath.h"

#include <SimpleLogger.h>

namespace {

LONGLONG Now() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}

LONGLONG Frequency() {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return f.QuadPart;
}

}  // namespace

void UsHistogram::Add(long long us) {
    if (us < 0) us = 0;
    m_bins[us < kBins ? us : kBins]++;
    m_count++;
    if (us > m_max) m_max = us;
    m_sum += (double)us;
}

long long UsHistogram::Percentile(double p) const {
    if (m_count == 0) return 0;
    const double want = p * (double)m_count;
    long long seen = 0;
    for (int i = 0; i <= kBins; i++) {
        seen += m_bins[i];
        if ((double)seen >= want) return i;
    }
    return kBins;
}

void IPacing::RecordWake(LONGLONG now) {
    if (m_lastWake != 0) m_interval.Add((now - m_lastWake) * 1000000 / m_freq);
    m_lastWake = now;
}

void IPacing::LogSummary() const {
    const double mean = m_interval.MeanUs();
    LOG("pacing summary (%s): %lld waits; between wakes p50 %lld us, p90 %lld, p99 %lld, "
        "max %lld, mean %.1f us (%.4f Hz)",
        Name(), m_lastWake != 0 ? m_interval.Count() + 1 : 0, m_interval.Percentile(0.5),
        m_interval.Percentile(0.9),
        m_interval.Percentile(0.99), m_interval.Max(), mean, mean > 0.0 ? 1000000.0 / mean : 0.0);
    if (m_late.Count() > 0) {
        LOG("pacing summary: the timer woke late by p50 %lld us, p90 %lld, p99 %lld, max %lld",
            m_late.Percentile(0.5), m_late.Percentile(0.9), m_late.Percentile(0.99),
            m_late.Max());
    }
    if (m_pathWait.Count() > 0) {
        LOG("pacing summary: after the clock, the present's own wait for room took p50 %lld us, "
            "p90 %lld, p99 %lld, max %lld",
            m_pathWait.Percentile(0.5), m_pathWait.Percentile(0.9),
            m_pathWait.Percentile(0.99), m_pathWait.Max());
    }
    LogHeld();
}

PathPacing::PathPacing(bool d3d11) : m_d3d11(d3d11) {
    m_freq = Frequency();
}

LONGLONG PathPacing::Wait(IPresentPath* path, LONGLONG* blockTicks) {
    // The D3D11 path blocks here on its waitable object; the D3D9 path returns at once and its
    // present blocks instead, so the wake recorded is the end of that present's block.
    const LONGLONG start = Now();
    path->WaitForFrame();
    const LONGLONG now = Now();
    *blockTicks = now - start;
    RecordWake(now);
    return now;
}

TimerPacing::TimerPacing(PresentScheduler* scheduler, bool tight)
    : m_scheduler(scheduler), m_tight(tight) {
    m_freq = Frequency();
}

void TimerPacing::Start() {
    m_scheduler->SetTightWait(m_tight);
    m_scheduler->Seed();
}

LONGLONG TimerPacing::Wait(IPresentPath* path, LONGLONG* blockTicks) {
    m_scheduler->WaitUntilDeadline();
    const LONGLONG woke = Now();
    const LONGLONG deadline = m_scheduler->Deadline();
    m_late.Add((woke - deadline) * 1000000 / m_freq);
    RecordWake(woke);
    path->WaitForFrame();
    const LONGLONG now = Now();
    m_pathWait.Add((now - woke) * 1000000 / m_freq);
    *blockTicks = now - woke;
    // Held for room past half a period, the present is no longer at the timer's deadline: the
    // step reads the time it really decides at, so a hold never ages the shown frame unseen.
    if (now - woke > m_scheduler->PeriodQpc() / 2) {
        m_held++;
        return now;
    }
    return deadline;
}

void TimerPacing::LogHeld() const {
    LOG("pacing summary: %lld presents were held for room past half a period, and decided at "
        "the time they were let go rather than at the timer's deadline", m_held);
}

void TimerPacing::Advance() {
    m_scheduler->Advance();
}
