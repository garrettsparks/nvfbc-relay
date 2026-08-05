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
//   --dxgkkw widens the keyword past the 0x1 default if the census looks too thin, and
//   --dxgkall drops the event-id filter to re-run the full census after a Windows update.
//
//   The census has now been run (600 s, 60x2). DxgKrnl carries VSyncDPC.FrameQPCTime at the
//   refresh rate with a VidPnSourceId, so the probe decodes it and prints its cadence beside
//   the NVIDIA one. Also decoded: VSyncSmoothenedTime, which exposes both an ORIGINAL and a
//   SMOOTHENED vsync stamp - Windows keeping two clocks is itself evidence about how even
//   the raw one is.

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
// The events worth decoding, found by running the census above on real hardware. Requested
// through an event-ID filter because the unfiltered provider delivered 27.4 MILLION events in
// 600 s and pushed ETW delivery latency to 738 ms p50 - harmless for a probe reading payload
// timestamps, useless for anything in-band, and a needless risk of loss.
//
//   17  VSyncDPC                   FrameQPCTime + VidPnSourceId. The scanout boundary.
//   259 MMIOFlipMultiPlaneOverlay  submission; its count matched NVIDIA FlipRequest EXACTLY
//                                  (72056 both), which is why FlipRequest looks like a submit
//                                  -path event rather than a completion.
//   266 IndependentFlip            PresentAtQpc.
//   502 VSyncSmoothenedTime        OriginalDpcFrameTime vs SmoothenedDpcFrameTime: Windows
//                                  keeps a SMOOTHED vsync clock beside the raw one, which is
//                                  the system saying outright that raw vsync timing is noisy.
//   505 VSyncHwFlipQueueLogUpdate  CompletionTimeStamp, an independent completion stamp.
//   181 VSyncInterrupt             no timestamp FIELD, but its event-header stamp is the raw
//                                  vblank interrupt - a scanout reference that depends on no
//                                  payload decoding at all.
//   273 VSyncDPCMultiPlane         FlipQueueIntervalTarget: what interval the driver is
//                                  AIMING for. A constant target beside a varying actual is
//                                  the difference between intent and outcome, stated by the
//                                  system rather than inferred by us.
//   506 ResetSmoother              CurrentSmoothenedVSyncPeriodQpc: what Windows currently
//                                  believes the vsync period to be.
//   458 DdiControlInterrupt2       VsyncState + LastFrameTime, low volume.
//   503 VSyncTimeStatistics        VsyncState and how long it spent on / off / keeping phase,
//                                  which is the VRR state machine reporting itself.
//   184 Present                    the APPLICATION's own Present() call, with the calling
//                                  process in the event header. This is what PresentMon derives
//                                  MsBetweenPresents from, and it is the candidate real-vs-
//                                  generated LABEL: the game calls Present once per real frame,
//                                  and a driver-generated frame has no application Present behind
//                                  it. A flip that matches a game Present is real; one that does
//                                  not is generated. Census counted 71995 of these in 600 s
//                                  (120/s) with all processes pooled, so the FIRST thing to check
//                                  is whether the game's own share comes out at a clean 60/s.
static const USHORT kDxgkWantIds[] = { 17, 181, 184, 259, 266, 273, 458, 502, 503, 505, 506 };
static const int    kDxgkWantN = (int)(sizeof(kDxgkWantIds) / sizeof(kDxgkWantIds[0]));
static bool g_dxgkFilter = true;

static DxgkKind g_dxgkKinds[kMaxDxgkKinds];
static int  g_dxgkKindN = 0;
// Scanout cadence measured from VSyncDPC, kept per head exactly like the NVIDIA one so the
// two can be read side by side. This is the comparison the whole probe now exists for.
static long long g_vsLastByHead[kMaxHeads] = {};
static long long g_vsHist[kDtsBuckets] = {};
static long long g_vsCount = 0;
static long long g_vsEvents = 0;
static long long g_vsSmoothN = 0;
static long long g_vsSmoothDeltaSum = 0;
static long long g_dxgkLogged = 0;   // shared --events budget for the per-event detail lines
// Application presents per process. The whole point of reading event 184 is attribution: pooled
// across processes it is meaningless, but if ONE process presents at the source rate while flips
// arrive at twice that, the flips without a present behind them are the generated frames.
static const int kMaxPresentPids = 24;
struct PidCount { unsigned long pid; long long n; };
static PidCount g_presentPids[kMaxPresentPids];
static int g_presentPidN = 0;
static long long g_presentTotal = 0;
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

