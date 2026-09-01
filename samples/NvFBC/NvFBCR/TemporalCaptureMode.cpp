#include "TemporalCaptureMode.h"
#include "FrameCompositors.h"
#include <SimpleLogger.h>
#include <cstdio>

// External global variables
extern IDirect3DDevice9Ex* g_pD3D9Device;
extern IDirect3DSurface9* g_backbuffer;
extern int BUF_WIDTH;
extern int BUF_HEIGHT;
extern int g_targetRefreshHz;
extern bool g_flipEx;

// Presents between estimator-vs-assumption audits (about 10 s at 60 Hz): rare enough to keep
// the log quiet, frequent enough that a wrong -src is caught within the first minute.
static const int kTelemetryPeriodPresents = 600;

// Source rate assumed when -src is not given: the slowest source served without
// configuration, at any present rate. Slower sources need an explicit -src.
static const float kDefaultAssumedSrcFps = 60.0f;

TemporalCaptureMode::TemporalCaptureMode(float framerate, bool vsyncPresent, float srcRateHint, bool lock,
                                         CompositorKind compositor, bool mark, unsigned int markFrames,
                                         bool tint, bool etw, bool noJoin, bool dejitter,
                                         bool fgPhase, bool phaseKeep, bool subGen,
                                         bool diffMap, unsigned int extraLagMs)
    : m_bracketingDelayQpc(0)
    , m_assumedSrcPeriodQpc(0)
    , m_compositor(NULL)
    , m_telemetryCountdown(0)
    , m_compositorKind(compositor)
    , m_lock(lock)
    , m_mark(mark)
    , m_markFrames(markFrames)
    , m_tint(tint)
    , m_etw(etw)
    , m_noJoin(noJoin)
    , m_dejitter(dejitter && etw && !noJoin)
    , m_fgPhase(fgPhase)
    , m_phaseKeep(phaseKeep && etw && !noJoin)
    , m_extraLagMs(extraLagMs)
    , m_phaseKeepRequested(phaseKeep)
    , m_subGen(subGen)
    , m_diffMap(diffMap)
    , m_vsyncPresent(vsyncPresent)
    , m_targetFramerate(framerate)
    , m_srcRateHint(srcRateHint)
    , m_device(NULL)
{
    m_baseQpc.QuadPart = 0;
}

TemporalCaptureMode::~TemporalCaptureMode() {
    // Stop the ETW session before anything else: its callback touches this object, and a
    // consumer thread outliving the object it writes into is a crash on shutdown.
    if (m_etw) {
        m_etwConsumer.LogSummary();
        m_etwConsumer.Stop();
    }
    delete m_compositor;
}

LONGLONG TemporalCaptureMode::LagForSourcePeriod(LONGLONG srcPeriodQpc) const {
    LONGLONG lag = srcPeriodQpc + srcPeriodQpc / 4;
    if (lag < m_scheduler.PeriodQpc()) lag = m_scheduler.PeriodQpc();
    return lag;
}

