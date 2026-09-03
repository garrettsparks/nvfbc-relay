/*!
 * \defgroup NvFBCR
 * \brief
 * Nvidia Framebuffer Capture Relay
 *
 * This is a simple application that captures display to a DirectX 9 surface,
 * sets up a pseudo-fullscreen window on another display,
 * and presents that surface on the window through the same DX9 context.
 *
 * It is very efficient because everything remains in VRAM and as few copies as possible are done to achieve this.
 * Many features and frills typical of capture software are omitted for speed and downstream flexibility.
 *
 * The process is as follows:
 *
 * Discover all physical heads on the system and ask the user to choose a capture and target display with a capture interval.
 * Create a window (in windowed mode) on the target display in pseudo-fullscreen, i.e., no borders or bar.
 * Create a DX9 context for that window and get a pointer to its single backbuffer as a DX9 surface (using immediate presentation).
 * Set up NvFBC capture device with the window's backbuffer as the target capture surface.
 * In a loop,
 *      Call NvFBC capture, thereby writing the captured frame directly to the window's backbuffer with no copies involved.
 *      Call Present.
 *
 *
 * Potential improvement is to use FLIPEX presentation model. Because this is a DX9 window not using FLIPEX presentation,
 *      a blt copy is performed as DWM manages the render. I couldn't get FLIPEX to work, primarily because
 *
 * https://learn.microsoft.com/en-us/windows/win32/direct3darticles/direct3d-9ex-improvements#direct3d-9ex-flip-mode-presentation
 * "If Windowed is set to TRUE and SwapEffect is set to D3DSWAPEFFECT_FLIPEX, the runtime creates one extra back buffer and rotates
 * whichever handle belongs to the buffer that becomes the front buffer at presentation time."
 *
 * This appears to be true - when modified with a backbuffer count of 2 as suggested in the FLIPEX documentation, every third frame
 * appears to be blank, which seems evidential of this extra back buffer. But I can't get a surface pointer to this buffer
 * through the dx9 device like I can for the buffers I explicitly requested on device creation.
 *
 * Stranger still, when I hold the NvFBC output buffer constant to one back buffer and just copy it to the other back buffer
 * using StretchRect on every frame, this works and does not skip every third frame, without obviously touching this third implicit back buffer.
 * But doing this copy defeats the purpose of using FLIPEX, which is to eliminate the extra copy.
 *
 */


#include <windows.h>

#include <iostream>
#include <assert.h>
#include <d3d9.h>
#include <vector>

#include <NvFBCLibrary.h>
#include <SimpleLogger.h>
#include <NvFBC/NvFBCToDx9vid.h>
#include <AdminCheck.h>

#include "IFrameCaptureMode.h"
#include "VsyncCaptureMode.h"
#include "TimerCaptureMode.h"
#include "DiagCaptureMode.h"
#include "TemporalCaptureMode.h"

using namespace std;


#define _CRT_SECURE_NO_WARNINGS  1

// DisplayPosition struct - defined early for use in capture modes
struct DisplayPosition {
    int dxAdapterIndex;
    RECT position;
    char deviceName[32];
    string friendlyName;
};

// Assumed source frame rate for the temporal lag (-src <fps>). The lag is static per run:
// max(present period, 1.25 x assumed source period); 0 means unset and the temporal modes
// assume sources run at 60 fps or faster. Declare slower sources (-src 30) to avoid
// after-frame starvation; declare faster ones (-src 240) to ride the present-period floor.
float g_srcRateHint = 0.0f;

// Opt in to the phase comb lock (-lock). Off by default: -src alone sizes the static lag
// (its established meaning) and leaves selection at v15 behavior; -lock adds the comb lock.
// Needs -src to derive the comb, so -lock without -src is inert.
bool g_lock = false;

// Burn the frame-counter marker into every present (-mark, debug builds/tests only):
// the exact video-to-log join key. See docs/frame-marker-spec.md.
bool g_mark = false;

// Stamp a coloured border on every synthesized (blended) frame (-tint, debug only) so
// blends are obvious while watching a capture at speed, without log-to-video alignment.
// Rides the blend shader's existing pass, so it costs no extra draw and leaves
// passthrough frames untouched.
bool g_tint = false;
// Consume the display driver's flip events alongside capture (-etw, debug). Diagnostic
// only: it adds flip lines to the log and nothing reads them. Off by default so the
// validated daily-driver path is untouched.
bool g_etw = false;
// -nojoin: keep the ETW session and the flip log, skip the per-present grid lookup. Exists
// so the join can be A/B'd inside ONE binary in ONE session: comparing against an older
// build confounds the join with everything else that changed, and the -etw off path logs no
// flips at all, so it cannot answer questions about the flip grid itself.
bool g_noJoin = false;
// -dejit: subtract each capture batch's measured delivery lateness from its ring stamps
// (needs -etw with the join on). The phantom-blend fix: a frame handed over late is stamped
// where its flip says it belongs, so the ring stops recording delivery delay as motion.
bool g_dejitter = false;
// -fgphase: the stage-7 gate instrument. Per capture batch, measure the CONTENT phase f of
// the generated member between its real neighbours (projection in downscaled luma, ring
// side); the DISPLAY phase g joins offline from the -etw flip lines. If f does not track g,
// generated frames are mistimed at the content level and no capture backend can fix it.
// Instrument runs are instrument runs: the readback stalls the capture thread each wake.
bool g_fgPhase = false;
// -phasekeep: phase-aware keep-real (needs -etw with the join on). Default keep-real retains
// member 1 of every batch, which is right wherever batch composition does not rotate. At x3
// it does - batch stride 2 against a 3-flip source period - so member 1 holds the real frame
// in only one class of three and the kept sequence runs gen, real, gen, gen, real, gen. This
// votes the rotation phase from arrival timing and keeps member 0 through the [real,gen]
// class, lifting real content from 2 of every 6 outputs to 4 of 6. Inert at x2 and FG off.
bool g_phaseKeep = false;
// -subgen: where blend mode would interpolate, present the driver-generated frame keep-real
// retracted instead, when one sits on the target and its pixels are not a capture-race copy
// of a real frame. Sharp where a blend doubles edges. Inert wherever nothing is retracted
// (frame generation off, a source that pairs nothing), and inert outside blend modes.
bool g_subGen = false;
// -diffmap: DIAGNOSTIC. Ask NvFBC for its own per-block difference map with each grab and
// log how many blocks it reports changed (diff= on the capture line). Decides nothing. It
// exists to test whether the driver already knows what the generated-frame content check
// re-derives with a GPU readback: a capture-race duplicate is a grab that returned the same
// content as the previous one, which is an all-zero map. Run with -fgphase, whose per-batch
// gdiff is the ground truth to join against.
bool g_diffMap = false;
// -lag N: extra bracketing delay in ms. Trades output latency, which the player never sees
// (the source display is direct) and which only shifts an already-delayed stream, for holds.
unsigned int g_extraLagMs = 0;

