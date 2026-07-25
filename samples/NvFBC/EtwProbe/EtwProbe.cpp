// EtwProbe - raw-ETW consumer for the NVIDIA flip-metering event (docs/etw-frame-timing-spec.md).
//
// WHAT THIS MEASURES, AND WHY IT IS ONE EVENT
//
// The capture APIs cannot say when a frame actually reached the screen: NvFBC gives grab-wake
// time, DXGI gives submission time, and under Smooth Motion the driver submits the real and
// generated frames together while scheduling their SCANOUTS half a source period apart. The
// driver does publish the schedule, through a single ETW event on its own provider:
//
//     NVIDIA DisplayDriver {AE4F8626-8265-40D1-A70B-11B64240E8E9}
//     FlipRequest (Id 1, level 0x04, keyword 0x1000000000000000)
//     fields: alloc (u64), vidPnSourceId (u32), ts (u64), token (u32)
//
// `ts` is the PROPOSED FLIP TIME in QPC ticks. Consecutive ts values on one head are the true
// scanout cadence, which is the number the relay has never had. PresentMon consumes this same
// event but COLLAPSES it: it dedupes by token, keeps only the latest flip time per head, and
// folds the result into one MsFlipDelay column on the application's present row - so the
// individual generated-frame flip times never reach its CSV. Reading the provider directly is
// how we see them, and it is a small surface: one provider, one event, four fields.
//
// THE MEASUREMENT (printed as a summary at exit)
//   - dts: gap between consecutive proposed flip times on a head. Under 2x Smooth Motion at a
//     60 fps source this should sit at ~8.33 ms. Whether it is EVEN is the open question the
//     grid-stamping design depends on; do not assume it, read the histogram.
//   - ahead: ts minus the event's own timestamp, i.e. how far in the future the driver
//     schedules a flip. Bounds how much warning a live consumer could get.
//   - lag: our callback time minus the event timestamp, i.e. ETW delivery latency. This is the
//     number that decides whether a live relay could ever use this in-band.
//
// DECODING CAVEAT: TdhGetProperty resolves fields by name only if the provider's manifest is
// registered on the machine. PresentMon embeds and loads NVIDIA's manifest itself, which
// suggests it may not be. So this probe tries TDH first and ALWAYS hexdumps the first few
// payloads; if TDH fails, the hexdump is how we recover the layout from real hardware rather
// than guessing at it. Field order in the struct above is READ order from PresentMon's
// consumer, which is not necessarily wire order.
//
// Requires elevation (real-time ETW). Usage: EtwProbe.exe [seconds] [--dxgk]
//   --dxgk also enables Microsoft-Windows-DxgKrnl and counts its events. Purely a control: if
//   the NVIDIA event count is zero but DxgKrnl is nonzero, the session works and the NVIDIA
//   provider is the problem (wrong GUID, Smooth Motion off, or nothing being flipped).

#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>
#include <tdh.h>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cstdint>
#include <cstring>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "tdh.lib")

// NVIDIA DisplayDriver. GUID and event descriptor taken from the manifest PresentMon embeds
// (PresentData/ETW/NV_DD.h); verified against its NVTraceConsumer, not guessed.
static const GUID kNvDisplayDriverGuid =
    { 0xae4f8626, 0x8265, 0x40d1, { 0xa7, 0x0b, 0x11, 0xb6, 0x42, 0x40, 0xe8, 0xe9 } };
static const USHORT   kFlipRequestId      = 1;
static const UCHAR    kFlipRequestLevel   = 4;
static const ULONGLONG kFlipRequestKeyword = 0x1000000000000000ull;

// Control provider: the DirectX graphics kernel. Only counted, never decoded.
static const GUID kDxgKrnlGuid =
    { 0x802ec45a, 0x1e99, 0x4b83, { 0x99, 0x20, 0x87, 0xc9, 0x82, 0x77, 0xba, 0x9d } };

static const wchar_t* kSessionName = L"EtwProbeSession";
static const int  kMaxHeads      = 8;    // vidPnSourceId values tracked
static const int  kHexDumpEvents = 8;    // payloads dumped verbatim for layout recovery
static const int  kLogEvents     = 400;  // per-event lines before going quiet
static const int  kDtsBuckets    = 80;   // 0.5 ms each, so 0..40 ms

static FILE*       g_log      = nullptr;
static LONGLONG    g_qpcFreq  = 1;
static TRACEHANDLE g_session  = 0;