UINT TemporalCaptureMode::GetPresentationInterval() const {
    // vsync present needs the device created with INTERVAL_ONE so PresentEx blocks on vsync.
    // NOTE: windowed INTERVAL_ONE blocks on DWM's compose clock, whose identity is
    // regime-dependent: composed desktop → primary/source display; fullscreen game on the
    // source → DWM composes only the card's display and the present is card-locked 60 Hz
    // (the production case). See spec Rounds 5-10.
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

bool TemporalCaptureMode::Setup() {
    m_device = g_pD3D9Device;

    // Before the ring starts: the instrument allocates its readback resources in Start.
    if (m_fgPhase) {
        m_ring.EnableFgPhase();
    }
    if (m_diffMap) {
        m_ring.EnableDiffMap();
    }
    if (m_phaseKeep) {
        m_ring.EnablePhaseKeep(this);
    }
    if (!m_ring.Setup(m_device, BUF_WIDTH, BUF_HEIGHT)) {
        return false;
    }
    if (!m_scheduler.Setup(m_targetFramerate)) {
        return false;
    }
    // Marker resources live on the PRESENT device (the burn is a backbuffer overlay,
    // never a ring-surface write). A failed Init disables the marker, not the relay.
    if (m_mark) {
        m_marker.Init(m_device, BUF_WIDTH, BUF_HEIGHT, m_markFrames);
    }
    // STATIC BRACKETING LAG: max(present period, 1.25 x assumed source period). The lag
    // exists so that a frame newer than the target has already arrived at pick time; the
    // worst-case wait is one source period, and 1.25x covers arrival jitter on top of it.
    // The assumption defaults to 60 fps (the slowest source served without configuration);
    // -src overrides it in either direction: slower sources need more lag, faster ones can
    // ride the present-period floor. Computed once and never moved: the lag is output
    // latency, and only a constant can be compensated for downstream (T10). The measured
    // source period is not fed back into the lag; it only audits the assumption (telemetry).
    const float assumedFps = (m_srcRateHint > 0.0f) ? m_srcRateHint : kDefaultAssumedSrcFps;
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

    LOG("Temporal mode initialized - %s present (%.2f fps nominal), nearest-frame selection + hysteresis",
        m_vsyncPresent ? "vsync/vblank" : "QPC-timer", m_targetFramerate);
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
    // remains - the same slip an unlocked beat already pays. Anchored ONLY to an explicit
    // -src: anchoring the default-60 fallback would manufacture false locks on undeclared
    // sources. The denominator scan is capped: past M=8 the comb spacing approaches arrival
    // jitter, the stability gate cannot close, and the lock refuses - correct for
    // effectively-irrational ratios. Latency: the pull adds a bounded slow sawtooth
    // (<= one comb spacing peak-to-peak, drift-rate ramp, one discrete step per beat),
    // accepted as a documented trade alongside the static lag (spec clause 4).
    m_policyCfg.phasePullSlewQpc = m_scheduler.Freq() / 40000;   // 25 us per present
    // Twice the declared source period: wide enough that ordinary jitter and a single
    // dropped frame stay under it, narrow enough that a frozen source (whose grab-timeout
    // re-grabs land ~100 ms apart) reads as stalled on the first present.
    m_policyCfg.stallSpanQpc = m_assumedSrcPeriodQpc * 2;
    // Under phase-aware keep-real the valid-frame cadence is ONE per source period (that is
    // the point), so a single missing real frame opens a bracket span of exactly two source
    // periods - the 2x threshold above with one microsecond of margin, measured. 2.5x keeps
    // one missing frame from reading as a stall while a genuinely frozen source (grab
    // timeouts at ~100 ms) still trips it on the first present.
    if (m_phaseKeep) m_policyCfg.stallSpanQpc = m_assumedSrcPeriodQpc * 5 / 2;
    if (m_lock && m_srcRateHint > 0.0f) {
        int combM = 1;
        bool combMatched = false;
        const double ratio = (double)m_srcRateHint / (double)m_targetFramerate;
        for (int m = 1; m <= 8; m++) {
            const double nm = ratio * (double)m;
            const long long n = (long long)(nm + 0.5);
            const double frac = nm - (double)n;
            if (n >= 1 && frac > -0.02 && frac < 0.02) { combM = m; combMatched = true; break; }
        }
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
        const LONGLONG sinkPeriodQpc = g_targetRefreshHz > 0
                                           ? m_scheduler.Freq() / g_targetRefreshHz
                                           : m_scheduler.PeriodQpc();
        m_policyCfg.srcPeriodQpc =
            policy::ToothGuardPeriod(m_assumedSrcPeriodQpc, sinkPeriodQpc, /*combOn=*/true);
    } else {
        LOG("Phase comb lock off (%s); target rides the static lag alone",
            !m_lock ? "-lock not set" : "-lock set but no -src to derive the comb");
    }

    // COMPOSITOR: nearest keeps the validated selection path; the synthesizing
    // compositors (blend, interp) pass a real frame through sharp whenever one sits
    // within the passthrough threshold of the target and synthesize at the bracket
    // weight otherwise. The threshold floors at the present period so an oversampling
    // source (whose frames are always within half a source period of any target)
    // passes through free; at-rate and slower sources get a quarter source period,
    // far above the locked operating point and far below the mid-gap distance of a
    // hole, so the gate cannot chatter.
    if (m_compositorKind != kCompositorNearest) {
        const LONGLONG thresholdBase =
            (m_assumedSrcPeriodQpc > m_scheduler.PeriodQpc()) ? m_assumedSrcPeriodQpc
                                                              : m_scheduler.PeriodQpc();
        m_policyCfg.passthroughQpc = thresholdBase / 4;
        if (m_compositorKind == kCompositorInterp) {
            m_synth = new InterpCompositor(&m_policyCfg);
            LOG("Interp compositor ACTIVE (o mode): passthrough threshold %lld us; op=/bw=/pt= on the temporal line",
                m_policyCfg.passthroughQpc * 1000000 / m_scheduler.Freq());
        } else {
            m_synth = new BlendCompositor(&m_policyCfg, m_tint);
            LOG("Blend compositor ACTIVE (b mode): passthrough threshold %lld us; op=/bw= on the temporal line",
                m_policyCfg.passthroughQpc * 1000000 / m_scheduler.Freq());
        }
        if (m_policyCfg.srcPeriodQpc > 0) {
            LOG("Composite tooth guard ACTIVE: synthesis must advance a full source period "
                "(%lld us teeth); op=hold-comb between teeth",
                m_policyCfg.srcPeriodQpc * 1000000 / m_scheduler.Freq());
        } else {
            // Stated rather than left as a missing line: a run that quietly lost the guard
            // reads as a parity-lottery blend storm with no explanation in the log.
            LOG("Composite tooth guard off (%s); mid-tooth targets synthesize",
                !m_lock ? "needs -lock" :
                m_srcRateHint <= 0.0f ? "needs -src" :
                "source is slower than the sink, so synthesis is rate conversion");
        }
        // Armed before Setup, which is where the content check allocates its readback
        // resources and where it disarms itself if they cannot be created.
        if (m_subGen) {
            m_synth->EnableGeneratedSubstitution(true);
            // The ring decides WHICH retracted members are substitutable, from the delivery
            // structure: it needs the declared source period to tell x2 from x3 and from a
            // source that pairs nothing.
            m_ring.EnableGeneratedSubstitution();
            LOG("Generated-frame substitution ACTIVE (-subgen): a retracted generated frame "
                "within the passthrough threshold replaces the blend, in the x2 regime only; "
                "op=pass-gen");
        }
        m_compositor = m_synth;
    } else {
        if (m_subGen) {
            LOG("-subgen ignored: nearest mode never blends, so there is nothing to replace");
        }
        m_compositor = new NearestCompositor(&m_policyCfg);
    }
    if (!m_compositor->Setup(m_device, BUF_WIDTH, BUF_HEIGHT)) {
        LOGERR("Compositor setup failed - refusing the mode");
        return false;
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
        LOGERR("-dejit REFUSED: needs the comb lock (-lock with -src) for its calm gate; "
               "running without delivery-lateness correction");
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
    return true;
}

void TemporalCaptureMode::Run(
    NvFBCToDx9Vid* nvfbcDx9,
    NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams,
    IDirect3DDevice9Ex* device,
    HWND hwnd)
{
    QueryPerformanceCounter(&m_baseQpc);
    // Every time in this log (arr=, dl=, tgt=, before=, after=) is microseconds since this
    // origin, not absolute QPC. Anything correlating the log against another QPC-stamped
    // source - an ETW trace, a second process - needs the origin to convert, and recovering
    // it by cross-correlating event sequences is guesswork this one line removes.
    LOG("QPC origin %lld ticks, frequency %lld Hz (log times are us since the origin)",
        m_baseQpc.QuadPart, m_scheduler.Freq());
    // Started here rather than in Setup because it needs that origin. A failure never stops
    // the capture, but it does interrupt: the console is closed by the time this runs, so a
    // logged-and-ignored failure is invisible until the capture is already spent. Asking for
    // -etw and silently getting a normal capture has cost a session once already.
    if (m_etw && !m_etwConsumer.Start(m_scheduler.Freq(), m_baseQpc.QuadPart)) {
        MessageBoxA(NULL,
                    "-etw was requested but the ETW session did not start.\n\n"
                    "Flip timing will NOT be recorded. The capture will otherwise run "
                    "normally.\n\nSee NvFBCR.log for the reason.",
                    "NvFBCR: ETW flip capture unavailable",
                    MB_OK | MB_ICONWARNING | MB_SETFOREGROUND | MB_TOPMOST);
    }
    const double usPerTick = 1000000.0 / (double)m_scheduler.Freq();
    const long long lagUs = (long long)(m_bracketingDelayQpc * usPerTick);

    // Note: Start releases nvfbcDx9 (the session bound to the present device) and rebinds
    // NvFBC to the ring's private capture device. nvfbcDx9 must not be used after this call.
    if (!m_ring.Start(nvfbcDx9, grabParams, m_baseQpc, hwnd)) {
        return;
    }

    // Capture-side compositor resources (the interp sidecar opens ring slot shared
    // handles, which exist only now). A compositor that cannot finish initializing
    // refuses the mode instead of silently running a different one.
    if (!m_compositor->OnCaptureStarted(&m_ring, m_baseQpc, m_scheduler.Freq())) {
        LOGERR("Compositor capture-side init failed - refusing the mode");
        m_ring.Stop();
        return;
    }

    MSG msg = {};
    LONGLONG lastPresentQpc = 0;
    long long presentFailures = 0;
    long long backbufferFailures = 0;
    // Present statistics live on the swapchain, not the device, and are only meaningful under
    // flip mode. Acquired once: the swapchain object is stable even though its buffers rotate.
    IDirect3DSwapChain9Ex* presentStatsSwapChain = NULL;
    UINT lastSyncRefresh = 0;
    long long missedRefreshes = 0, statsSamples = 0;
    if (g_flipEx) {
        IDirect3DSwapChain9* sc = NULL;
        if (SUCCEEDED(device->GetSwapChain(0, &sc)) && sc) {
            if (FAILED(sc->QueryInterface(__uuidof(IDirect3DSwapChain9Ex),
                                          (void**)&presentStatsSwapChain))) {
                presentStatsSwapChain = NULL;
            }
            sc->Release();
        }
        LOG("Flip-mode presentation ACTIVE (-flipex): FLIPEX swap effect, back buffer acquired "
            "per present; present statistics %s",
            presentStatsSwapChain ? "available (presentstats: lines follow)" : "UNAVAILABLE");
    }
    m_scheduler.Seed();

    while (TRUE)
    {
        // Present timing. Timer mode waits on the absolute-QPC deadline. Vsync mode lets the
        // INTERVAL_ONE present (below) be the wait, and anchors the target to "now" (just after
        // the previous compose tick) so selection runs on DWM's compose clock rather than QPC.
        // That clock is regime-dependent: composed desktop → primary/source display; fullscreen
        // game on the source → card-locked 60 Hz (spec Rounds 5-10).
        LONGLONG deadline;
        if (m_vsyncPresent) {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            deadline = now.QuadPart;
        } else {
            m_scheduler.WaitUntilDeadline();
            deadline = m_scheduler.Deadline();
        }
        // Comb lock applies the pull as extra lag; zero when disabled or disengaged. The
        // pull was computed from LAST present's bracket (closed loop, one-present latency
        // in the control path - negligible at 25 us/present slew).
        const LONGLONG target = deadline - (m_bracketingDelayQpc + m_lockState.pullQpc);

        // Stage 6: settle delivery-lateness corrections BEFORE the bracket reads the ring.
        // The walk consumes the ring's batch-start history (never slot fields, which the
        // capture thread owns), measures each batch against the flip grid, and inserts
        // accepted corrections into the overlay FindBracket reads through. The fence (the
        // newest target ever consumed) is the coherence rule - once a correction would
        // land at or behind it, the batch keeps its late stamp forever - and lock calm
        // keeps stamps still while the phase lock is riding out or converging from a
        // stall. Every verdict is counted; late-but-blocked is logged, because a capture
        // where corrections were measured and discarded must not read as one with no
        // late deliveries.
        if (m_dejitter) {
            if (target > m_maxTargetQpc) m_maxTargetQpc = target;
            const bool lockCalm =
                (m_lockState.stallRun == 0 && m_lockState.recoverRun == 0);
            const long long opens = m_ring.BatchOpens();
            if (opens - m_nextBatch > CaptureRing::kBatchHistory - 8) {
                // The walk was pinned (ETW outage, long stall) and the history lapped.
                // Skip forward and drop the chain: its stride does not span the gap.
                m_dejitSkipped += (opens - 8) - m_nextBatch;
                m_nextBatch = opens - 8;
                m_anchorChain = policy::AnchorChain();
            }
            while (m_nextBatch < opens) {
                const LONGLONG bs = m_ring.BatchStartAt(m_nextBatch);
                const policy::LateCorrection lc = m_etwConsumer.MeasureLateness(
                    0, bs, m_anchorChain, m_flipCadenceWindowQpc);
                // Flip data still in flight: retry THIS batch next present (the chain is
                // sequential, so later batches wait behind it).
                if (lc.dataPending) break;
                m_dejitMeasured++;
                if (lc.correctionTicks != 0) {
                    m_dejitLate++;
                    const char* verdict;
                    if (!lockCalm) {
                        m_dejitLockDeclined++;
                        verdict = "lock-declined";
                    } else if (bs <= m_maxTargetQpc ||
                               bs - lc.correctionTicks <= m_maxTargetQpc) {
                        m_dejitFenceBlocked++;
                        verdict = "fence-blocked";
                    } else {
                        m_overlay.Insert(bs, lc.correctionTicks);
                        m_dejitCorrected++;
                        verdict = "corrected";
                    }
                    LOG("dejit: batch arr=%lldus late by %lldus, %s",
                        (long long)((bs - m_baseQpc.QuadPart) * usPerTick),
                        (long long)(lc.correctionTicks * usPerTick), verdict);
                }
                m_nextBatch++;
            }
        }

        FrameBracket bracket;
        m_ring.FindBracket(target, m_dejitter ? &m_overlay : NULL, &bracket);

        // Update the comb-lock pull for the next present. Skipped when the bracket is
        // incomplete (startup, stalls): the pull freezes rather than integrating on a
        // one-sided error, and the frozen value stays bounded by construction.
        if (m_policyCfg.combQpc > 0) {
            const bool resumedFromStall =
                policy::UpdateStallRun(m_lockState, m_policyCfg, bracket.info);
            if (!policy::BracketIsStalled(bracket.info, m_policyCfg)) {
                policy::UpdatePhaseLock(m_lockState, m_policyCfg, bracket.info.beforeDiff,
                                        resumedFromStall);
            }
        }

        // The back buffer is acquired PER PRESENT, never cached. Under D3DSWAPEFFECT_DISCARD
        // back buffer 0 is the same surface every time and this is a no-op, but under FLIPEX
        // the runtime rotates which handle is the back buffer at presentation time, so a
        // cached pointer composites into a surface that is no longer the one being presented -
        // the "every third frame is blank" that made the earlier FLIPEX attempt fail. Falls
        // back to the cached global if the call fails, so a failure degrades rather than
        // presenting whatever the flip queue left behind.
        IDirect3DSurface9* backbuffer = NULL;
        if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backbuffer)) ||
            !backbuffer) {
            backbuffer = NULL;
            backbufferFailures++;
            if (backbufferFailures == 1 || (backbufferFailures % 600) == 0) {
                LOGERR("GetBackBuffer failed (%lld so far); falling back to the cached surface",
                       backbufferFailures);
            }
        }
        IDirect3DSurface9* const target = backbuffer ? backbuffer : g_backbuffer;

        // The DECISION is pure policy (selection or composite, in TemporalPolicy.cpp
        // with the mechanism rationale); the compositor executes it onto the
        // backbuffer. This loop owns the timing, the present, and the log.
        CompositeOutcome outcome;
        m_compositor->Compose(bracket, target, &outcome);

        // Burn the marker over the composed backbuffer, once per present (repeats
        // included: the counter identifies presented frames, not source frames).
        // Before the present stamp, so jit/pdt absorb its cost and a -mark on/off
        // A/B measures it.
        long long markN = -1;
        if (m_mark) {
            markN = (long long)m_marker.Burn(target, outcome.pickCode, outcome.weightQ,
                                             outcome.synthesized, m_compositor->Id(),
                                             outcome.pixelExec);
        }

        LARGE_INTEGER beforePresent;
        QueryPerformanceCounter(&beforePresent);
        // Timer: immediate (non-blocking). Vsync: the device was CREATED with INTERVAL_ONE
        // (GetPresentationInterval), so this present blocks until DWM's next compose (source
        // clock on a composed desktop; card clock under a fullscreen game) — that is the
        // frame-pacing wait in vsync mode.
        //
        // dwFlags is 0 and must stay 0. It is NOT a presentation interval: the only legal
        // values are D3DPRESENT_DONOTWAIT and D3DPRESENT_LINEAR_CONTENT, and the interval is
        // fixed at device creation. This argument used to receive the interval constants,
        // which meant vsync mode silently requested DONOTWAIT (numerically identical to
        // INTERVAL_ONE, both 1) and timer mode passed an undefined bit. Under DONOTWAIT a
        // present that would wait returns D3DERR_WASSTILLDRAWING WITHOUT PRESENTING, which is
        // invisible in the log (the present was counted) and shows downstream as the previous
        // frame repeating - the exact judder signature this relay is measured against.
        const HRESULT presentHr = device->PresentEx(NULL, NULL, NULL, NULL, 0);
        // GetBackBuffer AddRefs; release after the present so the runtime can rotate it.
        if (backbuffer) backbuffer->Release();
        if (FAILED(presentHr) || presentHr == S_PRESENT_MODE_CHANGED ||
            presentHr == S_PRESENT_OCCLUDED) {
            // Never silent: a present that did not reach the screen must be attributable, or
            // a video-vs-log disagreement has no explanation in the log.
            presentFailures++;
            if (presentFailures == 1 || (presentFailures % 600) == 0) {
                LOGERR("present returned 0x%08lx (%lld so far): the frame may not have reached "
                       "the screen", (unsigned long)presentHr, presentFailures);
            }
        }

        // PRESENT STATISTICS: what the SINK actually did with our frames, from the runtime
        // rather than inferred from ETW. Only a flip-mode swapchain reports these in windowed
        // mode - a bitblt one returns zeroes - so this is the half of -flipex that pays off
        // whether or not DWM ever promotes the window to independent flip.
        //
        // PresentRefreshCount equals SyncRefreshCount when every present landed on its own
        // vsync; when the former runs ahead, a refresh went by showing the previous frame,
        // which is a DOWNSTREAM DUPE measured in-process. That is the same quantity a marked
        // video plus a marker decode plus content-step analysis currently produces offline.
        if (g_flipEx && presentStatsSwapChain) {
            D3DPRESENTSTATS ps;
            ZeroMemory(&ps, sizeof(ps));
            if (SUCCEEDED(presentStatsSwapChain->GetPresentStatistics(&ps))) {
                if (lastSyncRefresh != 0 && ps.SyncRefreshCount > lastSyncRefresh) {
                    const UINT elapsed = ps.SyncRefreshCount - lastSyncRefresh;
                    // More than one sink refresh since the last present means refreshes that
                    // showed no new frame of ours.
                    if (elapsed > 1) missedRefreshes += (elapsed - 1);
                }
                lastSyncRefresh = ps.SyncRefreshCount;
                statsSamples++;
                if ((statsSamples % 1800) == 0) {
                    LOG("presentstats: present=%u presentRefresh=%u syncRefresh=%u "
                        "syncQpc=%lldus missedRefreshes=%lld over %lld presents",
                        ps.PresentCount, ps.PresentRefreshCount, ps.SyncRefreshCount,
                        (long long)((ps.SyncQPCTime.QuadPart - m_baseQpc.QuadPart) * usPerTick),
                        missedRefreshes, statsSamples);
                }
            }
        }

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
                lkField = m_lockState.engaged ? 1 : 0;
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
            LOG("temporal dl=%lldus tgt=%lldus before=%lldus(d%d) after=%lldus w=%.3f pick=%s jit=%lldus pdt=%lldus lag=%lldus pull=%lldus lk=%d mark=%lld%s%s",
                (long long)((deadline - m_baseQpc.QuadPart) * usPerTick),
                (long long)((target - m_baseQpc.QuadPart) * usPerTick),
                (long long)((bracket.info.beforeTs - m_baseQpc.QuadPart) * usPerTick), bracket.beforeDepth,
                bracket.info.hasAfter ? (long long)((bracket.info.afterTs - m_baseQpc.QuadPart) * usPerTick) : -1LL,
                bracket.weight, outcome.pickLabel,
                (long long)((beforePresent.QuadPart - deadline) * usPerTick),
                (long long)(presentDelta * usPerTick),
                lagUs,
                (long long)(m_lockState.pullQpc * usPerTick),
                lkField,
                markN,
                opFields,
                flipFields);
            if (!bracket.info.hasAfter) {
                LOG("temporal: no after-frame (source slower than present?) - repeating newest");
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

        if (!m_vsyncPresent) m_scheduler.Advance();   // vsync paces via the blocking present

        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if (msg.message == WM_QUIT) break;
        if (m_ring.HasStopped()) break;  // capture thread hit a fatal error
    }

    if (presentStatsSwapChain) {
        // The whole-run figure, so a capture carries its downstream dupe count without a
        // video: refreshes that showed no new frame of ours, over presents that reported.
        LOG("presentstats summary: %lld refreshes showed no new frame over %lld presents "
            "(%.3f/s at 60 Hz sink)", missedRefreshes, statsSamples,
            statsSamples > 0 ? (double)missedRefreshes * 60.0 / (double)statsSamples : 0.0);
        presentStatsSwapChain->Release();
        presentStatsSwapChain = NULL;
    }
    if (backbufferFailures > 0) {
        LOGERR("GetBackBuffer failed %lld times over the run", backbufferFailures);
    }
    if (presentFailures > 0) {
        LOGERR("present reported a non-OK status %lld times over the run", presentFailures);
    }

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
    if (m_subGen && m_synth) {
        // The population this feature addresses, measured in the field rather than inferred
        // from a replay. offered counts the presents where PLACEMENT said yes, so
        // offered - substituted - rejected is what the policy's own rules refused.
        const SynthCompositorBase::GenSubStats& g = m_synth->GeneratedSubstitutionStats();
        LOG("subgen summary: %lld substituted, %lld offered, %lld refused by the change map, "
            "%lld rejected on content, content check %lld us worst / %lld us mean",
            g.substituted, g.offered, m_ring.GeneratedDuplicatesRefused(), g.rejectedContent,
            g.checkUsMax, g.offered ? g.checkUsTotal / g.offered : 0);
    }
    m_ring.Stop();
}

const char* TemporalCaptureMode::GetModeName() const {
    if (m_compositorKind == kCompositorInterp) return "Temporal interp";
    if (m_compositorKind == kCompositorBlend) return "Temporal blend";
    return "Temporal";
}
