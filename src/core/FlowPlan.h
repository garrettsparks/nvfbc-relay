#pragma once

// FLOW AHEAD OF THE PRESENT (docs/optical-flow-interpolation-spec.md, 4.2). D3D-free and
// header-only so the policy suite tests it as it ships.
//
// The optical-flow compositor needs a flow field for the bracket pair of every present it
// synthesizes. Computing it when that present comes puts the flow engine's whole latency on
// the present thread. But the relay presents a fixed lag behind capture, so the frames a
// present will bracket have arrived several presents before it, and the next presents'
// targets are known to within the lock's pull. The planner predicts which upcoming presents
// will synthesize and names the pairs to flow now, and keeps the finished fields in a small
// cache keyed by the pair's capture stamps, so a 60 into 60 hole whose presents share one
// pair flows it once and a present predicted to pass through costs no flow at all.

#include <cstdint>

namespace policy {

// Two captured frames, named by their capture stamps, before < after.
struct FlowPair {
    int64_t beforeTs = 0;
    int64_t afterTs = 0;
};

inline bool SamePair(const FlowPair& a, const FlowPair& b) {
    return a.beforeTs == b.beforeTs && a.afterTs == b.afterTs;
}

// Whether a present at target would synthesize, and from which pair: the two stamps around
// the target (stamps ascending, n of them), with neither inside gate of it. The prediction
// uses the bare passthrough threshold for gate; the composite's one-sided band only widens
// the passing side, so a present inside the band is predicted to synthesize and at worst
// flows a pair it does not use.
inline bool PredictSynthPair(const int64_t* stamps, int n, int64_t target, int64_t gate,
                             FlowPair* pair) {
    int after = -1;
    for (int i = 0; i < n; i++) {
        if (stamps[i] > target) {
            after = i;
            break;
        }
    }
    if (after <= 0) return false;  // no frame on one side: a hold, not a synthesis
    const int64_t before = stamps[after - 1];
    if (target - before < gate || stamps[after] - target < gate) return false;
    pair->beforeTs = before;
    pair->afterTs = stamps[after];
    return true;
}

// Temporal hints carry the previous execute's flow into the next. They help only when the
// two pairs are neighbours in time; on an unrelated pair they degrade the result.
inline bool FlowHintsUsable(const FlowPair& previous, const FlowPair& next) {
    return previous.afterTs == next.beforeTs;
}

const int kFlowSlots = 4;

// How a synthesized present got its flow, for the log's fl= field.
enum class FlowUse : int {
    Hit = 0,   // flowed ahead, on the prediction of an earlier present
    Own = 1,   // not predicted: this present submitted it
};

// The cache of flow fields. A slot's contents (the GPU textures) belong to the caller; the
// planner only says which slot holds which pair and which slot a new pair goes in.
class FlowPlanner {
public:
    // The slot holding pair, or -1.
    int Find(const FlowPair& pair) const {
        for (int i = 0; i < kFlowSlots; i++) {
            if (m_slots[i].valid && SamePair(m_slots[i].pair, pair)) return i;
        }
        return -1;
    }

    // Once per present, before the decision: the pairs of the upcoming targets that are
    // predicted to synthesize and are not cached yet, each given a slot. Writes at most
    // maxOut slot indices to outSlots (the caller flows them, in order) and returns how many.
    // A slot is never taken from a pair predicted or used at this present.
    int Plan(const int64_t* stamps, int n, const int64_t* targets, int targetCount,
             int64_t gate, int64_t present, int* outSlots, int maxOut) {
        int out = 0;
        for (int t = 0; t < targetCount && out < maxOut; t++) {
            FlowPair pair;
            if (!PredictSynthPair(stamps, n, targets[t], gate, &pair)) continue;
            const int found = Find(pair);
            if (found >= 0) {
                m_slots[found].lastTouch = present;
                continue;
            }
            const int slot = Take(present);
            if (slot < 0) break;
            Fill(slot, pair, present, true);
            outSlots[out++] = slot;
        }
        return out;
    }

    // At a synthesized present: the slot to read for pair. Returns the slot and sets
    // *submitted when the pair was not cached and the caller must flow it into that slot now
    // (a missed prediction). Returns -1 only when every slot is in use at this present.
    int Use(const FlowPair& pair, int64_t present, bool* submitted, FlowUse* use) {
        int slot = Find(pair);
        *submitted = false;
        if (slot >= 0) {
            Slot& s = m_slots[slot];
            *use = s.predicted ? FlowUse::Hit : FlowUse::Own;
            if (!s.used && s.predicted) m_predictedUsed++;
            if (!s.used && !s.predicted) m_ownFlows++;
            s.used = true;
            s.lastTouch = present;
            return slot;
        }
        slot = Take(present);
        if (slot < 0) return -1;
        Fill(slot, pair, present, false);
        m_slots[slot].used = true;
        m_ownFlows++;
        *submitted = true;
        *use = FlowUse::Own;
        return slot;
    }

    // Drops every cached pair (a capture restart or a ring reset): stamps from before it can
    // never come again.
    void Reset() {
        for (int i = 0; i < kFlowSlots; i++) Evict(i);
    }

    long long Flows() const { return m_flows; }
    long long PredictedUsed() const { return m_predictedUsed; }
    long long PredictedUnused() const { return m_predictedUnused; }
    long long OwnFlows() const { return m_ownFlows; }

private:
    struct Slot {
        FlowPair pair;
        bool valid = false;
        bool predicted = false;  // flowed on a prediction rather than by the present using it
        bool used = false;
        int64_t lastTouch = 0;
    };

    // The least recently touched slot not touched at this present, emptied for reuse.
    int Take(int64_t present) {
        int best = -1;
        for (int i = 0; i < kFlowSlots; i++) {
            if (!m_slots[i].valid) return i;
            if (m_slots[i].lastTouch == present) continue;
            if (best < 0 || m_slots[i].lastTouch < m_slots[best].lastTouch) best = i;
        }
        if (best >= 0) Evict(best);
        return best;
    }

    void Fill(int slot, const FlowPair& pair, int64_t present, bool predicted) {
        Slot& s = m_slots[slot];
        s.pair = pair;
        s.valid = true;
        s.predicted = predicted;
        s.used = false;
        s.lastTouch = present;
        m_flows++;
    }

    void Evict(int slot) {
        Slot& s = m_slots[slot];
        if (s.valid && s.predicted && !s.used) m_predictedUnused++;
        s = Slot();
    }

    Slot m_slots[kFlowSlots];
    long long m_flows = 0;
    long long m_predictedUsed = 0;
    long long m_predictedUnused = 0;
    long long m_ownFlows = 0;
};

}  // namespace policy
