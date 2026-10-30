#include "CopyEnginePresent.h"

#include <math.h>
#include <string.h>
#include <SimpleLogger.h>

namespace {

// The interface ids of the composition swapchain, from Presentation.idl.
const GUID kIidPresentationFactory =
    {0x8FB37B58, 0x1D74, 0x4F64, {0xA4, 0x9C, 0x1F, 0x97, 0xA8, 0x0A, 0x2E, 0xC0}};
const GUID kIidPresentStatusPresentStatistics =
    {0xC9ED2A41, 0x79CB, 0x435E, {0x96, 0x4E, 0xC8, 0x55, 0x30, 0x55, 0x42, 0x0C}};
const GUID kIidIndependentFlipFramePresentStatistics =
    {0x8C93BE27, 0xAD94, 0x4DA0, {0x8F, 0xD4, 0x24, 0x13, 0x13, 0x2D, 0x12, 0x4E}};

typedef HRESULT(WINAPI* CreatePresentationFactoryFn)(IUnknown*, REFIID, void**);
typedef HRESULT(WINAPI* DCompositionCreateDeviceFn)(IDXGIDevice*, REFIID, void**);
typedef HRESULT(WINAPI* DCompositionCreateSurfaceHandleFn)(DWORD, SECURITY_ATTRIBUTES*, HANDLE*);
typedef HRESULT(WINAPI* D3D12CreateDeviceFn)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);

// How long the present thread waits for the copy queue to finish with a command allocator
// before it is reused. With two buffers the copy two presents back finished long ago; this only
// bounds a GPU that stopped.
const DWORD kAllocatorWaitMs = 1000;

template <typename T> void Release(T*& p) {
    if (p) p->Release();
    p = NULL;
}

}  // namespace

CopyEngineTarget::~CopyEngineTarget() {
    if (m_copyQueue && m_copyFence && m_copyEvent) {
        // Nothing is released under a copy still in flight.
        m_copyQueue->Signal(m_copyFence, ++m_copyValue);
        if (SUCCEEDED(m_copyFence->SetEventOnCompletion(m_copyValue, m_copyEvent))) {
            WaitForSingleObject(m_copyEvent, kAllocatorWaitMs);
        }
    }
    for (auto& source : m_sources) Release(source.second);
    for (int i = 0; i < kMaxBuffers; i++) {
        Release(m_stripUpload[i]);
        Release(m_buffer12[i]);
        Release(m_rtv[i]);
        Release(m_presentationBuffer[i]);
        Release(m_buffer[i]);
    }
    Release(m_drawFenceOnPresent);
    Release(m_drawFence);
    Release(m_copyFenceOnPresent);
    Release(m_copyFence);
    if (m_copyEvent) CloseHandle(m_copyEvent);
    Release(m_dcompContent);
    Release(m_dcompVisual);
    Release(m_dcompTarget);
    Release(m_dcomp);
    Release(m_surface);
    Release(m_manager);
    if (m_surfaceHandle) CloseHandle(m_surfaceHandle);
    Release(m_list);
    for (int i = 0; i < kMaxBuffers; i++) Release(m_alloc[i]);
    Release(m_copyQueue);
    Release(m_d3d12);
    Release(m_presentContext);
    Release(m_presentDevice);
    Release(m_drawContext);
}

bool CopyEngineTarget::Setup(ID3D11Device* drawDevice, ID3D11DeviceContext* drawContext,
                             HWND hwnd, int width, int height, DXGI_FORMAT format,
                             int sinkRefreshHz, int buffers) {
    m_buffers = buffers < 2 ? 2 : buffers;
    if (m_buffers > kMaxBuffers) m_buffers = kMaxBuffers;
    m_width = width;
    m_height = height;
    m_format = format;
    m_drawDevice = drawDevice;
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    if (sinkRefreshHz > 0) {
        m_refreshTicks = (double)freq.QuadPart / (double)sinkRefreshHz;
        m_refreshUnits = 10000000.0 / (double)sinkRefreshHz;
    }
    HRESULT hr = drawContext->QueryInterface(__uuidof(ID3D11DeviceContext4),
                                             (void**)&m_drawContext);
    if (FAILED(hr)) {
        LOGERR("Copy engine: the present's context has no fence support (0x%08lx)", hr);
        return false;
    }
    if (!CreateDevices(drawDevice)) return false;
    if (!CreateBuffers(hwnd, format)) return false;
    if (!CreateFences()) return false;
    m_strip = FrameMarker::StripRect(width, height);
    if (!CreateMarkerUploads()) return false;
    LOG("Copy engine: presents through the composition swapchain, %d buffers %dx%d; a present "
        "of one frame as it is is copied on the D3D12 copy queue, any other is drawn",
        m_buffers, width, height);
    return true;
}

