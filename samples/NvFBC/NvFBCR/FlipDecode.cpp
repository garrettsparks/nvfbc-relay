#include "FlipDecode.h"

#include <cstring>

namespace flipdecode {

const GUID kNvDisplayDriverGuid =
    { 0xae4f8626, 0x8265, 0x40d1, { 0xa7, 0x0b, 0x11, 0xb6, 0x42, 0x40, 0xe8, 0xe9 } };
const USHORT    kFlipRequestId      = 1;
const UCHAR     kFlipRequestLevel   = 4;
const ULONGLONG kFlipRequestKeyword = 0x1000000000000000ull;

const GUID kDxgKrnlGuid =
    { 0x802ec45a, 0x1e99, 0x4b83, { 0x99, 0x20, 0x87, 0xc9, 0x82, 0x77, 0xba, 0x9d } };

// Largest vidPnSourceId treated as plausible. Two heads are in use on the measured rig
// (the gaming display and the relay's own output); a decode claiming a wild head number
// is a misparse, not a new monitor.
static const uint32_t kMaxPlausibleHead = 8;

bool IsFlipRequest(PEVENT_RECORD ev) {
    return IsEqualGUID(ev->EventHeader.ProviderId, kNvDisplayDriverGuid) &&
           ev->EventHeader.EventDescriptor.Id == kFlipRequestId;
}

// Layout recovered from real payloads rather than guessed: a 44-byte record where the u64
// at +16 is the proposed flip time. It is identified positively, not by elimination - its
// per-head deltas land on the 8.33 ms half-period that a 60 fps source with 2x frame
// generation produces, and on the same trace those deltas agreed with NvFBC's independent
// arrival spacing to about 100 us.
//
// Guarded three ways, because a silently wrong timestamp is worse than no timestamp: the
// payload must be large enough, the descriptor version must be the one this layout came
// from, and the decoded flip time must land near the event that announced it. A misparse
// reads part of a handle or a zero run and lands astronomically outside that window.
static bool DecodePositional(PEVENT_RECORD ev, int64_t qpcFreq, FlipEvent* out) {
    if (ev->UserDataLength < 32) return false;
    if (ev->EventHeader.EventDescriptor.Version != 0) return false;

    const BYTE* p = (const BYTE*)ev->UserData;
    uint64_t alloc = 0, ts = 0;
    uint32_t head = 0, token = 0;
    memcpy(&alloc, p +  0, sizeof(alloc));
    memcpy(&ts,    p + 16, sizeof(ts));
    memcpy(&head,  p + 24, sizeof(head));
    memcpy(&token, p + 28, sizeof(token));

    const int64_t evt = ev->EventHeader.TimeStamp.QuadPart;
    const int64_t diff = (int64_t)ts - evt;
    if (qpcFreq > 0 && (diff > qpcFreq || diff < -qpcFreq)) return false;
    if (head >= kMaxPlausibleHead) return false;

    out->alloc = alloc;
    out->displayQpc = ts;
    out->head = head;
    out->token = token;
    out->eventQpc = evt;
    return true;
}

bool DecodeFlip(PEVENT_RECORD ev, int64_t qpcFreq, FlipEvent* out) {
    *out = FlipEvent{};
    out->eventQpc = ev->EventHeader.TimeStamp.QuadPart;
    return DecodePositional(ev, qpcFreq, out);
}

}  // namespace flipdecode
