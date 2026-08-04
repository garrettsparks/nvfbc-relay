#include "EtwConsumer.h"
#include "FlipDecode.h"

#include <SimpleLogger.h>
#include <evntcons.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "advapi32.lib")

// Session name is fixed: a stale session from a crashed run must be findable and stoppable
// rather than silently coexisting with a new one under a random name.
static const wchar_t* kSessionName = L"NvFBCRelayFlipSession";

// Measured settings, not chosen ones. See the spec's session configuration section; the
// short version is that defaults deliver on a ~1 s cadence and these bring it to ~8 ms p50.
static const ULONG kBufferSizeKb = 64;     // PresentMon's sizing for this provider
static const ULONG kMinBuffers   = 256;
static const ULONG kMaxBuffers   = 1024;
static const DWORD kFlushMs      = 10;     // quantised up to ~16 ms by the system timer

// One consumer per process, so the ETW callback (which is a plain function pointer with no
// user context on this path) can find it.
static EtwFlipConsumer* g_instance = NULL;

struct SessionProps {
    EVENT_TRACE_PROPERTIES props;
    wchar_t name[128];
};

static void FillProps(SessionProps* sp) {
    memset(sp, 0, sizeof(*sp));
    sp->props.Wnode.BufferSize    = sizeof(SessionProps);
    sp->props.Wnode.Flags         = WNODE_FLAG_TRACED_GUID;
    sp->props.Wnode.ClientContext = 1;   // QPC, so flip times share the relay's clock
    sp->props.LogFileMode         = EVENT_TRACE_REAL_TIME_MODE |
                                    EVENT_TRACE_NO_PER_PROCESSOR_BUFFERING;
    sp->props.BufferSize          = kBufferSizeKb;
    sp->props.MinimumBuffers      = kMinBuffers;
    sp->props.MaximumBuffers      = kMaxBuffers;
    sp->props.FlushTimer          = 1;
    sp->props.LoggerNameOffset    = offsetof(SessionProps, name);
}

EtwFlipConsumer::EtwFlipConsumer() {}

EtwFlipConsumer::~EtwFlipConsumer() { Stop(); }

bool EtwFlipConsumer::Start(LONGLONG qpcFreq, LONGLONG baseQpc) {
    if (g_instance) {
        LOGERR("ETW: a flip consumer is already running");
        return false;
    }
    m_qpcFreq = qpcFreq;
    m_baseQpc = baseQpc;

    SessionProps sp;
    FillProps(&sp);
    // A session surviving a previous crash would make StartTrace fail with ALREADY_EXISTS,
    // so clear it first rather than requiring a reboot to recover.
    ControlTraceW(0, kSessionName, &sp.props, EVENT_TRACE_CONTROL_STOP);

    FillProps(&sp);
    ULONG st = StartTraceW(&m_session, kSessionName, &sp.props);
    if (st != ERROR_SUCCESS) {
        LOGERR("ETW: StartTrace failed (%lu) - flip timing unavailable, capture continues",
               (unsigned long)st);
        return false;
    }

    st = EnableTraceEx2(m_session, &flipdecode::kNvDisplayDriverGuid,
                        EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                        flipdecode::kFlipRequestLevel,
                        flipdecode::kFlipRequestKeyword, 0, 0, NULL);
    if (st != ERROR_SUCCESS) {
        LOGERR("ETW: EnableTrace failed (%lu) - flip timing unavailable, capture continues",
               (unsigned long)st);
        ControlTraceW(m_session, kSessionName, &sp.props, EVENT_TRACE_CONTROL_STOP);
        m_session = 0;
        return false;
    }

    EVENT_TRACE_LOGFILEW lf;
    memset(&lf, 0, sizeof(lf));
    lf.LoggerName = (LPWSTR)kSessionName;
    // RAW_TIMESTAMP is what makes EventHeader.TimeStamp arrive in the QPC units requested
    // above; without it ETW converts to system time and nothing can be compared to arr=.
    lf.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD |
                          PROCESS_TRACE_MODE_RAW_TIMESTAMP;
    lf.EventRecordCallback = OnEventThunk;
    m_consumer = OpenTraceW(&lf);
    if (m_consumer == (TRACEHANDLE)INVALID_HANDLE_VALUE) {
        LOGERR("ETW: OpenTrace failed (%lu) - flip timing unavailable, capture continues",
               (unsigned long)GetLastError());
        ControlTraceW(m_session, kSessionName, &sp.props, EVENT_TRACE_CONTROL_STOP);
        m_session = 0;
        return false;
    }

    g_instance = this;
    m_stop.store(false);
    m_consumeThread = CreateThread(NULL, 0, ConsumeThunk, this, 0, NULL);
    m_flushThread   = CreateThread(NULL, 0, FlushThunk, this, 0, NULL);
    LOG("ETW flip consumer ACTIVE: NVIDIA DisplayDriver FlipRequest, %lu KB x %lu-%lu buffers, "
        "consolidated (not per-processor), forced flush every %lu ms",
        (unsigned long)kBufferSizeKb, (unsigned long)kMinBuffers, (unsigned long)kMaxBuffers,
        (unsigned long)kFlushMs);
    LOG("ETW: flip lines are 'flip disp=<us> evt=<us> head=<n> token=<n>', microseconds since "
        "the same QPC origin as arr=/dl=, so no cross-log alignment is needed");
    return true;
}

