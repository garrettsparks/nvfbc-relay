// Checks NvFBCApi.h against NVIDIA's own NvFBC headers. At compile time: every struct's size, every
// field's offset, every enum value and every version constant. At run time: every bitfield's
// position, and the session's method order, by calling an object built from NVIDIA's declaration
// through ours, the way the relay calls the driver's. CI builds and runs it while NVIDIA's headers
// are in the tree.
//
// Two controls prove the checks are live. Built with NVFBC_ABI_CHECK_CONTROL_STATIC, a false size
// check must stop the compile. Built with NVFBC_ABI_CHECK_CONTROL_RUNTIME, a mismatched pair of
// bits must be reported and fail the run.

#include <windows.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

// Ours sits in a namespace so both declarations of each name fit in one translation unit.
namespace ours
{
#include "NvFBCApi.h"
}

// NVIDIA's headers define the version constants and the interface type as macros with the same
// names as ours, so ours are read before those headers are included.
namespace ourConst
{
constexpr ours::NvU32 statusVer = ours::NVFBC_STATUS_VER;
constexpr ours::NvU32 createParamsVer = ours::NVFBC_CREATE_PARAMS_VER;
constexpr ours::NvU32 setupParamsVer = ours::NVFBC_TODX9VID_SETUP_PARAMS_V3_VER;
constexpr ours::NvU32 grabFrameParamsVer = ours::NVFBC_TODX9VID_GRAB_FRAME_PARAMS_V1_VER;
constexpr ours::NvU32 toDx9Vid = ours::NVFBC_TO_DX9_VID;
}

struct IDirect3DSurface9;
#include "NvFBC/nvFBC.h"
#include "NvFBC/nvFBCToDx9Vid.h"

#define SAME_SIZE(T) static_assert(sizeof(ours::T) == sizeof(::T), "size of " #T)
#define SAME_OFFSET(T, f) static_assert(offsetof(ours::T, f) == offsetof(::T, f), "offset of " #T "::" #f)
#define SAME_VALUE(n) static_assert((long long)ours::n == (long long)::n, "value of " #n)

SAME_SIZE(NvU32);
SAME_SIZE(NVFBCRESULT);
SAME_SIZE(NVFBC_STATE);
SAME_SIZE(NVFBCToDx9VidBufferFormat);
SAME_SIZE(NVFBCToDx9VidGrabMode);
SAME_SIZE(NVFBCToDx9VidGrabFlags);
SAME_SIZE(NvFBCToDx9Vid);

SAME_VALUE(NVFBC_SUCCESS);
SAME_VALUE(NVFBC_ERROR_INVALIDATED_SESSION);
SAME_VALUE(NVFBC_STATE_DISABLE);
SAME_VALUE(NVFBC_STATE_ENABLE);
SAME_VALUE(NVFBC_TODX9VID_ARGB10);
SAME_VALUE(NVFBC_TODX9VID_SOURCEMODE_SCALE);
SAME_VALUE(NVFBC_TODX9VID_NOWAIT);
SAME_VALUE(NVFBC_TODX9VID_WAIT_WITH_TIMEOUT);

static_assert(ourConst::statusVer == NVFBC_STATUS_VER, "NVFBC_STATUS_VER");
static_assert(ourConst::createParamsVer == NVFBC_CREATE_PARAMS_VER, "NVFBC_CREATE_PARAMS_VER");
static_assert(ourConst::setupParamsVer == NVFBC_TODX9VID_SETUP_PARAMS_V3_VER, "NVFBC_TODX9VID_SETUP_PARAMS_V3_VER");
static_assert(ourConst::grabFrameParamsVer == NVFBC_TODX9VID_GRAB_FRAME_PARAMS_V1_VER, "NVFBC_TODX9VID_GRAB_FRAME_PARAMS_V1_VER");
static_assert(ourConst::toDx9Vid == NVFBC_TO_DX9_VID, "NVFBC_TO_DX9_VID");

