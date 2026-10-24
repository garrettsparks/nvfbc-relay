#pragma once

// THE FLOW WARP'S MATH, as a CPU reference (docs/optical-flow-interpolation-spec.md, 4.4).
// D3D-free and header-only: the policy suite holds it to cases with known answers, and the
// shader that runs on the GPU is checked against it. Every constant here is the shader's.
//
// Positions are in output pixels with pixel i's centre at i + 0.5, as SV_Position gives them.
// A flow field holds one vector per grid cell, the motion in pixels of the pixels that cell
// covers: the forward field from the before frame to the after frame, the backward field the
// other way. A field is read at the cell a position falls in, clamped at the edges. Not
// bilinearly: smoothing the flow across an object's edge invents in-between motions there, and
// those land points on output pixels that no real point reaches, which hides the occlusions
// step 2 below exists to find.
//
// For an output pixel p at weight w (0 = the before frame's instant, 1 = the after frame's):
//
// 1. SOURCE BY FIXED POINT. The before frame supplies p from the point s0 whose own motion,
//    carried for a fraction w of the interval, lands on p: s0 + w F_fwd(s0) = p. It starts from
//    p - w F_fwd(p) (exact wherever the motion is uniform) and is refined by damped fixed-point
//    steps, which read the flow where the content IS rather than at the output pixel (reading
//    it at p misplaces every moving edge by the edge's motion). The after frame likewise, with
//    the backward field and 1 - w.
//
// 2. VISIBILITY FROM THE RESIDUAL. Where a frame holds no point that lands on p, the steps
//    cannot settle and |s + k F(s) - p| stays large. That is exactly an occlusion: behind a
//    moving object the uncovered background is in the after frame and in no point of the
//    before frame, and ahead of it the background about to be covered is in the before frame
//    only. A side with a large residual drops out and the other side supplies the pixel alone;
//    where both drop out, the plain crossfade at p.
//
//    Both sides can settle and still disagree: inside a moving object, just behind its trailing
//    edge, the before frame offers the object and the after frame the background it uncovers
//    LATER in the interval. Which one is in front at w is read from a round trip: content seen
//    in both frames goes out along one field and comes back along the other (the object), while
//    content seen in one frame only does not (the background, which in the other frame sits under
//    the object). A side that round-trips outweighs one that does not; a side that does not still
//    outweighs one that cannot settle at all. Timing is already in the fixed point: once the
//    object has passed p, no point of it lands there and its side drops out by residual.
//
// 3. THE STATIC GUARD. Where the two frames already agree around p but the warp would reach far,
//    the flow is wrong for this pixel (an opaque static overlay over a moving scene: the flow
//    follows the scene under it), and the crossfade at p, which is the overlay itself, is used.
//    Agreement is asked of p and four neighbours, not of p alone (FFmpeg's guard): moving
//    texture matches itself at one pixel by coincidence often enough to speckle, at five rarely.

#include <cmath>

namespace flowwarp {

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;
};

inline Vec2 Add(Vec2 a, Vec2 b) { return {a.x + b.x, a.y + b.y}; }
inline Vec2 Sub(Vec2 a, Vec2 b) { return {a.x - b.x, a.y - b.y}; }
inline Vec2 Scale(Vec2 a, float k) { return {a.x * k, a.y * k}; }
inline float Length(Vec2 a) { return std::sqrt(a.x * a.x + a.y * a.y); }
inline Vec2 Mix(Vec2 a, Vec2 b, float t) { return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t}; }

// Fixed-point steps and their damping (FFmpeg's vf_fruc_vulkan settles on the same pair).
const int kSourceSteps = 6;
const float kSourceDamping = 0.5f;

// Visibility: a residual under kResidualNearPx keeps a side whole, over kResidualFarPx drops it.
// With the field read per cell, a source that exists settles to a residual of zero; one that
// does not swings across a cell edge and misses by about the edge's step in motion.
const float kResidualNearPx = 0.5f;
const float kResidualFarPx = 1.5f;

// Round trip: a vector a and the vector b found where a leads agree when |a + b|^2 stays under
// kRoundTripScale (|a|^2 + |b|^2) + kRoundTripFloorPx2 (Sundaram, Brox and Keutzer 2010), and the
// agreement fades out by kRoundTripFade times that. A side that fails keeps kInconsistentWeight
// of its weight, so it still beats a side with no settled source.
const float kRoundTripScale = 0.01f;
const float kRoundTripFloorPx2 = 0.5f;
const float kRoundTripFade = 4.0f;
const float kInconsistentWeight = 0.01f;

// Below this summed side weight neither side can supply the pixel.
const float kMinSideWeight = 1e-4f;

// The static guard: luma agreement within about kStaticLumaSigma (on 0..1) at p and at the four
// points kGuardSpreadPx away counts as agreeing, and a warp reaching between kGuardNearPx and
// kGuardFarPx hands the pixel over to the crossfade progressively. FFmpeg uses 5 to 20 px at one
// pixel; 2 to 8 keeps an overlay sharp at the motion a 60 fps game makes in half a frame.
const float kStaticLumaSigma = 0.03f;
const float kGuardSpreadPx = 2.0f;
const float kGuardNearPx = 2.0f;
const float kGuardFarPx = 8.0f;

inline float Clamp01(float x) {
    if (x < 0.0f) return 0.0f;
    if (x > 1.0f) return 1.0f;
    return x;
}

