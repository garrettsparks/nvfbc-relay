#include "TemporalCaptureMode.h"
#include "FrameCompositors.h"
#include "D3D9Present.h"
#include "D3D11Present.h"
#include <SimpleLogger.h>
#include <cstdio>

// External global variables
extern IDirect3DDevice9Ex* g_pD3D9Device;
extern int BUF_WIDTH;
extern int BUF_HEIGHT;
extern int g_targetRefreshHz;

// Presents between estimator-vs-assumption audits (about 10 s at 60 Hz): rare enough to keep
// the log quiet, frequent enough that a wrong -src is caught within the first minute.
static const int kTelemetryPeriodPresents = 600;

TemporalCaptureMode::TemporalCaptureMode(float framerate, bool vsyncPresent, float srcRateHint, bool lock,
                                         CompositorKind compositor, bool mark, unsigned int markFrames,
                                         bool tint, bool etw, bool noJoin, bool dejitter,
                                         bool fgPhase, bool phaseKeep, unsigned int extraLagMs,
                                         bool d3d11Present)
    : m_bracketingDelayQpc(0)
    , m_assumedSrcPeriodQpc(0)
    , m_present(NULL)
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
    , m_phaseKeepRequested(phaseKeep)
{
    m_baseQpc.QuadPart = 0;

    // The one place that knows there are two present paths. The D3D11 backend IS the blend
    // pipeline and brings its own compositor; the D3D9 path hosts whichever IFrameCompositor
    // the mode letter selected. Both take &m_policyCfg, which Setup fills in before either
    // reads it. Nothing touches a device here: the paths create their resources in Setup,
    // after the ring has started.
    if (d3d11Present) {
        m_present = new D3D11PresentBackend(tint);
    } else {
        IFrameCompositor* c;
        if (compositor == kCompositorInterp) {
            c = new InterpCompositor(&m_policyCfg);
        } else if (compositor == kCompositorBlend) {
            c = new BlendCompositor(&m_policyCfg, tint);
        } else {
            c = new NearestCompositor(&m_policyCfg);
        }
        m_present = new D3D9PresentPath(c);
    }
    const char* kindName = "Temporal";
    if (compositor == kCompositorInterp) kindName = "Temporal interp";
    else if (compositor == kCompositorBlend) kindName = "Temporal blend";
    snprintf(m_modeName, sizeof(m_modeName), "%s (%s)", kindName, m_present->Name());
}

TemporalCaptureMode::~TemporalCaptureMode() {
    // Stop the ETW session before anything else: its callback touches this object, and a
    // consumer thread outliving the object it writes into is a crash on shutdown.
    if (m_etw) {
        m_etwConsumer.LogSummary();
        m_etwConsumer.Stop();
    }
    delete m_present;
}

