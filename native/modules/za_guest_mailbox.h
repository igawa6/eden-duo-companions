// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/mods/dsmod_module_extensions.h"
#include "za_assets.h"
#include "za_save.h"
#include <bit>
#include <cmath>

namespace za {

// The matching guest code is native/helper/za_guest_helper.S. The sequence
// is published last with release semantics. Eden advances the epoch on pause.
class GuestMailbox {
public:
    enum class Operation : u32 { ReplaceMove = 1, Exchange = 2, HeldItem = 3, Medicine = 4 };
    enum class OperationResult { Pending, Accepted, Rejected };
    enum class TravelResult : u32 { None, Confirmation, Expired, Unavailable, MarkerApplied };

    void Configure(const EdenDsmodHostExtensions* host) {
        *this = {};
        if (host && host->mailbox_address && host->mailbox_size >= 4096 &&
            host->load_u32 && host->store_u32 && host->store_u64) {
            io = *host;
        }
    }

    bool Poll(u64 tick) {
        u32 beat{};
        if (!io.load_u32 || !Read(Heartbeat, beat))
            return false;
        if (beat != last_beat) {
            last_beat = beat;
            beat_tick = tick;
        }
        u32 pending{}, opening{}, deadline{}, epoch{}, requested_epoch{};
        if (Read(TravelSelect, pending) && Read(TravelOpen, opening) && (pending || opening) &&
            Read(TravelDeadline, deadline) &&
            Read(Epoch, epoch) && Read(TravelEpoch, requested_epoch) &&
            (epoch != requested_epoch || ((deadline - beat) & 0x80000000u))) {
            Store(TravelOpen, 0);
            Store(TravelSelect, 0);
            Store(TravelResultOffset, static_cast<u32>(TravelResult::Expired));
        }
        return beat_tick != 0 && tick >= beat_tick && tick - beat_tick < 60;
    }

    bool Idle() const {
        u32 sequence{}, done{}, pending{};
        return Read(Sequence, sequence) && Read(Done, done) && sequence == done &&
               Read(TravelSelect, pending) && !pending && Read(TravelOpen, pending) && !pending;
    }

    bool RequestMapOpen() const {
        u32 epoch{}, heartbeat{};
        return Idle() && Read(Epoch, epoch) && Read(Heartbeat, heartbeat) &&
               Store(TravelEpoch, epoch) && Store(TravelDeadline, heartbeat+180) &&
               Store(TravelOpen, 1);
    }

    bool Request(Operation operation, u32 party, u32 first, u32 second,
                 const Entity& member, const Entity* boxed = nullptr,
                 u32* request_sequence = nullptr, u32 medicine_effect = 0) const {
        u32 sequence{}, done{}, epoch{};
        if (!member.checksum_ok || (boxed && !boxed->checksum_ok) || !Idle() ||
            !Read(Sequence, sequence) || !Read(Done, done) || sequence != done ||
            !Read(Epoch, epoch))
            return false;
        const bool sent = Store(RequestEpoch, epoch) && Store(Op, static_cast<u32>(operation)) &&
               Store(Party, party) && Store(First, first) && Store(Second, second) &&
               io.store_u64(io.userdata, PartyHeader, Header(member)) &&
               io.store_u64(io.userdata, BoxHeader, boxed ? Header(*boxed) : medicine_effect) &&
               Store(Sequence, sequence + 1);
        if (sent && request_sequence) *request_sequence = sequence + 1;
        return sent;
    }

    OperationResult BagResult(u32 request_sequence) const {
        u32 done{}, result{};
        if (!Read(HeldDone, done) || done != request_sequence || !Read(HeldResult, result))
            return OperationResult::Pending;
        return result ? OperationResult::Accepted : OperationResult::Rejected;
    }

    bool RequestTravel(const Assets::TravelDestination& destination, bool open) const {
        u32 pending{}, epoch{}, heartbeat{};
        const auto& first=destination.points[0];
        const auto& second=destination.points[1];
        return destination.Available() && first.key && Idle() && Read(TravelOpen, pending) && !pending &&
               Read(Epoch, epoch) && Read(Heartbeat, heartbeat) &&
               Store(TravelEpoch, epoch) && Store(TravelDeadline, heartbeat + 180) &&
               Store(TravelKind, destination.kind) && Store(TravelResultOffset, 0) &&
               Store(TravelRow, static_cast<u32>(first.row)) &&
               io.store_u64(io.userdata, TravelKey, first.key) &&
               Store(TravelSecondRow, static_cast<u32>(second.row)) &&
               io.store_u64(io.userdata, TravelSecondKey, second.key) &&
               Store(TravelSelect, 1) && (!open || Store(TravelOpen, 1));
    }

    TravelResult TravelStatus() const {
        u32 value{};
        Read(TravelResultOffset, value);
        return static_cast<TravelResult>(value);
    }

    bool RequestMarker(float x, float y, bool clear, bool open) const {
        u32 pending{}, epoch{}, heartbeat{};
        if (!std::isfinite(x) || !std::isfinite(y) || std::hypot(x,y)>2048.f) return false;
        const auto coordinates=std::bit_cast<u32>(x) | (u64{std::bit_cast<u32>(y)}<<32);
        return Idle() && Read(TravelOpen,pending) && !pending &&
               Read(Epoch,epoch) && Read(Heartbeat,heartbeat) &&
               Store(TravelEpoch,epoch) && Store(TravelDeadline,heartbeat+180) &&
               Store(TravelResultOffset,0) && io.store_u64(io.userdata,MarkerCoordinates,coordinates) &&
               io.store_u64(io.userdata,MarkerMapModelOffset,0) &&
               Store(TravelSelect,clear?3:2) && (!open || Store(TravelOpen,1));
    }

    u64 MarkerMapModel() const {
        u64 model{};
        return TravelStatus()==TravelResult::MarkerApplied && io.load_u64 &&
               io.load_u64(io.userdata,MarkerMapModelOffset,&model)?model:0;
    }

    void ClearTravelStatus() const {
        u32 pending{};
        if (Read(TravelSelect, pending) && !pending)
            Store(TravelResultOffset, 0);
    }

private:
    static constexpr u32 Sequence = 0, Done = 4, Heartbeat = 8;
    static constexpr u32 RequestEpoch = 16, Op = 20, Party = 24, First = 28, Second = 32;
    static constexpr u32 PartyHeader = 40, BoxHeader = 48, Epoch = 4092;
    static constexpr u32 HeldDone = 72, HeldResult = 76;
    // Separate requests for the field and the native map update threads.
    static constexpr u32 TravelOpen = 0x100, TravelEpoch = 0x104, TravelDeadline = 0x108;
    static constexpr u32 TravelSelect = 0x10c, TravelRow = 0x110, TravelKey = 0x118;
    static constexpr u32 TravelResultOffset = 0x120, TravelKind = 0x124;
    static constexpr u32 TravelSecondRow = 0x128, TravelSecondKey = 0x130;
    static constexpr u32 MarkerCoordinates = 0x138, MarkerMapModelOffset = 0x140;

    static u64 Header(const Entity& entity) {
        return entity.encryption_constant | (static_cast<u64>(entity.checksum) << 48);
    }

    bool Read(u32 offset, u32& value) const {
        return io.load_u32 && io.load_u32(io.userdata, offset, &value);
    }
    bool Store(u32 offset, u32 value) const {
        return io.store_u32(io.userdata, offset, value);
    }

    EdenDsmodHostExtensions io{};
    u32 last_beat{};
    u64 beat_tick{};
};

} // namespace za