SAME_SIZE(NvFBCStatusEx);
SAME_OFFSET(NvFBCStatusEx, dwVersion);
SAME_OFFSET(NvFBCStatusEx, dwNvFBCVersion);
SAME_OFFSET(NvFBCStatusEx, dwAdapterIdx);
SAME_OFFSET(NvFBCStatusEx, pPrivateData);
SAME_OFFSET(NvFBCStatusEx, dwPrivateDataSize);
SAME_OFFSET(NvFBCStatusEx, dwReserved);
SAME_OFFSET(NvFBCStatusEx, pReserved);

SAME_SIZE(NvFBCCreateParams);
SAME_OFFSET(NvFBCCreateParams, dwVersion);
SAME_OFFSET(NvFBCCreateParams, dwInterfaceType);
SAME_OFFSET(NvFBCCreateParams, dwMaxDisplayWidth);
SAME_OFFSET(NvFBCCreateParams, dwMaxDisplayHeight);
SAME_OFFSET(NvFBCCreateParams, pDevice);
SAME_OFFSET(NvFBCCreateParams, pPrivateData);
SAME_OFFSET(NvFBCCreateParams, dwPrivateDataSize);
SAME_OFFSET(NvFBCCreateParams, dwInterfaceVersion);
SAME_OFFSET(NvFBCCreateParams, pNvFBC);
SAME_OFFSET(NvFBCCreateParams, dwAdapterIdx);
SAME_OFFSET(NvFBCCreateParams, dwNvFBCVersion);
SAME_OFFSET(NvFBCCreateParams, cudaCtx);
SAME_OFFSET(NvFBCCreateParams, pPrivateData2);
SAME_OFFSET(NvFBCCreateParams, dwPrivateData2Size);
SAME_OFFSET(NvFBCCreateParams, dwReserved);
SAME_OFFSET(NvFBCCreateParams, pReserved);

SAME_SIZE(NvFBCFrameGrabInfo);
SAME_OFFSET(NvFBCFrameGrabInfo, dwWidth);
SAME_OFFSET(NvFBCFrameGrabInfo, dwHeight);
SAME_OFFSET(NvFBCFrameGrabInfo, dwBufferWidth);
SAME_OFFSET(NvFBCFrameGrabInfo, dwReserved);
SAME_OFFSET(NvFBCFrameGrabInfo, bOverlayActive);
SAME_OFFSET(NvFBCFrameGrabInfo, bMustRecreate);
SAME_OFFSET(NvFBCFrameGrabInfo, bFirstBuffer);
SAME_OFFSET(NvFBCFrameGrabInfo, bHWMouseVisible);
SAME_OFFSET(NvFBCFrameGrabInfo, bProtectedContent);
SAME_OFFSET(NvFBCFrameGrabInfo, dwDriverInternalError);
SAME_OFFSET(NvFBCFrameGrabInfo, bStereoOn);
SAME_OFFSET(NvFBCFrameGrabInfo, bIGPUCapture);
SAME_OFFSET(NvFBCFrameGrabInfo, dwSourcePID);
SAME_OFFSET(NvFBCFrameGrabInfo, dwReserved3);
SAME_OFFSET(NvFBCFrameGrabInfo, dwWaitModeUsed);
SAME_OFFSET(NvFBCFrameGrabInfo, dwReserved2);

SAME_SIZE(NVFBC_TODX9VID_OUT_BUF);
SAME_OFFSET(NVFBC_TODX9VID_OUT_BUF, pPrimary);
SAME_OFFSET(NVFBC_TODX9VID_OUT_BUF, pSecondary);

