// DdProbe - Phase 0 diagnostic for docs/dxgi-native-pipeline-spec.md.
//
// Stands up a DXGI Desktop Duplication session on one output and, per acquired frame, logs the
// present time DD reports (DXGI_OUTDUPL_FRAME_INFO::LastPresentTime) next to our own acquire QPC.
// It answers the two Phase-0 gates without building any of the pipeline:
//
//   1. COVERAGE: does DD keep the surface on the real source, or does it return
//      DXGI_ERROR_ACCESS_LOST under independent-flip / MPO (which fullscreen-borderless games
//      often take)? Every access-lost is logged; a game that never yields a frame answers the gate.
//   2. TIMELINE: is LastPresentTime a present-paced clock even when our acquire timing is bursty?
//      Compare lptdt (present-time delta) against acqdt (acquire delta). If the acquires jitter like
//      NvFBC's grabs but lptdt stays a clean cadence, LastPresentTime is the honest clock this whole
//      effort wants (and NvFBC's arr= receive-time is the corrupted one).
//
// No pixel processing: acquire, read the frame info, release. Console + DdProbe.log beside the exe.
// Usage: DdProbe.exe [outputIndex] [seconds]   (defaults: output 0, 30 s; run lists outputs first).

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cstdint>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

static FILE* g_log = nullptr;
static LONGLONG g_qpcFreq = 1;

static void LogLine(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fputs(buf, stdout);
    fputc('\n', stdout);
    if (g_log) { fputs(buf, g_log); fputc('\n', g_log); fflush(g_log); }
}

// QPC ticks -> microseconds (absolute, so a same-time relay run's arr= is on the same clock).
static long long UsFromQpc(LONGLONG ticks) {
    return (long long)((double)ticks * 1e6 / (double)g_qpcFreq);
}

// One (adapter, output) pair, flattened to a single global index the user selects.
struct OutputRef {
    IDXGIAdapter1* adapter;
    IDXGIOutput1*  output1;
    char           name[160];
};

// Enumerate every output on every adapter so the device is created on the SAME adapter that owns the
// chosen output (DuplicateOutput requires it; a default-adapter device fails on multi-GPU rigs).
static void EnumerateOutputs(IDXGIFactory1* factory, std::vector<OutputRef>* out) {
    IDXGIAdapter1* adapter = nullptr;
    for (UINT ai = 0; factory->EnumAdapters1(ai, &adapter) != DXGI_ERROR_NOT_FOUND; ai++) {
        DXGI_ADAPTER_DESC1 adesc = {};
        adapter->GetDesc1(&adesc);
        IDXGIOutput* output = nullptr;
        for (UINT oi = 0; adapter->EnumOutputs(oi, &output) != DXGI_ERROR_NOT_FOUND; oi++) {
            DXGI_OUTPUT_DESC odesc = {};
            output->GetDesc(&odesc);
            IDXGIOutput1* output1 = nullptr;
            if (SUCCEEDED(output->QueryInterface(__uuidof(IDXGIOutput1), (void**)&output1))) {
                OutputRef ref = {};
                adapter->AddRef();
                ref.adapter = adapter;
                ref.output1 = output1;
                snprintf(ref.name, sizeof(ref.name), "adapter %u (%ls) output %u (%ls) %ldx%ld",
                         ai, adesc.Description, oi, odesc.DeviceName,
                         odesc.DesktopCoordinates.right - odesc.DesktopCoordinates.left,
                         odesc.DesktopCoordinates.bottom - odesc.DesktopCoordinates.top);
                out->push_back(ref);
            }
            output->Release();
        }
        adapter->Release();
    }
}

