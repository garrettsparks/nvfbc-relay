/*!
 * \brief Declarations for the parts of the NVIDIA frame buffer capture interface this project uses
 */

#pragma once

#include <windows.h>
#include <stddef.h>

// The NvFBC interface as this project uses it: three of the DLL's exports, the capture session
// that writes into Direct3D 9 surfaces, and the structs and constants they take. Written from the
// interface's names, values and layouts, for x64 only. The driver reads and writes these structs
// whole, so every field is declared even where nothing here touches it. Enums and constants name
// only the values this project uses. The size and offset checks at the end fail the build if an
// edit moves anything the driver depends on.

struct IDirect3DSurface9;

typedef unsigned long NvU32;

enum NVFBCRESULT : int
{
    NVFBC_SUCCESS = 0,
    NVFBC_ERROR_INVALIDATED_SESSION = -3,
};

enum NVFBC_STATE : int
{
    NVFBC_STATE_DISABLE = 0,
    NVFBC_STATE_ENABLE = 1,
};

// A struct's version word: its size in the low 16 bits, its revision above that, and the
// interface generation in the top byte.
constexpr NvU32 NvFBCStructVersion(size_t size, NvU32 revision)
{
    return (NvU32)size | (revision << 16) | (0x70u << 24);
}

struct NvFBCStatusEx
{
    NvU32 dwVersion;
    NvU32 bIsCapturePossible : 1;
    NvU32 bCurrentlyCapturing : 1;
    NvU32 bCanCreateNow : 1;
    NvU32 bSupportMultiHead : 1;
    NvU32 bSupportConfigurableDiffMap : 1;
    NvU32 bSupportImageClassification : 1;
    NvU32 bReservedBits : 26;
    NvU32 dwNvFBCVersion;
    NvU32 dwAdapterIdx;
    void *pPrivateData;
    NvU32 dwPrivateDataSize;
    NvU32 dwReserved[59];
    void *pReserved[31];
};

constexpr NvU32 NVFBC_STATUS_VER = NvFBCStructVersion(sizeof(NvFBCStatusEx), 2);

struct NvFBCCreateParams
{
    NvU32 dwVersion;
    NvU32 dwInterfaceType;
    NvU32 dwMaxDisplayWidth;
    NvU32 dwMaxDisplayHeight;
    void *pDevice;
    void *pPrivateData;
    NvU32 dwPrivateDataSize;
    NvU32 dwInterfaceVersion;
    void *pNvFBC;
    NvU32 dwAdapterIdx;
    NvU32 dwNvFBCVersion;
    void *cudaCtx;
    void *pPrivateData2;
    NvU32 dwPrivateData2Size;
    NvU32 dwReserved[55];
    void *pReserved[27];
};

constexpr NvU32 NVFBC_CREATE_PARAMS_VER = NvFBCStructVersion(sizeof(NvFBCCreateParams), 2);

// The DLL's exports, resolved at run time.
typedef NVFBCRESULT(__stdcall *NvFBC_CreateFunctionExType)(void *createParams);
typedef NVFBCRESULT(__stdcall *NvFBC_GetStatusExFunctionType)(void *status);
typedef NVFBCRESULT(__stdcall *NvFBC_EnableFunctionType)(NVFBC_STATE state);

// Interface type for a capture session that writes into Direct3D 9 surfaces.
constexpr NvU32 NVFBC_TO_DX9_VID = 0x2003;

enum NVFBCToDx9VidBufferFormat : int
{
    NVFBC_TODX9VID_ARGB10 = 2,
};

enum NVFBCToDx9VidGrabMode : int
{
    NVFBC_TODX9VID_SOURCEMODE_SCALE = 1,
};

enum NVFBCToDx9VidGrabFlags : int
{
    NVFBC_TODX9VID_NOWAIT = 0x1,
    NVFBC_TODX9VID_WAIT_WITH_TIMEOUT = 0x10,
};

struct NvFBCFrameGrabInfo
{
    DWORD dwWidth;
    DWORD dwHeight;
    DWORD dwBufferWidth;
    DWORD dwReserved;
    BOOL bOverlayActive;
    BOOL bMustRecreate;
    BOOL bFirstBuffer;
    BOOL bHWMouseVisible;
    BOOL bProtectedContent;
    DWORD dwDriverInternalError;
    BOOL bStereoOn;
    BOOL bIGPUCapture;
    DWORD dwSourcePID;
    DWORD dwReserved3;
    DWORD bIsHDR : 1;
    DWORD bReservedBit1 : 1;
    DWORD bReservedBits : 30;
    DWORD dwWaitModeUsed;
    NvU32 dwReserved2[11];
};

