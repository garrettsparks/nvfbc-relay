#include "FrameMarker.h"
#include <SimpleLogger.h>

// Raw A2R10G10B10 texel values for the staging strip (the backbuffer format). Pure
// black/white luma: maximum separation through encode chains, no quantization level
// between them reads ambiguous.
static const DWORD kTexelWhite = 0xFFFFFFFF;
static const DWORD kTexelBlack = 0xC0000000;

// D3DCOLOR (8-bit ARGB) equivalents for the ColorFill fallback; the driver converts
// to the render-target format.
static const D3DCOLOR kFillWhite = D3DCOLOR_ARGB(255, 255, 255, 255);
static const D3DCOLOR kFillBlack = D3DCOLOR_ARGB(255, 0, 0, 0);

// 4-bit XOR fold of the payload nibbles (the marker's misread detector; monotonicity
// of the counter is the second layer, applied by the decoder).
static unsigned int ChecksumNibbles(unsigned long long payload) {
    payload ^= payload >> 32;
    payload ^= payload >> 16;
    payload ^= payload >> 8;
    payload ^= payload >> 4;
    return (unsigned int)(payload & 0xF);
}

// This build writes extension schema 0: core cells only. A future schema bumps this
// and appends its extension cells; the core cells above never move.
static const unsigned long long kExtSchema = 0;

// Cell values for one frame. The payload is cells 1-39 packed LSB-first: counter in
// bits 0-23, interp flag bit 24, weight bits 25-28, compositor bits 29-30, synthesis
// executor bits 31-32, pick code bits 33-35, extension schema bits 36-38. The
// checksum covers exactly that span.
static void BuildCells(unsigned int counter, int pickCode, int weightQ, bool interp,
                       int compositorId, int execCode, bool cells[/*kCells*/]) {
    unsigned long long payload = counter & 0xFFFFFFull;
    if (interp) payload |= 1ull << 24;
    payload |= (unsigned long long)(weightQ & 0xF) << 25;
    payload |= (unsigned long long)(compositorId & 0x3) << 29;
    payload |= (unsigned long long)(execCode & 0x3) << 31;
    payload |= (unsigned long long)(pickCode & 0x7) << 33;
    payload |= (kExtSchema & 0x7) << 36;
    cells[0] = true;
    for (int b = 0; b < 39; b++) {
        cells[1 + b] = ((payload >> b) & 1) != 0;
    }
    const unsigned int checksum = ChecksumNibbles(payload);
    for (int b = 0; b < 4; b++) {
        cells[40 + b] = ((checksum >> b) & 1) != 0;
    }
}

FrameMarker::FrameMarker()
    : m_device(NULL)
    , m_sysmem(NULL)
    , m_vram(NULL)
    , m_counter(0)
    , m_maxFrames(0)
    , m_active(false)
    , m_blitPathOk(true)
{
    m_destRect.left = m_destRect.top = m_destRect.right = m_destRect.bottom = 0;
}

FrameMarker::~FrameMarker() {
    if (m_sysmem) m_sysmem->Release();
    if (m_vram) m_vram->Release();
}

bool FrameMarker::Init(IDirect3DDevice9Ex* device, int bufWidth, int bufHeight, unsigned int maxFrames) {
    m_device = device;
    m_maxFrames = maxFrames;

    // The marker region scales off the WIDTH alone (square cells), so the decoder can
    // derive the full geometry from any recording's width. MulDiv rather than a
    // truncated per-cell pixel size: the strip must end at the exact fraction the
    // decoder computes, or cell centers drift across the strip at resolutions where
    // width/kCellsPerWidth is fractional.
    m_destRect.left = 0;
    m_destRect.top = 0;
    m_destRect.right = MulDiv(bufWidth, kGridW, kCellsPerWidth);
    m_destRect.bottom = MulDiv(bufWidth, kGridH, kCellsPerWidth);
    if (m_destRect.bottom > bufHeight) m_destRect.bottom = bufHeight;

    HRESULT hr = device->CreateOffscreenPlainSurface(
        kGridW * kTexelsPerCell, kGridH * kTexelsPerCell,
        D3DFMT_A2R10G10B10, D3DPOOL_SYSTEMMEM, &m_sysmem, NULL);
    if (SUCCEEDED(hr)) {
        hr = device->CreateOffscreenPlainSurface(
            kGridW * kTexelsPerCell, kGridH * kTexelsPerCell,
            D3DFMT_A2R10G10B10, D3DPOOL_DEFAULT, &m_vram, NULL);
    }
    if (FAILED(hr)) {
        LOGERR("marker: surface creation failed (hr=0x%08lx) - running unmarked", (unsigned long)hr);
        if (m_sysmem) { m_sysmem->Release(); m_sysmem = NULL; }
        if (m_vram) { m_vram->Release(); m_vram = NULL; }
        m_active = false;
        return false;
    }

    m_active = true;
    if (m_maxFrames)
        LOG("Frame marker ACTIVE (-mark %u): %d cells, %ldx%ld px strip at top-left; burning the first %u presents, mark= on the temporal line",
            m_maxFrames, kCells, m_destRect.right, m_destRect.bottom, m_maxFrames);
    else
        LOG("Frame marker ACTIVE (-mark): %d cells, %ldx%ld px strip at top-left; mark= on the temporal line",
            kCells, m_destRect.right, m_destRect.bottom);
    return true;
}

