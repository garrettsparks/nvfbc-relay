#pragma once

#include <windows.h>
#include <d3d9.h>
#include <NvFBCLibrary.h>
#include <NvFBC/NvFBCToDx9vid.h>

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

// Single-threaded, pump-driven capture ring (branch A: no capture thread).
//
// The owner's present loop calls PumpUntil(stopQpc) between present deadlines. Each blocking
// grab (NVFBC_TODX9VID_WAIT_WITH_TIMEOUT, dwWaitTime = time remaining) either returns early
// when a new source frame arrives, or expires at stopQpc. Crucially, NvFBC's timeout expiry
// does NOT return empty — it re-grabs the current (unchanged) screen and returns success — and
// a frame arriving within ~1ms of expiry is indistinguishable from that by timing alone. So
// stale grabs are detected by CONTENT, via the NvFBC diffmap: an all-zero diffmap means no
// content change since the previous grab, and the grab is skipped (not published). The ring
// therefore holds only content-distinct frames, QPC-stamped at arrival.
//
// Because capture and present share one thread, there is no D3D9 device-lock contention with
// the present path at all — the failure mode of the two-thread design, where a blocking grab
// held the device lock for up to a full source period and slaved present timing to capture
// arrivals (measured: present jitter = capture period / 2).
class CaptureRing {
public:
    static const int RING_SIZE = 8;  // generous for validation; shrink later from logged depth

    CaptureRing();
    ~CaptureRing();

    // Owns COM resources; non-copyable.
    CaptureRing(const CaptureRing&) = delete;
    CaptureRing& operator=(const CaptureRing&) = delete;

    // Allocate the capture target, ring slots, and the diffmap buffer.
    bool Setup(IDirect3DDevice9Ex* device, int width, int height);

    // Redirect NvFBC output into the capture target with the diffmap enabled, and switch the
    // grab to blocking-with-timeout. baseQpc is the shared logging time origin.
    bool Attach(NvFBCToDx9Vid* nvfbc, NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams, LARGE_INTEGER baseQpc);

    // Absorb source frames until ~stopQpc (returns at expiry or on a fatal NvFBC error).
    // Content-changed grabs are published into the ring; stale re-grabs are skipped.
    void PumpUntil(LONGLONG stopQpc);

    // True after a fatal NvFBC error (e.g. invalidated session).
    bool HasFatal() const { return m_fatal; }

    // Counters for logging: published frames and stale grabs skipped.
    long long Published() const { return m_published; }
    long long StaleSkips() const { return m_staleSkips; }

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

    bool DiffMapChanged() const;

    Slot m_ring[RING_SIZE];
    IDirect3DSurface9* m_captureTarget;  // NvFBC writes here; published grabs copy into slots
    IDirect3DDevice9Ex* m_device;
    NvFBCToDx9Vid* m_nvfbc;
    NVFBC_TODX9VID_GRAB_FRAME_PARAMS* m_grabParams;

    void* m_diffMapArray[1];             // ppDiffMap container (dwNumBuffers == 1)
    void* m_diffMap;                     // VirtualAlloc'd, NVFBC_TODX9VID_MAX_DIFF_MAP_SIZE
    int m_diffMapScanBytes;              // bytes actually used: ceil(w/128)*ceil(h/128)

    int m_width;
    int m_height;
    LARGE_INTEGER m_baseQpc;             // logging origin
    LONGLONG m_freqQuad;                 // QPC ticks/sec

    long long m_published;
    long long m_staleSkips;
    bool m_fatal;
    LONGLONG m_lastArrival;              // for the dt log field
};
