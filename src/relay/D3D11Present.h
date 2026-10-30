#pragma once

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <utility>
#include <vector>

#include "IPresentPath.h"
#include "BlendShaders.h"
#include "FlowPlan.h"
#include "FrameMarker.h"
#include "OpticalFlowWarp.h"
#include "WgcScaler.h"

class CopyEngineTarget;

// D3D11 PRESENT BACKEND: decides, composites and presents straight onto a DXGI flip-model
// swapchain, the present path that replaces the D3D9 compose-and-PresentEx one.
//
// WHY THIS EXISTS. The D3D9 present is a windowed INTERVAL_ONE, which does not wait on any
// monitor's vblank: it throttles on DWM's compose clock, whose rate follows whatever the
// SOURCE display is doing (the base rate under a fullscreen game, the displayed rate when
// that game runs in-game frame generation, the desktop rate with no game). The relay
// therefore inherits a present cadence, and a present jitter, that belong to another
// display entirely, and the sink samples one, two or three of those presents per scan.
//
// A flip-model swapchain whose window covers an output can be promoted by Windows to
// INDEPENDENT FLIP, where DWM leaves the present path and the display controller scans
// these buffers directly. The swapchain's frame-latency waitable object then signals on the
// SINK's vblank: one present per sink scan, phase-locked by construction, on every title
// regardless of source regime. That is the goal. It is NOT exclusive fullscreen: nothing here
// calls SetFullscreenState, and failing to be promoted degrades to being composed by DWM,
// which is what the relay does today.
//
// THE WAIT COMES BEFORE THE DECISION, not inside Present after the draw. A present that
// blocks after rendering settles into waiting almost a full extra frame between drawing and
// display; waiting on the swapchain's own object first, then deciding, drawing and queueing
// immediately, decides each frame as late as possible and shows it one sink period later
// rather than two.
//
// WHAT IS CERTAIN EVEN WITHOUT PROMOTION: flip model shares buffers with DWM instead of
// copying them into a redirection surface (one full-frame read+write removed per present),
// and GetFrameStatistics reports real data in windowed mode where a bitblt swapchain returns
// zeroes, giving an in-process missed-refresh counter: downstream dupes measurable from the
// log without a marked video and an offline decode.
//
// FEEDS OFF THE EXISTING RING. Ring slots are already shared render targets whose handles
// CaptureRing hands out for opening the same texture on another API's device, and
// FrameBracket already carries beforeSlot/afterSlot so a cross-API consumer can pick its
// own per-device aliases. Nothing about capture changes. The capture device drains an
// event query before publishing a slot, so a published slot is GPU-complete for this device
// exactly as it is for the D3D9 present device.
//
// DRAWS THE STEP'S DECISION. The composite decision is the policy step's, on the mode's one
// state; this backend draws it. The flow planner flows the pairs of the next presents the
// step, run ahead on a copy of that state, says will synthesize (PlanAhead).
//
// NEAREST, BLEND OR OPTICAL FLOW. This backend is the nearest selection (t), the blend pipeline
// (b) or, with interp, the optical flow interpolation (o). The nearest selection draws the one
// real frame the step picked, or the last one shown again for a repeat, exactly as the D3D9
// NearestCompositor copied it. The blend and the flow share the decision, holds and passthrough,
// with a synthesized present drawn as the flow warp (OpticalFlowWarp) where the blend would draw
// the lerp, and the lerp in its place when there is no flow for it. Each pair a present is
// predicted to synthesize is flowed ahead (policy::FlowPlanner), handed to the engine right
// after a present that draws, where its turns on the game's graphics engine take the draw's
// with them.
//
// VBLANK OR TIMER. The loop's pacing (Pacing.h) waits on the output's vblank through
// WaitForFrame, or on a timer and then WaitForFrame for room; Present takes the sync interval
// that goes with it.
//
// WITH -wgc THE SWAPCHAIN IS A CopyEngineTarget (unless -nocopyengine): the same decision and
// draw, into its buffers, except that a present showing one WGC frame as it is (no blend, no
// scaling) is copied on the copy engine, its marker strip with it, and so takes no turn on the
// game's graphics engine. A MODE ON A TIMER presents through the same target whatever the
// capture, drawing where it does not copy: the flip-model swapchain's frame-latency wait held
// the loop until the card's vblank, so the timer decided nothing there (2026-10-29).
class D3D11PresentBackend : public IPresentPath {
public:
    // tint is -tint: a border over 1.5% of each edge of every frame whose pixels are not one real
    // frame, red for a blend (as the D3D9 blend draws it), green for a flow warp, blue for a hold
    // of either. interp
    // selects the optical flow, nearest the
    // nearest selection. blendShader is -blend's shader (BlendShaders::Shader), -1 for the
    // present's own blend. timer is a mode on a timer; copyEngine is -wgc with the copy engine
    // on. Either puts the present on the copy engine's composition swapchain (three buffers for
    // a timer, two otherwise); frames are copied there only with copyEngine, drawn otherwise.
    D3D11PresentBackend(bool tint, bool interp, int blendShader = -1, bool nearest = false,
                        bool timer = false, bool copyEngine = false);
    ~D3D11PresentBackend();