struct NVFBC_TODX9VID_OUT_BUF
{
    IDirect3DSurface9 *pPrimary;
    IDirect3DSurface9 *pSecondary;
};

struct NVFBC_TODX9VID_SETUP_PARAMS
{
    NvU32 dwVersion;
    NvU32 bWithHWCursor : 1;
    NvU32 bStereoGrab : 1;
    NvU32 bDiffMap : 1;
    NvU32 bEnableSeparateCursorCapture : 1;
    NvU32 bHDRRequest : 1;
    NvU32 bClassificationMap : 1;
    NvU32 bReservedBits : 26;
    NVFBCToDx9VidBufferFormat eMode;
    NvU32 dwNumBuffers;
    int eDiffMapBlockSize;
    int eStereoFmt;
    NvU32 dwDiffMapBuffSize;
    NvU32 dwClassificationMapBuffSize;
    NvU32 dwClassificationMapStampWidth;
    NvU32 dwClassificationMapStampHeight;
    void **ppDiffMap;
    void **ppClassificationMap;
    NVFBC_TODX9VID_OUT_BUF *ppBuffer;
    void *hCursorCaptureEvent;
    NvU32 dwReserved[22];
    void *pReserved[12];
};

constexpr NvU32 NVFBC_TODX9VID_SETUP_PARAMS_V3_VER =
    NvFBCStructVersion(sizeof(NVFBC_TODX9VID_SETUP_PARAMS), 3);

struct NVFBC_TODX9VID_GRAB_FRAME_PARAMS
{
    NvU32 dwVersion;
    NvU32 dwFlags;
    NvU32 dwTargetWidth;
    NvU32 dwTargetHeight;
    NvU32 dwStartX;
    NvU32 dwStartY;
    NVFBCToDx9VidGrabMode eGMode;
    NvU32 dwBufferIdx;
    NvFBCFrameGrabInfo *pNvFBCFrameGrabInfo;
    NvU32 dwWaitTime;
    NvU32 dwReserved[23];
    void *pReserved[15];
};

constexpr NvU32 NVFBC_TODX9VID_GRAB_FRAME_PARAMS_V1_VER =
    NvFBCStructVersion(sizeof(NVFBC_TODX9VID_GRAB_FRAME_PARAMS), 1);

// The session NvFBC_CreateEx returns for NVFBC_TO_DX9_VID. The driver implements it and callers
// reach it through its virtual table, so these methods must stay first and in this order. The
// driver's fifth method, cursor capture, is not declared. Release ends the session; never delete
// the object.
class NvFBCToDx9Vid
{
public:
    virtual NVFBCRESULT __stdcall NvFBCToDx9VidSetUp(NVFBC_TODX9VID_SETUP_PARAMS *params) = 0;
    virtual NVFBCRESULT __stdcall NvFBCToDx9VidGrabFrame(NVFBC_TODX9VID_GRAB_FRAME_PARAMS *params) = 0;
    virtual NVFBCRESULT __stdcall NvFBCToDx9VidGPUBasedCPUSleep(long long microseconds) = 0;
    virtual NVFBCRESULT __stdcall NvFBCToDx9VidRelease() = 0;
};

// The x64 layout the driver expects: every size, every offset of a field this project touches,
// and every version word. None of these numbers may change.
static_assert(sizeof(void *) == 8, "NvFBC declarations are x64 only");
static_assert(sizeof(NvU32) == 4, "NvU32 size");
static_assert(sizeof(NVFBCRESULT) == 4, "NVFBCRESULT size");
static_assert(sizeof(NVFBC_STATE) == 4, "NVFBC_STATE size");
static_assert(sizeof(NvFBCToDx9Vid) == 8, "NvFBCToDx9Vid size");

static_assert(sizeof(NvFBCStatusEx) == 512, "NvFBCStatusEx size");
static_assert(offsetof(NvFBCStatusEx, dwVersion) == 0, "NvFBCStatusEx::dwVersion");
static_assert(offsetof(NvFBCStatusEx, dwNvFBCVersion) == 8, "NvFBCStatusEx::dwNvFBCVersion");
static_assert(offsetof(NvFBCStatusEx, dwAdapterIdx) == 12, "NvFBCStatusEx::dwAdapterIdx");
static_assert(NVFBC_STATUS_VER == 0x70020200, "NVFBC_STATUS_VER");

