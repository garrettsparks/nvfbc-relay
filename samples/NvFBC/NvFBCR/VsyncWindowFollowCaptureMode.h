#pragma once

#include "IFrameCaptureMode.h"
#include <string>

// VSync + Window Following capture mode
// Tracks a specific window and crops NVFBC capture to just that window
class VsyncWindowFollowCaptureMode : public IFrameCaptureMode {
public:
    VsyncWindowFollowCaptureMode(const std::string& windowTitle);
    virtual ~VsyncWindowFollowCaptureMode();

    virtual UINT GetPresentationInterval() const override;
    virtual bool Setup() override;
    virtual void Run(
        NvFBCToDx9Vid* nvfbcDx9,
        NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams,
        IDirect3DDevice9Ex* device,
        HWND hwnd) override;
    virtual const char* GetModeName() const override;

private:
    HWND m_targetWindow;
    std::string m_windowTitle;
    RECT m_lastWindowRect;
    bool m_windowFound;
    LARGE_INTEGER m_perfFreq;
    int m_captureWidth;
    int m_captureHeight;
    int m_lastLoggedWidth;
    int m_lastLoggedHeight;

    // Find window by partial title match
    static BOOL CALLBACK EnumWindowsCallback(HWND hwnd, LPARAM lParam);
    HWND FindWindowByTitle(const std::string& title);
    bool UpdateWindowTracking();
};