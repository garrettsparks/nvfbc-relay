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
// DECODING: field names are not available. TdhGetProperty resolves them only when the
// provider registers a manifest, and NVIDIA's DisplayDriver provider does not - it is an
// internal diagnostic surface, which is why PresentMon ships a reverse-engineered copy of
// the schema instead of asking the system. Measured on a current driver: 10800 of 10800
// events failed to resolve. Payloads are therefore decoded positionally from a layout
// recovered on real hardware (see FlipDecode), and this probe ALWAYS hexdumps the first few
// payloads so the layout can be re-derived if a driver update moves it.
//
// Requires elevation (real-time ETW).
// Usage: EtwProbe.exe [seconds] [--dxgk] [--events N] [--bufkb K] [--minbuf N]
//                     [--flushms M] [--noperproc] [--dxgkkw HEX] [--dxgklevel N]
//   --dxgk enables Microsoft-Windows-DxgKrnl and prints a census of every event kind that
//   arrives: id, opcode, version, count, and the TDH-resolved task/opcode/field names.
//
//   It started as a liveness control (NVIDIA silent + DxgKrnl alive = the provider is the
//   problem) and it still serves that, but its real job now is a question the NVIDIA event
//   cannot answer. That event is called FlipRequest and its payload is a PROPOSED flip time,
//   so it records what the driver INTENDED. Measured on real captures, consecutive proposals
//   sometimes alternate short/long by ~1 ms around a rock-steady mean while the monitor's own
//   readout stays locked - consistent with the proposals being corrected before scanout.
//   DxgKrnl is where a completion timestamp would live, and unlike NVIDIA's provider it
//   registers a manifest, so TDH names its events and fields instead of us guessing IDs.
//
//   --dxgkkw widens the keyword past the 0x1 default if the census looks too thin.

#include "FlipDecode.h"

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

// Provider identities and the payload layout live in FlipDecode so the relay and this
// probe cannot drift apart in how they read the same events.
using flipdecode::kNvDisplayDriverGuid;
using flipdecode::kFlipRequestId;
using flipdecode::kFlipRequestLevel;
using flipdecode::kFlipRequestKeyword;
using flipdecode::kDxgKrnlGuid;

static const wchar_t* kSessionName = L"EtwProbeSession";
// Trace buffer pool. Sized to match PresentMon's real-time session, which is the most
// mature consumer of this same provider: 64 KB buffers, 256 minimum, 1024 maximum.
//
// An earlier guess here was 4 KB x 16 on the theory that small buffers fill faster and so
// deliver sooner. Measurement killed it - shrinking buffers moved delivery latency
// hardly at all, because the kernel's real-time cadence, not buffer fill, is what governs
// it. Worse, that config left a 64 KB TOTAL pool against PresentMon's 16 MB, which is the
// likely reason the one run that consolidated per-processor buffers into that tiny pool
// also saw a third of the expected events: consolidation removes the per-CPU slack that
// was hiding how small the pool was.
static ULONG kBufferSizeKb = 64;
static ULONG kMinBuffers   = 256;
static ULONG kMaxBuffers   = 1024;
// Forced-flush interval in ms, 0 = off. The kernel delivers real-time buffers on a cadence
// of its own that measured at ~1 s and barely moved for 46x the traffic, but the flush
// StopTrace performs at shutdown delivered in 9.4 ms. So the consumer asking for a flush
// on a timer exercises a path already known to be fast.
static int   kFlushMs      = 0;
// Consolidate the per-processor buffers into one set. By default ETW keeps a buffer per
// CPU, so a given provider's traffic is divided across them and each fills N times slower.
static bool  kNoPerProc    = false;
// Loss statistics, captured from the session at stop. Aggressive flushing that "fixes"
// latency by dropping events would otherwise look like success.
static ULONG g_eventsLost = 0, g_rtBuffersLost = 0, g_logBuffersLost = 0, g_buffersWritten = 0;
static const int  kMaxHeads      = 8;    // vidPnSourceId values tracked
static const int  kHexDumpEvents = 8;    // payloads dumped verbatim for layout recovery
// Per-event lines before going quiet. The aggregates below (histogram, percentiles)
// always cover the whole run, but correlating flips against NvFBC wakes needs the raw
// ts= values, so the default is sized for a full minute at ~120 flips/s rather than for
// a small log. Override with --events N.
static int        g_logEvents    = 20000;
static const int  kDtsBuckets    = 80;   // 0.5 ms each, so 0..40 ms