// Optional count for -mark: burn only the first N presents (a head burst that aligns a
// stream VOD without marking watched gameplay), then run clean. 0 = every present (the
// bare -mark). The counter keeps advancing past N so mark= stays a continuous present count.
unsigned int g_markFrames = 0;

// Interp compositor backend for o:* modes (-interp flow|fruc). Flow (raw NVOFA + our
// warp) is the default: the only runtime dependency is the driver's nvofapi64.dll,
// while FRUC needs the SDK's NvOFFRUC.dll beside the exe.
int g_interpBackend = 1;

// Single fps validation policy for every entry point that accepts a rate (mode strings,
// -src): accept (0, 1000].
static bool ParseFps(const string& value, float* outFps) {
    try {
        float v = stof(value);
        if (v > 0.0f && v <= 1000.0f) { *outFps = v; return true; }
    }
    catch (...) {}
    return false;
}

// Helper function to parse capture mode string and create appropriate mode instance
IFrameCaptureMode* ParseCaptureMode(const string& modeStr) {
    if (modeStr.empty() || _stricmp(modeStr.c_str(), "vsync") == 0) {
        // Default to vsync mode
        return new VsyncCaptureMode();
    }

    // Temporal modes (t = nearest selection, b = blend compositor, o = optical-flow
    // interp compositor), vsync present (t:vsync / b:vsync / o:vsync or the bare
    // letter): CaptureRing-based temporal mode, present blocked on DWM's compose clock
    // (windowed INTERVAL_ONE; card-locked 60 Hz under a fullscreen game on the source —
    // the production case). Nominal 60 fps drives the bracketing lag; the actual
    // present rate is DWM's delivery.
    {
        char c0 = modeStr[0];
        if (c0 >= 'A' && c0 <= 'Z') c0 = (char)(c0 - 'A' + 'a');
        CompositorKind kind = kCompositorNearest;
        if (c0 == 'b') kind = kCompositorBlend;
        else if (c0 == 'o') kind = kCompositorInterp;
        if (_stricmp(modeStr.c_str(), "t") == 0 || _stricmp(modeStr.c_str(), "t:vsync") == 0 ||
            _stricmp(modeStr.c_str(), "b") == 0 || _stricmp(modeStr.c_str(), "b:vsync") == 0 ||
            _stricmp(modeStr.c_str(), "o") == 0 || _stricmp(modeStr.c_str(), "o:vsync") == 0) {
            return new TemporalCaptureMode(60.0f, /*vsyncPresent=*/true, g_srcRateHint, g_lock,
                                           kind, g_mark, g_markFrames, g_tint, g_etw, g_noJoin,
                                           g_dejitter, g_fgPhase, g_phaseKeep, g_subGen,
                                           g_diffMap, g_extraLagMs);
        }

        // D3D11 flip-model present (b:flip): the blend compositor decided and drawn on a D3D11
        // device onto a DXGI flip-model swapchain, paced by that swapchain's vsync present so
        // that a promotion to independent flip puts the present on the TARGET display's own
        // vblank rather than DWM's compose clock. Blend only: nearest and interp are not ported.
        if (modeStr.length() > 2 && modeStr[1] == ':' &&
            _stricmp(modeStr.c_str() + 2, "flip") == 0) {
            if (c0 != 'b') {
                LOGERR("Invalid capture mode: '%s' (the D3D11 flip present carries the blend "
                       "compositor only; use b:flip)", modeStr.c_str());
                return NULL;
            }
            return new TemporalCaptureMode(60.0f, /*vsyncPresent=*/true, g_srcRateHint, g_lock,
                                           kind, g_mark, g_markFrames, g_tint, g_etw, g_noJoin,
                                           g_dejitter, g_fgPhase, g_phaseKeep, g_subGen,
                                           g_diffMap, g_extraLagMs, /*d3d11Present=*/true);
        }

        // QPC-timer present (t:60 / b:60 / o:60 format).
        if (modeStr.length() > 2 && (c0 == 't' || c0 == 'b' || c0 == 'o') && modeStr[1] == ':') {
            float framerate;
            if (ParseFps(modeStr.substr(2), &framerate)) {
                return new TemporalCaptureMode(framerate, /*vsyncPresent=*/false, g_srcRateHint, g_lock,
                                               kind, g_mark, g_markFrames, g_tint, g_etw, g_noJoin,
                                               g_dejitter, g_fgPhase, g_phaseKeep, g_subGen,
                                           g_diffMap, g_extraLagMs);
            }
        }
    }

    // Diagnostic clock probes:
    //   diag        — QPC 60Hz + IMMEDIATE; logs DWM compose timing + card raster per tick
    //   diag:vsync  — INTERVAL_ONE; present block time measures DWM's delivery cadence
    if (_stricmp(modeStr.c_str(), "diag") == 0) {
        return new DiagCaptureMode(/*vsyncPresent=*/false);
    }
    if (_stricmp(modeStr.c_str(), "diag:vsync") == 0) {
        return new DiagCaptureMode(/*vsyncPresent=*/true);
    }

    // Try to parse as numeric framerate
    {
        float framerate;
        if (ParseFps(modeStr, &framerate)) {
            return new TimerCaptureMode(framerate);
        }
    }

    LOGERR("Invalid capture mode: '%s'", modeStr.c_str());
    LOGERR("Valid modes:");
    LOGERR("  vsync          - VSync-driven presentation");
    LOGERR("  t, t:vsync     - Temporal frame selection, presented on vsync (DWM compose clock)");
    LOGERR("  t:59.94        - Temporal frame selection, presented on a timer at given fps");
    LOGERR("  b, b:vsync, b:60 - Temporal blend compositor (sharp passthrough at the target, lerp otherwise)");
    LOGERR("  b:flip         - Temporal blend compositor presented through a D3D11 flip-model swapchain (independent-flip candidate)");
    LOGERR("  o, o:vsync, o:60 - Temporal interp compositor (NVOFA motion-compensated synthesis)");
    LOGERR("  diag, diag:vsync - Clock probes (DWM compose timing + card raster; vsync variant measures DWM delivery)");
    LOGERR("  60             - Timer mode (simple timer-driven at specified fps)");
    LOGERR("Options:");
    LOGERR("  -src 30        - Declared source fps; sizes the static temporal lag (default: assume >= 60)");
    LOGERR("  -lock          - Enable the phase comb lock (needs -src for the rate); off by default");
    LOGERR("  -interp flow|fruc - o:* synthesis engine (default flow = raw NVOFA + warp)");
    LOGERR("  -mark [N]      - Burn the frame-counter marker (video-to-log alignment, debug); off by default. N = first N presents only (stream head anchor), else every present");
    LOGERR("  -tint          - Border-tint synthesized frames red (blend mode, debug); off by default");
    LOGERR("  -etw           - Log the display driver's true scanout times alongside capture (debug); off by default");
    LOGERR("  -nojoin        - With -etw: log flips but skip the per-present grid lookup (debug A/B control)");
    LOGERR("  -dejit         - With -etw: re-stamp late-delivered capture batches onto the flip grid (phantom-blend fix)");
    LOGERR("  -fgphase       - Content-phase instrument: log per-batch f of generated frames (stage-7 gate; run with -etw for the offline g join)");
    LOGERR("  -phasekeep     - With -etw: phase-aware keep-real, so x3 keeps the real frame in every batch that has one (inert at x2)");
    return NULL;
}

