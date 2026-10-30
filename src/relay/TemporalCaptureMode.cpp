#include "TemporalCaptureMode.h"
#include "FrameCompositors.h"
#include "D3D9Present.h"
#include "D3D11Present.h"
#include "Pacing.h"
#include "NvFBCSession.h"
#include "OutputWindow.h"
#include <SimpleLogger.h>
#include <cstdio>

// Presents between estimator-vs-assumption audits (about 10 s at 60 Hz): rare enough to keep
// the log quiet, frequent enough that a wrong -src is caught within the first minute.
static const int kTelemetryPeriodPresents = 600;

// The capture ring as the policy step reads it. The step sees the bracket's stamps and diffs;
// the present path needs the slots and surfaces too, so the last brackets read are kept whole,
// by the target each was read at: this present's, and the planned presents' after it.
class CaptureRingView : public policy::IRingView {
public:
    explicit CaptureRingView(const CaptureRing& ring) : m_ring(ring) {}
    void FindBracket(int64_t target, const policy::StampOverlay* overlay,
                     policy::BracketInfo* out) const override {
        m_ring.FindBracket(target, overlay, &m_last);
        m_reads[m_nextRead].target = target;
        m_reads[m_nextRead].bracket = m_last;
        m_nextRead = (m_nextRead + 1) % kReads;
        *out = m_last.info;
    }
    void ReadRecentFrames(int64_t target, const policy::StampOverlay* overlay,
                          policy::RecentFrames* out) const override {
        m_ring.ReadRecentFrames(target, overlay, out);
    }
    long long BatchOpens() const override { return m_ring.BatchOpens(); }
    int64_t BatchStartAt(long long i) const override { return m_ring.BatchStartAt(i); }
    long long BatchHistory() const override { return CaptureRing::kBatchHistory; }
    // The whole bracket of the step's read, slots and surfaces included.
    const FrameBracket& LastBracket() const { return m_last; }
    // The whole bracket of the newest read at target (a planned present's), or NULL.
    const FrameBracket* BracketAt(int64_t target) const {
        for (int k = 1; k <= kReads; k++) {
            const Read& r = m_reads[(m_nextRead - k + kReads) % kReads];
            if (r.target == target) return &r.bracket;
        }
        return NULL;
    }

private:
    static const int kReads = 8;
    struct Read {
        int64_t target = INT64_MIN;
        FrameBracket bracket;
    };
    const CaptureRing& m_ring;
    mutable FrameBracket m_last;
    mutable Read m_reads[kReads];
    mutable int m_nextRead = 0;
};

// Stage 6's lateness source: the ETW consumer's flips on the source display (head 0).
class EtwLatenessSource : public policy::ILatenessSource {
public:
    EtwLatenessSource(const EtwFlipConsumer& etw, int64_t cadenceWindow)
        : m_etw(etw), m_cadenceWindow(cadenceWindow) {}
    policy::LateCorrection MeasureLateness(int64_t batchStart,
                                           policy::AnchorChain& chain) override {
        return m_etw.MeasureLateness(0, batchStart, chain, m_cadenceWindow);
    }

private:
    const EtwFlipConsumer& m_etw;
    int64_t m_cadenceWindow;
};

static const char* DejitVerdictLabel(policy::DejitVerdict v) {
    switch (v) {
        case policy::DejitVerdict::LockDeclined: return "lock-declined";
        case policy::DejitVerdict::FenceBlocked: return "fence-blocked";
        default:                                 return "corrected";
    }
}

TemporalCaptureMode::TemporalCaptureMode(float framerate, bool vsyncPresent, float srcRateHint, bool lock,
                                         CompositorKind compositor, bool mark, unsigned int markFrames,
                                         bool tint, bool etw, bool noJoin, bool dejitter,
                                         bool fgPhase, bool phaseKeep, unsigned int extraLagMs,
                                         bool d3d11Present, bool deferCopy, bool wgc,
                                         bool wgcDrop, bool wgcCursor, bool wgcOpen, int wgcScale,
                                         bool wgcCopyEngine, int blendShader, bool tightWait)
    : m_bracketingDelayQpc(0)
    , m_assumedSrcPeriodQpc(0)
    , m_present(NULL)
    , m_pacing(NULL)
    , m_telemetryCountdown(0)
    , m_compositorKind(compositor)
    , m_lock(lock)
    , m_mark(mark)
    , m_markFrames(markFrames)
    , m_vsyncPresent(vsyncPresent)
    , m_targetFramerate(framerate)
    , m_srcRateHint(srcRateHint)
    , m_device(NULL)
    , m_etw(etw)
    , m_noJoin(noJoin)
    , m_dejitter(dejitter && etw && !noJoin)
    , m_fgPhase(fgPhase)
    , m_phaseKeep(phaseKeep && etw && !noJoin)
    , m_extraLagMs(extraLagMs)
    , m_deferCopy(deferCopy && !fgPhase && !phaseKeep)
    , m_wgc(wgc && d3d11Present)
    , m_wgcDrop(wgcDrop)
    , m_wgcCursor(wgcCursor)
    , m_wgcOpen(wgcOpen)
    , m_wgcScale(wgcScale)
    , m_wgcCopyEngine(wgcCopyEngine)
    , m_phaseKeepRequested(phaseKeep)
{
    m_baseQpc.QuadPart = 0;

    // The one place that knows there are two present paths. The D3D11 backend draws every
    // compositor itself (the nearest frame, the blend, the optical flow) for the vblank and timer
    // modes; the D3D9 path, for the DWM modes (and the timer modes under -d3d9), hosts the blend
    // or nearest IFrameCompositor the mode letter selected. Neither decides: they draw the policy
    // step's decision. Nothing touches a device here: the paths create their resources in Setup,
    // after the ring has started.
    if (d3d11Present) {
        // A timer present goes through the composition swapchain with a third buffer: on the
        // flip-model one the frame-latency wait held the loop until the card's vblank, and the
        // timer decided nothing (rig, 2026-10-29: in game b:60 waited 16.6 ms for room after
        // every wake); with two buffers the composition swapchain did the same under Pandora's
        // load; with three the timer kept its time (0 presents held).
        m_present = new D3D11PresentBackend(tint, compositor == kCompositorInterp, blendShader,
                                            compositor == kCompositorNearest, !vsyncPresent,
                                            m_wgc && wgcCopyEngine);
    } else {
        if (blendShader >= 0) LOG("-blend ignored: it draws on the D3D11 present only");
        IFrameCompositor* c;
        if (compositor == kCompositorBlend) {
            c = new BlendCompositor(tint);
        } else {
            c = new NearestCompositor();
        }
        m_present = new D3D9PresentPath(c);
    }
    // The clock: a timer, or the present path's own wait (the output's vblank on the D3D11
    // present, DWM's compose clock in the D3D9 one's blocking present).
    if (!vsyncPresent) {
        m_pacing = new TimerPacing(&m_scheduler, tightWait);
    } else {
        m_pacing = new PathPacing(d3d11Present);
    }
    // mktrace.py reads the compositor from the "Temporal[ blend| interp] (" prefix.
    const char* kindName = "Temporal";
    if (compositor == kCompositorInterp) kindName = "Temporal interp";
    else if (compositor == kCompositorBlend) kindName = "Temporal blend";
    snprintf(m_modeName, sizeof(m_modeName), "%s (%s, paced by %s)", kindName, m_present->Name(),
             m_pacing->Name());
}

