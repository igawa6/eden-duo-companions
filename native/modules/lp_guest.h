// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Game-thread calls through the guest mailbox (tools/luminescent-platinum/gen_load_plan.py): a stub in
// FieldManager.fdUpdate runs one requested call per field frame. A job is a chain of calls;
// each step's result decides whether the next one runs, so every precondition is checked by the
// game itself on its own thread right before the action.

#pragma once

#include "lp_profile.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace lp_guest {

using u32 = std::uint32_t;
using u64 = std::uint64_t;

// mailbox layout (gen_load_plan.py)
constexpr u32 MbSeq = 0x00, MbTaken = 0x04, MbHeartbeat = 0x08, MbDone = 0x0C, MbFn = 0x10, MbArgs = 0x18,
              MbResult = 0x38, MbScratch = 0x80;

// BD 1.3.0 RVAs (IL2CPP methods take a trailing MethodInfo*, passed as 0)
namespace rva {
constexpr u64 IsRunningEvent = lp_profile::Active.guest.IsRunningEvent;    // EvDataManager.IsRunningEvent(this)
constexpr u64 UiOnUseFieldItem = lp_profile::Active.guest.UiOnUseFieldItem;  // FieldManager.UI_onUseFieldItem(this, ItemNo) -> FieldUseResult
constexpr u64 UseFieldItem = lp_profile::Active.guest.UseFieldItem;      // FieldManager.UseFieldItem(this, ItemNo)
constexpr u64 UiOnFieldWaza = lp_profile::Active.guest.UiOnFieldWaza;     // FieldManager.UI_onFieldWaza(this, FieldWazaParam) -> FieldUseResult
constexpr u64 UiSelectWaza = lp_profile::Active.guest.UiSelectWaza;      // FieldManager.UI_SelectWaza(this, WazaNo)
constexpr u64 UiOnWazaFly = lp_profile::Active.guest.UiOnWazaFly;       // FieldManager.UI_onWazaFly(this, ZoneID, locatorIndex)
constexpr u64 CanUseHidenWaza = lp_profile::Active.guest.CanUseHidenWaza;   // FieldPoketch.CanUseHidenWaza(type) -> bool (native "can't" text)
constexpr u64 UseHidenWaza = lp_profile::Active.guest.UseHidenWaza;      // FieldPoketch.UseHidenWaza(type)
} // namespace rva

struct Io {
    u64 address = 0; // guest address of the mailbox, 0 = no load plan
    std::function<bool(u32 off, u32* v)> load32;
    std::function<bool(u32 off, u32 v)> store32;
    std::function<bool(u32 off, u64 v)> store64;
    std::function<bool(u32 off, u64* v)> load64;
};

// One call: `fn` and its arguments are computed when the call is issued (after the previous
// step), `ok` judges the result (x0); a refused step ends the job with `refusal` as its message (an
// lp_strings key, worded in the game's language by the module; "" = no message).
struct Step {
    std::function<u64()> fn;
    std::function<std::array<u64, 4>()> args;
    std::function<bool(u64)> ok;
    std::string refusal;
};

inline bool Zero(u64 r) { return (r & 0xFFFFFFFF) == 0; }  // FieldUseResult.Available / false
inline bool True(u64 r) { return (r & 0xFF) != 0; }
inline bool False(u64 r) { return (r & 0xFF) == 0; }
inline bool Any(u64) { return true; }

class Mailbox {
public:
    using Clock = std::chrono::steady_clock;
    using Done = std::function<void(bool ok, const std::string& refusal)>;

    void Configure(Io io) { this->io = std::move(io); }
    bool Present() const { return io.address != 0; }
    // The stub has run within the last second (it only runs while the field updates).
    bool Alive() const { return Present() && last_beat_change.time_since_epoch().count() != 0 &&
                                Clock::now() - last_beat_change < std::chrono::seconds(1); }
    bool Busy() const { return !steps.empty() || waiting; }
    u64 Address(u32 off) const { return io.address + off; }
    bool Store32(u32 off, u32 v) const { return io.store32 && io.store32(off, v); }
    bool Store64(u32 off, u64 v) const { return io.store64 && io.store64(off, v); }

    bool Run(std::vector<Step> job, Done done) {
        if (!Present() || Busy() || job.empty()) return false;
        steps = std::move(job);
        next = 0;
        on_done = std::move(done);
        if (Issue()) return true;
        steps.clear();
        on_done = {};
        return false;
    }

    // Once per sample: heartbeat, then the pending call's result.
    void Poll() {
        u32 beat = 0;
        if (Present() && io.load32 && io.load32(MbHeartbeat, &beat) && beat != last_beat) {
            last_beat = beat;
            last_beat_change = Clock::now();
        }
        if (!waiting) return;
        u32 done = 0;
        if (io.load32 && io.load32(MbDone, &done) && done == seq) {
            u64 result = 0;
            waiting = false;
            if (!io.load64 || !io.load64(MbResult, &result)) return Finish(false, "toast_cant_now");
            const auto& s = steps[next];
            if (s.ok && !s.ok(result)) return Finish(false, std::string{s.refusal});
            if (++next >= steps.size()) return Finish(true, {});
            if (!Issue()) Finish(false, "toast_cant_now");
            return;
        }
        if (Clock::now() - issued > std::chrono::seconds(3)) {
            // the field stopped updating (menu, scene change): cancel; the stub skips fn == 0
            Store64(MbFn, 0);
            waiting = false;
            Finish(false, "toast_cant_now");
        }
    }

private:
    bool Issue() {
        const auto& s = steps[next];
        const u64 fn = s.fn ? s.fn() : 0;
        if (!fn) return false;
        const auto a = s.args ? s.args() : std::array<u64, 4>{};
        u32 current = 0;
        if (!io.load32 || !io.load32(MbSeq, &current)) return false;
        // A cancelled request the stub has not served yet still owns seq: writing fn / args now
        // could run them under the old seq with the previous job's arguments.
        u32 served = 0;
        if (!io.load32(MbDone, &served) || served != current) return false;
        bool ok = Store64(MbFn, fn);
        for (u32 k = 0; k < 4; ++k) ok = ok && Store64(MbArgs + 8 * k, a[k]);
        seq = current + 1;
        ok = ok && Store32(MbSeq, seq); // last: publishes the request
        waiting = ok;
        issued = Clock::now();
        return ok;
    }
    void Finish(bool ok, std::string refusal) {
        steps.clear();
        waiting = false;
        if (auto d = std::move(on_done)) d(ok, refusal);
    }

    Io io;
    std::vector<Step> steps;
    std::size_t next = 0;
    Done on_done;
    bool waiting = false;
    u32 seq = 0, last_beat = 0;
    Clock::time_point issued{}, last_beat_change{};
};

} // namespace lp_guest
