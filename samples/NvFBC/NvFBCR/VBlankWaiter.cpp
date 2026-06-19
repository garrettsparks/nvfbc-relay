#include "VBlankWaiter.h"
#include <SimpleLogger.h>

// DXGI isn't in the project's link line (it's a D3D9 app); pull it in here so the helper is
// self-contained without touching all four build configs' AdditionalDependencies.
#pragma comment(lib, "dxgi.lib")

VBlankWaiter::VBlankWaiter() : m_output(nullptr) {}

VBlankWaiter::~VBlankWaiter() {
    if (m_output) {
        m_output->Release();
        m_output = nullptr;
    }
}

bool VBlankWaiter::Setup(HMONITOR monitor) {
    IDXGIFactory1* factory = nullptr;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory);
    if (FAILED(hr) || !factory) {
        LOGERR("VBlankWaiter: CreateDXGIFactory1 failed (0x%08x)", hr);
        return false;
    }

    // Match the target display by HMONITOR (D3D9 and DXGI adapter ordinals aren't guaranteed to
    // correspond, so identify by monitor handle, not ordinal).
    for (UINT a = 0; ; a++) {
        IDXGIAdapter1* adapter = nullptr;
        if (factory->EnumAdapters1(a, &adapter) == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        for (UINT o = 0; ; o++) {
            IDXGIOutput* output = nullptr;
            if (adapter->EnumOutputs(o, &output) == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            DXGI_OUTPUT_DESC desc = {};
            if (SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == monitor) {
                m_output = output;   // retain
                adapter->Release();
                factory->Release();
                LOG("VBlankWaiter: bound to target output at (%ld,%ld)-(%ld,%ld)",
                    desc.DesktopCoordinates.left, desc.DesktopCoordinates.top,
                    desc.DesktopCoordinates.right, desc.DesktopCoordinates.bottom);
                return true;
            }
            output->Release();
        }
        adapter->Release();
    }

    factory->Release();
    LOGERR("VBlankWaiter: no DXGI output matched the target monitor (0x%p)", (void*)monitor);
    return false;
}

bool VBlankWaiter::Wait() {
    if (!m_output) {
        return false;
    }
    HRESULT hr = m_output->WaitForVBlank();
    if (FAILED(hr)) {
        LOGERR("VBlankWaiter: WaitForVBlank failed (0x%08x) - output lost", hr);
        return false;
    }
    return true;
}
