#include "FrameCompositors.h"
#include <SimpleLogger.h>

extern int g_interpBackend;   // NvFBCR.cpp: -interp flow|fruc

// Synthesis executor codes: what actually produced a present's pixels. One vocabulary
// across the log's sx= label and the marker's executor cells, matching the compositor
// IDs (0 real/none, 1 blend, 2 fruc, 3 flow-warp); append-only once shipped.
static const char* SynthExecLabel(int code) {
    switch (code) {
        case 1:  return "blend";
        case 2:  return "fruc";
        case 3:  return "flow-warp";
        default: return "none";
    }
}

// ---------------------------------------------------------------------------------
// NearestCompositor
// ---------------------------------------------------------------------------------

NearestCompositor::NearestCompositor(const policy::PolicyConfig* cfg)
    : m_cfg(cfg)
    , m_device(NULL)
    , m_lastShownSurface(NULL)
{
    m_rect.left = m_rect.top = m_rect.right = m_rect.bottom = 0;
}

bool NearestCompositor::Setup(IDirect3DDevice9Ex* device, int width, int height) {
    m_device = device;
    m_rect.left = 0;
    m_rect.top = 0;
    m_rect.right = width;
    m_rect.bottom = height;
    return true;
}

int NearestCompositor::Id() const {
    return 0;
}

void NearestCompositor::Compose(const FrameBracket& bracket, IDirect3DSurface9* backbuffer,
                                CompositeOutcome* out) {
    const policy::Pick pick = policy::SelectFrame(bracket.info, m_selState, *m_cfg);

    IDirect3DSurface9* chosen = NULL;
    switch (pick) {
        case policy::Pick::Before:
        case policy::Pick::BeforeAdv:
            chosen = bracket.beforeSurface;
            break;
        case policy::Pick::After:
        case policy::Pick::AfterAdv:
            chosen = bracket.afterSurface;
            break;
        default:
            // Repeat re-presents the last SHOWN surface, and bracket.before is not
            // always it: after an after-pick the target can still trail the shown
            // frame (lag > one source period at sub-rate sources), leaving before one
            // frame BEHIND the screen. The surface fallback chain covers startup,
            // before any frame has been shown.
            chosen = m_lastShownSurface;
            if (!chosen && bracket.info.hasBefore) chosen = bracket.beforeSurface;
            if (!chosen && bracket.info.hasAfter)  chosen = bracket.afterSurface;
            break;
    }

    if (chosen) {
        m_device->StretchRect(chosen, &m_rect, backbuffer, &m_rect, D3DTEXF_NONE);
        m_lastShownSurface = chosen;
    }

    out->pickLabel = policy::PickLabel(pick);
    out->opLabel = NULL;
    out->pickCode = (int)pick;
    out->weightQ = bracket.info.hasBefore ? (int)(bracket.weight * 15.0 + 0.5) : 0;
    out->synthesized = false;
    out->opWeight = 0.0;
    out->synthUs = -1;
    out->synthExec = NULL;
    out->pixelExec = 0;
}

// ---------------------------------------------------------------------------------
// SynthCompositorBase
// ---------------------------------------------------------------------------------

SynthCompositorBase::SynthCompositorBase(const policy::PolicyConfig* cfg)
    : m_cfg(cfg)
    , m_device(NULL)
    , m_holdSurface(NULL)
    , m_lastOutput(NULL)
    , m_lastOutputExec(0)
    , m_lastSynthUs(-1)
    , m_lastSynthExecCode(0)
{
    m_rect.left = m_rect.top = m_rect.right = m_rect.bottom = 0;
}

SynthCompositorBase::~SynthCompositorBase() {
    if (m_holdSurface) m_holdSurface->Release();
}

bool SynthCompositorBase::Setup(IDirect3DDevice9Ex* device, int width, int height) {
    m_device = device;
    m_rect.left = 0;
    m_rect.top = 0;
    m_rect.right = width;
    m_rect.bottom = height;

    if (!SetupResources()) {
        return false;
    }
    // Hold snapshot in the backbuffer's format: a synthesized output exists nowhere
    // else (ring frames stay real), so a stall/hole present that must re-show it needs
    // a surface with stable ownership. Flip-model backbuffer contents are not reusable
    // after present.
    HRESULT hr = device->CreateRenderTarget(width, height, D3DFMT_A2R10G10B10,
                                            D3DMULTISAMPLE_NONE, 0, FALSE,
                                            &m_holdSurface, NULL);
    if (FAILED(hr)) {
        LOGERR("compositor: hold surface creation failed (hr=0x%08lx)", (unsigned long)hr);
        return false;
    }
    return true;
}