TemporalCaptureMode::~TemporalCaptureMode() {
    // Stop the ETW session before anything else: its callback touches this object, and a
    // consumer thread outliving the object it writes into is a crash on shutdown.
    if (m_etw) {
        m_etwConsumer.LogSummary();
        m_etwConsumer.Stop();
    }
    delete m_pacing;
    delete m_present;
}

bool TemporalCaptureMode::PresentsViaD3D11() const {
    return m_present->OwnsOutputWindow();
}

bool TemporalCaptureMode::PresentsThroughComposition() const {
    return m_present->UsesComposition();
}

LONGLONG TemporalCaptureMode::LagForSourcePeriod(LONGLONG srcPeriodQpc) const {
    LONGLONG lag = srcPeriodQpc + srcPeriodQpc / 4;
    if (lag < m_scheduler.PeriodQpc()) lag = m_scheduler.PeriodQpc();
    return lag;
}

UINT TemporalCaptureMode::GetPresentationInterval() const {
    // The vsync present needs the D3D9 device created with INTERVAL_ONE so PresentEx blocks
    // on vsync. NOTE: windowed INTERVAL_ONE blocks on DWM's compose clock, whose identity is
    // regime-dependent: composed desktop -> primary/source display; fullscreen game on the
    // source -> DWM composes only the card's display and the present is card-locked 60 Hz
    // (the production case). On the D3D11 path the D3D9 swapchain never presents and this
    // is moot.
    return m_vsyncPresent ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE;
}

// The rotation length for the CURRENT grid, from measurements only: flips per source period
// from the declared -src against the measured flip spacing, stride from the ring's own
// batch-period estimate against the same spacing. x2 and frame-generation-off both yield 1
// and the mechanism stays inert; x3 yields 3. Returning 1 whenever anything is unknown is
// what makes "no flip data yet" indistinguishable from "cannot rotate", which is correct -
// both mean plain keep-real.
bool TemporalCaptureMode::Grid(long long batchPeriodTicks, int* outStride,
                               int* outFlipsPerSource, long long* outSpacingTicks) {
    if (!m_etw || m_noJoin || m_assumedSrcPeriodQpc <= 0 || batchPeriodTicks <= 0) return false;
    const LONGLONG spacing = m_etwConsumer.MedianFlipSpacing(0, m_flipCadenceWindowQpc);
    if (spacing <= 0) return false;
    *outFlipsPerSource = (int)((m_assumedSrcPeriodQpc + spacing / 2) / spacing);
    *outStride = (int)((batchPeriodTicks + spacing / 2) / spacing);
    *outSpacingTicks = spacing;
    return true;
}

bool TemporalCaptureMode::AnchorAndSteps(long long batchStartTs, long long prevAnchorTs,
                                         long long* outOffset, int* outSteps) {
    if (!m_etw || m_noJoin) return false;
    const policy::FlipPairing fp =
        m_etwConsumer.PairCapture(0, batchStartTs, 0, m_flipCadenceWindowQpc);
    if (!fp.anchorFound) return false;
    *outOffset = fp.anchorOffset;
    *outSteps = 1;
    if (prevAnchorTs >= 0) {
        const long long anchorTs = batchStartTs - fp.anchorOffset;
        *outSteps = m_etwConsumer.CountFlipsBetween(0, prevAnchorTs, anchorTs);
    }
    return true;
}