static FILE*       g_log      = nullptr;
static LONGLONG    g_qpcFreq  = 1;
static TRACEHANDLE g_session  = 0;

static long long g_nvEvents    = 0;
static long long g_flipEvents  = 0;
static long long g_dxgkEvents  = 0;
static long long g_decodeOk    = 0;
static long long g_decodeFail  = 0;
static long long g_startQpc    = 0;
static volatile bool g_stopFlush = false;
static int       g_dumped      = 0;

static long long g_lastTsByHead[kMaxHeads] = {};
static long long g_dtsHist[kDtsBuckets] = {};
static long long g_dtsCount = 0;
static long long g_dtsSum = 0;

// DxgKrnl census. The NVIDIA event is called FlipRequest and its payload field is a
// PROPOSED flip time, so it records the driver's INTENT. Whether the hardware scanned out
// on that schedule is a different question, and DxgKrnl is where the answer lives: unlike
// NVIDIA's provider it registers a manifest, so TDH resolves event and field names and we
// can find the completion event by READING what the system offers instead of guessing IDs
// off memory. This is deliberately a census, not a decoder: the first job is to learn which
// events exist and what their fields are called.
static const int kMaxDxgkKinds = 192;
static const int kDxgkNameLen  = 72;
static const int kDxgkPropsLen = 320;
struct DxgkKind {
    USHORT id;
    UCHAR  opcode;
    UCHAR  version;
    long long count;
    bool   resolved;
    char   task[kDxgkNameLen];
    char   opname[kDxgkNameLen];
    char   props[kDxgkPropsLen];
};
static DxgkKind g_dxgkKinds[kMaxDxgkKinds];
static int  g_dxgkKindN = 0;
// 0x1 is DxgKrnl's base keyword, which carries the flip and vsync events without pulling in
// the scheduler and memory-manager traffic that makes an unfiltered session lose data.
static ULONGLONG g_dxgkKeyword = 0x1;
static int       g_dxgkLevel   = TRACE_LEVEL_INFORMATION;
static bool g_dxgkNamed = false;   // did TDH resolve anything at all
static long long g_dxgkUnresolved = 0;

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

// Narrow a manifest's wide strings into the fixed buffers above. Names are ASCII in
// practice; anything else is truncated rather than rejected, because a garbled name is
// still a usable discriminator between event kinds.
static void NarrowInto(const wchar_t* w, char* out, int cap) {
    if (!w) { out[0] = 0; return; }
    int i = 0;
    for (; w[i] && i < cap - 1; i++) out[i] = (w[i] < 128) ? (char)w[i] : '?';
    out[i] = 0;
    while (i > 0 && (out[i - 1] == ' ' || out[i - 1] == '\n' || out[i - 1] == '\r')) out[--i] = 0;
}

