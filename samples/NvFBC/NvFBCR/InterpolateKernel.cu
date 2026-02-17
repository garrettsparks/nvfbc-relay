/*
 * CUDA kernel for motion-compensated frame interpolation
 * Uses optical flow vectors from NvOFA to warp frames
 */

#include <cuda_runtime.h>
#include <cuda.h>
#include <stdint.h>

// NvOF flow vector format: S10.5 (10-bit integer + 5-bit fractional, signed)
// Stored as int16_t (flowx, flowy) pairs
struct FlowVector {
    int16_t x;
    int16_t y;
};

// Convert S10.5 fixed-point to float
__device__ __forceinline__ float s10_5_to_float(int16_t val) {
    return (float)val / 32.0f;  // Divide by 2^5
}

// Bilinear sample from ABGR8 frame
__device__ __forceinline__ void bilinearSample(
    const uint8_t* frame,
    int width,
    int height,
    float x,
    float y,
    uint8_t* outPixel)
{
    // Clamp coordinates
    x = fmaxf(0.0f, fminf(x, (float)(width - 1)));
    y = fmaxf(0.0f, fminf(y, (float)(height - 1)));

    // Get integer and fractional parts
    int x0 = (int)x;
    int y0 = (int)y;
    int x1 = min(x0 + 1, width - 1);
    int y1 = min(y0 + 1, height - 1);
    float fx = x - (float)x0;
    float fy = y - (float)y0;

    // Get pixel pointers (4 bytes per pixel for ABGR8)
    const uint8_t* p00 = frame + (y0 * width + x0) * 4;
    const uint8_t* p10 = frame + (y0 * width + x1) * 4;
    const uint8_t* p01 = frame + (y1 * width + x0) * 4;
    const uint8_t* p11 = frame + (y1 * width + x1) * 4;

    // Bilinear weights
    float w00 = (1.0f - fx) * (1.0f - fy);
    float w10 = fx * (1.0f - fy);
    float w01 = (1.0f - fx) * fy;
    float w11 = fx * fy;

    // Interpolate each channel
    for (int c = 0; c < 4; c++) {
        float val = w00 * p00[c] + w10 * p10[c] + w01 * p01[c] + w11 * p11[c];
        outPixel[c] = (uint8_t)fminf(255.0f, fmaxf(0.0f, val + 0.5f));
    }
}

// Access flow vector by row using byte stride (NvOF buffers may be padded)
__device__ __forceinline__ FlowVector getFlowVector(
    const uint8_t* flowData,
    int flowStrideBytes,
    int x,
    int y)
{
    const FlowVector* row = (const FlowVector*)(flowData + y * flowStrideBytes);
    return row[x];
}

// Bilinear interpolate flow vector at sub-grid position
__device__ __forceinline__ void bilinearFlowSample(
    const uint8_t* flowData,
    int flowStrideBytes,
    int flowWidth,
    int flowHeight,
    float fx,
    float fy,
    float* outFlowX,
    float* outFlowY)
{
    // Clamp to flow grid bounds
    fx = fmaxf(0.0f, fminf(fx, (float)(flowWidth - 1)));
    fy = fmaxf(0.0f, fminf(fy, (float)(flowHeight - 1)));

    // Get integer and fractional parts
    int x0 = (int)fx;
    int y0 = (int)fy;
    int x1 = min(x0 + 1, flowWidth - 1);
    int y1 = min(y0 + 1, flowHeight - 1);
    float fracX = fx - (float)x0;
    float fracY = fy - (float)y0;

    // Get flow vectors at corners (using byte stride for correct row access)
    FlowVector f00 = getFlowVector(flowData, flowStrideBytes, x0, y0);
    FlowVector f10 = getFlowVector(flowData, flowStrideBytes, x1, y0);
    FlowVector f01 = getFlowVector(flowData, flowStrideBytes, x0, y1);
    FlowVector f11 = getFlowVector(flowData, flowStrideBytes, x1, y1);

    // Bilinear weights
    float w00 = (1.0f - fracX) * (1.0f - fracY);
    float w10 = fracX * (1.0f - fracY);
    float w01 = (1.0f - fracX) * fracY;
    float w11 = fracX * fracY;

    // Interpolate flow (convert from S10.5 to float)
    *outFlowX = w00 * s10_5_to_float(f00.x) + w10 * s10_5_to_float(f10.x) +
                w01 * s10_5_to_float(f01.x) + w11 * s10_5_to_float(f11.x);
    *outFlowY = w00 * s10_5_to_float(f00.y) + w10 * s10_5_to_float(f10.y) +
                w01 * s10_5_to_float(f01.y) + w11 * s10_5_to_float(f11.y);
}

