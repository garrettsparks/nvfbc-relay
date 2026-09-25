#pragma once

#include <windows.h>
#include <d3d9.h>

#include <string>
#include <vector>

// One display attached to the desktop, as the relay numbers it: its Direct3D 9 adapter ordinal,
// which is the number the prompts list and -source and -target take.
struct DisplayInfo {
    UINT adapter = 0;
    std::string deviceName;     // the GDI name, \\.\DISPLAY1
    std::string friendlyName;   // the monitor's own name; empty when Windows reports none
    RECT rect = {};             // desktop coordinates, in physical pixels
    int refreshHz = 0;          // 0 when it could not be read

    const std::string& Name() const { return friendlyName.empty() ? deviceName : friendlyName; }
    int Width() const { return rect.right - rect.left; }
    int Height() const { return rect.bottom - rect.top; }
};

// One entry per Direct3D 9 adapter, in ordinal order, each logged. Friendly names are best
// effort: a display Windows gives no name keeps its device name.
std::vector<DisplayInfo> EnumerateDisplays(IDirect3D9Ex* d3d);