// Resolve one event kind's names and top-level field names through TDH. Called once per
// distinct kind, never per event: TdhGetEventInformation allocates and parses a manifest
// section, which is far too slow for a callback that sees thousands of events a second.
static void ResolveDxgkKind(PEVENT_RECORD ev, DxgkKind* k) {
    k->resolved = true;
    ULONG size = 0;
    if (TdhGetEventInformation(ev, 0, nullptr, nullptr, &size) != ERROR_INSUFFICIENT_BUFFER) {
        g_dxgkUnresolved++;
        return;
    }
    TRACE_EVENT_INFO* info = (TRACE_EVENT_INFO*)malloc(size);
    if (!info) return;
    if (TdhGetEventInformation(ev, 0, nullptr, info, &size) != ERROR_SUCCESS) {
        free(info);
        g_dxgkUnresolved++;
        return;
    }
    g_dxgkNamed = true;
    if (info->TaskNameOffset)
        NarrowInto((const wchar_t*)((PBYTE)info + info->TaskNameOffset), k->task, kDxgkNameLen);
    if (info->OpcodeNameOffset)
        NarrowInto((const wchar_t*)((PBYTE)info + info->OpcodeNameOffset), k->opname, kDxgkNameLen);
    // Field names are the point: a completion event that carries a VidPnSourceId and a
    // timestamp is what would replace the NVIDIA proposal, and only the manifest says
    // which of these events has them.
    int w = 0;
    for (ULONG i = 0; i < info->TopLevelPropertyCount && w < kDxgkPropsLen - 2; i++) {
        const ULONG off = info->EventPropertyInfoArray[i].NameOffset;
        if (!off) continue;
        char name[kDxgkNameLen];
        NarrowInto((const wchar_t*)((PBYTE)info + off), name, kDxgkNameLen);
        const int n = snprintf(k->props + w, (size_t)(kDxgkPropsLen - w), "%s%s",
                               w ? "," : "", name);
        if (n <= 0) break;
        w += n;
    }
    free(info);
}

static void CountDxgk(PEVENT_RECORD ev) {
    const USHORT id = ev->EventHeader.EventDescriptor.Id;
    const UCHAR op = ev->EventHeader.EventDescriptor.Opcode;
    const UCHAR ver = ev->EventHeader.EventDescriptor.Version;
    for (int i = 0; i < g_dxgkKindN; i++) {
        DxgkKind* k = &g_dxgkKinds[i];
        if (k->id == id && k->opcode == op && k->version == ver) { k->count++; return; }
    }
    if (g_dxgkKindN >= kMaxDxgkKinds) return;
    DxgkKind* k = &g_dxgkKinds[g_dxgkKindN++];
    k->id = id; k->opcode = op; k->version = ver; k->count = 1;
    k->task[0] = 0; k->opname[0] = 0; k->props[0] = 0;
    ResolveDxgkKind(ev, k);
}

static int CmpDxgk(const void* a, const void* b) {
    const long long x = ((const DxgkKind*)a)->count, y = ((const DxgkKind*)b)->count;
    return (x < y) - (x > y);   // descending
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
        CountDxgk(ev);
        return;
    }
    if (!IsEqualGUID(ev->EventHeader.ProviderId, kNvDisplayDriverGuid)) return;
    g_nvEvents++;
    if (ev->EventHeader.EventDescriptor.Id != kFlipRequestId) return;
    g_flipEvents++;

    const long long evtQpc = ev->EventHeader.TimeStamp.QuadPart;
    const long long lagUs  = (now.QuadPart - evtQpc) * 1000000 / g_qpcFreq;

    // Verbatim payload for the first few events. This is the only route to recovering the
    // wire layout if a driver update moves it, so it runs regardless of decode success.
    if (g_dumped < kHexDumpEvents) {
        g_dumped++;
        LogLine("flip payload #%d: %lu bytes (ver=%u opcode=%u)",
                g_dumped, (unsigned long)ev->UserDataLength,
                ev->EventHeader.EventDescriptor.Version,
                ev->EventHeader.EventDescriptor.Opcode);
        HexDump((const BYTE*)ev->UserData, ev->UserDataLength);
    }

    flipdecode::FlipEvent fe;
    if (!flipdecode::DecodeFlip(ev, g_qpcFreq, &fe)) {
        g_decodeFail++;
        if (g_decodeFail <= 3) {
            // Print the numbers that failed, not just that something did: the guard
            // rejects on size, version, an implausible flip time, or an implausible head,
            // and which one it was decides whether the driver changed or the session is
            // misconfigured.
            unsigned long long rawTs = 0;
            if (ev->UserDataLength >= 24) memcpy(&rawTs, (const BYTE*)ev->UserData + 16, 8);
            LogLine("  decode rejected: len=%lu ver=%u payloadTs=%llu evtTs=%lld delta=%lld "
                    "(a delta far outside a second means the event clock is not QPC)",
                    (unsigned long)ev->UserDataLength,
                    ev->EventHeader.EventDescriptor.Version,
                    rawTs, (long long)ev->EventHeader.TimeStamp.QuadPart,
                    (long long)((long long)rawTs - ev->EventHeader.TimeStamp.QuadPart));
        }
        return;
    }
    g_decodeOk++;
    const uint64_t alloc = fe.alloc, ts = fe.displayQpc;
    const uint32_t head = fe.head, token = fe.token;

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

    if (g_flipEvents <= g_logEvents) {
        LogLine("flip #%lld evt=%lld recv_lag=%lldus ts=%llu ahead=%lldus dts=%lldus "
                "head=%u token=%u alloc=0x%llX",
                g_flipEvents, evtQpc, lagUs, (unsigned long long)ts, aheadUs, dtsUs,
                head, token, (unsigned long long)alloc);
    }
}