static long long g_nvEvents    = 0;
static long long g_flipEvents  = 0;
static long long g_dxgkEvents  = 0;
static long long g_decodeOk    = 0;
static long long g_decodeFail  = 0;
static int       g_dumped      = 0;

static long long g_lastTsByHead[kMaxHeads] = {};
static long long g_dtsHist[kDtsBuckets] = {};
static long long g_dtsCount = 0;
static long long g_dtsSum = 0;

// Reservoir-free percentile inputs: these streams are small enough to keep whole.
static const int kMaxSamples = 20000;
static long long g_lagUs[kMaxSamples];
static long long g_aheadUs[kMaxSamples];
static int g_lagN = 0, g_aheadN = 0;

static void LogLine(const char* fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    fputs(buf, stdout); fputc('\n', stdout);
    if (g_log) { fputs(buf, g_log); fputc('\n', g_log); fflush(g_log); }
}

// TdhGetProperty by field name. Returns false when the manifest is not registered, which is
// the case this probe is built to survive (see the decoding caveat above).
static bool GetProp(EVENT_RECORD* ev, const wchar_t* name, void* out, ULONG size) {
    PROPERTY_DATA_DESCRIPTOR desc = {};
    desc.PropertyName = (ULONGLONG)(uintptr_t)name;
    desc.ArrayIndex = ULONG_MAX;
    return TdhGetProperty(ev, 0, nullptr, 1, &desc, size, (PBYTE)out) == ERROR_SUCCESS;
}

static void HexDump(const BYTE* p, ULONG n) {
    char line[128];
    for (ULONG off = 0; off < n; off += 16) {
        size_t w = (size_t)snprintf(line, sizeof(line), "    +%02lu ", (unsigned long)off);
        for (ULONG i = 0; i < 16 && off + i < n && w < sizeof(line) - 4; i++) {
            w += (size_t)snprintf(line + w, sizeof(line) - w, "%02X ", p[off + i]);
        }
        LogLine("%s", line);
    }
}

static int Cmp(const void* a, const void* b) {
    const long long x = *(const long long*)a, y = *(const long long*)b;
    return (x > y) - (x < y);
}

static long long Pct(long long* v, int n, int p) {
    if (n <= 0) return 0;
    int i = (int)((long long)n * p / 100);
    if (i >= n) i = n - 1;
    return v[i];
}

static void WINAPI OnEvent(PEVENT_RECORD ev) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    if (IsEqualGUID(ev->EventHeader.ProviderId, kDxgKrnlGuid)) {
        g_dxgkEvents++;
        return;
    }
    if (!IsEqualGUID(ev->EventHeader.ProviderId, kNvDisplayDriverGuid)) return;
    g_nvEvents++;
    if (ev->EventHeader.EventDescriptor.Id != kFlipRequestId) return;
    g_flipEvents++;

    const long long evtQpc = ev->EventHeader.TimeStamp.QuadPart;
    const long long lagUs  = (now.QuadPart - evtQpc) * 1000000 / g_qpcFreq;

    // Verbatim payload for the first few events: this is what lets us recover the wire layout
    // on real hardware if TDH cannot resolve the field names.
    if (g_dumped < kHexDumpEvents) {
        g_dumped++;
        LogLine("flip payload #%d: %lu bytes (ver=%u opcode=%u)",
                g_dumped, (unsigned long)ev->UserDataLength,
                ev->EventHeader.EventDescriptor.Version,
                ev->EventHeader.EventDescriptor.Opcode);
        HexDump((const BYTE*)ev->UserData, ev->UserDataLength);
    }

    uint64_t alloc = 0, ts = 0;
    uint32_t head = 0, token = 0;
    const bool ok = GetProp(ev, L"ts", &ts, sizeof(ts)) &&
                    GetProp(ev, L"vidPnSourceId", &head, sizeof(head)) &&
                    GetProp(ev, L"token", &token, sizeof(token)) &&
                    GetProp(ev, L"alloc", &alloc, sizeof(alloc));
    if (!ok) {
        g_decodeFail++;
        if (g_decodeFail <= 3) {
            LogLine("  TDH decode failed (manifest likely not registered) - use the hexdumps above");
        }
        return;
    }
    g_decodeOk++;

    // ts is a QPC tick value: the flip the driver intends to scan out. Gaps between
    // consecutive values on one head ARE the scanout cadence.
    long long dtsUs = -1;
    if (head < kMaxHeads) {
        if (g_lastTsByHead[head] != 0 && (long long)ts > g_lastTsByHead[head]) {
            dtsUs = ((long long)ts - g_lastTsByHead[head]) * 1000000 / g_qpcFreq;
            const int b = (int)(dtsUs / 500);
            if (b >= 0 && b < kDtsBuckets) { g_dtsHist[b]++; g_dtsCount++; g_dtsSum += dtsUs; }
        }
        if ((long long)ts != 0) g_lastTsByHead[head] = (long long)ts;
    }

    const long long aheadUs = ((long long)ts - evtQpc) * 1000000 / g_qpcFreq;
    if (g_lagN < kMaxSamples) g_lagUs[g_lagN++] = lagUs;
    if (g_aheadN < kMaxSamples && ts != 0) g_aheadUs[g_aheadN++] = aheadUs;

    if (g_flipEvents <= kLogEvents) {
        LogLine("flip #%lld evt=%lld recv_lag=%lldus ts=%llu ahead=%lldus dts=%lldus "
                "head=%u token=%u alloc=0x%llX",
                g_flipEvents, evtQpc, lagUs, (unsigned long long)ts, aheadUs, dtsUs,
                head, token, (unsigned long long)alloc);
    }
}

