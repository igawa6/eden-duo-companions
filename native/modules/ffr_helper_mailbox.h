// SPDX-License-Identifier: GPL-3.0-or-later
//
// FINAL FANTASY RESONANCE guest helper mailbox (native/helper/ffr_guest_helper.S): the module side.
// 4096 bytes, epoch word at 4092 (the load plan's mailbox_epoch_offset). The module writes a request (fields first, req_seq
// last, release); the helper answers it on the game thread at the next CPP_TaskManager tick (result fields first, result_seq
// last, release). Offsets here and the .equ table at the top of the .S file must match.
#pragma once

#include <cstdint>
#include <vector>

#include "core/mods/dsmod_module_extensions.h"

namespace ffr_helper {

inline constexpr uint32_t kMailboxSize = 8192, kEpoch = 4092, kMagic = 0x32524646;  // 'FFR2'
inline constexpr uint32_t kMagicOff = 0x000, kHeartbeat = 0x004, kReqSeq = 0x008, kReqOp = 0x00C, kReqEpoch = 0x010,
                          kPtrA = 0x018, kPtrB = 0x020, kPtrC = 0x028, kPtrD = 0x040, kArgA = 0x030, kArgB = 0x034, kArgC = 0x038,
                          kResSeq = 0x060, kResCode = 0x064, kResValue = 0x068, kSndSeq = 0x078, kSndAck = 0x07C,
                          kSndTag = 0x080, kIds = 0x1800, kBits = 0x1C00;
inline constexpr uint32_t kMaxIds = 256;

// Travel/Check: a = map id, b = landing point, objects DA_WldMapParameter. Item: a = item id, b = target unit, objects
// DA_UI_SkillAction (+ DA_UI_SkillTargeting). Equip: a = unit, b = item, c = slot. Unequip: a = unit, b = item.
// Equippable: a = unit, b = count, the ids in the mailbox (Stage's ids), the answer in Bits.
// Track: a = quest id, object CPP_QuestSubsystem (the tracked quest again untracks it).
// Sounds have their own channel (Client::Sound): no request, no answer.
enum class Op : uint32_t { Check = 1, Travel = 2, Item = 3, Equip = 4, Unequip = 5, Equippable = 6, Track = 7 };

struct Request {
    Op op = Op::Check;
    uint64_t obj = 0, obj_class = 0, obj2 = 0, obj2_class = 0;
    int32_t a = 0, b = 0, c = 0;
    const std::vector<int>* ids = nullptr;  // Equippable: at most kMaxIds
};
enum class Result : uint32_t { Ok = 0, NotConfigured = 1, BadOp = 2, StaleEpoch = 3, NotReady = 4, Refused = 5 };

struct Outcome {
    bool answered = false;
    Result code = Result::NotConfigured;
    uint32_t tag = 0, tag_number = 0;  // the game's dialog tag for the place (FName)
};

class Client {
public:
    // The host gives a mailbox only when the package's load plan matched this build and its helper was installed.
    bool Bind(const EdenDsmodHostExtensions* ext) {
        *this = Client{};
        if (!ext || ext->version != EDEN_DSMOD_EXT_VERSION || ext->struct_size != sizeof(*ext) || ext->abi_hash != EDEN_DSMOD_EXT_HASH ||
            !ext->mailbox_address || ext->mailbox_size < kMailboxSize || !ext->load_u32 || !ext->load_u64 || !ext->store_u32 || !ext->store_u64)
            return false;
        io_ = *ext;
        uint32_t epoch = 0;
        if (!L32(kEpoch, epoch)) return false;
        if (!epoch && !S32(kEpoch, epoch = 1)) return false;
        uint32_t seq = 0;
        if (!L32(kResSeq, seq)) return false;
        seq_ = seq;
        if (!L32(kSndSeq, snd_)) return false;
        return bound_ = S32(kMagicOff, kMagic);
    }
    bool Bound() const { return bound_; }
    uint32_t Heartbeat() const { uint32_t v = 0; return L32(kHeartbeat, v) ? v : 0; }
    bool Busy() const { return busy_; }

