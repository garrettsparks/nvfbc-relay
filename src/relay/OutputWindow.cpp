#include "OutputWindow.h"

#include <SimpleLogger.h>

namespace {

const char kWindowClass[] = "NvFBCR output";

HWND g_outputWindow = NULL;

LRESULT CALLBACK RelayWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    // Closing the output window, by Alt+F4 or from outside, is how a run ends normally. The host
    // window is never shown, and its destruction at teardown must not ask for a quit.
    if (message == WM_DESTROY && window == g_outputWindow) {
        g_outputWindow = NULL;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(window, message, wParam, lParam);
}

}  // namespace

MaybeFailure CreateOutputWindows(HINSTANCE instance, int showCommand, const DisplayInfo& target,
                                 bool withHost, OutputWindows* out) {
    WNDCLASSEXA windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = RelayWindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorA(NULL, IDC_ARROW);
    windowClass.lpszClassName = kWindowClass;
    if (!RegisterClassExA(&windowClass)) {
        return WindowFailed("window class", GetLastError());
    }

    out->output = CreateWindowExA(WS_EX_TOPMOST, kWindowClass, "NvFBCR", WS_POPUP,
                                  target.rect.left, target.rect.top, target.Width(),
                                  target.Height(), NULL, NULL, instance, NULL);
    if (!out->output) {
        return WindowFailed("output window", GetLastError());
    }
    g_outputWindow = out->output;
    ShowWindow(out->output, showCommand);

    if (withHost) {
        out->host = CreateWindowExA(0, kWindowClass, "NvFBCR D3D9 host", WS_POPUP, 0, 0, 1, 1,
                                    NULL, NULL, instance, NULL);
        if (!out->host) {
            return WindowFailed("D3D9 host window", GetLastError());
        }
        LOG("D3D9 devices hosted on a hidden window; the output window is reserved for the "
            "D3D11 flip-model swapchain");
    }
    return std::nullopt;
}

void DestroyOutputWindows(OutputWindows* windows) {
    if (windows->host) {
        DestroyWindow(windows->host);
        windows->host = NULL;
    }
    if (windows->output) {
        if (IsWindow(windows->output)) DestroyWindow(windows->output);
        windows->output = NULL;
    }
    MSG quit;
    while (PeekMessageA(&quit, NULL, WM_QUIT, WM_QUIT, PM_REMOVE)) {
    }
}

bool PumpMessages() {
    MSG message;
    while (PeekMessageA(&message, NULL, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT) return false;
        TranslateMessage(&message);
        DispatchMessageA(&message);
    }
    return true;
}