void EtwFlipConsumer::Stop() {
    if (!m_session && !m_consumeThread) return;
    m_stop.store(true);

    SessionProps sp;
    FillProps(&sp);
    if (m_session) ControlTraceW(m_session, kSessionName, &sp.props, EVENT_TRACE_CONTROL_STOP);

    // ProcessTrace returns once the session stops, so the consumer thread unblocks here.
    if (m_consumeThread) {
        WaitForSingleObject(m_consumeThread, 3000);
        CloseHandle(m_consumeThread);
        m_consumeThread = NULL;
    }
    if (m_flushThread) {
        WaitForSingleObject(m_flushThread, 3000);
        CloseHandle(m_flushThread);
        m_flushThread = NULL;
    }
    if (m_consumer != (TRACEHANDLE)INVALID_HANDLE_VALUE) {
        CloseTrace(m_consumer);
        m_consumer = (TRACEHANDLE)INVALID_HANDLE_VALUE;
    }
    m_session = 0;
    if (g_instance == this) g_instance = NULL;
}

DWORD WINAPI EtwFlipConsumer::ConsumeThunk(LPVOID self) {
    EtwFlipConsumer* c = (EtwFlipConsumer*)self;
    ProcessTrace(&c->m_consumer, 1, NULL, NULL);   // blocks until the session stops
    return 0;
}

DWORD WINAPI EtwFlipConsumer::FlushThunk(LPVOID self) {
    ((EtwFlipConsumer*)self)->FlushLoop();
    return 0;
}

void EtwFlipConsumer::FlushLoop() {
    SessionProps sp;
    while (!m_stop.load(std::memory_order_relaxed)) {
        Sleep(kFlushMs);
        if (m_stop.load(std::memory_order_relaxed)) break;
        FillProps(&sp);
        ControlTraceW(m_session, kSessionName, &sp.props, EVENT_TRACE_CONTROL_FLUSH);
    }
}

void WINAPI EtwFlipConsumer::OnEventThunk(PEVENT_RECORD ev) {
    if (g_instance) g_instance->OnEvent(ev);
}

void EtwFlipConsumer::OnEvent(PEVENT_RECORD ev) {
    m_events.fetch_add(1, std::memory_order_relaxed);
    if (!flipdecode::IsFlipRequest(ev)) return;

    // Read the clock only once past the provider filter: this callback also sees every
    // non-flip event from the provider, and a QPC read on each of those is pure cost.
    LARGE_INTEGER nowQpc;
    QueryPerformanceCounter(&nowQpc);

    flipdecode::FlipEvent fe;
    if (!flipdecode::DecodeFlip(ev, m_qpcFreq, &fe)) {
        m_decodeFail.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    m_flips.fetch_add(1, std::memory_order_relaxed);

    policy::Flip f;
    f.displayTs = (int64_t)fe.displayQpc;
    f.eventTs   = fe.eventQpc;
    f.head      = fe.head;
    f.token     = fe.token;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_history.Add(f);
    }

    // Same origin and units as arr= and dl=, so the flip stream and the capture stream are
    // directly comparable inside one log without any alignment step.
    //
    // lag is how long after the driver announced the flip this process learned about it.
    // It decides whether the data could ever inform a decision rather than only describe
    // one afterwards, and it has only ever been measured on an idle desktop; under a game
    // plus a running relay it may be a different number entirely.
    const double usPerTick = 1000000.0 / (double)m_qpcFreq;
    LOG("flip disp=%lldus evt=%lldus lag=%lldus head=%u token=%u",
        (long long)(((int64_t)fe.displayQpc - m_baseQpc) * usPerTick),
        (long long)((fe.eventQpc - m_baseQpc) * usPerTick),
        (long long)((nowQpc.QuadPart - fe.eventQpc) * usPerTick),
        fe.head, fe.token);
}

void EtwFlipConsumer::CopyHistory(policy::FlipHistory* out) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    *out = m_history;
}

policy::FlipPairing EtwFlipConsumer::PairCapture(uint32_t head, int64_t batchStartTs,
                                                 int member, int64_t maxAnchorOffset) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return policy::PairBatchMember(m_history, head, batchStartTs, member, maxAnchorOffset);
}

void EtwFlipConsumer::LogSummary() {
    SessionProps sp;
    FillProps(&sp);
    ULONG eventsLost = 0, rtLost = 0, logLost = 0, written = 0;
    if (m_session &&
        ControlTraceW(m_session, kSessionName, &sp.props, EVENT_TRACE_CONTROL_QUERY) ==
            ERROR_SUCCESS) {
        eventsLost = sp.props.EventsLost;
        rtLost     = sp.props.RealTimeBuffersLost;
        logLost    = sp.props.LogBuffersLost;
        written    = sp.props.BuffersWritten;
    }
    long long dropped = 0, outOfOrder = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        dropped = m_history.Dropped();
        outOfOrder = m_history.OutOfOrder();
    }
    LOG("ETW summary: events %lld, flips %lld, decode failures %lld",
        m_events.load(), m_flips.load(), m_decodeFail.load());
    LOG("ETW losses: session events %lu, realtime buffers %lu, log buffers %lu, written %lu; "
        "history evicted %lld, out-of-order %lld%s",
        (unsigned long)eventsLost, (unsigned long)rtLost, (unsigned long)logLost,
        (unsigned long)written, dropped, outOfOrder,
        (eventsLost || rtLost || logLost)
            ? "   <== LOSS: this capture's flip stream is incomplete"
            : "");
}
