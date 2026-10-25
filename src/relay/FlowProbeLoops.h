#pragma once

#include <windows.h>
#include <dxgi.h>

#include <cstddef>
#include <functional>
#include <vector>

// The flow probe's sustained loop on the optical flow engine's D3D12 interface
// (docs/optical-flow-interpolation-spec.md, 6.1). It runs the flow the D3D11 loop runs (luma,
// grid 4, MEDIUM, cost on, at a fixed rate), so a GPU trace taken beside a game counts its turns
// on the game's graphics engine against the D3D11 loop's. It checks the engine's flow on the
// generated frames before it loops, and logs every second's times from the execute call to its
// return and to the CPU seeing the flow done.
namespace flowprobe {

// Two 8-bit luma frames, the second the first moved by (moveX, moveY).
struct LoopFrames {
    int width = 0;
    int height = 0;
    int moveX = 0;
    int moveY = 0;
    std::vector<unsigned char> before;
    std::vector<unsigned char> after;
};

// forwardOnly runs the forward flow alone, without the backward flow and its cost.
void RunLoopD3D12(IDXGIAdapter* adapter, const LoopFrames& frames, int seconds, int rate,
                  bool forwardOnly);

double Percentile(std::vector<double> v, double q);

// Reads a flow field of cellsX x cellsY vectors (two signed 16-bit values each, 5 fractional
// bits, rowPitch bytes a row), leaving out the cells within 16 px of the border, where the
// moved pattern clamps. Logs its mean and the share of cells within half a pixel of (wantX,
// wantY), and returns whether at least 90% are.
bool CheckFlow(const char* label, const char* direction, const unsigned char* data,
               size_t rowPitch, int cellsX, int cellsY, int grid, float wantX, float wantY);

// One flow: submit it, wait until it is done, and give the milliseconds from the call to its
// return and to the CPU seeing it done. False stops the loop.
using FlowStep = std::function<bool(double* returnMs, double* doneMs)>;

// Calls step `rate` times a second for `seconds`, each at its slot, and logs each second.
void RunPaced(const char* label, int seconds, int rate, const FlowStep& step);

}  // namespace flowprobe