SAME_SIZE(NVFBC_TODX9VID_SETUP_PARAMS);
SAME_OFFSET(NVFBC_TODX9VID_SETUP_PARAMS, dwVersion);
SAME_OFFSET(NVFBC_TODX9VID_SETUP_PARAMS, eMode);
SAME_OFFSET(NVFBC_TODX9VID_SETUP_PARAMS, dwNumBuffers);
SAME_OFFSET(NVFBC_TODX9VID_SETUP_PARAMS, eDiffMapBlockSize);
SAME_OFFSET(NVFBC_TODX9VID_SETUP_PARAMS, eStereoFmt);
SAME_OFFSET(NVFBC_TODX9VID_SETUP_PARAMS, dwDiffMapBuffSize);
SAME_OFFSET(NVFBC_TODX9VID_SETUP_PARAMS, dwClassificationMapBuffSize);
SAME_OFFSET(NVFBC_TODX9VID_SETUP_PARAMS, dwClassificationMapStampWidth);
SAME_OFFSET(NVFBC_TODX9VID_SETUP_PARAMS, dwClassificationMapStampHeight);
SAME_OFFSET(NVFBC_TODX9VID_SETUP_PARAMS, ppDiffMap);
SAME_OFFSET(NVFBC_TODX9VID_SETUP_PARAMS, ppClassificationMap);
SAME_OFFSET(NVFBC_TODX9VID_SETUP_PARAMS, ppBuffer);
SAME_OFFSET(NVFBC_TODX9VID_SETUP_PARAMS, hCursorCaptureEvent);
SAME_OFFSET(NVFBC_TODX9VID_SETUP_PARAMS, dwReserved);
SAME_OFFSET(NVFBC_TODX9VID_SETUP_PARAMS, pReserved);

SAME_SIZE(NVFBC_TODX9VID_GRAB_FRAME_PARAMS);
SAME_OFFSET(NVFBC_TODX9VID_GRAB_FRAME_PARAMS, dwVersion);
SAME_OFFSET(NVFBC_TODX9VID_GRAB_FRAME_PARAMS, dwFlags);
SAME_OFFSET(NVFBC_TODX9VID_GRAB_FRAME_PARAMS, dwTargetWidth);
SAME_OFFSET(NVFBC_TODX9VID_GRAB_FRAME_PARAMS, dwTargetHeight);
SAME_OFFSET(NVFBC_TODX9VID_GRAB_FRAME_PARAMS, dwStartX);
SAME_OFFSET(NVFBC_TODX9VID_GRAB_FRAME_PARAMS, dwStartY);
SAME_OFFSET(NVFBC_TODX9VID_GRAB_FRAME_PARAMS, eGMode);
SAME_OFFSET(NVFBC_TODX9VID_GRAB_FRAME_PARAMS, dwBufferIdx);
SAME_OFFSET(NVFBC_TODX9VID_GRAB_FRAME_PARAMS, pNvFBCFrameGrabInfo);
SAME_OFFSET(NVFBC_TODX9VID_GRAB_FRAME_PARAMS, dwWaitTime);
SAME_OFFSET(NVFBC_TODX9VID_GRAB_FRAME_PARAMS, dwReserved);
SAME_OFFSET(NVFBC_TODX9VID_GRAB_FRAME_PARAMS, pReserved);

#ifdef NVFBC_ABI_CHECK_CONTROL_STATIC
static_assert(sizeof(ours::NvFBCStatusEx) == sizeof(::NvFBCStatusEx) + 4,
              "control: a false size check, so this build must stop here");
#endif

static int passed = 0;
static int failed = 0;

static void Expect(bool ok, const char *what)
{
    if (ok)
    {
        ++passed;
        return;
    }
    ++failed;
    printf("MISMATCH %s\n", what);
}

// Sets the named bits in zeroed copies of both declarations and compares the bytes.
#define SAME_BITS(T, oursField, theirField, what) \
    do \
    { \
        ours::T a; \
        ::T b; \
        memset(&a, 0, sizeof a); \
        memset(&b, 0, sizeof b); \
        a.oursField = 1; \
        b.theirField = 1; \
        Expect(sizeof a == sizeof b && memcmp(&a, &b, sizeof a) == 0, what); \
    } while (0)
