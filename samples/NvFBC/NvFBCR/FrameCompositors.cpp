#include "FrameCompositors.h"
#include <math.h>
#include <SimpleLogger.h>

extern int g_interpBackend;   // NvFBCR.cpp: -interp flow|fruc

const char* SynthExecLabel(int code) {
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
    , m_subGen(false)
    , m_guardRT(NULL)
    , m_guardSys(NULL)
{
    m_rect.left = m_rect.top = m_rect.right = m_rect.bottom = 0;
    m_guardLuma[0] = m_guardLuma[1] = m_guardLuma[2] = NULL;
}

SynthCompositorBase::~SynthCompositorBase() {
    if (m_holdSurface) m_holdSurface->Release();
    if (m_guardRT) m_guardRT->Release();
    if (m_guardSys) m_guardSys->Release();
    for (int i = 0; i < 3; i++) delete[] m_guardLuma[i];
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

    // Content-check resources, only when the substitution is armed. A failure here disarms
    // the substitution rather than the mode: without the check it must not run at all, and
    // blending is a working answer.
    if (m_subGen) {
        hr = device->CreateRenderTarget(kGuardW * kGuardTiles, kGuardH, D3DFMT_A2R10G10B10,
                                        D3DMULTISAMPLE_NONE, 0, FALSE, &m_guardRT, NULL);
        if (SUCCEEDED(hr)) {
            hr = device->CreateOffscreenPlainSurface(kGuardW * kGuardTiles, kGuardH,
                                                     D3DFMT_A2R10G10B10,
                                                     D3DPOOL_SYSTEMMEM, &m_guardSys, NULL);
        }
        if (SUCCEEDED(hr)) {
            for (int i = 0; i < 3; i++) m_guardLuma[i] = new float[kGuardW * kGuardH];
        } else {
            LOGERR("compositor: generated-frame content check unavailable "
                   "(hr=0x%08lx) - substitution disabled", (unsigned long)hr);
            m_subGen = false;
        }
    }
    return true;
}

bool SynthCompositorBase::BlitGuardTile(IDirect3DSurface9* src, int tile) {
    RECT dst;
    dst.left = tile * kGuardW;
    dst.right = dst.left + kGuardW;
    dst.top = 0;
    dst.bottom = kGuardH;
    return SUCCEEDED(m_device->StretchRect(src, &m_rect, m_guardRT, &dst, D3DTEXF_LINEAR));
}

bool SynthCompositorBase::ReadGuardTiles() {
    if (FAILED(m_device->GetRenderTargetData(m_guardRT, m_guardSys)))
        return false;
    D3DLOCKED_RECT lr;
    if (FAILED(m_guardSys->LockRect(&lr, NULL, D3DLOCK_READONLY)))
        return false;
    const unsigned char* base = (const unsigned char*)lr.pBits;
    for (int y = 0; y < kGuardH; y++) {
        const unsigned int* row = (const unsigned int*)(base + (size_t)y * lr.Pitch);
        for (int t = 0; t < kGuardTiles; t++) {
            float* out = m_guardLuma[t];
            for (int x = 0; x < kGuardW; x++) {
                // A2R10G10B10, 10 bits per channel. Luma is the -fgphase instrument's
                // integer weighting, kept EXACTLY, including its scale: the ratio below is
                // scale-free so the units cannot affect it, but the motion floor is a
                // number someone will compare against that instrument's output, and a
                // rescaling here is what turned that floor into a magic constant once
                // already. These are raw channel levels, same as fgphase reports.
                const unsigned int p = row[t * kGuardW + x];
                const float r = (float)((p >> 20) & 0x3FF);
                const float g = (float)((p >> 10) & 0x3FF);
                const float b = (float)(p & 0x3FF);
                out[y * kGuardW + x] = (2.0f * r + 5.0f * g + b) * 0.125f;
            }
        }
    }
    m_guardSys->UnlockRect();
    return true;
}

bool SynthCompositorBase::GeneratedContentUsable(const FrameBracket& bracket) {
    if (!m_guardRT || !m_guardSys || !m_guardLuma[0]) return false;
    if (!bracket.genSurface || !bracket.beforeSurface || !bracket.afterSurface) return false;

    LARGE_INTEGER t0, t1, freq;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);

    // All three downscales are queued before anything is read back, so the pipeline drains
    // once. Issuing a blit and immediately reading it forces a drain per frame.
    const bool ok = BlitGuardTile(bracket.genSurface, 0) &&
                    BlitGuardTile(bracket.beforeSurface, 1) &&
                    BlitGuardTile(bracket.afterSurface, 2) &&
                    ReadGuardTiles();

    QueryPerformanceCounter(&t1);
    const long long us = (t1.QuadPart - t0.QuadPart) * 1000000 / freq.QuadPart;
    m_genSub.checkUsTotal += us;
    if (us > m_genSub.checkUsMax) m_genSub.checkUsMax = us;
    if (!ok) return false;

    // RMS, matching the instrument the thresholds were measured on. gdiff is against the
    // NEARER real neighbour, because a race duplicate copies whichever frame the capture
    // actually had in hand; motion is the bracket pair against each other, which is the
    // scale everything here is measured in. A 5% margin is cropped for the same reason the
    // instrument crops one: edge pixels carry scaling artifacts.
    const float* neighbour = (bracket.info.beforeDiff <= bracket.info.afterDiff)
                                 ? m_guardLuma[1] : m_guardLuma[2];
    const int mx = kGuardW / 20, my = kGuardH / 20;
    double sgg = 0.0, smm = 0.0;
    int n = 0;
    for (int y = my; y < kGuardH - my; y++) {
        for (int x = mx; x < kGuardW - mx; x++) {
            const int i = y * kGuardW + x;
            const double g = (double)m_guardLuma[0][i] - (double)neighbour[i];
            const double m = (double)m_guardLuma[1][i] - (double)m_guardLuma[2][i];
            sgg += g * g;
            smm += m * m;
            n++;
        }
    }
    if (n == 0) return false;
    const double gdiff = sqrt(sgg / n);
    const double motion = sqrt(smm / n);

    // Static content: nothing moved between the bracket frames, so a repeat cannot be seen
    // and the ratio is a division by noise. Allow it rather than computing a random answer.
    if (motion < kStaticMotionFloor) return true;
    return gdiff / motion >= kMinContentRatio;
}

