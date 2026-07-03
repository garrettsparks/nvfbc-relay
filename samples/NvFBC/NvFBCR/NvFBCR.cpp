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
#include <d3dcompiler.h>
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

// Helper function to parse capture mode string and create appropriate mode instance
IFrameCaptureMode* ParseCaptureMode(const string& modeStr) {
    if (modeStr.empty() || _stricmp(modeStr.c_str(), "vsync") == 0) {
        // Default to vsync mode
        return new VsyncCaptureMode();
    }

    // Temporal selection + vsync present (t:vsync or just t): CaptureRing-based temporal mode,
    // present blocked on DWM's compose clock (windowed INTERVAL_ONE; card-locked 60 Hz under a
    // fullscreen game on the source — the production case). Nominal 60 fps drives the
    // bracketing lag; the actual present rate is DWM's delivery.
    if (_stricmp(modeStr.c_str(), "t") == 0 || _stricmp(modeStr.c_str(), "t:vsync") == 0) {
        return new TemporalCaptureMode(60.0f, /*vsyncPresent=*/true);
    }

    // Temporal selection + QPC-timer present (t:60 format).
    if (modeStr.length() > 2 && modeStr[0] == 't' && modeStr[1] == ':') {
        try {
            float framerate = stof(modeStr.substr(2));
            if (framerate > 0.0f && framerate <= 1000.0f) {
                return new TemporalCaptureMode(framerate, /*vsyncPresent=*/false);
            }
        }
        catch (...) {
            // Invalid number after t:
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

    // Blend selection (b, b:vsync, b:<rate>): the same CaptureRing temporal loop, but the
    // compose step renders lerp(before, after, w) instead of picking the nearest frame —
    // removes stride quantization at non-integer rate ratios (e.g. 90->60) at the cost of
    // motion blur proportional to the bracket gap.
    if (_stricmp(modeStr.c_str(), "b") == 0 || _stricmp(modeStr.c_str(), "b:vsync") == 0) {
        return new TemporalCaptureMode(60.0f, /*vsyncPresent=*/true, kCompositorBlend);
    }
    if (modeStr.length() > 2 && modeStr[0] == 'b' && modeStr[1] == ':') {
        try {
            float framerate = stof(modeStr.substr(2));
            if (framerate > 0.0f && framerate <= 1000.0f) {
                return new TemporalCaptureMode(framerate, /*vsyncPresent=*/false, kCompositorBlend);
            }
        }
        catch (...) {
            // Invalid number after b:
        }
    }

    // Optical-flow interpolation (o, o:vsync, o:<rate>): the temporal loop with the NvOFFRUC
    // motion-compensated compositor (D3D11 sidecar). Falls back to blend at runtime on any
    // sidecar/engine failure. Prior attempt's conclusions are contaminated evidence (no
    // decoupled capture, guessed timestamps, gen frames unknowingly fed) - this is the
    // clean-timeline re-trial. Expectation of quality issues is on record; the raw-flow
    // variant is the co-primary bet (see -interp on the nvof-warp branch).
    if (_stricmp(modeStr.c_str(), "o") == 0 || _stricmp(modeStr.c_str(), "o:vsync") == 0) {
        return new TemporalCaptureMode(60.0f, /*vsyncPresent=*/true, kCompositorInterp);
    }
    if (modeStr.length() > 2 && modeStr[0] == 'o' && modeStr[1] == ':') {
        try {
            float framerate = stof(modeStr.substr(2));
            if (framerate > 0.0f && framerate <= 1000.0f) {
                return new TemporalCaptureMode(framerate, /*vsyncPresent=*/false, kCompositorInterp);
            }
        }
        catch (...) {
            // Invalid number after o:
        }
    }

    // Try to parse as numeric framerate
    try {
        float framerate = stof(modeStr);
        if (framerate > 0.0f && framerate <= 1000.0f) {
            return new TimerCaptureMode(framerate);
        }
    }
    catch (...) {
        // Not a valid number
    }

    LOGERR("Invalid capture mode: '%s'", modeStr.c_str());
    LOGERR("Valid modes:");
    LOGERR("  vsync          - VSync-driven presentation");
    LOGERR("  t, t:vsync     - Temporal frame selection, presented on vsync (DWM compose clock)");
    LOGERR("  t:59.94        - Temporal frame selection, presented on a timer at given fps");
    LOGERR("  b, b:vsync     - Temporal blend (lerp of bracketing frames), presented on vsync");
    LOGERR("  b:59.94        - Temporal blend, presented on a timer at given fps");
    LOGERR("  o, o:vsync     - Optical-flow interpolation (NvOFFRUC), presented on vsync");
    LOGERR("  o:59.94        - Optical-flow interpolation, presented on a timer at given fps");
    LOGERR("Options:");
    LOGERR("  -interp fruc|flow - o:* engine (default fruc; flow = raw NVOFA + our warp)");
    LOGERR("  diag, diag:vsync - Clock probes (DWM compose timing + card raster; vsync variant measures DWM delivery)");
    LOGERR("  60             - Timer mode (simple timer-driven at specified fps)");
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

// Interp compositor backend for o:* modes (-interp fruc|flow). FRUC is the runnable default
// until the raw-flow session init is verified against the dropped-in D3D11 OF header (see
// FlowWarpEngine); flow is the favored long-term bet.
int g_interpBackend = 0;

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

    int i = 0;
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
    //d3dpp.BackBufferFormat = D3DFMT_X8R8G8B8;

    d3dpp.BackBufferWidth  = BUF_WIDTH;
    d3dpp.BackBufferHeight = BUF_HEIGHT;
    d3dpp.BackBufferCount = 1;
    d3dpp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    //d3dpp.SwapEffect = D3DSWAPEFFECT_FLIPEX;
    d3dpp.PresentationInterval = presentationInterval;
    //d3dpp.Flags = D3DPRESENTFLAG_VIDEO;
    d3dpp.hDeviceWindow = hwnd;
    // D3DCREATE_MULTITHREADED: the temporal modes drive capture on a separate thread from
    // present, so D3D9 device calls (StretchRect/Present) come from two threads. This flag
    // makes the D3D9 runtime serialize them safely. It only affects this process's own
    // device, not the captured game's rendering.
    DWORD dwBehaviorFlags = D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED;

    hr = g_pD3DEx->CreateDeviceEx(
        deviceID,
        D3DDEVTYPE_HAL,
        hwnd,
        dwBehaviorFlags,
        &d3dpp,
        NULL,
        &g_pD3D9Device);

    assert(SUCCEEDED(hr));

    return hr;
}

HRESULT InitD3D9Surfaces()
{
    HRESULT hr = E_FAIL;

    if (g_pD3D9Device)
    {

        hr = g_pD3D9Device->CreateOffscreenPlainSurface(BUF_WIDTH, BUF_HEIGHT, D3DFMT_A2B10G10R10, D3DPOOL_DEFAULT, &g_backbuffer, NULL); //D3DFMT_A8R8G8B8 D3DFMT_A2B10G10R10
        if (FAILED(hr))
        {
            LOGERR("Failed to create D3D9 surface D3DFMT_A2B10G10R10 (error: 0x%08x)", hr);
        }
        g_pD3D9Device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &g_backbuffer);
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

bool ParseCommandLineArgs(LPSTR lpCmdLine, int* sourceIndex, int* targetIndex, string* framerateStr) {
    *sourceIndex = -1;
    *targetIndex = -1;
    *framerateStr = "";

    if (!lpCmdLine || strlen(lpCmdLine) == 0) {
        return false;
    }

    string cmdLine(lpCmdLine);
    vector<string> args;

    // Split command line into tokens
    size_t pos = 0;
    while (pos < cmdLine.length()) {
        // Skip whitespace
        while (pos < cmdLine.length() && cmdLine[pos] == ' ') {
            pos++;
        }
        if (pos >= cmdLine.length()) break;

        // Extract token
        size_t tokenStart = pos;
        while (pos < cmdLine.length() && cmdLine[pos] != ' ') {
            pos++;
        }
        args.push_back(cmdLine.substr(tokenStart, pos - tokenStart));
    }

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
        else if (args[i] == "-interp" && i + 1 < args.size()) {
            if (args[i + 1] == "flow")       g_interpBackend = 1;
            else if (args[i + 1] == "fruc")  g_interpBackend = 0;
            else LOGERR("Unknown -interp value '%s' (fruc|flow) - keeping default", args[i + 1].c_str());
            foundAny = true;
            i++; // Skip the value
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
    cout << endl;
    cout << "  b, b:vsync     - Temporal blend (lerp of bracketing frames), presented on vsync" << endl;
    cout << "  b:59.94        - Temporal blend, presented on a timer at given fps" << endl;
    cout << endl;
    cout << "  diag, diag:vsync - Clock probes (DWM compose timing + card raster)" << endl;
    cout << endl;
    cout << "  60             - Timer mode (simple timer-driven at specified fps)" << endl;
    cout << endl;
    cout << "Capture/Present framerate (blank for vsync) ? ";
    string cinString;
    getline(cin, cinString);
    if (!cinString.empty())
        *framerateStr = cinString;

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

    // Create capture mode instance
    IFrameCaptureMode* captureMode = ParseCaptureMode(framerateStr);
    if (!captureMode) {
        LOGERR("Failed to create capture mode");
        Cleanup();
        return -1;
    }

    LOG("=== NvFBCR Starting ===");
    LOG("Source display: [%d] %s (%s)", source.dxAdapterIndex, source.friendlyName.c_str(), source.deviceName);
    LOG("Target display: [%d] %s (%s)", target.dxAdapterIndex, target.friendlyName.c_str(), target.deviceName);
    g_targetAdapterIndex = target.dxAdapterIndex;
    LOG("Capture mode: %s", captureMode->GetModeName());
    LOG("Buffer size: %dx%d", BUF_WIDTH, BUF_HEIGHT);

    BUF_WIDTH = target.position.right - target.position.left;
    BUF_HEIGHT = target.position.bottom - target.position.top;

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

    NvFBCFrameGrabInfo frameGrabInfo = { 0 };

    //! DX9 resources
    NVFBC_TODX9VID_OUT_BUF NvFBC_OutBuf[1];

    //! Load the nvfbc Library
    pNVFBCLib = new NvFBCLibrary();
    if (!pNVFBCLib->load())
    {
        LOGERR("Unable to load the NvFBC library");
        return -1;
    }

    g_bNvFBCLibLoaded = true;
    if (!SUCCEEDED(InitD3D9(source.dxAdapterIndex, hWnd, captureMode->GetPresentationInterval())))
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
    DX9SetupParams.eMode = NVFBC_TODX9VID_ARGB10; //NVFBC_TODX9VID_ARGB10; //NVFBC_TODX9VID_ARGB;
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
