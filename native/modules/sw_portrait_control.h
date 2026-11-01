// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "sw_helper_mailbox.h"
#include <algorithm>
#include <cstdint>

namespace sw_portrait_control {
// Host request ownership only. Native lifetime/serialization belongs to the guest
// helper; cancelling never releases a guest object from the host thread.
template<class Load, class Store> class Control {
public:
    Control(Load load, Store store) : load_(load), store_(store) {}
    bool Initialize() {
        using namespace sw_helper;
        std::uint32_t req{}, seq{}, complete{};
        if (!load_(OffsetPortraitReqSeq, &req) || !load_(OffsetPortraitSeq, &seq) ||
            !load_(OffsetPortraitCompleteSeq, &complete)) return false;
        next_ = std::max(req, seq);
        outstanding_ = req != complete;
        // Withdraw an orphan even if its guest work has not started yet.
        return !req || store_(OffsetPortraitCancelSeq, req);
    }
    void Shutdown() {
        Cancel();
        std::uint32_t req{};
        if (load_(sw_helper::OffsetPortraitReqSeq, &req) && req)
            store_(sw_helper::OffsetPortraitCancelSeq, req);
    }
    void Tick(bool want, bool can_start, std::uint32_t captured, std::int64_t now) {
        using namespace sw_helper;
        std::uint32_t epoch{}, phase{}, seq{}, beat{}, complete{}, req{};
        const bool ok = load_(EpochOffset, &epoch) && epoch &&
            load_(OffsetPortraitPhase, &phase) && load_(OffsetPortraitSeq, &seq) &&
            load_(OffsetPortraitBeat, &beat) && load_(OffsetPortraitCompleteSeq, &complete) &&
            load_(OffsetPortraitReqSeq, &req);
        if (!ok) { Cancel(); return; }
        phase_ = phase;
        outstanding_ = req != complete;
        if (epoch_ != epoch) {
            Cancel(); epoch_ = epoch; beat_at_ = -1; beat_ = beat; beat_seen_ = true; attempts_ = 0;
        }
        if (beat != beat_) {
            if (beat_seen_) beat_at_ = now;
            beat_seen_ = true; beat_ = beat;
        }
        if (!want) { Cancel(); attempts_ = 0; }
        if (owned_) {
            if (!want || now - started_ > 30000) Cancel();
            if (seq == owned_ && phase == 2 && captured == owned_)
                store_(OffsetPortraitCapturedSeq, owned_);
            // The guest publishes Seq before allocation. Idle alone cannot mean
            // completion: it may still be entering Rendering on another thread.
            if (complete == owned_ && phase == 0) {
                owned_ = 0; retry_at_ = now + 3000;
            }
            return;
        }
        if (!want || !can_start || phase != 0 || outstanding_ || attempts_ >= 3 || now < retry_at_ ||
            beat_at_ < 0 || now - beat_at_ > 600) return;
        std::uint32_t free{}, field_req{}, field_ack{}, progression{};
        if (!load_(OffsetPortraitReqSeq, &req) || req != seq ||
            !load_(OffsetFieldFree, &free) || free != 1 ||
            !load_(OffsetFieldReqSeq, &field_req) || !load_(OffsetFieldAckSeq, &field_ack) ||
            field_req != field_ack || !load_(OffsetFieldProgressionSeq, &progression) || progression) return;
        if (++next_ == 0) ++next_;
        if (!store_(OffsetPortraitReqEpoch, epoch) ||
            !store_(OffsetPortraitReqSeq, next_)) return; // publish the sequence last
        owned_ = next_; started_ = now; ++attempts_;
    }
    std::uint32_t sequence() const { return owned_; }
    std::uint32_t phase() const { return phase_; }
    bool busy() const { return owned_ || outstanding_ || phase_ == 1 || phase_ == 3 || phase_ == 4; }
private:
    void Cancel() { if (owned_) store_(sw_helper::OffsetPortraitCancelSeq, owned_); }
    Load load_; Store store_;
    std::uint32_t next_{}, owned_{}, epoch_{}, phase_{}, beat_{};
    int attempts_{};
    bool beat_seen_{}, outstanding_{};
    std::int64_t beat_at_{-1}, started_{}, retry_at_{};
};
}
