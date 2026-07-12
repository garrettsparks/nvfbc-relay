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
    static const int RING_SIZE = 8;  // generous for validation; shrink later from logged depth

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

    // EMA of the source's batch-start period in QPC ticks (0 until warmed up). TELEMETRY
    // ONLY: consumers may audit their configured source-rate assumption against it, but it
    // must never drive the bracketing lag; the lag is a static launch-time constant so that
    // output latency stays compensable downstream. Intra-batch frame-gen gaps and stall gaps
    // over 125 ms are excluded; grab-timeout re-grabs of a static source DO enter (they are
    // the source's effective cadence while nothing new is drawn).
    LONGLONG EstimatedSourcePeriodQpc() const { return m_srcPeriodEmaQpc.load(); }

    // Find the published frames bracketing targetQpc (present-device aliases).
    void FindBracket(LONGLONG targetQpc, FrameBracket* out) const;

    // Request the content probe (-probe) before Start: per-grab NvFBC diffmap (changed 32x32
    // blocks vs the previous grab) and high-frequency-content classification map, appended to
    // the capture log line. Instrument-only; adds driver-side per-grab work plus a small scan
    // to the capture loop (widens the wake-to-regrab window), so keep it OFF for production
    // runs and same-config A/B any numbers gathered with it on.
    void EnableContentProbe() { m_probeRequested = true; }

    // Request a frame dump (-dump <seconds>) before Start: DUMP_FRAMES consecutive captures,
    // starting the given number of seconds after capture begins, are copied GPU-to-GPU into
    // staging surfaces as they arrive (cheap, timeline-preserving) and drained to BMP files
    // in the working directory once the window completes (capture stalls during the drain -
    // the run's timing is over at that point). Ground-truth instrument: the files show which
    // captured frames are real vs interpolated (ghosting under panning motion).
    void EnableFrameDump(int atSeconds) { m_dumpAtSeconds = atSeconds; }

    static const int DUMP_FRAMES = 30;

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
    std::atomic<long long> m_srcPeriodEmaQpc;  // capture-thread-written source period estimate
    std::atomic<bool> m_stop;
    long long m_writeCount;               // capture-thread-local

    struct DumpMeta {
        long long captureIndex;
        LONGLONG arrQpc;
        LONGLONG dtQpc;
        int blkChanged;                   // -1 when the probe is off
        unsigned long long hfSum;
    };

    void DrainFrameDump();

    int m_dumpAtSeconds;                  // 0 = disabled
    LONGLONG m_dumpStartQpc;              // window opens (computed at Start)
    int m_dumpCount;                      // staged so far
    bool m_dumpDrained;
    IDirect3DSurface9* m_dumpStaging[DUMP_FRAMES];   // capture-device render targets
    DumpMeta m_dumpMeta[DUMP_FRAMES];

    bool m_probeRequested;                // content probe asked for (-probe)
    bool m_probeActive;                   // probe survived session setup (driver support)
    bool m_classMapActive;                // classification map survived setup (may lack driver support separately)
    void* m_diffMap;                      // VirtualAlloc'd; one byte per block, driver-written per grab
    void* m_classMap;                     // VirtualAlloc'd; high-frequency-content stamps, driver-written per grab
    int m_diffBlocks;                     // diffmap bytes to scan for m_width x m_height
    int m_classStamps;                    // classification bytes to scan for m_width x m_height
};