void SynthCompositorBase::Compose(const FrameBracket& bracket, IDirect3DSurface9* backbuffer,
                                  CompositeOutcome* out) {
    const policy::CompositeDecision d = policy::DecideComposite(bracket.info, m_compState, *m_cfg);

    out->pickLabel = policy::PickLabel(policy::Pick::None);
    out->opLabel = policy::CompositeLabel(d.op);
    out->pickCode = 0;
    out->weightQ = 0;
    out->synthesized = false;
    out->opWeight = 0.0;
    out->synthUs = -1;
    out->synthExec = NULL;
    out->pixelExec = 0;

    switch (d.op) {
        case policy::CompositeOp::PassthroughBefore:
        case policy::CompositeOp::PassthroughAfter: {
            IDirect3DSurface9* chosen;
            if (d.op == policy::CompositeOp::PassthroughBefore) {
                chosen = bracket.beforeSurface;
            } else {
                chosen = bracket.afterSurface;
                out->weightQ = 15;
                out->opWeight = 1.0;
            }
            m_device->StretchRect(chosen, &m_rect, backbuffer, &m_rect, D3DTEXF_NONE);
            m_lastOutput = chosen;
            m_lastOutputExec = 0;
            break;
        }
        case policy::CompositeOp::Synthesize: {
            m_lastSynthUs = -1;
            m_lastSynthExecCode = 0;
            if (RenderSynthesis(bracket, d.weight, backbuffer)) {
                // Snapshot before the marker burn so a later hold re-presents clean
                // content and burns its own fresh marker.
                m_device->StretchRect(backbuffer, &m_rect, m_holdSurface, &m_rect, D3DTEXF_NONE);
                m_lastOutput = m_holdSurface;
                m_lastOutputExec = m_lastSynthExecCode;
                out->synthesized = true;
                out->weightQ = (int)(d.weight * 15.0 + 0.5);
                out->opWeight = d.weight;
                out->synthUs = m_lastSynthUs;
                out->synthExec = SynthExecLabel(m_lastSynthExecCode);
                out->pixelExec = m_lastSynthExecCode;
            } else {
                // Emergency passthrough of the nearer real frame: a failed synthesis
                // must not present whatever the flip queue left in the backbuffer.
                IDirect3DSurface9* chosen = bracket.beforeSurface;
                if (bracket.info.afterDiff < bracket.info.beforeDiff) chosen = bracket.afterSurface;
                LOGERR("compositor: synthesis failed - passing through a real frame");
                m_device->StretchRect(chosen, &m_rect, backbuffer, &m_rect, D3DTEXF_NONE);
                m_lastOutput = chosen;
                m_lastOutputExec = 0;
                out->synthExec = SynthExecLabel(0);
            }
            break;
        }
        default: {
            // Hold: nothing presentable at the target. The fallback chain covers
            // startup, before anything has been output.
            IDirect3DSurface9* src = m_lastOutput;
            if (!src && bracket.info.hasBefore) src = bracket.beforeSurface;
            if (!src && bracket.info.hasAfter)  src = bracket.afterSurface;
            if (src) {
                m_device->StretchRect(src, &m_rect, backbuffer, &m_rect, D3DTEXF_NONE);
                m_lastOutput = src;
                if (src != m_holdSurface) m_lastOutputExec = 0;
            }
            out->synthesized = m_lastOutputExec != 0;
            out->pixelExec = m_lastOutputExec;
            break;
        }
    }
}

// ---------------------------------------------------------------------------------
// BlendCompositor
// ---------------------------------------------------------------------------------

BlendCompositor::BlendCompositor(const policy::PolicyConfig* cfg, bool tint)
    : SynthCompositorBase(cfg)
    , m_tint(tint)
{
}

int BlendCompositor::Id() const {
    return 1;
}

bool BlendCompositor::SetupResources() {
    return m_blender.Setup(m_device, m_tint);
}

bool BlendCompositor::RenderSynthesis(const FrameBracket& bracket, double weight,
                                      IDirect3DSurface9* /*backbuffer*/) {
    // BlendRenderer draws over the device's current render target, which is the
    // backbuffer here (nothing else in the present path changes it).
    m_lastSynthExecCode = 1;
    return m_blender.Blend(bracket.beforeTexture, bracket.afterTexture, (float)weight);
}

// ---------------------------------------------------------------------------------
// InterpCompositor
// ---------------------------------------------------------------------------------

InterpCompositor::InterpCompositor(const policy::PolicyConfig* cfg)
    : SynthCompositorBase(cfg)
    , m_backend(g_interpBackend)
{
}

int InterpCompositor::Id() const {
    if (m_backend == kInterpBackendFlow) return 3;   // flow-warp marker compositor ID
    return 2;                                        // fruc
}

bool InterpCompositor::SetupResources() {
    // The lerp fallback must be ready before the sidecar exists (the sidecar arrives
    // in OnCaptureStarted); the sidecar's own setup failing refuses the mode instead.
    return m_blender.Setup(m_device);
}

bool InterpCompositor::OnCaptureStarted(CaptureRing* ring, LARGE_INTEGER baseQpc,
                                        LONGLONG freqQpc) {
    return m_sidecar.Setup(m_device, ring, m_rect.right, m_rect.bottom, baseQpc, freqQpc);
}

bool InterpCompositor::RenderSynthesis(const FrameBracket& bracket, double weight,
                                       IDirect3DSurface9* backbuffer) {
    if (m_sidecar.Enabled()) {
        const LONGLONG target = bracket.info.beforeTs + bracket.info.beforeDiff;
        if (m_sidecar.Interpolate(bracket, target)) {
            m_device->StretchRect(m_sidecar.OutputSurface9(), &m_rect, backbuffer, &m_rect,
                                  D3DTEXF_NONE);
            m_lastSynthUs = m_sidecar.LastProcessUs();
            m_lastSynthExecCode = (m_backend == kInterpBackendFlow) ? 3 : 2;
            return true;
        }
    }
    // Lerp fallback keeps the output synthesized-at-target when the engine cannot
    // deliver; sx= names the lerp on these presents, which is what marks a fallback.
    m_lastSynthExecCode = 1;
    return m_blender.Blend(bracket.beforeTexture, bracket.afterTexture, (float)weight);
}