    D3D11PresentBackend(const D3D11PresentBackend&) = delete;
    D3D11PresentBackend& operator=(const D3D11PresentBackend&) = delete;

    // Call AFTER CaptureRing::Start (slot shared handles must exist). The swapchain goes on
    // ctx.outputWindow at ctx's output size; the D3D9 device is unused, since this path
    // brings its own. cfg is borrowed from the owning mode and must outlive the backend;
    // mark/markFrames arm the frame marker as FrameMarker::Init does. Failure is loud and
    // leaves the backend disabled so the caller can refuse the mode rather than run degraded.
    bool Setup(const RelayContext& ctx, CaptureRing* ring, const policy::PolicyConfig* cfg,
               bool mark, unsigned int markFrames, LARGE_INTEGER baseQpc,
               LONGLONG freqQpc) override;

    // With the optical flow: two presents ahead, the horizon the flow planner was built for.
    int LookaheadPresents() const override { return m_flow ? kFlowLookahead : 0; }
    void PlanAhead(const FrameBracket* upcoming, const LONGLONG* targets,
                   const bool* synthesizes, int count) override;

    // Draw the step's decision onto the current back buffer. Fills out exactly as the D3D9
    // synthesizing compositor would, so the temporal log line reads the same.
    void Compose(const FrameBracket& bracket, const policy::PresentStepResult& step,
                 CompositeOutcome* out) override;

    // Burn the frame marker over the composed back buffer. Returns the counter burned
    // (mark= on the temporal line), or -1 when the marker is off.
    long long BurnMarker(const CompositeOutcome& out) override;

    // THE FRAME-PACING WAIT. Blocks until the swapchain has room for the next frame, which
    // with a latency of one is the moment the previous frame was consumed: the sink's vblank
    // under independent flip, DWM's compose otherwise. Call at the top of every present,
    // before the decision. Returns false on the bounded timeout, which the caller treats as
    // a wait that did not pace anything.
    bool WaitForFrame() override;

    // True once the wait has timed out for long enough that the swapchain is not coming back.
    // The caller must stop the mode: nothing recovers from here, and a loop that keeps turning
    // presents nothing while holding the capture session against the next run.
    bool SwapChainStalled() const override;

    // Queue the drawn back buffer. Does not block: WaitForFrame already made room, so the
    // pacing block reported here is 0. The flip-model swapchain presents with sync interval 1,
    // on the next vblank; the composition swapchain always presents on a vblank.
    LONGLONG Present() override;

    // Whole-run present statistics; safe to call when disabled.
    void LogSummary() const override;

    const char* Name() const override { return m_name; }
    const char* RefusalAdvice() const override {
        if (m_flowFailed) {
            return "To use the blend instead, type b:vsync at the mode prompt.";
        }
        if (m_copyEngineFailed && m_timer) {
            return "To start with the older output method, add -d3d9 to the launch options.";
        }
        if (m_copyEngineFailed) {
            return "To start with the older output method, add -nocopyengine to the launch "
                   "options.";
        }
        return "To try the older output method, type b:dwm at the mode prompt.";
    }
    bool OwnsOutputWindow() const override { return true; }
    bool UsesComposition() const override { return m_composition; }