int main(int argc, char** argv) {
    const int outputIndex = (argc > 1) ? atoi(argv[1]) : 0;
    const int seconds     = (argc > 2) ? atoi(argv[2]) : 30;

    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    g_qpcFreq = freq.QuadPart;
    g_log = fopen("DdProbe.log", "w");

    LogLine("=== DdProbe (DXGI Desktop Duplication timeline diagnostic) ===");

    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory))) {
        LogLine("FATAL: CreateDXGIFactory1 failed");
        return 1;
    }

    std::vector<OutputRef> outputs;
    EnumerateOutputs(factory, &outputs);
    LogLine("outputs found: %zu", outputs.size());
    for (size_t i = 0; i < outputs.size(); i++) LogLine("  [%zu] %s", i, outputs[i].name);

    if (outputs.empty() || outputIndex < 0 || outputIndex >= (int)outputs.size()) {
        LogLine("FATAL: output index %d out of range", outputIndex);
        return 1;
    }
    OutputRef& sel = outputs[outputIndex];
    LogLine("duplicating [%d] %s for %d s", outputIndex, sel.name, seconds);

    // Device on the chosen output's adapter (DuplicateOutput requirement).
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDevice(sel.adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0,
                                   D3D11_SDK_VERSION, &device, &fl, &context);
    if (FAILED(hr)) {
        LogLine("FATAL: D3D11CreateDevice failed (hr=0x%08lx)", (unsigned long)hr);
        return 1;
    }

    IDXGIOutputDuplication* dupl = nullptr;
    hr = sel.output1->DuplicateOutput(device, &dupl);
    if (FAILED(hr)) {
        // E_ACCESSDENIED here usually means the output is already being duplicated or is in an
        // exclusive-fullscreen mode DD cannot open.
        LogLine("FATAL: DuplicateOutput failed (hr=0x%08lx) - output busy or exclusive-fullscreen",
                (unsigned long)hr);
        return 1;
    }

    LARGE_INTEGER startQpc; QueryPerformanceCounter(&startQpc);
    const LONGLONG endTick = startQpc.QuadPart + (LONGLONG)seconds * g_qpcFreq;

    long long frames = 0, accessLost = 0, coalesced = 0, mouseOnly = 0;
    long long prevLpt = 0, prevAcq = 0;
    long long lptDtSum = 0, lptDtN = 0, acqDtSum = 0, acqDtN = 0;

    for (;;) {
        LARGE_INTEGER nowQ; QueryPerformanceCounter(&nowQ);
        if (nowQ.QuadPart >= endTick) break;

        DXGI_OUTDUPL_FRAME_INFO fi = {};
        IDXGIResource* res = nullptr;
        hr = dupl->AcquireNextFrame(1000, &fi, &res);

        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            continue;  // no new present within the window; fine on a paused source
        }
        if (hr == DXGI_ERROR_ACCESS_LOST) {
            // The gate that likely fails on a real game: independent-flip / MPO / mode change took
            // the surface. Try to re-acquire the duplication so we can see if it recovers.
            accessLost++;
            LogLine("ACCESS LOST (independent-flip / mode change) - re-duplicating (#%lld)", accessLost);
            if (dupl) { dupl->Release(); dupl = nullptr; }
            if (FAILED(sel.output1->DuplicateOutput(device, &dupl)) || !dupl) {
                LogLine("  re-duplicate failed - output not capturable right now; retrying");
                Sleep(200);
            }
            continue;
        }
        if (FAILED(hr)) {
            LogLine("AcquireNextFrame failed (hr=0x%08lx)", (unsigned long)hr);
            break;
        }

        LARGE_INTEGER acq; QueryPerformanceCounter(&acq);
        const long long lptUs = fi.LastPresentTime.QuadPart ? UsFromQpc(fi.LastPresentTime.QuadPart) : 0;
        const long long acqUs = UsFromQpc(acq.QuadPart);

        if (fi.LastPresentTime.QuadPart == 0) {
            mouseOnly++;  // pointer-only update, no new desktop frame
        } else {
            frames++;
            if (fi.AccumulatedFrames > 1) coalesced++;  // we missed some; only the latest is here
            const long long lptDt = prevLpt ? (lptUs - prevLpt) : 0;
            const long long acqDt = prevAcq ? (acqUs - prevAcq) : 0;
            if (prevLpt) { lptDtSum += lptDt; lptDtN++; }
            if (prevAcq) { acqDtSum += acqDt; acqDtN++; }
            LogLine("present #%lld lpt=%lldus lptdt=%lldus acq=%lldus acqdt=%lldus accum=%u",
                    frames, lptUs, lptDt, acqUs, acqDt, fi.AccumulatedFrames);
            prevLpt = lptUs;
            prevAcq = acqUs;
        }

        res->Release();
        dupl->ReleaseFrame();
    }

    LogLine("=== summary ===");
    LogLine("presents=%lld  mouse-only=%lld  coalesced(missed)=%lld  access-lost=%lld",
            frames, mouseOnly, coalesced, accessLost);
    if (lptDtN) LogLine("mean lptdt (present cadence) = %lldus (~%.1f fps)",
                        lptDtSum / lptDtN, 1e6 / ((double)lptDtSum / lptDtN));
    if (acqDtN) LogLine("mean acqdt (our acquire cadence) = %lldus", acqDtSum / acqDtN);
    LogLine("KEY: if lptdt is a clean cadence while acqdt jitters, LastPresentTime is the honest clock.");

    if (dupl) dupl->Release();
    if (context) context->Release();
    if (device) device->Release();
    for (auto& o : outputs) { o.output1->Release(); o.adapter->Release(); }
    factory->Release();
    if (g_log) fclose(g_log);
    return 0;
}
