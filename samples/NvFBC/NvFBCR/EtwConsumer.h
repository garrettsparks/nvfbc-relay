#pragma once

#include "TemporalPolicy.h"

#include <windows.h>
#include <evntrace.h>
#include <atomic>
#include <mutex>

// Reads the display driver's FlipRequest events while the relay runs, so a capture carries
// the true scanout times alongside its own arrival and present stamps in ONE log on ONE
// clock. Diagnostic only: it writes log lines and fills a history, and nothing in the
// policy reads any of it.
//
// It exists before anything consumes it on purpose. The relationship between a capture wake
// and the flip it belongs to is currently an inference drawn from three matched events; the
// only way to promote that to a rule is to collect thousands of them under real load, and
// the only comfortable way to collect them is to have the relay do it rather than to
// choreograph a separate probe against a running game.
//
// SESSION CONFIGURATION IS LOAD-BEARING. Left at defaults, real-time ETW delivers on a ~1 s
// cadence, which is 70x past the relay's bracketing lag and would make the data useless. The
// settings in the .cpp were measured, not chosen; see the spec's session configuration
// section before changing any of them.
class EtwFlipConsumer {
public:
    EtwFlipConsumer();
    ~EtwFlipConsumer();

    // Starts the session and the consumer and flush threads. Returns false and logs the
    // reason if the session cannot start; the relay carries on regardless, because this is
    // instrumentation and must never be able to take capture down with it.
    // baseQpc is the relay's own log origin, not a fresh reading: flip lines must be in the
    // same units and origin as arr= and dl= or the single-log advantage evaporates.
    bool Start(LONGLONG qpcFreq, LONGLONG baseQpc);
    void Stop();

    // Snapshot under the lock. The history is written from the ETW thread, so a reader on
    // the present thread must go through here rather than touching it directly.
    void CopyHistory(policy::FlipHistory* out) const;

    // Place one captured frame on the flip grid, holding the lock only for the lookup.
    //
    // Preferred over CopyHistory for the present thread: the history is ~48 KB, so a
    // snapshot is a memcpy of the same order as the entire measured present jitter (p50
    // 3 us), while the lookup itself is a few bounded scans. It also needs no 48 KB of
    // somewhere to live on a thread that runs every 16.67 ms.
    //
    // Callers must gate on whether ETW was requested at all rather than relying on this
    // returning an empty verdict: with -etw off the session never starts, and not taking
    // the lock keeps that configuration on exactly the code path it had before any of this
    // existed, which is what makes an on/off comparison meaningful.
    policy::FlipPairing PairCapture(uint32_t head, int64_t batchStartTs, int member,
                                    int64_t maxAnchorOffset) const;

    long long Flips() const { return m_flips.load(std::memory_order_relaxed); }
    long long DecodeFailures() const { return m_decodeFail.load(std::memory_order_relaxed); }

    // Logs event counts, decode failures, and the session's own loss counters. Losses are
    // logged even when zero: an estimator fed silently incomplete data is the failure this
    // project keeps rediscovering, so the number should be present in every capture rather
    // than only when someone thinks to look.
    void LogSummary();

private:
    static void WINAPI OnEventThunk(PEVENT_RECORD ev);
    static DWORD WINAPI ConsumeThunk(LPVOID self);
    static DWORD WINAPI FlushThunk(LPVOID self);
    void OnEvent(PEVENT_RECORD ev);
    void FlushLoop();

    TRACEHANDLE m_session = 0;
    TRACEHANDLE m_consumer = (TRACEHANDLE)INVALID_HANDLE_VALUE;
    HANDLE m_consumeThread = NULL;
    HANDLE m_flushThread = NULL;
    std::atomic<bool> m_stop{false};
    std::atomic<long long> m_flips{0};
    std::atomic<long long> m_events{0};
    std::atomic<long long> m_decodeFail{0};
    LONGLONG m_qpcFreq = 0;
    LONGLONG m_baseQpc = 0;

    mutable std::mutex m_mutex;
    policy::FlipHistory m_history;
};
