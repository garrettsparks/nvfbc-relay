#include "FrameCompositors.h"
#include <SimpleLogger.h>

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
}

// ---------------------------------------------------------------------------------
// BlendCompositor
// ---------------------------------------------------------------------------------

BlendCompositor::BlendCompositor(const policy::PolicyConfig* cfg)
    : m_cfg(cfg)
    , m_device(NULL)
    , m_holdSurface(NULL)
    , m_lastOutput(NULL)
    , m_lastSynthesized(false)
{
    m_rect.left = m_rect.top = m_rect.right = m_rect.bottom = 0;
}

BlendCompositor::~BlendCompositor() {
    if (m_holdSurface) m_holdSurface->Release();
}

bool BlendCompositor::Setup(IDirect3DDevice9Ex* device, int width, int height) {
    m_device = device;
    m_rect.left = 0;
    m_rect.top = 0;
    m_rect.right = width;
    m_rect.bottom = height;

    if (!m_blender.Setup(device)) {
        return false;
    }
    // Hold snapshot in the backbuffer's format: a blended output exists nowhere else
    // (ring frames stay real), so a stall/hole present that must re-show it needs a
    // surface with stable ownership. Flip-model backbuffer contents are not reusable
    // after present.
    HRESULT hr = device->CreateRenderTarget(width, height, D3DFMT_A2R10G10B10,
                                            D3DMULTISAMPLE_NONE, 0, FALSE,
                                            &m_holdSurface, NULL);
    if (FAILED(hr)) {
        LOGERR("BlendCompositor: hold surface creation failed (hr=0x%08lx)", (unsigned long)hr);
        return false;
    }
    return true;
}

int BlendCompositor::Id() const {
    return 1;
}

void BlendCompositor::Compose(const FrameBracket& bracket, IDirect3DSurface9* backbuffer,
                              CompositeOutcome* out) {
    const policy::CompositeDecision d = policy::DecideComposite(bracket.info, m_compState, *m_cfg);

    out->pickLabel = policy::PickLabel(policy::Pick::None);
    out->opLabel = policy::CompositeLabel(d.op);
    out->pickCode = 0;
    out->weightQ = 0;
    out->synthesized = false;
    out->opWeight = 0.0;

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
            m_lastSynthesized = false;
            break;
        }
        case policy::CompositeOp::Blend: {
            if (m_blender.Blend(bracket.beforeTexture, bracket.afterTexture, (float)d.weight)) {
                // Snapshot before the marker burn so a later hold re-presents clean
                // content and burns its own fresh marker.
                m_device->StretchRect(backbuffer, &m_rect, m_holdSurface, &m_rect, D3DTEXF_NONE);
                m_lastOutput = m_holdSurface;
                m_lastSynthesized = true;
                out->synthesized = true;
                out->weightQ = (int)(d.weight * 15.0 + 0.5);
                out->opWeight = d.weight;
            } else {
                // Emergency passthrough of the nearer real frame: a failed draw must
                // not present whatever the flip queue left in the backbuffer.
                IDirect3DSurface9* chosen = bracket.beforeSurface;
                if (bracket.info.afterDiff < bracket.info.beforeDiff) chosen = bracket.afterSurface;
                LOGERR("BlendCompositor: lerp draw failed - passing through a real frame");
                m_device->StretchRect(chosen, &m_rect, backbuffer, &m_rect, D3DTEXF_NONE);
                m_lastOutput = chosen;
                m_lastSynthesized = false;
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
                m_lastSynthesized = (src == m_holdSurface) ? m_lastSynthesized : false;
            }
            out->synthesized = m_lastSynthesized;
            break;
        }
    }
}
