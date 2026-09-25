// NvFBCR captures one display with NvFBC and presents it on another, usually a capture card's.
// This file is the shell's sequence: each step below either succeeds or returns the failure
// that stops the relay, and WinMain turns that failure into teardown and a popup.

#include <windows.h>

#include <NvFBCLoader.h>
#include <SimpleLogger.h>

#include <string>
#include <vector>

#include "D3D9Setup.h"
#include "DiagCaptureMode.h"
#include "Displays.h"
#include "Failure.h"
#include "IFrameCaptureMode.h"
#include "LaunchOptions.h"
#include "NvFBCSession.h"
#include "OutputWindow.h"
#include "Prompts.h"
#include "RelayContext.h"
#include "TemporalCaptureMode.h"
#include "TimerCaptureMode.h"
#include "VsyncCaptureMode.h"

namespace {

// How the steps ended when they did not end in a failure.
enum class Ending { WindowClosed, InputEnded, Relaunch };

// Everything the steps create, so teardown releases it in one place whichever step stopped.
struct Relay {
    RelayContext ctx;
    NvFBCLoader nvfbc;
    LaunchChoice choice;
    IFrameCaptureMode* mode = NULL;
    OutputWindows windows;
    NvFBCFrameGrabInfo grabInfo = {};
    NVFBC_TODX9VID_GRAB_FRAME_PARAMS grab = {};
    Ending ending = Ending::WindowClosed;
};

// The capture mode a mode string names, or NULL for one the grammar refuses, which the command
// line and prompt checks have already turned away. The grammar, and which present path each
// spelling selects, is launch::ParseMode, where the policy suite pins it.
IFrameCaptureMode* CreateCaptureMode(const std::string& modeText, const launch::Options& o) {
    const launch::ModeSpec spec = launch::ParseMode(modeText);
    switch (spec.kind) {
    case launch::ModeKind::Vsync:
        return new VsyncCaptureMode();
    case launch::ModeKind::Diag:
        return new DiagCaptureMode(spec.vsyncPresent);
    case launch::ModeKind::Timer:
        return new TimerCaptureMode(spec.framerate);
    case launch::ModeKind::Invalid:
        return NULL;
    case launch::ModeKind::Temporal:
        break;
    }
    CompositorKind compositor = kCompositorNearest;
    if (spec.compositor == launch::Compositor::Blend) compositor = kCompositorBlend;
    else if (spec.compositor == launch::Compositor::Interp) compositor = kCompositorInterp;
    return new TemporalCaptureMode(spec.framerate, spec.vsyncPresent, o.srcRateHint, o.lock,
                                   compositor, o.mark, o.markFrames, o.tint, o.etw, o.noJoin,
                                   o.dejitter, o.fgPhase, o.phaseKeep, o.extraLagMs,
                                   spec.d3d11Present);
}

// Takes the named lock that keeps a second relay from starting. Two relays cannot share NvFBC,
// and both would write the same log, the newcomer truncating it while the other keeps writing
// at its old offset. NULL when another relay holds it.
HANDLE TakeSingleInstanceLock() {
    HANDLE lock = CreateMutexA(NULL, TRUE, "Global\\NvFBCR_SingleInstance");
    if (lock && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(lock);
        return NULL;
    }
    return lock;
}

void LogWallClock() {
    // A UTC time and a QPC reading taken together, so the log's QPC timeline can be placed
    // against wall-clock records such as the OBS frame trace's unix_ns.
    FILETIME now;
    GetSystemTimePreciseAsFileTime(&now);
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    const unsigned long long ticks = ((unsigned long long)now.dwHighDateTime << 32) |
                                     now.dwLowDateTime;
    SYSTEMTIME utc;
    FileTimeToSystemTime(&now, &utc);
    const unsigned long long kUnixEpochTicks = 116444736000000000ULL;
    LOG("Wall clock %04u-%02u-%02u %02u:%02u:%02u.%07llu UTC (unix %llu ns) at QPC %lld",
        (unsigned)utc.wYear, (unsigned)utc.wMonth, (unsigned)utc.wDay, (unsigned)utc.wHour,
        (unsigned)utc.wMinute, (unsigned)utc.wSecond, ticks % 10000000ULL,
        (ticks - kUnixEpochTicks) * 100ULL, qpc.QuadPart);
}

MaybeFailure RunRelay(Relay& r, HINSTANCE instance, int showCommand,
                      const std::string& commandLine, bool relaunched) {
    RelayContext& ctx = r.ctx;
    if (MaybeFailure f = CreateDirect3D(&ctx.d3d)) return f;
    const std::vector<DisplayInfo> displays = EnumerateDisplays(ctx.d3d);
    if (displays.size() < 2) return TooFewDisplays(displays.size());
    if (MaybeFailure f = LoadNvFBC(&r.nvfbc)) return f;
    ctx.nvfbc = &r.nvfbc;

    if (!ChooseLaunch(commandLine, displays, &r.choice)) {
        LOG("The console's input ended before the prompts were answered");
        r.ending = Ending::InputEnded;
        return std::nullopt;
    }
    launch::Options& options = r.choice.options;
    const std::string note = launch::ResolveDependencies(&options);
    if (!note.empty()) LOG("%s", note.c_str());

    const DisplayInfo& source = displays[r.choice.source];
    const DisplayInfo& target = displays[r.choice.target];
    r.mode = CreateCaptureMode(r.choice.mode, options);
    if (!r.mode) return ModeCouldNotStart(r.choice.mode.c_str());
    // Flip mode cannot serve every mode. The D3D11 path's D3D9 swapchain never presents, and a
    // FLIPEX device on its hidden host would only add a way for creation to fail. The modes that
    // grab straight into the back buffer fetch it once at startup, and flip mode rotates it.
    if (options.flipEx && r.mode->PresentsViaD3D11()) {
        LOG("-flipex ignored: b:vsync presents through its own D3D11 swapchain");
        options.flipEx = false;
    } else if (options.flipEx && !r.mode->PresentsOnTargetAdapter()) {
        LOG("-flipex ignored: this mode grabs into a back buffer fetched once, which flip mode "
            "rotates");
        options.flipEx = false;
    }

    LOG("Source display: [%u] %s (%s)", source.adapter, source.Name().c_str(),
        source.deviceName.c_str());
    LOG("Target display: [%u] %s (%s)", target.adapter, target.Name().c_str(),
        target.deviceName.c_str());
    if (target.refreshHz > 0) {
        LOG("Target display refresh: %d Hz (the SINK rate; the tooth guard arms off this, not "
            "off the present rate)", target.refreshHz);
    } else {
        LOG("Target display refresh: UNKNOWN (EnumDisplaySettings failed); the tooth guard "
            "falls back to the present period");
    }
    LOG("Capture mode: %s", r.mode->GetModeName());
    LOG("%s", launch::ResolvedOptionsLine(options, r.mode->PresentsViaD3D11()).c_str());

    // Settled before any window exists, so turning NvFBC on and relaunching never flashes a
    // window on the capture card. A status that cannot be read is no reason to turn anything
    // on; the session's creation below fails with its own popup then.
    const CaptureStatus status = ReadCaptureStatus(&r.nvfbc);
    if (status.read && !status.possible) {
        if (MaybeFailure f = TurnOnCapture(&r.nvfbc, relaunched)) return f;
        r.ending = Ending::Relaunch;
        return std::nullopt;
    }

    ctx.sourceAdapter = source.adapter;
    ctx.targetAdapter = target.adapter;
    ctx.width = target.Width();
    ctx.height = target.Height();
    ctx.sinkRefreshHz = target.refreshHz;
    ctx.flipEx = options.flipEx;
    LOG("Buffer size: %dx%d", ctx.width, ctx.height);

    if (MaybeFailure f = CreateOutputWindows(instance, showCommand, target,
                                             r.mode->PresentsViaD3D11(), &r.windows)) {
        return f;
    }
    ctx.outputWindow = r.windows.output;
    ctx.deviceWindow = r.windows.host ? r.windows.host : r.windows.output;

    // The present device goes on the adapter that owns the output window when the mode can
    // afford it. The modes that grab straight into the back buffer cannot: their device must
    // stay where NvFBC captures.
    const bool onTarget = r.mode->PresentsOnTargetAdapter();
    const UINT presentAdapter = onTarget ? target.adapter : source.adapter;
    LOG("Present device adapter: %u (%s); capture stays on source adapter %u", presentAdapter,
        onTarget ? "target - owns the output window"
                 : "source - mode captures into its back buffer",
        source.adapter);
    if (MaybeFailure f = CreatePresentDevice(&ctx, presentAdapter,
                                             r.mode->GetPresentationInterval())) {
        return f;
    }
    if (MaybeFailure f = CreateStartupSession(&ctx, &r.grab, &r.grabInfo)) return f;
    if (MaybeFailure f = r.mode->Setup(ctx)) return f;

    LOG("Entering capture loop - mode: %s", r.mode->GetModeName());
    return r.mode->Run(ctx, &r.grab);
}

// Releases what the steps created, each session before the device it is bound to and each
// device before its window, whichever step the relay stopped at.
void Teardown(Relay& r, const char* why) {
    LOG("Shutting down: %s", why);
    RelayContext& ctx = r.ctx;
    if (ctx.session) {
        ctx.session->NvFBCToDx9VidRelease();
        ctx.session = NULL;
    }
    delete r.mode;
    r.mode = NULL;
    if (ctx.backBuffer) {
        ctx.backBuffer->Release();
        ctx.backBuffer = NULL;
    }
    if (ctx.presentDevice) {
        ctx.presentDevice->Release();
        ctx.presentDevice = NULL;
    }
    if (ctx.d3d) {
        ctx.d3d->Release();
        ctx.d3d = NULL;
    }
    DestroyOutputWindows(&r.windows);
    r.nvfbc.close();
    LOG("Shutdown complete");
    SimpleLogger::getInstance().flush();
}

const char* EndingText(Ending ending, bool failed) {
    if (failed) return "a failure (the reason follows)";
    if (ending == Ending::Relaunch) return "relaunching with NvFBC turned on";
    if (ending == Ending::InputEnded) return "the console's input ended";
    return "the output window closed";
}

}  // namespace