MaybeFailure TemporalCaptureMode::Setup(const RelayContext& ctx) {
    m_device = ctx.presentDevice;

    // Before the ring starts: the instrument allocates its readback resources in Start.
    if (m_fgPhase) {
        m_ring.EnableFgPhase();
    }
    if (m_phaseKeep) {
        m_ring.EnablePhaseKeep(this);
    }
    if (!m_ring.Setup(m_device, ctx.width, ctx.height) ||
        !m_scheduler.Setup(m_targetFramerate)) {
        return ModeCouldNotStart(GetModeName());
    }
    // STATIC BRACKETING LAG: max(present period, 1.25 x assumed source period). The lag
    // exists so that a frame newer than the target has already arrived at pick time; the
    // worst-case wait is one source period, and 1.25x covers arrival jitter on top of it.
    // The assumption defaults to 60 fps (the slowest source served without configuration);
    // -src overrides it in either direction: slower sources need more lag, faster ones can
    // ride the present-period floor. Computed once and never moved: the lag is output
    // latency, and only a constant can be compensated for downstream (T10). The measured
    // source period is not fed back into the lag; it only audits the assumption (telemetry).
    const float assumedFps = policy::AssumedSrcFps(m_srcRateHint);
    m_assumedSrcPeriodQpc = (LONGLONG)((double)m_scheduler.Freq() / assumedFps);
    m_bracketingDelayQpc = LagForSourcePeriod(m_assumedSrcPeriodQpc);
    if (m_extraLagMs > 0) {
        m_bracketingDelayQpc += (LONGLONG)m_extraLagMs * m_scheduler.Freq() / 1000;
        // Slots past the default cost VRAM only when the lag actually asks for them. The
        // rule itself is policy::RingSlotsForLag so the suite can pin it; only the extra-lag
        // case resizes, because the default depth already covers the unextended lag at every
        // source rate the relay accepts.
        m_ring.SetSlotsInUse(policy::RingSlotsForLag(m_bracketingDelayQpc,
                                                    m_assumedSrcPeriodQpc,
                                                    CaptureRing::kDefaultRingSlots,
                                                    CaptureRing::RING_SIZE));
    }
    // The deferred ring copy sizes its limits from this lag, with one refresh of the output as
    // the margin.
    if (m_deferCopy) {
        m_ring.EnableDeferCopy(m_bracketingDelayQpc, m_assumedSrcPeriodQpc,
                               ctx.sinkRefreshHz > 0 ? m_scheduler.Freq() / ctx.sinkRefreshHz
                                                     : m_scheduler.PeriodQpc());
    }
    if (m_wgc) {
        m_ring.EnableWgc(m_wgcDrop, m_wgcCursor, m_wgcOpen, m_wgcScale, m_wgcCopyEngine);
        LOG("Capture through Windows.Graphics.Capture asked (-wgc): it takes over from NvFBC "
            "once the D3D11 present is up, and NvFBC stays if it cannot start");
    }
    m_flipCadenceWindowQpc = m_scheduler.Freq() / 5;    // 200 ms; see the header for why
    m_telemetryCountdown = kTelemetryPeriodPresents;

    // Selection stickiness (Schmitt band): prefer the before-frame unless the after-frame is
    // closer by more than this margin. Without it, when the target dwells near the midpoint
    // between two source frames, capture jitter (~±300 µs) flips the nearest-pick every present
    // — measured at 240→60 (v0.0.10 validation) as ~6 s windows of stride-3/5 alternation
    // (period-2 judder) every ~33.5 s. With the band, the pick holds one side through the dwell
    // and slips exactly once per sweep (a single 1-source-frame step — imperceptible). 1 ms:
    // comfortably above jitter, well below any source period we target (4.17 ms at 240 Hz).
    m_policyCfg.stickinessQpc = m_scheduler.Freq() / 1000;

    LOG("Temporal mode initialized - paced by %s (%.2f fps nominal), nearest-frame selection + hysteresis",
        m_pacing->Name(), m_targetFramerate);
    if (m_srcRateHint <= 0.0f) {
        LOGERR("Source rate NOT DECLARED: %.1f fps ASSUMED. The lag below is sized for it, and so "
               "are the comb lock and the passthrough threshold wherever they run. Declare -src "
               "<base fps> for any other source; 60x2 frame generation is -src 60", assumedFps);
    }
    LOG("Temporal lag fixed at %lld us (source assumed %s%.1f fps)",
        m_bracketingDelayQpc * 1000000 / m_scheduler.Freq(),
        (m_srcRateHint > 0.0f) ? "-src " : ">= ", assumedFps);
    if (m_extraLagMs > 0) {
        LOG("Extra bracketing lag ACTIVE (-lag %u): +%u ms of output latency buys fewer holds "
            "(a hold re-presents the last output; the frame it wanted arrives late, not never). "
            "Ring grown to %d slots so the target stays inside the search window.",
            m_extraLagMs, m_extraLagMs, m_ring.SlotsInUse());
    }
    LOG("Selection stickiness band: %lld us (anti flip-flop at bracket midpoint)",
        m_policyCfg.stickinessQpc * 1000000 / m_scheduler.Freq());

    // PHASE COMB LOCK (see docs/phase-comb-lock-spec.md). Each present the target's phase
    // within a source interval advances by (presentP mod srcP); at a rational rate ratio
    // N:M (reduced) it visits exactly M values spaced srcP/M apart - the comb. A slow
    // control term (the pull, applied as extra lag) locks the target onto the comb, so
    // selection operates at a stable phase just behind each real frame instead of sweeping
    // through the bracket every beat: the boundary-dwell excursions (gate-decline repeat,
    // then a multi-frame catch-up) become unreachable, and one comb-spacing slip per beat
    // remains - the same slip an unlocked beat already pays. Anchored to the declared -src,
    // or to the assumed 60 without one: an undeclared source at another rate either shares
    // that comb or sweeps across it, and a sweeping phase rarely settles the stability gate,
    // so the lock stays released. The denominator scan is capped: past M=8 the comb spacing
    // approaches arrival jitter, the stability gate cannot close, and the lock refuses -
    // correct for effectively-irrational ratios. Latency: the pull adds a bounded slow sawtooth
    // (<= one comb spacing peak-to-peak, drift-rate ramp, one discrete step per beat),
    // accepted as a documented trade alongside the static lag (spec clause 4).
    m_policyCfg.phasePullSlewQpc = m_scheduler.Freq() / 40000;   // 25 us per present
    // Twice the declared source period: wide enough that ordinary jitter and a single
    // dropped frame stay under it, narrow enough that a source hitching at ~100 ms per frame
    // reads as stalled on the first present. A frozen source delivers nothing at all, and
    // its one-sided brackets read as stalled whatever this is.
    m_policyCfg.stallSpanQpc = m_assumedSrcPeriodQpc * 2;
    // Under phase-aware keep-real the valid-frame cadence is ONE per source period (that is
    // the point), so a single missing real frame opens a bracket span of exactly two source
    // periods - the 2x threshold above with one microsecond of margin, measured. 2.5x keeps
    // one missing frame from reading as a stall while a genuinely hitching source still
    // trips it on the first present.
    if (m_phaseKeep) m_policyCfg.stallSpanQpc = m_assumedSrcPeriodQpc * 5 / 2;
    const float lockAnchorFps = policy::LockAnchorFps(m_lock, m_srcRateHint);
    if (lockAnchorFps > 0.0f) {
        bool combMatched = false;
        const int combM = policy::CombDenominator((double)lockAnchorFps,
                                                  (double)m_targetFramerate, &combMatched);
        m_policyCfg.combQpc = m_assumedSrcPeriodQpc / combM;
        LOG("Phase comb lock ACTIVE (-lock): modulus %lld us (ratio denominator M=%d%s); pull=/lk= on the temporal line",
            m_policyCfg.combQpc * 1000000 / m_scheduler.Freq(), combM,
            combMatched ? "" : ", no rational match: M=1 fallback, stability gate decides");
        // The composite tooth guard rides the comb, for sources at or above the SINK rate
        // (an eighth of tolerance for declared-vs-nominal skew). The sink is what decides
        // whether interpolating between source frames buys anything: at or below the
        // source rate the extra frames are discarded by the display and all a mid-tooth
        // blend does is make which frames survive a matter of sampling phase, while above
        // it they are genuinely shown and the synthesis is rate conversion doing its job.
        //
        // NOT the present period, which only coincides with the sink under a vsync present
        // whose compose clock happens to run at the sink rate. A timer present (b:120)
        // decouples the two, and reading the present period there would disarm the guard
        // in exactly the regime it exists for. Falls back to the present period when the
        // refresh could not be read, which is the pre-existing behavior.
        const LONGLONG sinkPeriodQpc = ctx.sinkRefreshHz > 0
                                           ? m_scheduler.Freq() / ctx.sinkRefreshHz
                                           : m_scheduler.PeriodQpc();
        m_policyCfg.srcPeriodQpc =
            policy::ToothGuardPeriod(m_assumedSrcPeriodQpc, sinkPeriodQpc, /*combOn=*/true);
    } else {
        LOG("Phase comb lock off (-nolock); target rides the static lag alone");
    }

    // COMPOSITOR: nearest keeps the validated selection path; the synthesizing
    // compositors (blend, interp) pass a real frame through sharp whenever one sits
    // within the passthrough threshold of the target and synthesize at the bracket
    // weight otherwise. A source at twice the present rate or faster (whose frames are
    // always within half a source period of any target) takes a quarter of the present
    // period and passes through free; every other source takes a quarter of its own
    // period, far above the locked operating point and far below the mid-gap distance
    // of a hole, so the gate cannot chatter. The rule is policy's so the replay sizes
    // the same gate; see PassthroughThreshold for the regime that told the two apart.
    // The compositor itself lives behind the present path and initializes there, once
    // the ring has created the slot shared handles.
    if (m_compositorKind != kCompositorNearest) {
        m_policyCfg.passthroughQpc =
            policy::PassthroughThreshold(m_assumedSrcPeriodQpc, m_scheduler.PeriodQpc());
        const bool interp = m_compositorKind == kCompositorInterp;
        LOG("%s compositor ACTIVE on the %s: passthrough threshold %lld us; %s on the temporal line",
            interp ? "Interp" : "Blend", m_present->Name(),
            m_policyCfg.passthroughQpc * 1000000 / m_scheduler.Freq(),
            interp ? "op=/bw=/sx=/fl=, and pt= (an earlier warp's GPU microseconds) with pta= "
                     "(how many presents earlier)" : "op=/bw=");
        // The phase-step lookahead needs the comb to be one source frame, which is where a step
        // leaves the target between real frames. It reads frames ahead of the target, so it
        // only has room to work when the lag is longer than a few source periods.
        m_policyCfg.phaseLookahead =
            m_policyCfg.combQpc > 0 && m_policyCfg.combQpc == m_assumedSrcPeriodQpc;
        // A step that settles in the ring too late to plan ahead (right after a hitch) is met
        // once the target reaches it.
        m_policyCfg.lateLookahead = m_policyCfg.phaseLookahead;
        // Right after a hitch a step can be measured from the hitch's uneven frames; one whose
        // first frame is already at the target is refused and left to a late move or the lock.
        m_policyCfg.lookaheadNeedsLead = m_policyCfg.phaseLookahead;
        LOG("Phase-step lookahead %s", m_policyCfg.phaseLookahead
                                            ? "ACTIVE with late moves, refusing a step first seen at the target: rephase: lines when a step is seen, moved, cancelled or refused"
                                            : "off (comb lock off, or its comb is finer than a source frame)");
        if (m_policyCfg.srcPeriodQpc > 0) {
            m_policyCfg.resumeGuardExemptPresents = policy::kResumeGuardExemptPresents;
            LOG("Composite tooth guard ACTIVE: synthesis must advance a full source period "
                "(%lld us teeth); op=hold-comb between teeth; stands aside for %d presents after a "
                "stall resume while presenting at the source rate",
                m_policyCfg.srcPeriodQpc * 1000000 / m_scheduler.Freq(),
                m_policyCfg.resumeGuardExemptPresents);
        } else {
            // Stated rather than left as a missing line: a run that quietly lost the guard
            // reads as a parity-lottery blend storm with no explanation in the log.
            LOG("Composite tooth guard off (%s); mid-tooth targets synthesize",
                m_policyCfg.combQpc <= 0
                    ? "needs the comb lock, and -nolock turned it off"
                    : "source is slower than the sink, so synthesis is rate conversion");
        }
    }

    // RESOLVED-CONFIG VALIDATION, deliberately last: everything above may still be deciding
    // what the config IS, so a cross-feature requirement checked earlier reads a field that
    // has not been written yet. That exact mistake shipped once - this check sat above the
    // comb-lock block that sets combQpc, so -dejit refused itself in EVERY configuration
    // and a live A/B measured two identical runs.
    //
    // -dejit needs the comb lock: the calm gate that pauses corrections through stall
    // recoveries reads lock state, and without a lock it is vacuously open. Refusing loudly
    // beats running unguarded through exactly the window corrections were measured to harm.
    if (m_dejitter && m_policyCfg.combQpc <= 0) {
        LOGERR("-dejit REFUSED: needs the comb lock for its calm gate, and -nolock turned it "
               "off; running without delivery-lateness correction");
        m_dejitter = false;
    }
    if (m_dejitter) {
        LOG("Delivery-lateness correction ACTIVE (-dejit): late batches corrected onto the "
            "flip grid via the stamp overlay; dejit: lines mark each verdict");
    }
    if (m_phaseKeep) {
        LOG("Phase-aware keep-real ACTIVE (-phasekeep): the batch-composition rotation is "
            "voted from arrival timing, and [real,gen] batches keep member 0 instead of "
            "member 1. Inert wherever composition does not rotate (x2, FG off).");
    } else if (m_phaseKeepRequested) {
        LOGERR("-phasekeep REFUSED: needs -etw with the join on (the rotation is read from "
               "the flip grid); running plain keep-real");
    }
    return std::nullopt;
}

