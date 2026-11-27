// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "../helper/pla_mailbox.h"
#include "core/mods/dsmod_module_extensions.h"
#include "pla_blocks.h"

namespace pla {
struct Selection {
    unsigned mode = 0, item = 0, party = 0;
    bool operator==(const Selection &) const = default;
};
inline Selection Selected(const Selector &s) { return {s.mode, s.item_index, s.party_index}; }

// Only mailbox stores; no write API, input simulation or declarative guest bridge.
// Times are monotonic milliseconds, supplied by the module (fake clock in tests).
class TapSelect {
  public:
    ~TapSelect() { Clear(); }
    void Configure(const EdenDsmodHostExtensions *ext) {
        Clear();
        io = {};
        if (ext && ext->version == EDEN_DSMOD_EXT_VERSION && ext->struct_size == sizeof(*ext) &&
            ext->abi_hash == EDEN_DSMOD_EXT_HASH && ext->mailbox_address &&
            ext->mailbox_size >= PLA_MAILBOX_SIZE && ext->load_u32 && ext->store_u32) {
            io = *ext;
            // Cancel any request left by a previous module instance; do not reuse its sequence.
            uint32_t req = 0;
            if (Load(PLA_REQ, req))
                Store(PLA_CANCEL, req);
        }
    }
    bool Ready() const {
        uint32_t epoch = 0;
        return Load(PLA_EPOCH, epoch) && epoch;
    }
    bool Start(const Selector &s, const Party &p, unsigned mode, unsigned index, bool allowed,
               int64_t now) {
        // A second tap cannot rewrite the payload of an outstanding request.
        if (active || !allowed || !Ready() || !s.valid || !s.view || s.input_state == 2 ||
            !s.enabled || mode > 1 || (mode == 0 ? index >= s.item_count : index >= s.party_count))
            return false;
        list = s;
        target_mode = mode;
        target_index = index;
        if (!TargetValid(s, p))
            return false;
        if (mode == 1) {
            const auto &mon = p.slots[s.party[index]];
            target_species = mon.species;
            target_form = mon.form;
            target_name = mon.nickname;
        }
        before = Selected(s);
        active = true;
        last_progress = now;
        return true;
    }
    void Clear() {
        if (waiting)
            Store(PLA_CANCEL, sequence);
        active = waiting = false;
    }
    void Watchdog(int64_t now) {
        if (active && now - last_progress >= 2000)
            Clear();
    }
    void Step(const Selector &s, const Party &p, bool allowed, int64_t now) {
        if (!active)
            return;
        if (!allowed || !Ready() || !s.valid || s.view != list.view || s.input_state == 2 ||
            !s.enabled || now - last_progress >= 2000 || !SameLists(s) || !TargetValid(s, p)) {
            Clear();
            return;
        }
        if (target_mode == 1) {
            const auto &mon = p.slots[s.party[target_index]];
            if (mon.species != target_species || mon.form != target_form ||
                mon.nickname != target_name) {
                Clear();
                return;
            }
        }
        const auto current = Selected(s);
        if (waiting) {
            // Before acknowledgement a sample may already see the one expected native step.
            if (!(current == before) && !(current == after)) {
                Clear();
                return;
            }
            uint32_t ack = 0, result = PLA_INVALID;
            if (!Load(PLA_ACK, ack) || ack != sequence)
                return;
            Selection post;
            if (!Load(PLA_RESULT, result) || !Load(PLA_POST_MODE, post.mode) ||
                !Load(PLA_POST_ITEM, post.item) || !Load(PLA_POST_PARTY, post.party)) {
                Clear();
                return;
            }
            if (result == PLA_DONE) {
                if (!(post == after)) {
                    Clear();
                    return;
                }
                // ACK alone is insufficient. Wait for a fresh reader to observe progress.
                if (current == before)
                    return;
                before = current;
                last_progress = now;
            } else if (result != PLA_BUSY || !(post == before) || !(current == before)) {
                Clear();
                return;
            }
            waiting = false;
        } else if (!(current == before)) {
            Clear();
            return; // pad/native change wins
        }
        const unsigned current_index = target_mode == 0 ? current.item : current.party;
        if (current.mode == target_mode && current_index == target_index) {
            Clear();
            return;
        }
        if (s.animation)
            return; // tapping during native animation waits without a request
        after = before;
        unsigned op = PLA_TOGGLE;
        if (current.mode != target_mode) {
            after.mode = target_mode;
        } else {
            const unsigned n = target_mode == 0 ? s.item_count : s.party_count;
            const unsigned forward = (target_index + n - current_index) % n;
            const unsigned backward = (current_index + n - target_index) % n;
            op = forward <= backward ? PLA_NEXT : PLA_PREVIOUS;
            auto &index = target_mode == 0 ? after.item : after.party;
            index = op == PLA_NEXT ? (current_index + 1) % n : (current_index + n - 1) % n;
        }
        uint32_t req = 0, ack = 0, epoch = 0;
        if (!Load(PLA_REQ, req) || !Load(PLA_ACK, ack) || req != ack || !Load(PLA_EPOCH, epoch) ||
            !epoch)
            return;
        sequence = req + 1;
        if (!sequence)
            sequence = 1;
        bool ok = Store(PLA_OP, op) && Store(PLA_REQ_EPOCH, epoch) &&
                  Store(PLA_MODE, before.mode) && Store(PLA_ITEM, before.item) &&
                  Store(PLA_PARTY, before.party) && Store(PLA_VIEW, uint32_t(s.view)) &&
                  Store(PLA_VIEW + 4, uint32_t(s.view >> 32)) && Store(PLA_ITEMS, s.item_count) &&
                  Store(PLA_PARTIES, s.party_count) && Store(PLA_CANCEL, 0);
        // Publish last, release. Failed payload stores never publish a partial command.
        if (!ok || !Store(PLA_REQ, sequence)) {
            Clear();
            return;
        }
        waiting = true;
    }
    bool Pending(unsigned mode, unsigned index) const {
        return active && mode == target_mode && index == target_index;
    }
    bool Active() const { return active; }

  private:
    bool Load(uint32_t offset, uint32_t &value) const {
        return io.load_u32 && io.load_u32(io.userdata, offset, &value);
    }
    bool Store(uint32_t offset, uint32_t value) const {
        return io.store_u32 && io.store_u32(io.userdata, offset, value);
    }
    bool TargetValid(const Selector &s, const Party &p) const {
        if (target_mode == 0)
            return target_index < s.item_count && s.items[target_index].count;
        if (target_index >= s.party_count || !p.valid)
            return false;
        const auto slot = s.party[target_index];
        return slot < p.count && p.slots[slot].valid && !p.slots[slot].fainted && p.slots[slot].hp;
    }
    bool SameLists(const Selector &s) const {
        if (s.item_count != list.item_count || s.party_count != list.party_count)
            return false;
        for (unsigned j = 0; j < s.item_count; ++j)
            if (s.items[j].id != list.items[j].id)
                return false;
        for (unsigned j = 0; j < s.party_count; ++j)
            if (s.party[j] != list.party[j])
                return false;
        return true;
    }
    EdenDsmodHostExtensions io{};
    bool active = false, waiting = false;
    uint32_t sequence = 0;
    unsigned target_mode = 0, target_index = 0, target_species = 0, target_form = 0;
    std::string target_name;
    int64_t last_progress = 0;
    Selector list;
    Selection before, after;
};
} // namespace pla