#define SAME_BIT(T, f) SAME_BITS(T, f, f, "bit " #T "::" #f)

// Stands in for the driver's session: built from NVIDIA's declaration, so its virtual table is
// laid out the way the driver's is. Each method answers with its own code.
class StandInSession : public ::NvFBCToDx9Vid
{
public:
    ::NVFBCRESULT NVFBCAPI NvFBCToDx9VidSetUp(::NVFBC_TODX9VID_SETUP_PARAMS *) override
    {
        return (::NVFBCRESULT)-16;
    }
    ::NVFBCRESULT NVFBCAPI NvFBCToDx9VidGrabFrame(::NVFBC_TODX9VID_GRAB_FRAME_PARAMS *) override
    {
        return (::NVFBCRESULT)-17;
    }
    ::NVFBCRESULT NVFBCAPI NvFBCToDx9VidGPUBasedCPUSleep(__int64) override
    {
        return (::NVFBCRESULT)-18;
    }
    ::NVFBCRESULT NVFBCAPI NvFBCToDx9VidRelease() override
    {
        return (::NVFBCRESULT)-19;
    }
    ::NVFBCRESULT NVFBCAPI NvFBCToDx9VidCursorCapture(::NVFBC_CURSOR_CAPTURE_PARAMS *) override
    {
        return (::NVFBCRESULT)-20;
    }
};

int main()
{
    SAME_BIT(NvFBCStatusEx, bIsCapturePossible);
    SAME_BIT(NvFBCStatusEx, bCurrentlyCapturing);
    SAME_BIT(NvFBCStatusEx, bCanCreateNow);
    SAME_BIT(NvFBCStatusEx, bSupportMultiHead);
    SAME_BIT(NvFBCStatusEx, bSupportConfigurableDiffMap);
    SAME_BIT(NvFBCStatusEx, bSupportImageClassification);

    SAME_BIT(NVFBC_TODX9VID_SETUP_PARAMS, bWithHWCursor);
    SAME_BIT(NVFBC_TODX9VID_SETUP_PARAMS, bStereoGrab);
    SAME_BIT(NVFBC_TODX9VID_SETUP_PARAMS, bDiffMap);
    SAME_BIT(NVFBC_TODX9VID_SETUP_PARAMS, bEnableSeparateCursorCapture);
    SAME_BIT(NVFBC_TODX9VID_SETUP_PARAMS, bHDRRequest);
    SAME_BIT(NVFBC_TODX9VID_SETUP_PARAMS, bClassificationMap);

    SAME_BIT(NvFBCFrameGrabInfo, bIsHDR);
    SAME_BIT(NvFBCFrameGrabInfo, bReservedBit1);

#ifdef NVFBC_ABI_CHECK_CONTROL_RUNTIME
    SAME_BITS(NvFBCStatusEx, bIsCapturePossible, bCurrentlyCapturing,
              "control: two different bits compared as one, so this must be reported");
#endif

    StandInSession standIn;
    ours::NvFBCToDx9Vid *session =
        reinterpret_cast<ours::NvFBCToDx9Vid *>(static_cast<::NvFBCToDx9Vid *>(&standIn));
    Expect(session->NvFBCToDx9VidSetUp(NULL) == -16, "method 1, NvFBCToDx9VidSetUp");
    Expect(session->NvFBCToDx9VidGrabFrame(NULL) == -17, "method 2, NvFBCToDx9VidGrabFrame");
    Expect(session->NvFBCToDx9VidGPUBasedCPUSleep(0) == -18, "method 3, NvFBCToDx9VidGPUBasedCPUSleep");
    Expect(session->NvFBCToDx9VidRelease() == -19, "method 4, NvFBCToDx9VidRelease");

    printf("NvFBC ABI check: sizes, offsets, values and versions agree at compile time; "
           "%d of %d run-time checks passed\n",
           passed, passed + failed);
    return failed ? 1 : 0;
}