bool CopyEngineTarget::CreateDevices(ID3D11Device* drawDevice) {
    IDXGIDevice* dxgiDevice = NULL;
    IDXGIAdapter* adapter = NULL;
    HRESULT hr = drawDevice->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice);
    if (SUCCEEDED(hr)) hr = dxgiDevice->GetAdapter(&adapter);
    Release(dxgiDevice);
    if (FAILED(hr)) {
        LOGERR("Copy engine: the present's adapter could not be read (0x%08lx)", hr);
        return false;
    }

    // The presentation manager refuses a device without these two flags.
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_SINGLETHREADED |
                       D3D11_CREATE_DEVICE_PREVENT_INTERNAL_THREADING_OPTIMIZATIONS;
    ID3D11DeviceContext* context = NULL;
    hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, NULL, flags, NULL, 0,
                           D3D11_SDK_VERSION, &m_presentDevice, NULL, &context);
    if (SUCCEEDED(hr)) {
        hr = context->QueryInterface(__uuidof(ID3D11DeviceContext4), (void**)&m_presentContext);
    }
    Release(context);
    if (FAILED(hr)) {
        Release(adapter);
        LOGERR("Copy engine: the presenting device could not be made (0x%08lx)", hr);
        return false;
    }

    HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
    D3D12CreateDeviceFn createDevice =
        d3d12 ? (D3D12CreateDeviceFn)(void*)GetProcAddress(d3d12, "D3D12CreateDevice") : NULL;
    hr = createDevice ? createDevice(adapter, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device),
                                     (void**)&m_d3d12)
                      : E_NOINTERFACE;
    Release(adapter);
    if (FAILED(hr)) {
        LOGERR("Copy engine: no D3D12 device for the copy queue (0x%08lx)", hr);
        return false;
    }
    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_COPY;
    hr = m_d3d12->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), (void**)&m_copyQueue);
    for (int i = 0; i < m_buffers && SUCCEEDED(hr); i++) {
        hr = m_d3d12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY,
                                             __uuidof(ID3D12CommandAllocator), (void**)&m_alloc[i]);
    }
    if (SUCCEEDED(hr)) {
        hr = m_d3d12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, m_alloc[0], NULL,
                                        __uuidof(ID3D12GraphicsCommandList), (void**)&m_list);
    }
    if (SUCCEEDED(hr)) hr = m_list->Close();
    if (FAILED(hr)) {
        LOGERR("Copy engine: the copy queue could not be made (0x%08lx)", hr);
        return false;
    }
    return true;
}