bool TemporalCaptureMode::PresentsViaD3D11() const {
    return m_present->OwnsOutputWindow();
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

bool TemporalCaptureMode::Setup() {
    m_device = g_pD3D9Device;

    // Before the ring starts: the instrument allocates its readback resources in Start.
    if (m_fgPhase) {
        m_ring.EnableFgPhase();
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
            interp ? "op=/bw=/pt=" : "op=/bw=");
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
    // The capture device joins the present device on whatever window main created it on:
    // the output window when the D3D9 swapchain presents there, a hidden host window when
    // the output window belongs to another API's swapchain (flip model allows one swapchain
    // per window and no second API on it).
    HWND deviceWnd = hwnd;
    D3DDEVICE_CREATION_PARAMETERS creation;
    ZeroMemory(&creation, sizeof(creation));
    if (SUCCEEDED(device->GetCreationParameters(&creation)) && creation.hFocusWindow) {
        deviceWnd = creation.hFocusWindow;
    }
    if (!m_ring.Start(nvfbcDx9, grabParams, m_baseQpc, deviceWnd)) {
        return;
    }

    // The present path finishes initializing now that the ring's slot shared handles exist:
    // the D3D9 compositor and its capture-side aliases, or the whole D3D11 backend. Either
    // refuses the mode when it cannot finish instead of silently running a different one,
    // and it refuses LOUDLY: the console is closed by now, so a logged refusal alone would
    // read as the relay vanishing.
    if (!m_present->Setup(device, hwnd, &m_ring, BUF_WIDTH, BUF_HEIGHT, &m_policyCfg, m_mark,
                          m_markFrames, m_baseQpc, m_scheduler.Freq())) {
        LOGERR("%s init failed - refusing the mode", m_present->Name());
        m_ring.Stop();
        const char* advice = m_present->RefusalAdvice();
        char text[640];
        snprintf(text, sizeof(text),
                 "The %s could not be initialized.\n\n"
                 "The relay will NOT start. It deliberately does not fall back to another "
                 "present path: a run labelled one way that silently presented another would "
                 "be worse than no run.\n\nSee NvFBCR.log for the reason.%s%s",
                 m_present->Name(), advice[0] ? "\n\n" : "", advice);
        MessageBoxA(NULL, text, "NvFBCR: present path unavailable",
                    MB_OK | MB_ICONERROR | MB_SETFOREGROUND | MB_TOPMOST);
        return;
    }

    MSG msg = {};
    LONGLONG lastPresentQpc = 0;
    m_scheduler.Seed();

    while (TRUE)
    {
        // Present timing. Timer mode waits on the absolute-QPC deadline. Vsync mode lets the
        // present path's own blocking wait be the pacing, and anchors the target to "now"
        // (just after the previous frame was consumed) so selection runs on that path's
        // clock rather than QPC: DWM's compose tick on the D3D9 swapchain (regime-dependent:
        // the source display's rate on a composed desktop, card-locked 60 Hz under a
        // fullscreen game on the source), the sink's vblank on the flip-model swapchain.
        LONGLONG deadline;
        // How long this present's pacing wait blocked (blk= on the line). A vsync present
        // that is doing its job spends nearly the whole period waiting; one that returns at
        // once is not pacing anything, and pdt alone cannot tell those apart. A path that
        // paces BEFORE the decision blocks in WaitForFrame and is measured here; one that
        // paces in the present itself returns at once here and reports the block from
        // Present, so the sum is the whole wait either way.
        LONGLONG blockTicks = 0;
        if (m_vsyncPresent) {
            LARGE_INTEGER waitStart, now;
            QueryPerformanceCounter(&waitStart);
            m_present->WaitForFrame();
            QueryPerformanceCounter(&now);
            blockTicks = now.QuadPart - waitStart.QuadPart;
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

        // The DECISION is pure policy (selection or composite, in TemporalPolicy.cpp with
        // the mechanism rationale); the present path executes it onto its own target,
        // burns the marker over it (once per present, repeats included: the counter
        // identifies presented frames, not source frames; before the present stamp so
        // jit/pdt absorb its cost) and shows it. This loop owns the timing, the bracket,
        // the lock and the log.
        CompositeOutcome outcome;
        m_present->Compose(bracket, &outcome);
        const long long markN = m_present->BurnMarker(outcome);
        LARGE_INTEGER beforePresent;
        QueryPerformanceCounter(&beforePresent);
        blockTicks += m_present->Present(m_vsyncPresent);

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
            // blk= goes LAST, after the appended op/flip fields. The offline parsers match this
            // line as a contiguous chain of named fields, so a field inserted between two
            // existing ones silently drops every field after it rather than failing.
            LOG("temporal dl=%lldus tgt=%lldus before=%lldus(d%d) after=%lldus w=%.3f pick=%s jit=%lldus pdt=%lldus lag=%lldus pull=%lldus lk=%d mark=%lld%s%s blk=%lldus",
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
                flipFields,
                (long long)(blockTicks * usPerTick));
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

        if (!m_vsyncPresent) m_scheduler.Advance();   // vsync paces via the blocking present

        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if (msg.message == WM_QUIT) break;
        if (m_ring.HasStopped()) break;  // capture thread hit a fatal error
        if (m_present->SwapChainStalled()) {
            // The swapchain stopped retiring frames and does not come back. Stopping is the
            // only correct move: nothing reaches the screen either way, and a loop left
            // turning here holds the NvFBC session against the next run while its output
            // window may not even be somewhere a person can close it.
            LOGERR("%s: swapchain stalled - no paced present for seconds. Stopping the "
                   "capture so the session is released and this run ends attributably.",
                   m_present->Name());
            break;
        }
    }

    m_present->LogSummary();

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
}

const char* TemporalCaptureMode::GetModeName() const {
    return m_modeName;
}
