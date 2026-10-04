#include "Failure.h"

#include <SimpleLogger.h>

#include <cstdarg>
#include <cstdio>

namespace {

std::string Format(const char* fmt, ...) {
    char text[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    return text;
}

void ShowPopup(const std::string& title, const std::string& text, UINT icon) {
    const std::string caption = "NvFBCR: " + title;
    MessageBoxA(NULL, text.c_str(), caption.c_str(),
                MB_OK | icon | MB_SETFOREGROUND | MB_TOPMOST);
}

}  // namespace

int ExitWithFailure(const Failure& failure) {
    std::string text = failure.text;
    // A failure without a log line was raised before the log may be opened, and opening it is
    // what truncates it, so the logger is not touched at all then.
    if (!failure.log.empty()) {
        SimpleLogger& log = SimpleLogger::getInstance();
        LOGERR("Stopping: %s", failure.log.c_str());
        log.flush();
        text += log.isEnabled() ? "\n\nThe reason is in NvFBCR.log."
                                : "\n\nTo record the reason, create an empty NvFBCR.log beside "
                                  "NvFBCR.exe and run again.";
    }
    ShowPopup(failure.title, text, MB_ICONERROR);
    return 1;
}

void ShowWarning(const char* title, const std::string& text) {
    ShowPopup(title, text, MB_ICONWARNING);
}

Failure AlreadyRunning() {
    return {"already running",
            "Another NvFBCR is already running, and only one can run at a time.\n\n"
            "Close it first. If no window is visible, end NvFBCR.exe in Task Manager.",
            ""};
}

Failure GraphicsUnavailable(HRESULT hr) {
    return {"could not start",
            "NvFBCR could not start the Windows graphics system it draws with (Direct3D 9).\n\n"
            "Restart the PC. If that does not help, update or reinstall the graphics driver.",
            Format("Direct3DCreate9Ex failed (0x%08lx)", (unsigned long)hr)};
}

Failure TooFewDisplays(size_t count) {
    return {"needs two displays",
            Format("NvFBCR shows one display's picture on another, so it needs two displays, "
                   "and Windows reports only %u.\n\n"
                   "Check that the capture card is connected and turned on, and that Windows "
                   "extends the desktop onto it (Settings, System, Display: Extend these "
                   "displays). Then start NvFBCR again.",
                   (unsigned)count),
            Format("%u display(s) enumerated; the relay needs a source and a target",
                   (unsigned)count)};
}

Failure NvFBCLibraryMissing() {
    return {"NVIDIA capture not found",
            "NvFBCR could not load NvFBC64.dll, the NVIDIA screen capture library that comes "
            "with the NVIDIA graphics driver.\n\n"
            "Install or update the NVIDIA graphics driver, then start NvFBCR again.",
            "NvFBC64.dll did not load, or lacks an export the relay needs"};
}

Failure CaptureOffNotAdmin() {
    return {"NVIDIA capture is off",
            "NVIDIA screen capture (NvFBC) is turned off on this PC, and turning it on needs "
            "administrator rights, which NvFBCR does not have.\n\n"
            "Sign in with an administrator account and start NvFBCR again, or ask an "
            "administrator to run NvFBCEnable.exe -enable once.",
            "NvFBC capture is not possible and the process is not elevated, so it cannot be "
            "enabled"};
}

Failure CaptureEnableFailed(NVFBCRESULT result) {
    return {"NVIDIA capture is off",
            Format("NVIDIA screen capture (NvFBC) is turned off on this PC, and NvFBCR could not "
                   "turn it on (error 0x%X).\n\n"
                   "Run NvFBCEnable.exe -enable, then start NvFBCR again. If that fails too, "
                   "update the NVIDIA graphics driver.",
                   (unsigned)result),
            Format("NvFBC_Enable failed (0x%X)", (unsigned)result)};
}

Failure CaptureStillOff() {
    return {"NVIDIA capture is off",
            "NvFBCR turned on NVIDIA screen capture and restarted, but capture is still not "
            "available.\n\n"
            "Restart the PC, then start NvFBCR again. If that does not help, update the NVIDIA "
            "graphics driver.",
            "NvFBC capture is still not possible after enabling it and relaunching; not "
            "relaunching again"};
}

Failure RelaunchFailed(DWORD error) {
    return {"could not restart",
            Format("NvFBCR turned on NVIDIA screen capture, but could not restart itself to use "
                   "it (error %lu).\n\n"
                   "Start NvFBCR again.",
                   (unsigned long)error),
            Format("relaunch after enabling NvFBC failed: CreateProcess error %lu",
                   (unsigned long)error)};
}

Failure WindowFailed(const char* which, DWORD error) {
    return {"could not start",
            Format("NvFBCR could not create its window on the capture card display (error "
                   "%lu).\n\n"
                   "Start NvFBCR again. If this keeps happening, restart the PC.",
                   (unsigned long)error),
            Format("could not create the %s (error %lu)", which, (unsigned long)error)};
}

Failure PresentDeviceFailed(const char* what, HRESULT hr) {
    return {"could not start",
            Format("NvFBCR could not set up drawing on the capture card display (error "
                   "0x%08lx).\n\n"
                   "Check that the capture card display is connected and turned on, then start "
                   "NvFBCR again. If this keeps happening, update the graphics driver.",
                   (unsigned long)hr),
            Format("%s failed (0x%08lx)", what, (unsigned long)hr)};
}

Failure FlipModeRefused(HRESULT hr) {
    return {"flip mode unavailable",
            Format("-flipex was asked for, but the graphics driver refused it (error 0x%08lx). "
                   "NvFBCR does not fall back to its usual output, because the capture would "
                   "then be labelled wrongly.\n\n"
                   "Start NvFBCR without -flipex.",
                   (unsigned long)hr),
            Format("flip mode REFUSED (0x%08lx); not falling back to bitblt, since a run "
                   "labelled flipex that presented through the old path would be worse than no "
                   "run",
                   (unsigned long)hr)};
}

Failure CaptureInUse(const char* where) {
    return {"capture in use",
            "Another program is using NVIDIA screen capture, so NvFBCR cannot capture the game "
            "display.\n\n"
            "Close other capture and streaming programs, and end any NvFBCR.exe still listed in "
            "Task Manager. Then start NvFBCR again.",
            Format("%s: NvFBC cannot create a session now (bCanCreateNow=false)", where)};
}

Failure CaptureCouldNotStart(const char* where) {
    return {"could not start",
            "NvFBCR could not start capturing the game display.\n\n"
            "Start NvFBCR again. If this keeps happening, restart the PC or update the NVIDIA "
            "graphics driver.",
            Format("%s: capture could not start", where)};
}

Failure ModeCouldNotStart(const char* modeName) {
    return {"could not start",
            "NvFBCR could not start the mode you chose.\n\n"
            "Start NvFBCR again, and press Enter at the mode prompt for the default mode.",
            Format("%s mode setup failed", modeName)};
}

Failure PresentPathRefused(const char* pathName, const char* advice) {
    std::string text = "NvFBCR could not set up its output on the capture card display, so it "
                       "will not start.";
    if (advice[0]) text += std::string("\n\n") + advice;
    return {"could not start", text, Format("%s init failed - refusing the mode", pathName)};
}

Failure CaptureLost(const char* where) {
    return {"stopped",
            "NvFBCR stopped because it lost the capture of the game display. This can happen "
            "when the game display changes resolution.\n\n"
            "Start NvFBCR again.",
            Format("%s: NvFBC session invalidated", where)};
}

Failure OutputStalled(const char* pathName) {
    return {"stopped",
            "NvFBCR stopped because the capture card display stopped taking new frames.\n\n"
            "Check that the capture card is connected and turned on, then start NvFBCR again. "
            "To try the older output method, type b:dwm at the mode prompt.",
            Format("%s: swapchain stalled - no paced present for seconds", pathName)};
}