bool CopyEngineTarget::CreateBuffers(HWND hwnd, DXGI_FORMAT format) {
    HMODULE dcomp = LoadLibraryW(L"dcomp.dll");
    CreatePresentationFactoryFn createFactory = NULL;
    DCompositionCreateDeviceFn createDcomp = NULL;
    DCompositionCreateSurfaceHandleFn createSurfaceHandle = NULL;
    if (dcomp) {
        createFactory = (CreatePresentationFactoryFn)(void*)GetProcAddress(
            dcomp, "CreatePresentationFactory");
        createDcomp = (DCompositionCreateDeviceFn)(void*)GetProcAddress(
            dcomp, "DCompositionCreateDevice");
        createSurfaceHandle = (DCompositionCreateSurfaceHandleFn)(void*)GetProcAddress(
            dcomp, "DCompositionCreateSurfaceHandle");
    }
    if (!createFactory || !createDcomp || !createSurfaceHandle) {
        LOGERR("Copy engine: the composition swapchain is not available in dcomp.dll");
        return false;
    }
    composition::IPresentationFactory* factory = NULL;
    HRESULT hr = createFactory(m_presentDevice, kIidPresentationFactory, (void**)&factory);
    if (FAILED(hr) || !factory) {
        LOGERR("Copy engine: CreatePresentationFactory failed (0x%08lx)", hr);
        return false;
    }
    const bool supported = factory->IsPresentationSupported() != 0;
    const bool independentFlip = factory->IsPresentationSupportedWithIndependentFlip() != 0;
    LOG("Copy engine: composition swapchain supported %s, with independent flip %s",
        supported ? "yes" : "no", independentFlip ? "yes" : "no");
    if (supported) hr = factory->CreatePresentationManager(&m_manager);
    Release(factory);
    if (!supported || FAILED(hr)) {
        LOGERR("Copy engine: no presentation manager (0x%08lx)", hr);
        return false;
    }
    hr = createSurfaceHandle(COMPOSITIONOBJECT_ALL_ACCESS, NULL, &m_surfaceHandle);
    if (SUCCEEDED(hr)) hr = m_manager->CreatePresentationSurface(m_surfaceHandle, &m_surface);
    if (FAILED(hr)) {
        LOGERR("Copy engine: the presentation surface could not be made (0x%08lx)", hr);
        return false;
    }

    // The buffers live on the presenting device; the drawing device and the D3D12 device open
    // them through one NT handle each.
    ID3D11Device1* drawDevice1 = NULL;
    hr = m_drawDevice->QueryInterface(__uuidof(ID3D11Device1), (void**)&drawDevice1);
    if (FAILED(hr)) {
        LOGERR("Copy engine: the present's device cannot open shared buffers (0x%08lx)", hr);
        return false;
    }
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = m_width;
    td.Height = m_height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = format;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE |
                   D3D11_RESOURCE_MISC_SHARED_DISPLAYABLE;
    for (int i = 0; i < m_buffers && SUCCEEDED(hr); i++) {
        hr = m_presentDevice->CreateTexture2D(&td, NULL, &m_buffer[i]);
        if (SUCCEEDED(hr)) {
            hr = m_manager->AddBufferFromResource(m_buffer[i], &m_presentationBuffer[i]);
        }
        if (SUCCEEDED(hr)) hr = m_presentationBuffer[i]->GetAvailableEvent(&m_available[i]);
        IDXGIResource1* resource = NULL;
        HANDLE handle = NULL;
        ID3D11Texture2D* onDraw = NULL;
        if (SUCCEEDED(hr)) {
            hr = m_buffer[i]->QueryInterface(__uuidof(IDXGIResource1), (void**)&resource);
        }
        if (SUCCEEDED(hr)) {
            hr = resource->CreateSharedHandle(
                NULL, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, NULL, &handle);
        }
        if (SUCCEEDED(hr)) {
            hr = drawDevice1->OpenSharedResource1(handle, __uuidof(ID3D11Texture2D),
                                                  (void**)&onDraw);
        }
        if (SUCCEEDED(hr)) hr = m_drawDevice->CreateRenderTargetView(onDraw, NULL, &m_rtv[i]);
        if (SUCCEEDED(hr)) {
            hr = m_d3d12->OpenSharedHandle(handle, __uuidof(ID3D12Resource),
                                           (void**)&m_buffer12[i]);
        }
        if (SUCCEEDED(hr)) {
            // Black until the first present draws or copies over it.
            const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            m_drawContext->ClearRenderTargetView(m_rtv[i], black);
        }
        Release(onDraw);
        Release(resource);
        if (handle) CloseHandle(handle);
    }
    Release(drawDevice1);
    if (FAILED(hr)) {
        LOGERR("Copy engine: the displayable buffers could not be made and shared (0x%08lx)", hr);
        return false;
    }
    m_drawContext->Flush();

    // The surface as the output window's content, through DirectComposition.
    IDXGIDevice* dxgiDevice = NULL;
    hr = m_presentDevice->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice);
    if (SUCCEEDED(hr)) {
        hr = createDcomp(dxgiDevice, __uuidof(IDCompositionDevice), (void**)&m_dcomp);
    }
    Release(dxgiDevice);
    if (SUCCEEDED(hr)) hr = m_dcomp->CreateTargetForHwnd(hwnd, TRUE, &m_dcompTarget);
    if (SUCCEEDED(hr)) hr = m_dcomp->CreateVisual(&m_dcompVisual);
    if (SUCCEEDED(hr)) hr = m_dcomp->CreateSurfaceFromHandle(m_surfaceHandle, &m_dcompContent);
    if (SUCCEEDED(hr)) hr = m_dcompVisual->SetContent(m_dcompContent);
    if (SUCCEEDED(hr)) hr = m_dcompTarget->SetRoot(m_dcompVisual);
    if (SUCCEEDED(hr)) hr = m_dcomp->Commit();
    if (FAILED(hr)) {
        LOGERR("Copy engine: the surface could not be put on the output window (0x%08lx)", hr);
        return false;
    }
    m_surface->SetAlphaMode(DXGI_ALPHA_MODE_IGNORE);
    m_surface->SetColorSpace(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
    RECT source = {0, 0, m_width, m_height};
    m_surface->SetSourceRect(&source);
    m_manager->EnablePresentStatisticsKind(composition::PresentStatisticsKind_PresentStatus, TRUE);
    // How each present reached the screen: flipped straight to it, or composed by Windows (a
    // buffer the display cannot take as it is, such as one of another format).
    m_manager->EnablePresentStatisticsKind(
        composition::PresentStatisticsKind_IndependentFlipFrame, TRUE);
    m_manager->EnablePresentStatisticsKind(composition::PresentStatisticsKind_CompositionFrame,
                                           TRUE);
    m_manager->GetLostEvent(&m_lost);
    m_manager->GetPresentStatisticsAvailableEvent(&m_statsEvent);
    return true;
}

