// EtwProbe - first-cut ETW consumer for docs/etw-frame-timing-spec.md (Phase-0 exploration).
//
// Opens a real-time ETW session on the DirectX kernel provider (Microsoft-Windows-DxgKrnl) and
// logs every event's QPC timestamp + event id + opcode. Goal: SEE the present/flip event stream
// and its delivery latency, and confirm the timestamps are QPC (same clock as the relay's arr=),
// so an offline nearest-QPC join to an NvFBC log is feasible. It does NOT yet decode the present
// token, display time, or real/gen frame type - those need TDH property decode against the (not
// blind-guessable) event schema; this proves the plumbing and shows which event ids to decode.
//
// UNTESTED (written on macOS). Needs on-hardware iteration: the interesting event ids under
// DxgKrnl (Present, Flip, HSyncDPCMultiPlane, etc.) must be identified from the live output, then
// decoded. If raw ETW proves too fragile, the spec's fallback is the PresentMon service API.
//
// Requires elevation (real-time ETW session). Usage: EtwProbe.exe [seconds]   (default 20).

#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cstdint>
#include <string>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "tdh.lib")

// Microsoft-Windows-DxgKrnl {802EC45A-1E99-4B83-9920-87C98277BA9D}: the DirectX graphics kernel
// provider PresentMon consumes for present/flip/display events.
static const GUID kDxgKrnlGuid =
    { 0x802ec45a, 0x1e99, 0x4b83, { 0x99, 0x20, 0x87, 0xc9, 0x82, 0x77, 0xba, 0x9d } };

static const wchar_t* kSessionName = L"EtwProbeSession";

static FILE*      g_log        = nullptr;
static LONGLONG   g_qpcFreq    = 1;
static long long  g_firstQpc   = 0;
static long long  g_events     = 0;
static TRACEHANDLE g_session   = 0;

static void LogLine(const char* fmt, ...) {
    char buf[256];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    fputs(buf, stdout); fputc('\n', stdout);
    if (g_log) { fputs(buf, g_log); fputc('\n', g_log); }
}

// One record per emitted event. TimeStamp is QPC because the session clock is set to QPC below,
// so it lines up directly with the relay's arr= (also QPC).
static void WINAPI OnEvent(PEVENT_RECORD ev) {
    if (!IsEqualGUID(ev->EventHeader.ProviderId, kDxgKrnlGuid)) return;
    const long long qpc = ev->EventHeader.TimeStamp.QuadPart;
    if (g_firstQpc == 0) g_firstQpc = qpc;
    const double tMs = (double)(qpc - g_firstQpc) * 1000.0 / (double)g_qpcFreq;
    g_events++;
    // Sample the stream (every event would flood): log a slice so the cadence/latency is visible.
    if (g_events <= 400) {
        LogLine("evt id=%u opcode=%u ver=%u  qpc=%lld  t=%.3fms",
                ev->EventHeader.EventDescriptor.Id,
                ev->EventHeader.EventDescriptor.Opcode,
                ev->EventHeader.EventDescriptor.Version,
                qpc, tMs);
    }
}

// Stops the session after the run window so ProcessTrace returns.
static DWORD WINAPI StopAfter(LPVOID arg) {
    const int seconds = (int)(intptr_t)arg;
    Sleep((DWORD)seconds * 1000);
    // ControlTrace needs a properties buffer sized for the session name.
    const size_t nameBytes = (wcslen(kSessionName) + 1) * sizeof(wchar_t);
    const size_t sz = sizeof(EVENT_TRACE_PROPERTIES) + nameBytes;
    EVENT_TRACE_PROPERTIES* p = (EVENT_TRACE_PROPERTIES*)calloc(1, sz);
    p->Wnode.BufferSize = (ULONG)sz;
    ControlTraceW(g_session, kSessionName, p, EVENT_TRACE_CONTROL_STOP);
    free(p);
    return 0;
}

int main(int argc, char** argv) {
    const int seconds = (argc > 1) ? atoi(argv[1]) : 20;
    LARGE_INTEGER f; QueryPerformanceFrequency(&f); g_qpcFreq = f.QuadPart;
    g_log = fopen("EtwProbe.log", "w");
    LogLine("=== EtwProbe (DxgKrnl present/flip event stream) - run %d s ===", seconds);

    // Real-time session properties. The session name is stored at LoggerNameOffset right after
    // the struct; ClientContext = 1 makes event TimeStamps QPC (matching the relay's arr=).
    const size_t nameBytes = (wcslen(kSessionName) + 1) * sizeof(wchar_t);
    const size_t sz = sizeof(EVENT_TRACE_PROPERTIES) + nameBytes;
    EVENT_TRACE_PROPERTIES* props = (EVENT_TRACE_PROPERTIES*)calloc(1, sz);
    props->Wnode.BufferSize    = (ULONG)sz;
    props->Wnode.Flags         = WNODE_FLAG_TRACED_GUID;
    props->Wnode.ClientContext = 1;                       // QPC clock
    props->LogFileMode         = EVENT_TRACE_REAL_TIME_MODE;
    props->LoggerNameOffset    = sizeof(EVENT_TRACE_PROPERTIES);

    // A stale session from a crashed run would block StartTrace; stop it first, ignore errors.
    ControlTraceW(0, kSessionName, props, EVENT_TRACE_CONTROL_STOP);
    props->Wnode.BufferSize    = (ULONG)sz;              // ControlTrace may have overwritten fields
    props->Wnode.Flags         = WNODE_FLAG_TRACED_GUID;
    props->Wnode.ClientContext = 1;
    props->LogFileMode         = EVENT_TRACE_REAL_TIME_MODE;
    props->LoggerNameOffset    = sizeof(EVENT_TRACE_PROPERTIES);

    ULONG st = StartTraceW(&g_session, kSessionName, props);
    if (st != ERROR_SUCCESS) {
        LogLine("FATAL: StartTrace failed (%lu) - need elevation? (real-time ETW requires admin)", st);
        return 1;
    }
    st = EnableTraceEx2(g_session, &kDxgKrnlGuid, EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                        TRACE_LEVEL_INFORMATION, 0, 0, 0, NULL);
    if (st != ERROR_SUCCESS) LogLine("WARN: EnableTraceEx2 failed (%lu)", st);

    EVENT_TRACE_LOGFILEW logfile = {};
    logfile.LoggerName          = (LPWSTR)kSessionName;
    logfile.ProcessTraceMode    = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
    logfile.EventRecordCallback = OnEvent;
    TRACEHANDLE consumer = OpenTraceW(&logfile);
    if (consumer == INVALID_PROCESSTRACE_HANDLE) {
        LogLine("FATAL: OpenTrace failed (%lu)", GetLastError());
        return 1;
    }

    CreateThread(NULL, 0, StopAfter, (LPVOID)(intptr_t)seconds, 0, NULL);
    ProcessTrace(&consumer, 1, NULL, NULL);   // blocks until the stop thread stops the session

    LogLine("=== done: %lld DxgKrnl events seen (logged first 400) ===", g_events);
    CloseTrace(consumer);
    free(props);
    if (g_log) fclose(g_log);
    return 0;
}