    // A new request; false while one is in flight or when the mailbox refuses the stores.
    bool Stage(Op op, uint64_t da, uint64_t da_class, int32_t map_id, int32_t landing) {
        Request r;
        r.op = op, r.obj = da, r.obj_class = da_class, r.a = map_id, r.b = landing;
        return Stage(r);
    }
    bool Stage(const Request& r) {
        if (!bound_ || busy_) return false;
        uint32_t epoch = 0;
        if (!L32(kEpoch, epoch) || !epoch) return false;
        if (r.ids) {
            if (r.ids->size() > kMaxIds) return false;
            for (size_t i = 0; i < r.ids->size(); ++i)
                if (!S32(kIds + uint32_t(4 * i), uint32_t((*r.ids)[i]))) return false;
        }
        if (!S32(kReqOp, uint32_t(r.op)) || !S32(kReqEpoch, epoch) || !S64(kPtrA, r.obj) || !S64(kPtrB, r.obj_class) ||
            !S64(kPtrC, r.obj2) || !S64(kPtrD, r.obj2_class) || !S32(kArgA, uint32_t(r.a)) || !S32(kArgB, uint32_t(r.b)) ||
            !S32(kArgC, uint32_t(r.c)))
            return false;
        if (!S32(kReqSeq, ++seq_)) return false;
        busy_ = true;
        return true;
    }
    // The answer to the request in flight (answered false while there is none yet).
    Outcome Poll() {
        Outcome o;
        uint32_t seq = 0, code = 0;
        if (!busy_ || !L32(kResSeq, seq) || seq != seq_) return o;
        uint64_t v = 0;
        if (!L32(kResCode, code) || !L64(kResValue, v)) return o;
        busy_ = false;
        o.answered = true, o.code = Result(code), o.tag = uint32_t(v), o.tag_number = uint32_t(v >> 32);
        return o;
    }
    // Equippable's answer: bit i set when the unit can wear ids[i]
    bool Bit(size_t i) const {
        uint32_t w = 0;
        return i < kMaxIds && L32(kBits + uint32_t(4 * (i / 32)), w) && (w >> (i % 32)) & 1;
    }
    // A UI sound (a gameplay tag's FName) on the sound channel: the helper plays it at its next tick, whatever request is in flight
    bool Sound(uint64_t tag) {
        return bound_ && tag && S64(kSndTag, tag) && S32(kSndSeq, ++snd_);
    }
    // Gives up on the request in flight: a new epoch, so a helper that reads it later refuses it as stale.
    void Abandon() {
        if (!busy_) return;
        uint32_t epoch = 0;
        if (L32(kEpoch, epoch)) S32(kEpoch, epoch + 1 ? epoch + 1 : 1);
        busy_ = false;
        uint32_t seq = 0;
        if (L32(kReqSeq, seq)) S32(kResSeq, seq);  // (the helper answers only a req_seq it has not answered)
    }

private:
    bool L32(uint32_t o, uint32_t& v) const { return io_.load_u32(io_.userdata, o, &v) == EDEN_DSMOD_TRUE; }
    bool L64(uint32_t o, uint64_t& v) const { return io_.load_u64(io_.userdata, o, &v) == EDEN_DSMOD_TRUE; }
    bool S32(uint32_t o, uint32_t v) const { return io_.store_u32(io_.userdata, o, v) == EDEN_DSMOD_TRUE; }
    bool S64(uint32_t o, uint64_t v) const { return io_.store_u64(io_.userdata, o, v) == EDEN_DSMOD_TRUE; }
    EdenDsmodHostExtensions io_{};
    bool bound_ = false, busy_ = false;
    uint32_t seq_ = 0, snd_ = 0;
};

}  // namespace ffr_helper