// ===============================================


// DirectX resources
IDirect3D9Ex        *g_pD3DEx = NULL;
IDirect3DDevice9Ex  *g_pD3D9Device = NULL;
IDirect3DSurface9* g_backbuffer = NULL;

bool g_bNvFBCLibLoaded = false;

NvFBCToDx9Vid *NvFBCDX9 = NULL;
NvFBCLibrary *pNVFBCLib;

int BUF_WIDTH;
int BUF_HEIGHT;

DisplayPosition source, target;

// Target display's D3D9 adapter ordinal (set once displays are chosen). DiagCaptureMode uses it
// to open a private device on the capture-card adapter for GetRasterStatus probes.
int g_targetAdapterIndex = 0;

// Source display's D3D9 adapter ordinal. CaptureRing pins its capture device here EXPLICITLY
// rather than inheriting the present device's ordinal, because the present device may sit on
// the target adapter and NvFBC must capture the SOURCE display.
int g_sourceAdapterIndex = 0;

// Flip-mode presentation (-flipex): D3DSWAPEFFECT_FLIPEX instead of the bitblt DISCARD. Opt-in
// because it changes how every frame reaches DWM; see the swap-chain setup for why it is wanted
// and why the previous attempt failed. Off leaves presentation byte-identical to today.
bool g_flipEx = false;

// Hidden window hosting the D3D9 devices when the output window belongs to a D3D11 flip-model
// swapchain (b:flip). Flip model allows one swapchain per window and no second API on it, and a
// D3D9 device cannot exist without a device window, so the present and capture devices move
// here and the D3D9 swapchain never presents. NULL on every other path, where the D3D9 present
// device owns the output window as it always has.
HWND g_d3d9HostWnd = NULL;

// Target display's refresh rate in Hz, 0 when it could not be read. This is the SINK rate: what
// the capture card can actually show, which is not the rate we present at (a vsync present rides
// DWM's compose clock, and a timer present rides -framerate). The composite tooth guard arms off
// this, because whether interpolating between source frames buys anything is a question about the
// sink: at or below the source rate every extra frame is discarded, above it they are shown.
int g_targetRefreshHz = 0;

vector <DisplayPosition> displays;

void Cleanup()
{
    LOG("Cleanup started");

    //! Release the NvFBCDX9 instance
    if (NvFBCDX9)
    {
        NvFBCDX9->NvFBCToDx9VidRelease();
        NvFBCDX9 = NULL;
    }

    if (g_backbuffer) {
        g_backbuffer->Release();
        g_backbuffer = NULL;
    }

    if (g_pD3D9Device)
    {
        g_pD3D9Device->Release();
        g_pD3D9Device = NULL;
    }

    if (g_pD3DEx)
    {
        g_pD3DEx->Release();
        g_pD3DEx = NULL;
    }

    if (g_bNvFBCLibLoaded)
    {
        pNVFBCLib->close();
    }

    if (pNVFBCLib)
    {
        delete pNVFBCLib;
        pNVFBCLib = NULL;
    }

    LOG("Cleanup completed");
    SimpleLogger::getInstance().flush();
}

int InitDisplays() {

    Direct3DCreate9Ex(D3D_SDK_VERSION, &g_pD3DEx);
    int adapterCount = static_cast<int>(g_pD3DEx->GetAdapterCount());

    for (int i = 0; i < adapterCount; i++) {
        MONITORINFOEX mi;
        ZeroMemory(&mi, sizeof(mi));
        mi.cbSize = sizeof(mi);
        GetMonitorInfo(g_pD3DEx->GetAdapterMonitor(i), &mi);

        DisplayPosition newMon = {
                    i,
                    mi.rcMonitor
        };
        strcpy(newMon.deviceName, mi.szDevice);
        displays.push_back(newMon);

    }


    vector<DISPLAYCONFIG_PATH_INFO> paths;
    vector<DISPLAYCONFIG_MODE_INFO> modes;
    UINT32 flags = QDC_ONLY_ACTIVE_PATHS | QDC_VIRTUAL_MODE_AWARE;
    LONG result = ERROR_SUCCESS;

    do
    {
        // Determine how many path and mode structures to allocate
        UINT32 pathCount, modeCount;
        result = GetDisplayConfigBufferSizes(flags, &pathCount, &modeCount);

        if (result != ERROR_SUCCESS)
        {
            LOGERR("GetDisplayConfigBufferSizes failed (error: 0x%08x)", result);
            return HRESULT_FROM_WIN32(result);
        }

        // Allocate the path and mode arrays
        paths.resize(pathCount);
        modes.resize(modeCount);

        // Get all active paths and their modes
        result = QueryDisplayConfig(flags, &pathCount, paths.data(), &modeCount, modes.data(), nullptr);

        // The function may have returned fewer paths/modes than estimated
        paths.resize(pathCount);
        modes.resize(modeCount);

        // It's possible that between the call to GetDisplayConfigBufferSizes and QueryDisplayConfig
        // that the display state changed, so loop on the case of ERROR_INSUFFICIENT_BUFFER.
    } while (result == ERROR_INSUFFICIENT_BUFFER);

    if (result != ERROR_SUCCESS)
    {
        LOGERR("QueryDisplayConfig failed (error: 0x%08x)", result);
        return HRESULT_FROM_WIN32(result);
    }

    // For each active path
    for (int i = 0; i < paths.size(); i++)
    {
        // Find the target (monitor) friendly name
        DISPLAYCONFIG_TARGET_DEVICE_NAME targetName = {};
        targetName.header.adapterId = paths[i].targetInfo.adapterId;
        targetName.header.id = paths[i].targetInfo.id;
        targetName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        targetName.header.size = sizeof(targetName);
        result = DisplayConfigGetDeviceInfo(&targetName.header);

        if (result != ERROR_SUCCESS)
        {
            return HRESULT_FROM_WIN32(result);
        }

        char devNameStr[64];
        char DefChar = ' ';
        WideCharToMultiByte(CP_ACP, 0, targetName.monitorFriendlyDeviceName, -1, devNameStr, 64, &DefChar, NULL);

        displays[i].friendlyName = string(devNameStr);
    }
    return 1;
}

