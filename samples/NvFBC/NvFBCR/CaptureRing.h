#pragma once

#include <windows.h>
#include <d3d9.h>
#include <NvFBCLibrary.h>
#include <NvFBC/NvFBCToDx9vid.h>
#include <atomic>
#include <thread>

#include "TemporalPolicy.h"

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

    // Find the published frames bracketing targetQpc (present-device aliases). overlay,
    // when non-null, supplies stage-6 lateness corrections subtracted from slot stamps AT
    // READ TIME: slots themselves are never mutated (the capture thread stays their only
    // writer), and a recycled slot cannot inherit a stale correction because its stamp is
    // a different key. Null keeps the exact pre-stage-6 read path.
    void FindBracket(LONGLONG targetQpc, const policy::StampOverlay* overlay,
                     FrameBracket* out) const;

    // Batch-start history for the stage-6 walk, so the present thread never reads slot
    // fields the capture thread may be recycling: the capture thread appends each batch's
    // start stamp here at batch open (single producer), the present thread reads entries
    // below BatchOpens() (single consumer). Entries are written once and published by the
    // release store in the counter; kBatchHistory is sized so lapping within one present
    // is impossible short of a multi-second present stall, which the caller guards.
    static const int kBatchHistory = 64;
    long long BatchOpens() const { return m_batchOpens.load(std::memory_order_acquire); }
    LONGLONG BatchStartAt(long long i) const { return m_batchStarts[i % kBatchHistory]; }

    // Shared handle of slot i, for opening the same texture on another API's device.
    HANDLE SlotSharedHandle(int i) const { return m_ring[i].sharedHandle; }

    // What the ring needs from the flip grid to read the batch-composition rotation, kept
    // to two questions so the ring never learns about ETW. Both are answered on the CAPTURE
    // thread at batch open, so both must be cheap and must not block: the implementation
    // takes the flip history's lock for a single bounded lookup (measured ~160 ns since the
    // history walk stops at the cadence window), never for a walk.
    class IRotationOracle {
    public:
        virtual ~IRotationOracle() {}
        // The CURRENT measured grid: flips a batch advances, flips in one source period,
        // and the flip step itself. False when the grid is not known yet, which keeps the
        // mechanism inert. The ring turns these into the rotation length and the member
        // stamp offsets, so nothing here declares a frame-generation multiplier.
        virtual bool Grid(long long batchPeriodTicks, int* outStride,
                          int* outFlipsPerSource, long long* outSpacingTicks) = 0;
        // Batch start minus its nearest flip. False when the batch cannot be placed on the
        // grid at all, which is not an error - it just yields no vote this batch.
        virtual bool ArrivalOffset(long long batchStartTs, long long* outOffset) = 0;
    };

    // Arm phase-aware keep-real (-phasekeep) before Start. Default keep-real retains member
    // 1 of every batch, which is right wherever composition does not rotate; where it does
    // (x3: batch stride 2 against a 3-flip source period) member 1 holds the real frame in
    // only one class of three, so the kept sequence runs gen, real, gen, gen, real, gen.
    // With the oracle the ring votes on the rotation phase and retains member 0 through the
    // [real,gen] class instead, lifting real content from 2 of every 6 outputs to 4 of 6.
    // Until the vote is decisive - and whenever grid continuity breaks - this falls back to
    // plain keep-real, so the failure mode is exactly today's behaviour.
    void EnablePhaseKeep(IRotationOracle* oracle) { m_rotationOracle = oracle; }

    // Session telemetry for the phase vote, logged at exit by the owner.
    long long PhaseKeepBatches() const { return m_phaseKeepBatches; }
    long long PhaseKeepFlipped() const { return m_phaseKeepFlipped; }
    long long PhaseKeepEmpty() const { return m_phaseKeepEmpty; }
    long long PhaseKeepResets() const { return m_phaseKeepResets; }

    // Request the content-phase instrument (-fgphase) before Start.
    //
    // IT MODELS THE [generated, real] BATCH, so it is an x2 instrument. Its f is defined
    // against a previous-real -> current-real axis, which exists only where member 0 is the
    // generated frame and member 1 the real one. At x3 composition rotates and no single
    // axis holds for every batch, so an f logged there is not the quantity the stage-7 gate
    // asks about. It composes safely with phase-aware keep-real - the reference frame
    // follows whatever the ring actually kept - but composing safely is not the same as
    // being meaningful, and only the x2 numbers answer the gate.
    //
    // Per batch it measures WHERE BETWEEN ITS REAL NEIGHBOURS the generated member's
    // content sits (the fraction f: does content phase track display phase?), by projecting
    // the generated frame onto the previous-real -> current-real axis in downscaled luma. One
    // fgphase: log line per measured batch; the display fraction g is joined OFFLINE from
    // the flip lines, so nothing here reads ETW. Instrument runs are instrument runs, not
    // reference runs: the readback stalls the capture thread on the GPU once per wake.
    void EnableFgPhase() { m_fgPhaseRequested = true; }