bool FrameMarker::BurnBlit(IDirect3DSurface9* backbuffer, const bool cells[kCells]) {
    D3DLOCKED_RECT locked;
    HRESULT hr = m_sysmem->LockRect(&locked, NULL, 0);
    if (FAILED(hr)) return false;
    for (int y = 0; y < kGridH * kTexelsPerCell; y++) {
        DWORD* row = (DWORD*)((BYTE*)locked.pBits + y * locked.Pitch);
        const bool cellRow = (y / kTexelsPerCell) == 1;
        for (int x = 0; x < kGridW * kTexelsPerCell; x++) {
            const int cx = x / kTexelsPerCell;
            const bool white = cellRow && (cx >= 1) && (cx <= kCells) && cells[cx - 1];
            row[x] = white ? kTexelWhite : kTexelBlack;
        }
    }
    m_sysmem->UnlockRect();

    hr = m_device->UpdateSurface(m_sysmem, NULL, m_vram, NULL);
    if (SUCCEEDED(hr)) {
        hr = m_device->StretchRect(m_vram, NULL, backbuffer, &m_destRect, D3DTEXF_POINT);
    }
    return SUCCEEDED(hr);
}

bool FrameMarker::BurnColorFill(IDirect3DSurface9* backbuffer, const bool cells[kCells]) {
    HRESULT hr = m_device->ColorFill(backbuffer, &m_destRect, kFillBlack);
    if (FAILED(hr)) return false;
    const int destW = m_destRect.right - m_destRect.left;
    const int destH = m_destRect.bottom - m_destRect.top;
    for (int i = 0; i < kCells; i++) {
        if (!cells[i]) continue;
        RECT cell;
        cell.left = m_destRect.left + MulDiv(destW, 1 + i, kGridW);
        cell.right = m_destRect.left + MulDiv(destW, 2 + i, kGridW);
        cell.top = m_destRect.top + MulDiv(destH, 1, kGridH);
        cell.bottom = m_destRect.top + MulDiv(destH, 2, kGridH);
        hr = m_device->ColorFill(backbuffer, &cell, kFillWhite);
        if (FAILED(hr)) return false;
    }
    return true;
}

unsigned int FrameMarker::Burn(IDirect3DSurface9* backbuffer, int pickCode, int weightQ,
                               bool interp, int compositorId, int execCode) {
    // The counter advances even when drawing is disabled or fails, so mark= in the
    // log stays a pure present count and a mid-run draw failure cannot shift the
    // video-to-log join for frames already recorded.
    const unsigned int burned = m_counter;
    m_counter = (m_counter + 1) & 0xFFFFFF;
    if (!m_active || !backbuffer) return burned;
    // -mark N: draw only the first N presents, then run clean; the counter above still
    // advanced so mark= stays a continuous present count for the whole session.
    if (m_maxFrames != 0 && burned >= m_maxFrames) return burned;

    bool cells[kCells];
    BuildCells(burned, pickCode, weightQ, interp, compositorId, execCode, cells);

    if (m_blitPathOk) {
        if (BurnBlit(backbuffer, cells)) return burned;
        m_blitPathOk = false;
        LOGERR("marker: blit path failed - falling back to per-cell ColorFill");
    }
    if (!BurnColorFill(backbuffer, cells)) {
        m_active = false;
        LOGERR("marker: ColorFill path failed - marker disabled, counter still logged");
    }
    return burned;
}
