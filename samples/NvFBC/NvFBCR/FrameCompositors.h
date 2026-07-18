#pragma once

#include "IFrameCompositor.h"
#include "BlendRenderer.h"
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

// Blend composition: DecideComposite passes a real frame through sharp when one sits at
// the target, lerps the bracket pair at the bracket weight otherwise, and holds the last
// output when nothing is presentable. Synthesized output exists only on the backbuffer
// and in the private hold snapshot; ring frames are never written.
class BlendCompositor : public IFrameCompositor {
public:
    explicit BlendCompositor(const policy::PolicyConfig* cfg);
    virtual ~BlendCompositor();

    virtual bool Setup(IDirect3DDevice9Ex* device, int width, int height) override;
    virtual int Id() const override;
    virtual void Compose(const FrameBracket& bracket, IDirect3DSurface9* backbuffer,
                         CompositeOutcome* out) override;

private:
    const policy::PolicyConfig* m_cfg;
    policy::CompositeState m_compState;
    BlendRenderer m_blender;
    IDirect3DDevice9Ex* m_device;
    RECT m_rect;
    IDirect3DSurface9* m_holdSurface;   // owned render target: snapshot of the last blend
    IDirect3DSurface9* m_lastOutput;    // what Hold re-presents (ring alias or m_holdSurface)
    bool m_lastSynthesized;             // whether m_lastOutput holds synthesized pixels
};
