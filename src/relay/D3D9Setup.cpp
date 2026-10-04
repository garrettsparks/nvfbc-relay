#include "D3D9Setup.h"

#include <SimpleLogger.h>

MaybeFailure CreateDirect3D(IDirect3D9Ex** out) {
    *out = NULL;
    const HRESULT hr = Direct3DCreate9Ex(D3D_SDK_VERSION, out);
    if (FAILED(hr) || !*out) {
        *out = NULL;
        return GraphicsUnavailable(hr);
    }
    return std::nullopt;
}

D3DPRESENT_PARAMETERS WindowedPresentParams(HWND window, int width, int height, D3DFORMAT format,
                                            UINT backBufferCount, D3DSWAPEFFECT swapEffect,
                                            UINT presentationInterval) {
    D3DPRESENT_PARAMETERS params = {};
    params.Windowed = TRUE;
    params.hDeviceWindow = window;
    params.BackBufferWidth = (UINT)width;
    params.BackBufferHeight = (UINT)height;
    params.BackBufferFormat = format;
    params.BackBufferCount = backBufferCount;
    params.SwapEffect = swapEffect;
    params.PresentationInterval = presentationInterval;
    return params;
}

MaybeFailure CreatePresentDevice(RelayContext* ctx, UINT adapter, UINT presentationInterval) {
    // The bitblt swap effect converts a back buffer that does not match the display mode on
    // every present, so the relay keeps its 10-bit format there. Flip mode hands the buffers to
    // DWM with nowhere to convert, and the device is refused unless the format follows the mode.
    D3DFORMAT format = D3DFMT_A2R10G10B10;

    // Whether the back buffer matches the mode of the adapter it is shown on decides between a
    // clean present and a converted one, so it is logged on every run rather than deduced.
    D3DDISPLAYMODEEX mode = {};
    mode.Size = sizeof(mode);
    if (SUCCEEDED(ctx->d3d->GetAdapterDisplayModeEx(adapter, &mode, NULL))) {
        if (ctx->flipEx) format = mode.Format;
        const bool matches = mode.Format == format && mode.Width == (UINT)ctx->width &&
                             mode.Height == (UINT)ctx->height;
        LOG("Display mode on adapter %u: %ux%u @%uHz format %d; back buffer %dx%d format %d "
            "-> %s", adapter, mode.Width, mode.Height, mode.RefreshRate, (int)mode.Format,
            ctx->width, ctx->height, (int)format,
            matches ? "MATCH"
                    : "MISMATCH (expect S_PRESENT_MODE_CHANGED and a per-present convert)");
    } else {
        LOGERR("GetAdapterDisplayModeEx failed on adapter %u; cannot tell whether the back "
               "buffer matches the display mode%s", adapter,
               ctx->flipEx ? " and flip mode will probably be refused" : "");
    }

    D3DPRESENT_PARAMETERS params = WindowedPresentParams(
        ctx->deviceWindow, ctx->width, ctx->height, format, ctx->flipEx ? 2 : 1,
        ctx->flipEx ? D3DSWAPEFFECT_FLIPEX : D3DSWAPEFFECT_DISCARD, presentationInterval);
    // Multithreaded because the temporal modes call the device from the capture thread and the
    // present loop at once. Present statistics are only gathered when asked for at creation, and
    // without them the statistics read as zeroes, which looks like a sink that never missed.
    DWORD behavior = D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED;
    if (ctx->flipEx) behavior |= D3DCREATE_ENABLE_PRESENTSTATS;

    HRESULT hr = ctx->d3d->CreateDeviceEx(adapter, D3DDEVTYPE_HAL, ctx->deviceWindow, behavior,
                                          &params, NULL, &ctx->presentDevice);
    if (FAILED(hr)) {
        ctx->presentDevice = NULL;
        LOGERR("CreateDeviceEx failed (0x%08lx): %dx%d fmt %d, swap effect %d, %u back buffers, "
               "interval 0x%08x, behavior 0x%08lx", (unsigned long)hr, ctx->width, ctx->height,
               (int)params.BackBufferFormat, (int)params.SwapEffect, params.BackBufferCount,
               presentationInterval, (unsigned long)behavior);
        // A refused flip mode never falls back to bitblt: the run would be a bitblt run under a
        // flipex name, and the name outlives the log.
        if (ctx->flipEx) return FlipModeRefused(hr);
        return PresentDeviceFailed("CreateDeviceEx", hr);
    }

    // Flip mode queues presents instead of copying them, and with the default latency of 3 the
    // loop runs frames ahead of the display and the present stops being the pacing wait
    // (measured: 118 presents/s became 155-180/s, with triple the jitter). A latency of 1 makes
    // each present wait for the previous frame to be consumed.
    if (ctx->flipEx) {
        const HRESULT latency = ctx->presentDevice->SetMaximumFrameLatency(1);
        if (FAILED(latency)) {
            LOGERR("SetMaximumFrameLatency(1) failed (0x%08lx); flip mode will queue up to the "
                   "driver default and present pacing will not be trustworthy",
                   (unsigned long)latency);
        } else {
            UINT got = 0;
            ctx->presentDevice->GetMaximumFrameLatency(&got);
            LOG("Frame latency set to 1 for flip mode (device reports %u): PresentEx blocks "
                "until the previous frame is consumed", got);
        }
    }

    hr = ctx->presentDevice->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &ctx->backBuffer);
    if (FAILED(hr) || !ctx->backBuffer) {
        ctx->backBuffer = NULL;
        return PresentDeviceFailed("GetBackBuffer", hr);
    }
    return std::nullopt;
}