    bool Enabled() const { return m_enabled; }

private:
    bool CreateDeviceAndSwapChain(HWND hwnd, int width, int height, bool copyEngine,
                                  int sinkRefreshHz);
    bool OpenRingAliases(CaptureRing* ring);
    bool NeedsScaling(int slot) const;
    bool CreatePipeline();
    // Readies the -wgcscale filter; bicubic or Lanczos that cannot be readied falls back to
    // bilinear.
    bool SetupScaling(int filter, int width, int height);
    // Re-acquire back buffer 0 as the render target. Under flip model the buffer behind
    // index 0 changes after every Present, so this runs once per present. With the copy engine
    // the target is the copy-engine target's next buffer.
    bool AcquireBackBuffer();
    // With the copy engine: the WGC texture a present of slot as it is can be copied from, or
    // NULL when it has to be drawn.
    ID3D11Texture2D* CopySourceFor(int slot);
    // Draw lerp(a, b, w) over the whole back buffer. Passthrough binds the same slot to both
    // inputs with w = 0, so there is ONE code path and the swapchain format is free to differ
    // from the ring's (a CopyResource would demand they match exactly).
    bool Draw(int slotA, int slotB, float w);
    // The view to sample for a ring slot. Under -wgc a slot may name a texture of WGC's frame
    // pool on this device; one view is made per pool texture the first time it is seen.
    ID3D11ShaderResourceView* SourceFor(int slot);
    bool DrawMarker(const bool cells[FrameMarker::kCells]);
    // -tint: the border over the current target, drawn after whatever drew a synthesized frame
    // (the blend, a -blend shader, the scaler or the warp), so one pass serves all four. exec is
    // the executor code of the pixels; hold is a hold re-drawing a synthesized frame.
    void DrawTint(int exec, bool hold);
    void SampleStats();
    void SamplePresentationPath();
    // The optical flow's view of a ring slot: its stamp and the view to sample.
    OpticalFlowWarp::Frame FlowFrame(int slot, int64_t ts);
    // Plans the flows of the brackets PlanAhead kept that will synthesize.
    void PlanFlows();
    // Hands cache slot's planned pair to the engine. False leaves it planned.
    bool SubmitFlow(int cacheSlot);
    // After this present's draw (or in place of one): the planned flows that ride it, and every
    // one the next present needs whether or not anything was drawn. afterPresent: the copy
    // engine's place for them, behind the present, where every planned flow goes.
    void SubmitPlanned(bool drew, bool afterPresent = false);
    // A synthesized present's frame: the warp from the pair's flow when it was handed over at an
    // earlier present, or the lerp when it was not (it is handed over now, for later presents of
    // the pair) or there is none. Sets exec (3 warp, 1 lerp), the cache slot it read (-1 for the
    // lerp) and fl=.
    bool DrawSynthesis(const FrameBracket& bracket, float w, int* exec, int* flowSlot,
                       const char** flowLabel);

    // How long one frame-pacing wait may block. Far above any real vblank interval, far below
    // anything a user would notice, so the loop keeps turning even if the swapchain stops.
    static const DWORD kWaitTimeoutMs = 250;
    // Consecutive timeouts that mean the swapchain is gone rather than briefly busy. Five
    // seconds: healthy captures record zero timeouts end to end, so this cannot false-fire on
    // a mode change or an alt-tab, and it bounds how long a dead run can sit unnoticed.
    static const int kStallTimeouts = 5000 / kWaitTimeoutMs;

    // Marker compositor-ID cell: the nearest selection, the blend pipeline, or the optical flow,
    // as the D3D9 compositors number them.
    static const int kCompositorIdNearest = 0;
    static const int kCompositorIdBlend = 1;
    static const int kCompositorIdFlow = 3;
    // Synthesis executor codes: the lerp, matching the D3D9 blend compositor's, and the warp.
    static const int kExecBlend = 1;
    static const int kExecWarp = 3;
    // Presents ahead the optical flow plans for.
    static const int kFlowLookahead = 2;

    bool m_enabled;
    int m_width, m_height;
    int m_ringSlots;

    bool m_tintRequested;

    ID3D11Device* m_dev;
    ID3D11DeviceContext* m_ctx;
    IDXGISwapChain1* m_swapChain;
    // Reports the swapchain's composition mode, an overlay plane against a composition
    // surface. Related to whether the present path is promoted but not equivalent to it, so it
    // informs that question rather than settling it. Purely diagnostic: NULL leaves the backend
    // fully functional with only the mode unreported.
    IDXGISwapChainMedia* m_swapChainMedia;
    HANDLE m_frameWait;                 // the swapchain's frame-latency waitable object
    ID3D11RenderTargetView* m_rtv;      // the current back buffer; recreated per present

    ID3D11Texture2D* m_ringAlias[CaptureRing::RING_SIZE];
    ID3D11ShaderResourceView* m_ringSrv[CaptureRing::RING_SIZE];