// Ask the kernel to hand over whatever it has, rather than waiting for its own cadence.
static DWORD WINAPI FlushLoop(LPVOID) {
    const size_t nameBytes = (wcslen(kSessionName) + 1) * sizeof(wchar_t);
    const size_t sz = sizeof(EVENT_TRACE_PROPERTIES) + nameBytes;
    EVENT_TRACE_PROPERTIES* p = (EVENT_TRACE_PROPERTIES*)calloc(1, sz);
    if (!p) return 0;
    while (!g_stopFlush) {
        Sleep((DWORD)kFlushMs);
        memset(p, 0, sz);
        p->Wnode.BufferSize = (ULONG)sz;
        ControlTraceW(g_session, kSessionName, p, EVENT_TRACE_CONTROL_FLUSH);
    }
    free(p);
    return 0;
}

static DWORD WINAPI StopAfter(LPVOID arg) {
    const int seconds = (int)(intptr_t)arg;
    Sleep((DWORD)seconds * 1000);
    g_stopFlush = true;
    const size_t nameBytes = (wcslen(kSessionName) + 1) * sizeof(wchar_t);
    const size_t sz = sizeof(EVENT_TRACE_PROPERTIES) + nameBytes;
    EVENT_TRACE_PROPERTIES* p = (EVENT_TRACE_PROPERTIES*)calloc(1, sz);
    if (!p) return 0;
    p->Wnode.BufferSize = (ULONG)sz;
    // The stop call returns the session's final counters, which is the only honest way to
    // tell a genuine latency win from one bought by dropping events.
    if (ControlTraceW(g_session, kSessionName, p, EVENT_TRACE_CONTROL_STOP) == ERROR_SUCCESS) {
        g_eventsLost     = p->EventsLost;
        g_rtBuffersLost  = p->RealTimeBuffersLost;
        g_logBuffersLost = p->LogBuffersLost;
        g_buffersWritten = p->BuffersWritten;
    }
    free(p);
    return 0;
}

static void FillProps(EVENT_TRACE_PROPERTIES* props, size_t sz) {
    props->Wnode.BufferSize    = (ULONG)sz;   // size of THIS struct, not the trace buffer
    props->Wnode.Flags         = WNODE_FLAG_TRACED_GUID;
    props->Wnode.ClientContext = 1;                     // QPC clock, matching the relay's arr=
    props->LogFileMode         = EVENT_TRACE_REAL_TIME_MODE;
    props->LoggerNameOffset    = sizeof(EVENT_TRACE_PROPERTIES);

    // Delivery latency is set here, and the defaults are useless for this purpose.
    // Measured with them: buffers flushed once a second and delivered the PREVIOUS
    // second's, so even the freshest event in a burst was ~1017 ms old - two orders of
    // magnitude past the relay's 20833 us bracketing lag. The same run's final burst,
    // which StopTrace flushes immediately, delivered in 9.8 ms, so the pipeline is fast
    // and only the cadence was slow.
    //
    // FlushTimer is in SECONDS and cannot go below 1, so it is not the lever. Buffers
    // also flush when FULL, so the lever is making them small enough to fill quickly:
    // at the measured ~100 KB/s of provider traffic a 4 KB buffer fills in roughly
    // 40 ms. More buffers in rotation keeps the provider from stalling or dropping while
    // the consumer drains one.
    if (kNoPerProc) props->LogFileMode |= EVENT_TRACE_NO_PER_PROCESSOR_BUFFERING;
    props->BufferSize     = kBufferSizeKb;
    props->MinimumBuffers = kMinBuffers;
    props->MaximumBuffers = kMaxBuffers;
    props->FlushTimer     = 1;
}

