#pragma once

// Windows.Graphics.Capture (WGC) as the ring's capture source, asked for with -wgc on a mode of
// the D3D11 present. A WGC session delivers the source display's frames into a frame pool, and
// every frame the ring stores stays in the pool for as long as its slot names it. The pool is on
// a device of its
// own, made on the D3D11 present's adapter, and the present opens each of its textures through
// a shared handle and samples it there. With -nowgcopen, or where a device of its own does not
// work, the pool is on the present's device and the present samples it directly. Either way no
// frame is copied and nothing is waited for.
//
// The WGC interfaces are declared here by hand, in the order and with the interface IDs of
// mingw-w64's windows.graphics.capture.h and Microsoft's windows-rs bindings, because the zig
// cross-build's headers do not carry them. Everything is loaded at run time, so the exe imports
// nothing new.

#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <inspectable.h>
#include <SimpleLogger.h>
#include <stdio.h>
#include <utility>
#include <vector>

#include "CaptureRing.h"
#include "ICaptureSource.h"

namespace wgc {

struct SizeInt32 { INT32 Width; INT32 Height; };
struct TimeSpan { INT64 Duration; };   // 100 ns units
struct EventToken { INT64 value; };

static const GUID kIidItemInterop =
    {0x3628e81b, 0x3cac, 0x4c60, {0xb7, 0xf4, 0x23, 0xce, 0x0e, 0x0c, 0x33, 0x56}};
static const GUID kIidItem =
    {0x79c3f95b, 0x31f7, 0x4ec2, {0xa4, 0x64, 0x63, 0x2e, 0xf5, 0xd3, 0x07, 0x60}};
static const GUID kIidPoolStatics2 =
    {0x589b103f, 0x6bbc, 0x5df5, {0xa9, 0x91, 0x02, 0xe2, 0x8b, 0x3b, 0x66, 0xd5}};
static const GUID kIidSession2 =
    {0x2c39ae40, 0x7d2e, 0x5044, {0x80, 0x4e, 0x8b, 0x67, 0x99, 0xd4, 0xcf, 0x9e}};
static const GUID kIidSession3 =
    {0xf2cdd966, 0x22ae, 0x5ea1, {0x95, 0x96, 0x3a, 0x28, 0x93, 0x44, 0xc3, 0xbe}};
static const GUID kIidSession5 =
    {0x67c0ea62, 0x1f85, 0x5061, {0x92, 0x5a, 0x23, 0x9b, 0xe0, 0xac, 0x09, 0xcb}};
static const GUID kIidFrame2 =
    {0x37869cfa, 0x2b48, 0x5ebf, {0x9a, 0xfb, 0xdf, 0xfd, 0x80, 0x5d, 0xef, 0xdb}};
static const GUID kIidClosable =
    {0x30d5a829, 0x7fa4, 0x4026, {0x83, 0xbb, 0xd7, 0x5b, 0xae, 0x4e, 0xa9, 0x9e}};
static const GUID kIidDxgiAccess =
    {0xa9b3d012, 0x3df2, 0x4ee3, {0xb8, 0xd1, 0x86, 0x95, 0xf4, 0x57, 0xd3, 0xc1}};
static const GUID kIidFrameArrivedHandler =
    {0x51a947f7, 0x79cf, 0x5a3e, {0xa3, 0xa5, 0x12, 0x89, 0xcf, 0xa6, 0xdf, 0xe8}};
static const GUID kIidAgileObject =
    {0x94ea2b94, 0xe9cc, 0x49e0, {0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90}};

struct ItemInterop : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE CreateForWindow(HWND window, REFIID iid, void** out) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateForMonitor(HMONITOR monitor, REFIID iid,
                                                       void** out) = 0;
};
struct Item : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_DisplayName(void** name) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Size(SizeInt32* size) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_Closed(void* handler, EventToken* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_Closed(EventToken token) = 0;
};
struct Frame : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Surface(IInspectable** surface) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_SystemRelativeTime(TimeSpan* time) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ContentSize(SizeInt32* size) = 0;
};
struct RectInt32 { INT32 X; INT32 Y; INT32 Width; INT32 Height; };
// IVectorView<RectInt32>
struct RectView : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE GetAt(UINT32 index, RectInt32* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Size(UINT32* size) = 0;
    virtual HRESULT STDMETHODCALLTYPE IndexOf(RectInt32 value, UINT32* index,
                                              unsigned char* found) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMany(UINT32 start, UINT32 capacity, RectInt32* values,
                                              UINT32* actual) = 0;
};
// What Windows reports as changed in a frame since the frame before it.
struct Frame2 : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_DirtyRegions(RectView** regions) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_DirtyRegionMode(int* mode) = 0;
};
struct Session : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE StartCapture() = 0;
};
struct Session2 : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_IsCursorCaptureEnabled(unsigned char* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_IsCursorCaptureEnabled(unsigned char value) = 0;
};
struct Session3 : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_IsBorderRequired(unsigned char* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_IsBorderRequired(unsigned char value) = 0;
};
// The least time between two delivered frames.
struct Session5 : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_MinUpdateInterval(TimeSpan* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_MinUpdateInterval(TimeSpan value) = 0;
};
struct FrameArrivedHandler : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Invoke(IInspectable* sender, IInspectable* args) = 0;
};
struct Pool : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE Recreate(IInspectable* device, int format, INT32 buffers,
                                               SizeInt32 size) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryGetNextFrame(Frame** frame) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_FrameArrived(FrameArrivedHandler* handler,
                                                       EventToken* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_FrameArrived(EventToken token) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateCaptureSession(Item* item, Session** session) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_DispatcherQueue(IInspectable** queue) = 0;
};
struct PoolStatics2 : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE CreateFreeThreaded(IInspectable* device, int format,
                                                         INT32 buffers, SizeInt32 size,
                                                         Pool** pool) = 0;
};
struct Closable : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE Close() = 0;
};
struct DxgiAccess : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetInterface(REFIID iid, void** out) = 0;
};

