#pragma once

#include <windows.h>
#include <d3d9.h>
#include <NvFBCLibrary.h>
#include <NvFBC/NvFBCToDx9vid.h>
#include <atomic>
#include <thread>

// Result of a bracketing query: the captured frames immediately before and after a target
// time, plus the interpolation weight a blending consumer would use. Surfaces/textures are
// the PRESENT-device aliases of the shared ring slots (borrowed; do not Release).
struct FrameBracket {
    bool hasBefore;
    bool hasAfter;
    IDirect3DSurface9* beforeSurface;
    IDirect3DSurface9* afterSurface;
    IDirect3DTexture9* beforeTexture;
    IDirect3DTexture9* afterTexture;
    LONGLONG beforeTs;                 // QPC of the before frame's arrival
    LONGLONG afterTs;
    LONGLONG beforeDiff;               // target - beforeTs (>= 0)
    LONGLONG afterDiff;                // afterTs - target (> 0)
    int beforeDepth;                   // captures back from newest where the before frame sits
    double weight;                     // beforeDiff / (beforeDiff + afterDiff); 1.0 if no after
};

// Source-paced capture ring on its OWN D3D9Ex device (branch B: two devices).
//
// The two-thread design failed on a shared device because the blocking NvFBC grab holds the
// device lock for its entire wait (up to a full source period), slaving present timing to
// capture arrivals (measured: present jitter = capture period / 2). Here the capture side
// gets a private device: Start() releases the caller's NvFBC session and creates a new one
// bound to a capture-only device, so the grab's lock holds affect nothing the present thread
// uses. Grabs go back to long, fully event-driven blocking waits (true arrival timestamps,
// no polling, no diffmap needed).
//
// Ring slots are SHARED render-target textures: created on the capture device (NvFBC's
// StretchRect source side) and opened on the present device via D3D9Ex shared handles, so the
// present thread reads them without touching the capture device. Known risk: D3D9Ex shared
// surfaces have no cross-device sync primitive; ordering relies on driver behavior. If the
// output shows tearing/partial frames inside slots, that is the cause.
class CaptureRing {
public:
    // 16 slots. At the observed ~386 publish/s (240fps content + HW-cursor wakes) an 8-slot ring
    // spans only ~20.8ms — barely above the one-period (16.67ms) bracketing lag — so cursor-wake
    // bursts pushed the target off the back of the ring (~23% of presents → "target older than
    // ring window"). 16 slots span ~41ms at that rate, a robust margin. The before-frame logs at
    // depth ~6, so 16 keeps it comfortably mid-ring. (Cheap: 16 shared RT textures per device.)
    static const int RING_SIZE = 16;

    CaptureRing();
    ~CaptureRing();

    // Owns COM resources and a thread; non-copyable.
    CaptureRing(const CaptureRing&) = delete;
    CaptureRing& operator=(const CaptureRing&) = delete;

    // Stash the present device + dimensions (capture-side resources are created in Start,
    // which has the HWND needed for the capture device).
    bool Setup(IDirect3DDevice9Ex* presentDevice, int width, int height);

    // Create the capture device + shared ring, rebind NvFBC to the capture device (releases
    // the passed-in session and replaces the global), and start the capture thread.
    bool Start(NvFBCToDx9Vid* nvfbc, NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams,
               LARGE_INTEGER baseQpc, HWND hwnd);

    // Signal and join the capture thread (idempotent).
    void Stop();

    // True once capture is no longer running (stop requested or fatal NvFBC error).
    bool HasStopped() const { return m_stop.load(); }

    // Count of fully-published frames so far.
    long long Published() const { return m_published.load(); }

    // Find the published frames bracketing targetQpc (present-device aliases).
    void FindBracket(LONGLONG targetQpc, FrameBracket* out) const;

private:
    struct Slot {
        IDirect3DTexture9* capTexture;    // capture device (StretchRect destination)
        IDirect3DSurface9* capSurface;
        IDirect3DTexture9* mainTexture;   // present device alias (opened via shared handle)
        IDirect3DSurface9* mainSurface;
        LARGE_INTEGER timestamp;
        bool valid;
    };

    void CaptureLoop(NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams);

    Slot m_ring[RING_SIZE];
    IDirect3DSurface9* m_captureTarget;   // on the capture device; NvFBC writes here
    IDirect3DDevice9Ex* m_presentDevice;
    IDirect3DDevice9Ex* m_capDevice;      // private capture device
    IDirect3DQuery9* m_capSync;           // event query: flush capture writes before publish
    NvFBCToDx9Vid* m_nvfbc;               // session bound to the capture device
    int m_width;
    int m_height;
    LARGE_INTEGER m_baseQpc;              // logging origin
    LONGLONG m_freqQuad;                  // QPC ticks/sec

    std::thread m_captureThread;
    std::atomic<long long> m_published;
    std::atomic<bool> m_stop;
    long long m_writeCount;               // capture-thread-local
};