MaybeFailure TemporalCaptureMode::Run(RelayContext& ctx,
                                      NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams) {
    QueryPerformanceCounter(&m_baseQpc);
    // Every time in this log (arr=, dl=, tgt=, before=, after=) is microseconds since this
    // origin, not absolute QPC. Anything correlating the log against another QPC-stamped
    // source - an ETW trace, a second process - needs the origin to convert, and recovering
    // it by cross-correlating event sequences is guesswork this one line removes.
    LOG("QPC origin %lld ticks, frequency %lld Hz (log times are us since the origin)",
        m_baseQpc.QuadPart, m_scheduler.Freq());
    // Started here rather than in Setup because it needs that origin. A failure never stops
    // the capture: without flip timing the relay runs everything except late-batch
    // correction, and a stream is not worth taking down over that. It does interrupt, because
    // the console is closed by the time this runs and a failure that was only logged would
    // stay invisible until the capture is spent. Administrator rights are rarely the cause,
    // since the manifest always elevates, so the popup names them only when access was denied.
    if (m_etw && !m_etwConsumer.Start(m_scheduler.Freq(), m_baseQpc.QuadPart)) {
        const bool dejitterLost = m_dejitter;
        if (m_dejitter) {
            // Off before the loop, so the dejit summary cannot report work that never ran.
            LOGERR("Delivery-lateness correction OFF for this run: -dejit needs flip timing, and "
                   "the ETW session did not start");
            m_dejitter = false;
        }
        const unsigned long code = m_etwConsumer.StartError();
        char text[640];
        snprintf(text, sizeof(text),
                 "NvFBCR could not read frame timing from the graphics driver (error %lu).\n\n"
                 "%s%s\n\n"
                 "To skip this check and this message, launch with -noetw.",
                 code,
                 dejitterLost ? "It keeps running, but may show a few more double-image frames "
                                "this session."
                              : "It keeps running as normal.",
                 code == ERROR_ACCESS_DENIED
                     ? "\n\nAccess was denied. Run NvFBCR.exe as administrator."
                     : "");
        ShowWarning("frame timing unavailable", text);
    }
    const double usPerTick = 1000000.0 / (double)m_scheduler.Freq();
    const long long lagUs = (long long)(m_bracketingDelayQpc * usPerTick);

    // Start releases the shell's startup session (bound to the present device) and rebinds
    // NvFBC to the ring's private capture device, which it creates on the window every D3D9
    // device shares. From here the ring owns the session.
    if (!m_ring.Start(ctx, grabParams, m_baseQpc)) {
        if (m_ring.SessionRefused()) return SessionRefused(ctx.nvfbc, "CaptureRing");
        return CaptureCouldNotStart("CaptureRing");
    }

    // The present path finishes initializing now that the ring's slot shared handles exist:
    // the D3D9 compositor and its capture-side aliases, or the whole D3D11 backend. Either
    // refuses the mode when it cannot finish instead of silently running a different one,
    // and the refusal ends the run with its popup.
    if (!m_present->Setup(ctx, &m_ring, &m_policyCfg, m_mark, m_markFrames, m_baseQpc,
                          m_scheduler.Freq())) {
        m_ring.Stop();
        return PresentPathRefused(m_present->Name(), m_present->RefusalAdvice());
    }

    MaybeFailure ended;
    LONGLONG lastPresentQpc = 0;
    CaptureRingView ringView(m_ring);
    EtwLatenessSource lateness(m_etwConsumer, m_flipCadenceWindowQpc);
    m_pacing->Start();

    while (TRUE)
    {
        // Present timing (Pacing.h). The timer waits on the absolute-QPC deadline and hands it
        // to the step. A display's clock (the output's vblank, DWM's compose) anchors the target
        // to "now", just after the clock ticked, so selection runs on that clock rather than
        // QPC.
        //
        // How long this present's pacing wait blocked on a display's clock (blk= on the line).
        // A vsync present that is doing its job spends nearly the whole period waiting; one that
        // returns at once is not pacing anything, and pdt alone cannot tell those apart. A clock
        // waited on BEFORE the decision is measured by the pacing; the D3D9 present, which
        // paces in the present itself, reports the block from Present, so the sum is the whole
        // wait either way.
        LONGLONG blockTicks = 0;
        const LONGLONG deadline = m_pacing->Wait(m_present, &blockTicks);
        // THE POLICY STEP (policy::StepPresent, in src/core, the code the replay runs): the
        // target behind the deadline by the lag, the comb lock's pull and a wrap's ease; a
        // planned phase-step move landing; stage 6 settling delivery-lateness corrections
        // before the bracket reads the ring (the walk consumes the ring's batch-start history,
        // never slot fields, which the capture thread owns); the bracket; the lock's update
        // for the next present; the lookahead; the tooth guard's exemption; the decision. The
        // step returns what happened, and the lines below log it in the order it happened.
        // Stage 6 runs only with -dejit; the overlay it fills is empty otherwise.
        const policy::PresentStepResult step =
            policy::StepPresent(m_step, m_policyCfg, ringView, m_dejitter ? &lateness : NULL,
                                deadline, m_bracketingDelayQpc);
        const LONGLONG target = step.target;
        if (step.rephase != 0) {
            LOG("rephase: pull moved %+lld us onto a phase step",
                (long long)(step.rephase * usPerTick));
        }
        // Every stage-6 verdict is counted; late-but-blocked is logged, because a capture
        // where corrections were measured and discarded must not read as one with no late
        // deliveries.
        m_dejitSkipped += step.dejitSkipped;
        m_dejitMeasured += step.dejitMeasured;
        m_dejitLate += step.dejitLate;
        m_dejitLockDeclined += step.dejitLockDeclined;
        m_dejitFenceBlocked += step.dejitFenceBlocked;
        m_dejitCorrected += step.dejitCorrected;
        for (int i = 0; i < step.nLateBatches; i++) {
            const policy::LateBatch& lb = step.lateBatches[i];
            LOG("dejit: batch arr=%lldus late by %lldus, %s",
                (long long)((lb.batchStart - m_baseQpc.QuadPart) * usPerTick),
                (long long)(lb.correction * usPerTick), DejitVerdictLabel(lb.verdict));
        }
        if (step.lookaheadEvent == policy::LookaheadEvent::Planned) {
            LOG("rephase: step of %+lld us seen in the ring, first frame %lld us ahead "
                "of the target",
                (long long)(step.lookaheadSize * usPerTick),
                (long long)((step.lookaheadFirstTs - target) * usPerTick));
        } else if (step.lookaheadEvent == policy::LookaheadEvent::Cancelled) {
            LOG("rephase: cancelled, the newest frames went back before the step");
        } else if (step.lookaheadEvent == policy::LookaheadEvent::PlannedLate) {
            LOG("rephase: late step of %+lld us, the target already sits on it",
                (long long)(step.lookaheadSize * usPerTick));
        } else if (step.lookaheadEvent == policy::LookaheadEvent::Refused) {
            LOG("rephase: refused a step of %+lld us, first frame only %lld us ahead of "
                "the target",
                (long long)(step.lookaheadSize * usPerTick),
                (long long)((step.lookaheadFirstTs - target) * usPerTick));
        }
        // The ring's whole bracket, with the step's view of it (the guard's exemption).
        FrameBracket bracket = ringView.LastBracket();
        bracket.info = step.bracket;

        // The DECISION was the step's (selection or composite, in TemporalPolicy.cpp with
        // the mechanism rationale); the present path executes it onto its own target,
        // burns the marker over it (once per present, repeats included: the counter
        // identifies presented frames, not source frames; before the present stamp so
        // jit/pdt absorb its cost) and shows it. This loop owns the timing and the log.
        // A path that plans ahead (the optical flow) is told the next presents as the step
        // will decide them: a copy of the step state stepped at deadlines one present period
        // on, so the plan sees the lock's ease, a lookahead move and the tooth guard as those
        // presents will.
        const int ahead = m_present->LookaheadPresents();
        if (ahead > 0) {
            static const int kMaxAhead = 4;
            policy::PresentStepResult planned[kMaxAhead];
            FrameBracket upcoming[kMaxAhead];
            LONGLONG targets[kMaxAhead];
            bool synthesizes[kMaxAhead];
            const int n = ahead < kMaxAhead ? ahead : kMaxAhead;
            policy::PlanNextPresents(m_step, m_policyCfg, ringView, deadline,
                                     m_scheduler.PeriodQpc(), m_bracketingDelayQpc, n, planned);
            for (int j = 0; j < n; j++) {
                targets[j] = planned[j].target;
                const FrameBracket* read = ringView.BracketAt(targets[j]);
                upcoming[j] = read ? *read : FrameBracket{};
                upcoming[j].info = planned[j].bracket;
                synthesizes[j] = planned[j].decision.op == policy::CompositeOp::Synthesize;
            }
            m_present->PlanAhead(upcoming, targets, synthesizes, n);
        }

        CompositeOutcome outcome;
        m_present->Compose(bracket, step, &outcome);
        const long long markN = m_present->BurnMarker(outcome);
        LARGE_INTEGER beforePresent;
        QueryPerformanceCounter(&beforePresent);
        blockTicks += m_present->Present();

        // Inter-present interval (should hold steady at the present period if the scheduler works).
        LONGLONG presentDelta = (lastPresentQpc != 0) ? (beforePresent.QuadPart - lastPresentQpc) : 0;
        lastPresentQpc = beforePresent.QuadPart;

        // Logging: bracket timestamps double as the source timeline; w is what blend would use;
        // jit is actual-present vs scheduled deadline; pdt is the actual inter-present gap.
        if (!bracket.info.hasBefore) {
            // Benign while the ring is still filling at startup; once it has wrapped at least
            // once it means the target fell off the back of the ring.
            if (m_ring.Published() >= m_ring.SlotsInUse()) {
                LOGERR("temporal: target older than ring window - ring too small / delay too large (p=%lld)",
                    m_ring.Published());
            }
        } else {
            // pull/lk/mark are append-only: effective latency = lag + pull (a bounded
            // sawtooth at lock); lk=-1 marks the lock feature disabled entirely; mark=-1
            // marks the frame marker disabled (video-to-log join key otherwise).
            // op=/bw= append only in blend mode (what the compositor did and at what
            // weight); nearest lines carry no new fields.
            int lkField = -1;
            if (m_policyCfg.combQpc > 0) {
                lkField = m_step.lock.engaged ? 1 : 0;
            }
            // Where the shown frame actually scanned out, when ETW is running. Diagnostic:
            // nothing above this point consulted it, and the selection that produced this
            // line was made from arrival stamps exactly as it always has been.
            //
            // Done HERE rather than at capture time on purpose. At grab-return the member's
            // flip is usually not announced yet (it scans out a grid step later), so a
            // capture-time verdict would report not-yet-announced for structural reasons and
            // measure nothing. By present time the target is a bracketing lag back and the
            // data has arrived.
            //
            // Head 0 is the source display and head 1 is the relay's own output. If that
            // ever inverts, this reads as a total pairing failure in the log rather than as
            // plausible wrong numbers.
            //
            // BOTH sides, not just the before-frame: pass-after is the most common outcome
            // (measured 49% of presents at 60x2), so pairing only the before-frame would
            // report a scanout time for a frame that was not shown on most lines. With both,
            // aflip minus bflip is also the TRUE source interval the bracket spans, which is
            // the quantity that exposed the x3 judder and is worth having per present.
            //
            // Field names mirror the before=/after= pair already on this line rather than
            // compressing to a prefix, so the line needs no legend to read.
            char flipFields[176] = "";
            if (m_etw && !m_noJoin) {
                auto place = [&](char* out, size_t cap, const char* tag, bool has,
                                 int64_t stampTs, int member) {
                    if (!has) return 0;
                    const policy::FlipPairing fp =
                        m_etwConsumer.PairCapture(0, stampTs, member, m_flipCadenceWindowQpc);
                    if (fp.paired) {
                        return snprintf(out, cap, " %sflip=%lldus %soff=%lldus %smem=%d",
                                        tag,
                                        (long long)((fp.displayTs - m_baseQpc.QuadPart) * usPerTick),
                                        tag, (long long)(fp.anchorOffset * usPerTick),
                                        tag, member);
                    }
                    return snprintf(out, cap, " %sflip=none:%s %smem=%d", tag,
                                    fp.memberAhead ? "ahead" :
                                    fp.gridGap ? "gap" :
                                    fp.anchorFound ? "unplaced" : "noanchor",
                                    tag, member);
                };
                // n == 0 when there is no before-frame, which must still leave the after-side
                // reported rather than skipped: a one-sided bracket is exactly the case where
                // knowing what the surviving side scanned out is most useful.
                int n = place(flipFields, sizeof(flipFields), "b", bracket.info.hasBefore,
                              bracket.info.beforeTs, bracket.beforeMember);
                if (n < 0) n = 0;
                if ((size_t)n < sizeof(flipFields)) {
                    place(flipFields + n, sizeof(flipFields) - n, "a", bracket.info.hasAfter,
                          bracket.info.afterTs, bracket.afterMember);
                }
            }
            char opFields[96] = "";
            if (outcome.opLabel) {
                int n = snprintf(opFields, sizeof(opFields), " op=%s bw=%.3f",
                                 outcome.opLabel, outcome.opWeight);
                if (outcome.synthExec && n > 0 && (size_t)n < sizeof(opFields)) {
                    n += snprintf(opFields + n, sizeof(opFields) - n, " sx=%s",
                                  outcome.synthExec);
                }
                if (outcome.synthUs >= 0 && n > 0 && (size_t)n < sizeof(opFields)) {
                    snprintf(opFields + n, sizeof(opFields) - n, " pt=%lld", outcome.synthUs);
                }
            }
            // The ease left after this present's lock update, printed only while a wrap is being
            // crossed, like pull= beside it.
            char easeField[32] = "";
            if (m_step.lock.easeQpc != 0) {
                snprintf(easeField, sizeof(easeField), " ease=%lldus",
                         (long long)(m_step.lock.easeQpc * usPerTick));
            }
            // How the optical flow's synthesized present got its flow, then how many presents ago
            // the synthesis pt= timed was drawn, then the -blend pan shader's shift for that same
            // draw, last on the line.
            char flowField[64] = "";
            int nf = 0;
            if (outcome.flowLabel) {
                nf = snprintf(flowField, sizeof(flowField), " fl=%s", outcome.flowLabel);
            }
            if (outcome.synthAge >= 0 && nf >= 0 && (size_t)nf < sizeof(flowField)) {
                nf += snprintf(flowField + nf, sizeof(flowField) - nf, " pta=%d", outcome.synthAge);
            }
            if (outcome.panFollow >= 0 && nf >= 0 && (size_t)nf < sizeof(flowField)) {
                snprintf(flowField + nf, sizeof(flowField) - nf, " ps=%.1f,%.1f,%.2f,%s",
                         (double)outcome.panX, (double)outcome.panY, (double)outcome.panRatio,
                         outcome.panFollow ? "y" : "n");
            }
            // blk= goes after the appended op/flip fields, followed only by ease=, fl=, pta= and
            // ps=.
            // The offline parsers match this line as a contiguous chain of named fields, so a
            // field inserted between two existing ones silently drops every field after it rather
            // than failing.
            LOG("temporal dl=%lldus tgt=%lldus before=%lldus(d%d) after=%lldus w=%.3f pick=%s jit=%lldus pdt=%lldus lag=%lldus pull=%lldus lk=%d mark=%lld%s%s blk=%lldus%s%s",
                (long long)((deadline - m_baseQpc.QuadPart) * usPerTick),
                (long long)((target - m_baseQpc.QuadPart) * usPerTick),
                (long long)((bracket.info.beforeTs - m_baseQpc.QuadPart) * usPerTick), bracket.beforeDepth,
                bracket.info.hasAfter ? (long long)((bracket.info.afterTs - m_baseQpc.QuadPart) * usPerTick) : -1LL,
                bracket.weight, outcome.pickLabel,
                (long long)((beforePresent.QuadPart - deadline) * usPerTick),
                (long long)(presentDelta * usPerTick),
                lagUs,
                (long long)(m_step.lock.pullQpc * usPerTick),
                lkField,
                markN,
                opFields,
                flipFields,
                (long long)(blockTicks * usPerTick),
                easeField,
                flowField);
            // Once per run rather than once per present: a static screen delivers no new
            // frames at all, so a run lasts as long as the screen does, and every present in
            // it already carries after=-1 on its own line.
            if (!bracket.info.hasAfter) {
                if (m_noAfterRun == 0) {
                    LOG("temporal: no after-frame (source slower than present?) - repeating newest");
                }
                m_noAfterRun++;
            } else if (m_noAfterRun > 0) {
                LOG("temporal: after-frame back after %lld presents without one", m_noAfterRun);
                m_noAfterRun = 0;
            }
        }

        // ESTIMATOR TELEMETRY: the measured source period never drives the lag; it audits
        // the declared assumption. Slower than assumed means the bracketing headroom is gone
        // and repeats follow (wrong -src, or the source is struggling); much faster than
        // assumed means latency is left on the table, and a lag exceeding the ring's history
        // at the measured rate means selection is pinned at the ring's oldest frame.
        if (--m_telemetryCountdown <= 0) {
            m_telemetryCountdown = kTelemetryPeriodPresents;
            const LONGLONG est = m_ring.EstimatedSourcePeriodQpc();
            if (est > 0) {
                const double estFps = (double)m_scheduler.Freq() / (double)est;
                const LONGLONG lagForEst = LagForSourcePeriod(est);
                const long long lagForEstUs = (long long)(lagForEst * usPerTick);
                if (m_bracketingDelayQpc > est * m_ring.SlotsInUse()) {
                    LOGERR("temporal telemetry: lag %lld us exceeds ring history at the measured ~%.1f fps - display pinned at oldest frame; pass -src %.0f (lag %lld us)",
                        lagUs, estFps, estFps, lagForEstUs);
                } else if (est > m_assumedSrcPeriodQpc + m_assumedSrcPeriodQpc / 8) {
                    LOGERR("temporal telemetry: source measuring ~%.1f fps, slower than assumed - expect repeats; pass -src %.0f (lag %lld us)",
                        estFps, estFps, lagForEstUs);
                } else if (est * 2 < m_assumedSrcPeriodQpc && lagForEst + m_scheduler.Freq() / 500 < m_bracketingDelayQpc) {
                    LOG("temporal telemetry: source measuring ~%.1f fps; -src %.0f would lower lag to %lld us",
                        estFps, estFps, lagForEstUs);
                }
            }
        }

        m_pacing->Advance();

        if (!PumpMessages()) break;
        if (m_ring.HasStopped()) {
            // The capture thread stops itself only when NvFBC invalidates its session.
            ended = CaptureLost("CaptureRing");
            break;
        }
        if (m_present->SwapChainStalled()) {
            // The swapchain stopped retiring frames and does not come back. Stopping is the
            // only correct move: nothing reaches the screen either way, and a loop left
            // turning here holds the NvFBC session against the next run while its output
            // window may not even be somewhere a person can close it.
            LOGERR("%s: swapchain stalled - no paced present for seconds. Stopping the "
                   "capture so the session is released and this run ends attributably.",
                   m_present->Name());
            ended = OutputStalled(m_present->Name());
            break;
        }
    }

    m_present->LogSummary();
    m_pacing->LogSummary();

    if (m_phaseKeep) {
        // Without the flipped count, a live A/B cannot tell "the rotation was read and no
        // batch needed flipping" from "the vote never converged and this ran as plain
        // keep-real" - which are the pass and the silent-no-op, and they look identical in
        // the output.
        LOG("phasekeep summary: %lld batches steered, %lld kept an earlier member, "
            "%lld all-generated batches dropped, %lld singles reclaimed, "
            "%lld undecided dropped, %lld vote resets",
            m_ring.PhaseKeepBatches(), m_ring.PhaseKeepFlipped(),
            m_ring.PhaseKeepEmpty(), m_ring.PhaseKeepReclaimed(),
            m_ring.PhaseKeepUndecided(), m_ring.PhaseKeepResets());
    }
    if (m_dejitter) {
        LOG("dejit summary: %lld batches measured, %lld late, %lld corrected, "
            "%lld fence-blocked, %lld lock-declined, %lld skipped",
            m_dejitMeasured, m_dejitLate, m_dejitCorrected,
            m_dejitFenceBlocked, m_dejitLockDeclined, m_dejitSkipped);
    }
    m_ring.Stop();
    // After Stop: the capture thread has joined, so its counter is settled.
    LOG("capture summary: %lld grabs waited out the grab timeout and stored nothing (the source drew no new frame)",
        m_ring.GrabTimeoutsSkipped());
    return ended;
}

const char* TemporalCaptureMode::GetModeName() const {
    return m_modeName;
}