// The pool calls this from one of its own threads each time a frame is ready.
class ArrivalSignal : public FrameArrivedHandler {
public:
    explicit ArrivalSignal(HANDLE event) : m_event(event) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (IsEqualGUID(iid, __uuidof(IUnknown)) || IsEqualGUID(iid, kIidFrameArrivedHandler) ||
            IsEqualGUID(iid, kIidAgileObject)) {
            *out = this;
            AddRef();
            return S_OK;
        }
        *out = NULL;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&m_refs); }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG refs = InterlockedDecrement(&m_refs);
        if (refs == 0) delete this;
        return (ULONG)refs;
    }
    HRESULT STDMETHODCALLTYPE Invoke(IInspectable*, IInspectable*) override {
        SetEvent(m_event);
        return S_OK;
    }
    virtual ~ArrivalSignal() {}

private:
    HANDLE m_event;
    LONG m_refs = 1;
};

// The Direct3D 11 texture a frame's picture is in, with a reference the caller releases. The
// pool owns the texture for as long as the pool lives, and reuses it once the frame is given
// back.
inline ID3D11Texture2D* TextureOf(Frame* frame) {
    IInspectable* surface = NULL;
    DxgiAccess* access = NULL;
    ID3D11Texture2D* texture = NULL;
    if (SUCCEEDED(frame->get_Surface(&surface)) && surface) {
        if (SUCCEEDED(surface->QueryInterface(kIidDxgiAccess, (void**)&access))) {
            access->GetInterface(__uuidof(ID3D11Texture2D), (void**)&texture);
            access->Release();
        }
        surface->Release();
    }
    return texture;
}

// Closes a frame, session or pool, which gives a frame back to its pool, and releases it.
inline void Dispose(IInspectable* object) {
    if (!object) return;
    Closable* closable = NULL;
    if (SUCCEEDED(object->QueryInterface(kIidClosable, (void**)&closable))) {
        closable->Close();
        closable->Release();
    }
    object->Release();
}

}  // namespace wgc

class WgcCaptureSource : public ICaptureSource {
public:
    explicit WgcCaptureSource(CaptureRing& ring) : r(ring) {
        for (int i = 0; i < CaptureRing::RING_SIZE; i++) m_slotFrame[i] = NULL;
        m_why[0] = '\0';
    }
    ~WgcCaptureSource() { Close(); }

    WgcCaptureSource(const WgcCaptureSource&) = delete;
    WgcCaptureSource& operator=(const WgcCaptureSource&) = delete;

