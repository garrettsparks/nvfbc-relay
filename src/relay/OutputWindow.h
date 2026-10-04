#pragma once

#include <windows.h>

#include "Displays.h"
#include "Failure.h"

// The relay's windows: the output window, a borderless topmost window covering the target
// display, and, when the output window belongs to a D3D11 flip-model swapchain, a hidden host
// window for the D3D9 devices. Flip model allows one swapchain per window and no second API on
// it, and a D3D9 device cannot exist without a window, so the devices move to the host.
struct OutputWindows {
    HWND output = NULL;
    HWND host = NULL;
};

MaybeFailure CreateOutputWindows(HINSTANCE instance, int showCommand, const DisplayInfo& target,
                                 bool withHost, OutputWindows* out);

// Destroys whichever of the windows still exist. The output window's destruction is what ends a
// run, so this also discards the quit request it leaves behind: a pending quit dismisses the
// next popup the moment it opens, and teardown may be followed by one.
void DestroyOutputWindows(OutputWindows* windows);

// Dispatches every message waiting for this thread. Returns false once the output window has
// closed, when the capture loop should end.
bool PumpMessages();
