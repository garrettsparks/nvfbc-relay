#include "Displays.h"

#include <SimpleLogger.h>

#include <cstdio>
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

// The graphics driver each display runs on, one line per distinct driver, so a log says which
// driver it was captured on. NVIDIA's own release number is the last five digits of the third
// and fourth parts of the Windows driver version.
void LogDrivers(IDirect3D9Ex* d3d, const std::vector<DisplayInfo>& displays) {
    std::vector<std::string> drivers;
    std::vector<std::string> shownOn;
    for (const DisplayInfo& d : displays) {
        D3DADAPTER_IDENTIFIER9 id = {};
        if (FAILED(d3d->GetAdapterIdentifier(d.adapter, 0, &id))) {
            LOGERR("Display [%u]: its graphics driver could not be identified", d.adapter);
            continue;
        }
        const unsigned product = HIWORD(id.DriverVersion.HighPart);
        const unsigned version = LOWORD(id.DriverVersion.HighPart);
        const unsigned subVersion = HIWORD(id.DriverVersion.LowPart);
        const unsigned build = LOWORD(id.DriverVersion.LowPart);
        char text[640];
        const int used = snprintf(text, sizeof(text), "%s, version %u.%u.%u.%u", id.Description,
                                  product, version, subVersion, build);
        if (id.VendorId == 0x10DE && used > 0 && used < (int)sizeof(text)) {
            const unsigned release = (subVersion % 10) * 10000 + build;
            snprintf(text + used, sizeof(text) - used, " (NVIDIA %u.%02u)", release / 100,
                     release % 100);
        }
        size_t k = 0;
        while (k < drivers.size() && drivers[k] != text) k++;
        if (k == drivers.size()) {
            drivers.push_back(text);
            shownOn.push_back("");
        }
        shownOn[k] += (shownOn[k].empty() ? "[" : " [") + std::to_string(d.adapter) + "]";
    }
    for (size_t k = 0; k < drivers.size(); k++) {
        LOG("Graphics driver for display %s: %s", shownOn[k].c_str(), drivers[k].c_str());
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
    LogDrivers(d3d, displays);
    return displays;
}