    // Starts a session on the source display, with its frame pool on a device of its own, or
    // with -nowgcopen on device, the D3D11 present's. Returns NULL once it is capturing, or what
    // stopped it, with nothing left open. Where the pool on a device of its own does not start,
    // or the present cannot open its textures (CheckOwnDevice), the log says why and the session
    // starts again on the present's device, where the game loses about 58 more points
    // (benchmark, 2026-10-14). All of it happens before NvFBC is let go, so no frame from a pool
    // the present cannot read is ever named in a slot.
    const char* Open(ID3D11Device* device) {
        if (r.m_wgcOwnDevice) {
            const char* why = MakeOwnDevice(device);
            if (!why) why = Start();
            if (!why) why = CheckOwnDevice();
            if (!why) return NULL;
            LOGERR("WGC: with the frame pool on a device of its own, %s; the pool goes on the "
                   "D3D11 present's device instead", why);
            Close();
            m_ownDeviceFailed = true;
        }
        m_device = device;
        m_device->AddRef();
        return Start();
    }

private:
    // Opens the session on m_device. It stops short where it would deliver fewer frames than
    // NvFBC, or a picture of another size: Windows' least time between frames has to come down
    // to 0.001 ms, and the source display has to be the size of the output, since the present
    // samples the pool's textures with point sampling and no scaling of its own.
    //
    // The least time between frames is 16 ms as found on Windows 11 build 26100, which
    // delivered a 185 fps game at about 53 frames a second. It has to be far under a
    // millisecond: under Smooth Motion the real frame arrives 0.27 ms behind the generated one
    // (0.066 ms at the closest), and with 1 ms set Windows held the real frame back for its
    // compositor's next tick and, whenever that tick met the next pair, never delivered it.
    const char* Start() {
        HMODULE combase = LoadLibraryW(L"combase.dll");
        HMODULE d3d11 = LoadLibraryW(L"d3d11.dll");
        if (!combase || !d3d11) return Refuse("combase.dll or d3d11.dll did not load");
        typedef HRESULT(WINAPI * RoInitializeFn)(int);
        typedef HRESULT(WINAPI * GetFactoryFn)(void*, REFIID, void**);
        typedef HRESULT(WINAPI * CreateStringFn)(const wchar_t*, UINT32, void**);
        typedef HRESULT(WINAPI * DeleteStringFn)(void*);
        typedef HRESULT(WINAPI * WrapDeviceFn)(IDXGIDevice*, IInspectable**);
        RoInitializeFn roInitialize =
            reinterpret_cast<RoInitializeFn>(::GetProcAddress(combase, "RoInitialize"));
        GetFactoryFn getFactory =
            reinterpret_cast<GetFactoryFn>(::GetProcAddress(combase, "RoGetActivationFactory"));
        CreateStringFn createString =
            reinterpret_cast<CreateStringFn>(::GetProcAddress(combase, "WindowsCreateString"));
        DeleteStringFn deleteString =
            reinterpret_cast<DeleteStringFn>(::GetProcAddress(combase, "WindowsDeleteString"));
        WrapDeviceFn wrapDevice = reinterpret_cast<WrapDeviceFn>(
            ::GetProcAddress(d3d11, "CreateDirect3D11DeviceFromDXGIDevice"));
        if (!roInitialize || !getFactory || !createString || !deleteString || !wrapDevice) {
            return Refuse("the Windows Runtime entry points were not found");
        }
        // Multithreaded. RPC_E_CHANGED_MODE means COM is already initialized on this thread in
        // the other mode, which is left as it is.
        m_result = roInitialize(1);
        if (FAILED(m_result) && m_result != (HRESULT)0x80010106) return Fail("RoInitialize");

        // The pool is made on m_device: a device of its own (MakeOwnDevice), whose textures
        // the present's device opens through shared handles (OpenOnPresent), that opening being
        // the only use of the present's device, its context never touched; or the present's.
        IDXGIDevice* dxgiDevice = NULL;
        m_result = m_device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice);
        if (FAILED(m_result)) return Fail("IDXGIDevice");
        m_result = wrapDevice(dxgiDevice, &m_winrtDevice);
        dxgiDevice->Release();
        if (FAILED(m_result)) return Fail("CreateDirect3D11DeviceFromDXGIDevice");