    // -wgc: the ring was asked for WGC, so this device is offered to it, and once it has been
    // m_ring is set and slots are looked up there first (SourceFor).
    bool m_wgc = false;
    CaptureRing* m_ring = NULL;
    // The copy engine (-wgc without -nocopyengine): the target in place of the swapchain, the
    // WGC texture this present copies (NULL when it drew), and whether its setup failed, which
    // changes what the refusal tells the user.
    CopyEngineTarget* m_copyEngine = NULL;
    // A timer's present goes through that target whatever the capture, as does -wgc's with the
    // copy engine (m_composition, from the constructor); a frame is copied on the copy engine
    // only in the second case (m_copies). The name says which target and compositor this is.
    bool m_timer = false;
    bool m_copies = false;
    bool m_composition = false;
    char m_name[64] = "";
    ID3D11Texture2D* m_copySource = NULL;
    bool m_copyEngineLost = false;
    bool m_copyEngineFailed = false;
    // -wgcscale: the filter a WGC frame of another size than the output is scaled with
    // (WgcScaler::Filter) and its scaler, and the presents that scaled and that drew frames at
    // the output's size.
    int m_scale = WgcScaler::Bilinear;
    WgcScaler m_scaler;
    long long m_scaledDraws = 0;
    long long m_unscaledDraws = 0;
    std::vector<std::pair<ID3D11Texture2D*, ID3D11ShaderResourceView*>> m_poolSrvs;

    ID3D11VertexShader* m_vs;
    ID3D11PixelShader* m_ps;
    ID3D11PixelShader* m_markerPs;
    ID3D11PixelShader* m_tintPs = NULL;     // -tint's border colour; NULL without -tint
    ID3D11Buffer* m_cb;                 // one float4 for the lerp, one uint4 of marker cells
    ID3D11SamplerState* m_sampler;

    // What the last present drew, so a hold re-draws the same thing. Kept as inputs rather
    // than as a copied-out texture on purpose: snapshotting the output every present would
    // reintroduce exactly the full-frame copy this backend exists to remove. The slots are
    // still live at hold time because a hold is issued the present after its content was
    // drawn, and the ring keeps many presents of history.
    int m_lastSlotA, m_lastSlotB;
    float m_lastWeight;
    int m_lastExec;                     // executor code of the held pixels (0 = real)
    bool m_haveLast;
    // A held warp draws again from the same flow while its cache slot still holds that pair.
    int m_lastFlowSlot = -1;
    policy::FlowPair m_lastFlowPair;

    // -blend: the shader for a synthesized frame at the output's size, timed in pt=; NULL
    // without -blend, when the present draws the blend of its own.
    int m_blendShader = -1;
    BlendShaders* m_blend = NULL;

    // The nearest selection (t): one real frame per present, never a synthesized one.
    bool m_nearest = false;

    // The optical flow (o): NULL on the blend pipeline.
    bool m_interp = false;
    bool m_flowFailed = false;         // the flow engine could not be set up
    OpticalFlowWarp* m_flow = NULL;
    policy::FlowPlanner m_planner;
    // What each cache slot holds beyond the planner's pair: the ring slots its frames were in
    // when it was planned, whether it still waits to be handed over, and the present that needs
    // it first.
    struct PlannedFlow {
        policy::FlowPair pair;
        int beforeSlot = -1;
        int afterSlot = -1;
        bool pending = false;           // planned, waiting to be handed over
        bool handed = false;            // handed to the engine: the slot's fields are this pair's
        bool failed = false;            // the engine refused it, or a frame had no input
        long long needBy = 0;
    };
    PlannedFlow m_planned[policy::kFlowSlots];
    // The upcoming presents PlanAhead handed in, planned in Compose: each one's bracket, its
    // target and whether the step will synthesize it.
    FrameBracket m_ahead[kFlowLookahead];
    LONGLONG m_aheadTargets[kFlowLookahead] = {};
    bool m_aheadSynth[kFlowLookahead] = {};
    int m_aheadCount = 0;
    // The pair handed over last, for the engine's temporal hints.
    policy::FlowPair m_lastHanded;
    bool m_haveLastHanded = false;
    long long m_flowHit = 0, m_flowLate = 0, m_flowOwn = 0, m_flowMiss = 0;
    long long m_flowsRidden = 0, m_flowsAlone = 0, m_flowsAfterPresent = 0;

    bool m_mark;
    FrameMarker m_marker;               // counter and cell layout only; drawn here
    RECT m_markerRect;

    // Present statistics. lastSyncRefresh is the sink refresh count our previous present
    // landed on; a gap wider than one means refreshes went by showing no new frame of ours.
    UINT m_lastSyncRefresh;
    long long m_missedRefreshes;
    long long m_statsSamples;
    long long m_statsRebases;           // refresh-counter base changes: presentation path changed
    int m_compMode;                     // last DXGI_FRAME_PRESENTATION_MODE seen; -1 = none yet
    long long m_compModeChanges;
    long long m_samplesOverlay;         // presents scanned from an overlay plane
    long long m_samplesComposed;        // presents composed by DWM
    long long m_presentFailures;
    long long m_drawFailures;
    long long m_waitTimeouts;
    long long m_consecutiveWaitTimeouts;
    HRESULT m_lastPresentHr;            // last status, so only CHANGES are logged
    long long m_presents;
};