// Main interpolation kernel
// Uses backward warping: for each output pixel, find where it came from in the source frames
__global__ void interpolateKernel(
    const uint8_t* frame0,      // Previous frame (ABGR8)
    const uint8_t* frame1,      // Current frame (ABGR8)
    const uint8_t* flowData,    // Flow from frame0 to frame1 (raw buffer with stride)
    uint8_t* output,            // Interpolated output (ABGR8)
    int width,
    int height,
    int flowWidth,
    int flowHeight,
    int flowStrideBytes,        // Actual byte stride of flow buffer rows
    int gridSize,
    float weight)               // 0.0 = frame0, 1.0 = frame1, 0.5 = midpoint
{
    // Calculate output pixel position
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;

    if (x >= width || y >= height) {
        return;
    }

    // Calculate position in flow grid (with sub-grid precision)
    float flowX = (float)x / (float)gridSize;
    float flowY = (float)y / (float)gridSize;

    // Get interpolated flow vector at this pixel position
    float fx, fy;
    bilinearFlowSample(flowData, flowStrideBytes, flowWidth, flowHeight, flowX, flowY, &fx, &fy);

    // Calculate source positions using backward warping
    // The flow represents motion from frame0 to frame1
    // For an interpolated frame at time t (weight), we want to go backwards
    // Source position in frame0: (x - weight * flow)
    // Source position in frame1: (x + (1-weight) * flow) = (x - weight * flow + flow)

    float srcX0 = (float)x - weight * fx;
    float srcY0 = (float)y - weight * fy;

    float srcX1 = (float)x + (1.0f - weight) * fx;
    float srcY1 = (float)y + (1.0f - weight) * fy;

    // Sample from both frames
    uint8_t pixel0[4], pixel1[4];
    bilinearSample(frame0, width, height, srcX0, srcY0, pixel0);
    bilinearSample(frame1, width, height, srcX1, srcY1, pixel1);

    // Blend the two samples based on weight
    // weight=0 -> all frame0, weight=1 -> all frame1
    uint8_t* outPixel = output + (y * width + x) * 4;
    for (int c = 0; c < 4; c++) {
        float val = (1.0f - weight) * pixel0[c] + weight * pixel1[c];
        outPixel[c] = (uint8_t)fminf(255.0f, fmaxf(0.0f, val + 0.5f));
    }
}

// Host-callable function to launch the kernel
extern "C" void launchInterpolateKernel(
    const uint8_t* frame0,
    const uint8_t* frame1,
    const uint8_t* flowData,
    uint8_t* output,
    int width,
    int height,
    int flowWidth,
    int flowHeight,
    int flowStrideBytes,
    int gridSize,
    float weight,
    CUstream stream)
{
    // Calculate grid and block dimensions
    dim3 blockDim(16, 16);
    dim3 gridDim(
        (width + blockDim.x - 1) / blockDim.x,
        (height + blockDim.y - 1) / blockDim.y
    );

    // Launch kernel
    interpolateKernel<<<gridDim, blockDim, 0, stream>>>(
        frame0,
        frame1,
        flowData,
        output,
        width,
        height,
        flowWidth,
        flowHeight,
        flowStrideBytes,
        gridSize,
        weight
    );
}