bool CopyEngineTarget::CreateFences() {
    ID3D11Device5* presentDevice5 = NULL;
    ID3D11Device5* drawDevice5 = NULL;
    HANDLE copyHandle = NULL, drawHandle = NULL;
    HRESULT hr = m_presentDevice->QueryInterface(__uuidof(ID3D11Device5), (void**)&presentDevice5);
    if (SUCCEEDED(hr)) {
        hr = m_drawDevice->QueryInterface(__uuidof(ID3D11Device5), (void**)&drawDevice5);
    }
    if (SUCCEEDED(hr)) {
        hr = m_d3d12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, __uuidof(ID3D12Fence),
                                  (void**)&m_copyFence);
    }
    if (SUCCEEDED(hr)) {
        hr = m_d3d12->CreateSharedHandle(m_copyFence, NULL, GENERIC_ALL, NULL, &copyHandle);
    }
    if (SUCCEEDED(hr)) {
        hr = presentDevice5->OpenSharedFence(copyHandle, __uuidof(ID3D11Fence),
                                             (void**)&m_copyFenceOnPresent);
    }
    if (SUCCEEDED(hr)) {
        hr = drawDevice5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, __uuidof(ID3D11Fence),
                                      (void**)&m_drawFence);
    }
    if (SUCCEEDED(hr)) hr = m_drawFence->CreateSharedHandle(NULL, GENERIC_ALL, NULL, &drawHandle);
    if (SUCCEEDED(hr)) {
        hr = presentDevice5->OpenSharedFence(drawHandle, __uuidof(ID3D11Fence),
                                             (void**)&m_drawFenceOnPresent);
    }
    if (copyHandle) CloseHandle(copyHandle);
    if (drawHandle) CloseHandle(drawHandle);
    Release(presentDevice5);
    Release(drawDevice5);
    m_copyEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (FAILED(hr) || !m_copyEvent) {
        LOGERR("Copy engine: the shared fences could not be made (0x%08lx)", hr);
        return false;
    }
    return true;
}