        static const wchar_t kItemClass[] = L"Windows.Graphics.Capture.GraphicsCaptureItem";
        static const wchar_t kPoolClass[] = L"Windows.Graphics.Capture.Direct3D11CaptureFramePool";
        void* name = NULL;
        wgc::ItemInterop* interop = NULL;
        createString(kItemClass, (UINT32)(sizeof(kItemClass) / sizeof(wchar_t) - 1), &name);
        m_result = getFactory(name, wgc::kIidItemInterop, (void**)&interop);
        deleteString(name);
        if (FAILED(m_result)) return Fail("the capture item factory");
        m_result = interop->CreateForMonitor(r.m_sourceMonitor, wgc::kIidItem, (void**)&m_item);
        interop->Release();
        if (FAILED(m_result)) return Fail("CreateForMonitor");
        m_result = m_item->get_Size(&m_size);
        if (FAILED(m_result)) return Fail("the capture item's size");
        if (m_size.Width != r.m_width || m_size.Height != r.m_height) {
            LOG("WGC: the source display is %dx%d and the output %dx%d; the present scales its "
                "frames", (int)m_size.Width, (int)m_size.Height, r.m_width, r.m_height);
        }
        m_contentSize = m_size;

        wgc::PoolStatics2* statics = NULL;
        createString(kPoolClass, (UINT32)(sizeof(kPoolClass) / sizeof(wchar_t) - 1), &name);
        m_result = getFactory(name, wgc::kIidPoolStatics2, (void**)&statics);
        deleteString(name);
        if (FAILED(m_result)) return Fail("the frame pool factory");
        // A buffer for every ring slot, so a frame can stay in the pool for as long as its slot
        // names it, and two more so the compositor always has one to write.
        m_buffers = (INT32)(r.m_ringSlots + 2);
        const int kFormatBgra8 = 87;   // B8G8R8A8UIntNormalized
        m_result = statics->CreateFreeThreaded(m_winrtDevice, kFormatBgra8, m_buffers, m_size,
                                               &m_pool);
        statics->Release();
        if (FAILED(m_result)) {
            m_pool = NULL;
            return Fail("CreateFreeThreaded");
        }

        m_event = CreateEventW(NULL, FALSE, FALSE, NULL);
        if (!m_event) return Refuse("CreateEventW failed");
        m_signal = new wgc::ArrivalSignal(m_event);
        m_result = m_pool->add_FrameArrived(m_signal, &m_token);
        if (FAILED(m_result)) return Fail("add_FrameArrived");
        m_handlerAdded = true;
        m_result = m_pool->CreateCaptureSession(m_item, &m_session);
        if (FAILED(m_result)) return Fail("CreateCaptureSession");
        // The mouse pointer is in the picture unless -nowgccursor leaves it out, as NvFBC's
        // grab does. No yellow border around the captured display.
        wgc::Session2* session2 = NULL;
        if (SUCCEEDED(m_session->QueryInterface(wgc::kIidSession2, (void**)&session2))) {
            const HRESULT set = session2->put_IsCursorCaptureEnabled(r.m_wgcCursor ? 1 : 0);
            m_cursorOff = SUCCEEDED(set) && !r.m_wgcCursor;
            session2->Release();
        }
        wgc::Session3* session3 = NULL;
        if (SUCCEEDED(m_session->QueryInterface(wgc::kIidSession3, (void**)&session3))) {
            m_borderOff = SUCCEEDED(session3->put_IsBorderRequired(0));
            session3->Release();
        }
        wgc::Session5* session5 = NULL;
        if (FAILED(m_session->QueryInterface(wgc::kIidSession5, (void**)&session5))) {
            return Refuse("this Windows has no setting for the least time between frames");
        }
        wgc::TimeSpan found = {}, now = {};
        const wgc::TimeSpan wanted = {10};   // 0.001 ms
        session5->get_MinUpdateInterval(&found);
        m_result = session5->put_MinUpdateInterval(wanted);
        session5->get_MinUpdateInterval(&now);
        session5->Release();
        LOG("WGC: least time between frames %.3f ms as found, %.3f ms after asking for %.3f ms",
            (double)found.Duration / 10000.0, (double)now.Duration / 10000.0,
            (double)wanted.Duration / 10000.0);
        if (FAILED(m_result) || now.Duration > wanted.Duration) {
            snprintf(m_why, sizeof(m_why), "Windows kept the least time between frames at "
                     "%.3f ms (result 0x%08lx)", (double)now.Duration / 10000.0,
                     (unsigned long)m_result);
            Close();
            return m_why;
        }
        m_result = m_session->StartCapture();
        if (FAILED(m_result)) return Fail("StartCapture");
        return NULL;
    }

