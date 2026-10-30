#pragma once

#include <windows.h>

#include "PresentScheduler.h"

class IPresentPath;

// Microsecond durations counted in 1 us bins up to 40 ms, so a run's percentiles come out at
// exit without keeping every sample. Anything at or past 40 ms lands in one last bin, whose
// percentile reads as 40000; the maximum is kept exactly.
class UsHistogram {
public:
    void Add(long long us);
    long long Count() const { return m_count; }
    long long Max() const { return m_max; }
    double MeanUs() const { return m_count > 0 ? m_sum / (double)m_count : 0.0; }
    // The smallest bin holding at least fraction p of the samples.
    long long Percentile(double p) const;

private:
    static const int kBins = 40000;
    long long m_bins[kBins + 1] = {};
    long long m_count = 0;
    long long m_max = 0;
    double m_sum = 0.0;
};

// THE PACING: what a temporal mode's present waits on before the policy step decides it. Two
// kinds, one per mode string's second part (launch::ParseMode):
//   PathPacing   the present path's own wait: on the D3D11 present the swapchain's waitable object
//                (the output's vblank under independent flip, :vsync and the bare b and o); on the
//                D3D9 present its INTERVAL_ONE present, which blocks on DWM's compose clock (:dwm
//                and the bare t).
//   TimerPacing  PresentScheduler's QPC deadline (:<fps>), with the tight wait unless
//                -notightwait.
// After the timer, the D3D11 path's own wait runs, so a frame is never drawn into a buffer the
// display still holds; with the composition swapchain's third buffer it finds room within the
// period (Pandora, 2026-10-29), and the summary says how long it actually took. DWM's clock on
// the D3D11 present (DwmFlush, then that wait) was tried and dropped the same day: the two beat
// whenever DWM composes at the game's rate, not the card's.
//
// Every pacing logs at exit how evenly it woke: the spread of the times between wakes, and for
// the timer how late it woke. That is the measurement of the clocks on the rig, taken on every
// run rather than by a separate probe.
class IPacing {
public:
    virtual ~IPacing() {}

    // Once, just before the first Wait.
    virtual void Start() {}

    // Blocks until this present is due and the path has room for it. Returns the deadline the
    // policy step reads: the scheduled one for the timer, the moment the wait ended for the
    // others. *blockTicks is the time spent on a display's clock, blk= on the temporal line (the
    // timer's own wait is jit=, so it is not counted there).
    virtual LONGLONG Wait(IPresentPath* path, LONGLONG* blockTicks) = 0;

    // After this present.
    virtual void Advance() {}

    // The clock, for the mode name and the log.
    virtual const char* Name() const = 0;

    // The run's wake statistics.
    void LogSummary() const;

protected:
    // What a pacing adds to the summary after the shared lines.
    virtual void LogHeld() const {}

    // A wake of this pacing's own clock at now (QPC).
    void RecordWake(LONGLONG now);

    LONGLONG m_freq = 0;
    LONGLONG m_lastWake = 0;
    UsHistogram m_interval;     // between two wakes of the clock
    UsHistogram m_late;         // timer only: how far past its deadline it woke
    UsHistogram m_pathWait;     // the D3D11 path's own wait after another clock woke
};

class PathPacing : public IPacing {
public:
    // d3d11: the D3D11 present's waitable object, else the D3D9 present's blocking INTERVAL_ONE.
    explicit PathPacing(bool d3d11);
    LONGLONG Wait(IPresentPath* path, LONGLONG* blockTicks) override;
    const char* Name() const override {
        return m_d3d11 ? "the output's vblank" : "DWM's compose clock, through the blocking present";
    }

private:
    bool m_d3d11;
};

class TimerPacing : public IPacing {
public:
    // scheduler is borrowed from the mode, which sets its period up; tight is the tight wait.
    TimerPacing(PresentScheduler* scheduler, bool tight);
    void Start() override;
    LONGLONG Wait(IPresentPath* path, LONGLONG* blockTicks) override;
    void Advance() override;
    const char* Name() const override {
        return m_tight ? "a timer, waited out on the clock" : "a timer";
    }

protected:
    void LogHeld() const override;

private:
    PresentScheduler* m_scheduler;
    bool m_tight;
    long long m_held = 0;       // presents held for room past half a period
};