bool CopyEngineTarget::CreateMarkerUploads() {
    D3D12_RESOURCE_DESC strip = {};
    strip.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    strip.Width = m_strip.right - m_strip.left;
    strip.Height = m_strip.bottom - m_strip.top;
    strip.DepthOrArraySize = 1;
    strip.MipLevels = 1;
    strip.Format = m_format;
    strip.SampleDesc.Count = 1;
    UINT64 bytes = 0;
    m_d3d12->GetCopyableFootprints(&strip, 0, 1, 0, &m_stripFootprint, NULL, NULL, &bytes);

    D3D12_HEAP_PROPERTIES upload = {};
    upload.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = bytes;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    HRESULT hr = S_OK;
    for (int i = 0; i < m_buffers && SUCCEEDED(hr); i++) {
        hr = m_d3d12->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &bd,
                                              D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                                              __uuidof(ID3D12Resource), (void**)&m_stripUpload[i]);
        // An upload heap may stay mapped for its whole life.
        if (SUCCEEDED(hr)) hr = m_stripUpload[i]->Map(0, NULL, (void**)&m_stripMapped[i]);
    }
    if (FAILED(hr)) {
        LOGERR("Copy engine: the marker strip's upload buffers could not be made (0x%08lx)", hr);
        return false;
    }
    return true;
}

void CopyEngineTarget::MarkNextCopy(const bool cells[FrameMarker::kCells]) {
    for (int i = 0; i < FrameMarker::kCells; i++) m_markCells[i] = cells[i];
    m_markPending = true;
}

// The strip in the pixels the D3D11 present's marker shader draws over the same rectangle: the
// grid's cell row white where a cell is set, everything else black, opaque. A pixel belongs to
// the cell its centre falls in, as the shader's uv does.
void CopyEngineTarget::WriteMarkerStrip(int buffer) {
    const int width = m_strip.right - m_strip.left;
    const int height = m_strip.bottom - m_strip.top;
    const UINT32 white = 0xFFFFFFFFu, black = 0xFF000000u;
    BYTE* base = m_stripMapped[buffer];
    const UINT pitch = m_stripFootprint.Footprint.RowPitch;
    // Every row is one of two: all black, or the cell row's pattern.
    std::vector<UINT32> cellRow((size_t)width), blackRow((size_t)width, black);
    for (int x = 0; x < width; x++) {
        const int cx = (int)(((double)x + 0.5) * FrameMarker::kGridW / width);
        const bool set = cx >= 1 && cx <= FrameMarker::kCells && m_markCells[cx - 1];
        cellRow[x] = set ? white : black;
    }
    for (int y = 0; y < height; y++) {
        const int cy = (int)(((double)y + 0.5) * FrameMarker::kGridH / height);
        const std::vector<UINT32>& row = cy == 1 ? cellRow : blackRow;
        memcpy(base + (size_t)pitch * y, row.data(), (size_t)width * sizeof(UINT32));
    }
}

// The frame around the strip's rectangle (below it, then to its right), and the strip into it,
// so the three copies write separate pixels.
void CopyEngineTarget::RecordMarkedCopy(ID3D12Resource* from, int buffer) {
    const UINT stripW = (UINT)(m_strip.right - m_strip.left);
    const UINT stripH = (UINT)(m_strip.bottom - m_strip.top);
    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = m_buffer12[buffer];
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = from;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    const D3D12_BOX below = {0, stripH, 0, (UINT)m_width, (UINT)m_height, 1};
    m_list->CopyTextureRegion(&dst, 0, stripH, 0, &src, &below);
    const D3D12_BOX right = {stripW, 0, 0, (UINT)m_width, stripH, 1};
    m_list->CopyTextureRegion(&dst, stripW, 0, 0, &src, &right);
    D3D12_TEXTURE_COPY_LOCATION strip = {};
    strip.pResource = m_stripUpload[buffer];
    strip.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    strip.PlacedFootprint = m_stripFootprint;
    m_list->CopyTextureRegion(&dst, 0, 0, 0, &strip, NULL);
}

