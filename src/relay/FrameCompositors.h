#pragma once

#include "IFrameCompositor.h"
#include "BlendRenderer.h"
#include "TemporalPolicy.h"

// Nearest-frame composition: the step's selection picks one real ring frame (or a repeat)
// and it is copied to the backbuffer sharp.
class NearestCompositor : public IFrameCompositor {
public:
    NearestCompositor();

    virtual bool Setup(IDirect3DDevice9Ex* device, int width, int height) override;
    virtual int Id() const override;
    virtual void Compose(const FrameBracket& bracket, const policy::PresentStepResult& step,
                         IDirect3DSurface9* backbuffer, CompositeOutcome* out) override;

private:
    IDirect3DDevice9Ex* m_device;
    RECT m_rect;
    IDirect3DSurface9* m_lastShownSurface;  // ring alias (borrowed); what Repeat re-presents
};

// Shared executor for the synthesizing compositors. The passthrough, hold and snapshot
// handling of the step's composite decision is identical whatever renders the synthesized
// frame; subclasses provide only the synthesis draw. Synthesized output exists only on the
// backbuffer and in the private hold snapshot; ring frames are never written.
class SynthCompositorBase : public IFrameCompositor {
public:
    SynthCompositorBase();
    virtual ~SynthCompositorBase();

    virtual bool Setup(IDirect3DDevice9Ex* device, int width, int height) override;
    virtual void Compose(const FrameBracket& bracket, const policy::PresentStepResult& step,
                         IDirect3DSurface9* backbuffer, CompositeOutcome* out) override;

protected:
    // Device resources beyond the shared hold surface; called from Setup.
    virtual bool SetupResources() = 0;

    // Render the synthesized frame for this bracket at the given weight onto the
    // backbuffer. On false the base passes the nearer real frame through instead.
    // Set m_lastSynthExecCode to the executor that produced the pixels (always;
    // success implies it) and m_lastSynthUs when an engine time is worth logging (pt=).
    virtual bool RenderSynthesis(const FrameBracket& bracket, double weight,
                                 IDirect3DSurface9* backbuffer) = 0;

    IDirect3DDevice9Ex* m_device;
    RECT m_rect;
    IDirect3DSurface9* m_holdSurface;   // owned render target: snapshot of the last synthesis
    IDirect3DSurface9* m_lastOutput;    // what Hold re-presents (ring alias or m_holdSurface)
    int m_lastOutputExec;               // executor code of m_lastOutput's pixels (0 = real)
    long long m_lastSynthUs;            // engine time of the current synthesis; -1 = none
    int m_lastSynthExecCode;            // executor code of the current synthesis
};

// Blend composition: the synthesized frame is a ps_3_0 lerp of the bracket pair.
class BlendCompositor : public SynthCompositorBase {
public:
    // tint: debug only, stamps a border on every synthesized frame so blends are visible
    // at playback speed. It rides the existing lerp pass and never touches passthroughs.
    explicit BlendCompositor(bool tint = false);
    virtual int Id() const override;

protected:
    virtual bool SetupResources() override;
    virtual bool RenderSynthesis(const FrameBracket& bracket, double weight,
                                 IDirect3DSurface9* backbuffer) override;

private:
    BlendRenderer m_blender;
    bool m_tint;
};
