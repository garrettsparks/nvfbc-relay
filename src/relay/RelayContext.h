#pragma once

#include <windows.h>
#include <d3d9.h>
#include <NvFBCApi.h>

class NvFBCLoader;

// What the shell sets up and hands a capture mode: the Direct3D 9 objects, the two displays, the
// NvFBC library and the startup capture session. The shell creates and releases all of it except
// the session, which has one owner at a time: the shell until a mode takes it over, then that
// mode, which sets it to NULL here the moment it releases it, so nothing releases it twice.
struct RelayContext {
    IDirect3D9Ex* d3d = NULL;
    IDirect3DDevice9Ex* presentDevice = NULL;
    // Back buffer 0 of the present device, fetched once: the modes that grab straight into the
    // back buffer hand it to NvFBC, and the D3D9 present path falls back to it.
    IDirect3DSurface9* backBuffer = NULL;
    HWND outputWindow = NULL;   // on the target display
    HWND deviceWindow = NULL;   // the window every D3D9 device is created on
    UINT sourceAdapter = 0;     // Direct3D 9 adapter ordinals, which are the display numbers
    UINT targetAdapter = 0;
    int width = 0;              // the target display's size: back buffer, ring slots, swapchain
    int height = 0;
    int sinkRefreshHz = 0;      // the target display's refresh rate; 0 when it could not be read
    bool flipEx = false;        // the present device uses the FLIPEX swap effect (-flipex)
    NvFBCLoader* nvfbc = NULL;
    NvFBCToDx9Vid* session = NULL;
};