HRESULT InitD3D9(unsigned int deviceID, HWND hwnd, UINT presentationInterval)
{
    HRESULT hr = S_OK;
    D3DPRESENT_PARAMETERS d3dpp;

    // Create the Direct3D9 device and the swap chain.
    ZeroMemory(&d3dpp, sizeof(d3dpp));

    d3dpp.Windowed = TRUE;
    d3dpp.BackBufferFormat = D3DFMT_A2R10G10B10;
    d3dpp.BackBufferWidth  = BUF_WIDTH;
    d3dpp.BackBufferHeight = BUF_HEIGHT;
    // FLIP MODE (-flipex): the bitblt swap effect copies the back buffer into DWM's
    // redirection surface on every present - a full-frame read+write the relay pays
    // unconditionally - and it makes present statistics unavailable (a bitblt windowed
    // swapchain reports all zeroes, which is why sink timing has had to be reconstructed
    // from ETW). Flip mode shares the buffers with DWM instead: no copy, real present
    // statistics, and it is the precondition for DWM promoting the window to independent
    // flip, which would pace presents on the CARD's vblank rather than the compose clock.
    //
    // FLIPEX was tried once before and abandoned because "every third frame appears blank".
    // That was this code's own bug, not a platform limit: the runtime "rotates whichever
    // handle belongs to the buffer that becomes the front buffer at presentation time", and
    // the back buffer was fetched ONCE at startup and cached forever, so with 2 requested
    // buffers plus the implicit extra, two of every three presents composited into a surface
    // that was not the current back buffer. The present loop now re-acquires the back buffer
    // every present, which is what flip mode requires.
    //
    // FLIPEX ALSO CONSTRAINS THE FORMAT. Bitblt tolerates a back buffer that does not match
    // the display mode by converting on every present - which this relay has silently been
    // doing forever: with A2R10G10B10 against an 8-bit mode, PresentEx returns
    // S_PRESENT_MODE_CHANGED on EVERY present (27254 times in one 259 s capture), whose
    // documented advice is "pick a back buffer format similar to the current display mode".
    // Flip mode hands the buffers to DWM directly, so there is nowhere to hide a conversion
    // and CreateDeviceEx refuses the mismatch outright. The ring stays 10-bit either way;
    // only this final hop follows the sink, which discards the extra bits regardless.
    //
    // The mode is logged on EVERY run, not just flip mode: whether the back buffer matches the
    // adapter it is presented on is the difference between a clean present and a converted one,
    // and it was invisible for the whole life of this relay. Put the comparison in the log so a
    // mismatch is read rather than deduced.
    {
        D3DDISPLAYMODEEX mode;
        ZeroMemory(&mode, sizeof(mode));
        mode.Size = sizeof(mode);
        if (SUCCEEDED(g_pD3DEx->GetAdapterDisplayModeEx(deviceID, &mode, NULL))) {
            // Decided BEFORE the log, so the line describes the format actually used. Flip
            // mode cannot convert - it hands the buffers to DWM - so it must follow the mode
            // exactly or CreateDeviceEx refuses outright. Bitblt keeps its historical format:
            // changing that is a deliberate decision about the shipping path, not something to
            // bundle into an adapter move.
            if (g_flipEx) d3dpp.BackBufferFormat = mode.Format;
            const bool matches = (mode.Format == d3dpp.BackBufferFormat) &&
                                 (mode.Width == (UINT)BUF_WIDTH) &&
                                 (mode.Height == (UINT)BUF_HEIGHT);
            LOG("Display mode on adapter %u: %ux%u @%uHz format %d; back buffer %dx%d format %d "
                "-> %s", deviceID, mode.Width, mode.Height, mode.RefreshRate, (int)mode.Format,
                BUF_WIDTH, BUF_HEIGHT, (int)d3dpp.BackBufferFormat,
                matches ? "MATCH"
                        : "MISMATCH (expect S_PRESENT_MODE_CHANGED and a per-present convert)");
        } else {
            LOGERR("GetAdapterDisplayModeEx failed on adapter %u; cannot tell whether the back "
                   "buffer matches the display mode%s", deviceID,
                   g_flipEx ? " and flip mode will probably be refused" : "");
        }
    }
    d3dpp.BackBufferCount = g_flipEx ? 2 : 1;
    d3dpp.SwapEffect = g_flipEx ? D3DSWAPEFFECT_FLIPEX : D3DSWAPEFFECT_DISCARD;
    d3dpp.PresentationInterval = presentationInterval;
    d3dpp.hDeviceWindow = hwnd;
    // D3DCREATE_MULTITHREADED: the temporal modes drive capture on a separate thread from
    // present, so D3D9 device calls (StretchRect/Present) come from two threads. This flag
    // makes the D3D9 runtime serialize them safely. It only affects this process's own
    // device, not the captured game's rendering.
    DWORD dwBehaviorFlags = D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED;
    // Present statistics are not gathered unless the device asks for them at creation, and
    // the failure is silent: GetPresentStats would keep returning zeroes and read as "the
    // sink never missed a refresh", which is the most misleading answer available.
    if (g_flipEx) dwBehaviorFlags |= D3DCREATE_ENABLE_PRESENTSTATS;

    hr = g_pD3DEx->CreateDeviceEx(
        deviceID,
        D3DDEVTYPE_HAL,
        hwnd,
        dwBehaviorFlags,
        &d3dpp,
        NULL,
        &g_pD3D9Device);

    // The HRESULT used to be swallowed by an assert, which NDEBUG compiles out - so a refused
    // device produced "Unable to create D3D9Ex Device" and nothing else, and the reason had to
    // be guessed. Print the code and the parameters that can plausibly cause a refusal.
    if (FAILED(hr)) {
        LOGERR("CreateDeviceEx failed (0x%08lx): %dx%d fmt %d, swap effect %d, %u back buffers, "
               "interval 0x%08x, behavior 0x%08lx", (unsigned long)hr, BUF_WIDTH, BUF_HEIGHT,
               (int)d3dpp.BackBufferFormat, (int)d3dpp.SwapEffect, d3dpp.BackBufferCount,
               presentationInterval, (unsigned long)dwBehaviorFlags);
    }

    // FRAME LATENCY: how many presents may be QUEUED before PresentEx blocks. D3D9Ex defaults
    // to 3, and under bitblt with a single back buffer that never mattered - the copy into
    // DWM's redirection surface is synchronous enough that the present is the pacing wait.
    // Flip mode queues instead of copying, so with the default latency the loop can run three
    // frames ahead of the display and PresentEx stops being backpressure at all: measured, the
    // present rate rose from ~118/s to 155-180/s and the spacing jitter roughly tripled, which
    // is the opposite of what flip mode was adopted for.
    //
    // 1 means "block until the previous frame has been consumed", restoring the phase-locking
    // backpressure that makes the vsync present a clock rather than a queue push.
    if (SUCCEEDED(hr) && g_flipEx && g_pD3D9Device) {
        const HRESULT lat = g_pD3D9Device->SetMaximumFrameLatency(1);
        if (FAILED(lat)) {
            LOGERR("SetMaximumFrameLatency(1) failed (0x%08lx); flip mode will queue up to the "
                   "driver default and present pacing will not be trustworthy",
                   (unsigned long)lat);
        } else {
            UINT got = 0;
            g_pD3D9Device->GetMaximumFrameLatency(&got);
            LOG("Frame latency set to 1 for flip mode (device reports %u): PresentEx blocks "
                "until the previous frame is consumed", got);
        }
    }

    // A refused FLIP MODE fails the run. It deliberately does NOT fall back to bitblt: the
    // capture would then be a bitblt run wearing a flipex file name, and the file name outlives
    // the log. The same rule the ETW session follows, for the same reason - "asking for -etw
    // and silently getting a normal capture has cost a session once already" - except that a
    // present-path substitution invalidates every number in the run rather than one instrument,
    // so this aborts instead of warning and continuing.
    if (FAILED(hr) && g_flipEx) {
        LOGERR("flip mode REFUSED (0x%08lx). NOT falling back to bitblt: a run labelled flipex "
               "that silently presented through the old path would be worse than no run.",
               (unsigned long)hr);
        MessageBoxA(NULL,
                    "-flipex was requested but the device could not be created.\n\n"
                    "The relay will NOT start. It deliberately does not fall back to the "
                    "normal present path, because the capture would then be mislabelled.\n\n"
                    "Most likely cause: the back buffer format does not match the display "
                    "mode. See NvFBCR.log for the HRESULT and the parameters tried.",
                    "NvFBCR: flip mode unavailable",
                    MB_OK | MB_ICONERROR | MB_SETFOREGROUND | MB_TOPMOST);
    }

    return hr;
}