int CopyEngineTarget::WaitForBuffer(DWORD timeoutMs) {
    HANDLE waits[2] = {m_lost, m_available[m_next]};
    const DWORD r = WaitForMultipleObjects(2, waits, FALSE, timeoutMs);
    if (r == WAIT_OBJECT_0) {
        if (!m_lostReported) {
            LOGERR("Copy engine: the presentation manager was lost; nothing more can be shown");
            m_lostReported = true;
        }
        return -1;
    }
    if (r != WAIT_OBJECT_0 + 1) return 0;
    // A buffer comes back when the one after it is shown, so the time between two coming back
    // is the time a picture stayed on the display: more than one refresh is a refresh that
    // showed no new picture. This reads the moment the wait ended, so it counts only while the
    // wait really waits: a timer that finds its buffer back already reads its own period here.
    // The summary's displayed count has no such limit.
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (m_lastBack != 0 && m_refreshTicks > 0.0) {
        const double refreshes = (double)(now.QuadPart - m_lastBack) / m_refreshTicks;
        if (refreshes >= 1.5) m_missedRefreshes += (long long)floor(refreshes + 0.5) - 1;
    }
    m_lastBack = now.QuadPart;
    return 1;
}

ID3D12Resource* CopyEngineTarget::OpenOnD3D12(ID3D11Texture2D* texture) {
    for (const auto& source : m_sources) {
        if (source.first == texture) return source.second;
    }
    ID3D12Resource* opened = NULL;
    IDXGIResource1* resource = NULL;
    HANDLE handle = NULL;
    HRESULT hr = texture->QueryInterface(__uuidof(IDXGIResource1), (void**)&resource);
    if (SUCCEEDED(hr)) {
        hr = resource->CreateSharedHandle(NULL, DXGI_SHARED_RESOURCE_READ, NULL, &handle);
    }
    if (SUCCEEDED(hr)) {
        hr = m_d3d12->OpenSharedHandle(handle, __uuidof(ID3D12Resource), (void**)&opened);
    }
    if (handle) CloseHandle(handle);
    Release(resource);
    // The first of each is logged and the summary counts them all: a pool makes every texture
    // alike, so one texture's result stands for the rest.
    if (FAILED(hr)) {
        opened = NULL;
        m_openFailures++;
        if (m_openFailures == 1) {
            LOGERR("Copy engine: a capture texture could not be opened on the D3D12 device "
                   "(0x%08lx); its presents are drawn", hr);
        }
    } else if (m_opened++ == 0) {
        LOG("Copy engine: the first capture texture opened on the D3D12 device");
    }
    m_sources.push_back(std::make_pair(texture, opened));   // the pool keeps its texture alive
    return opened;
}

bool CopyEngineTarget::CanCopy(ID3D11Texture2D* source) {
    if (!source) return false;
    D3D11_TEXTURE2D_DESC d;
    source->GetDesc(&d);
    if ((int)d.Width != m_width || (int)d.Height != m_height) return false;
    // The buffers are made in the frames' format, so a frame of another format is a fault
    // (a frame source this code does not know), not a choice: it is drawn, counted and said.
    if (d.Format != m_format) {
        if (m_formatMismatches++ == 0) {
            LOGERR("Copy engine: a frame in DXGI format %d is not the buffers' format %d; such "
                   "frames are drawn", (int)d.Format, (int)m_format);
        }
        return false;
    }
    return OpenOnD3D12(source) != NULL;
}

bool CopyEngineTarget::PresentCopy(ID3D11Texture2D* source) {
    ID3D12Resource* from = OpenOnD3D12(source);
    if (!from) return PresentDrawn();
    // Each buffer has its own allocator, free again once the copy into that buffer two presents
    // back has finished.
    const int a = m_next;
    if (m_copyFence->GetCompletedValue() < m_allocDone[a] &&
        SUCCEEDED(m_copyFence->SetEventOnCompletion(m_allocDone[a], m_copyEvent))) {
        WaitForSingleObject(m_copyEvent, kAllocatorWaitMs);
    }
    HRESULT hr = m_alloc[a]->Reset();
    if (SUCCEEDED(hr)) hr = m_list->Reset(m_alloc[a], NULL);
    if (FAILED(hr)) {
        LOGERR("Copy engine: the copy list could not be reset (0x%08lx)", hr);
        return false;
    }
    if (m_markPending) {
        WriteMarkerStrip(a);
        RecordMarkedCopy(from, a);
        m_markPending = false;
        m_markedCopies++;
    } else {
        m_list->CopyResource(m_buffer12[a], from);
    }
    m_list->Close();
    ID3D12CommandList* lists[1] = {m_list};
    m_copyQueue->ExecuteCommandLists(1, lists);
    m_copyQueue->Signal(m_copyFence, ++m_copyValue);
    m_allocDone[a] = m_copyValue;
    m_presentContext->Wait(m_copyFenceOnPresent, m_copyValue);
    m_copies++;
    return Present();
}

