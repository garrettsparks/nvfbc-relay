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
#include <NvFBC/NvFBCToDx9vid.h>

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

struct DisplayPosition {
    int dxAdapterIndex;
    RECT position;
    char deviceName[32];
    string friendlyName;
} source, target;

vector <DisplayPosition> displays;

void Cleanup()
{

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
    if (timer)
    {
        CloseHandle(timer);
    }

}

int InitDisplays() {

    Direct3DCreate9Ex(D3D_SDK_VERSION, &g_pD3DEx);
    unsigned int adapterCount = g_pD3DEx->GetAdapterCount();

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
            fprintf(stderr, "Failed to create D3D9 surfaces D3DFMT_A2B10G10R10 for output. Error 0x%08x\n", hr);
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
        fprintf(stderr, "Unable to enumerate display adapters");
        return -1;
    }
    ConsoleUserInput();

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
        fprintf(stderr, "Unable to load the NvFBC library\n");
        return -1;
    }

    g_bNvFBCLibLoaded = true;
    if (!SUCCEEDED(InitD3D9(source.dxAdapterIndex, hWnd)))
    {
        fprintf(stderr, "Unable to create a D3D9Ex Device\n");
        Cleanup();
        return -1;
    }

    if (!SUCCEEDED(InitD3D9Surfaces()))
    {
        fprintf(stderr, "Unable to create a D3D9Ex Device\n");
        Cleanup();
        return -1;
    }
    //! Create an instance of the NvFBCDX9 class, the first argument specifies the frame buffer
    NvFBCDX9 = (NvFBCToDx9Vid*)pNVFBCLib->create(NVFBC_TO_DX9_VID, &maxDisplayWidth, &maxDisplayHeight, 0, (void*)g_pD3D9Device);
    if (!NvFBCDX9)
    {
        fprintf(stderr, "Failed to create an instance of NvFBCToDx9Vid.  Please check the following requirements\n");
        fprintf(stderr, "1) A driver R355 or newer with capable Tesla/Quadro/GRID capable product\n");
        fprintf(stderr, "2) Run \"NvFBCEnable -enable\" after a new driver installation\n");
        Cleanup();
        return -1;
    }

    fprintf(stderr, "NvFBCToDX9Vid CreateEx() success!\n");

    NvFBC_OutBuf[0].pPrimary = g_backbuffer;


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
        fprintf(stderr, "Failed when calling NvFBCDX9->NvFBCToDX9VidSetup()\n");
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

    li.QuadPart = -(10000000 / framerate);
    timer = CreateWaitableTimer(NULL, TRUE, NULL);

    if (NULL == timer)
    {
        printf("CreateWaitableTimer failed (%d)\n", GetLastError());
        return 1;
    }

    while (TRUE)
    {

        SetWaitableTimer(timer, &li, 0, NULL, NULL, FALSE);
        fbcRes = NvFBCDX9->NvFBCToDx9VidGrabFrame(&fbcDX9GrabParams);
        g_pD3D9Device->PresentEx(NULL, NULL, NULL, NULL, D3DPRESENT_INTERVAL_IMMEDIATE); //D3DPRESENT_FORCEIMMEDIATE //D3DPRESENT_INTERVAL_IMMEDIATE
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if (msg.message == WM_QUIT)
            break;

        WaitForSingleObject(timer, INFINITE);

    }

    Cleanup();

    return msg.wParam;
}






