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

    // Offer the driver's retracted generated frame as an alternative to synthesizing.
    // Off by default: with it off the policy never sees a candidate and every decision is
    // bit-for-bit what it was.
    void EnableGeneratedSubstitution(bool on) { m_subGen = on; }

    // Counts for the summary line, so a field capture reports the population directly
    // instead of it being inferred from a replay. Refusals are split by cause because
    // they mean different things: out-of-gate argues about a threshold, reuse and
    // no-advance are rules working, and content-rejected is the capture race.
    struct GenSubStats {
        long long substituted = 0;
        long long offered = 0;          // placement said yes, before the content check
        long long rejectedContent = 0;
        long long checkUsMax = 0;
        long long checkUsTotal = 0;
    };
    const GenSubStats& GeneratedSubstitutionStats() const { return m_genSub; }

protected:
    // Device resources beyond the shared hold surface; called from Setup.
    virtual bool SetupResources() = 0;

    // Render the synthesized frame for this bracket at the given weight onto the
    // backbuffer. On false the base passes the nearer real frame through instead.
    // Set m_lastSynthExecCode to the executor that produced the pixels (always;
    // success implies it) and m_lastSynthUs when an engine time is worth logging (pt=).
    virtual bool RenderSynthesis(const FrameBracket& bracket, double weight,
                                 IDirect3DSurface9* backbuffer) = 0;

    const policy::PolicyConfig* m_cfg;
    policy::CompositeState m_compState;
    IDirect3DDevice9Ex* m_device;
    RECT m_rect;
    IDirect3DSurface9* m_holdSurface;   // owned render target: snapshot of the last synthesis
    IDirect3DSurface9* m_lastOutput;    // what Hold re-presents (ring alias or m_holdSurface)
    int m_lastOutputExec;               // executor code of m_lastOutput's pixels (0 = real)
    long long m_lastSynthUs;            // engine time of the current synthesis; -1 = none
    int m_lastSynthExecCode;            // executor code of the current synthesis

private:
    // THE CONTENT CHECK. The capture API races: roughly 12% of the time the generated slot
    // holds a copy of a real frame instead, and presenting that shows the same content
    // twice. The discriminator is the RATIO gdiff/motion, never absolute gdiff - 0.55 for a
    // genuine generated frame against ~0 for a race duplicate, and constant across captures
    // with 25x different motion. An absolute threshold inverts the answer on low-motion
    // content.
    //
    // Runs ONLY where a substitution is already on the table (measured: ~0.3 presents per
    // second). The same comparison on every capture wake is what the -fgphase diagnostic
    // does, and it moves motion-gated video duplicates from 0.23/s to 0.79/s.
    bool GeneratedContentUsable(const FrameBracket& bracket);
    // Downscale one source into its tile of the guard target. GPU work only, no readback:
    // all three tiles are issued before anything is read, so the pipeline drains ONCE
    // instead of once per frame. The three-drain version cost a measured 864 us mean and
    // 27 ms worst on the present thread, and produced 375 late presents in an hour.
    bool BlitGuardTile(IDirect3DSurface9* src, int tile);
    // Lock the guard target once and convert all three tiles to luma.
    bool ReadGuardTiles();

    // Working geometry for the check, per tile. Far smaller than the -fgphase instrument's
    // 320x180, because this asks only whether two frames differ at all, not how far content
    // moved. Three tiles side by side: generated, before, after.
    static const int kGuardW = 64;
    static const int kGuardH = 36;
    static const int kGuardTiles = 3;
    // gdiff/motion below this reads as the same content twice. The measured classes sit at
    // ~0.55 and ~0 with a near-empty 0.15-0.40 gap (0.16% of 8975 samples), so the cut is
    // nowhere near either.
    static constexpr double kMinContentRatio = 0.15;
    // Motion below this is treated as static and the frame is allowed without a ratio.
    //
    // A DIVISION GUARD, not a visibility threshold, and in the -fgphase instrument's own
    // units so it can be compared against that instrument's output without conversion.
    // Swept against the ground-truth capture, the duplicate rate above the floor is
    // 8.87-8.98% for every floor from 0.0 to 5.0: the floor does not change how well the
    // ratio discriminates, only how many substitutions skip the check, and that grows
    // monotonically (34 batches skipped at 0.2, 1807 at 5.0). The bimodal gap is empty in
    // the lowest motion bands too, so there is no level at which the ratio stops working.
    //
    // So it sits just above zero: at that motion a duplicate and a blend are equally
    // invisible, and a larger value only lets more unchecked frames through. For scale,
    // the instrument reads p50 2.84 on natural gameplay and 70.6 on a constant-yaw pan.
    //
    // The first field build had 0.002 here on a rescaled 0-1 signal, which meant 2.05 in
    // these units - above the median of natural gameplay - and skipped the check on 19.2%
    // of batches.
    static constexpr double kStaticMotionFloor = 0.01;

    bool m_subGen;
    IDirect3DSurface9* m_guardRT;
    IDirect3DSurface9* m_guardSys;
    float* m_guardLuma[3];              // gen, before, after
    GenSubStats m_genSub;
};

// Blend composition: the synthesized frame is a ps_3_0 lerp of the bracket pair.
class BlendCompositor : public SynthCompositorBase {
public:
    // tint: debug only, stamps a border on every synthesized frame so blends are visible
    // at playback speed. It rides the existing lerp pass and never touches passthroughs.
    explicit BlendCompositor(const policy::PolicyConfig* cfg, bool tint = false);
    virtual int Id() const override;

protected:
    virtual bool SetupResources() override;
    virtual bool RenderSynthesis(const FrameBracket& bracket, double weight,
                                 IDirect3DSurface9* backbuffer) override;

private:
    BlendRenderer m_blender;
    bool m_tint;
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
