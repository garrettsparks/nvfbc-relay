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
    // Ring capacity is a STARTUP decision (-ring N, default 8, clamped [3, MAX_RING_SIZE]).
    // Deliberately not runtime-adaptive: slots are shared GPU textures whose handles the
    // present device opened at Start — reallocating mid-session would break
    // publish-then-never-touch and churn shared handles for no benefit. Size once from the
    // regime you run (FG multipliers halve/divide the valid population: keep-real retraction
    // leaves ~capacity/k valid frames at multiplier k — at ×2 the default 8 gives 4 valid;
    // higher multipliers want -ring 4*k). The logged bracket depth d<n> is the margin gauge.
    static const int MAX_RING_SIZE = 32;

    // Configured slot count for this run.
    int Capacity() const { return m_capacity; }

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

    Slot m_ring[MAX_RING_SIZE];
    int m_capacity;                       // configured slot count (3..MAX_RING_SIZE)
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
