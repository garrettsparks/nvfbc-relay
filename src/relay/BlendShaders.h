#pragma once

#include <windows.h>
#include <d3d11.h>

#include <vector>

// THE BLEND SHADERS (-blend): other ways to draw a synthesized frame on the D3D11 present from
// what the blend has, the two frames and the weight, each timed on the GPU. The present draws
// with them only for -blend and only a synthesized frame at the output's size; without -blend
// it draws the blend of its own and nothing here exists.
//
//   plain   the blend, lerp(a, b, w) of the stored values: the baseline, timed like the others.
//   linear  the blend in linear light: decoded from sRGB, mixed, encoded again, so a mix of a
//           bright and a dark picture comes out at its true brightness, not darker.
//   pan     the blend following a camera pan: one shift of the whole picture from a to b is
//           found, each frame is sampled moved by its share of it, and each pixel keeps that
//           mix or the plain one, whichever's two samples agree better, so a static overlay
//           stays where it is. The shift is found coarse to fine on luma: every shift up to 16
//           texels each way on a 1/16 copy, then 3 each way around it on a 1/4 copy, then 3
//           pixels each way at full size with a parabola through the best and its neighbours
//           for the fraction. It is used only when its match beats no shift at all by a fifth.
//           Phase correlation in place of the coarse search found the same shifts at slightly
//           more cost (ofprobe, 2026-10-25) and was removed.
//
// Everything runs on the present's context, so it goes to the GPU in the present's own turns.
class BlendShaders {
public:
    // Linear and Pan are -blend's numbers (launch::BlendShaderNames); Plain, the present's own
    // blend done here and timed, is the probe's baseline.
    enum Shader { Linear = 0, Pan = 1, Plain = 2 };

    static const char* Name(int shader);

    BlendShaders() = default;
    ~BlendShaders();
    BlendShaders(const BlendShaders&) = delete;
    BlendShaders& operator=(const BlendShaders&) = delete;

    // Readies shader for frames and an output of width x height. Loud on failure.
    bool Setup(ID3D11Device* dev, ID3D11DeviceContext* ctx, int shader, int width, int height);

    // The pan shader: finds the shift from a to b into the shift texture. The others: nothing.
    bool Estimate(ID3D11ShaderResourceView* a, ID3D11ShaderResourceView* b);
    // Draws the synthesized frame at w over rtv; the pan shader uses the last Estimate's shift.
    bool Blend(ID3D11ShaderResourceView* a, ID3D11ShaderResourceView* b, float w,
               ID3D11RenderTargetView* rtv);
    // Estimate then Blend, timed, for the present numbered present (TakeTime).
    bool Draw(ID3D11ShaderResourceView* a, ID3D11ShaderResourceView* b, float w,
              ID3D11RenderTargetView* rtv, long long present);

    // The GPU time of draws at earlier presents, read without waiting, as
    // OpticalFlowWarp::TakeWarpTime reads the warp's. For the pan shader, the newest one's
    // shift as well, followed or not: *shiftX, *shiftY in output pixels, *ratio its match's
    // cost over no shift's, *follow 1 when it was followed, 0 when not; -1 for the others.
    bool TakeTime(long long present, long long* us, int* presentsAgo, int* follow,
                  float* shiftX, float* shiftY, float* ratio);

    // For the probe: the shift texture, one float4 (x and y in output pixels, 1 when the pan
    // shader follows it, its match's cost over no shift's), and setting it by hand.
    ID3D11Texture2D* ShiftTexture() const { return m_shift.tex; }
    void ForceShift(float x, float y);
    // For the probe: timestamp queries the pan shader's Estimate ends after each of its steps,
    // the frames shrunk, the coarse step, the 1/4 step and the full-size step; NULL for none.
    static const int kSteps = 4;
    void SetStepStamps(ID3D11Query* const* stamps) { m_stepStamps = stamps; }

    void LogSummary() const;

private:
    struct Tex {
        ID3D11Texture2D* tex = NULL;
        ID3D11ShaderResourceView* srv = NULL;
        ID3D11RenderTargetView* rtv = NULL;
        ID3D11UnorderedAccessView* uav = NULL;
    };
    // One estimation step's constants, as the compute shaders' cbuffer Stage reads them.
    struct Stage {
        int sizeX, sizeY;   // the source in texels
        int radius;         // candidate shifts -radius..radius each way
        int stride;         // the cost is summed over every stride-th texel each way
        int scale;          // the previous step's shift times scale is the centre; 0 none
        int color;          // the source is a colour frame, its luma read; else one channel
        int zero;           // one candidate more, no shift at all, last
        int last;           // the pick refines the fraction and decides whether to follow it
    };
    struct Timing {
        ID3D11Query* disjoint = NULL;
        ID3D11Query* start = NULL;
        ID3D11Query* end = NULL;
        ID3D11Texture2D* shift = NULL;   // the pan shader: a copy of this draw's shift, to read
        long long present = 0;
        bool pending = false;
    };
    static const int kTimings = 8;

    bool MakeTex(Tex* t, int w, int h, DXGI_FORMAT format, bool rt, bool uav);
    void FreeTex(Tex* t);
    bool CompilePs(const char* src, const char* name, ID3D11PixelShader** out);
    bool CompileCs(const char* src, const char* name, ID3D11ComputeShader** out);
    void SetStage(const Stage& s);
    void RunCs(ID3D11ComputeShader* cs, ID3D11ShaderResourceView* t0,
               ID3D11ShaderResourceView* t1, ID3D11ShaderResourceView* t2,
               ID3D11UnorderedAccessView* u0, UINT x, UINT y, UINT z);
    void Downscale(ID3D11ShaderResourceView* src, int srcW, int srcH, bool color, const Tex& dst,
                   int dstW, int dstH);
    void Search(ID3D11ShaderResourceView* a, ID3D11ShaderResourceView* b,
                ID3D11ShaderResourceView* prev, const Stage& s, const Tex& out);

    bool m_ready = false;
    int m_shader = Plain;
    int m_width = 0, m_height = 0;
    int m_w1 = 0, m_h1 = 0, m_w2 = 0, m_h2 = 0;
    ID3D11Device* m_dev = NULL;          // borrowed from the present
    ID3D11DeviceContext* m_ctx = NULL;   // borrowed from the present

    ID3D11VertexShader* m_vs = NULL;
    ID3D11PixelShader* m_blendPs = NULL;
    ID3D11PixelShader* m_downPs = NULL;
    ID3D11ComputeShader* m_searchCs = NULL;
    ID3D11ComputeShader* m_pickCs = NULL;
    ID3D11SamplerState* m_linear = NULL;
    ID3D11Buffer* m_blendCb = NULL;
    ID3D11Buffer* m_downCb = NULL;
    ID3D11Buffer* m_stageCb = NULL;

    Tex m_a1, m_b1, m_a2, m_b2;          // luma at 1/4 and 1/16 of each frame
    Tex m_costs;                         // one row of candidate costs
    Tex m_coarse, m_mid, m_shift;        // the shift after each step, one float4 each

    ID3D11Query* const* m_stepStamps = NULL;
    Timing m_timings[kTimings];
    std::vector<float> m_us;             // every draw's GPU time read, for the summary
    std::vector<float> m_shiftLengths;   // every followed shift read, in pixels
    std::vector<float> m_ratios;         // every shift read's cost over no shift's
    long long m_draws = 0;
    long long m_untimed = 0;
    long long m_shiftsRead = 0;
    long long m_shiftsFollowed = 0;
};