HRESULT InitD3D9Surfaces()
{
    HRESULT hr = E_FAIL;

    if (g_pD3D9Device)
    {

        hr = g_pD3D9Device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &g_backbuffer);
        if (FAILED(hr))
        {
            LOGERR("Failed to get backbuffer surface (error: 0x%08x)", hr);
        }
    }

    return hr;
}

// this is the main message handler for the program
LRESULT CALLBACK WindowProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_DESTROY:
    {
        PostQuitMessage(0);
        return 0;
    } break;
    }

    return DefWindowProc(hWnd, message, wParam, lParam);
}

int ReadIntFromCmd(string prompt) {
    cout << prompt;
    string cinString;
    getline(cin, cinString);
    return cinString.empty() ? -1 : stoi(cinString);
}

// Whitespace tokenizer shared by the command line and the console prompt, so both paths
// always split options identically.
static vector<string> SplitTokens(const string& text) {
    vector<string> tokens;
    size_t pos = 0;
    while (pos < text.length()) {
        while (pos < text.length() && text[pos] == ' ') pos++;
        size_t start = pos;
        while (pos < text.length() && text[pos] != ' ') pos++;
        if (pos > start) tokens.push_back(text.substr(start, pos - start));
    }
    return tokens;
}

// Option dispatch shared by the command line and the console prompt: applies the option at
// tokens[i] and returns how many tokens it consumed (0 = not a recognized option). New
// value-taking options belong here so both entry points accept them.
static size_t ApplyOption(const vector<string>& tokens, size_t i) {
    if (tokens[i] == "-lock") {
        g_lock = true;
        return 1;
    }
    if (tokens[i] == "-tint") {
        g_tint = true;
        return 1;
    }
    if (tokens[i] == "-etw") {
        g_etw = true;
        return 1;
    }
    if (tokens[i] == "-nojoin") {
        g_noJoin = true;
        return 1;
    }
    if (tokens[i] == "-dejit") {
        g_dejitter = true;
        return 1;
    }
    if (tokens[i] == "-fgphase") {
        g_fgPhase = true;
        return 1;
    }
    if (tokens[i] == "-phasekeep") {
        g_phaseKeep = true;
        return 1;
    }
    if (tokens[i] == "-subgen") {
        g_subGen = true;
        return 1;
    }
    if (tokens[i] == "-diffmap") {
        g_diffMap = true;
        return 1;
    }
    if (tokens[i] == "-flipex") {
        g_flipEx = true;
        return 1;
    }
    if (tokens[i] == "-mark") {
        g_mark = true;
        // Optional frame count: consume the next token as N only if it is all digits, so
        // a bare -mark (or -mark followed by another flag) keeps marking every present.
        if (i + 1 < tokens.size() && !tokens[i + 1].empty() &&
            tokens[i + 1].find_first_not_of("0123456789") == string::npos) {
            g_markFrames = (unsigned int)strtoul(tokens[i + 1].c_str(), NULL, 10);
            return 2;
        }
        return 1;
    }
    if (tokens[i] == "-lag" && i + 1 < tokens.size()) {
        const long v = strtol(tokens[i + 1].c_str(), NULL, 10);
        if (v >= 0 && v <= 200) g_extraLagMs = (unsigned int)v;
        else LOGERR("-lag value '%s' invalid (0-200 ms) - ignored", tokens[i + 1].c_str());
        return 2;
    }
    if (tokens[i] == "-src" && i + 1 < tokens.size()) {
        float v;
        if (ParseFps(tokens[i + 1], &v)) g_srcRateHint = v;
        else LOGERR("-src value '%s' invalid (1-1000) - ignored", tokens[i + 1].c_str());
        return 2;
    }
    if (tokens[i] == "-interp" && i + 1 < tokens.size()) {
        if (tokens[i + 1] == "flow")      g_interpBackend = 1;
        else if (tokens[i + 1] == "fruc") g_interpBackend = 0;
        else LOGERR("-interp value '%s' invalid (flow|fruc) - keeping default", tokens[i + 1].c_str());
        return 2;
    }
    return 0;
}

bool ParseCommandLineArgs(LPSTR lpCmdLine, int* sourceIndex, int* targetIndex, string* framerateStr) {
    *sourceIndex = -1;
    *targetIndex = -1;
    *framerateStr = "";

    if (!lpCmdLine || strlen(lpCmdLine) == 0) {
        return false;
    }

    vector<string> args = SplitTokens(string(lpCmdLine));

    bool foundAny = false;

    // Parse arguments
    for (size_t i = 0; i < args.size(); i++) {
        if (args[i] == "-source" && i + 1 < args.size()) {
            *sourceIndex = stoi(args[i + 1]);
            foundAny = true;
            i++; // Skip the value
        }
        else if (args[i] == "-target" && i + 1 < args.size()) {
            *targetIndex = stoi(args[i + 1]);
            foundAny = true;
            i++; // Skip the value
        }
        else if (args[i] == "-framerate" && i + 1 < args.size()) {
            *framerateStr = args[i + 1];  // Store as string instead of converting to int
            foundAny = true;
            i++; // Skip the value
        }
        else {
            size_t consumed = ApplyOption(args, i);
            if (consumed > 0) {
                foundAny = true;
                i += consumed - 1;
            }
        }
    }

    return foundAny;
}