static DWORD WINAPI StopAfter(LPVOID arg) {
    const int seconds = (int)(intptr_t)arg;
    Sleep((DWORD)seconds * 1000);
    const size_t nameBytes = (wcslen(kSessionName) + 1) * sizeof(wchar_t);
    const size_t sz = sizeof(EVENT_TRACE_PROPERTIES) + nameBytes;
    EVENT_TRACE_PROPERTIES* p = (EVENT_TRACE_PROPERTIES*)calloc(1, sz);
    if (!p) return 0;
    p->Wnode.BufferSize = (ULONG)sz;
    ControlTraceW(g_session, kSessionName, p, EVENT_TRACE_CONTROL_STOP);
    free(p);
    return 0;
}

static void FillProps(EVENT_TRACE_PROPERTIES* props, size_t sz) {
    props->Wnode.BufferSize    = (ULONG)sz;
    props->Wnode.Flags         = WNODE_FLAG_TRACED_GUID;
    props->Wnode.ClientContext = 1;                     // QPC clock, matching the relay's arr=
    props->LogFileMode         = EVENT_TRACE_REAL_TIME_MODE;
    props->LoggerNameOffset    = sizeof(EVENT_TRACE_PROPERTIES);
}

static void Summary() {
    LogLine("");
    LogLine("=== summary ===");
    LogLine("NVIDIA DisplayDriver events: %lld (FlipRequest %lld, decoded %lld, decode-failed %lld)",
            g_nvEvents, g_flipEvents, g_decodeOk, g_decodeFail);
    if (g_dxgkEvents) LogLine("DxgKrnl control events: %lld", g_dxgkEvents);

    if (g_nvEvents == 0) {
        LogLine("");
        LogLine("NO NVIDIA DisplayDriver EVENTS.");
        LogLine("  If DxgKrnl above is nonzero the session works, so suspect: Smooth Motion off,");
        LogLine("  nothing being flipped on this GPU, or the provider not present on this driver.");
        LogLine("  If DxgKrnl is also zero, the session itself never delivered - check elevation.");
        return;
    }
    if (g_decodeOk == 0) {
        LogLine("");
        LogLine("EVENTS ARRIVED BUT NONE DECODED: the manifest is not registered for TDH.");
        LogLine("  The hexdumps above are the payloads; derive the field offsets from them and");
        LogLine("  decode positionally instead of by name.");
        return;
    }

    if (g_dtsCount) {
        LogLine("");
        LogLine("scanout cadence (dts between consecutive proposed flip times), n=%lld mean=%.3fms",
                g_dtsCount, (double)g_dtsSum / (double)g_dtsCount / 1000.0);
        for (int b = 0; b < kDtsBuckets; b++) {
            if (!g_dtsHist[b]) continue;
            const double pctOf = 100.0 * (double)g_dtsHist[b] / (double)g_dtsCount;
            char bar[64];
            int n = (int)(pctOf / 2.0);
            if (n > 60) n = 60;
            memset(bar, '#', (size_t)n); bar[n] = 0;
            LogLine("  %5.1f-%5.1fms %7lld %5.1f%% %s",
                    b * 0.5, (b + 1) * 0.5, g_dtsHist[b], pctOf, bar);
        }
        LogLine("  (2x Smooth Motion on a 60 fps source should concentrate near 8.33 ms;");
        LogLine("   a tight single peak means the grid is even and can be modelled arithmetically)");
    }

    qsort(g_lagUs, (size_t)g_lagN, sizeof(long long), Cmp);
    qsort(g_aheadUs, (size_t)g_aheadN, sizeof(long long), Cmp);
    if (g_lagN) {
        LogLine("");
        LogLine("ETW delivery lag (callback minus event stamp): p50=%lldus p95=%lldus max=%lldus",
                Pct(g_lagUs, g_lagN, 50), Pct(g_lagUs, g_lagN, 95), g_lagUs[g_lagN - 1]);
        LogLine("  (relay bracketing lag is 20833us: a delivery lag above that means the label");
        LogLine("   for a frame arrives AFTER the policy already decided about it)");
    }
    if (g_aheadN) {
        LogLine("scheduled ahead (proposed flip minus event stamp): p50=%lldus p95=%lldus",
                Pct(g_aheadUs, g_aheadN, 50), Pct(g_aheadUs, g_aheadN, 95));
    }
}

