#include "FlowProbeLoops.h"
#include <SimpleLogger.h>
#include <d3d12.h>

#include <cstring>

#include "nvof/nvOpticalFlowCommon.h"
#include "nvof/nvOpticalFlowD3D12.h"

// The probe's loop on the optical flow engine's D3D12 interface. The interface has no queue of
// its own: an execute waits for fence values and sets one when the flow is done, so the CPU
// learns of the end from the fence and nothing is asked of a graphics queue. Frames are uploaded
// and the flow read back once, through a direct queue, before the loop starts.
//
// d3d12.dll and the engine's D3D12 entry point are found by name at run time, so the relay
// imports nothing new for a probe.

namespace flowprobe {
namespace {

const char* kLabel = "d3d12";

template <class T>
struct Com {
    T* p = nullptr;
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    ~Com() {
        if (p) p->Release();
    }
    void** out() { return reinterpret_cast<void**>(&p); }
};

using CreateDeviceFn = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
using CreateInstanceFn = NV_OF_STATUS(NVOFAPI*)(uint32_t, NV_OF_D3D12_API_FUNCTION_LIST*);

D3D12_RESOURCE_DESC TextureDesc(UINT width, UINT height, DXGI_FORMAT format) {
    D3D12_RESOURCE_DESC d = {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = width;
    d.Height = height;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = format;
    d.SampleDesc.Count = 1;
    return d;
}

D3D12_RESOURCE_DESC BufferDesc(UINT64 bytes) {
    D3D12_RESOURCE_DESC d = {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = bytes;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = DXGI_FORMAT_UNKNOWN;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return d;
}

bool Committed(ID3D12Device* dev, D3D12_HEAP_TYPE heap, const D3D12_RESOURCE_DESC& desc,
               D3D12_RESOURCE_STATES state, ID3D12Resource** out) {
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = heap;
    return SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc, state, NULL,
                                                  __uuidof(ID3D12Resource),
                                                  reinterpret_cast<void**>(out)));
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES from,
                                  D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    return b;
}

// The device's direct queue with one list and the fence the whole loop counts on.
struct Gpu {
    Com<ID3D12Device> dev;
    Com<ID3D12CommandQueue> queue;
    Com<ID3D12CommandAllocator> alloc;
    Com<ID3D12GraphicsCommandList> list;
    Com<ID3D12Fence> fence;
    UINT64 value = 0;
    HANDLE event = NULL;
    ~Gpu() {
        if (event) CloseHandle(event);
    }

    // Waits on the CPU until the fence reaches v; false after two seconds.
    bool WaitFor(UINT64 v) {
        if (fence.p->GetCompletedValue() >= v) return true;
        if (FAILED(fence.p->SetEventOnCompletion(v, event))) return false;
        return WaitForSingleObject(event, 2000) == WAIT_OBJECT_0;
    }