_Use_decl_annotations_ int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR commandLine,
                                          int showCommand) {
    // Before anything reads a monitor rectangle, so every rectangle is in physical pixels.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // Before the first log line: opening the log truncates it, and while another relay runs,
    // the log is that relay's.
    HANDLE instanceLock = TakeSingleInstanceLock();
    if (!instanceLock) return ExitWithFailure(AlreadyRunning());

    const bool relaunched = GetEnvironmentVariableA(kRelaunchMarker, NULL, 0) > 0;
    if (relaunched) SimpleLogger::ContinueExistingLog();
    LOG("%s", relaunched ? "NvFBCR starting, relaunched after turning on NvFBC"
                         : "NvFBCR starting");
    LogWallClock();
    const std::string arguments = commandLine ? commandLine : "";
    LOG("Command line: '%s'", arguments.c_str());

    Relay r;
    const MaybeFailure failure = RunRelay(r, instance, showCommand, arguments, relaunched);
    Teardown(r, EndingText(r.ending, failure.has_value()));
    if (failure) return ExitWithFailure(*failure);
    if (r.ending == Ending::Relaunch) {
        std::string relaunch = "-source " + std::to_string(r.choice.source) + " -target " +
                               std::to_string(r.choice.target) + " -framerate " +
                               (r.choice.mode.empty() ? std::string("b:vsync") : r.choice.mode);
        const std::string options = launch::FormatOptions(r.choice.options);
        if (!options.empty()) relaunch += " " + options;
        if (MaybeFailure f = Relaunch(relaunch, &instanceLock)) return ExitWithFailure(*f);
    }
    return 0;
}