static void Summary() {
    LARGE_INTEGER endQpc;
    QueryPerformanceCounter(&endQpc);
    LogLine("");
    LogLine("=== summary ===");
    // The relay's log is normally much longer than the probe's window (it starts before
    // the game and stops after). These two values are what window it to this trace, on
    // the same QPC clock the relay records its origin in.
    LogLine("session window (QPC ticks): %lld .. %lld",
            g_startQpc, (long long)endQpc.QuadPart);
    LogLine("NVIDIA DisplayDriver events: %lld (FlipRequest %lld, decoded %lld, undecodable %lld)",
            g_nvEvents, g_flipEvents, g_decodeOk, g_decodeFail);
    if (g_dxgkEvents) {
        LogLine("DxgKrnl events: %lld across %d distinct kinds%s",
                g_dxgkEvents, g_dxgkKindN,
                g_dxgkNamed ? "" : "   <== TDH resolved NOTHING; names below are blank");
        // Sorted by volume, because the event that fires once per scanout is the one whose
        // rate matches the refresh rate, and that is visible in the count alone.
        qsort(g_dxgkKinds, (size_t)g_dxgkKindN, sizeof(DxgkKind), CmpDxgk);
        LogLine("  %-6s %-4s %-4s %10s  %-28s %-16s %s",
                "id", "op", "ver", "count", "task", "opcode", "fields");
        for (int i = 0; i < g_dxgkKindN; i++) {
            const DxgkKind* k = &g_dxgkKinds[i];
            LogLine("  %-6u %-4u %-4u %10lld  %-28s %-16s %s",
                    (unsigned)k->id, (unsigned)k->opcode, (unsigned)k->version, k->count,
                    k->task[0] ? k->task : "-", k->opname[0] ? k->opname : "-",
                    k->props[0] ? k->props : "-");
        }
        if (g_dxgkUnresolved) {
            LogLine("  (%lld kinds had no manifest entry; those rows show '-')",
                    g_dxgkUnresolved);
        }
        LogLine("  READ THIS AGAINST THE NVIDIA RATE ABOVE. A kind whose count matches the");
        LogLine("  refresh rate and whose fields carry a VidPnSourceId is the scanout event;");
        LogLine("  if its cadence is even while FlipRequest's is not, FlipRequest is an intent.");
    }
    LogLine("session losses: events %lu, realtime buffers %lu, log buffers %lu, "
            "buffers written %lu%s",
            (unsigned long)g_eventsLost, (unsigned long)g_rtBuffersLost,
            (unsigned long)g_logBuffersLost, (unsigned long)g_buffersWritten,
            (g_eventsLost || g_rtBuffersLost || g_logBuffersLost)
                ? "   <== LOSS: any latency figure here was partly bought by dropping data"
                : "");

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
        LogLine("EVENTS ARRIVED BUT NONE DECODED: the payload no longer matches the layout");
        LogLine("  this build was built against. The hexdumps above are the current payloads;");
        LogLine("  re-derive the field offsets from them.");
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
        else if (!strcmp(argv[i], "--events") && i + 1 < argc) g_logEvents = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--bufkb") && i + 1 < argc) kBufferSizeKb = (ULONG)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--minbuf") && i + 1 < argc) kMinBuffers = (ULONG)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--flushms") && i + 1 < argc) kFlushMs = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--noperproc")) kNoPerProc = true;
        else if (!strcmp(argv[i], "--dxgkkw") && i + 1 < argc)
            g_dxgkKeyword = strtoull(argv[++i], nullptr, 0);
        else if (!strcmp(argv[i], "--dxgklevel") && i + 1 < argc)
            g_dxgkLevel = atoi(argv[++i]);
        else seconds = atoi(argv[i]);
    }
    if (seconds <= 0) seconds = 20;
    if (g_logEvents < 0) g_logEvents = 0;

    LARGE_INTEGER f; QueryPerformanceFrequency(&f); g_qpcFreq = f.QuadPart;
    LARGE_INTEGER startQpc; QueryPerformanceCounter(&startQpc); g_startQpc = startQpc.QuadPart;
    g_log = fopen("EtwProbe.log", "w");
    LogLine("=== EtwProbe (NVIDIA DisplayDriver FlipRequest) - run %d s%s ===",
            seconds, alsoDxgk ? ", DxgKrnl control enabled" : "");
    LogLine("QPC frequency %lld Hz", g_qpcFreq);
    // evt= and ts= below are ABSOLUTE QPC ticks on the same clock the relay logs its
    // origin in, so the two logs join exactly with no wallclock or fingerprinting.
    LogLine("per-event lines: first %d flips", g_logEvents);
    LogLine("trace buffers: %lu KB x %lu-%lu, flush timer 1 s, forced flush %s, "
            "per-processor buffering %s",
            (unsigned long)kBufferSizeKb, (unsigned long)kMinBuffers,
            (unsigned long)kMaxBuffers,
            kFlushMs > 0 ? "on" : "off", kNoPerProc ? "OFF (consolidated)" : "on (default)");
    if (kFlushMs > 0) LogLine("  forced flush every %d ms", kFlushMs);

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
        // Keyword 0 means "every keyword", which on DxgKrnl is a firehose (context switches,
        // allocations, paging). Default to the base keyword and let --dxgkkw widen it, so a
        // discovery run does not drown itself and start losing the events it came for.
        st = EnableTraceEx2(g_session, &kDxgKrnlGuid, EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                            (UCHAR)g_dxgkLevel, g_dxgkKeyword, 0, 0, nullptr);
        if (st != ERROR_SUCCESS) LogLine("WARN: EnableTraceEx2(DxgKrnl) failed (%lu)", st);
        else LogLine("DxgKrnl enabled: level %d keyword 0x%llX",
                     g_dxgkLevel, (unsigned long long)g_dxgkKeyword);
    }

    EVENT_TRACE_LOGFILEW logfile = {};
    logfile.LoggerName          = (LPWSTR)kSessionName;
    // RAW_TIMESTAMP is what makes EventHeader.TimeStamp arrive in the units the session
    // asked for (Wnode.ClientContext = 1, i.e. QPC). Without it ETW helpfully converts
    // every stamp to system time, so the event clock and the payload's flip time end up
    // in different units and nothing can be compared against anything.
    logfile.ProcessTraceMode    = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD |
                                  PROCESS_TRACE_MODE_RAW_TIMESTAMP;
    logfile.EventRecordCallback = OnEvent;
    TRACEHANDLE consumer = OpenTraceW(&logfile);
    if (consumer == INVALID_PROCESSTRACE_HANDLE) {
        LogLine("FATAL: OpenTrace failed (%lu)", GetLastError());
        free(props);
        return 1;
    }

    HANDLE stopper = CreateThread(nullptr, 0, StopAfter, (LPVOID)(intptr_t)seconds, 0, nullptr);
    HANDLE flusher = kFlushMs > 0 ? CreateThread(nullptr, 0, FlushLoop, nullptr, 0, nullptr) : NULL;
    ProcessTrace(&consumer, 1, nullptr, nullptr);   // blocks until the stop thread stops the session
    g_stopFlush = true;
    if (flusher) { WaitForSingleObject(flusher, 2000); CloseHandle(flusher); }
    if (stopper) CloseHandle(stopper);

    Summary();
    CloseTrace(consumer);
    free(props);
    if (g_log) fclose(g_log);
    return 0;
}