// Read one named scalar field. Works for any integer width up to 8 bytes because the value
// is zero-extended into a u64, which is what every field we want here is. Returns false
// rather than a default when the field is absent, so a manifest change surfaces as missing
// data instead of as a plausible zero.
static bool TdhScalar(PEVENT_RECORD ev, const wchar_t* name, uint64_t* out) {
    PROPERTY_DATA_DESCRIPTOR d = {};
    d.PropertyName = (ULONGLONG)(ULONG_PTR)name;
    d.ArrayIndex = ULONG_MAX;
    ULONG size = 0;
    if (TdhGetPropertySize(ev, 0, nullptr, 1, &d, &size) != ERROR_SUCCESS) return false;
    if (size == 0 || size > sizeof(uint64_t)) return false;
    uint64_t v = 0;
    if (TdhGetProperty(ev, 0, nullptr, 1, &d, size, (PBYTE)&v) != ERROR_SUCCESS) return false;
    *out = v;
    return true;
}

// The measurement this probe now exists to make: the ACTUAL scanout grid, from the graphics
// kernel, beside the NVIDIA driver's PROPOSED one. If VSyncDPC's cadence is even while
// FlipRequest's alternates, FlipRequest is an intent and every "true scanout time" derived
// from it needs revisiting.
static void DecodeDxgk(PEVENT_RECORD ev, long long evtQpc) {
    const USHORT id = ev->EventHeader.EventDescriptor.Id;
    if (id == 17) {                       // VSyncDPC
        uint64_t qpc = 0, head = 0, frame = 0;
        if (!TdhScalar(ev, L"FrameQPCTime", &qpc)) return;
        TdhScalar(ev, L"VidPnSourceId", &head);
        TdhScalar(ev, L"FrameNumber", &frame);
        g_vsEvents++;
        long long dtsUs = -1;
        if (head < (uint64_t)kMaxHeads && qpc != 0) {
            if (g_vsLastByHead[head] != 0 && (long long)qpc > g_vsLastByHead[head]) {
                dtsUs = ((long long)qpc - g_vsLastByHead[head]) * 1000000 / g_qpcFreq;
                const int b = (int)(dtsUs / 500);
                if (b >= 0 && b < kDtsBuckets) { g_vsHist[b]++; g_vsCount++; }
            }
            g_vsLastByHead[head] = (long long)qpc;
        }
        if (g_vsEvents <= g_logEvents) {
            LogLine("vsync head=%llu frame=%llu qpc=%lld dts=%lldus evt=%lld",
                    (unsigned long long)head, (unsigned long long)frame,
                    (long long)qpc, dtsUs, evtQpc);
        }
        return;
    }
    if (id == 502) {                      // VSyncSmoothenedTime
        uint64_t head = 0, orig = 0, smooth = 0, delta = 0;
        TdhScalar(ev, L"VidPnSourceId", &head);
        const bool haveOrig = TdhScalar(ev, L"OriginalDpcFrameTime", &orig);
        const bool haveSm = TdhScalar(ev, L"SmoothenedDpcFrameTime", &smooth);
        TdhScalar(ev, L"FrameTimeDeltaIn100ns", &delta);
        if (haveOrig && haveSm) {
            g_vsSmoothN++;
            g_vsSmoothDeltaSum += (long long)delta;
        }
        if (g_vsSmoothN <= g_logEvents) {
            LogLine("vsmooth head=%llu orig=%llu smooth=%llu delta100ns=%llu evt=%lld",
                    (unsigned long long)head, (unsigned long long)orig,
                    (unsigned long long)smooth, (unsigned long long)delta, evtQpc);
        }
        return;
    }
    // Per-process present counting happens BEFORE the log budget, because the census has to
    // cover the whole run: a rate is only meaningful over the full window, and truncating it
    // at --events would silently report the rate of the first few minutes.
    if (id == 184) {
        const unsigned long pid = (unsigned long)ev->EventHeader.ProcessId;
        g_presentTotal++;
        bool found = false;
        for (int i = 0; i < g_presentPidN; i++) {
            if (g_presentPids[i].pid == pid) { g_presentPids[i].n++; found = true; break; }
        }
        if (!found && g_presentPidN < kMaxPresentPids) {
            g_presentPids[g_presentPidN].pid = pid;
            g_presentPids[g_presentPidN].n = 1;
            g_presentPidN++;
        }
    }

    // Everything below is per-event detail for offline windowing, so it shares one budget
    // with --events. Unbudgeted it is ~500 lines/s, which turns a ten-minute run into a
    // 50 MB log and makes the probe's own I/O a variable in the thing it is measuring.
    if (g_dxgkLogged >= g_logEvents) return;
    g_dxgkLogged++;

    if (id == 505) {                      // VSyncHwFlipQueueLogUpdate
        uint64_t head = 0, comp = 0, done = 0, pid = 0;
        if (!TdhScalar(ev, L"CompletionTimeStamp", &comp)) return;
        TdhScalar(ev, L"VidPnSourceId", &head);
        TdhScalar(ev, L"FlipsCompletedCount", &done);
        TdhScalar(ev, L"PresentId", &pid);
        LogLine("hwflip head=%llu comp=%llu done=%llu presentid=%llu evt=%lld",
                (unsigned long long)head, (unsigned long long)comp,
                (unsigned long long)done, (unsigned long long)pid, evtQpc);
        return;
    }
    if (id == 266) {                      // IndependentFlip
        uint64_t head = 0, at = 0, dur = 0;
        if (!TdhScalar(ev, L"PresentAtQpc", &at)) return;
        TdhScalar(ev, L"VidPnSourceId", &head);
        TdhScalar(ev, L"Duration", &dur);
        LogLine("indflip head=%llu at=%llu dur=%llu evt=%lld",
                (unsigned long long)head, (unsigned long long)at,
                (unsigned long long)dur, evtQpc);
        return;
    }
    if (id == 259) {                      // MMIOFlipMultiPlaneOverlay
        uint64_t head = 0, seq = 0, pid = 0;
        TdhScalar(ev, L"VidPnSourceId", &head);
        TdhScalar(ev, L"FlipSubmitSequence", &seq);
        TdhScalar(ev, L"FlipPresentId", &pid);
        LogLine("mmioflip head=%llu seq=%llu presentid=%llu evt=%lld",
                (unsigned long long)head, (unsigned long long)seq,
                (unsigned long long)pid, evtQpc);
        return;
    }
    if (id == 184) {                      // Present: the application's own Present() call
        uint64_t head = 0, iv = 0, flags = 0;
        TdhScalar(ev, L"VidPnSourceId", &head);
        TdhScalar(ev, L"FlipInterval", &iv);
        TdhScalar(ev, L"Flags", &flags);
        // ProcessId comes from the event HEADER, not the payload: it is what attributes a
        // present to the game rather than to DWM, and without it the stream is every
        // process on the machine pooled together.
        LogLine("present pid=%lu head=%llu interval=%llu flags=0x%llX evt=%lld",
                (unsigned long)ev->EventHeader.ProcessId, (unsigned long long)head,
                (unsigned long long)iv, (unsigned long long)flags, evtQpc);
        return;
    }
    if (id == 181) {                      // VSyncInterrupt: header stamp IS the vblank
        uint64_t target = 0, addr = 0;
        TdhScalar(ev, L"VidPnTargetId", &target);
        TdhScalar(ev, L"ScannedPhysicalAddress", &addr);
        LogLine("vsyncint target=%llu scanned=0x%llX evt=%lld",
                (unsigned long long)target, (unsigned long long)addr, evtQpc);
        return;
    }
    if (id == 273) {                      // VSyncDPCMultiPlane: the AIMED-FOR interval
        uint64_t head = 0, frame = 0, itarget = 0, entries = 0, planes = 0;
        TdhScalar(ev, L"VidPnSourceId", &head);
        TdhScalar(ev, L"FrameNumber", &frame);
        TdhScalar(ev, L"FlipQueueIntervalTarget", &itarget);
        TdhScalar(ev, L"FlipEntryCount", &entries);
        TdhScalar(ev, L"PlaneCount", &planes);
        LogLine("vsmulti head=%llu frame=%llu itarget=%llu entries=%llu planes=%llu evt=%lld",
                (unsigned long long)head, (unsigned long long)frame,
                (unsigned long long)itarget, (unsigned long long)entries,
                (unsigned long long)planes, evtQpc);
        return;
    }
    if (id == 506) {                      // ResetSmoother: Windows' believed vsync period
        uint64_t cur = 0, def = 0;
        TdhScalar(ev, L"CurrentSmoothenedVSyncPeriodQpc", &cur);
        TdhScalar(ev, L"NewDefaultVSyncPeriodQpc", &def);
        LogLine("smoothreset cur=%llu newdefault=%llu evt=%lld",
                (unsigned long long)cur, (unsigned long long)def, evtQpc);
        return;
    }
    if (id == 458) {                      // DdiControlInterrupt2: VRR state
        uint64_t head = 0, state = 0, last = 0;
        TdhScalar(ev, L"VidPnSourceId", &head);
        TdhScalar(ev, L"VsyncState", &state);
        TdhScalar(ev, L"LastFrameTime", &last);
        LogLine("vsyncstate head=%llu state=%llu lastframe=%llu evt=%lld",
                (unsigned long long)head, (unsigned long long)state,
                (unsigned long long)last, evtQpc);
        return;
    }
    if (id == 503) {                      // VSyncTimeStatistics: the VRR state machine
        uint64_t state = 0, on = 0, keep = 0, nop = 0;
        TdhScalar(ev, L"VsyncState", &state);
        TdhScalar(ev, L"VSyncOnTotalTimeMs", &on);
        TdhScalar(ev, L"VSyncOffKeepPhaseTotalTimeMs", &keep);
        TdhScalar(ev, L"VSyncOffNoPhaseTotalTimeMs", &nop);
        LogLine("vsyncstats state=%llu onMs=%llu keepPhaseMs=%llu noPhaseMs=%llu evt=%lld",
                (unsigned long long)state, (unsigned long long)on,
                (unsigned long long)keep, (unsigned long long)nop, evtQpc);
        return;
    }
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
        DecodeDxgk(ev, ev->EventHeader.TimeStamp.QuadPart);
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
    if (g_vsCount) {
        // THE COMPARISON. Same buckets, same clock, same run: the graphics kernel's actual
        // vsync grid against the display driver's proposed one. A verdict is deliberately
        // NOT printed here - the shape of both histograms is the evidence, and collapsing
        // it to one word is how a measurement stops being checkable.
        LogLine("");
        LogLine("=== ACTUAL scanout (DxgKrnl VSyncDPC.FrameQPCTime), n=%lld from %lld events ===",
                g_vsCount, g_vsEvents);
        long long peak = 0, mode = 0;
        for (int i = 0; i < kDtsBuckets; i++) {
            if (g_vsHist[i] > peak) { peak = g_vsHist[i]; mode = i; }
        }
        for (int i = 0; i < kDtsBuckets; i++) {
            if (!g_vsHist[i]) continue;
            LogLine("  %5.1f-%5.1f ms  %8lld  %5.1f%%%s", i * 0.5, (i + 1) * 0.5,
                    g_vsHist[i], 100.0 * (double)g_vsHist[i] / (double)g_vsCount,
                    i == mode ? "   <== mode" : "");
        }
        if (g_presentTotal) {
            // The label test. If one process presents at the SOURCE rate while head-0 flips
            // arrive at twice it, the flips with no present behind them are the generated
            // frames - which is a real-vs-generated label that needs no content analysis.
            const double secs = (double)(endQpc.QuadPart - g_startQpc) / (double)g_qpcFreq;
            LogLine("");
            LogLine("=== application Present() by process, %lld total (%.1f/s pooled) ===",
                    g_presentTotal, (double)g_presentTotal / secs);
            for (int i = 0; i < g_presentPidN; i++) {
                for (int j = i + 1; j < g_presentPidN; j++) {
                    if (g_presentPids[j].n > g_presentPids[i].n) {
                        const PidCount t = g_presentPids[i];
                        g_presentPids[i] = g_presentPids[j];
                        g_presentPids[j] = t;
                    }
                }
            }
            for (int i = 0; i < g_presentPidN; i++) {
                LogLine("  pid %-7lu %8lld   %6.1f/s", g_presentPids[i].pid,
                        g_presentPids[i].n, (double)g_presentPids[i].n / secs);
            }
            LogLine("  A process at the SOURCE rate beside head-0 flips at twice it is the");
            LogLine("  real-vs-generated label: generated frames have no Present behind them.");
        }
        if (g_vsSmoothN) {
            LogLine("  VSyncSmoothenedTime samples %lld, mean FrameTimeDelta %.3f ms",
                    g_vsSmoothN,
                    (double)g_vsSmoothDeltaSum / (double)g_vsSmoothN / 10000.0);
            LogLine("  (Windows keeps a smoothed vsync clock beside the raw one; that it does");
            LogLine("   so at all is the system reporting that raw vsync timing is not even.)");
        }
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
        else if (!strcmp(argv[i], "--dxgkall")) g_dxgkFilter = false;
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
        // Ask the provider for only the ids we decode. Unfiltered, this provider delivered
        // 27.4 M events in 600 s; filtered it is ~500/s, which keeps delivery latency usable
        // and removes any chance that the events we came for are the ones that get dropped.
        // --dxgkall turns the filter off to re-run the census after a Windows update.
        BYTE filterBuf[sizeof(EVENT_FILTER_EVENT_ID) + sizeof(USHORT) * 16] = {};
        ENABLE_TRACE_PARAMETERS params = {};
        EVENT_FILTER_DESCRIPTOR fd = {};
        if (g_dxgkFilter) {
            EVENT_FILTER_EVENT_ID* f = (EVENT_FILTER_EVENT_ID*)filterBuf;
            f->FilterIn = TRUE;
            f->Count = (USHORT)kDxgkWantN;
            for (int i = 0; i < kDxgkWantN; i++) f->Events[i] = kDxgkWantIds[i];
            fd.Ptr = (ULONGLONG)(ULONG_PTR)f;
            fd.Size = (ULONG)(sizeof(EVENT_FILTER_EVENT_ID) +
                              sizeof(USHORT) * (size_t)(kDxgkWantN - 1));
            fd.Type = EVENT_FILTER_TYPE_EVENT_ID;
            params.Version = ENABLE_TRACE_PARAMETERS_VERSION_2;
            params.EnableFilterDesc = &fd;
            params.FilterDescCount = 1;
        }
        st = EnableTraceEx2(g_session, &kDxgKrnlGuid, EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                            (UCHAR)g_dxgkLevel, g_dxgkKeyword, 0, 0,
                            g_dxgkFilter ? &params : nullptr);
        if (st != ERROR_SUCCESS) LogLine("WARN: EnableTraceEx2(DxgKrnl) failed (%lu)", st);
        else if (g_dxgkFilter) {
            char ids[64]; int w = 0;
            for (int i = 0; i < kDxgkWantN && w < (int)sizeof(ids) - 6; i++)
                w += snprintf(ids + w, sizeof(ids) - w, "%s%u", i ? "," : "",
                              (unsigned)kDxgkWantIds[i]);
            LogLine("DxgKrnl enabled: level %d keyword 0x%llX, event ids {%s}",
                    g_dxgkLevel, (unsigned long long)g_dxgkKeyword, ids);
        } else {
            LogLine("DxgKrnl enabled: level %d keyword 0x%llX, ALL event ids (census mode)",
                    g_dxgkLevel, (unsigned long long)g_dxgkKeyword);
        }
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
