#pragma once

#include "IFrameCompositor.h"
#include "BlendRenderer.h"
#include "InterpSidecar.h"
#include "TemporalPolicy.h"

// Nearest-frame composition: SelectFrame picks one real ring frame (or a repeat) and it
// is copied to the backbuffer sharp. This is the pre-blend present path unchanged, moved
// behind the compositor seam.
class NearestCompositor : public IFrameCompositor {
public:
    // cfg is borrowed from the owning mode and must outlive the compositor.
    explicit NearestCompositor(const policy::PolicyConfig* cfg);

    virtual bool Setup(IDirect3DDevice9Ex* device, int width, int height) override;
    virtual int Id() const override;
    virtual void Compose(const FrameBracket& bracket, IDirect3DSurface9* backbuffer,
                         CompositeOutcome* out) override;

private:
    const policy::PolicyConfig* m_cfg;
    policy::SelectionState m_selState;
    IDirect3DDevice9Ex* m_device;
    RECT m_rect;
    IDirect3DSurface9* m_lastShownSurface;  // ring alias (borrowed); what Repeat re-presents
};

// Shared executor for the synthesizing compositors. DecideComposite's passthrough,
// hold, snapshot, and side/monotone bookkeeping are identical whatever renders the
// synthesized frame; subclasses provide only the synthesis draw. Synthesized output
// exists only on the backbuffer and in the private hold snapshot; ring frames are
// never written.
class SynthCompositorBase : public IFrameCompositor {
public:
    explicit SynthCompositorBase(const policy::PolicyConfig* cfg);
    virtual ~SynthCompositorBase();

    virtual bool Setup(IDirect3DDevice9Ex* device, int width, int height) override;
    virtual void Compose(const FrameBracket& bracket, IDirect3DSurface9* backbuffer,
                         CompositeOutcome* out) override;

protected:
    // Device resources beyond the shared hold surface; called from Setup.
    virtual bool SetupResources() = 0;

    // Render the synthesized frame for this bracket at the given weight onto the
    // backbuffer. On false the base passes the nearer real frame through instead.
    // Set m_lastSynthUs when an engine time is worth logging (pt=).
    virtual bool RenderSynthesis(const FrameBracket& bracket, double weight,
                                 IDirect3DSurface9* backbuffer) = 0;

    const policy::PolicyConfig* m_cfg;
    policy::CompositeState m_compState;
    IDirect3DDevice9Ex* m_device;
    RECT m_rect;
    IDirect3DSurface9* m_holdSurface;   // owned render target: snapshot of the last synthesis
    IDirect3DSurface9* m_lastOutput;    // what Hold re-presents (ring alias or m_holdSurface)
    bool m_lastSynthesized;             // whether m_lastOutput holds synthesized pixels
    long long m_lastSynthUs;            // engine time of the current synthesis; -1 = none
};

// Blend composition: the synthesized frame is a ps_3_0 lerp of the bracket pair.
class BlendCompositor : public SynthCompositorBase {
public:
    explicit BlendCompositor(const policy::PolicyConfig* cfg);
    virtual int Id() const override;

protected:
    virtual bool SetupResources() override;
    virtual bool RenderSynthesis(const FrameBracket& bracket, double weight,
                                 IDirect3DSurface9* backbuffer) override;

private:
    BlendRenderer m_blender;
};

// Interpolating composition: the synthesized frame comes from the D3D11 sidecar
// (raw NVOFA flow + our warp, or NvOFFRUC, per -interp), with the lerp as the
// per-present fallback when the engine cannot deliver. The sidecar initializes in
// OnCaptureStarted (it opens ring slot shared handles, which exist only after
// CaptureRing::Start); a sidecar that cannot initialize refuses the mode.
class InterpCompositor : public SynthCompositorBase {
public:
    explicit InterpCompositor(const policy::PolicyConfig* cfg);
    virtual int Id() const override;
    virtual bool OnCaptureStarted(CaptureRing* ring, LARGE_INTEGER baseQpc,
                                  LONGLONG freqQpc) override;

protected:
    virtual bool SetupResources() override;
    virtual bool RenderSynthesis(const FrameBracket& bracket, double weight,
                                 IDirect3DSurface9* backbuffer) override;

private:
    InterpSidecar m_sidecar;
    BlendRenderer m_blender;   // lerp fallback; output stays synthesized-at-target
    int m_backend;             // InterpBackend, latched at construction
};