// Exact at both ends (t = 0 gives a, t = 1 gives b, bit for bit), which a + (b - a) t is not.
inline float Lerp(float a, float b, float t) { return a * (1.0f - t) + b * t; }

inline float Smoothstep(float e0, float e1, float x) {
    const float t = Clamp01((x - e0) / (e1 - e0));
    return t * t * (3.0f - 2.0f * t);
}

// One vector per grid cell, row major, cellsX * cellsY of them, in pixels.
struct FlowField {
    int cellsX = 0;
    int cellsY = 0;
    float grid = 1.0f;
    const Vec2* vectors = nullptr;

    Vec2 Cell(int cx, int cy) const {
        if (cx < 0) cx = 0;
        if (cy < 0) cy = 0;
        if (cx > cellsX - 1) cx = cellsX - 1;
        if (cy > cellsY - 1) cy = cellsY - 1;
        return vectors[cy * cellsX + cx];
    }

    // The vector of the cell a position in pixels falls in.
    Vec2 At(Vec2 p) const {
        return Cell((int)std::floor(p.x / grid), (int)std::floor(p.y / grid));
    }
};

// The point s with s + k F(s) = p, from p - k F(p) by damped fixed-point steps; *residual is
// how far the last s misses, in pixels.
inline Vec2 SolveSource(const FlowField& f, Vec2 p, float k, float* residual) {
    Vec2 s = Sub(p, Scale(f.At(p), k));
    for (int i = 0; i < kSourceSteps; i++) {
        s = Mix(s, Sub(p, Scale(f.At(s), k)), kSourceDamping);
    }
    *residual = Length(Sub(Add(s, Scale(f.At(s), k)), p));
    return s;
}

inline float Visibility(float residual) {
    return 1.0f - Smoothstep(kResidualNearPx, kResidualFarPx, residual);
}

// How well the content at s round-trips: out along `out` to s + out(s), back along `back` from
// there. 1 = seen in both frames, 0 = seen in this one only.
inline float RoundTrip(const FlowField& out, const FlowField& back, Vec2 s) {
    const Vec2 a = out.At(s);
    const Vec2 b = back.At(Add(s, a));
    const Vec2 sum = Add(a, b);
    const float miss = sum.x * sum.x + sum.y * sum.y;
    const float allowed =
        kRoundTripScale * (a.x * a.x + a.y * a.y + b.x * b.x + b.y * b.y) + kRoundTripFloorPx2;
    return 1.0f - Smoothstep(allowed, kRoundTripFade * allowed, miss);
}

// What the warp produced for one pixel, with its parts for tests and diagnostics.
struct WarpResult {
    float value = 0.0f;
    Vec2 before;           // where the before frame was read
    Vec2 after;            // where the after frame was read
    float visBefore = 0.0f;
    float visAfter = 0.0f;
    float tripBefore = 0.0f;
    float tripAfter = 0.0f;
    float guard = 0.0f;    // 0 = warped, 1 = crossfade at p
};

// One channel of one output pixel. before and after are callables taking a Vec2 position in
// pixels and returning that frame's value there (bilinear and clamped, as the GPU's sampler);
// the value doubles as the guard's luma, so tests run on luma images. The shader does the same
// for each colour channel with one luma for the guard.
template <class BeforeFn, class AfterFn>
inline WarpResult WarpPixel(const BeforeFn& before, const AfterFn& after, const FlowField& fwd,
                            const FlowField& bwd, Vec2 p, float w) {
    WarpResult r;
    float res0 = 0.0f;
    float res1 = 0.0f;
    r.before = SolveSource(fwd, p, w, &res0);
    r.after = SolveSource(bwd, p, 1.0f - w, &res1);
    r.visBefore = Visibility(res0);
    r.visAfter = Visibility(res1);
    r.tripBefore = RoundTrip(fwd, bwd, r.before);
    r.tripAfter = RoundTrip(bwd, fwd, r.after);

    const float z0 = before(p);
    const float z1 = after(p);
    const float crossfade = Lerp(z0, z1, w);

    const float k0 = (1.0f - w) * r.visBefore * (kInconsistentWeight + r.tripBefore);
    const float k1 = w * r.visAfter * (kInconsistentWeight + r.tripAfter);
    float warped = crossfade;
    if (k0 + k1 >= kMinSideWeight) {
        warped = Lerp(before(r.before), after(r.after), k1 / (k0 + k1));
    }

    float worstDz = std::fabs(z0 - z1);
    const Vec2 spread[4] = {{kGuardSpreadPx, 0.0f}, {-kGuardSpreadPx, 0.0f},
                            {0.0f, kGuardSpreadPx}, {0.0f, -kGuardSpreadPx}};
    for (const Vec2& o : spread) {
        const Vec2 q = Add(p, o);
        worstDz = std::fmax(worstDz, std::fabs(before(q) - after(q)));
    }
    const float dz = worstDz / kStaticLumaSigma;
    const float staticness = std::exp(-dz * dz);
    const float reach = std::fmax(Length(Sub(r.before, p)), Length(Sub(r.after, p)));
    r.guard = staticness * Smoothstep(kGuardNearPx, kGuardFarPx, reach);
    r.value = warped + (crossfade - warped) * r.guard;
    return r;
}

}  // namespace flowwarp