void ConsoleUserInput(string* framerateStr) {
    AllocConsole();
    FILE* fDummy;
    freopen_s(&fDummy, "CONOUT$", "w", stdout);
    freopen_s(&fDummy, "CONIN$", "r", stdin);
    cout.clear();
    clog.clear();
    cin.clear();
    cout << endl;
    for (vector<DisplayPosition>::iterator iter = displays.begin(); iter < displays.end(); iter++) {

        cout << "Adapter index [" << iter->dxAdapterIndex << "]"
            << endl << "\t"
            << "Scaled Position Top Left [" << iter->position.left << "," << iter->position.top << "]"
            << " | Scaled Position Bottom Right [" << iter->position.right << "," << iter->position.bottom << "]"
            << endl << "\t"
            << "Identifier [" << iter->deviceName << "]"
            << endl << "\t"
            << "Name [" << iter->friendlyName << "]"
            << endl;
    }

    int sourceIndex;
    int outputIndex;
    for (sourceIndex = ReadIntFromCmd("Capture Display Index ? "); sourceIndex < 0 || sourceIndex > displays.size() - 1;) {
        sourceIndex = ReadIntFromCmd("Capture Display Index ? ");
    }
    for (outputIndex = ReadIntFromCmd("Output Display Index ? "); outputIndex < 0 || outputIndex > displays.size() - 1;) {
        outputIndex = ReadIntFromCmd("Output Display Index ? ");
    }

    cout << endl << "Available capture modes:" << endl;
    cout << "  vsync          - VSync-driven presentation (matches target display refresh)" << endl;
    cout << endl;
    cout << "  t, t:vsync     - Temporal frame selection, presented on vsync (DWM compose clock)" << endl;
    cout << "  t:59.94        - Temporal frame selection, presented on a timer at given fps" << endl;
    cout << "  b, b:vsync, b:60 - Temporal blend compositor (sharp passthrough at the target, lerp otherwise)" << endl;
    cout << "  b:flip         - Temporal blend compositor on a D3D11 flip-model swapchain (independent-flip candidate)" << endl;
    cout << "  o, o:vsync, o:60 - Temporal interp compositor (NVOFA motion-compensated synthesis)" << endl;
    cout << "  t:60 -src 30   - Mode plus options: -src <fps> declares the source rate (lag sizing)" << endl;
    cout << "  -lock          - Enable the phase comb lock (needs -src; off by default)" << endl;
    cout << "  -interp flow|fruc - o:* synthesis engine (default flow = raw NVOFA + warp)" << endl;
    cout << "  -mark [N]      - Burn the frame-counter marker (video-to-log alignment, debug); N = first N presents only" << endl;
    cout << "  -tint          - Border-tint synthesized frames red (blend mode, debug)" << endl;
    cout << "  -flipex        - Flip-mode presentation (no DWM copy, present statistics; experimental)" << endl;
    cout << endl;
    cout << "  diag, diag:vsync - Clock probes (DWM compose timing + card raster)" << endl;
    cout << endl;
    cout << "  60             - Timer mode (simple timer-driven at specified fps)" << endl;
    cout << endl;
    cout << "Capture/Present framerate (blank for vsync) ? ";
    string cinString;
    getline(cin, cinString);
    if (!cinString.empty()) {
        // The prompt is the usual launch path, so the mode may carry options ("t:60 -src 30").
        // Display indices were already chosen interactively; everything after the mode token
        // goes through the same option dispatch as the command line.
        size_t space = cinString.find(' ');
        if (space != string::npos) {
            vector<string> opts = SplitTokens(cinString.substr(space));
            for (size_t i = 0; i < opts.size(); i++) {
                size_t consumed = ApplyOption(opts, i);
                if (consumed > 0) i += consumed - 1;
                else cout << "Unknown option '" << opts[i] << "' - ignored" << endl;
            }
            if (g_srcRateHint > 0.0f) cout << "Declared source rate: " << g_srcRateHint << " fps"
                << (g_lock ? " (comb lock on)" : " (comb lock off)") << endl;
            if (g_tint) cout << "Blend tint: on (synthesized frames bordered)" << endl;
            if (g_mark && g_markFrames) cout << "Frame marker: on (first " << g_markFrames << " presents)" << endl;
            else if (g_mark)            cout << "Frame marker: on" << endl;
            cinString = cinString.substr(0, space);
        }
        *framerateStr = cinString;
    }

    for (vector<DisplayPosition>::iterator iter = displays.begin(); iter < displays.end(); iter++) {

        if (iter->dxAdapterIndex == outputIndex) {
            target = *iter;
        }
        if (iter->dxAdapterIndex == sourceIndex) {
            source = *iter;
        }
    }

    fclose(stdin);
    fclose(stdout);
    FreeConsole();
}