    // Runs the recorded list and waits for it.
    bool Submit() {
        if (FAILED(list.p->Close())) return false;
        ID3D12CommandList* lists[] = {list.p};
        queue.p->ExecuteCommandLists(1, lists);
        if (FAILED(queue.p->Signal(fence.p, ++value))) return false;
        if (!WaitFor(value)) return false;
        return SUCCEEDED(alloc.p->Reset()) && SUCCEEDED(list.p->Reset(alloc.p, NULL));
    }
};

// Copies bytes (rowBytes a row, height rows) into a texture in the common state.
bool Upload(Gpu& g, ID3D12Resource* tex, const D3D12_RESOURCE_DESC& desc, const unsigned char* bytes,
            UINT rowBytes) {
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
    UINT rows = 0;
    UINT64 rowSize = 0, total = 0;
    g.dev.p->GetCopyableFootprints(&desc, 0, 1, 0, &fp, &rows, &rowSize, &total);
    Com<ID3D12Resource> staging;
    if (!Committed(g.dev.p, D3D12_HEAP_TYPE_UPLOAD, BufferDesc(total),
                   D3D12_RESOURCE_STATE_GENERIC_READ, &staging.p)) {
        return false;
    }
    unsigned char* mapped = NULL;
    if (FAILED(staging.p->Map(0, NULL, reinterpret_cast<void**>(&mapped)))) return false;
    for (UINT y = 0; y < rows; y++) {
        std::memcpy(mapped + fp.Offset + (size_t)y * fp.Footprint.RowPitch,
                    bytes + (size_t)y * rowBytes, rowBytes);
    }
    staging.p->Unmap(0, NULL);
    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = tex;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = staging.p;
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = fp;
    D3D12_RESOURCE_BARRIER b = Transition(tex, D3D12_RESOURCE_STATE_COMMON,
                                          D3D12_RESOURCE_STATE_COPY_DEST);
    g.list.p->ResourceBarrier(1, &b);
    g.list.p->CopyTextureRegion(&dst, 0, 0, 0, &src, NULL);
    b = Transition(tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    g.list.p->ResourceBarrier(1, &b);
    return g.Submit();
}

// Reads a flow texture back and checks it against the known motion.
bool ReadBack(Gpu& g, ID3D12Resource* tex, const D3D12_RESOURCE_DESC& desc, int grid, float wantX,
              float wantY, const char* direction) {
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
    UINT rows = 0;
    UINT64 rowSize = 0, total = 0;
    g.dev.p->GetCopyableFootprints(&desc, 0, 1, 0, &fp, &rows, &rowSize, &total);
    Com<ID3D12Resource> readback;
    if (!Committed(g.dev.p, D3D12_HEAP_TYPE_READBACK, BufferDesc(total),
                   D3D12_RESOURCE_STATE_COPY_DEST, &readback.p)) {
        return false;
    }
    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = tex;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = readback.p;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = fp;
    D3D12_RESOURCE_BARRIER b = Transition(tex, D3D12_RESOURCE_STATE_COMMON,
                                          D3D12_RESOURCE_STATE_COPY_SOURCE);
    g.list.p->ResourceBarrier(1, &b);
    g.list.p->CopyTextureRegion(&dst, 0, 0, 0, &src, NULL);
    b = Transition(tex, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    g.list.p->ResourceBarrier(1, &b);
    if (!g.Submit()) return false;
    unsigned char* mapped = NULL;
    D3D12_RANGE whole = {0, (SIZE_T)total};
    if (FAILED(readback.p->Map(0, &whole, reinterpret_cast<void**>(&mapped)))) return false;
    const bool right = CheckFlow(kLabel, direction, mapped + fp.Offset, fp.Footprint.RowPitch,
                                 (int)desc.Width, (int)desc.Height, grid, wantX, wantY);
    D3D12_RANGE none = {0, 0};
    readback.p->Unmap(0, &none);
    return right;
}

// The first format the engine reports for a usage, or fallback.
DXGI_FORMAT FirstFormat(NV_OF_D3D12_API_FUNCTION_LIST& of, NvOFHandle h, NV_OF_BUFFER_USAGE usage,
                        DXGI_FORMAT fallback) {
    uint32_t count = 0;
    if (of.nvOFGetSurfaceFormatCountD3D12(h, usage, NV_OF_MODE_OPTICALFLOW, &count) !=
            NV_OF_SUCCESS || count == 0 || count > 32) {
        return fallback;
    }
    DXGI_FORMAT formats[32] = {};
    if (of.nvOFGetSurfaceFormatD3D12(h, usage, NV_OF_MODE_OPTICALFLOW, formats) != NV_OF_SUCCESS) {
        return fallback;
    }
    return formats[0];
}

}  // namespace

void RunLoopD3D12(IDXGIAdapter* adapter, const LoopFrames& frames, int seconds, int rate,
                  bool forwardOnly) {
    const int grid = 4;
    HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
    CreateDeviceFn createDevice =
        d3d12 ? reinterpret_cast<CreateDeviceFn>(GetProcAddress(d3d12, "D3D12CreateDevice")) : NULL;
    HMODULE nvof = LoadLibraryW(L"nvofapi64.dll");
    CreateInstanceFn createInstance =
        nvof ? reinterpret_cast<CreateInstanceFn>(GetProcAddress(nvof, "NvOFAPICreateInstanceD3D12"))
             : NULL;
    if (!createDevice || !createInstance) {
        LOG("ofprobe: %s: RESULT %s not found", kLabel,
            createDevice ? "the engine's D3D12 entry point" : "D3D12CreateDevice");
        return;
    }

    Gpu g;
    if (FAILED(createDevice(adapter, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), g.dev.out()))) {
        LOG("ofprobe: %s: RESULT no D3D12 device", kLabel);
        return;
    }
    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    g.event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (FAILED(g.dev.p->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), g.queue.out())) ||
        FAILED(g.dev.p->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                               __uuidof(ID3D12CommandAllocator), g.alloc.out())) ||
        FAILED(g.dev.p->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.alloc.p, NULL,
                                          __uuidof(ID3D12GraphicsCommandList), g.list.out())) ||
        FAILED(g.dev.p->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence),
                                    g.fence.out())) ||
        !g.event) {
        LOG("ofprobe: %s: RESULT the queue, list or fence could not be created", kLabel);
        return;
    }

    NV_OF_D3D12_API_FUNCTION_LIST of = {};
    NV_OF_STATUS st = createInstance(NV_OF_API_VERSION, &of);
    if (st != NV_OF_SUCCESS) {
        LOG("ofprobe: %s: RESULT the engine's D3D12 interface refused (status %d)", kLabel, (int)st);
        return;
    }
    struct Session {
        NV_OF_D3D12_API_FUNCTION_LIST* of = nullptr;
        NvOFHandle handle = NULL;
        NvOFGPUBufferHandle buffers[6] = {};
        ~Session() {
            if (!of) return;
            for (NvOFGPUBufferHandle b : buffers) {
                if (!b) continue;
                NV_OF_UNREGISTER_RESOURCE_PARAMS_D3D12 p = {};
                p.hOFGpuBuffer = b;
                of->nvOFUnregisterResourceD3D12(&p);
            }
            if (handle) of->nvOFDestroy(handle);
        }
    } s;
    s.of = &of;
    st = of.nvCreateOpticalFlowD3D12(g.dev.p, &s.handle);
    if (st != NV_OF_SUCCESS) {
        LOG("ofprobe: %s: RESULT no optical flow session (status %d)", kLabel, (int)st);
        return;
    }
    NV_OF_INIT_PARAMS init = {};
    init.width = (uint32_t)frames.width;
    init.height = (uint32_t)frames.height;
    init.outGridSize = NV_OF_OUTPUT_VECTOR_GRID_SIZE_4;
    init.mode = NV_OF_MODE_OPTICALFLOW;
    init.perfLevel = NV_OF_PERF_LEVEL_MEDIUM;
    init.enableOutputCost = NV_OF_TRUE;
    init.predDirection = forwardOnly ? NV_OF_PRED_DIRECTION_FORWARD : NV_OF_PRED_DIRECTION_BOTH;
    init.inputBufferFormat = NV_OF_BUFFER_FORMAT_GRAYSCALE8;
    st = of.nvOFInit(s.handle, &init);
    if (st != NV_OF_SUCCESS) {
        LOG("ofprobe: %s: RESULT init refused (status %d)", kLabel, (int)st);
        return;
    }
    const DXGI_FORMAT flowFormat =
        FirstFormat(of, s.handle, NV_OF_BUFFER_USAGE_OUTPUT, DXGI_FORMAT_R16G16_SINT);
    const DXGI_FORMAT costFormat =
        FirstFormat(of, s.handle, NV_OF_BUFFER_USAGE_COST, DXGI_FORMAT_R32_UINT);
    LOG("ofprobe: %s: flow output format %d, cost format %d (DXGI)", kLabel, (int)flowFormat,
        (int)costFormat);

    const UINT cellsX = (UINT)((frames.width + grid - 1) / grid);
    const UINT cellsY = (UINT)((frames.height + grid - 1) / grid);
    const D3D12_RESOURCE_DESC inputDesc =
        TextureDesc((UINT)frames.width, (UINT)frames.height, DXGI_FORMAT_R8_UNORM);
    const D3D12_RESOURCE_DESC flowDesc = TextureDesc(cellsX, cellsY, flowFormat);
    const D3D12_RESOURCE_DESC costDesc = TextureDesc(cellsX, cellsY, costFormat);
    Com<ID3D12Resource> before, after, fwd, bwd, fwdCost, bwdCost;
    struct Res { Com<ID3D12Resource>* res; const D3D12_RESOURCE_DESC* desc; const char* what; };
    const Res all[6] = {
        {&before, &inputDesc, "before frame"}, {&after, &inputDesc, "after frame"},
        {&fwd, &flowDesc, "forward flow"},      {&bwd, &flowDesc, "backward flow"},
        {&fwdCost, &costDesc, "forward cost"},  {&bwdCost, &costDesc, "backward cost"},
    };
    // Without the backward flow, its field and cost (3 and 5) are neither made nor registered.
    auto used = [&](int i) { return !forwardOnly || (i != 3 && i != 5); };
    for (int i = 0; i < 6; i++) {
        if (!used(i)) continue;
        const Res& r = all[i];
        if (!Committed(g.dev.p, D3D12_HEAP_TYPE_DEFAULT, *r.desc, D3D12_RESOURCE_STATE_COMMON,
                       &r.res->p)) {
            LOG("ofprobe: %s: RESULT the %s could not be created", kLabel, r.what);
            return;
        }
    }
    if (!Upload(g, before.p, inputDesc, frames.before.data(), (UINT)frames.width) ||
        !Upload(g, after.p, inputDesc, frames.after.data(), (UINT)frames.width)) {
        LOG("ofprobe: %s: RESULT the frames could not be uploaded", kLabel);
        return;
    }
    for (int i = 0; i < 6; i++) {
        if (!used(i)) continue;
        NV_OF_REGISTER_RESOURCE_PARAMS_D3D12 p = {};
        p.resource = all[i].res->p;
        p.inputFencePoint.fence = g.fence.p;
        p.inputFencePoint.value = g.value;
        p.hOFGpuBuffer = &s.buffers[i];
        p.outputFencePoint.fence = g.fence.p;
        p.outputFencePoint.value = ++g.value;
        st = of.nvOFRegisterResourceD3D12(s.handle, &p);
        if (st != NV_OF_SUCCESS || !g.WaitFor(g.value)) {
            LOG("ofprobe: %s: RESULT the %s was not registered (status %d)", kLabel, all[i].what,
                (int)st);
            return;
        }
    }

    // One flow: wait for nothing new, set the fence when done, and wait for it on the CPU.
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    auto flow = [&](double* returnMs, double* doneMs) {
        NV_OF_FENCE_POINT ready = {g.fence.p, g.value};
        NV_OF_FENCE_POINT done = {g.fence.p, g.value + 1};
        NV_OF_EXECUTE_INPUT_PARAMS_D3D12 in = {};
        in.inputFrame = s.buffers[0];
        in.referenceFrame = s.buffers[1];
        in.disableTemporalHints = NV_OF_FALSE;
        in.numFencePoints = 1;
        in.fencePoint = &ready;
        NV_OF_EXECUTE_OUTPUT_PARAMS_D3D12 out = {};
        out.outputBuffer = s.buffers[2];
        out.bwdOutputBuffer = s.buffers[3];
        out.outputCostBuffer = s.buffers[4];
        out.bwdOutputCostBuffer = s.buffers[5];
        out.fencePoint = &done;
        LARGE_INTEGER c0, c1, c2;
        QueryPerformanceCounter(&c0);
        if (of.nvOFExecuteD3D12(s.handle, &in, &out) != NV_OF_SUCCESS) return false;
        QueryPerformanceCounter(&c1);
        g.value++;
        if (!g.WaitFor(g.value)) return false;
        QueryPerformanceCounter(&c2);
        *returnMs = 1000.0 * (double)(c1.QuadPart - c0.QuadPart) / (double)freq.QuadPart;
        *doneMs = 1000.0 * (double)(c2.QuadPart - c0.QuadPart) / (double)freq.QuadPart;
        return true;
    };

    for (int i = 0; i < 5; i++) {
        double r = 0.0, d = 0.0;
        if (!flow(&r, &d)) {
            LOG("ofprobe: %s: RESULT a warm-up flow failed or did not finish", kLabel);
            return;
        }
    }
    const bool fwdRight = ReadBack(g, fwd.p, flowDesc, grid, (float)frames.moveX,
                                   (float)frames.moveY, "forward");
    const bool bwdRight = forwardOnly || ReadBack(g, bwd.p, flowDesc, grid, -(float)frames.moveX,
                                                  -(float)frames.moveY, "backward");
    if (!fwdRight || !bwdRight) {
        LOG("ofprobe: %s: RESULT the flow is wrong; no loop", kLabel);
        return;
    }
    LOG("ofprobe: %s loop: %d s at %d flows a second, R8 as GRAYSCALE8 input, grid 4 MEDIUM %s, "
        "cost on, the end read from a fence", kLabel, seconds, rate,
        forwardOnly ? "forward only" : "both ways");
    RunPaced(kLabel, seconds, rate, flow);
}

}  // namespace flowprobe