void SynthCompositorBase::Compose(const FrameBracket& bracket, IDirect3DSurface9* backbuffer,
                                  CompositeOutcome* out) {
    // The generated frame is offered to the policy only after PLACEMENT says a substitution
    // is on the table and the CONTENT check says the slot really holds a generated frame.
    // Order matters: placement is arithmetic, the content check is a GPU readback, and
    // asking them the other way round would pay for the readback on every present.
    policy::BracketInfo info = bracket.info;
    if (m_subGen && info.hasGen) {
        if (policy::GeneratedCandidateOnTarget(info, m_compState, *m_cfg)) {
            m_genSub.offered++;
            if (bracket.genScreened) {
                // The capture side already refused duplicates using the driver's change
                // map, which is both more accurate than this comparison and free. Anything
                // still reachable has been screened, so paying for a GPU sync here would
                // buy nothing.
                info.genUsable = true;
            } else {
                info.genUsable = GeneratedContentUsable(bracket);
                if (!info.genUsable) m_genSub.rejectedContent++;
            }
        }
    } else {
        info.hasGen = false;   // disarmed: the policy must not see a candidate at all
    }
    const policy::CompositeDecision d = policy::DecideComposite(info, m_compState, *m_cfg);

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
        case policy::CompositeOp::PassthroughGenerated: {
            // A frame the driver rendered at this instant, presented sharp. Its pixels are
            // one real frame, so synthesized/pixelExec stay at the passthrough values and
            // the marker's documented invariant holds; op=pass-gen carries the provenance.
            m_device->StretchRect(bracket.genSurface, &m_rect, backbuffer, &m_rect,
                                  D3DTEXF_NONE);
            m_lastOutput = bracket.genSurface;
            m_lastOutputExec = 0;
            m_genSub.substituted++;
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
