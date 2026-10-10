#pragma once

#include <windows.h>

#include "TemporalPolicy.h"

// What the capture ring needs from whatever delivers the source display's frames.
//
// The ring owns time and decisions: the arrival stamp of every wake, which wakes make a batch,
// which member of a batch is kept, and the slot a frame goes in. A capture source owns the
// pixels: how the next frame is waited for, and how its picture gets to where the present can
// read it. Everything here is called on the capture thread.
class ICaptureSource {
public:
    virtual ~ICaptureSource() {}

    // Logged once, before the first wait.
    virtual void LogStart() = 0;

    // Two wakes closer together than this are one batch, the members of a frame-generation
    // pair. It differs by source because each hands a pair's members over at its own spacing.
    virtual LONGLONG BatchWindowQpc() const = 0;

    enum class Wake {
        Frame,     // a new frame is in hand
        Timeout,   // the wait ran out and nothing new came
        Failed,    // this wait failed; the ring waits again
        Lost,      // the session is gone; capture stops
    };
    // Blocks until the next frame arrives or the source's own wait runs out. *arrived is the
    // moment the wait returned, which the ring stamps a frame with.
    virtual Wake Wait(LARGE_INTEGER* arrived) = 0;

    // Where the frame in hand goes, asked once its place in a batch is known.
    struct Placement {
        // Its picture is not put in the slot now: the slot is published without pixels and
        // the source fills it later, or never if the frame turns out not to be needed.
        bool waits = false;
        // A slot left waiting at an earlier wake that this frame makes needless.
        int replacedSlot = -1;
    };
    virtual Placement Place(const policy::BatchDecision& batch) = 0;

    // Puts the frame in hand into a ring slot and returns once the present may read it. Not
    // called for a frame Place left waiting.
    virtual void Store(int slot) = 0;

    // The wake is published in `slot` as capture number `count`. kept is false for a member
    // the keep decision dropped at once.
    virtual void Published(int slot, long long count, const Placement& placement, bool kept) = 0;

    // Logged once when capture ends.
    virtual void LogSummary(long long wakesStored) = 0;
};
