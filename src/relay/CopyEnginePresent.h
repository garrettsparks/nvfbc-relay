#pragma once

#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <dcomp.h>
#include <utility>
#include <vector>

#include "FrameMarker.h"

// The D3D11 present's target with -wgc (unless -nocopyengine): a present that shows one frame as
// it is goes through the GPU's copy engine instead of its graphics engine
// (docs/copy-engine-present-spec.md).
//
// WHY. Every present of the D3D11 path took the game's graphics engine once, for the draw that
// goes in with it, and the game is switched out and back for each. A present with no GPU work
// behind it takes no visit, and neither does a copy on a D3D12 copy queue (measured beside a
// benchmark). A copy queue cannot write a DXGI swapchain's own buffers (the device is
// removed), so the output is a composition swapchain: the presentation manager presents any
// displayable texture, bound to the output window through DirectComposition.
//
// THREE DEVICES, ONE THREAD. The presentation manager needs a D3D11 device made single-threaded,
// and the D3D11 present's device is also used by the capture thread (it opens WGC's pool textures),
// so the presentation manager gets a device of its own that does nothing but present. A D3D12
// device holds the copy queue. The D3D11 present's device draws into the same buffers when a
// present cannot be a copy (a blend, a scaled frame, the marker). All of it runs on the present
// thread. Shared fences order the work: the presenting device waits on the copy queue's fence, or
// on the drawing device's, before each present.
//
// TWO BUFFERS, as the flip-model swapchain had: the buffer shown before the current one comes
// back when the next is shown, and waiting for it is the frame-pacing wait, at the card's refresh.
// A mode on a timer asks for THREE: with two, a GPU-bound game held every timer present a whole
// refresh for its buffer (Pandora, 2026-10-29: about 95% of presents held past half a refresh),
// so the card's refresh paced it again; a third gives the timer that refresh of slack.
//
// THE FRAME MARKER of a copied present is copied too: its strip is written on the CPU into an
// upload buffer, in the pixels the marker shader draws, and the copy queue puts it over the
// frame. The frame is copied around the strip's rectangle, so no two copies of one list write
// the same pixels and the list needs no barrier. A marked run therefore takes the path an
// unmarked one does.
//
// Everything Windows-version-specific is loaded at run time (d3d12.dll, dcomp.dll), so the
// relay's imports do not change.
namespace composition {

// The composition swapchain's interfaces, declared from the SDK's Presentation.idl (the MinGW
// headers have no presentation.h), in the IDL's method order. A method that returns a structure
// by value is declared with the pointer it fills in: MSVC and MinGW return those differently from
// a member function, and the pointer is what both pass underneath.
struct SystemInterruptTime { UINT64 value; };   // 100 ns units since the system started
struct PresentationTransform { float M11, M12, M21, M22, M31, M32; };
typedef UINT64 CompositionFrameId;
enum PresentStatisticsKind {
    PresentStatisticsKind_PresentStatus = 1,
    PresentStatisticsKind_CompositionFrame = 2,
    PresentStatisticsKind_IndependentFlipFrame = 3,
};
enum PresentStatus {
    PresentStatus_Queued = 0,
    PresentStatus_Skipped = 1,
    PresentStatus_Canceled = 2,
};

struct IPresentationBuffer : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetAvailableEvent(HANDLE* availableEventHandle) = 0;
    virtual HRESULT STDMETHODCALLTYPE IsAvailable(boolean* isAvailable) = 0;
};
struct IPresentationContent : public IUnknown {
    virtual void STDMETHODCALLTYPE SetTag(UINT_PTR tag) = 0;
};
struct IPresentationSurface : public IPresentationContent {
    virtual HRESULT STDMETHODCALLTYPE SetBuffer(IPresentationBuffer* presentationBuffer) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetColorSpace(DXGI_COLOR_SPACE_TYPE colorSpace) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetAlphaMode(DXGI_ALPHA_MODE alphaMode) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetSourceRect(const RECT* sourceRect) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetTransform(PresentationTransform* transform) = 0;
    virtual HRESULT STDMETHODCALLTYPE RestrictToOutput(IUnknown* output) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDisableReadback(boolean value) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetLetterboxingMargins(float l, float t, float r,
                                                             float b) = 0;
};
struct IPresentStatistics : public IUnknown {
    virtual UINT64 STDMETHODCALLTYPE GetPresentId() = 0;
    virtual PresentStatisticsKind STDMETHODCALLTYPE GetKind() = 0;
};
struct IPresentStatusPresentStatistics : public IPresentStatistics {
    virtual CompositionFrameId STDMETHODCALLTYPE GetCompositionFrameId() = 0;
    virtual PresentStatus STDMETHODCALLTYPE GetPresentStatus() = 0;
};
struct IIndependentFlipFramePresentStatistics : public IPresentStatistics {
    virtual void STDMETHODCALLTYPE GetOutputAdapterLUID(LUID* luid) = 0;
    virtual UINT STDMETHODCALLTYPE GetOutputVidPnSourceId() = 0;
    virtual UINT_PTR STDMETHODCALLTYPE GetContentTag() = 0;
    virtual void STDMETHODCALLTYPE GetDisplayedTime(SystemInterruptTime* time) = 0;
    virtual void STDMETHODCALLTYPE GetPresentDuration(SystemInterruptTime* duration) = 0;
};
struct IPresentationManager : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE AddBufferFromResource(IUnknown* resource,
                                                            IPresentationBuffer** buffer) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreatePresentationSurface(HANDLE compositionSurfaceHandle,
                                                                IPresentationSurface** surface) = 0;
    virtual UINT64 STDMETHODCALLTYPE GetNextPresentId() = 0;
    virtual HRESULT STDMETHODCALLTYPE SetTargetTime(SystemInterruptTime targetTime) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPreferredPresentDuration(
        SystemInterruptTime duration, SystemInterruptTime tolerance) = 0;
    virtual HRESULT STDMETHODCALLTYPE ForceVSyncInterrupt(boolean force) = 0;
    virtual HRESULT STDMETHODCALLTYPE Present() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPresentRetiringFence(REFIID riid, void** fence) = 0;
    virtual HRESULT STDMETHODCALLTYPE CancelPresentsFrom(UINT64 presentIdToCancelFrom) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetLostEvent(HANDLE* lostEventHandle) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPresentStatisticsAvailableEvent(HANDLE* handle) = 0;
    virtual HRESULT STDMETHODCALLTYPE EnablePresentStatisticsKind(PresentStatisticsKind kind,
                                                                  boolean enabled) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetNextPresentStatistics(IPresentStatistics** stats) = 0;
};
struct IPresentationFactory : public IUnknown {
    virtual boolean STDMETHODCALLTYPE IsPresentationSupported() = 0;
    virtual boolean STDMETHODCALLTYPE IsPresentationSupportedWithIndependentFlip() = 0;
    virtual HRESULT STDMETHODCALLTYPE CreatePresentationManager(IPresentationManager** manager) = 0;
};

}  // namespace composition

