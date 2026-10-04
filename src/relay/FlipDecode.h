#pragma once

#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>
#include <cstdint>

// Reading the NVIDIA display driver's FlipRequest events: the proposed scanout time for
// every frame the driver puts on a head, including the ones frame generation produced.
// Shared by the standalone probe and the relay so there is exactly one description of the
// wire format; a driver that changes the payload should break both at once, loudly,
// rather than leaving one of them quietly decoding stale offsets.
//
// This layer is Windows-bound by construction (it consumes EVENT_RECORD). Anything the
// policy is meant to reason about belongs in a plain structure with no ETW types in it,
// so it stays simulatable without a capture.

namespace flipdecode {

// NVIDIA DisplayDriver. GUID and event descriptor come from the manifest PresentMon
// embeds (PresentData/ETW/NV_DD.h), cross-checked against its NVTraceConsumer.
extern const GUID      kNvDisplayDriverGuid;
extern const USHORT    kFlipRequestId;
extern const UCHAR     kFlipRequestLevel;
extern const ULONGLONG kFlipRequestKeyword;

// The DirectX graphics kernel, used only as a control: if its events arrive while
// NVIDIA's do not, the session works and the provider is the problem.
extern const GUID      kDxgKrnlGuid;

// One flip the driver intends to scan out.
struct FlipEvent {
    uint64_t alloc      = 0;
    uint64_t displayQpc = 0;   // proposed flip time, QPC ticks
    uint32_t head       = 0;   // vidPnSourceId: which display
    uint32_t token      = 0;
    int64_t  eventQpc   = 0;   // when the driver announced it, QPC ticks
};

// Decode one FlipRequest payload from the recovered wire layout.
//
// Decoding by field name via TDH is not an option and is not attempted: that resolves only
// when the provider registers a manifest, and NVIDIA's DisplayDriver provider does not. It
// is an internal diagnostic surface, which is why PresentMon carries a reverse-engineered
// copy of the schema rather than asking the system for it. Measured here: 10800 of 10800
// events failed to resolve on a current driver.
//
// qpcFreq is used to sanity-check the decoded timestamp, so pass the real one.
bool DecodeFlip(PEVENT_RECORD ev, int64_t qpcFreq, FlipEvent* out);

// True when the record is a FlipRequest from the NVIDIA provider.
bool IsFlipRequest(PEVENT_RECORD ev);

}  // namespace flipdecode