// the entry point for any Windows program
_Use_decl_annotations_ int WINAPI WinMain(HINSTANCE hInstance,
    HINSTANCE hPrevInstance,
    LPSTR lpCmdLine,
    int nCmdShow)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    if (!InitDisplays()) {
        LOGERR("Unable to enumerate display adapters");
        return -1;
    }

    // Try to parse command line arguments
    LOG("Command line received: '%s'", lpCmdLine ? lpCmdLine : "(null)");
    int argSourceIndex = -1;
    int argTargetIndex = -1;
    string framerateStr = "";
    bool hasArgs = ParseCommandLineArgs(lpCmdLine, &argSourceIndex, &argTargetIndex, &framerateStr);
    LOG("Parsed args - hasArgs: %d, source: %d, target: %d, framerate: %s", hasArgs, argSourceIndex, argTargetIndex, framerateStr.c_str());

    // If all required args are provided via command line, use them
    if (hasArgs && argSourceIndex >= 0 && argTargetIndex >= 0 &&
        argSourceIndex < static_cast<int>(displays.size()) && argTargetIndex < static_cast<int>(displays.size())) {
        // Set source and target from command line args
        for (vector<DisplayPosition>::iterator iter = displays.begin(); iter < displays.end(); iter++) {
            if (iter->dxAdapterIndex == argTargetIndex) {
                target = *iter;
            }
            if (iter->dxAdapterIndex == argSourceIndex) {
                source = *iter;
            }
        }
    } else {
        // Fall back to interactive console input
        ConsoleUserInput(&framerateStr);
    }

    // The substitution wants the driver's change map: it is what tells a generated frame
    // from a capture-race duplicate of the frame beside it. Measured against the pixel
    // instrument over 65619 batches it caught 98.80% of duplicates with zero false
    // positives, and it costs a comparison on a value the capture loop already has rather
    // than a GPU pipeline sync on the present thread. Implied rather than a second flag to
    // remember, and harmless where the driver refuses it: the compositor's own content
    // check still covers that case.
    //
    // MUST be resolved before ParseCaptureMode, which passes these flags to the mode's
    // constructor - setting it afterwards would log the intent and change nothing.
    if (g_subGen && !g_diffMap) {
        g_diffMap = true;
        LOG("-subgen implies -diffmap: the change map is the content check");
    }

    // Create capture mode instance
    IFrameCaptureMode* captureMode = ParseCaptureMode(framerateStr);
    if (!captureMode) {
        LOGERR("Failed to create capture mode");
        Cleanup();
        return -1;
    }
    // The D3D9 swapchain never presents on the D3D11 path, so its swap effect is moot, and a
    // FLIPEX device on the hidden host window would only add a way for creation to fail.
    if (captureMode->PresentsViaD3D11() && g_flipEx) {
        LOG("-flipex ignored: b:flip presents through its own D3D11 swapchain");
        g_flipEx = false;
    }

    LOG("=== NvFBCR Starting ===");
    LOG("Source display: [%d] %s (%s)", source.dxAdapterIndex, source.friendlyName.c_str(), source.deviceName);
    LOG("Target display: [%d] %s (%s)", target.dxAdapterIndex, target.friendlyName.c_str(), target.deviceName);
    g_targetAdapterIndex = target.dxAdapterIndex;
    g_sourceAdapterIndex = source.dxAdapterIndex;
    {
        DEVMODEA dm;
        ZeroMemory(&dm, sizeof(dm));
        dm.dmSize = sizeof(dm);
        if (EnumDisplaySettingsA(target.deviceName, ENUM_CURRENT_SETTINGS, &dm) &&
            dm.dmDisplayFrequency > 1) {
            g_targetRefreshHz = (int)dm.dmDisplayFrequency;
            LOG("Target display refresh: %d Hz (the SINK rate; the tooth guard arms off this, "
                "not off the present rate)", g_targetRefreshHz);
        } else {
            LOG("Target display refresh: UNKNOWN (EnumDisplaySettings failed); the tooth guard "
                "falls back to the present period");
        }
    }
    LOG("Capture mode: %s", captureMode->GetModeName());
    // Echo resolved options so console-entered config is recoverable from the log, not just argv.
    char markDesc[48];
    if (!g_mark)           snprintf(markDesc, sizeof(markDesc), "off");
    else if (g_markFrames) snprintf(markDesc, sizeof(markDesc), "on (first %u presents)", g_markFrames);
    else                   snprintf(markDesc, sizeof(markDesc), "on (every present)");
    LOG("Resolved options: src rate hint %.1f fps%s, comb lock %s, frame marker %s, blend tint %s, "
        "etw flip capture %s, flip join %s, dejitter %s, fgphase %s, phasekeep %s, "
        "generated-frame substitution %s, diffmap %s, flip mode %s, extra lag %u ms, "
        "present path %s",
        g_srcRateHint, g_srcRateHint > 0.0f ? "" : " (unset; assume >=60)",
        g_lock ? "on" : "off", markDesc, g_tint ? "on" : "off", g_etw ? "on" : "off",
        !g_etw ? "off (no -etw)" : (g_noJoin ? "OFF (-nojoin)" : "on"),
        !g_dejitter ? "off"
                    : (g_etw && !g_noJoin ? "ON (-dejit)"
                                          : "REFUSED (-dejit needs -etw with the join on)"),
        g_fgPhase ? "requested (-fgphase; ACTIVE only when the instrument line follows)" : "off",
        !g_phaseKeep ? "off"
                     : (g_etw && !g_noJoin ? "ON (-phasekeep)"
                                           : "REFUSED (-phasekeep needs -etw with the join on)"),
        g_subGen ? "ON (-subgen)" : "off",
        g_diffMap ? "requested (-diffmap; ACTIVE only when the instrument line follows)" : "off",
        g_flipEx ? "FLIPEX (-flipex)" : "bitblt (DISCARD)",
        g_extraLagMs,
        captureMode->PresentsViaD3D11() ? "D3D11 flip-model swapchain (b:flip)" : "D3D9 swapchain");

    BUF_WIDTH = target.position.right - target.position.left;
    BUF_HEIGHT = target.position.bottom - target.position.top;
    LOG("Buffer size: %dx%d", BUF_WIDTH, BUF_HEIGHT);

    HWND hWnd;
    WNDCLASSEX wc;

    DWORD maxDisplayWidth = -1, maxDisplayHeight = -1;

    ZeroMemory(&wc, sizeof(WNDCLASSEX));

    wc.cbSize = sizeof(WNDCLASSEX);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "WindowClass";

    // register the window class
    RegisterClassEx(&wc);

    hWnd = CreateWindowEx(NULL,
        "WindowClass",
        "NvFBCR",
        WS_EX_TOPMOST | WS_POPUP, // pseudo fullscreen //WS_OVERLAPPEDWINDOW for windowed
        target.position.left, target.position.top,    // the starting x and y positions
        BUF_WIDTH, BUF_HEIGHT,
        NULL,
        NULL,
        hInstance,
        NULL);

    ShowWindow(hWnd, nCmdShow);

    // On the D3D11 present path the output window belongs to the DXGI flip-model swapchain
    // alone: flip model allows one swapchain per window and no second API on it, and a D3D9
    // swapchain sharing the window would make a failed promotion uninterpretable. The D3D9
    // devices (present, and the ring's capture device) take this never-shown host instead.
    if (captureMode->PresentsViaD3D11()) {
        g_d3d9HostWnd = CreateWindowEx(0, "WindowClass", "NvFBCR D3D9 host", WS_POPUP,
                                       0, 0, 1, 1, NULL, NULL, hInstance, NULL);
        if (!g_d3d9HostWnd) {
            LOGERR("Unable to create the D3D9 host window (error %lu)", GetLastError());
            delete captureMode;
            Cleanup();
            return -1;
        }
        LOG("D3D9 devices hosted on a hidden window; the output window is reserved for the "
            "D3D11 flip-model swapchain");
    }

    NvFBCFrameGrabInfo frameGrabInfo = { 0 };

    //! DX9 resources
    NVFBC_TODX9VID_OUT_BUF NvFBC_OutBuf[1] = {};

    //! Load the nvfbc Library
    pNVFBCLib = new NvFBCLibrary();
    if (!pNVFBCLib->load())
    {
        LOGERR("Unable to load the NvFBC library");
        return -1;
    }

    g_bNvFBCLibLoaded = true;
    // The present device goes on the adapter that owns the OUTPUT WINDOW when the mode can
    // afford it (see IFrameCaptureMode::PresentsOnTargetAdapter). The legacy modes cannot:
    // NvFBC writes their back buffer directly, so their device must stay where NvFBC captures.
    const unsigned int presentAdapter = captureMode->PresentsOnTargetAdapter()
                                            ? (unsigned int)target.dxAdapterIndex
                                            : (unsigned int)source.dxAdapterIndex;
    LOG("Present device adapter: %u (%s); capture stays on source adapter %d",
        presentAdapter,
        captureMode->PresentsOnTargetAdapter() ? "target - owns the output window"
                                               : "source - mode captures into its back buffer",
        source.dxAdapterIndex);
    if (!SUCCEEDED(InitD3D9(presentAdapter, g_d3d9HostWnd ? g_d3d9HostWnd : hWnd,
                            captureMode->GetPresentationInterval())))
    {
        LOGERR("Unable to create D3D9Ex Device");
        delete captureMode;
        Cleanup();
        return -1;
    }

    if (!SUCCEEDED(InitD3D9Surfaces()))
    {
        LOGERR("Unable to create D3D9Ex surfaces");
        Cleanup();
        return -1;
    }
    //! Create an instance of the NvFBCDX9 class, the first argument specifies the frame buffer
    NvFBCDX9 = (NvFBCToDx9Vid*)pNVFBCLib->create(NVFBC_TO_DX9_VID, &maxDisplayWidth, &maxDisplayHeight, 0, (void*)g_pD3D9Device);
    if (!NvFBCDX9)
    {
        // Check if NvFBC is not enabled
        NvFBCStatusEx status = {0};
        status.dwVersion = NVFBC_STATUS_VER;
        status.dwAdapterIdx = 0;
        NVFBCRESULT statusRes = pNVFBCLib->getStatus(&status);

        if (statusRes == NVFBC_SUCCESS && !status.bIsCapturePossible)
        {
            LOG("NvFBC not enabled (bIsCapturePossible=false), attempting to enable...");

            // Check if running as admin before attempting to enable
            if (!IsRunningAsAdmin())
            {
                LOGERR("Failed to enable NvFBC: Administrator privileges required");
                LOGERR("Please run NvFBCR.exe as Administrator to enable NvFBC");
                LOGERR("Alternatively, run NvFBCEnable.exe as Administrator first, then run NvFBCR.exe");

                // Show error to user in console
                AllocConsole();
                FILE* fDummy;
                freopen_s(&fDummy, "CONOUT$", "w", stdout);
                freopen_s(&fDummy, "CONIN$", "r", stdin);
                cout.clear();
                cin.clear();

                cout << "\nERROR: Failed to enable NvFBC\n";
                cout << "---------------------------------------\n";
                cout << "Administrator privileges are required to enable NvFBC.\n\n";
                cout << "Please either:\n";
                cout << "  1. Run NvFBCR.exe as Administrator, OR\n";
                cout << "  2. Run NvFBCEnable.exe as Administrator first, then run NvFBCR.exe normally\n\n";
                cout << "Press Enter to exit...";
                cin.get();

                fclose(stdin);
                fclose(stdout);
                FreeConsole();

                Cleanup();
                return -1;
            }

            NVFBCRESULT enableRes = pNVFBCLib->enable(NVFBC_STATE_ENABLE);

            if (enableRes != NVFBC_SUCCESS)
            {
                LOGERR("Failed to enable NvFBC (result: 0x%X) - cannot proceed", enableRes);
                Cleanup();
                return -1;
            }

            LOG("NvFBC enabled successfully - restarting application...");
            Cleanup();

            // Restart the process with current configuration
            STARTUPINFO si = { 0 };
            si.cb = sizeof(si);
            PROCESS_INFORMATION pi = { 0 };
            char exePath[MAX_PATH];
            GetModuleFileNameA(NULL, exePath, MAX_PATH);

            // Build command line with current values
            // Note: lpCommandLine must include the exe name as first token when lpApplicationName is non-NULL
            // Windows will strip the first token and pass the rest to WinMain's lpCmdLine
            char newCmdLine[512];
            sprintf_s(newCmdLine, sizeof(newCmdLine), "\"%s\" -source %d -target %d -framerate %s",
                exePath, source.dxAdapterIndex, target.dxAdapterIndex,
                framerateStr.empty() ? "vsync" : framerateStr.c_str());

            LOG("Relaunching with command line: '%s'", newCmdLine);
            LOG("Executable path: '%s'", exePath);

            if (CreateProcessA(NULL, newCmdLine, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
            {
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);
                return 0;
            }
            else
            {
                LOGERR("Failed to restart process (error: %d)", GetLastError());
                return -1;
            }
        }

        LOGERR("Failed to create NvFBCToDx9Vid instance");
        LOGERR("    Requirement 1) Driver R355+ with Tesla/Quadro/GRID");
        LOGERR("    Requirement 2) Run 'NvFBCEnable -enable' after driver installation");
        Cleanup();
        return -1;
    }

    LOG("NvFBCToDX9Vid instance created successfully");

    NvFBC_OutBuf[0].pPrimary = g_backbuffer;


    NVFBC_TODX9VID_SETUP_PARAMS DX9SetupParams = {};
    DX9SetupParams.dwVersion = NVFBC_TODX9VID_SETUP_PARAMS_V3_VER;
    // bWithHWCursor = 1 here is fine: this session serves the plain vsync/timer modes, which grab
    // with NOWAIT (poll, never wait) — cursor moves cannot inflate their rate, and keeping the OS
    // cursor in the output is desirable for plain relay use. The temporal modes discard this
    // session (CaptureRing rebinds NvFBC) and use bWithHWCursor = 0 there, where the BLOCKING grab
    // would otherwise wake at mouse-polling rate and pollute the ring timeline (spec Round 9).
    DX9SetupParams.bWithHWCursor = 1;
    DX9SetupParams.bStereoGrab = 0;
    DX9SetupParams.bDiffMap = 0;
    DX9SetupParams.ppBuffer = NvFBC_OutBuf;
    DX9SetupParams.eMode = NVFBC_TODX9VID_ARGB10;
    DX9SetupParams.dwNumBuffers = 1;
    DX9SetupParams.bHDRRequest = TRUE;


    if (NVFBC_SUCCESS != NvFBCDX9->NvFBCToDx9VidSetUp(&DX9SetupParams))
    {
        LOGERR("Failed calling NvFBCToDx9VidSetUp()");
        Cleanup();
        return -1;
    }

    //! Setup NvFBC Grab Parameters
    NVFBC_TODX9VID_GRAB_FRAME_PARAMS fbcDX9GrabParams = { 0 };
    {
        fbcDX9GrabParams.dwVersion = NVFBC_TODX9VID_GRAB_FRAME_PARAMS_V1_VER;
        fbcDX9GrabParams.dwFlags = NVFBC_TODX9VID_NOWAIT;
        fbcDX9GrabParams.eGMode = NVFBC_TODX9VID_SOURCEMODE_SCALE;
        fbcDX9GrabParams.dwTargetWidth = BUF_WIDTH;
        fbcDX9GrabParams.dwTargetHeight = BUF_HEIGHT;
        fbcDX9GrabParams.pNvFBCFrameGrabInfo = &frameGrabInfo;
    }

    // Setup and run capture mode
    if (!captureMode->Setup()) {
        LOGERR("Failed to setup capture mode");
        delete captureMode;
        Cleanup();
        return -1;
    }

    LOG("Entering capture loop - mode: %s", captureMode->GetModeName());

    // Run the mode's capture loop (contains entire loop logic including message processing)
    captureMode->Run(NvFBCDX9, &fbcDX9GrabParams, g_pD3D9Device, hWnd);

    // Clean up capture mode
    delete captureMode;

    Cleanup();

    return 0;
}