public:
    void LogStart() override {
        // "ON" in capitals is a pointer -nowgccursor asked to leave out: Windows did not take
        // the setting.
        const char* cursor = "ON";
        if (m_cursorOff) cursor = "off (-nowgccursor)";
        else if (r.m_wgcCursor) cursor = "on (-nowgccursor leaves it out)";
        const char* place = "the D3D11 present's device (-nowgcopen)";
        if (m_presentDevice) {
            place = "a device of its own, each texture opened on the D3D11 present's device "
                    "through a shared handle (-nowgcopen keeps it on the present's device)";
        } else if (m_ownDeviceFailed) {
            place = "the D3D11 present's device, since a device of its own did not work";
        }
        LOG("WGC: capturing the %dx%d source display into a pool of %d buffers on %s; a ring "
            "slot names the pool texture its frame is in, and no frame is copied; cursor %s, "
            "border %s; wakes under %lld us apart are one batch",
            (int)m_size.Width, (int)m_size.Height, (int)m_buffers, place, cursor,
            m_borderOff ? "off" : "ON", (long long)(BatchWindowQpc() * 1000000 / r.m_freqQuad));
        if (r.m_wgcDropPointerFrames) {
            LOG("WGC: a frame that changed under a tenth of the largest area a frame changed in "
                "the last half second, as a move of the mouse pointer makes, is left out "
                "(-nowgcdrop keeps them)");
        } else {
            LOG("WGC: frames that changed only a small area are kept (-nowgcdrop)");
        }
    }

    // The same window as NvFBC's. With the least time between frames at 0.001 ms, WGC hands
    // over a frame-generation pair as NvFBC does: generated then real, 0.27 ms apart (1.04 ms
    // at the most) under Smooth Motion at 60 x2, the next pair at least 15.5 ms later.
    LONGLONG BatchWindowQpc() const override { return (r.m_freqQuad * 3) / 1000; }

    // Blocks until a frame is in the pool or the wait has run out in full. A frame already
    // queued is taken at once. A frame that only a move of the mouse pointer brought goes
    // straight back to the pool and the wait goes on, so the ring is never told of it.
    Wake Wait(LARGE_INTEGER* arrived) override {
        LARGE_INTEGER freq, start, now;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&start);
        for (;;) {
            m_pool->TryGetNextFrame(&m_frame);
            if (m_frame && r.m_wgcDropPointerFrames && PointerOnly(m_frame)) {
                wgc::Dispose(m_frame);
                m_frame = NULL;
                m_pointerFrames++;
                continue;
            }
            if (m_frame) break;
            QueryPerformanceCounter(&now);
            const LONGLONG waitedMs = (now.QuadPart - start.QuadPart) * 1000 / freq.QuadPart;
            if (waitedMs >= (LONGLONG)kWaitMs) return Wake::Timeout;
            WaitForSingleObject(m_event, (DWORD)((LONGLONG)kWaitMs - waitedMs));
        }
        QueryPerformanceCounter(arrived);
        m_texture = wgc::TextureOf(m_frame);
        if (!m_texture) {
            wgc::Dispose(m_frame);
            m_frame = NULL;
            m_noTexture++;
            if (m_noTexture == 1 || (m_noTexture % 1000) == 0) {
                LOGERR("WGC: a frame came with no texture and was dropped (%lld so far)",
                       m_noTexture);
            }
            return Wake::Failed;
        }
        wgc::SizeInt32 content = {};
        if (SUCCEEDED(m_frame->get_ContentSize(&content)) &&
            (content.Width != m_contentSize.Width || content.Height != m_contentSize.Height)) {
            LOGERR("WGC: frames are now %dx%d, and the pool's buffers %dx%d", (int)content.Width,
                   (int)content.Height, (int)m_size.Width, (int)m_size.Height);
            m_contentSize = content;
        }
        return Wake::Frame;
    }

    // Nothing waits: the frame stays where it is, in the pool.
    Placement Place(const policy::BatchDecision&) override { return Placement(); }

    // The slot names the frame's pool texture, and the frame the slot named before goes back
    // to the pool. The texture is named before the old frame is given back, and the pool keeps
    // every texture it made for as long as it lives, so a texture the present has just read
    // from a slot is never one that no longer exists.
    bool Store(int slot) override {
        ID3D11Texture2D* named = m_presentDevice ? OpenOnPresent(m_texture) : m_texture;
        r.m_wgcTexture[slot].store(named, std::memory_order_release);
        m_texture->Release();
        m_texture = NULL;
        wgc::Dispose(m_slotFrame[slot]);
        m_slotFrame[slot] = m_frame;
        m_frame = NULL;
        return true;
    }

    void Published(int, long long, const Placement&, bool) override {}

    void LogSummary(long long wakesStored) override {
        LOG("WGC summary: %lld wakes stored, %lld frames came with no texture and were dropped, "
            "%lld frames that changed only a small area were left out",
            wakesStored, m_noTexture, m_pointerFrames);
        if (m_presentDevice) {
            LOG("WGC summary: %zu pool textures opened on the present's device, %lld could not "
                "be opened", m_opened.size(), m_openFailures);
        }
    }

