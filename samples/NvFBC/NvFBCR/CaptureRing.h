#pragma once

#include <windows.h>
#include <d3d9.h>
#include <NvFBCLibrary.h>
#include <NvFBC/NvFBCToDx9vid.h>
#include <atomic>
#include <thread>

#include "TemporalPolicy.h"

class EtwFlipConsumer;

// Result of a bracketing query: the captured frames immediately before and after a target
// time, plus the interpolation weight a blending consumer would use. The timestamp/diff
// half lives in the embedded policy::BracketInfo so the policy layer consumes it without
// a copy. Surfaces/textures are the PRESENT-device aliases of the shared ring slots
// (borrowed; do not Release).
struct FrameBracket {
    policy::BracketInfo info;          // hasBefore/hasAfter, timestamps, diffs from target
    IDirect3DSurface9* beforeSurface;
    IDirect3DSurface9* afterSurface;
    IDirect3DTexture9* beforeTexture;
    IDirect3DTexture9* afterTexture;
    int beforeDepth = 0;               // captures back from newest where the before frame sits
    int beforeSlot = -1;               // ring slot indices; cross-API consumers pick their own
    int afterSlot = -1;                //  per-device aliases by slot (-1 when the side is absent)
    double weight = 0.0;               // beforeDiff / (beforeDiff + afterDiff); 1.0 if no after
    // Position inside the capture batch. The timestamps above are BATCH-START, shared by
    // every member, so placing a frame on the driver's flip grid needs this to know how many
    // flips along it scanned out.
    int beforeMember = 0;
    int afterMember = 0;
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

    // Stage 6: walk unprocessed batches in the published window in arrival order, measure
    // each one's delivery lateness through `measure`, and subtract it from the batch's
    // stamps. Present-thread only (the same thread FindBracket runs on). safeQpc is the
    // coherence fence: a stamp never moves unless both its current and corrected values
    // are strictly newer, so nothing can cross a target the policy has consumed.
    // lastDoneQpc is the caller-held cursor of the newest batch already settled; a batch
    // whose flip data is still in flight stops the walk (the anchor chain is sequential),
    // to be retried next present. Returns batches whose stamps moved this call.
    int CorrectLateStamps(LONGLONG safeQpc, LONGLONG* lastDoneQpc,
                          EtwFlipConsumer* etw, policy::AnchorChain* chain,
                          LONGLONG cadenceWindowQpc, bool lockCalm);

    // Shared handle of slot i, for opening the same texture on another API's device.
    HANDLE SlotSharedHandle(int i) const { return m_ring[i].sharedHandle; }

private:
    struct Slot {
        IDirect3DTexture9* capTexture;    // capture device (StretchRect destination)
        IDirect3DSurface9* capSurface;
        IDirect3DTexture9* mainTexture;   // present device alias (opened via shared handle)
        IDirect3DSurface9* mainSurface;
        HANDLE sharedHandle;              // retained for cross-API consumers
        LARGE_INTEGER timestamp;
        // The batch-start the slot was published with. timestamp starts equal to it and a
        // lateness correction may move timestamp; this stays put, because the flip-grid
        // anchoring is keyed to when the batch ARRIVED, not to where its stamp ended up.
        LONGLONG batchStartQpc;
        int member;                       // position inside its capture batch; 0 opens one
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
};