static_assert(sizeof(NvFBCCreateParams) == 512, "NvFBCCreateParams size");
static_assert(offsetof(NvFBCCreateParams, dwVersion) == 0, "NvFBCCreateParams::dwVersion");
static_assert(offsetof(NvFBCCreateParams, dwInterfaceType) == 4, "NvFBCCreateParams::dwInterfaceType");
static_assert(offsetof(NvFBCCreateParams, dwMaxDisplayWidth) == 8, "NvFBCCreateParams::dwMaxDisplayWidth");
static_assert(offsetof(NvFBCCreateParams, dwMaxDisplayHeight) == 12, "NvFBCCreateParams::dwMaxDisplayHeight");
static_assert(offsetof(NvFBCCreateParams, pDevice) == 16, "NvFBCCreateParams::pDevice");
static_assert(offsetof(NvFBCCreateParams, pNvFBC) == 40, "NvFBCCreateParams::pNvFBC");
static_assert(offsetof(NvFBCCreateParams, dwAdapterIdx) == 48, "NvFBCCreateParams::dwAdapterIdx");
static_assert(NVFBC_CREATE_PARAMS_VER == 0x70020200, "NVFBC_CREATE_PARAMS_VER");

static_assert(sizeof(NvFBCFrameGrabInfo) == 108, "NvFBCFrameGrabInfo size");

static_assert(sizeof(NVFBC_TODX9VID_OUT_BUF) == 16, "NVFBC_TODX9VID_OUT_BUF size");
static_assert(offsetof(NVFBC_TODX9VID_OUT_BUF, pPrimary) == 0, "NVFBC_TODX9VID_OUT_BUF::pPrimary");

static_assert(sizeof(NVFBC_TODX9VID_SETUP_PARAMS) == 256, "NVFBC_TODX9VID_SETUP_PARAMS size");
static_assert(offsetof(NVFBC_TODX9VID_SETUP_PARAMS, dwVersion) == 0, "NVFBC_TODX9VID_SETUP_PARAMS::dwVersion");
static_assert(offsetof(NVFBC_TODX9VID_SETUP_PARAMS, eMode) == 8, "NVFBC_TODX9VID_SETUP_PARAMS::eMode");
static_assert(offsetof(NVFBC_TODX9VID_SETUP_PARAMS, dwNumBuffers) == 12, "NVFBC_TODX9VID_SETUP_PARAMS::dwNumBuffers");
static_assert(offsetof(NVFBC_TODX9VID_SETUP_PARAMS, ppBuffer) == 56, "NVFBC_TODX9VID_SETUP_PARAMS::ppBuffer");
static_assert(NVFBC_TODX9VID_SETUP_PARAMS_V3_VER == 0x70030100, "NVFBC_TODX9VID_SETUP_PARAMS_V3_VER");

static_assert(sizeof(NVFBC_TODX9VID_GRAB_FRAME_PARAMS) == 256, "NVFBC_TODX9VID_GRAB_FRAME_PARAMS size");
static_assert(offsetof(NVFBC_TODX9VID_GRAB_FRAME_PARAMS, dwVersion) == 0, "NVFBC_TODX9VID_GRAB_FRAME_PARAMS::dwVersion");
static_assert(offsetof(NVFBC_TODX9VID_GRAB_FRAME_PARAMS, dwFlags) == 4, "NVFBC_TODX9VID_GRAB_FRAME_PARAMS::dwFlags");
static_assert(offsetof(NVFBC_TODX9VID_GRAB_FRAME_PARAMS, dwTargetWidth) == 8, "NVFBC_TODX9VID_GRAB_FRAME_PARAMS::dwTargetWidth");
static_assert(offsetof(NVFBC_TODX9VID_GRAB_FRAME_PARAMS, dwTargetHeight) == 12, "NVFBC_TODX9VID_GRAB_FRAME_PARAMS::dwTargetHeight");
static_assert(offsetof(NVFBC_TODX9VID_GRAB_FRAME_PARAMS, eGMode) == 24, "NVFBC_TODX9VID_GRAB_FRAME_PARAMS::eGMode");
static_assert(offsetof(NVFBC_TODX9VID_GRAB_FRAME_PARAMS, pNvFBCFrameGrabInfo) == 32, "NVFBC_TODX9VID_GRAB_FRAME_PARAMS::pNvFBCFrameGrabInfo");
static_assert(offsetof(NVFBC_TODX9VID_GRAB_FRAME_PARAMS, dwWaitTime) == 40, "NVFBC_TODX9VID_GRAB_FRAME_PARAMS::dwWaitTime");
static_assert(NVFBC_TODX9VID_GRAB_FRAME_PARAMS_V1_VER == 0x70010100, "NVFBC_TODX9VID_GRAB_FRAME_PARAMS_V1_VER");