class CopyEngineTarget {
public:
    CopyEngineTarget() = default;
    ~CopyEngineTarget();

    CopyEngineTarget(const CopyEngineTarget&) = delete;
    CopyEngineTarget& operator=(const CopyEngineTarget&) = delete;

    // Builds the presenting device, the D3D12 copy queue and the buffers on the adapter of
    // drawDevice (the D3D11 present's device, which draws into the buffers when a present is not
    // a copy), and puts the buffers on hwnd. width, height and format are the output's;
    // sinkRefreshHz (0 when unknown) sizes the missed-refresh count; buffers is 2 or 3. Failure
    // is logged and leaves nothing usable.
    bool Setup(ID3D11Device* drawDevice, ID3D11DeviceContext* drawContext, HWND hwnd, int width,
               int height, DXGI_FORMAT format, int sinkRefreshHz, int buffers = 2);

    // The frame-pacing wait: blocks until the next buffer has come back from the display. 1
    // when it has, 0 on the timeout, -1 when the presentation manager was lost (it does not come
    // back).
    int WaitForBuffer(DWORD timeoutMs);

    // The next buffer as a render target on the drawing device.
    ID3D11RenderTargetView* NextTarget() const { return m_rtv[m_next]; }

    // True when source, a texture on the drawing device, can be copied into a buffer as it is:
    // the same size and format, and openable on the D3D12 device (once per texture, kept).
    bool CanCopy(ID3D11Texture2D* source);

    // Has the next PresentCopy put the frame marker with these cells over its copy.
    void MarkNextCopy(const bool cells[FrameMarker::kCells]);

    // Copies source into the next buffer on the copy queue, the marker strip with it when
    // MarkNextCopy asked for one, and presents it. CanCopy(source) must have been true.
    bool PresentCopy(ID3D11Texture2D* source);

    // Presents what the drawing device drew into the next buffer, after that work is done.
    bool PresentDrawn();