bool CopyEngineTarget::PresentDrawn() {
    m_drawContext->Signal(m_drawFence, ++m_drawValue);
    m_drawContext->Flush();
    m_presentContext->Wait(m_drawFenceOnPresent, m_drawValue);
    m_draws++;
    return Present();
}

bool CopyEngineTarget::Present() {
    m_surface->SetBuffer(m_presentationBuffer[m_next]);
    const HRESULT hr = m_manager->Present();
    m_next = (m_next + 1) % m_buffers;
    DrainStatistics();
    if (FAILED(hr)) {
        if (m_presentFailures++ == 0) LOGERR("Copy engine: Present failed (0x%08lx)", hr);
        return false;
    }
    m_presents++;
    return true;
}

void CopyEngineTarget::DrainStatistics() {
    while (m_statsEvent && WaitForSingleObject(m_statsEvent, 0) == WAIT_OBJECT_0) {
        composition::IPresentStatistics* stats = NULL;
        if (FAILED(m_manager->GetNextPresentStatistics(&stats)) || !stats) break;
        const composition::PresentStatisticsKind kind = stats->GetKind();
        if (kind == composition::PresentStatisticsKind_IndependentFlipFrame) {
            m_flipFrames++;
            composition::IIndependentFlipFramePresentStatistics* flip = NULL;
            if (SUCCEEDED(stats->QueryInterface(kIidIndependentFlipFramePresentStatistics,
                                                (void**)&flip))) {
                composition::SystemInterruptTime shown = {};
                flip->GetDisplayedTime(&shown);
                flip->Release();
                CountDisplayedGap(shown.value);
            }
        } else if (kind == composition::PresentStatisticsKind_CompositionFrame) {
            m_composedFrames++;
            m_lastDisplayed = 0;   // its time is not read, so the next gap has no start
        }
        if (kind == composition::PresentStatisticsKind_PresentStatus) {
            composition::IPresentStatusPresentStatistics* status = NULL;
            if (SUCCEEDED(stats->QueryInterface(kIidPresentStatusPresentStatistics,
                                                (void**)&status))) {
                const composition::PresentStatus s = status->GetPresentStatus();
                if (s == composition::PresentStatus_Skipped) m_skipped++;
                else if (s == composition::PresentStatus_Canceled) m_canceled++;
                status->Release();
            }
        }
        stats->Release();
    }
}

void CopyEngineTarget::CountDisplayedGap(UINT64 shown) {
    if (m_lastDisplayed != 0 && m_refreshUnits > 0.0) {
        const double refreshes = (double)(shown - m_lastDisplayed) / m_refreshUnits;
        if (refreshes >= 1.5) {
            m_displayedGaps++;
            m_repeatedRefreshes += (long long)floor(refreshes + 0.5) - 1;
            LOG("display gap: %.2f refreshes between two frames on the display", refreshes);
        }
    }
    m_lastDisplayed = shown;
}

void CopyEngineTarget::LogSummary() const {
    LOG("copy engine displayed: %lld refreshes held a frame over, in %lld gaps (from the times "
        "the display reported)", m_repeatedRefreshes, m_displayedGaps);
    LOG("copy engine summary: %lld presents (%lld copied on the copy engine, %lld of them with "
        "the marker, %lld drawn), %lld failed, %lld skipped and %lld canceled by the "
        "presentation manager; %lld refreshes showed no new frame (from when the buffers came "
        "back); %lld capture textures opened on D3D12, %lld could not be",
        m_presents, m_copies, m_markedCopies, m_draws, m_presentFailures, m_skipped, m_canceled,
        m_missedRefreshes, m_opened, m_openFailures);
    LOG("copy engine presentation: %lld frames flipped straight to the display, %lld composed "
        "by Windows; %lld presents drawn because the frame's format was not the buffers'",
        m_flipFrames, m_composedFrames, m_formatMismatches);
}
