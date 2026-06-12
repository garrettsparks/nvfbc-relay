#pragma once

#include <windows.h>
#include <d3d9.h>
#include <NvFBCLibrary.h>
#include <NvFBC/NvFBCToDx9vid.h>
#include <atomic>
#include <thread>

// Result of a bracketing query: the captured frames immediately before and after a target
// time, plus the interpolation weight a blending consumer would use.
struct FrameBracket {
    bool hasBefore;
    bool hasAfter;
    IDirect3DSurface9* beforeSurface;  // borrowed from the ring slot (do not Release)
    IDirect3DSurface9* afterSurface;
    IDirect3DTexture9* beforeTexture;  // same slots as textures, for shader sampling
    IDirect3DTexture9* afterTexture;
    LONGLONG beforeTs;                 // QPC of the before frame's arrival
    LONGLONG afterTs;
    LONGLONG beforeDiff;               // target - beforeTs (>= 0)
    LONGLONG afterDiff;                // afterTs - target (> 0)
    int beforeDepth;                   // captures back from newest where the before frame sits
    double weight;                     // beforeDiff / (beforeDiff + afterDiff); 1.0 if no after
};

// Source-paced capture ring shared by the temporal/blend (and future optical-flow) modes.
//
// Owns the capture side of the two-thread flow: a capture thread loops a *blocking* NvFBC grab
// (NVFBC_TODX9VID_WAIT_WITH_TIMEOUT), which returns once per real source frame, so each frame
// is QPC-stamped at arrival (≈ its true source time) and copied into a fixed ring slot. A
// monotonic count is published via an atomic after each slot is fully written — the only state
// shared with the consumer, who reads published slots and never signals back (strictly one-way
// producer -> consumer; the ring overwrites oldest slots naturally, no purging).
//
// Slots are render-target textures (with their level-0 surfaces exposed) so consumers can
// either StretchRect them (temporal) or sample them in a shader (blend).
//
// Experiment C — short-timeout polling + diffmap dedup: NvFBC's WAIT_WITH_TIMEOUT expiry does
// not return empty; it re-grabs the unchanged screen and returns success. With the short
// timeout (which bounds how long each grab holds the shared device lock), most grab calls are
// therefore stale re-grabs. The NvFBC diffmap detects them by content (all-zero = unchanged)
// so only content-distinct frames are published, keeping arrival timestamps meaningful. After
// each stale result the capture thread sleeps 1 ms to hand the device lock to the present
// thread (guarding against lock-acquisition unfairness/convoy).
//
// Requires D3DCREATE_MULTITHREADED on the device, since the capture thread issues StretchRect
// while the present thread issues its own D3D9 calls.
class CaptureRing {
public:
    static const int RING_SIZE = 8;  // generous for validation; shrink later from logged depth

    CaptureRing();
    ~CaptureRing();

    // Owns COM resources and a thread; non-copyable.
    CaptureRing(const CaptureRing&) = delete;
    CaptureRing& operator=(const CaptureRing&) = delete;

    // Allocate the capture target and ring slots.
    bool Setup(IDirect3DDevice9Ex* device, int width, int height);

    // Redirect NvFBC output into the capture target, switch the grab to blocking-with-timeout,
    // and start the capture thread. baseQpc is the shared logging time origin.
    bool Start(NvFBCToDx9Vid* nvfbc, NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams, LARGE_INTEGER baseQpc);

    // Signal and join the capture thread (idempotent).
    void Stop();

    // True once capture is no longer running (stop requested or fatal NvFBC error).
    bool HasStopped() const { return m_stop.load(); }

    // Count of fully-published frames so far.
    long long Published() const { return m_published.load(); }

    // Stale grabs skipped via the diffmap (timeout re-grabs / no-content wakes).
    long long StaleSkips() const { return m_staleSkips.load(); }

    // Find the published frames bracketing targetQpc. Fields for a missing side are zeroed and
    // the corresponding has* flag false.
    void FindBracket(LONGLONG targetQpc, FrameBracket* out) const;

private:
    struct Slot {
        IDirect3DTexture9* texture;
        IDirect3DSurface9* surface;   // level 0 of texture
        LARGE_INTEGER timestamp;
        bool valid;
    };

    void CaptureLoop(NvFBCToDx9Vid* nvfbc, NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams);
    bool DiffMapChanged() const;

    Slot m_ring[RING_SIZE];
    IDirect3DSurface9* m_captureTarget;  // NvFBC writes here; capture thread copies into slots
    IDirect3DDevice9Ex* m_device;
    void* m_diffMapArray[1];             // ppDiffMap container (dwNumBuffers == 1)
    void* m_diffMap;                     // VirtualAlloc'd, NVFBC_TODX9VID_MAX_DIFF_MAP_SIZE
    int m_diffMapScanBytes;              // bytes actually used: ceil(w/128)*ceil(h/128)
    std::atomic<long long> m_staleSkips;
    int m_width;
    int m_height;
    LARGE_INTEGER m_baseQpc;             // logging origin
    LONGLONG m_freqQuad;                 // QPC ticks/sec, for log µs conversion

    std::thread m_captureThread;
    std::atomic<long long> m_published;
    std::atomic<bool> m_stop;
    long long m_writeCount;              // capture-thread-local
};