    // Whole-run counts, once at exit.
    void LogSummary() const;

private:
    bool CreateDevices(ID3D11Device* drawDevice);
    bool CreateBuffers(HWND hwnd, DXGI_FORMAT format);
    bool CreateFences();
    bool CreateMarkerUploads();
    void WriteMarkerStrip(int buffer);
    void RecordMarkedCopy(ID3D12Resource* from, int buffer);
    ID3D12Resource* OpenOnD3D12(ID3D11Texture2D* texture);
    bool Present();
    void DrainStatistics();
    void CountDisplayedGap(UINT64 shown);

    static const int kMaxBuffers = 3;
    int m_buffers = 2;                  // in use, 2 or 3 (Setup)

    int m_width = 0, m_height = 0;
    DXGI_FORMAT m_format = DXGI_FORMAT_UNKNOWN;
    double m_refreshTicks = 0.0;        // one refresh in QPC ticks; 0 when the rate is unknown

    ID3D11Device* m_drawDevice = NULL;          // borrowed
    ID3D11DeviceContext4* m_drawContext = NULL;
    ID3D11Device* m_presentDevice = NULL;       // single-threaded, presents only
    ID3D11DeviceContext4* m_presentContext = NULL;
    ID3D12Device* m_d3d12 = NULL;
    ID3D12CommandQueue* m_copyQueue = NULL;
    ID3D12CommandAllocator* m_alloc[kMaxBuffers] = {};
    ID3D12GraphicsCommandList* m_list = NULL;

    composition::IPresentationManager* m_manager = NULL;
    composition::IPresentationSurface* m_surface = NULL;
    IDCompositionDevice* m_dcomp = NULL;
    IDCompositionTarget* m_dcompTarget = NULL;
    IDCompositionVisual* m_dcompVisual = NULL;
    IUnknown* m_dcompContent = NULL;
    HANDLE m_surfaceHandle = NULL;
    HANDLE m_lost = NULL;
    HANDLE m_statsEvent = NULL;

    ID3D11Texture2D* m_buffer[kMaxBuffers] = {};                // on the presenting device
    composition::IPresentationBuffer* m_presentationBuffer[kMaxBuffers] = {};
    HANDLE m_available[kMaxBuffers] = {};
    ID3D11RenderTargetView* m_rtv[kMaxBuffers] = {};            // on the drawing device
    ID3D12Resource* m_buffer12[kMaxBuffers] = {};
    UINT64 m_allocDone[kMaxBuffers] = {};
    int m_next = 0;

    // The copy queue's fence (D3D12 signals, the presenting device waits) and the drawing
    // device's (it signals, the presenting device waits), each with its own count.
    ID3D12Fence* m_copyFence = NULL;
    ID3D11Fence* m_copyFenceOnPresent = NULL;
    UINT64 m_copyValue = 0;
    HANDLE m_copyEvent = NULL;
    ID3D11Fence* m_drawFence = NULL;
    ID3D11Fence* m_drawFenceOnPresent = NULL;
    UINT64 m_drawValue = 0;

    // Pool textures opened on the D3D12 device, NULL for one that could not be.
    std::vector<std::pair<ID3D11Texture2D*, ID3D12Resource*>> m_sources;

    // The marker strip: its rectangle (FrameMarker::StripRect, at the top left), one upload
    // buffer per output buffer, kept mapped, and the cells MarkNextCopy asked for.
    RECT m_strip = {0, 0, 0, 0};
    ID3D12Resource* m_stripUpload[kMaxBuffers] = {};
    BYTE* m_stripMapped[kMaxBuffers] = {};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT m_stripFootprint = {};
    bool m_markPending = false;
    bool m_markCells[FrameMarker::kCells] = {};

    LONGLONG m_lastBack = 0;            // QPC when a buffer last came back
    long long m_copies = 0, m_markedCopies = 0, m_draws = 0, m_presents = 0;
    long long m_presentFailures = 0, m_skipped = 0, m_canceled = 0, m_missedRefreshes = 0;
    long long m_flipFrames = 0, m_composedFrames = 0;   // how presents reached the screen
    double m_refreshUnits = 0.0;        // one refresh in SystemInterruptTime units; 0 when unknown
    UINT64 m_lastDisplayed = 0;         // when the last frame flipped to the display was shown
    long long m_displayedGaps = 0;      // displayed frames that came after a repeated refresh
    long long m_repeatedRefreshes = 0;  // refreshes those gaps held the old frame
    long long m_formatMismatches = 0;                   // presents drawn over a frame's format
    long long m_opened = 0, m_openFailures = 0;
    bool m_lostReported = false;
};