private:
    struct Slot {
        IDirect3DTexture9* capTexture;    // capture device (StretchRect destination)
        IDirect3DSurface9* capSurface;
        IDirect3DTexture9* mainTexture;   // present device alias (opened via shared handle)
        IDirect3DSurface9* mainSurface;
        HANDLE sharedHandle;              // retained for cross-API consumers
        LARGE_INTEGER timestamp;
        // The batch this slot came from, which is the stage-6 overlay's key. Held
        // separately because a member's TIMESTAMP may sit a flip step past its batch start
        // (phase-aware keep-real stamps member m at its own flip so content and stamp
        // agree), and a correction measured per batch must still find every member of it.
        // Equal to timestamp wherever member stamps are not offset, so the pre-existing
        // read path is bit-for-bit unchanged there.
        LARGE_INTEGER batchStart;
        int member;                       // position inside its capture batch; 0 opens one
        bool valid;
    };

    void CaptureLoop(NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams);

    // Content-phase instrument (-fgphase). Working geometry is a 320x180 GPU downscale:
    // small enough that the sync readback costs ~230 KB a wake, large enough that the
    // estimator's validated displacement envelope (exact to 8 source px, mean error under
    // 0.05 out to 128) comfortably covers real pan motion.
    static const int kFgW = 320;
    static const int kFgH = 180;
    bool ReadFgLuma(IDirect3DSurface9* src, float* out);
    void FgPhaseOnWake(int member, int slot, LONGLONG batchStartUs, bool keptThisMember);

    // Phase-aware keep-real state, capture-thread-owned end to end: the vote is fed and read
    // on the same thread that retracts, so there is no cross-thread publication to get wrong.
    IRotationOracle* m_rotationOracle = NULL;
    policy::RotationPhase m_rotation;
    long long m_phaseKeepBatches = 0;   // batches the vote was consulted for
    long long m_phaseKeepFlipped = 0;   // batches that kept a member plain keep-real would not
    long long m_phaseKeepEmpty = 0;     // all-generated batches dropped whole
    long long m_phaseKeepResets = 0;    // votes dropped on a continuity break
    // Grid measurements for the batch in flight, read once at batch open: members arrive a
    // submission epsilon apart, so re-reading per member would only add lock traffic.
    int m_rotRealMember = -1;           // member this batch should keep; <0 = fall back
    LONGLONG m_rotSpacing = 0;          // flip step, for member stamp offsets

    bool m_fgPhaseRequested = false;
    bool m_fgPhaseActive = false;
    IDirect3DSurface9* m_fgSmallRT = NULL;   // capture device, StretchRect destination
    IDirect3DSurface9* m_fgSmallSys = NULL;  // sysmem twin for GetRenderTargetData
    // Rotating wake buffers plus the last KEPT frame's luma. A member-0 wake is only known
    // to be a kept single when the NEXT wake fails to retract it, so promotion to "kept"
    // is deferred one wake.
    float* m_fgLumWake[2] = { NULL, NULL };
    float* m_fgLumKept = NULL;
    int m_fgWakeParity = 0;
    int m_fgPrevMember = -1;
    bool m_fgKeptValid = false;
    bool m_fgPrevWakeValid = false;

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
    LONGLONG m_batchStarts[kBatchHistory] = {};   // written by capture thread at batch open
    std::atomic<long long> m_batchOpens{0};
};