int main(int argc, char** argv) {
    int seconds = 20;
    bool alsoDxgk = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--dxgk")) alsoDxgk = true;
        else seconds = atoi(argv[i]);
    }
    if (seconds <= 0) seconds = 20;

    LARGE_INTEGER f; QueryPerformanceFrequency(&f); g_qpcFreq = f.QuadPart;
    g_log = fopen("EtwProbe.log", "w");
    LogLine("=== EtwProbe (NVIDIA DisplayDriver FlipRequest) - run %d s%s ===",
            seconds, alsoDxgk ? ", DxgKrnl control enabled" : "");
    LogLine("QPC frequency %lld Hz", g_qpcFreq);

    const size_t nameBytes = (wcslen(kSessionName) + 1) * sizeof(wchar_t);
    const size_t sz = sizeof(EVENT_TRACE_PROPERTIES) + nameBytes;
    EVENT_TRACE_PROPERTIES* props = (EVENT_TRACE_PROPERTIES*)calloc(1, sz);
    if (!props) return 1;

    // A stale session from a crashed run would block StartTrace; stop it first, ignore errors.
    FillProps(props, sz);
    ControlTraceW(0, kSessionName, props, EVENT_TRACE_CONTROL_STOP);
    FillProps(props, sz);   // ControlTrace overwrites fields

    ULONG st = StartTraceW(&g_session, kSessionName, props);
    if (st != ERROR_SUCCESS) {
        LogLine("FATAL: StartTrace failed (%lu) - real-time ETW requires elevation", st);
        free(props);
        return 1;
    }

    st = EnableTraceEx2(g_session, &kNvDisplayDriverGuid, EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                        kFlipRequestLevel, kFlipRequestKeyword, 0, 0, nullptr);
    if (st != ERROR_SUCCESS) {
        LogLine("WARN: EnableTraceEx2(NVIDIA DisplayDriver) failed (%lu) - provider may not exist "
                "on this driver", st);
    }
    if (alsoDxgk) {
        st = EnableTraceEx2(g_session, &kDxgKrnlGuid, EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                            TRACE_LEVEL_INFORMATION, 0, 0, 0, nullptr);
        if (st != ERROR_SUCCESS) LogLine("WARN: EnableTraceEx2(DxgKrnl) failed (%lu)", st);
    }

    EVENT_TRACE_LOGFILEW logfile = {};
    logfile.LoggerName          = (LPWSTR)kSessionName;
    logfile.ProcessTraceMode    = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
    logfile.EventRecordCallback = OnEvent;
    TRACEHANDLE consumer = OpenTraceW(&logfile);
    if (consumer == INVALID_PROCESSTRACE_HANDLE) {
        LogLine("FATAL: OpenTrace failed (%lu)", GetLastError());
        free(props);
        return 1;
    }

    HANDLE stopper = CreateThread(nullptr, 0, StopAfter, (LPVOID)(intptr_t)seconds, 0, nullptr);
    ProcessTrace(&consumer, 1, nullptr, nullptr);   // blocks until the stop thread stops the session
    if (stopper) CloseHandle(stopper);

    Summary();
    CloseTrace(consumer);
    free(props);
    if (g_log) fclose(g_log);
    return 0;
}
