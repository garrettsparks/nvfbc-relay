#pragma once

#include <windows.h>
#include <d3d9.h>

#include "Failure.h"
#include "RelayContext.h"

MaybeFailure CreateDirect3D(IDirect3D9Ex** out);

// Windowed present parameters for a device on the given window. Every D3D9 device in the relay
// is windowed and differs only in these values: the present device, the ring's capture device
// and the diag mode's raster probe.
D3DPRESENT_PARAMETERS WindowedPresentParams(HWND window, int width, int height, D3DFORMAT format,
                                            UINT backBufferCount, D3DSWAPEFFECT swapEffect,
                                            UINT presentationInterval);

// Creates the present device on the given adapter for ctx->deviceWindow, sized ctx->width by
// ctx->height, and fetches its back buffer into ctx->backBuffer.
MaybeFailure CreatePresentDevice(RelayContext* ctx, UINT adapter, UINT presentationInterval);