private:
    // The frame pool's own device, made on the adapter the present's device is on, so that
    // opening a pool texture on the present's device stays on one graphics card. It also takes
    // the present's device as ID3D11Device1, which opens the textures.
    const char* MakeOwnDevice(ID3D11Device* device) {
        HMODULE d3d11 = LoadLibraryW(L"d3d11.dll");
        if (!d3d11) return Refuse("d3d11.dll did not load");
        typedef HRESULT(WINAPI * CreateDeviceFn)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
                                                 const D3D_FEATURE_LEVEL*, UINT, UINT,
                                                 ID3D11Device**, D3D_FEATURE_LEVEL*,
                                                 ID3D11DeviceContext**);
        CreateDeviceFn createDevice =
            reinterpret_cast<CreateDeviceFn>(::GetProcAddress(d3d11, "D3D11CreateDevice"));
        if (!createDevice) return Refuse("D3D11CreateDevice was not found");
        m_result = device->QueryInterface(__uuidof(ID3D11Device1), (void**)&m_presentDevice);
        if (FAILED(m_result)) return Fail("the present's device as ID3D11Device1");
        IDXGIDevice* dxgiDevice = NULL;
        IDXGIAdapter* adapter = NULL;
        m_result = device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice);
        if (SUCCEEDED(m_result)) {
            m_result = dxgiDevice->GetAdapter(&adapter);
            dxgiDevice->Release();
        }
        if (FAILED(m_result)) return Fail("the present's adapter");
        // With an adapter named, D3D11CreateDevice takes no driver type.
        m_result = createDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, NULL,
                                D3D11_CREATE_DEVICE_BGRA_SUPPORT, NULL, 0, D3D11_SDK_VERSION,
                                &m_device, NULL, NULL);
        adapter->Release();
        if (FAILED(m_result)) {
            m_device = NULL;
            return Fail("D3D11CreateDevice for the frame pool");
        }
        return NULL;
    }

    // Whether the present's device opens the pool's textures, asked of the first frame before
    // NvFBC is let go. It takes Windows having made the textures shareable, which it did on
    // Windows 11 build 26100 (SHARED and SHARED_NTHANDLE, read 2026-10-14) but does not
    // promise, and a pool makes every texture alike, so the first answers for the rest. Windows
    // hands over a frame as soon as capture starts (about 10 ms after, on the rig's desktop,
    // 2026-10-14); none in half a second leaves nothing to ask, which counts as not working.
    // The frame goes back to the pool.
    const char* CheckOwnDevice() {
        LARGE_INTEGER freq, start, now;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&start);
        wgc::Frame* frame = NULL;
        for (;;) {
            m_pool->TryGetNextFrame(&frame);
            if (frame) break;
            QueryPerformanceCounter(&now);
            const LONGLONG waitedMs = (now.QuadPart - start.QuadPart) * 1000 / freq.QuadPart;
            if (waitedMs >= (LONGLONG)kCheckMs) {
                snprintf(m_why, sizeof(m_why), "no frame came within %lu ms to check that the "
                         "present's device opens the pool's textures", (unsigned long)kCheckMs);
                return m_why;
            }
            WaitForSingleObject(m_event, (DWORD)((LONGLONG)kCheckMs - waitedMs));
        }
        ID3D11Texture2D* texture = wgc::TextureOf(frame);
        HRESULT result = E_POINTER;
        ID3D11Texture2D* opened = texture ? OpenShared(texture, &result) : NULL;
        if (texture) texture->Release();
        wgc::Dispose(frame);
        if (!opened) {
            snprintf(m_why, sizeof(m_why), "the first frame's pool texture could not be opened "
                     "on the present's device (0x%08lx)", (unsigned long)result);
            return m_why;
        }
        opened->Release();
        return NULL;
    }

    // A pool texture opened on the present's device through an NT shared handle, or NULL with
    // the reason in *result.
    ID3D11Texture2D* OpenShared(ID3D11Texture2D* pool, HRESULT* result) {
        ID3D11Texture2D* opened = NULL;
        IDXGIResource1* resource = NULL;
        *result = pool->QueryInterface(__uuidof(IDXGIResource1), (void**)&resource);
        if (SUCCEEDED(*result)) {
            HANDLE handle = NULL;
            *result = resource->CreateSharedHandle(NULL, DXGI_SHARED_RESOURCE_READ, NULL, &handle);
            if (SUCCEEDED(*result)) {
                *result = m_presentDevice->OpenSharedResource1(handle, __uuidof(ID3D11Texture2D),
                                                               (void**)&opened);
                CloseHandle(handle);
            }
            resource->Release();
        }
        return opened;
    }

    // The pool texture as the present's device sees it, opened the first time it comes and
    // kept for as long as the session lasts. The first frame's opened before capture moved
    // here (CheckOwnDevice) and a pool makes every texture alike, so a texture that cannot be
    // opened is not expected; one would leave its slots naming nothing, so the present shows
    // the ring's own surface for them, and the summary counts it.
    ID3D11Texture2D* OpenOnPresent(ID3D11Texture2D* pool) {
        for (size_t i = 0; i < m_opened.size(); i++) {
            if (m_opened[i].first == pool) return m_opened[i].second;
        }
        HRESULT result = S_OK;
        ID3D11Texture2D* opened = OpenShared(pool, &result);
        if (!opened) {
            m_openFailures++;
            if (m_openFailures == 1) {
                LOGERR("WGC: a pool texture could not be opened on the present's device "
                       "(0x%08lx); its slots show the ring's own surface", (unsigned long)result);
            }
        }
        m_opened.push_back(std::make_pair(pool, opened));   // the pool keeps its texture alive
        return opened;
    }

    // True for a frame Windows reports as changed only in a small area against the game's own
    // frames: under a tenth of the largest area a frame changed lately (policy::IsPointerFrame
    // has the rule). A move of the mouse pointer makes one while WGC is capturing: Windows
    // delivers a frame whose picture is a copy of the frame before it and marks the pointer's
    // rectangle as all that changed, where a frame the game presented is marked as the whole of
    // what it drew. Left in, such a frame reaches the ring as a wake between the pairs of a
    // frame-generated source, or as a third member of one.
    //
    // A frame with no list of changed areas, or an empty one, is never this. Neither is a
    // small change once nothing larger has come for half a second, so a still screen keeps the
    // small updates that are all it has.
    bool PointerOnly(wgc::Frame* frame) {
        long long area = -1;
        wgc::Frame2* frame2 = NULL;
        wgc::RectView* regions = NULL;
        if (SUCCEEDED(frame->QueryInterface(wgc::kIidFrame2, (void**)&frame2)) && frame2) {
            if (SUCCEEDED(frame2->get_DirtyRegions(&regions)) && regions) {
                UINT32 count = 0;
                regions->get_Size(&count);
                area = 0;
                for (UINT32 i = 0; i < count; i++) {
                    wgc::RectInt32 rect = {};
                    if (FAILED(regions->GetAt(i, &rect))) {
                        area = -1;
                        break;
                    }
                    area += (long long)rect.Width * rect.Height;
                }
                regions->Release();
            }
            frame2->Release();
        }
        if (area < 0 && !m_noAreasLogged) {
            m_noAreasLogged = true;
            LOG("WGC: a frame came with no list of changed areas, and such a frame is never "
                "left out");
        }
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        return policy::IsPointerFrame(m_pointerDrop, area, now.QuadPart,
                                      (r.m_freqQuad * policy::kPointerDropForgetMs) / 1000);
    }

    // How long a wait lasts with no frame, as long as NvFBC's grab waits: it bounds how
    // quickly the capture thread sees a stop request.
    static const DWORD kWaitMs = 100;
    // How long CheckOwnDevice waits for a first frame to ask about.
    static const DWORD kCheckMs = 500;

    const char* Refuse(const char* why) {
        snprintf(m_why, sizeof(m_why), "%s", why);
        Close();
        return m_why;
    }
    const char* Fail(const char* step) {
        snprintf(m_why, sizeof(m_why), "%s failed (0x%08lx)", step, (unsigned long)m_result);
        Close();
        return m_why;
    }

    // Lets everything go. The present may still look a slot up, so each slot stops naming a
    // pool texture before its frame goes back.
    void Close() {
        if (m_pool && m_handlerAdded) m_pool->remove_FrameArrived(m_token);
        m_handlerAdded = false;
        for (int i = 0; i < CaptureRing::RING_SIZE; i++) {
            r.m_wgcTexture[i].store(NULL, std::memory_order_release);
            wgc::Dispose(m_slotFrame[i]);
            m_slotFrame[i] = NULL;
        }
        if (m_texture) m_texture->Release();
        m_texture = NULL;
        wgc::Dispose(m_frame);
        m_frame = NULL;
        wgc::Dispose(m_session);
        m_session = NULL;
        for (size_t i = 0; i < m_opened.size(); i++) {
            if (m_opened[i].second) m_opened[i].second->Release();
        }
        m_opened.clear();
        if (m_presentDevice) m_presentDevice->Release();
        m_presentDevice = NULL;
        wgc::Dispose(m_pool);
        m_pool = NULL;
        if (m_item) m_item->Release();
        m_item = NULL;
        if (m_signal) m_signal->Release();
        m_signal = NULL;
        if (m_winrtDevice) m_winrtDevice->Release();
        m_winrtDevice = NULL;
        if (m_device) m_device->Release();
        m_device = NULL;
        if (m_event) CloseHandle(m_event);
        m_event = NULL;
    }

    CaptureRing& r;
    HRESULT m_result = S_OK;
    char m_why[200];
    wgc::SizeInt32 m_size = {};
    wgc::SizeInt32 m_contentSize = {};
    INT32 m_buffers = 0;
    bool m_cursorOff = false;
    bool m_borderOff = false;
    ID3D11Device* m_device = NULL;
    ID3D11Device1* m_presentDevice = NULL;   // pool on its own device only: opens its textures
    std::vector<std::pair<ID3D11Texture2D*, ID3D11Texture2D*>> m_opened;   // pool, on present
    long long m_openFailures = 0;
    bool m_ownDeviceFailed = false;   // a device of its own did not work (Open)
    IInspectable* m_winrtDevice = NULL;
    wgc::Item* m_item = NULL;
    wgc::Pool* m_pool = NULL;
    wgc::Session* m_session = NULL;
    wgc::ArrivalSignal* m_signal = NULL;
    wgc::EventToken m_token = {};
    bool m_handlerAdded = false;
    HANDLE m_event = NULL;
    wgc::Frame* m_frame = NULL;                       // the frame in hand, between Wait and Store
    ID3D11Texture2D* m_texture = NULL;                // its texture
    wgc::Frame* m_slotFrame[CaptureRing::RING_SIZE];  // the frame each slot names
    long long m_noTexture = 0;
    long long m_pointerFrames = 0;      // left out by PointerOnly
    policy::PointerDropState m_pointerDrop;   // the largest area the game's frames changed lately
    bool m_noAreasLogged = false;
};
