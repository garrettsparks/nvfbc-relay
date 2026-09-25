#pragma once

#include <windows.h>
#include <NvFBCApi.h>

#include <optional>
#include <string>

// Why the relay is stopping, told twice: the popup's plain words for the person at the PC (what
// failed, then what to do), and the log line's detail for whoever reads NvFBCR.log. Every
// popup's wording is built in Failure.cpp, so the user-facing text can be read in one place.
struct Failure {
    std::string title;   // shown after "NvFBCR: " in the popup's title bar
    std::string text;
    std::string log;     // empty only for a failure raised before the log may be opened
};

using MaybeFailure = std::optional<Failure>;

// Logs "Stopping: <log>", flushes the log, shows the popup with a sentence on where the reason
// is recorded, and returns the exit code. WinMain calls it after teardown and nothing else ends
// the process with an error, so an exit cannot be written without its popup.
int ExitWithFailure(const Failure& failure);

// A popup for a problem that does not stop the run.
void ShowWarning(const char* title, const std::string& text);

Failure AlreadyRunning();
Failure GraphicsUnavailable(HRESULT hr);
Failure TooFewDisplays(size_t count);
Failure NvFBCLibraryMissing();
Failure CaptureOffNotAdmin();
Failure CaptureEnableFailed(NVFBCRESULT result);
Failure CaptureStillOff();
Failure RelaunchFailed(DWORD error);
Failure WindowFailed(const char* which, DWORD error);
Failure PresentDeviceFailed(const char* what, HRESULT hr);
Failure FlipModeRefused(HRESULT hr);
Failure CaptureInUse(const char* where);
Failure CaptureCouldNotStart(const char* where);
Failure ModeCouldNotStart(const char* modeName);
Failure PresentPathRefused(const char* pathName, const char* advice);
Failure CaptureLost(const char* where);
Failure OutputStalled(const char* pathName);
