#pragma once

#include <windows.h>
#include <d3d9.h>

#include "CaptureRing.h"
#include "IFrameCompositor.h"
#include "RelayContext.h"
#include "TemporalPolicy.h"

// THE PRESENT PATH: everything between the bracket the present loop found and the frame
// reaching the output window. One implementation per swapchain: the D3D9 device's own
// swapchain, whose windowed INTERVAL_ONE present blocks on DWM's compose clock
// (D3D9PresentPath), and a DXGI flip-model swapchain on the output window, paced by the
// sink's own vblank once Windows promotes it to independent flip (D3D11PresentBackend).
// The loop in TemporalCaptureMode owns the timing, the bracket, the comb lock and the log
// line, and never asks which of the two it is driving.
//
// Each implementation owns exactly one policy::CompositeState, because DecideComposite
// mutates the state it is handed (both Schmitt bands, the last output and target stamps)
// and two deciders on one present would fight over it. The D3D9 path reaches its state
// through the IFrameCompositor it hosts; the D3D11 path is the blend pipeline and decides
// for itself.
//
// Call order per present: WaitForFrame, Compose, BurnMarker, Present. The pacing wait sits
// in WHICHEVER call blocks for that path: the D3D11 path blocks in WaitForFrame, on the
// swapchain's waitable object and before the decision, and its Present returns at once;
// the D3D9 path's WaitForFrame returns at once and its INTERVAL_ONE PresentEx is the wait.
// The loop measures the first around the call and takes the second from Present's return
// value, so blk= on the temporal line is the whole pacing block whichever path ran.
class IPresentPath {
public:
    virtual ~IPresentPath() {}

    // Call AFTER CaptureRing::Start (the ring's slot shared handles must exist). ctx holds
    // the D3D9 present device the shell created, the output window and the output size; a
    // path with its own device ignores the D3D9 one. cfg is borrowed from the owning mode and
    // must outlive the path. mark/markFrames arm the frame marker; baseQpc and freqQpc are
    // the log's time origin and clock. Failure is loud (LOGERR) and the caller refuses the
    // mode rather than running degraded.
    virtual bool Setup(const RelayContext& ctx, CaptureRing* ring,
                       const policy::PolicyConfig* cfg, bool mark, unsigned int markFrames,
                       LARGE_INTEGER baseQpc, LONGLONG freqQpc) = 0;

    // The frame-pacing wait, where this path has one before the decision. Returns false
    // when a bounded wait timed out, which the caller reads as a present that was not paced.
    virtual bool WaitForFrame() = 0;

    // Decide this present and draw it onto this path's own target.
    virtual void Compose(const FrameBracket& bracket, CompositeOutcome* out) = 0;

    // Burn the frame marker over the composed output. Returns the counter burned (mark= on
    // the temporal line), or -1 when the marker is off.
    virtual long long BurnMarker(const CompositeOutcome& out) = 0;

    // Show the composed frame. Returns the QPC ticks the call blocked as a pacing wait: the
    // D3D9 vsync present blocks here; a path that already waited in WaitForFrame returns 0.
    virtual LONGLONG Present(bool vsync) = 0;

    // True once this path's swapchain has stopped retiring frames for good. The caller must
    // stop the mode: nothing recovers from here, and a loop left turning holds the capture
    // session against the next run.
    virtual bool SwapChainStalled() const = 0;

    // Whole-run statistics, logged once at exit.
    virtual void LogSummary() const = 0;

    // Short name for the log and the mode name.
    virtual const char* Name() const = 0;

    // What to tell the user when Setup fails: the mode to type instead, or an empty string
    // when this path has no alternative.
    virtual const char* RefusalAdvice() const = 0;

    // True when this path's swapchain must be the only one on the output window. Flip model
    // allows one swapchain per window and no second API on it, so main then hosts the D3D9
    // devices (the present device, and the ring's capture device) on a hidden window.
    virtual bool OwnsOutputWindow() const = 0;
};
