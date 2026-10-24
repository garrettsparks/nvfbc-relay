#pragma once

#include <cstdio>
#include <string>

#include "FlowWarpMath.h"

// The flow warp's pixel shader (docs/optical-flow-interpolation-spec.md, 4.4): the GPU form of
// src/core/FlowWarpMath.h, step for step. Its constants are written into the source from that
// header, so the two cannot drift apart; the probe's self-test checks the compiled shader against
// the CPU reference on generated frames and fields.
//
// Bindings: t0 the before frame, t1 the after frame (sampled linearly, clamped), t2 the forward
// field, t3 the backward field (R16G16_SINT, S10.5 fixed point, one vector per grid cell), s0 the
// linear clamp sampler, b0 WarpParams. Positions are pixels with centres at i + 0.5, as
// SV_Position gives them; a frame is sampled at position / size.
//
// The guard's luma is BT.709 of the sampled colour; on grey frames it equals the CPU reference's
// one-channel value exactly, which is what the self-test feeds both.
inline std::string FlowWarpShaderSource() {
    char defines[1024];
    snprintf(defines, sizeof(defines),
             "#define SOURCE_STEPS %d\n"
             "#define SOURCE_DAMPING %.9g\n"
             "#define RESIDUAL_NEAR %.9g\n"
             "#define RESIDUAL_FAR %.9g\n"
             "#define TRIP_SCALE %.9g\n"
             "#define TRIP_FLOOR %.9g\n"
             "#define TRIP_FADE %.9g\n"
             "#define INCONSISTENT %.9g\n"
             "#define MIN_SIDE %.9g\n"
             "#define LUMA_SIGMA %.9g\n"
             "#define GUARD_SPREAD %.9g\n"
             "#define GUARD_NEAR %.9g\n"
             "#define GUARD_FAR %.9g\n",
             flowwarp::kSourceSteps, flowwarp::kSourceDamping, flowwarp::kResidualNearPx,
             flowwarp::kResidualFarPx, flowwarp::kRoundTripScale, flowwarp::kRoundTripFloorPx2,
             flowwarp::kRoundTripFade, flowwarp::kInconsistentWeight, flowwarp::kMinSideWeight,
             flowwarp::kStaticLumaSigma, flowwarp::kGuardSpreadPx, flowwarp::kGuardNearPx,
             flowwarp::kGuardFarPx);
    static const char* kBody =
        "Texture2D beforeTex : register(t0);\n"
        "Texture2D afterTex : register(t1);\n"
        "Texture2D<int2> fwdTex : register(t2);\n"
        "Texture2D<int2> bwdTex : register(t3);\n"
        "SamplerState samp : register(s0);\n"
        "cbuffer WarpParams : register(b0) {\n"
        "    float w;        // 0 = the before frame's instant, 1 = the after frame's\n"
        "    float grid;     // flow cell size in pixels\n"
        "    float2 size;    // frame size in pixels\n"
        "    int2 cells;     // flow cells across and down\n"
        "    int2 unused;\n"
        "};\n"
        "float2 FlowAt(Texture2D<int2> f, float2 p) {\n"
        "    int2 c = clamp(int2(floor(p / grid)), int2(0, 0), cells - 1);\n"
        "    return float2(f.Load(int3(c, 0))) / 32.0;\n"
        "}\n"
        "float2 Solve(Texture2D<int2> f, float2 p, float k, out float residual) {\n"
        "    float2 s = p - k * FlowAt(f, p);\n"
        "    [unroll] for (int i = 0; i < SOURCE_STEPS; i++) {\n"
        "        s = lerp(s, p - k * FlowAt(f, s), SOURCE_DAMPING);\n"
        "    }\n"
        "    residual = length(s + k * FlowAt(f, s) - p);\n"
        "    return s;\n"
        "}\n"
        "float RoundTrip(Texture2D<int2> outF, Texture2D<int2> backF, float2 s) {\n"
        "    float2 a = FlowAt(outF, s);\n"
        "    float2 b = FlowAt(backF, s + a);\n"
        "    float2 sum = a + b;\n"
        "    float allowed = TRIP_SCALE * (dot(a, a) + dot(b, b)) + TRIP_FLOOR;\n"
        "    return 1.0 - smoothstep(allowed, TRIP_FADE * allowed, dot(sum, sum));\n"
        "}\n"
        "float3 Before(float2 p) { return beforeTex.SampleLevel(samp, p / size, 0).rgb; }\n"
        "float3 After(float2 p) { return afterTex.SampleLevel(samp, p / size, 0).rgb; }\n"
        "float Luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }\n"
        "float4 main(float4 pos : SV_Position) : SV_Target {\n"
        "    float2 p = pos.xy;\n"
        "    float r0, r1;\n"
        "    float2 s0 = Solve(fwdTex, p, w, r0);\n"
        "    float2 s1 = Solve(bwdTex, p, 1.0 - w, r1);\n"
        "    float vis0 = 1.0 - smoothstep(RESIDUAL_NEAR, RESIDUAL_FAR, r0);\n"
        "    float vis1 = 1.0 - smoothstep(RESIDUAL_NEAR, RESIDUAL_FAR, r1);\n"
        "    float trip0 = RoundTrip(fwdTex, bwdTex, s0);\n"
        "    float trip1 = RoundTrip(bwdTex, fwdTex, s1);\n"
        "    float3 z0 = Before(p);\n"
        "    float3 z1 = After(p);\n"
        "    float3 crossfade = z0 * (1.0 - w) + z1 * w;\n"
        "    float k0 = (1.0 - w) * vis0 * (INCONSISTENT + trip0);\n"
        "    float k1 = w * vis1 * (INCONSISTENT + trip1);\n"
        "    float3 warped = crossfade;\n"
        "    if (k0 + k1 >= MIN_SIDE) {\n"
        "        float t = k1 / (k0 + k1);\n"
        "        warped = Before(s0) * (1.0 - t) + After(s1) * t;\n"
        "    }\n"
        "    float worst = abs(Luma(z0) - Luma(z1));\n"
        "    float2 spread[4] = { float2(GUARD_SPREAD, 0), float2(-GUARD_SPREAD, 0),\n"
        "                         float2(0, GUARD_SPREAD), float2(0, -GUARD_SPREAD) };\n"
        "    [unroll] for (int i = 0; i < 4; i++) {\n"
        "        float2 q = p + spread[i];\n"
        "        worst = max(worst, abs(Luma(Before(q)) - Luma(After(q))));\n"
        "    }\n"
        "    float dz = worst / LUMA_SIGMA;\n"
        "    float staticness = exp(-dz * dz);\n"
        "    float reach = max(length(s0 - p), length(s1 - p));\n"
        "    float guard = staticness * smoothstep(GUARD_NEAR, GUARD_FAR, reach);\n"
        "    return float4(warped + (crossfade - warped) * guard, 1.0);\n"
        "}\n";
    return std::string(defines) + kBody;
}

// The constant buffer the shader reads, 32 bytes.
struct FlowWarpParams {
    float w;
    float grid;
    float width;
    float height;
    int cellsX;
    int cellsY;
    int unused[2];
};
