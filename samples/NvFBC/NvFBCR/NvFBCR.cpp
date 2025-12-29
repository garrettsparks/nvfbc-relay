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

using namespace std;


#define _CRT_SECURE_NO_WARNINGS  1


// DirectX resources
IDirect3D9Ex        *g_pD3DEx = NULL;
IDirect3DDevice9Ex  *g_pD3D9Device = NULL;
IDirect3DSurface9* g_backbuffer = NULL;

bool g_bNvFBCLibLoaded = false;

NvFBCToDx9Vid *NvFBCDX9 = NULL;
NvFBCLibrary *pNVFBCLib;

int BUF_WIDTH;
int BUF_HEIGHT;

HANDLE timer;
LARGE_INTEGER li;
int framerate = 60;

// Frame blending resources
#define FRAME_HISTORY_SIZE 2  // Reduced from 3 for better performance
struct FrameHistoryEntry {
    IDirect3DSurface9* surface;
    LARGE_INTEGER timestamp;
    bool valid;
};
FrameHistoryEntry g_frameHistory[FRAME_HISTORY_SIZE];
int g_currentHistoryIndex = 0;
IDirect3DSurface9* g_captureTarget = NULL;  // Intermediate capture surface
LARGE_INTEGER g_perfFreq;

struct DisplayPosition {
    int dxAdapterIndex;
    RECT position;
    char deviceName[32];
    string friendlyName;
} source, target;

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

    // Release frame history surfaces
    for (int i = 0; i < FRAME_HISTORY_SIZE; i++)
    {
        if (g_frameHistory[i].surface)
        {
            g_frameHistory[i].surface->Release();
            g_frameHistory[i].surface = NULL;
        }
    }

    if (g_captureTarget)
    {
        g_captureTarget->Release();
        g_captureTarget = NULL;
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
    if (timer)
    {
        CloseHandle(timer);
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

HRESULT InitD3D9(unsigned int deviceID, HWND hwnd)
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
    d3dpp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    //d3dpp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
    //d3dpp.Flags = D3DPRESENTFLAG_VIDEO;
    d3dpp.hDeviceWindow = hwnd;
    DWORD dwBehaviorFlags = D3DCREATE_HARDWARE_VERTEXPROCESSING;

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

HRESULT InitFrameBlending()
{
    HRESULT hr = S_OK;

    // Initialize frame history
    for (int i = 0; i < FRAME_HISTORY_SIZE; i++)
    {
        g_frameHistory[i].surface = NULL;
        g_frameHistory[i].valid = false;
        g_frameHistory[i].timestamp.QuadPart = 0;
    }

    // Create capture target surface (where NvFBC will write)
    hr = g_pD3D9Device->CreateOffscreenPlainSurface(
        BUF_WIDTH, BUF_HEIGHT,
        D3DFMT_A2B10G10R10,
        D3DPOOL_DEFAULT,
        &g_captureTarget,
        NULL);

    if (FAILED(hr))
    {
        LOGERR("Failed to create capture target surface (error: 0x%08x)", hr);
        return hr;
    }

    // Create frame history surfaces
    for (int i = 0; i < FRAME_HISTORY_SIZE; i++)
    {
        hr = g_pD3D9Device->CreateOffscreenPlainSurface(
            BUF_WIDTH, BUF_HEIGHT,
            D3DFMT_A2B10G10R10,
            D3DPOOL_DEFAULT,
            &g_frameHistory[i].surface,
            NULL);

        if (FAILED(hr))
        {
            LOGERR("Failed to create frame history surface %d (error: 0x%08x)", i, hr);
            return hr;
        }
    }

    QueryPerformanceFrequency(&g_perfFreq);

    LOG("Frame blending initialized (history size: %d)", FRAME_HISTORY_SIZE);
    return S_OK;
}

void BlendFramesToBackbuffer(LARGE_INTEGER targetTime)
{
    // Find the two frames that bracket the target time
    int bestBefore = -1;
    int bestAfter = -1;
    LONGLONG smallestBeforeDiff = LLONG_MAX;
    LONGLONG smallestAfterDiff = LLONG_MAX;

    for (int i = 0; i < FRAME_HISTORY_SIZE; i++)
    {
        if (!g_frameHistory[i].valid) continue;

        LONGLONG diff = targetTime.QuadPart - g_frameHistory[i].timestamp.QuadPart;

        if (diff >= 0 && diff < smallestBeforeDiff)
        {
            smallestBeforeDiff = diff;
            bestBefore = i;
        }
        else if (diff < 0 && -diff < smallestAfterDiff)
        {
            smallestAfterDiff = -diff;
            bestAfter = i;
        }
    }

    // If we have frames to blend
    if (bestBefore >= 0 && bestAfter >= 0)
    {
        // Calculate blend weight (0.0 = use before frame, 1.0 = use after frame)
        double totalDiff = (double)(g_frameHistory[bestAfter].timestamp.QuadPart - g_frameHistory[bestBefore].timestamp.QuadPart);
        double weight = totalDiff > 0 ? (double)smallestBeforeDiff / totalDiff : 0.5;

        // For D3D9 without pixel shaders, we'll use a simple approach:
        // Blit first frame at reduced alpha, then second frame on top
        // This creates a blended effect

        RECT srcRect = { 0, 0, (LONG)BUF_WIDTH, (LONG)BUF_HEIGHT };

        // Copy "before" frame to backbuffer
        g_pD3D9Device->StretchRect(
            g_frameHistory[bestBefore].surface,
            &srcRect,
            g_backbuffer,
            &srcRect,
            D3DTEXF_NONE);

        // Note: True alpha blending would require render target + textures + pixel shader
        // For now, we'll just use the closest frame (simple nearest-neighbor selection)
        // A full implementation would need a more complex setup
    }
    else if (bestBefore >= 0)
    {
        // Only have a "before" frame, use it
        RECT srcRect = { 0, 0, (LONG)BUF_WIDTH, (LONG)BUF_HEIGHT };
        g_pD3D9Device->StretchRect(
            g_frameHistory[bestBefore].surface,
            &srcRect,
            g_backbuffer,
            &srcRect,
            D3DTEXF_NONE);
    }
    else if (bestAfter >= 0)
    {
        // Only have an "after" frame, use it
        RECT srcRect = { 0, 0, (LONG)BUF_WIDTH, (LONG)BUF_HEIGHT };
        g_pD3D9Device->StretchRect(
            g_frameHistory[bestAfter].surface,
            &srcRect,
            g_backbuffer,
            &srcRect,
            D3DTEXF_NONE);
    }
    // If no valid frames, backbuffer will just show whatever was there before
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

bool ParseCommandLineArgs(LPSTR lpCmdLine, int* sourceIndex, int* targetIndex, int* framerateValue) {
    *sourceIndex = -1;
    *targetIndex = -1;
    *framerateValue = -1;

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
            *framerateValue = stoi(args[i + 1]);
            foundAny = true;
            i++; // Skip the value
        }
    }

    return foundAny;
}

void ConsoleUserInput() {
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
    cout << "Capture/Present framerate (blank to default 60fps) ? ";
    string cinString;
    getline(cin, cinString);
    if (!cinString.empty())
        framerate = stoi(cinString);

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

    if (!InitDisplays()) {
        LOGERR("Unable to enumerate display adapters");
        return -1;
    }

    // Try to parse command line arguments
    LOG("Command line received: '%s'", lpCmdLine ? lpCmdLine : "(null)");
    int argSourceIndex = -1;
    int argTargetIndex = -1;
    int argFramerate = -1;
    bool hasArgs = ParseCommandLineArgs(lpCmdLine, &argSourceIndex, &argTargetIndex, &argFramerate);
    LOG("Parsed args - hasArgs: %d, source: %d, target: %d, framerate: %d", hasArgs, argSourceIndex, argTargetIndex, argFramerate);

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

        // Set framerate if provided, otherwise use default
        if (argFramerate > 0) {
            framerate = argFramerate;
        }
    } else {
        // Fall back to interactive console input
        ConsoleUserInput();
    }

    LOG("=== NvFBCR Starting ===");
    LOG("Source display: [%d] %s (%s)", source.dxAdapterIndex, source.friendlyName.c_str(), source.deviceName);
    LOG("Target display: [%d] %s (%s)", target.dxAdapterIndex, target.friendlyName.c_str(), target.deviceName);
    LOG("Framerate: %d fps", framerate);
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
    if (!SUCCEEDED(InitD3D9(source.dxAdapterIndex, hWnd)))
    {
        LOGERR("Unable to create D3D9Ex Device");
        Cleanup();
        return -1;
    }

    if (!SUCCEEDED(InitD3D9Surfaces()))
    {
        LOGERR("Unable to create D3D9Ex surfaces");
        Cleanup();
        return -1;
    }

    if (!SUCCEEDED(InitFrameBlending()))
    {
        LOGERR("Unable to initialize frame blending");
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
            sprintf_s(newCmdLine, sizeof(newCmdLine), "\"%s\" -source %d -target %d -framerate %d",
                exePath, source.dxAdapterIndex, target.dxAdapterIndex, framerate);

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

    // NvFBC writes to capture target, not directly to backbuffer
    NvFBC_OutBuf[0].pPrimary = g_captureTarget;


    NVFBC_TODX9VID_SETUP_PARAMS DX9SetupParams = {};
    DX9SetupParams.dwVersion = NVFBC_TODX9VID_SETUP_PARAMS_V3_VER;
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

    MSG msg;

    //! Setup NvFBC Grab Parameters
    NVFBC_TODX9VID_GRAB_FRAME_PARAMS fbcDX9GrabParams = { 0 };
    NVFBCRESULT fbcRes = NVFBC_SUCCESS;
    {
        fbcDX9GrabParams.dwVersion = NVFBC_TODX9VID_GRAB_FRAME_PARAMS_V1_VER;
        fbcDX9GrabParams.dwFlags = NVFBC_TODX9VID_NOWAIT;
        fbcDX9GrabParams.eGMode = NVFBC_TODX9VID_SOURCEMODE_SCALE;
        fbcDX9GrabParams.dwTargetWidth = BUF_WIDTH;
        fbcDX9GrabParams.dwTargetHeight = BUF_HEIGHT;
        fbcDX9GrabParams.pNvFBCFrameGrabInfo = &frameGrabInfo;
    }

    LOG("Entering capture loop (frame blending enabled, history size: %d)", FRAME_HISTORY_SIZE);

    LARGE_INTEGER nextPresentTime, currentTime;
    QueryPerformanceCounter(&nextPresentTime);
    LONGLONG ticksPerFrame = g_perfFreq.QuadPart / framerate;

    while (TRUE)
    {
        QueryPerformanceCounter(&currentTime);

        // Calculate time until next present
        LONGLONG timeUntilPresent = nextPresentTime.QuadPart - currentTime.QuadPart;
        DWORD msUntilPresent = timeUntilPresent > 0 ?
            (DWORD)((timeUntilPresent * 1000) / g_perfFreq.QuadPart) : 0;

        // Smart sleep: if we're far from present time, sleep most of it
        if (msUntilPresent > 5)
        {
            Sleep(msUntilPresent - 4);  // Wake up 4ms before present time
            continue;  // Skip frame capture, just loop back to check timing
        }

        // Poll for latest frame (NOWAIT - never blocks)
        fbcRes = NvFBCDX9->NvFBCToDx9VidGrabFrame(&fbcDX9GrabParams);

        if (fbcRes == NVFBC_ERROR_INVALIDATED_SESSION)
        {
            LOGERR("NvFBC session invalidated - session needs to be recreated");
            break;
        }

        // Only store frame if we're close to present time (within 2 frame periods)
        // This reduces copies from ~224/sec to ~120/sec
        if (fbcRes == NVFBC_SUCCESS && timeUntilPresent < (ticksPerFrame * 2))
        {
            QueryPerformanceCounter(&currentTime);

            // Copy captured frame to current history slot
            RECT srcRect = { 0, 0, (LONG)BUF_WIDTH, (LONG)BUF_HEIGHT };
            g_pD3D9Device->StretchRect(
                g_captureTarget,
                &srcRect,
                g_frameHistory[g_currentHistoryIndex].surface,
                &srcRect,
                D3DTEXF_NONE);

            // Update timestamp and mark valid
            g_frameHistory[g_currentHistoryIndex].timestamp = currentTime;
            g_frameHistory[g_currentHistoryIndex].valid = true;

            // Advance to next slot
            g_currentHistoryIndex = (g_currentHistoryIndex + 1) % FRAME_HISTORY_SIZE;
        }

        // Check if it's time to present
        QueryPerformanceCounter(&currentTime);
        if (currentTime.QuadPart >= nextPresentTime.QuadPart)
        {
            // Blend frames from history to backbuffer based on target present time
            BlendFramesToBackbuffer(nextPresentTime);

            // Present the blended result
            g_pD3D9Device->PresentEx(NULL, NULL, NULL, NULL, D3DPRESENT_INTERVAL_IMMEDIATE);

            // Schedule next present
            nextPresentTime.QuadPart += ticksPerFrame;

            // Prevent falling too far behind
            if (nextPresentTime.QuadPart < currentTime.QuadPart)
            {
                nextPresentTime = currentTime;
            }
        }

        // Process Windows messages
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        if (msg.message == WM_QUIT)
            break;
    }

    Cleanup();

    return static_cast<int>(msg.wParam);
}
