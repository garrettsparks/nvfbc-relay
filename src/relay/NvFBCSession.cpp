#include "NvFBCSession.h"

#include <AdminCheck.h>
#include <SimpleLogger.h>

#include <vector>

MaybeFailure LoadNvFBC(NvFBCLoader* nvfbc) {
    if (!nvfbc->load()) return NvFBCLibraryMissing();
    return std::nullopt;
}

CaptureStatus ReadCaptureStatus(NvFBCLoader* nvfbc) {
    CaptureStatus s;
    NvFBCStatusEx status = {};
    status.dwVersion = NVFBC_STATUS_VER;
    status.dwAdapterIdx = 0;
    const NVFBCRESULT result = nvfbc->getStatus(&status);
    if (result != NVFBC_SUCCESS) {
        LOGERR("NvFBC status unavailable (result: 0x%X)", (unsigned)result);
        return s;
    }
    s.read = true;
    s.possible = status.bIsCapturePossible != 0;
    s.canCreateNow = status.bCanCreateNow != 0;
    LOG("NvFBC status: capture possible %s, can create now %s", s.possible ? "yes" : "no",
        s.canCreateNow ? "yes" : "no");
    return s;
}

MaybeFailure TurnOnCapture(NvFBCLoader* nvfbc, bool relaunched) {
    if (relaunched) return CaptureStillOff();
    // The manifest elevates every launch, so only an account that cannot elevate gets here.
    if (!IsRunningAsAdmin()) return CaptureOffNotAdmin();
    LOG("NvFBC capture is not possible; turning it on, then relaunching");
    const NVFBCRESULT result = nvfbc->enable(NVFBC_STATE_ENABLE);
    if (result != NVFBC_SUCCESS) return CaptureEnableFailed(result);
    return std::nullopt;
}

MaybeFailure Relaunch(const std::string& arguments, HANDLE* instanceLock) {
    char exePath[MAX_PATH];
    const DWORD length = GetModuleFileNameA(NULL, exePath, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return RelaunchFailed(GetLastError());

    // The command line starts with the exe, which Windows strips before the new process's
    // WinMain sees the rest.
    const std::string commandLine = std::string("\"") + exePath + "\" " + arguments;
    std::vector<char> writable(commandLine.begin(), commandLine.end());
    writable.push_back('\0');
    LOG("Relaunching with command line: '%s'", commandLine.c_str());

    // The new process continues this log instead of truncating it, so everything this process
    // wrote must be on disk first, and it takes the single-instance lock as it starts, so this
    // process lets go of it here. Nothing more is logged unless the start fails, and then there
    // is no new process to collide with.
    SetEnvironmentVariableA(kRelaunchMarker, "1");
    SimpleLogger::getInstance().flush();
    ReleaseMutex(*instanceLock);
    CloseHandle(*instanceLock);
    *instanceLock = NULL;

    STARTUPINFOA startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {};
    if (!CreateProcessA(NULL, writable.data(), NULL, NULL, FALSE, 0, NULL, NULL, &startup,
                        &process)) {
        return RelaunchFailed(GetLastError());
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return std::nullopt;
}

MaybeFailure CreateStartupSession(RelayContext* ctx, NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grab,
                                  NvFBCFrameGrabInfo* grabInfo) {
    DWORD maxWidth = 0;
    DWORD maxHeight = 0;
    ctx->session = (NvFBCToDx9Vid*)ctx->nvfbc->create(NVFBC_TO_DX9_VID, &maxWidth, &maxHeight,
                                                      0, (void*)ctx->presentDevice);
    if (!ctx->session) return SessionRefused(ctx->nvfbc, "startup session");

    // The hardware cursor stays in the picture here: this session serves the modes that poll
    // without waiting, where cursor moves cannot change the capture rate. The temporal modes
    // replace it with a session that leaves the cursor out, because their blocking grab would
    // otherwise wake at the mouse's polling rate.
    NVFBC_TODX9VID_OUT_BUF target = {};
    target.pPrimary = ctx->backBuffer;
    NVFBC_TODX9VID_SETUP_PARAMS setup = {};
    setup.dwVersion = NVFBC_TODX9VID_SETUP_PARAMS_V3_VER;
    setup.bWithHWCursor = 1;
    setup.bStereoGrab = 0;
    setup.bDiffMap = 0;
    setup.bHDRRequest = 1;
    setup.eMode = NVFBC_TODX9VID_ARGB10;
    setup.dwNumBuffers = 1;
    setup.ppBuffer = &target;
    const NVFBCRESULT result = ctx->session->NvFBCToDx9VidSetUp(&setup);
    if (result != NVFBC_SUCCESS) {
        LOGERR("NvFBCToDx9VidSetUp failed on the startup session (result: 0x%X)",
               (unsigned)result);
        return CaptureCouldNotStart("startup session");
    }

    *grab = {};
    grab->dwVersion = NVFBC_TODX9VID_GRAB_FRAME_PARAMS_V1_VER;
    grab->dwFlags = NVFBC_TODX9VID_NOWAIT;
    grab->eGMode = NVFBC_TODX9VID_SOURCEMODE_SCALE;
    grab->dwTargetWidth = (NvU32)ctx->width;
    grab->dwTargetHeight = (NvU32)ctx->height;
    grab->pNvFBCFrameGrabInfo = grabInfo;
    LOG("Startup capture session set up on the back buffer (hardware cursor on, 10-bit ARGB, "
        "no-wait grab)");
    return std::nullopt;
}

Failure SessionRefused(NvFBCLoader* nvfbc, const char* where) {
    const CaptureStatus status = ReadCaptureStatus(nvfbc);
    if (status.read && status.possible && !status.canCreateNow) return CaptureInUse(where);
    return CaptureCouldNotStart(where);
}
