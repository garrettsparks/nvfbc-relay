#include "Displays.h"

#include <SimpleLogger.h>

#include <cstring>

namespace {

std::string Narrow(const WCHAR* text) {
    char out[128] = "";
    WideCharToMultiByte(CP_ACP, 0, text, -1, out, (int)sizeof(out), NULL, NULL);
    return out;
}

// Gives each display its monitor's friendly name. Windows' display configuration lists one
// path per active monitor, and each path names the GDI device it is shown through, so the name
// goes to the display with that device name. The order of the paths is not the order of the
// Direct3D adapters, and a duplicated desktop has more paths than adapters; there the first
// monitor's name is kept.
void AddFriendlyNames(std::vector<DisplayInfo>* displays) {
    std::vector<DISPLAYCONFIG_PATH_INFO> activePaths;
    std::vector<DISPLAYCONFIG_MODE_INFO> activeModes;
    LONG result = ERROR_SUCCESS;
    do {
        UINT32 pathSlots = 0;
        UINT32 modeSlots = 0;
        result = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathSlots, &modeSlots);
        if (result != ERROR_SUCCESS) break;
        activePaths.assign(pathSlots, DISPLAYCONFIG_PATH_INFO{});
        activeModes.assign(modeSlots, DISPLAYCONFIG_MODE_INFO{});
        // The configuration can change between the two calls, which then asks for more room.
        result = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathSlots, activePaths.data(),
                                    &modeSlots, activeModes.data(), NULL);
        activePaths.resize(pathSlots);
    } while (result == ERROR_INSUFFICIENT_BUFFER);
    if (result != ERROR_SUCCESS) {
        LOGERR("Display names unavailable: the display configuration query failed (error %ld); "
               "device names stand in", result);
        return;
    }

    for (const DISPLAYCONFIG_PATH_INFO& path : activePaths) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source = {};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = path.sourceInfo.adapterId;
        source.header.id = path.sourceInfo.id;
        DISPLAYCONFIG_TARGET_DEVICE_NAME target = {};
        target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        target.header.size = sizeof(target);
        target.header.adapterId = path.targetInfo.adapterId;
        target.header.id = path.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS ||
            DisplayConfigGetDeviceInfo(&target.header) != ERROR_SUCCESS) {
            continue;
        }
        const std::string gdiName = Narrow(source.viewGdiDeviceName);
        for (DisplayInfo& d : *displays) {
            if (d.friendlyName.empty() && _stricmp(d.deviceName.c_str(), gdiName.c_str()) == 0) {
                d.friendlyName = Narrow(target.monitorFriendlyDeviceName);
            }
        }
    }
}

}  // namespace

std::vector<DisplayInfo> EnumerateDisplays(IDirect3D9Ex* d3d) {
    std::vector<DisplayInfo> displays;
    const UINT count = d3d->GetAdapterCount();
    for (UINT i = 0; i < count; i++) {
        DisplayInfo d;
        d.adapter = i;
        MONITORINFOEXA monitor = {};
        monitor.cbSize = sizeof(monitor);
        if (GetMonitorInfoA(d3d->GetAdapterMonitor(i), &monitor)) {
            d.deviceName = monitor.szDevice;
            d.rect = monitor.rcMonitor;
        } else {
            LOGERR("Display [%u]: its monitor information could not be read", i);
        }
        DEVMODEA mode = {};
        mode.dmSize = sizeof(mode);
        if (!d.deviceName.empty() &&
            EnumDisplaySettingsA(d.deviceName.c_str(), ENUM_CURRENT_SETTINGS, &mode) &&
            mode.dmDisplayFrequency > 1) {
            d.refreshHz = (int)mode.dmDisplayFrequency;
        }
        displays.push_back(d);
    }
    AddFriendlyNames(&displays);

    for (const DisplayInfo& d : displays) {
        LOG("Display [%u] %s (%s), %dx%d at (%ld,%ld), %d Hz", d.adapter, d.Name().c_str(),
            d.deviceName.c_str(), d.Width(), d.Height(), d.rect.left, d.rect.top, d.refreshHz);
    }
    return displays;
}
