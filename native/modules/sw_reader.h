// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// The reader layer: one Sample() per domain (trainer, misc8, party, live party, bag, repel, dex,
// location, dens, state, battle), each publishing a `sw.<domain>.ready` gate plus a human
// `sw.<domain>.status` string. A domain looks itself up in sw_addresses.h's table and reads
// nothing unless that lead's `verified` bit is set (MODULE_GUIDE.md section 6, "Fail closed per
// domain"). Every domain listed above is wired to a live-verified anchor as of 2026-10-26.
//
// Fail-closed rule: a read or validation failure on any given tick does not publish that value
// this tick. The host clears its snapshot every tick (`ModRuntime::Tick()` in mod_runtime.cpp
// calls `snapshot.ints.clear()`/`texts.clear()` before the module's sample() runs), so a key this
// module does not publish on a tick is ABSENT that tick, not "last value held over" -- any sticky
// behaviour must be kept in member state and republished explicitly (see `phase_` below).
//
// Cost (pre-Thor review H3, 2026-10-26): sample() runs at 60 Hz on the emulator's timing thread,
// so the heavy domains (party, live party, bag, dex, dens, the battle weakness boards) are
// change-driven: each reads its raw guest bytes, hashes them together with its companion-side
// selection into a signature, and only re-decodes and re-renders its keys when that signature
// changes. Otherwise the recorded publishes (a `Tape`) are replayed -- they must still be
// republished every tick because of the snapshot clear above. Dex and dens re-read their (large,
// rarely changing) blocks at most every kSlowRefreshTicks; romfs-derived per-species/per-move
// records are cached for the Reader's lifetime; the evolution table loads in slices.

#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/mods/dsmod_module_abi.h"
#include "sw_addresses.h"
#include "sw_battle.h"
#include "sw_battle_field.h"
#include "sw_battle_raid.h"
#include "sw_battle_foe.h"
#include "sw_battle_cursor.h"
#include "sw_battle_gauge.h"
#include "sw_helper_mailbox.h"
#include "sw_dex_index.h"
#include "sw_dex_text.h"
#include "sw_dex_info.h"
#include "sw_effectiveness_text.h"
#include "sw_font_metrics.h"
#include "sw_evolution.h"
#include "sw_item_descriptions.h"
#include "sw_item_icons.h"
#include "sw_item_names.h"
#include "sw_item_pouch.h"
#include "sw_level.h"
#include "sw_move_data.h"
#include "sw_move_names.h"
#include "sw_map_selection.h"
#include "sw_personal_stats.h"
#include "sw_pk8.h"
#include "sw_route.h"
#include "sw_party_details.h"
#include "sw_overworld.h"
#include "sw_town_map.h"
#include "sw_species_icons.h"
#include "sw_species_icon_metrics.h"
#include "sw_species_names.h"
#include "sw_lang.h"
#include "sw_strings.h"
#include "sw_text_fit.h"
#include "sw_species_types.h"
#include "sw_status_icons.h"
#include "sw_type_chart.h"
#include "sw_variant.h"
#include "sw_zone.h"
#include "sw_zukan.h"

namespace sw_reader {

using u64 = std::uint64_t;

enum class Status { kUnavailable, kUnresolved, kReady };

inline const char* StatusText(Status s) {
    switch (s) {
        case Status::kReady: return "ready";
        case Status::kUnresolved: return "unresolved anchor (see sw_addresses.h)";
        default: return "unavailable";
    }
}

/// Publishes `sw.<domain>.ready` (0/1) and `sw.<domain>.status` (text) for one domain, from one
/// anchor lookup. Centralised so every domain's "not verified yet" path looks identical in the
/// snapshot a page can bind to.
inline void PublishDomainStatus(const EdenDsmodHostApi& host, const char* domain,
                                const sw_addresses::Anchor* anchor) {
    char ready_name[64];
    char status_name[64];
    std::snprintf(ready_name, sizeof ready_name, "sw.%s.ready", domain);
    std::snprintf(status_name, sizeof status_name, "sw.%s.status", domain);
    const Status status = !anchor ? Status::kUnresolved
                          : !anchor->verified ? Status::kUnavailable
                                              : Status::kReady;
    if (host.publish_i64) host.publish_i64(host.userdata, ready_name, status == Status::kReady ? 1 : 0);
    if (host.publish_text) host.publish_text(host.userdata, status_name, StatusText(status));
}

/// One guest read with a torn-read check: read twice, and only accept the bytes if both reads
/// agree (MODULE_GUIDE.md section 6, "Reject torn reads"). Returns false on any read failure or
/// disagreement, leaving `out` untouched.
inline bool ReadStable(const EdenDsmodHostApi& host, u64 address, void* out, std::size_t size) {
    if (!host.is_mapped || !host.read_memory) return false;
    if (!host.is_mapped(host.userdata, address, size)) return false;
    std::string a(size, '\0'), b(size, '\0');
    if (!host.read_memory(host.userdata, address, a.data(), size)) return false;
    if (!host.read_memory(host.userdata, address, b.data(), size)) return false;
    if (a != b) return false;
    std::memcpy(out, a.data(), size);
    return true;
}

/// FNV-1a 64 over a byte range, folded 8 bytes at a time (a change detector, not a checksum the
/// game uses). `h` chains several ranges into one signature.
inline std::uint64_t HashBytes(const void* data, std::size_t size, std::uint64_t h = 0xcbf29ce484222325ull) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    std::size_t i = 0;
    for (; i + 8 <= size; i += 8) {
        std::uint64_t w;
        std::memcpy(&w, p + i, 8);
        h = (h ^ w) * 0x100000001b3ull;
    }
    for (; i < size; ++i) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}
inline std::uint64_t HashValue(std::uint64_t v, std::uint64_t h) { return HashBytes(&v, sizeof v, h); }

/// What one domain published on the tick its inputs last changed, replayed verbatim on every tick
/// they have not (the host clears its snapshot every tick, see the file header).
struct Tape {
    std::uint64_t sig = 0;
    bool valid = false;
    std::vector<std::pair<std::string, std::int64_t>> ints;
    std::vector<std::pair<std::string, std::string>> texts;
    void Clear() {
        valid = false;
        ints.clear();
        texts.clear();
    }
    void Replay(const EdenDsmodHostApi& host) const {
        if (host.publish_i64)
            for (const auto& [k, v] : ints) host.publish_i64(host.userdata, k.c_str(), v);
        if (host.publish_text)
            for (const auto& [k, v] : texts) host.publish_text(host.userdata, k.c_str(), v.c_str());
    }
};

/// Reader instance: per-boot companion selection, the phase machine, the change-driven domain
/// tapes and the romfs-derived caches (MODULE_GUIDE.md section 7.1's Reader::Sample shape).
class Reader {
public:
    void Sample(const EdenDsmodHostApi& host) {
        const u64 heap_begin = host.get_heap_begin ? host.get_heap_begin(host.userdata) : 0;
        ++tick_;
        PublishGameStrings(host);
        StepEvolutionTable(host); // M5: the 898-file evolution table loads in slices, never in one tick

        SampleTrainer(host, heap_begin);
        SampleMisc8(host, heap_begin);
        // The live party (party.array_root, current HP/status, the same objects the game's own Bag
        // uses) is the party source; the save-snapshot mirror (party.save_mirror) is only used,
        // whole, when the live array itself cannot be read -- never per slot (review M1: the two
        // orders can differ, so mixing them would put one member's data on another's tile).
        SampleLiveParty(host, heap_begin);
        if (!live_party_ok_) SampleParty(host, heap_begin);
        SamplePartyDetails(host);
        SampleBag(host, heap_begin);
        SampleRepel(host, heap_begin);
        SampleDex(host, heap_begin);
        SampleLocation(host, heap_begin);
        SampleDens(host, heap_begin);
        // SampleState (CurrentScreen -> field/battle classification) must run before SampleBattle:
        // the battle struct's own address is only meaningful (not stale prior-battle memory) while
        // the game is actually in a battle, and SampleBattle gates on this tick's classification.
        SampleState(host, heap_begin);
        map_area_ = -1;
        SampleOverworld(host);
        SampleMapPlaces(host);
        SampleProgress(host, heap_begin);
        SampleRoute(host);
        SampleBattle(host, heap_begin);
    }

    // A new session (module configure): forget everything that belongs to the previous boot's
    // game state. The encounter cache is per session; other static romfs caches survive.
    void ResetSession() {
        encounters_.Reset();
        phase_ = Phase::kTitle;
        phase_known_ = false;
        field_stable_ticks_ = 0;
        reload_nonfield_ticks_ = 0;
        confirmed_zone_name_.clear();
        screen_sentinel_battle_ = false;
        last_phase_battle_ = false;
        phase_battle_edges_ = 0;
        battle_hint_ = false;
        helper_seen_ = false;
        field_frames_ = battle_end_field_frames_ = 0;
        battle_silent_ticks_ = 0;
        battle_client_ = 0;
        battle_command_waiting_ = false;
        SetBattleScreen(0);
        battle_ball_count_ = 0;
        field_own_side_ = -1;
        battle_end_ticks_ = 0;
        world_player_ = 0;
        world_signature_ = 0;
        world_scan_tick_ = 0;
        map_hint_zone_ = 0;
        map_hint_parent_ = 0;
        map_last_.valid = false;
        map_selection_.Clear();
        map_area_ = -1;
        map_zone_ = 0;
        field_zone_ = 0;
        live_weather_ = -1;
        champion_known_ = champion_ = false;
        route_open_ = false;
        route_zone_ = 0;
        route_weather_ = -1;
        wild_target_ = 0;
        dex_blocks_ok_ = false;
        bike_state_ready_ = false;
        bike_riding_ = false;
        for (Tape* t : {&party_tape_, &live_tape_, &bag_tape_, &dex_tape_, &dens_tape_,
                        &foe_board_tape_, &own_board_tape_, &party_detail_tape_,
                        &bench_icon_tape_, &raid_boss_board_tape_, &route_tape_, &wild_tape_})
            t->Clear();
        for (Tape& t : raid_mon_tape_) t.Clear();
        for (Tape& t : field_side_tape_) t.Clear();
        for (Tape& t : switch_pick_tape_) t.Clear();
        live_party_cache_.fill(LiveSlot{});
        live_party_ok_ = false;
        party_detail_index_ = -1;
        party_detail_ec_ = 0;
        party_detail_tab_ = 0;
        party_detail_move_index_ = -1;
        party_detail_move_id_ = 0;
        party_detail_mons_.fill(sw_pk8::LivePartyMon{});
        bag_raw_ok_ = false;
        dex_region_available_ = {true, false, false};
        dex_sel_region_ = 0;
        dex_sel_row_ = 0;
        bag_sel_pocket_ = 0;
        bag_sel_row_ = 0;
        dmax_preview_ = {};
        battle_raid_ready_ = false;
        battle_dmax_active_ = false;
        battle_own_actor_ = 0;
    }

    // The game's unlocked Flying Taxi destinations prove that a DLC region was discovered;
    // seen DLC species alone do not (they can arrive through trades without owning the DLC).
    // The module supplies this only after a successful native FLY_QUERY. Until then DLC
    // totals and entry lists remain unavailable instead of exposing the complete region size.
    void SetDexRegionAccess(bool armor, bool crown) {
        const std::array<bool, 3> next{true, armor, crown};
        if (next == dex_region_available_) return;
        dex_region_available_ = next;
        if (!dex_region_available_[static_cast<std::size_t>(dex_sel_region_)]) {
            dex_sel_region_ = 0;
            dex_sel_row_ = 0;
        }
        dex_tape_.Clear();
    }

    bool bike_ready() const { return bike_state_ready_; }
    bool bike_riding() const { return bike_riding_; }
    bool battle_raid_ready() const { return battle_raid_ready_; }
    bool battle_dmax_active() const { return battle_dmax_active_; }
    std::uint64_t battle_own_actor() const { return battle_own_actor_; }

    void SetMapZoneHint(std::uint64_t current, std::uint64_t parent) {
        map_hint_zone_ = current;
        map_hint_parent_ = parent;
        if (!current || current == sw_town_map::kZoneHashBasis) {
            map_hint_zone_ = 0;
            map_hint_parent_ = 0;
        }
    }

    // Native query results are paired with the actual source move IDs. They affect display
    // metadata only: BattleView and the native item/action preconditions retain base IDs/PP.
    void SetDmaxPreview(bool wanted, const std::array<std::uint32_t, 4>& max_ids = {},
                        const std::array<std::uint32_t, 4>& powers = {},
                        const std::array<std::int32_t, 4>& gmax_indices = {},
                        const std::array<std::uint32_t, 4>& base_ids = {},
                        std::uint64_t source_actor = 0) {
        dmax_preview_ = {};
        if (!wanted) return;
        dmax_preview_.wanted = true;
        dmax_preview_.client = battle_client_;
        dmax_preview_.actor = source_actor ? source_actor : battle_own_actor_;
        dmax_preview_.max_ids = max_ids;
        dmax_preview_.powers = powers;
        dmax_preview_.gmax_indices = gmax_indices;
        dmax_preview_.base_ids = base_ids;
    }

    // Live party slot identity for the field medicine target check (review M1): the encryption
    // constant of the live slot the Bag's "use on" tile shows (0 = no valid live slot there).
    std::uint32_t live_party_ec(int slot) const {
        if (slot < 0 || slot >= 6) return 0;
        const auto& s = live_party_cache_[static_cast<std::size_t>(slot)];
        return s.valid ? s.ec : 0;
    }
    bool live_party_slot_ok(int slot) const {
        // Field medicine/PP targets: an Egg is never one (party swap uses live_party_array()).
        return slot >= 0 && slot < 6 && live_party_cache_[static_cast<std::size_t>(slot)].valid &&
               !live_party_cache_[static_cast<std::size_t>(slot)].egg;
    }
    // Party reorder (Pokemon page drag, owner Thor 2026-10-31): the live party object's slot
    // pointer array as read THIS tick (root+0x58+8*i, count at +0x88). Only valid while every
    // member decoded (live_party_ok_ and each slot valid) in the plain field (not a native menu,
    // which may be listing the party itself).
    struct PartyArray {
        bool ok = false;
        u64 root = 0;
        int count = 0;
        std::array<u64, 6> ptr{};
    };
    PartyArray live_party_array() const {
        PartyArray out = live_party_array_;
        if (!live_party_ok_ || phase_ != Phase::kField) out.ok = false;
        for (int i = 0; out.ok && i < out.count; ++i)
            if (!live_party_cache_[static_cast<std::size_t>(i)].valid || !out.ptr[static_cast<std::size_t>(i)]) out.ok = false;
        return out;
    }

    // Companion-owned UI selection (the task's "companion keeps its own selection" -- no direct
    // game action, just which pocket/row/region/entry this module's own Bag/Pokédex pages are
    // currently showing). Called from the module's ActionCallback (ModuleExtensions' on_action)
    // for the "bag_pocket"/"bag_row"/"dex_region"/"dex_row" manifest actions -- see
    // 0100ABF008968000.cpp. Returns false (action not handled/out of range) so the host's
    // "module declined" path behaves normally rather than silently swallowing a bad tap.
    bool DispatchAction(const char* action, std::int64_t argument) {
        if (!action) return false;
        const std::string_view a{action};
        if (a == "party_detail") {
            if (argument < 0 || argument >= 6 || !live_party_ok_) return false;
            const auto i = static_cast<std::size_t>(argument);
            if (!live_party_cache_[i].valid || party_detail_mons_[i].egg) return false;
            party_detail_index_ = static_cast<int>(argument);
            party_detail_ec_ = live_party_cache_[i].ec;
            party_detail_tab_ = 0;
            party_detail_move_index_ = -1;
            return true;
        }
        if (a == "party_detail_close") {
            party_detail_index_ = -1;
            party_detail_move_index_ = -1;
            return true;
        }
        if (a == "party_detail_move") {
            if (argument == -1) { party_detail_move_index_ = -1; return true; }
            if (party_detail_index_ < 0 || party_detail_tab_ != 1 || argument < 0 || argument >= 4) return false;
            const auto& mon = party_detail_mons_[static_cast<std::size_t>(party_detail_index_)];
            const auto move = mon.moves[static_cast<std::size_t>(argument)];
            if (move == 0 || move > sw_move_data::kMaxMoveId) return false;
            party_detail_move_index_ = static_cast<int>(argument);
            party_detail_move_id_ = move;
            return true;
        }
        if (a == "party_detail_tab") {
            if (party_detail_index_ < 0 || argument < 0 || argument > 2) return false;
            party_detail_tab_ = static_cast<int>(argument);
            party_detail_move_index_ = -1;
            return true;
        }
        if (a == "bag_pocket") {
            if (argument < 0 || argument >= sw_item_pouch::kPouchCount) return false;
            bag_sel_pocket_ = static_cast<int>(argument);
            bag_sel_row_ = 0; // D2 behaviour: switching pockets resets the row selection
            return true;
        }
        if (a == "bag_row") {
            if (argument < 0 || argument >= kMaxBagRows) return false;
            bag_sel_row_ = static_cast<int>(argument);
            return true;
        }
        if (a == "dex_region") {
            if (argument < 0 || argument > 2 ||
                !dex_region_available_[static_cast<std::size_t>(argument)]) return false;
            dex_sel_region_ = static_cast<int>(argument);
            dex_sel_row_ = 0; // switching region resets the row selection
            return true;
        }
        // Battle page matchup-card tab (Foe / Own / Field): companion-side selection only.
        if (a == "battle_tab") {
            if (argument < 0 || argument > 2) return false;
            battle_tab_ = static_cast<int>(argument);
            return true;
        }
        if (a == "map_select") {
            if (argument == -1) { map_selection_.Clear(); return true; }
            return argument >= 0 && argument < static_cast<std::int64_t>(map_catalog_.anchors.size()) &&
                map_area_ >= 0 && map_catalog_ready_ &&
                map_selection_.Select(map_catalog_, static_cast<int>(argument), map_area_);
        }
        // Town Map "Wild Pokemon" row -> the grid page for the place it shows (the selected place,
        // else where the player stands), latched until Back; route_weather = a weather chip.
        if (a == "map_wild") {
            const auto* zone = sw_route::Find(encounters_.data, wild_target_);
            if (!zone) return false;
            route_zone_ = wild_target_;
            route_weather_ = sw_route::DefaultWeather(*zone, route_zone_ == field_zone_ ? live_weather_ : -1);
            route_open_ = true;
            ++route_serial_;
            return true;
        }
        if (a == "route_back") { route_open_ = false; return true; }
        if (a == "route_weather") {
            const auto* zone = sw_route::Find(encounters_.data, route_zone_);
            if (!zone || !route_open_) return false;
            int k = 0;
            for (const int w : sw_route::kChipOrder) {
                if (!sw_route::Possible(*zone, w)) continue;
                if (k++ == argument) {
                    if (route_weather_ != w) ++route_serial_;
                    route_weather_ = w;
                    return true;
                }
            }
            return false;
        }
        if (a == "dex_detail_tab") { // Pokédex detail card: Entry / Stats / Info
            if (argument < 0 || argument > 2) return false;
            dex_detail_tab_ = static_cast<int>(argument);
            return true;
        }
        if (a == "dex_row") {
            if (argument < 0 || argument >= kMaxDexRows) return false;
            dex_sel_row_ = static_cast<int>(argument);
            return true;
        }
        return false;
    }

    // Last SampleBattle()'s view of the active battler, for the direct battle commands
    // (sw_helper_mailbox.h's CheckFight/CheckRun and the FIGHT request's expected move id).
    const sw_helper::BattleView& battle_view() const { return battle_view_; }
    bool native_battle_command_waiting() const { return battle_command_waiting_; }
    // The native top-screen cursor (sw_battle_cursor.h); menu None/-1 = unknown this tick.
    const sw_battle_cursor::Snapshot& native_cursor() const { return native_cursor_; }
    // Ownership-resolved menu Layouts (hide phase 2) and the raid catch prompt, this tick.
    const sw_battle_cursor::Layouts& menu_layouts() const { return menu_layouts_; }
    bool catch_prompt() const { return catch_prompt_; }
    // Native command cells (bit = cursor index) last seen this battle, -1 unknown.
    int command_cells() const { return command_cells_; }
    // Battle rule (client main +0x150: 0 single, 1 double, 2 raid), -1 unknown.
    int battle_rule() const { return battle_rule_; }
    // The top target select may be hidden. While it is open: exactly when the bottom shows its
    // cards (sw.battle.target.live, held through single failed reads; never top hidden with the
    // bottom on its notice). Before it opens (the request must be up before its first frame): the
    // last target select of this battle had its cards and the roster view decoded recently.
    bool target_cursor_ok() const {
        return native_cursor_.menu == sw_battle_cursor::Menu::Target
                   ? target_live_
                   : target_cursor_ok_ && target_sides_ok_ && window_misses_ <= kMenuHoldTicks;
    }
    // A confirmed native menu / the target cards / the roster survive this many consecutive ticks
    // of failed reads (0.5 s at 60 Hz) before they count as gone (owner Thor 2026-11-01 doubles).
    static constexpr int kMenuHoldTicks = 30;
    // The battle client the native cursor snapshot was sampled from (cursor-channel requests).
    u64 native_client() const { return battle_client_; }
    bool SelectFoe(int index) {
        if (index < 0 || index > 1 || (index == 1 && !foe_second_ready_)) return false;
        battle_foe_sel_ = index;
        return true;
    }
    const sw_map_selection::Catalog& map_catalog() const { return map_catalog_; }
    const sw_map_selection::Selection& map_selection() const { return map_selection_; }
    bool map_ready() const { return map_area_ >= 0 && map_catalog_ready_; }
    int battle_screen() const { return battle_screen_; }
    bool InspectBattleMove(int slot) {
        if (battle_screen_ != 1 || slot < 0 || slot >= 4 || !battle_view_.in_battle ||
            !battle_view_.own_ok || !battle_view_.move_id[slot]) return false;
        battle_inspect_move_ = slot;
        battle_move_detail_tab_ = 0;
        return true;
    }
    bool SetBattleMoveDetailTab(int tab) {
        if (battle_screen_ != 1 || tab < 0 || tab > 3) return false;
        battle_move_detail_tab_ = tab;
        return true;
    }
    // Top -> bottom (owner 2026-10-31, "in sync like Lumi"): the bottom follows the native menu
    // on its edges only -- the top opening the move list shows the companion's move list, the
    // top going back to the command menu leaves it -- so a companion-owned screen (ball picker,
    // a Fight tap staged from the bottom) is never overridden while the native menu stays put.
    // The hovered move is the inspected (highlighted) row whenever it changes.
    // The native menu hold (owner Thor 2026-11-01, doubles: the top menu hide and the bottom
    // target cards / notice flickered). A confirmed command menu, move list or target select (its
    // hovered card known) is kept while this tick's sample failed for a read reason only: the
    // client state was unreadable, or still READ as that menu's own state while a later link of
    // the chain failed. The state read as anything else, the battle client changing or leaving
    // the battle ends it at once; kMenuHoldTicks failed ticks in a row end it too (fail safe).
    void HoldNativeMenu(u64 main_base, bool in_battle, bool state_read, bool forced_party,
                        u64 state_fn, std::uint32_t state_step) {
        using sw_battle_cursor::Menu;
        const auto confirmed = [](const sw_battle_cursor::Snapshot& s) {
            return s.menu == Menu::Command || s.menu == Menu::Fight || (s.menu == Menu::Target && s.target >= 0);
        };
        const auto state_is = [&](Menu menu) {
            switch (menu) {
            case Menu::Command:
                return state_fn == main_base + sw_battle_cursor::kCommandWaitFn &&
                       state_step == sw_battle_cursor::kCommandWaitStep;
            case Menu::Fight:
                return state_fn == main_base + sw_battle_cursor::kFightFn && state_step == sw_battle_cursor::kFightWaitStep;
            case Menu::Target:
                return state_fn == main_base + sw_battle_cursor::kFightFn &&
                       state_step == sw_battle_cursor::kFightTargetStep;
            default:
                return false;
            }
        };
        if (confirmed(native_cursor_)) {
            menu_hold_ = {battle_client_, native_cursor_, 0};
            return;
        }
        if (in_battle && battle_client_ && menu_hold_.client == battle_client_ && !forced_party &&
            (!state_read || state_is(menu_hold_.snap.menu)) && menu_hold_.misses < kMenuHoldTicks) {
            ++menu_hold_.misses;
            ++diag_menu_held_;
            native_cursor_ = menu_hold_.snap;
            return;
        }
        // A target select lost to failed reads (not read as over): its cards are gone, so the
        // next request must not pre-arm the top hide for it (fail safe: the top shows).
        if (menu_hold_.snap.menu == Menu::Target && !(state_read && !state_is(Menu::Target)))
            target_cursor_ok_ = false;
        menu_hold_ = {};
    }

    void SyncNativeCursor(const std::array<std::uint16_t, 4>& moves) {
        using sw_battle_cursor::Menu;
        const Menu menu = native_cursor_.menu;
        if (menu != native_menu_last_) {
            if (menu == Menu::Fight && battle_screen_ != 1) SetBattleScreen(1);
            else if (menu == Menu::Command && (native_menu_last_ == Menu::Fight ||
                                               native_menu_last_ == Menu::Target) && battle_screen_ == 1)
                SetBattleScreen(0);
            if (menu == Menu::Target && battle_screen_ != 1) SetBattleScreen(1); // its overlay
            if (menu != Menu::Fight) native_move_last_ = -1;
        }
        native_menu_last_ = menu;
        const int move = native_cursor_.move;
        if (menu == Menu::Fight && move >= 0 && move < 4 && moves[static_cast<std::size_t>(move)] &&
            move != native_move_last_) {
            if (battle_inspect_move_ != move) battle_move_detail_tab_ = 0;
            battle_inspect_move_ = move;
            native_move_last_ = move;
        }
    }
    void SetBattleScreen(int view) {
        if (view == 1 && battle_screen_ != 1) battle_move_detail_tab_ = 0;
        battle_screen_ = view >= 0 && view <= 2 ? view : 0;
        if (battle_screen_ != 2) battle_ball_id_ = 0;
        else if (!battle_ball_id_ && battle_ball_count_) battle_ball_id_ = battle_balls_[0];
    }
    bool CycleBattleBall(int direction) {
        if (battle_screen_ != 2 || battle_ball_count_ == 0) return false;
        int index = 0;
        for (int i = 0; i < battle_ball_count_; ++i) if (battle_balls_[i] == battle_ball_id_) index = i;
        index = (index + (direction > 0 ? 1 : battle_ball_count_ - 1)) % battle_ball_count_;
        battle_ball_id_ = battle_balls_[index];
        return true;
    }
    int battle_ball_id() const { return battle_ball_id_; }
    bool battle_has_balls() const { return battle_ball_count_ > 0; }
    void PublishBattleScreen(const EdenDsmodHostApi& host) {
        PubI(host, "sw.battle.view", battle_screen_);
        PubI(host, "sw.battle.view.command", battle_screen_ == 0 ? 1 : 0);
        PubI(host, "sw.battle.view.moves", battle_screen_ == 1 ? 1 : 0);
        PubI(host, "sw.battle.view.balls", battle_screen_ == 2 ? 1 : 0);
        PubI(host, "sw.battle.move_detail.tab", battle_move_detail_tab_);
        for (int tab = 0; tab < 4; ++tab) {
            const auto key = "sw.battle.move_detail.tab" + std::to_string(tab);
            PubI(host, key.c_str(), battle_move_detail_tab_ == tab ? 1 : 0);
        }
    }
    // The guest helper's signals for this tick (module-owned): `alive` = the battle client's
    // per-frame beat is fresh (the whole battle, animations included), `helper` = the helper is
    // installed and has spoken this boot (then `alive` alone decides when a battle ends),
    // `client` = the beating battle client object (0 = unknown).
    void SetBattleHint(bool alive, bool helper, u64 client) {
        // An emulation stall in battle (no battle beat AND no field frame since the last beat,
        // under kBattleStallTicks): the battle and its client are still there. Keep the client and
        // the battle page's own state (move page, inspected move, Dynamax preview) instead of
        // resetting them -- desktop 2026-11-07: the native Dynamax toggle's asset load silenced the
        // beat, the bottom fell back to the command page and dropped the Max moves.
        if (!alive && battle_client_ && phase_ == Phase::kBattle && helper_seen_ &&
            field_frames_ - battle_end_field_frames_ < kBattleEndFieldFrames &&
            battle_silent_ticks_ < kBattleStallTicks) {
            battle_hint_ = false;
            return;
        }
        if (alive && (!battle_hint_ || client != battle_client_)) ++battle_epoch_; // a new battle
        if (!alive || client != battle_client_) { SetBattleScreen(0); battle_inspect_actor_ = 0; }
        if (!alive || client != battle_client_) battle_command_waiting_ = false;
        if (!alive || client != battle_client_) field_own_side_ = -1;
        battle_hint_ = alive;
        helper_seen_ = helper_seen_ || helper;
        battle_client_ = alive ? client : 0;
    }
    // The guest helper's field frames seen so far this session (FieldStatus::frames, module-owned).
    void SetFieldFrames(std::uint64_t frames) { field_frames_ = frames; }
    // Field actions (2026-10-26): the Bag page's selected item (0 = none) and its live count, the
    // live Repel record, and whether this tick classified the game as in the field.
    int bag_sel_item() const { return bag_sel_item_id_; }
    int bag_sel_count() const { return bag_sel_item_count_; }
    int repel_steps() const { return repel_steps_; }
    bool in_battle_phase() const { return phase_ == Phase::kBattle; }
    // The battle client of this battle, held through an emulation stall (SetBattleHint); 0 = none.
    u64 battle_client() const { return battle_client_; }
    bool in_title_or_loading() const { return phase_ == Phase::kTitle || phase_ == Phase::kLoading; }

    // Companion language (sw_lang.h): the @settings "lang" choice, read by the module each tick.
    // A change drops every language-dependent cache (romfs text tables, the Town Map catalogue's
    // place names) and every change-driven publish recording, so the next Sample re-reads the new
    // language's files and republishes all text. Returns true when the language changed.
    bool SetLanguage(sw_lang::Lang lang) {
        if (lang_known_ && lang == sw_lang::Current()) return false;
        const bool changed = lang_known_ || lang != sw_lang::Current();
        lang_known_ = true;
        sw_lang::SetCurrent(lang);
        if (!changed) return false;
        species_names_attempted_ = species_names_ready_ = false;
        species_names_.clear();
        move_names_attempted_ = move_names_ready_ = false;
        move_names_.clear();
        effectiveness_text_attempted_ = effectiveness_text_ready_ = false;
        effectiveness_text_.clear();
        item_names_attempted_ = item_names_ready_ = false;
        item_names_.clear();
        item_descs_attempted_ = item_descs_ready_ = false;
        item_descs_.clear();
        dex_flavor_attempted_ = dex_flavor_ready_ = false;
        dex_flavor_.clear();
        dex_info_tables_attempted_ = false;
        dex_category_.clear(); dex_height_.clear(); dex_weight_.clear(); dex_ability_names_.clear();
        party_text_attempted_ = false;
        ability_names_.clear(); ability_descs_.clear(); nature_names_.clear(); move_descs_.clear();
        battle_move_text_attempted_ = false;
        field_strings_attempted_ = false;
        field_strings_.clear();
        gmax_strings_attempted_ = false;
        gmax_names_.clear(); gmax_descriptions_.clear();
        map_catalog_attempted_ = map_catalog_ready_ = false;
        map_catalog_ = {};
        map_names_.clear();
        place_names_attempted_ = false;
        battle_tower_name_.clear();
        Republish();
        ++lang_epoch_;
        return true;
    }
    // Drops every change-driven publish recording: the next Sample publishes all text and its
    // measured geometry again (a language change, or the companion font changed under it).
    void Republish() {
        for (Tape* t : {&party_tape_, &live_tape_, &bag_tape_, &dex_tape_, &dens_tape_, &foe_board_tape_,
                        &own_board_tape_, &bench_icon_tape_, &raid_boss_board_tape_, &ball_tape_,
                        &party_detail_tape_, &route_tape_, &wild_tape_})
            t->Clear();
        for (auto& t : raid_mon_tape_) t.Clear();
        for (auto& t : field_side_tape_) t.Clear();
        for (auto& t : switch_pick_tape_) t.Clear();
    }
    // Every published text also goes to `note` (sw_modfont::Font::Note: glyphs the Japanese atlas
    // still lacks, e.g. a nickname).
    void SetTextNote(void (*note)(void*, std::string_view), void* ctx) {
        text_note_ = note;
        text_note_ctx_ = ctx;
    }
    // The game's own words the manifest labels show (F13: strings.json "game" rows, read from the
    // player's romfs by sw_str::LoadGame): sw.str.<key> in the companion language, every tick
    // (the host clears its snapshot per sample). A line the romfs could not give shows the
    // companion's English placeholder.
    void PublishGameStrings(const EdenDsmodHostApi& host) {
        if (!host.publish_text) return;
        static const std::vector<std::pair<sw_str::Id, std::string>> keys = [] {
            std::vector<std::pair<sw_str::Id, std::string>> out;
            for (int i = 0; i < static_cast<int>(sw_str::Id::kCount); ++i)
                if (sw_str::IsGame(static_cast<sw_str::Id>(i)))
                    out.emplace_back(static_cast<sw_str::Id>(i), std::string("sw.str.") + sw_str::kKeys[i]);
            return out;
        }();
        for (const auto& [id, key] : keys) PubT(host, key.c_str(), sw_str::S(id));
    }
    // Bumped on every language change; caches outside the Reader key on it (fly row names).
    std::uint32_t lang_epoch() const { return lang_epoch_; }
    // Fly list row name in the companion language: the zone's Town Map name (script/place_name),
    // Route 10's second landing zone under z_r1001's name, Battle Tower under the Town Map
    // facility name (townmap_facility "townmap_factory_33"), plus the table's own " (2)" landing
    // number. When the language's files cannot be read the row shows its table number ("#17"),
    // the module has no place names of its own (F13).
    std::string FlyName(const EdenDsmodHostApi& host, std::size_t index) {
        if (index >= sw_fly::kDestinations.size()) return {};
        const auto& dest = sw_fly::kDestinations[index];
        EnsurePlaceNames(host);
        std::string name = PlaceNameOf(dest.zone);
        if (name.empty() && dest.zone == sw_town_map::ZoneHash("z_r1002")) name = PlaceNameOf(sw_town_map::ZoneHash("z_r1001"));
        if (name.empty() && dest.zone == sw_town_map::ZoneHash("z_c0306")) name = battle_tower_name_;
        if (name.empty()) return "#" + std::to_string(index + 1);
        if (dest.number > 1) name += " (" + std::to_string(dest.number) + ")";
        return name;
    }
    const char* FlyRegion(std::size_t index) const {
        return index < sw_fly::kDestinations.size() ? sw_str::RegionName(sw_fly::kDestinations[index].region) : "";
    }
    // Live count of a battle item (Medicine + Poké Balls pockets), for the ball picker/commit.
    int BattleItemCount(int item_id) const {
        if (!bag_raw_ok_ || item_id <= 0) return 0;
        for (int p = 0; p < 2; ++p) // Medicine + Poke Balls pockets
            for (std::size_t i = 0; i < sw_item_pouch::kPouches[p].slot_count; ++i) {
                const auto slot = sw_item_pouch::DecodeSlotAt(bag_raw_.data(), bag_raw_.size(), p, i);
                if (slot.item_id == item_id) return slot.count;
            }
        return 0;
    }

private:
    void (*text_note_)(void*, std::string_view) = nullptr;
    void* text_note_ctx_ = nullptr;
    bool lang_known_ = false;
    std::uint32_t lang_epoch_ = 0;
    // --- change-driven publishing (review H3) ------------------------------------------------
    // Every publish in this class goes through PubI/PubT, which also record into `tape_` while a
    // domain renders inside Taped(): an unchanged signature replays the recording instead.
    Tape* tape_ = nullptr;
    std::uint64_t tick_ = 0;
    static constexpr std::uint64_t kSlowRefreshTicks = 15; // dex/dens re-read at most every 0.25 s

    void PubI(const EdenDsmodHostApi& host, const char* key, std::int64_t v) {
        if (host.publish_i64) host.publish_i64(host.userdata, key, v);
        if (tape_) tape_->ints.emplace_back(key, v);
    }
    // --- dev.19: the HP the top screen shows (sw_battle_gauge.h) ------------------------------
    sw_battle_gauge::Scanner gauge_scan_{};
    sw_battle_gauge::Follower gauge_own_{}, gauge_foe_{};
    std::uint64_t gauge_claimed_[2]{};
    int FollowHp(const EdenDsmodHostApi& host, u64 heap_begin, sw_battle_gauge::Follower& f,
                 const sw_battle::Mon& m, bool ok, bool in_battle) {
        using namespace sw_battle_gauge;
        if (!in_battle) { gauge_scan_.Reset(); gauge_passes_ = 0; gauge_own_.Reset(); gauge_foe_.Reset(); gauge_claimed_[0] = gauge_claimed_[1] = 0; return -1; }
        if (!ok) return -1;
        if (f.species != m.species) { // a new battler: no animation from the previous one
            if (f.plate) for (auto& c : gauge_claimed_) if (c == f.plate) c = 0;
            f.Reset();
            f.species = m.species;
            f.shown = m.cur_hp;
            gauge_scan_.Reset();
            gauge_passes_ = 0;
        }
        int target = m.cur_hp;
        if (f.plate) {
            std::uint8_t raw[0x40];
            Plate p;
            const bool live = ReadStable(host, f.plate, raw, sizeof raw) && ParsePlate(raw, p) && p.max == m.max_hp;
            // a plate that still disagrees with the battle struct ~8 s later is a stale copy (an
            // earlier battle's plate): drop it for good and follow the battle struct instead
            f.mismatch = live && static_cast<int>(p.target) != m.cur_hp ? f.mismatch + 1 : 0;
            if (live && f.mismatch < 480) {
                target = static_cast<int>(p.target);
            } else {
                if (live) f.rejected = f.plate;
                for (auto& c : gauge_claimed_) if (c == f.plate) c = 0;
                f.plate = 0;
            }
        }
        if (&f == &gauge_own_ && (!gauge_own_.plate || !gauge_foe_.plate)) { // one scan pass serves both battlers (own drives it)
            // the plates are created when the HP boxes first slide in, after the intro: a finished
            // pass that missed a plate starts over after ~1 s
            // At most kGaugeMaxPasses full passes per battler (review 2026-10-31: an unbounded
            // restart kept a 4 MiB/tick scan running for a whole battle when the plate window
            // missed); after that the battle struct's HP (still animated) is used.
            if (gauge_scan_.done && gauge_passes_ < kGaugeMaxPasses && ++gauge_retry_ >= 60) {
                gauge_retry_ = 0; ++gauge_passes_; gauge_scan_.Reset();
            }
            gauge_scan_.Advance(heap_begin,
                // one plain read per chunk (a 64 KiB live heap window is never byte-stable across two
                // reads); every candidate is re-read with ReadStable before it is trusted
                [&](u64 a, void* d, std::size_t n) {
                    return host.is_mapped && host.read_memory && host.is_mapped(host.userdata, a, n) &&
                           host.read_memory(host.userdata, a, d, n);
                },
                [&](u64 a, const Plate& candidate) {
                    std::uint8_t again[0x40];
                    Plate p;
                    if (!ReadStable(host, a, again, sizeof again) || !ParsePlate(again, p) ||
                        p.max != candidate.max) return;
                    for (auto* g : {&gauge_own_, &gauge_foe_}) {
                        const bool own_side = g == &gauge_own_;
                        const int want_max = own_side ? own_max_seen_ : foe_max_seen_;
                        const int want_hp = own_side ? own_hp_seen_ : foe_hp_seen_;
                        if (g->plate || g->rejected == a || static_cast<int>(p.max) != want_max || static_cast<int>(p.target) != want_hp) continue;
                        if (gauge_claimed_[0] == a || gauge_claimed_[1] == a) continue;
                        g->plate = a;
                        gauge_claimed_[own_side ? 0 : 1] = a;
                        break;
                    }
                });
        }
        if (&f == &gauge_own_) { own_max_seen_ = m.max_hp; own_hp_seen_ = m.cur_hp; }
        else { foe_max_seen_ = m.max_hp; foe_hp_seen_ = m.cur_hp; }
        if (target != f.last_target) { f.span = f.shown >= 0 ? target - f.shown : 0; f.last_target = target; }
        f.shown = Step(f.shown, target, f.span);
        PubI(host, &f == &gauge_own_ ? "sw.battle.own.gauge_source" : "sw.battle.foe.gauge_source", f.plate ? 1 : 0);
        return f.shown;
    }
    int gauge_retry_ = 0;
    int gauge_passes_ = 0;                       // restarts used for the current battler
    static constexpr int kGaugeMaxPasses = 2;    // 1 initial pass + 2 restarts
    int own_max_seen_ = 0, own_hp_seen_ = -1, foe_max_seen_ = 0, foe_hp_seen_ = -1;

    // --- dev.19: battle stats, stat stages and gender from the bpp (LIVE-READERS-132 dev.19) --
    // bpp +0x2A2..+0x2AA u16 Attack/Defense/Sp.Atk/Sp.Def/Speed (live: Pikachu Lv100 146/116/
    // 136/136/216 = the exact Gen 8 values for its 31 IVs, 0 EVs, neutral nature), +0x2AC/+0x2AD
    // battle types, +0x2AF gender (0 male, 1 female, 2 none; Pikachu male, Stunky female),
    // +0x2B4..+0x2BA stat stages Attack, Defense, Sp.Atk, Sp.Def, Speed, accuracy, evasion with 6
    // = no change (all seven read 6 at battle start, both sides).
    static int StagedStat(int raw, int stage) {
        const int n = stage >= 0 ? 2 + stage : 2, d = stage >= 0 ? 2 : 2 - stage;
        return raw * n / d;
    }
    void PublishBattleStats(const EdenDsmodHostApi& host, const std::string& side, const std::uint8_t* raw,
                            bool ok, int shown_hp) {
        static const char* kStage[2] = {"accuracy", "evasion"};
        const auto pi = [&](const std::string& k, std::int64_t v) { PubI(host, (side + k).c_str(), v); };
        const auto pt = [&](const std::string& k, const std::string& v) { PubT(host, (side + k).c_str(), v.c_str()); };
        const int gender = ok ? raw[0x2AF] : -1;
        pi(".gender_ready", ok && gender >= 0 && gender <= 1 ? 1 : 0);
        pi(".gender", ok ? gender : -1);
        std::string accev;
        for (int k = 0; k < 6; ++k) {
            const std::string q = ".st" + std::to_string(k) + ".";
            const int stage = k == 0 || !ok ? 0 : static_cast<int>(raw[0x2B4 + k - 1]) - 6;
            const bool stage_ok = stage >= -6 && stage <= 6;
            pi(q + "ready", ok ? 1 : 0);
            if (!ok) { pi(q + "up", 0); pi(q + "down", 0); continue; }
            std::string text;
            if (k == 0) {
                text = std::to_string(std::max(shown_hp, 0)) + "/" + std::to_string(sw_battle::Rd16(raw + 0x72));
            } else {
                const int v = sw_battle::Rd16(raw + 0x2A2 + (k - 1) * 2);
                text = std::to_string(v);
                if (stage_ok && stage != 0) text += " » " + std::to_string(StagedStat(v, stage));
            }
            pt(q + "text", text);
            pt(q + "stage", stage > 0 ? "+" + std::to_string(stage) : std::to_string(stage));
            pi(q + "up", stage_ok && stage > 0);
            pi(q + "down", stage_ok && stage < 0);
        }
        for (int k = 0; ok && k < 2; ++k) {
            const int stage = static_cast<int>(raw[0x2B9 + k]) - 6;
            if (stage == 0 || stage < -6 || stage > 6) continue;
            if (!accev.empty()) accev += "   ";
            accev += std::string(sw_str::S(k == 0 ? sw_str::Id::kStageAccuracy : sw_str::Id::kStageEvasion)) + (stage > 0 ? "+" : "") + std::to_string(stage);
        }
        (void)kStage;
        pt(".accev", accev);
        pi(".accev_on", accev.empty() ? 0 : 1);
    }

    // The Bag pocket's name: the game's own (bag_pocket.dat) in the companion language.
    static const char* PocketName(int pocket) {
        using sw_str::Id;
        static constexpr Id k[] = {Id::kBagPocket0, Id::kBagPocket1, Id::kBagPocket2, Id::kBagPocket3, Id::kBagPocket4,
                                   Id::kBagPocket5, Id::kBagPocket6, Id::kBagPocket7, Id::kBagPocket8};
        return pocket >= 0 && pocket < 9 ? sw_str::S(k[pocket]) : "";
    }
    // gen_manifest.py dex evolution column: method label wrap width (pitch 180 - 12) at scale 4.
    static constexpr int kEvoMethodWidth = 168;
    static const char* MoveCategoryText(int category) {
        return sw_str::S(category == 0 ? sw_str::Id::kMoveCatStatus
                         : category == 1 ? sw_str::Id::kMoveCatPhysical : sw_str::Id::kMoveCatSpecial);
    }
    static int PpTier(int pp, int max) {
        if (max <= 0 || pp <= 0) return max > 0 ? 3 : 0;
        if (pp * 4 <= max) return 2;
        if (pp * 2 <= max) return 1;
        return 0;
    }
    void PubT(const EdenDsmodHostApi& host, const char* key, const char* v) {
        // Japanese game text separates phrases with U+3000; the runtime wraps at U+0020 only (a
        // run without one is split anywhere), so those become the line-break opportunities.
        // Chinese has no word spaces: a space between two Han characters (a flattened game line
        // break) goes, and the runtime wraps the run by character (sw_text_fit JoinCjkSpaces).
        std::string spaced;
        if (v && sw_lang::IsJapanese(sw_lang::Current()) && std::strstr(v, "\xE3\x80\x80")) {
            spaced = sw_text_fit::IdeographicSpacesToBreaks(v);
            v = spaced.c_str();
        } else if (v && sw_lang::IsChinese(sw_lang::Current()) && std::strchr(v, ' ')) {
            spaced = sw_text_fit::JoinCjkSpaces(v);
            v = spaced.c_str();
        }
        if (host.publish_text) host.publish_text(host.userdata, key, v);
        if (text_note_ && v) text_note_(text_note_ctx_, v);
        if (tape_) tape_->texts.emplace_back(key, v ? v : "");
        PublishTextGeometry(host, key, v ? v : "");
    }
    // Owner dev.23: where text ends, measured with the package font's own advances
    // (sw_font_metrics.h == the runtime's Canvas::MeasureText). The battle header's gender mark
    // follows the name (".name_w", px44 = text_scale 5); a move row centres its name and
    // effectiveness when the effectiveness fits one line (".eff_2l", px34 = scale 4 in the
    // 220 px lane of the battle card).
    static constexpr int kEffLane = 220;
    void PublishTextGeometry(const EdenDsmodHostApi& host, std::string_view key, std::string_view v) {
        if (!host.publish_i64) return;
        constexpr std::string_view eff = "effectiveness_text";
        if (key == "sw.battle.own.name" || key == "sw.battle.foe.name") {
            const std::string k = std::string(key) + "_w";
            PubI(host, k.c_str(), sw_font::MeasureText(v, 5));
        } else if (key.size() > eff.size() && key.substr(key.size() - eff.size()) == eff) {
            const std::string k = std::string(key.substr(0, key.size() - eff.size())) + "eff_2l";
            PubI(host, k.c_str(), sw_font::MeasureText(v, 4) > kEffLane ? 1 : 0);
        }
    }
    template <class Render>
    void Taped(Tape& tape, std::uint64_t sig, const EdenDsmodHostApi& host, Render&& render) {
        if (tape.valid && tape.sig == sig) {
            tape.Replay(host);
            return;
        }
        tape.Clear();
        Tape* outer = tape_;
        tape_ = &tape;
        render();
        tape_ = outer;
        tape.sig = sig;
        tape.valid = true;
    }
    Tape party_tape_, live_tape_, bag_tape_, dex_tape_, dens_tape_, foe_board_tape_, own_board_tape_;
    // Battle-domain identity caches (review OPTIMIZE #8): species/form/move identity rarely
    // changes tick-to-tick during a battle, while HP/PP bars must redraw every tick -- each of
    // these tapes caches only the identity-derived publish (names/icons/type boards/field-state
    // text), keyed on a signature that excludes raw HP/PP bytes; the always-fresh HP/PP/fainted
    // numbers are published outside the Taped() block, every call, regardless of cache hit.
    Tape bench_icon_tape_, raid_boss_board_tape_, ball_tape_;
    std::array<Tape, 5> raid_mon_tape_;
    std::array<Tape, 2> field_side_tape_;
    std::array<Tape, 3> switch_pick_tape_;

    // --- romfs-derived records, cached for the Reader's lifetime (static game data) -----------
    // personal_total.bin types / base HP / growth per species, waza records per move: one romfs
    // read the first time each is needed, never again (a failure is cached too, never retried).
    struct SpeciesRec {
        std::int8_t types = 0, stats = 0; // 0 not asked yet, 1 ok, -1 failed
        std::uint8_t t1 = 0, t2 = 0, base_hp = 0, growth = 0;
    };
    std::vector<SpeciesRec> species_recs_;
    struct MoveRec {
        std::int8_t state = 0;
        sw_move_data::MoveRecord rec{};
    };
    std::vector<MoveRec> move_recs_;

    SpeciesRec* SpeciesSlot(std::uint16_t species) {
        if (species < 1 || species > sw_species_types::kMaxKnownSpecies) return nullptr;
        if (species_recs_.empty()) species_recs_.resize(sw_species_types::kMaxKnownSpecies + 1);
        return &species_recs_[species];
    }
    bool CachedTypes(const EdenDsmodHostApi& host, std::uint16_t species, std::uint8_t& t1, std::uint8_t& t2) {
        SpeciesRec* r = SpeciesSlot(species);
        if (!r) return false;
        if (r->types == 0) r->types = sw_species_types::GetTypes(host, species, r->t1, r->t2) ? 1 : -1;
        if (r->types != 1) return false;
        t1 = r->t1;
        t2 = r->t2;
        return true;
    }
    bool CachedBaseHpAndGrowth(const EdenDsmodHostApi& host, std::uint16_t species, std::uint8_t& hp,
                               std::uint8_t& growth) {
        SpeciesRec* r = SpeciesSlot(species);
        if (!r) return false;
        if (r->stats == 0)
            r->stats = sw_personal_stats::GetBaseHpAndGrowth(host, species, r->base_hp, r->growth) ? 1 : -1;
        if (r->stats != 1) return false;
        hp = r->base_hp;
        growth = r->growth;
        return true;
    }
    bool CachedMove(const EdenDsmodHostApi& host, std::uint16_t move_id, sw_move_data::MoveRecord& out) {
        if (move_id == 0 || move_id > sw_move_data::kMaxMoveId) return false;
        if (move_recs_.empty()) move_recs_.resize(sw_move_data::kMaxMoveId + 1);
        auto& m = move_recs_[move_id];
        if (m.state == 0) m.state = sw_move_data::ReadMove(host, move_id, m.rec) ? 1 : -1;
        if (m.state != 1) return false;
        out = m.rec;
        return true;
    }

    sw_helper::BattleView battle_view_{};
    bool battle_hint_ = false;
    bool helper_seen_ = false;
    std::uint64_t field_frames_ = 0;            // FieldStatus::frames (SetFieldFrames)
    std::uint64_t battle_end_field_frames_ = 0; // field_frames_ at the battle beat's last tick
    int battle_silent_ticks_ = 0;
    u64 battle_client_ = 0;
    // Bumped at every battle start (a new beating client, or in_battle rising): per-battle caches key on it.
    std::uint64_t battle_epoch_ = 0;
    bool epoch_in_battle_ = false;
    bool battle_command_waiting_ = false;
    sw_battle_cursor::Snapshot native_cursor_{};
    sw_battle_cursor::Layouts menu_layouts_{};
    sw_battle_cursor::LayoutCache layout_cache_{};
    bool catch_prompt_ = false;
    int command_cells_ = -1;
    u64 command_cells_client_ = 0;
    int battle_rule_ = -1;
    bool target_cursor_ok_ = true;
    bool target_sides_ok_ = false;
    bool target_live_ = false;
    // The hold (kMenuHoldTicks): the last confirmed native menu snapshot, the last complete target
    // cards (a tape replayed while held), the last roster decode and catch-prompt reading.
    struct MenuHold {
        u64 client = 0;
        sw_battle_cursor::Snapshot snap{};
        int misses = 0;
    } menu_hold_{};
    Tape target_cards_;
    u64 target_cards_client_ = 0;
    int target_cards_misses_ = 0;
    struct RootHold {
        u64 client = 0, own_base = 0, party_base = 0;
        int source = 1, misses = 0;
    } root_hold_{};
    sw_battle_foe::Snapshot sides_last_{};
    u64 sides_client_ = 0;
    int sides_misses_ = 0;
    int window_misses_ = 0; // consecutive in-battle ticks the client state was unreadable
    u64 catch_client_ = 0;
    int catch_known_ = -1, catch_misses_ = 0;
    // Diagnostics (cumulative ticks; sw.battle.hold.*): how often a hold bridged a failed read.
    std::int64_t diag_menu_held_ = 0, diag_cards_held_ = 0, diag_sides_fail_ = 0, diag_window_fail_ = 0;
    int battle_foe_sel_ = 0;        // doubles: which active foe the Foe tab / matchups show
    bool foe_second_ready_ = false;
    int native_target_last_ = -1;
    sw_battle_cursor::Menu native_menu_last_ = sw_battle_cursor::Menu::None;
    int native_move_last_ = -1;
    int battle_screen_ = 0;
    int battle_ball_id_ = 0;
    int battle_ball_count_ = 0;
    std::array<int, 30> battle_balls_{};
    bool ball_screen_native_ = false;
    int battle_inspect_move_ = 0;
    int battle_move_detail_tab_ = 0;
    u64 battle_inspect_actor_ = 0;
    std::array<std::uint16_t, 4> battle_inspect_ids_{};
    bool battle_move_text_attempted_ = false;
    int field_own_side_ = -1;
    int battle_tab_ = 0;       // 0 foe, 1 own, 2 Field
    int active_idx_ = 0;       // own active battler's bpp index (= pokeID), sticky within a battle
    std::vector<std::uint8_t> bag_raw_;
    bool bag_raw_ok_ = false;
    int battle_end_ticks_ = 0;

    void EnterBattle() {
        phase_ = Phase::kBattle;
        battle_silent_ticks_ = 0;
        battle_end_field_frames_ = field_frames_;
        battle_tab_ = 0;
        SetBattleScreen(0);
        battle_ball_count_ = 0;
        active_idx_ = 0;
        battle_end_ticks_ = 0;
    }
    // Companion-owned selection state (see DispatchAction above). Defaults match the native
    // UI's own fresh-open behaviour: pocket 0 (Medicine, the first native pocket tab) / region 0
    // (Galar) / row 0.
    int bag_sel_pocket_ = 0;
    int bag_sel_row_ = 0;
    int bag_sel_item_id_ = 0;    // SampleBag: the selected row's item (0 = none / not read)
    int bag_sel_item_count_ = 0;
    int repel_steps_ = -1;       // SampleRepel: steps left (-1 = not read)
    int dex_sel_region_ = 0;
    int dex_sel_row_ = 0;
    std::array<bool, 3> dex_region_available_{true, false, false};
    static constexpr int kMaxBagRows = 256;  // covers every pocket in the verified fixture fully
                                              // (largest real pocket, Items, has 242 owned slots)
    static constexpr int kMaxDexRows = 400;  // the largest region (Galar); Armor/Crown use fewer
    static constexpr int kMaxEvoMembers = sw_evolution::kMaxChainMembers; // the Pokédex detail
                                              // card's evolution-row slot cap -- see sw_evolution.h
    // Species -> English name table (sw_species_names.h), loaded once from the player's own
    // romfs and cached for the Reader's lifetime -- static game data, not per-tick state, so
    // there is no reason to re-read and re-decrypt monsname.dat every sample. A load failure
    // (old host, missing file) leaves `species_names_ready_` false forever for this boot; callers
    // must treat that as "no name available" and fall back, never retry-spam read_romfs.
    std::vector<std::string> species_names_;
    bool species_names_ready_ = false;
    bool species_names_attempted_ = false;

    void EnsureSpeciesNames(const EdenDsmodHostApi& host) {
        if (species_names_attempted_) return;
        species_names_attempted_ = true;
        species_names_ready_ = sw_species_names::LoadNames(host, species_names_);
    }

    // Move id -> English name table (sw_move_names.h), same "load once, cache for the Reader's
    // lifetime" treatment as species names above.
    std::vector<std::string> move_names_;
    bool move_names_ready_ = false;
    bool move_names_attempted_ = false;

    void EnsureMoveNames(const EdenDsmodHostApi& host) {
        if (move_names_attempted_) return;
        move_names_attempted_ = true;
        move_names_ready_ = sw_move_names::LoadNames(host, move_names_);
    }

    // Native effectiveness-tier label text (sw_effectiveness_text.h), same "load once, cache for
    // the Reader's lifetime" treatment as species/move names above.
    std::vector<std::string> effectiveness_text_;
    bool effectiveness_text_ready_ = false;
    bool effectiveness_text_attempted_ = false;

    void EnsureEffectivenessText(const EdenDsmodHostApi& host) {
        if (effectiveness_text_attempted_) return;
        effectiveness_text_attempted_ = true;
        effectiveness_text_ready_ = sw_effectiveness_text::LoadStrings(host, effectiveness_text_);
    }

    // Item id -> English name (sw_item_names.h) and -> description (sw_item_descriptions.h),
    // same "load once from the player's own romfs, cache for the Reader's lifetime" treatment as
    // species/move names above (static game data, not per-tick state).
    std::vector<std::string> item_names_;
    bool item_names_ready_ = false;
    bool item_names_attempted_ = false;
    void EnsureItemNames(const EdenDsmodHostApi& host) {
        if (item_names_attempted_) return;
        item_names_attempted_ = true;
        item_names_ready_ = sw_item_names::LoadNames(host, item_names_);
    }

    std::vector<std::string> item_descs_;
    bool item_descs_ready_ = false;
    bool item_descs_attempted_ = false;
    void EnsureItemDescriptions(const EdenDsmodHostApi& host) {
        if (item_descs_attempted_) return;
        item_descs_attempted_ = true;
        item_descs_ready_ = sw_item_descriptions::LoadDescriptions(host, item_descs_);
    }

    // Species -> Pokédex flavor text (sw_dex_text.h), same cache treatment.
    std::vector<std::string> dex_flavor_;
    bool dex_flavor_ready_ = false;
    bool dex_flavor_attempted_ = false;
    // Pokédex detail depth (sw_dex_info.h): category/height/weight/ability-name tables, loaded
    // once; the selected species' personal record, re-read only when the species changes.
    std::vector<std::string> dex_category_, dex_height_, dex_weight_, dex_ability_names_;
    bool dex_info_tables_attempted_ = false;
    int dex_detail_tab_ = 0;
    std::uint16_t dex_info_species_ = 0;
    bool dex_info_ok_ = false;
    sw_dex_info::Record dex_info_{};
    void EnsureDexInfoTables(const EdenDsmodHostApi& host) {
        if (dex_info_tables_attempted_) return;
        dex_info_tables_attempted_ = true;
        sw_dex_info::LoadDat(host, sw_lang::MessagePath(sw_dex_info::kCategoryFile).c_str(), dex_category_);
        sw_dex_info::LoadDat(host, sw_lang::MessagePath(sw_dex_info::kHeightFile).c_str(), dex_height_);
        sw_dex_info::LoadDat(host, sw_lang::MessagePath(sw_dex_info::kWeightFile).c_str(), dex_weight_);
        sw_dex_info::LoadDat(host, sw_lang::MessagePath(sw_dex_info::kAbilityFile).c_str(), dex_ability_names_);
    }
    void PublishDexInfo(const EdenDsmodHostApi& host, std::uint16_t species) {
        EnsureDexInfoTables(host);
        if (species != dex_info_species_) {
            dex_info_species_ = species;
            dex_info_ok_ = sw_dex_info::ReadRecord(host, species, dex_info_);
        }
        const auto T = [&](const char* k, const std::string& v) {
            if (host.publish_text) PubT(host, k, v.c_str());
        };
        const auto& r = dex_info_;
        T("sw.dex.detail.category", sw_dex_info::Line(dex_category_, species));
        T("sw.dex.detail.height", sw_dex_info::Line(dex_height_, species));
        T("sw.dex.detail.weight", sw_dex_info::Line(dex_weight_, species));
        PubI(host, "sw.dex.detail.info_ready", dex_info_ok_ ? 1 : 0);
        if (!dex_info_ok_) return;
        // Display order HP, Attack, Defense, Sp. Atk, Sp. Def, Speed (file order HP Atk Def Spe SpA SpD).
        static constexpr int kOrder[6] = {0, 1, 2, 4, 5, 3};
        int total = 0;
        char key[48];
        for (int k = 0; k < 6; ++k) {
            const int v = r.base[static_cast<std::size_t>(kOrder[k])];
            total += v;
            std::snprintf(key, sizeof key, "sw.dex.detail.bs%d", k);
            PubI(host, key, v);
        }
        PubI(host, "sw.dex.detail.bs_total", total);
        std::string regular, hidden;
        sw_dex_info::Abilities(r, dex_ability_names_, regular, hidden);
        T("sw.dex.detail.abilities", regular);
        T("sw.dex.detail.hidden_ability", hidden);
        PubI(host, "sw.dex.detail.abilities_ready", regular.empty() ? 0 : 1);
        PubI(host, "sw.dex.detail.hidden_ready", hidden.empty() ? 0 : 1);
        T("sw.dex.detail.gender", sw_dex_info::Gender(r.gender));
        T("sw.dex.detail.egg_groups", sw_dex_info::EggGroups(r));
        T("sw.dex.detail.catch_rate", std::to_string(r.catch_rate));
        T("sw.dex.detail.ev_yield", sw_dex_info::EvYield(r));
        T("sw.dex.detail.base_exp", std::to_string(r.base_exp));
        T("sw.dex.detail.hatch", sw_str::Format(sw_str::Id::kDexHatchCycles, r.hatch));
        T("sw.dex.detail.growth", sw_dex_info::GrowthName(r.growth));
        T("sw.dex.detail.friendship", std::to_string(r.friendship));
    }

    void EnsureDexFlavorText(const EdenDsmodHostApi& host) {
        if (dex_flavor_attempted_) return;
        dex_flavor_attempted_ = true;
        dex_flavor_ready_ = sw_dex_text::LoadFlavorText(host, dex_flavor_);
    }

    // Region dex index (1-based "No. NNN") -> species, read from the personal table once
    // (sw_dex_index.h). Static game data, same cache treatment.
    sw_dex_index::ReverseIndex dex_reverse_{};
    bool dex_reverse_ready_ = false;
    bool dex_reverse_attempted_ = false;
    void EnsureDexReverseIndex(const EdenDsmodHostApi& host) {
        if (dex_reverse_attempted_) return;
        dex_reverse_attempted_ = true;
        dex_reverse_ready_ = sw_dex_index::BuildReverseIndex(host, dex_reverse_);
    }

    // Evolution chains (sw_evolution.h): ~900 small evo_NNN.bin files, loaded kEvoFilesPerTick
    // per sample from the first tick on (pre-Thor review M5: the old one-shot load stalled the
    // timing thread for >1 s on the first Pokedex tick). Static game data, kept for the Reader's
    // lifetime; the Pokedex evolution row stays empty until `evo_table_ready_`.
    sw_evolution::Table evo_table_{};
    bool evo_table_ready_ = false;
    bool evo_table_any_ = false;
    int evo_next_species_ = 1;
    static constexpr int kEvoFilesPerTick = 48;
    void StepEvolutionTable(const EdenDsmodHostApi& host) {
        if (evo_next_species_ > sw_evolution::kMaxSpecies) return;
        if (!host.read_romfs) {
            evo_next_species_ = sw_evolution::kMaxSpecies + 1;
            return;
        }
        for (int k = 0; k < kEvoFilesPerTick && evo_next_species_ <= sw_evolution::kMaxSpecies; ++k)
            evo_table_any_ = sw_evolution::LoadSpecies(host, evo_table_, evo_next_species_++) || evo_table_any_;
        if (evo_next_species_ > sw_evolution::kMaxSpecies) evo_table_ready_ = evo_table_any_;
    }

    // Word-wraps a decoded message-table string into up to `max_lines` lines of at most
    // `max_chars` characters each, for a manifest label widget that is NOT asked to wrap/shrink
    // itself (gen_manifest.py's T() helper only offers a single-line shrink-then-ellipsize mode,
    // whose floor equals its own target scale at the small body-text tier this module uses, i.e.
    // it never actually shrinks -- any line wider than the box ellipsizes immediately). The
    // game's own authored line breaks (the literal 2-char "\n" marker
    // sw_text::ParseLineString emits, not a real '\n' byte) are first collapsed to spaces: they
    // were tuned for the NATIVE text box's own width, not this module's, so re-flowing from
    // scratch (the same choice this project's own design-mockup renderer made, kit.py's
    // `fit_lines()`) gives a more reliable fit than trusting breaks sized for a different box.
    // A greedy word-wrap, never splitting a word; if content remains after `max_lines`, the last
    // line gets a trailing "..." (character-budget based, not pixel-measured -- this module has
    // no font-metrics access -- tuned empirically against the real engine font in the harness).
    // Preserve the whole native message; the runtime can wrap it using real font metrics.
    static std::string FlatGameText(const std::string& text) {
        std::string out;
        out.reserve(text.size());
        for (std::size_t i = 0; i < text.size();) {
            if (text.compare(i, 2, "\\n") == 0) {
                if (!out.empty() && out.back() != ' ') out += ' ';
                i += 2;
            } else if (text[i] == '\n' || text[i] == '\r') {
                if (!out.empty() && out.back() != ' ') out += ' ';
                ++i;
            } else out += text[i++];
        }
        return out;
    }

    // This tick's decoded LIVE party (SampleLiveParty), indexed like the live array -- the same
    // index the game's own Bag uses for a "use on" target (CanUse/Apply take it). Consumed by
    // the field medicine target check (live_party_ec), the phase machine (AnyLivePartySlotValid)
    // and the battle bench's status dot (species-matched against the bpp).
    struct LiveSlot {
        std::uint16_t species = 0;
        std::uint8_t level = 0;
        std::uint16_t current_hp = 0;
        std::uint16_t max_hp = 0;
        std::uint32_t status = 0;
        std::uint32_t ec = 0; // encryption constant (PK8 +0x00, unencrypted): the slot's identity
        bool valid = false;
        bool egg = false;     // valid for reorder only: never a medicine/battle/summary target
        bool shiny = false;   // never set for an Egg
    };
    std::array<LiveSlot, 6> live_party_cache_{};
    bool live_party_ok_ = false; // the live array's root and count read this tick
    PartyArray live_party_array_{};
    bool live_party_empty_ = false; // root read this tick with a clean count of 0 (new game, no starter yet)
    std::array<sw_pk8::LivePartyMon, 6> party_detail_mons_{};
    int party_detail_index_ = -1, party_detail_tab_ = 0, party_detail_move_index_ = -1;
    std::uint16_t party_detail_move_id_ = 0;
    std::uint32_t party_detail_ec_ = 0;
    std::uint64_t party_detail_live_sig_ = 0;
    Tape party_detail_tape_;
    struct DetailPersonalCache {
        std::uint32_t key = 0;
        sw_party_details::Personal record{};
        bool valid = false;
    };
    std::vector<DetailPersonalCache> detail_personals_;
    bool party_text_attempted_ = false;
    std::vector<std::string> ability_names_, ability_descs_, nature_names_, move_descs_;

    bool CachedDetailPersonal(const EdenDsmodHostApi& host, const sw_pk8::LivePartyMon& mon,
                              sw_party_details::Personal& out) {
        const std::uint32_t key = static_cast<std::uint32_t>(mon.species) * 256 + mon.form;
        for (const auto& c : detail_personals_) if (c.key == key) {
            if (c.valid) out = c.record;
            return c.valid;
        }
        DetailPersonalCache c;
        c.key = key;
        c.valid = sw_party_details::LoadPersonal(host, mon.species, mon.form, c.record);
        detail_personals_.push_back(c);
        if (c.valid) out = c.record;
        return c.valid;
    }

    void SamplePartyDetails(const EdenDsmodHostApi& host) {
        if (party_detail_index_ >= 0) {
            const auto i = static_cast<std::size_t>(party_detail_index_);
            if (!live_party_ok_ || !live_party_cache_[i].valid || party_detail_mons_[i].egg ||
                live_party_cache_[i].ec != party_detail_ec_) party_detail_index_ = -1;
        }
        if (party_detail_index_ < 0 || party_detail_tab_ != 1 ||
            (party_detail_move_index_ >= 0 &&
             party_detail_mons_[static_cast<std::size_t>(party_detail_index_)].moves[party_detail_move_index_] != party_detail_move_id_))
            party_detail_move_index_ = -1;
        PubI(host, "sw.party.detail.move_open", party_detail_move_index_ >= 0 ? 1 : 0);
        PubI(host, "sw.party.detail.move_index", party_detail_move_index_);
        for (int i = 0; i < 4; ++i) {
            const auto key = "sw.party.detail.move_selected" + std::to_string(i);
            PubI(host, key.c_str(), party_detail_move_index_ == i ? 1 : 0);
        }
        PubI(host, "sw.party.detail.open", party_detail_index_ >= 0 ? 1 : 0);
        for (int i = 0; i < 6; ++i) {
            const std::string key = "sw.party." + std::to_string(i) + ".detail_selected";
            PubI(host, key.c_str(), party_detail_index_ == i ? 1 : 0);
        }
        if (party_detail_index_ < 0) return;
        auto sig = HashValue(static_cast<std::uint64_t>(party_detail_index_), party_detail_live_sig_);
        sig = HashValue(static_cast<std::uint64_t>(party_detail_tab_), sig);
        Taped(party_detail_tape_, sig, host, [&] {
            const auto& slot = live_party_cache_[static_cast<std::size_t>(party_detail_index_)];
            const auto& mon = party_detail_mons_[static_cast<std::size_t>(party_detail_index_)];
            const std::string prefix = "sw.party.detail.";
            auto pi = [&](const std::string& key, std::int64_t value) { PubI(host, (prefix + key).c_str(), value); };
            auto pt = [&](const std::string& key, const std::string& value) { PubT(host, (prefix + key).c_str(), value.c_str()); };
            pi("index", party_detail_index_);
            pi("tab", party_detail_tab_);
            for (int j = 0; j < 3; ++j) pi("tab" + std::to_string(j), party_detail_tab_ == j ? 1 : 0);
            std::string name;
            const bool species_ok = species_names_ready_ && sw_species_names::SpeciesName(species_names_, mon.species, name);
            if (species_ok) pt("species_name", name);
            if (sw_font::CoversText(mon.nickname)) pt("name", mon.nickname); // the native summary's nickname
            else if (species_ok) pt("name", name);
            PublishIcon(host, "sw.party.detail", mon.species);
            pi("shiny", slot.shiny ? 1 : 0);   // never set for an Egg (no detail view for one either)
            pt("level_text", sw_str::Level(slot.level));
            pt("hp_text", std::to_string(slot.current_hp) + "/" + std::to_string(slot.max_hp));
            pt("gender_text", mon.gender == 0 ? sw_str::S(sw_str::Id::kGenderMale) : mon.gender == 1 ? sw_str::S(sw_str::Id::kGenderFemale)
                               : mon.gender == 2 ? sw_str::S(sw_str::Id::kGenderNone) : "—");
            using sw_str::Id;
            const Id statuses[]{Id::kStatusHealthy, Id::kStatusParalyzed, Id::kStatusAsleep, Id::kStatusFrozen,
                                Id::kStatusBurned, Id::kStatusPoisoned};
            pt("status_text", slot.current_hp == 0 ? sw_str::S(Id::kStatusFainted) : mon.status <= 5 ? sw_str::S(statuses[mon.status]) : "—");
            if (!party_text_attempted_) {
                party_text_attempted_ = true;
                sw_party_details::LoadText(host, sw_lang::MessagePath("tokusei.dat").c_str(), ability_names_);
                sw_party_details::LoadText(host, sw_lang::MessagePath("tokuseiinfo.dat").c_str(), ability_descs_);
                sw_party_details::LoadText(host, sw_lang::MessagePath("seikaku.dat").c_str(), nature_names_);
                sw_party_details::LoadText(host, sw_lang::MessagePath("wazainfo.dat").c_str(), move_descs_);
            }
            pi("nature_ready", mon.nature < nature_names_.size() ? 1 : 0);
            pi("ability_ready", mon.ability > 0 && mon.ability < ability_names_.size() ? 1 : 0);
            if (mon.nature < nature_names_.size()) pt("nature", nature_names_[mon.nature]);
            if (mon.ability > 0 && mon.ability < ability_names_.size()) pt("ability", ability_names_[mon.ability]);
            if (mon.ability > 0 && mon.ability < ability_descs_.size())
                pt("ability_description", sw_party_details::OneLine(ability_descs_[mon.ability]));
            EnsureItemNames(host);
            const bool item_ok = mon.held_item > 0 && item_names_ready_ &&
                                 sw_item_names::ItemName(item_names_, mon.held_item, name);
            pt("item_name", mon.held_item == 0 ? sw_str::S(sw_str::Id::kCommonNone) : item_ok ? name : "—");
            char icon[160]{};
            const bool item_icon = item_ok && sw_item_icons::IconSource(mon.held_item, icon, sizeof icon);
            pi("item_icon_ready", item_icon ? 1 : 0);
            if (item_icon) pt("item_icon_src", icon);
            sw_party_details::Personal personal;
            const bool personal_ok = CachedDetailPersonal(host, mon, personal);
            std::array<std::uint16_t, 6> stats{};
            const bool stats_ok = personal_ok && sw_party_details::Stats(mon, personal, slot.level, stats) &&
                                  stats[0] == slot.max_hp;
            pi("stats_ready", stats_ok ? 1 : 0);
            if (stats_ok) {
                const char* names[]{"hp", "attack", "defense", "speed", "sp_attack", "sp_defense"};
                for (int j = 0; j < 6; ++j) pi(std::string("stat.") + names[j], stats[j]);
            }
            pi("types_ready", personal_ok ? 1 : 0);
            // Individual entries retain the personal-table type ID used by native type art.
            // Clear every row before replaying a changed form/detail, including failed loads.
            for (const char* group : {"weak", "resist", "immune"}) {
                pi(std::string(group) + "_count", 0);
                for (int j = 0; j < sw_species_types::kPersonalTypeCount; ++j)
                    pi(std::string(group) + std::to_string(j) + ".ready", 0);
            }
            if (personal_ok) {
                pi("type2_show", personal.type1 != personal.type2 ? 1 : 0);
                if (sw_species_types::TypeImageSource(personal.type1, true, icon, sizeof icon)) pt("type1_label_src", icon);
                if (personal.type1 != personal.type2 && sw_species_types::TypeImageSource(personal.type2, true, icon, sizeof icon))
                    pt("type2_label_src", icon);
                const auto matchups = sw_party_details::TypeMatchups(personal.type1, personal.type2);
                pt("weak_text", matchups.weak.empty() ? sw_str::S(sw_str::Id::kCommonNone) : matchups.weak);
                pt("resist_text", matchups.resist.empty() ? sw_str::S(sw_str::Id::kCommonNone) : matchups.resist);
                pt("immune_text", matchups.immune.empty() ? sw_str::S(sw_str::Id::kCommonNone) : matchups.immune);
                std::array<int, 3> counts{};
                const char* groups[]{"weak", "resist", "immune"};
                for (int type = 0; type < sw_species_types::kPersonalTypeCount; ++type) {
                    const int pct = sw_type_chart::EffectivenessPercentDual(
                        sw_species_types::ToChartType(type), sw_species_types::ToChartType(personal.type1),
                        sw_species_types::ToChartType(personal.type2));
                    if (pct == 100) continue;
                    const int group = pct == 0 ? 2 : pct > 100 ? 0 : 1;
                    const std::string key = std::string(groups[group]) + std::to_string(counts[group]++) + ".";
                    pi(key + "ready", 1);
                    pi(key + "type_id", type);
                    if (sw_species_types::TypeImageSource(type, false, icon, sizeof icon)) pt(key + "src", icon);
                    pt(key + "text", std::string(sw_str::TypeName(type)) +
                        (pct == 0 ? " ×0" : pct == 25 ? " ×¼" : pct == 50 ? " ×½" : pct == 200 ? " ×2" : " ×4"));
                }
                for (int j = 0; j < 3; ++j) pi(std::string(groups[j]) + "_count", counts[j]);
                pt("matchup_note", sw_str::S(sw_str::Id::kPartyMatchupNote));
            }
            for (int j = 0; j < 4; ++j) {
                const std::string key = "move" + std::to_string(j + 1) + ".";
                sw_move_data::MoveRecord record;
                const bool ready = mon.moves[j] > 0 && mon.pp_ups[j] <= 3 && CachedMove(host, mon.moves[j], record) &&
                                   record.pp > 0 && mon.pp[j] <= record.pp * (5 + mon.pp_ups[j]) / 5;
                pi(key + "ready", ready ? 1 : 0);
                if (!ready) continue;
                if (move_names_ready_ && sw_move_names::MoveName(move_names_, mon.moves[j], name)) pt(key + "name", name);
                pt(key + "pp_text", std::to_string(mon.pp[j]) + "/" + std::to_string(record.pp * (5 + mon.pp_ups[j]) / 5));
                pt(key + "power_text", record.power == 0 ? "—" : std::to_string(record.power));
                pt(key + "category_text", MoveCategoryText(record.category));
                if (sw_species_types::TypeImageSource(record.type, true, icon, sizeof icon)) pt(key + "type_label_src", icon);
                if (mon.moves[j] < move_descs_.size()) pt(key + "description", sw_party_details::OneLine(move_descs_[mon.moves[j]]));
            }
        });
    }


    // This tick's decoded zone name (populated by SampleLocation, consumed by SampleDens to
    // match the current zone against the den-zone table -- same "populated earlier in Sample(),
    // consumed later in the same tick" pattern as live_party_cache_ above).
    // `Sample()`'s call order (SampleLocation before SampleDens) guarantees this is fresh.
    std::string zone_cache_;
    bool zone_cache_ready_ = false;

    // Shared by SampleParty (per-slot) and SampleBattle (the live foe, which is never a party
    // member) -- species -> types -> weakness/immunity board, published under `key_prefix`
    // (e.g. "sw.party.0" or "sw.battle.foe"). Gated independently of whatever else `key_prefix`
    // already published this tick -- a romfs read failure here must not take that down.
    // sw_type_chart::Type and sw_species_types's personal-table type id use DIFFERENT id orders
    // (sw_species_types.h's own header comment warns about exactly this) -- PublishWeaknessBoard
    // computes its weak/immune list by walking sw_type_chart::Type (0..17, that enum's order), but
    // TypeImageSource() takes a personal-table id. The inverse of sw_species_types::ToChartType
    // (its own kMap, so no second hand-maintained index table can drift from it). -1 is
    // unreachable in practice (both cover the same 18 real types) but is handled by the caller
    // skipping that icon, never a guess.
    static int ChartTypeToPersonalId(int chart_type_index) {
        if (chart_type_index < 0 || chart_type_index >= sw_type_chart::kTypeCount) return -1;
        for (int pid = 0; pid < sw_species_types::kPersonalTypeCount; ++pid)
            if (static_cast<int>(sw_species_types::ToChartType(pid)) == chart_type_index) return pid;
        return -1;
    }

    void PublishWeaknessBoard(const EdenDsmodHostApi& host, const char* key_prefix, std::uint16_t species) {
        char key[72];
        std::uint8_t t1 = 0, t2 = 0;
        const bool types_ok = CachedTypes(host, species, t1, t2);
        std::snprintf(key, sizeof key, "%s.types_ready", key_prefix);
        if (host.publish_i64) PubI(host, key, types_ok ? 1 : 0);
        if (!types_ok) return;

        std::snprintf(key, sizeof key, "%s.type1", key_prefix);
        if (host.publish_i64) PubI(host, key, t1);
        std::snprintf(key, sizeof key, "%s.type2", key_prefix);
        if (host.publish_i64) PubI(host, key, t2);
        std::snprintf(key, sizeof key, "%s.mono_type", key_prefix);
        if (host.publish_i64) PubI(host, key, t1 == t2 ? 1 : 0);
        // D2 port: one gate for the second type disc/label (a widget has a single need_bind).
        std::snprintf(key, sizeof key, "%s.type2_show", key_prefix);
        if (host.publish_i64) PubI(host, key, t1 != t2 ? 1 : 0);

        char src1[160]{}, src2[160]{};
        if (host.publish_text && sw_species_types::TypeImageSource(t1, /*label=*/false, src1, sizeof src1)) {
            std::snprintf(key, sizeof key, "%s.type1_src", key_prefix);
            PubT(host, key, src1);
        }
        if (host.publish_text && t1 != t2 &&
            sw_species_types::TypeImageSource(t2, /*label=*/false, src2, sizeof src2)) {
            std::snprintf(key, sizeof key, "%s.type2_src", key_prefix);
            PubT(host, key, src2);
        }
        // D2 port: the native type LABEL art (typeIcon_text_NN, "ICE") for the battle matchup card.
        if (host.publish_text && sw_species_types::TypeImageSource(t1, /*label=*/true, src1, sizeof src1)) {
            std::snprintf(key, sizeof key, "%s.type1_label_src", key_prefix);
            PubT(host, key, src1);
        }
        if (host.publish_text && t1 != t2 &&
            sw_species_types::TypeImageSource(t2, /*label=*/true, src2, sizeof src2)) {
            std::snprintf(key, sizeof key, "%s.type2_label_src", key_prefix);
            PubT(host, key, src2);
        }

        // Weak/immune as both a text list (accessibility/compact layouts) and indexed per-type
        // discs (the approved D2 direction's weakness board: "type-icon discs", round-7 rule 7 --
        // icon_type/bin/type_dummy_eng.arc, the same TypeImageSource() this function already
        // calls for the mon's own type icons above). Capped at kMaxBoardIcons each -- generous
        // for the real chart (the worst real dual-type case has at most a handful of
        // super-effective attacking types; the cap exists only so a page's fixed row of disc
        // widgets has a known maximum to bind, never to drop a real result).
        constexpr int kMaxBoardIcons = 8;
        int weak_n = 0, immune_n = 0, resist_n = 0;
        std::string weak, immune;
        if (host.publish_text || host.publish_i64) {
            using sw_type_chart::EffectivenessPercentDual;
            using sw_type_chart::Type;
            const Type d1 = sw_species_types::ToChartType(t1);
            const Type d2 = sw_species_types::ToChartType(t2);
            // Walk attackers in the game's own personal-table type order (Normal, Fighting, Flying,
            // ...) -- the order the native UI and the D2 mockup list type discs in.
            for (int k = 0; k < sw_species_types::kPersonalTypeCount; ++k) {
                const auto attacker = sw_species_types::ToChartType(k);
                const int ti = static_cast<int>(attacker);
                const auto pct = EffectivenessPercentDual(attacker, d1, d2);
                const int name_pid = ChartTypeToPersonalId(ti);
                const char* name = name_pid >= 0 ? sw_str::TypeName(name_pid) : "";
                if (pct > 100) {
                    if (!weak.empty()) weak += ", ";
                    weak += name;
                    if (pct >= 400) weak += " x4";
                    if (weak_n < kMaxBoardIcons) {
                        char src[160]{};
                        const int pid = ChartTypeToPersonalId(ti);
                        if (pid >= 0 && sw_species_types::TypeImageSource(pid, /*label=*/false, src, sizeof src)) {
                            std::snprintf(key, sizeof key, "%s.weak%d_src", key_prefix, weak_n);
                            if (host.publish_text) PubT(host, key, src);
                            // the native type plate (icon + the game's own type name) for the matchup rows
                            char label[160]{};
                            if (sw_species_types::TypeImageSource(pid, /*label=*/true, label, sizeof label)) {
                                std::snprintf(key, sizeof key, "%s.weak%d_label_src", key_prefix, weak_n);
                                if (host.publish_text) PubT(host, key, label);
                            }
                            std::snprintf(key, sizeof key, "%s.weak%d_quad", key_prefix, weak_n);
                            if (host.publish_i64) PubI(host, key, pct >= 400 ? 1 : 0);
                            // Personal-table type id (0..17): the manifest tints the mask-like
                            // disc texture (BC5, no real colour of its own) per type via
                            // tint_bind/tint_colors, since the actual species -> type is only
                            // known live, never at manifest-generation time.
                            std::snprintf(key, sizeof key, "%s.weak%d_type", key_prefix, weak_n);
                            if (host.publish_i64) PubI(host, key, pid);
                            ++weak_n;
                        }
                    }
                } else if (pct > 0 && pct < 100) {
                    // D2 port: "Not very effective" row of the battle matchup card.
                    if (resist_n < kMaxBoardIcons) {
                        char src[160]{};
                        const int pid = ChartTypeToPersonalId(ti);
                        if (pid >= 0 && sw_species_types::TypeImageSource(pid, /*label=*/false, src, sizeof src)) {
                            std::snprintf(key, sizeof key, "%s.resist%d_src", key_prefix, resist_n);
                            if (host.publish_text) PubT(host, key, src);
                            // the native type plate (icon + the game's own type name) for the matchup rows
                            char label[160]{};
                            if (sw_species_types::TypeImageSource(pid, /*label=*/true, label, sizeof label)) {
                                std::snprintf(key, sizeof key, "%s.resist%d_label_src", key_prefix, resist_n);
                                if (host.publish_text) PubT(host, key, label);
                            }
                            std::snprintf(key, sizeof key, "%s.resist%d_type", key_prefix, resist_n);
                            if (host.publish_i64) PubI(host, key, pid);
                            ++resist_n;
                        }
                    }
                } else if (pct == 0) {
                    if (!immune.empty()) immune += ", ";
                    immune += name;
                    if (immune_n < kMaxBoardIcons) {
                        char src[160]{};
                        const int pid = ChartTypeToPersonalId(ti);
                        if (pid >= 0 && sw_species_types::TypeImageSource(pid, /*label=*/false, src, sizeof src)) {
                            std::snprintf(key, sizeof key, "%s.immune%d_src", key_prefix, immune_n);
                            if (host.publish_text) PubT(host, key, src);
                            // the native type plate (icon + the game's own type name) for the matchup rows
                            char label[160]{};
                            if (sw_species_types::TypeImageSource(pid, /*label=*/true, label, sizeof label)) {
                                std::snprintf(key, sizeof key, "%s.immune%d_label_src", key_prefix, immune_n);
                                if (host.publish_text) PubT(host, key, label);
                            }
                            std::snprintf(key, sizeof key, "%s.immune%d_type", key_prefix, immune_n);
                            if (host.publish_i64) PubI(host, key, pid);
                            ++immune_n;
                        }
                    }
                }
            }
        }
        if (host.publish_i64) {
            std::snprintf(key, sizeof key, "%s.weak_count", key_prefix);
            PubI(host, key, weak_n);
            std::snprintf(key, sizeof key, "%s.immune_count", key_prefix);
            PubI(host, key, immune_n);
            std::snprintf(key, sizeof key, "%s.resist_count", key_prefix);
            PubI(host, key, resist_n);
            // Explicit per-index ready flags (0 or 1), published for the FULL fixed range every
            // tick -- not just up to this tick's count -- so a page's fixed row of disc widgets
            // can hide_bind on them directly, and a disc that was shown last tick but isn't this
            // tick (a different species/types resolved) is actively hidden, never left stale-true.
            for (int j = 0; j < kMaxBoardIcons; ++j) {
                std::snprintf(key, sizeof key, "%s.weak%d_ready", key_prefix, j);
                PubI(host, key, j < weak_n ? 1 : 0);
                if (j >= weak_n) { // no stale 4x frame on an emptied slot
                    std::snprintf(key, sizeof key, "%s.weak%d_quad", key_prefix, j);
                    PubI(host, key, 0);
                }
                std::snprintf(key, sizeof key, "%s.immune%d_ready", key_prefix, j);
                PubI(host, key, j < immune_n ? 1 : 0);
                std::snprintf(key, sizeof key, "%s.resist%d_ready", key_prefix, j);
                PubI(host, key, j < resist_n ? 1 : 0);
            }
        }
        if (host.publish_text) {
            std::snprintf(key, sizeof key, "%s.weak_text", key_prefix);
            PubT(host, key, weak.empty() ? sw_str::S(sw_str::Id::kCommonNone) : weak.c_str());
            std::snprintf(key, sizeof key, "%s.immune_text", key_prefix);
            PubT(host, key, immune.empty() ? "—" : immune.c_str());
        }
    }

    // D2 port: a species' box icon under `key_prefix` (<prefix>.icon_ready + .icon_src), the same
    // fail-closed sw_species_icons::IconSource the party strip uses.
    void PublishIcon(const EdenDsmodHostApi& host, const char* key_prefix, std::uint16_t species) {
        char key[72];
        char src[160]{};
        const bool ok = species != 0 && sw_species_icons::IconSource(species, src, sizeof src);
        std::snprintf(key, sizeof key, "%s.icon_ready", key_prefix);
        if (host.publish_i64) PubI(host, key, ok ? 1 : 0);
        if (ok && host.publish_text) {
            std::snprintf(key, sizeof key, "%s.icon_src", key_prefix);
            PubT(host, key, src);
        }
        if (ok) PublishIconOffset(host, key_prefix, species);
    }

    // <prefix>.icon_dx/.icon_dy (icon pixels; the manifest scales them by draw size / 136x112):
    // moves the art's visible feet onto one baseline and centres it by its art, from the
    // offline-baked alpha boxes (sw_species_icon_metrics.h). No box -> 0/0 (texture placement).
    void PublishIconOffset(const EdenDsmodHostApi& host, const char* key_prefix, std::uint16_t species) {
        if (!host.publish_i64) return;
        int dx = 0, dy = 0;
        if (!sw_species_icon_metrics::OffsetFor(species, dx, dy)) dx = dy = 0;
        char key[80];
        std::snprintf(key, sizeof key, "%s.icon_dx", key_prefix);
        PubI(host, key, dx);
        std::snprintf(key, sizeof key, "%s.icon_dy", key_prefix);
        PubI(host, key, dy);
    }

    // D2 port: a species' status-ailment badge under `key_prefix` (<prefix>.status_ready +
    // .status_src), the labelled icon_sick texture sw_status_icons.h resolves. `max_hp==0` means
    // "no live HP for this slot" -- passed through so FromStatus never claims "fainted" from an
    // absent HP reading alone.
    void PublishStatusIcon(const EdenDsmodHostApi& host, const char* key_prefix,
                                   std::uint32_t status, std::uint16_t current_hp,
                                   std::uint16_t max_hp) {
        char key[80];
        char src[160]{};
        const auto code = sw_status_icons::FromStatus(status, current_hp, max_hp);
        const bool ok = sw_status_icons::IconSource(code, src, sizeof src);
        std::snprintf(key, sizeof key, "%s.status_ready", key_prefix);
        if (host.publish_i64) PubI(host, key, ok ? 1 : 0);
        if (ok && host.publish_text) {
            std::snprintf(key, sizeof key, "%s.status_src", key_prefix);
            PubT(host, key, src);
        }
    }

    // Every Sample* below follows the same shape: look the anchor up, publish its status, and
    // -- only once an anchor's `verified` bit is set -- resolve it with
    // sw_addresses::Resolve(heap_begin, *anchor), read it with ReadStable, and decode with the
    // matching sw_pk8/sw_text helper. `heap_begin` is threaded through now so that step is a
    // pure addition once a lead lands, not a reader-shape change.

    // MyStatus8: OT name, TID/SID, Watt, game byte. All four anchors are verified (2 boots,
    // 2026-10-24) and read as one logical record -- if any one of the four reads/validates fail
    // this tick, nothing from this tick is published (no half-updated trainer card).
    void SampleTrainer(const EdenDsmodHostApi& host, u64 heap_begin) {
        const auto* a_watt = sw_addresses::Find("mystatus.watt");
        const auto* a_ids = sw_addresses::Find("mystatus.tid_sid");
        const auto* a_ot = sw_addresses::Find("mystatus.ot_name");
        const auto* a_game = sw_addresses::Find("mystatus.game");
        PublishDomainStatus(host, "trainer", a_watt);
        if (!a_watt || !a_watt->verified || !a_ids || !a_ids->verified || !a_ot ||
            !a_ot->verified || !a_game || !a_game->verified)
            return;

        // One span over the four fields of the one MyStatus8 record (review L7: a piecewise read
        // could tear across a save load); each field is sliced out at its own anchor offset.
        const u64 span_lo = std::min({a_watt->sysbot_offset, a_ids->sysbot_offset, a_ot->sysbot_offset,
                                      a_game->sysbot_offset});
        const u64 span_hi = std::max({a_watt->sysbot_offset + a_watt->size, a_ids->sysbot_offset + a_ids->size,
                                      a_ot->sysbot_offset + a_ot->size, a_game->sysbot_offset + a_game->size});
        std::array<std::uint8_t, 0x40> rec{};
        if (span_hi - span_lo > rec.size() ||
            !ReadStable(host, heap_begin + span_lo, rec.data(), static_cast<std::size_t>(span_hi - span_lo)))
            return;
        const auto field = [&](const sw_addresses::Anchor* an) { return rec.data() + (an->sysbot_offset - span_lo); };
        std::uint32_t watt = 0;
        std::uint8_t ids_raw[4]{};
        std::uint8_t ot_raw[0x1A]{};
        std::memcpy(&watt, field(a_watt), sizeof watt);
        std::memcpy(ids_raw, field(a_ids), sizeof ids_raw);
        std::memcpy(ot_raw, field(a_ot), sizeof ot_raw);
        // a_game/field(a_game) itself is unread beyond this point: its anchor still belongs to
        // the shared span_lo/span_hi atomicity gate above (the four MyStatus8 fields are read as
        // one record), but PKHeX's GameVersion.SW byte (sw.trainer.game) is not bound by any page
        // (0 gen_manifest.py hits) and is no longer published.

        // UTF-16LE, NUL-terminated, up to 13 code units (0x1A bytes). Only the printable-ASCII
        // range is decoded this slice (the one live fixture's OT name, "Player", is ASCII) --
        // a non-ASCII OT name is a known gap (sw_text.h's decoder is not wired here), and we
        // fail closed rather than show a mangled partial name for it.
        std::string ot_name;
        bool ot_ok = true;
        for (std::size_t i = 0; i + 1 < sizeof ot_raw; i += 2) {
            const unsigned cu = static_cast<unsigned>(ot_raw[i]) |
                                 (static_cast<unsigned>(ot_raw[i + 1]) << 8);
            if (cu == 0) break;
            if (cu < 0x20 || cu > 0x7E) {
                ot_ok = false;
                break;
            }
            ot_name.push_back(static_cast<char>(cu));
        }
        if (!ot_ok || ot_name.empty()) return;

        const unsigned tid16 = static_cast<unsigned>(ids_raw[0]) | (static_cast<unsigned>(ids_raw[1]) << 8);
        const unsigned sid16 = static_cast<unsigned>(ids_raw[2]) | (static_cast<unsigned>(ids_raw[3]) << 8);

        if (host.publish_i64) {
            PubI(host, "sw.trainer.tid", tid16);
            PubI(host, "sw.trainer.sid", sid16);
            PubI(host, "sw.trainer.watt", watt);
        }
        if (host.publish_text) {
            PubT(host, "sw.trainer.ot_name", ot_name.c_str());
            char id_text[40];
            std::snprintf(id_text, sizeof id_text, sw_str::S(sw_str::Id::kCardTrainerIds), tid16, sid16);
            PubT(host, "sw.trainer.id_text", id_text);
            char watt_text[32];
            std::snprintf(watt_text, sizeof watt_text, "%u Watts", watt);
            PubT(host, "sw.trainer.watt_text", watt_text);
        }
    }

    // Misc8 (KMisc): Badges/Money/BP, one logical record (torn-read double-read on all three,
    // reject the whole tick if any one field disagrees or fails a sanity bound -- the same
    // "no half-updated card" rule SampleTrainer uses). VERIFIED 2026-10-25, 2 boots, see
    // sw_addresses.h's misc8.money_badges note and LIVE-READERS-132.md.
    void SampleMisc8(const EdenDsmodHostApi& host, u64 heap_begin) {
        const auto* a = sw_addresses::Find("misc8.money_badges");
        PublishDomainStatus(host, "misc8", a);
        if (!a || !a->verified) return;

        // One span (review L7): badges +0x00, money +0x04, BP +0x11C of the same record.
        std::array<std::uint8_t, 0x11E> rec{};
        if (!ReadStable(host, sw_addresses::Resolve(heap_begin, *a), rec.data(), rec.size())) return;
        const std::uint8_t badges = rec[0x00];
        std::uint32_t money = 0;
        std::uint16_t bp = 0;
        std::memcpy(&money, rec.data() + 0x04, sizeof money);
        std::memcpy(&bp, rec.data() + 0x11C, sizeof bp);

        // Sanity bounds (PORTING_A_GAME.md "No heuristics" still allows rejecting an
        // impossible value): the game caps Money at 9,999,999 and BP at 9999, so a decode
        // outside either bound is a torn/garbage read, not a real save state -- fail closed
        // rather than show it.
        if (money > 9999999u || bp > 9999u) return;

        int badge_count = 0;
        for (int b = 0; b < 8; ++b)
            if (badges & (1u << b)) ++badge_count;

        if (host.publish_i64) {
            PubI(host, "sw.misc8.badges", badges);
            PubI(host, "sw.misc8.badge_count", badge_count);
            PubI(host, "sw.misc8.ready", 1);
            PubI(host, "sw.misc8.money", money);
            PubI(host, "sw.misc8.bp", bp);
        }
        if (host.publish_text) {
            // Group with commas (the built-in label font has no "group" styling helper here).
            // The leading mark is a plain ASCII "P", not the Unicode Peso sign the pre-font-fix
            // code used: the mockup team's own font research (tools/mockup/kit.py's money())
            // checked every decoded game font (fs bitmap, Rodin, UD Kakugo, Switch shared) and
            // found the real Pokédollar glyph in none of them, constructing it instead from the
            // fs font's "P" plus a drawn stem bar. This module's plain label/value widgets have
            // no glyph-composition facility (same gap that comment already named), so with a
            // real game font now set (see the package's "font" key) a raw "₱" would draw as a
            // missing-glyph box -- worse than the plain "P" this uses instead.
            std::string grouped;
            {
                char digits[16];
                std::snprintf(digits, sizeof digits, "%u", money);
                const std::size_t len = std::strlen(digits);
                for (std::size_t i = 0; i < len; ++i) {
                    if (i > 0 && (len - i) % 3 == 0) grouped.push_back(',');
                    grouped.push_back(digits[i]);
                }
            }
            char money_text[40];
            std::snprintf(money_text, sizeof money_text, "P%s", grouped.c_str());
            PubT(host, "sw.misc8.money_text", money_text);
            char bp_text[24];
            std::snprintf(bp_text, sizeof bp_text, "%u BP", bp);
            PubT(host, "sw.misc8.bp_text", bp_text);
            char badge_text[16];
            std::snprintf(badge_text, sizeof badge_text, "%d/8", badge_count);
            PubT(host, "sw.misc8.badge_text", badge_text);
        }
    }

    // Per-slot keys a party tile binds (Pokemon page, Town Map strip, Bag "use on" grid), from one
    // decoded member. Shared by the live array (the normal source) and the save mirror (fallback).
    // The box icon the native party list shows for an Egg (form 00, not Manaphy's 01).
    static constexpr const char* kEggIconSrc =
        "romfs:/bin/appli/icon_pokemon/poke_icon_0000_00s_n.bntx#poke_icon_0000_00s_n";

    // An Egg slot (owner 2026-10-31, "Lumi had this bug before"): name/icon/egg flag only. Every
    // key that could reveal what hatches (species, level, HP, types, weakness, status, moves,
    // item) is published not-ready / cleared, so no board, strip or matchup shows it.
    void PublishEggSlot(const EdenDsmodHostApi& host, int i) {
        const std::string k = "sw.party." + std::to_string(i) + ".";
        PubI(host, (k + "ready").c_str(), 1);
        PubI(host, (k + "egg").c_str(), 1);
        PubI(host, (k + "species").c_str(), 0);
        PubI(host, (k + "level").c_str(), 0);
        PubI(host, (k + "max_hp").c_str(), 0);
        PubT(host, (k + "level_text").c_str(), "");
        PubT(host, (k + "name").c_str(), sw_str::S(sw_str::Id::kEgg));
        PubI(host, (k + "hp_ready").c_str(), 0);
        // An Egg never shows what hatches -- nor whether it is shiny (item E).
        for (const char* z : {"current_hp", "hp_pct", "hpg", "hpy", "hpr", "status", "types_ready",
                              "status_ready", "item_ready", "item_icon_ready", "icon_dx", "icon_dy", "shiny"})
            PubI(host, (k + z).c_str(), 0);
        for (const char* z : {"hp_pct_text", "hp_text", "item_name"}) PubT(host, (k + z).c_str(), "");
        for (int m = 1; m <= 4; ++m) PubI(host, (k + "move" + std::to_string(m) + ".ready").c_str(), 0);
        PubI(host, (k + "icon_ready").c_str(), 1);
        PubT(host, (k + "icon_src").c_str(), kEggIconSrc);
    }

    void PublishPartySlot(const EdenDsmodHostApi& host, int i, std::uint16_t species, int level,
                          std::uint16_t max_hp, int current_hp /* -1 = not known */, std::uint32_t status,
                          const std::string& nickname = {}, bool shiny = false) {
        char key[40];
        std::snprintf(key, sizeof key, "sw.party.%d.ready", i);
        PubI(host, key, 1);
        std::snprintf(key, sizeof key, "sw.party.%d.shiny", i);
        PubI(host, key, shiny ? 1 : 0);
        std::snprintf(key, sizeof key, "sw.party.%d.egg", i);
        PubI(host, key, 0);
        std::snprintf(key, sizeof key, "sw.party.%d.species", i);
        PubI(host, key, species);
        std::snprintf(key, sizeof key, "sw.party.%d.level", i);
        PubI(host, key, level);
        std::snprintf(key, sizeof key, "sw.party.%d.max_hp", i);
        PubI(host, key, max_hp);
        std::snprintf(key, sizeof key, "sw.party.%d.level_text", i);
        PubT(host, key, sw_str::Level(level).c_str());
        // A nickname the package font cannot draw (another script) falls back to the species name.
        std::string name = sw_font::CoversText(nickname) ? nickname : std::string{};
        if (!name.empty() || (species_names_ready_ && sw_species_names::SpeciesName(species_names_, species, name))) {
            std::snprintf(key, sizeof key, "sw.party.%d.name", i);
            PubT(host, key, name.c_str());
        }
        if (current_hp >= 0 && max_hp > 0) {
            const int pct = std::clamp(current_hp * 100 / max_hp, 0, 100);
            std::snprintf(key, sizeof key, "sw.party.%d.current_hp", i);
            PubI(host, key, current_hp);
            std::snprintf(key, sizeof key, "sw.party.%d.hp_ready", i);
            PubI(host, key, 1);
            std::snprintf(key, sizeof key, "sw.party.%d.hp_pct", i);
            PubI(host, key, pct);
            // Three mutually-exclusive zone gates (D2's hp_bar() thresholds: green >50%, yellow
            // >20%, else red) -- the manifest stacks 3 coloured bars, each need_bind-gated.
            std::snprintf(key, sizeof key, "sw.party.%d.hpg", i);
            PubI(host, key, pct > 50 ? 1 : 0);
            std::snprintf(key, sizeof key, "sw.party.%d.hpy", i);
            PubI(host, key, pct > 20 && pct <= 50 ? 1 : 0);
            std::snprintf(key, sizeof key, "sw.party.%d.hpr", i);
            PubI(host, key, pct <= 20 ? 1 : 0);
            std::snprintf(key, sizeof key, "sw.party.%d.status", i);
            PubI(host, key, static_cast<std::int64_t>(status));
            char pct_text[8];
            std::snprintf(pct_text, sizeof pct_text, "%d%%", pct);
            std::snprintf(key, sizeof key, "sw.party.%d.hp_pct_text", i);
            PubT(host, key, pct_text);
        }
        std::snprintf(key, sizeof key, "sw.party.%d", i);
        PublishIcon(host, key, species);
        PublishStatusIcon(host, key, status, current_hp >= 0 ? static_cast<std::uint16_t>(current_hp) : 0,
                          current_hp >= 0 ? max_hp : 0);
        PublishWeaknessBoard(host, key, species);
    }

    // Pokemon page row (owner Thor 2026-10-31): held item name/icon and "cur/max" HP per member.
    // Live array only (the save mirror has no current HP); unset slots keep item_ready = 0.
    void PublishPartySlotExtras(const EdenDsmodHostApi& host, int i, const sw_pk8::LivePartyMon& m,
                                std::uint16_t max_hp) {
        const std::string k = "sw.party." + std::to_string(i) + ".";
        PubT(host, (k + "hp_text").c_str(), (std::to_string(m.current_hp) + "/" + std::to_string(max_hp)).c_str());
        EnsureItemNames(host);
        std::string name;
        const bool item_ok = m.held_item > 0 && item_names_ready_ && sw_item_names::ItemName(item_names_, m.held_item, name);
        PubI(host, (k + "item_ready").c_str(), 1);
        PubT(host, (k + "item_name").c_str(), m.held_item == 0 ? sw_str::S(sw_str::Id::kCommonNone) : item_ok ? name.c_str() : "—");
        char icon[160]{};
        const bool icon_ok = item_ok && sw_item_icons::IconSource(m.held_item, icon, sizeof icon);
        PubI(host, (k + "item_icon_ready").c_str(), icon_ok ? 1 : 0);
        if (icon_ok) PubT(host, (k + "item_icon_src").c_str(), icon);
    }

    // Save-snapshot party mirror (sw_addresses.h "party.save_mirror"): 6 encrypted PK8 (0x158
    // each) plus a count byte. Max HP/level only -- it holds no current HP. FALLBACK ONLY: Sample()
    // calls it only on a tick the live array (SampleLiveParty) could not be read at all, and then
    // it supplies every slot, never single slots next to live ones (review M1: the mirror's order
    // can differ from the live array's, the one the Bag's "use on" target index refers to).
    void SampleParty(const EdenDsmodHostApi& host, u64 heap_begin) {
        const auto* a_party = sw_addresses::Find("party.save_mirror");
        PublishDomainStatus(host, "party", a_party);
        if (!a_party || !a_party->verified) return;

        constexpr int kMaxSlots = 6;
        std::array<std::uint8_t, sw_pk8::kSizeParty * kMaxSlots + 1> raw{}; // + the count byte
        if (!ReadStable(host, sw_addresses::Resolve(heap_begin, *a_party), raw.data(), raw.size())) {
            party_tape_.Clear();
            return;
        }
        Taped(party_tape_, HashBytes(raw.data(), raw.size()), host, [&] {
            EnsureSpeciesNames(host);
            const int party_count = raw[sw_pk8::kSizeParty * kMaxSlots] <= kMaxSlots
                                        ? raw[sw_pk8::kSizeParty * kMaxSlots] : kMaxSlots;
            PubI(host, "sw.party.count", party_count);
            PubI(host, "sw.party.six", party_count >= 6 ? 1 : 0); // the Pokemon board's 6-row layout
            int level_sum = 0, level_n = 0;
            for (int i = 0; i < kMaxSlots; ++i) {
                const sw_pk8::PartyMon m = sw_pk8::DecodeParty(raw.data() + i * sw_pk8::kSizeParty);
                // Fail-closed per slot: empty slot, out-of-range species or a failed PK8 checksum
                // (torn read, or stale memory past the real party) publishes "not ready".
                const bool valid = i < party_count && m.species != 0 && m.species_in_range && m.checksum_valid;
                if (!valid) {
                    char key[40];
                    std::snprintf(key, sizeof key, "sw.party.%d.ready", i);
                    PubI(host, key, 0);
                    continue;
                }
                if (m.egg) { PublishEggSlot(host, i); continue; }
                level_sum += m.level;
                ++level_n;
                PublishPartySlot(host, i, m.species, m.level, m.max_hp, -1, 0, m.nickname,
                                 sw_pk8::IsShiny(m.pid, m.tid, m.sid));
            }
            // The save mirror has no current HP: average level only, "able" unknown.
            PublishPartySummary(host, level_sum, level_n, -1);
        });
    }

    // Party summary (Town Map strip): rounded mean level of non-Egg members, how many non-Egg
    // members can still battle (current HP > 0; able < 0 = not known, e.g. the save mirror).
    void PublishPartySummary(const EdenDsmodHostApi& host, int level_sum, int level_n, int able) {
        PubI(host, "sw.party.avg_ready", level_n > 0 ? 1 : 0);
        PubI(host, "sw.party.avg_level", level_n > 0 ? (level_sum + level_n / 2) / level_n : 0);
        PubI(host, "sw.party.able_ready", able >= 0 ? 1 : 0);
        if (able >= 0) PubI(host, "sw.party.able", able);
    }

    void PublishLivePartyMoves(const EdenDsmodHostApi& host, int slot, const sw_pk8::LivePartyMon& mon) {
        for (int i = 0; i < 4; ++i) {
            const std::string key = "sw.party." + std::to_string(slot) + ".move" + std::to_string(i + 1);
            sw_move_data::MoveRecord record;
            const bool data_ok = mon.moves[i] > 0 && mon.pp_ups[i] <= 3 && CachedMove(host, mon.moves[i], record) &&
                                 record.pp > 0;
            const int max_pp = data_ok ? record.pp * (5 + mon.pp_ups[i]) / 5 : 0;
            const bool ready = data_ok && max_pp < 256 && mon.pp[i] <= max_pp;
            PubI(host, (key + ".ready").c_str(), ready ? 1 : 0);
            if (!ready) continue;
            PubI(host, (key + ".id").c_str(), mon.moves[i]);
            PubI(host, (key + ".pp").c_str(), mon.pp[i]);
            PubI(host, (key + ".pp_max").c_str(), max_pp);
            PubI(host, (key + ".needs_pp").c_str(), mon.pp[i] < max_pp ? 1 : 0);
            const std::string pp_text = std::to_string(mon.pp[i]) + "/" + std::to_string(max_pp);
            PubT(host, (key + ".pp_text").c_str(), pp_text.c_str());
            std::string name;
            const bool name_ok = move_names_ready_ && sw_move_names::MoveName(move_names_, mon.moves[i], name);
            PubI(host, (key + ".name_ready").c_str(), name_ok ? 1 : 0);
            if (name_ok) PubT(host, (key + ".name").c_str(), name.c_str());
        }
    }

    // Live party array (sw_addresses.h "party.array_root"): the game's own party object (the field
    // manager's +0x58, the object the native Bag's "use on" step passes to CanUse/Apply). +0x58 +
    // 8*i: the slot objects, +0x88: the member count (1..6). Each slot object's encrypted 0x148
    // "stored" PK8 at +0xD0 holds the LIVE current HP/status. Level/max HP are computed from the
    // blob's own EXP/IV/EV (sw_level.h). Reads up to `count` slots (review M1: the old cap of 5
    // was derived from a 5-member fixture; +0x80, the 6th pointer, is live when count is 6).
    // Change-driven: the decode and the ~70 publishes per slot only run when a raw byte changed.
    void SampleLiveParty(const EdenDsmodHostApi& host, u64 heap_begin) {
        live_party_ok_ = false;
        live_party_empty_ = false;
        const auto* a_root = sw_addresses::Find("party.array_root");
        PublishDomainStatus(host, "party", a_root);
        live_party_array_ = {};
        const auto fail = [&] {
            live_party_cache_.fill(LiveSlot{});
            party_detail_mons_.fill(sw_pk8::LivePartyMon{});
            live_tape_.Clear();
        };
        if (!a_root || !a_root->verified) return fail();

        constexpr u64 kSlotPtrBase = 0x58;
        constexpr int kMaxLiveSlots = 6;
        constexpr u64 kBlobOffset = 0xD0;
        const u64 root_addr = sw_addresses::Resolve(heap_begin, *a_root);
        // One span: slot pointers +0x58..+0x87 and the count at +0x88 (review L7: no piecewise
        // reads of one record).
        std::array<std::uint8_t, 0x38> head{};
        if (!ReadStable(host, root_addr + kSlotPtrBase, head.data(), head.size())) return fail();
        // The count is the BYTE at +0x88 (the party's own accessors use ldrb [+0x88]); +0x89 is
        // the party's own member index the native exchange (main+0x7844F0) moves with its member.
        // Live-caught 2026-10-31 (dev.17): after a reorder +0x89 = 2, a u64 read gave 0x206 and the
        // live array was rejected (stale order, phase stuck in loading after a reboot).
        const std::uint64_t count64 = head[0x30];
        // A new game has no Pokemon until the starter: a clean count of 0 is a real state (the
        // phase machine treats it as a valid, empty party), not a failed read.
        if (count64 == 0) live_party_empty_ = true;
        if (count64 < 1 || count64 > kMaxLiveSlots) return fail();
        live_party_ok_ = true;
        const int count = static_cast<int>(count64);
        live_party_array_ = {true, root_addr, count, {}};
        for (int i = 0; i < count; ++i)
            std::memcpy(&live_party_array_.ptr[static_cast<std::size_t>(i)], head.data() + i * 8, 8);

        std::array<std::array<std::uint8_t, sw_pk8::kSizeStored>, kMaxLiveSlots> blobs{};
        std::array<bool, kMaxLiveSlots> read_ok{};
        std::uint64_t sig = HashBytes(head.data(), head.size());
        for (int i = 0; i < count; ++i) {
            std::uint64_t slot_ptr = 0;
            std::memcpy(&slot_ptr, head.data() + i * 8, sizeof slot_ptr);
            read_ok[static_cast<std::size_t>(i)] =
                slot_ptr != 0 && ReadStable(host, slot_ptr + kBlobOffset, blobs[static_cast<std::size_t>(i)].data(),
                                            sw_pk8::kSizeStored);
            sig = HashValue(read_ok[static_cast<std::size_t>(i)] ? 1 : 0, sig);
            if (read_ok[static_cast<std::size_t>(i)])
                sig = HashBytes(blobs[static_cast<std::size_t>(i)].data(), sw_pk8::kSizeStored, sig);
        }

        party_detail_live_sig_ = sig;
        Taped(live_tape_, sig, host, [&] {
            EnsureSpeciesNames(host);
            EnsureMoveNames(host);
            live_party_cache_.fill(LiveSlot{});
            party_detail_mons_.fill(sw_pk8::LivePartyMon{});
            PubI(host, "sw.party.count", count);
            PubI(host, "sw.party.six", count >= 6 ? 1 : 0);
            int level_sum = 0, level_n = 0, able = 0;
            for (int i = 0; i < kMaxLiveSlots; ++i) {
                char key[40];
                std::snprintf(key, sizeof key, "sw.party.%d.item_ready", i);
                PubI(host, key, 0); // PublishPartySlotExtras re-arms it for a decoded member
                std::snprintf(key, sizeof key, "sw.party.%d.ready", i);
                const auto& blob = blobs[static_cast<std::size_t>(i)];
                if (i >= count || !read_ok[static_cast<std::size_t>(i)]) {
                    PubI(host, key, 0); // beyond the party, or this slot failed: its tile locks
                    continue;
                }
                const sw_pk8::LivePartyMon m = sw_pk8::DecodeLiveStored(blob.data());
                std::uint8_t base_hp = 0, growth_byte = 0;
                if (m.species == 0 || !m.species_in_range || !m.checksum_valid ||
                    !CachedBaseHpAndGrowth(host, m.species, base_hp, growth_byte)) {
                    PubI(host, key, 0);
                    continue;
                }
                // Positive forms may use a different HP/growth row; hyper-trained HP uses31
                // in native summary stats even when its original stored IV remains lower.
                if (m.form != 0) {
                    sw_party_details::Personal personal;
                    if (!CachedDetailPersonal(host, m, personal)) { PubI(host, key, 0); continue; }
                    base_hp = personal.base[0];
                    growth_byte = personal.growth;
                }
                const auto growth = sw_level::GrowthRateFromByte(growth_byte);
                const int level = sw_level::LevelFromExp(growth, m.experience);
                const auto hp_iv = static_cast<std::uint8_t>((m.hyper_train & 1u) != 0 ? 31 : m.iv_hp);
                const std::uint16_t max_hp = sw_level::ComputeMaxHp(base_hp, hp_iv, m.ev_hp, level);
                // A current HP above the computed max means the computation disagrees with the
                // game's or a torn read slipped past the checksum: never a self-contradicting bar.
                if (max_hp == 0 || m.current_hp > max_hp) {
                    PubI(host, key, 0);
                    continue;
                }
                std::uint32_t ec = 0;
                std::memcpy(&ec, blob.data(), sizeof ec);
                live_party_cache_[static_cast<std::size_t>(i)] = {m.species, static_cast<std::uint8_t>(level),
                                                                  m.current_hp, max_hp, m.status, ec, true, m.egg,
                                                                  !m.egg && sw_pk8::IsShiny(m.pid, m.tid, m.sid)};
                party_detail_mons_[static_cast<std::size_t>(i)] = m;
                if (m.egg) {
                    PublishEggSlot(host, i);
                    continue;
                }
                level_sum += level;
                ++level_n;
                if (m.current_hp > 0) ++able;
                PublishPartySlot(host, i, m.species, level, max_hp, m.current_hp, m.status, m.nickname,
                                 sw_pk8::IsShiny(m.pid, m.tid, m.sid));
                PublishLivePartyMoves(host, i, m);
                PublishPartySlotExtras(host, i, m, max_hp);
            }
            PublishPartySummary(host, level_sum, level_n, able);
        });
    }

    // Repel record (field.repel): u16 item that started it, u16 steps left -- written by the
    // game's own UseRepel (main+0x1439120: 100/200/250 for Repel/Super/Max) and counted down by
    // the game per step. Published for the Bag page (Repel rows lock while one is in effect).
    void SampleRepel(const EdenDsmodHostApi& host, u64 heap_begin) {
        repel_steps_ = -1;
        const auto fail = [&] {
            if (host.publish_i64) PubI(host, "sw.repel.ready", 0);
        };
        const auto* a = sw_addresses::Find("field.repel");
        if (!a || !a->verified || !heap_begin) return fail();
        std::uint8_t raw[4]{};
        if (!ReadStable(host, sw_addresses::Resolve(heap_begin, *a), raw, sizeof raw)) return fail();
        const int item = raw[0] | (raw[1] << 8);
        const int steps = raw[2] | (raw[3] << 8);
        // An unknown item id means this is not the record we think it is (no heuristics).
        if (!(item == 0 || sw_helper::IsRepel(item)) || steps > 250) return fail();
        repel_steps_ = steps;
        if (host.publish_i64) {
            PubI(host, "sw.repel.ready", 1);
            PubI(host, "sw.repel.active", steps > 0 ? 1 : 0);
            PubI(host, "sw.repel.steps", steps);
        }
    }

    // MyItem8 (KItem) pouch: decodes the whole 1214-slot block, then publishes (a) a per-pocket
    // "selected"/"badge" ("!" = has a NEW item) summary for all 9 pockets always, and (b) a
    // compacted (zero gaps) row list of the COMPANION-SELECTED pocket's owned items only, capped
    // at kMaxBagRows, and the selected row's detail card. The item use itself (field medicine,
    // Repel) is the module's field action; this domain supplies the selected item and its count.
    // Change-driven: re-rendered only when the block or the companion selection changes.
    void SampleBag(const EdenDsmodHostApi& host, u64 heap_begin) {
        const auto* a = sw_addresses::Find("bag.item_pouch");
        PublishDomainStatus(host, "bag", a);
        const auto fail = [&] {
            bag_sel_item_id_ = 0;
            bag_sel_item_count_ = 0;
            bag_raw_ok_ = false;
            bag_tape_.Clear();
            PubI(host, "sw.bike.owned_known", 0);
            PubI(host, "sw.bike.owned", 0);
        };
        if (!a || !a->verified) return fail();
        bag_raw_.resize(sw_item_pouch::kBlockBytes);
        if (!ReadStable(host, sw_addresses::Resolve(heap_begin, *a), bag_raw_.data(), bag_raw_.size()))
            return fail();
        // Kept for the battle page's Bag tab (KItem is live: an item used in battle decrements here).
        bag_raw_ok_ = true;
        // Rotom Bike obtained = the Key Items pouch holds the bike in any of its forms (1081, the
        // water-upgraded 1266, the two colours; a new game's bag has none). Unknown (no bag read)
        // stays owned_known=0.
        PubI(host, "sw.bike.owned_known", 1);
        PubI(host, "sw.bike.owned", sw_item_pouch::HasRotomBike(bag_raw_.data(), bag_raw_.size()) ? 1 : 0);
        std::uint64_t sig = HashBytes(bag_raw_.data(), bag_raw_.size());
        sig = HashValue(static_cast<std::uint64_t>(bag_sel_pocket_) << 32 | static_cast<std::uint32_t>(bag_sel_row_), sig);
        Taped(bag_tape_, sig, host, [&] { RenderBag(host, bag_raw_); });
    }

    void RenderBag(const EdenDsmodHostApi& host, const std::vector<std::uint8_t>& raw) {
        bag_sel_item_id_ = 0;
        bag_sel_item_count_ = 0;
        EnsureItemNames(host);
        EnsureItemDescriptions(host);

        int sel_pocket = bag_sel_pocket_;
        if (sel_pocket < 0 || sel_pocket >= sw_item_pouch::kPouchCount) sel_pocket = 0;

        char key[64];
        for (int p = 0; p < sw_item_pouch::kPouchCount; ++p) {
            bool has_new = false;
            const auto& pd = sw_item_pouch::kPouches[p];
            for (std::size_t s = 0; s < pd.slot_count; ++s) {
                const auto slot = sw_item_pouch::DecodeSlotAt(raw.data(), raw.size(), p, s);
                if (slot.item_id != 0 && slot.is_new) { has_new = true; break; }
            }
            std::snprintf(key, sizeof key, "sw.bag.pocket.%d.selected", p);
            if (host.publish_i64) PubI(host, key, p == sel_pocket ? 1 : 0);
            std::snprintf(key, sizeof key, "sw.bag.pocket.%d.badge", p);
            if (host.publish_i64) PubI(host, key, has_new ? 1 : 0);
        }
        if (host.publish_i64) PubI(host, "sw.bag.sel_pocket", sel_pocket);
        if (host.publish_text)
            PubT(host, "sw.bag.pocket_name", PocketName(sel_pocket));

        // Compact the selected pocket's nonzero slots into a 0-based row list (empty pouch
        // capacity slots are not real items and are never shown as rows).
        const auto& pd = sw_item_pouch::kPouches[sel_pocket];
        std::vector<sw_item_pouch::Slot> rows;
        rows.reserve(pd.slot_count);
        for (std::size_t s = 0; s < pd.slot_count; ++s) {
            const auto slot = sw_item_pouch::DecodeSlotAt(raw.data(), raw.size(), sel_pocket, s);
            if (slot.item_id != 0) rows.push_back(slot);
        }
        const int row_total = static_cast<int>(rows.size());
        const int row_count = std::min(row_total, kMaxBagRows);
        if (host.publish_i64) {
            PubI(host, "sw.bag.row_count", row_count);
            PubI(host, "sw.bag.row_total", row_total);
            // Honest truncation flag: only the first kMaxBagRows owned items of a pocket larger
            // than that are shown (this module's verified fixture never exceeds it, but a
            // different save's "Items"/"TMs" pocket plausibly could) -- never silently dropped.
            PubI(host, "sw.bag.truncated", row_total > row_count ? 1 : 0);
        }
        if (row_total > row_count && host.publish_text) {
            PubT(host, "sw.bag.overflow_text", sw_str::Format(sw_str::Id::kBagOverflow, row_total - row_count).c_str());
        }

        int sel_row = bag_sel_row_;
        if (row_count <= 0) sel_row = -1;
        else if (sel_row < 0 || sel_row >= row_count) sel_row = 0;
        if (host.publish_i64) PubI(host, "sw.bag.sel_row", sel_row);

        // Rows past row_count publish nothing: the list's repeat is bound to row_count (no
        // per-row ready/item_id key is bound, and the tape replays every key every sample).
        for (int i = 0; i < row_count && i < kMaxBagRows; ++i) {
            const auto& slot = rows[static_cast<std::size_t>(i)];
            std::snprintf(key, sizeof key, "sw.bag.row.%d.selected", i);
            if (host.publish_i64) PubI(host, key, i == sel_row ? 1 : 0);

            std::string name;
            const bool have_name = item_names_ready_ && sw_item_names::ItemName(item_names_, slot.item_id, name);
            if (have_name) {
                std::snprintf(key, sizeof key, "sw.bag.row.%d.name", i);
                PubT(host, key, name.c_str());
            }
            if (host.publish_text) {
                char count_text[24];
                std::snprintf(count_text, sizeof count_text, "x%u",
                              static_cast<unsigned>(slot.count));
                std::snprintf(key, sizeof key, "sw.bag.row.%d.count_text", i);
                PubT(host, key, count_text);
            }
            char icon_src[160]{};
            const bool icon_ok = have_name && sw_item_icons::IconSource(slot.item_id, icon_src, sizeof icon_src);
            std::snprintf(key, sizeof key, "sw.bag.row.%d.icon_ready", i);
            if (host.publish_i64) PubI(host, key, icon_ok ? 1 : 0);
            if (icon_ok && host.publish_text) {
                std::snprintf(key, sizeof key, "sw.bag.row.%d.icon_src", i);
                PubT(host, key, icon_src);
            }
        }

        // Detail card: icon/name/description for the selected row only.
        const bool detail_ok = sel_row >= 0 && sel_row < row_count;
        PubI(host, "sw.bag.detail.description_ready", 0);
        if (host.publish_i64) PubI(host, "sw.bag.detail.ready", detail_ok ? 1 : 0);
        if (!detail_ok) return;
        const auto& dslot = rows[static_cast<std::size_t>(sel_row)];
        bag_sel_item_id_ = dslot.item_id;
        bag_sel_item_count_ = static_cast<int>(dslot.count);
        std::string dname;
        const bool have_dname = item_names_ready_ && sw_item_names::ItemName(item_names_, dslot.item_id, dname);
        if (have_dname) PubT(host, "sw.bag.detail.name", dname.c_str());
        char dicon[160]{};
        if (have_dname && sw_item_icons::IconSource(dslot.item_id, dicon, sizeof dicon)) {
            if (host.publish_i64) PubI(host, "sw.bag.detail.icon_ready", 1);
            if (host.publish_text) PubT(host, "sw.bag.detail.icon_src", dicon);
        } else if (host.publish_i64) {
            PubI(host, "sw.bag.detail.icon_ready", 0);
        }
        std::string ddesc;
        if (item_descs_ready_ &&
            sw_item_descriptions::ItemDescription(item_descs_, dslot.item_id, ddesc) &&
            host.publish_text) {
            const std::string full_description = FlatGameText(ddesc);
            PubT(host, "sw.bag.detail.description", full_description.c_str());
            PubI(host, "sw.bag.detail.description_ready", !full_description.empty() ? 1 : 0);
        }
    }

    // Zukan8 (Galar/Isle of Armor/Crown Tundra dex): counts only for discovered regions, plus
    // a full ordered entry list (native "No. NNN" order) for the COMPANION-SELECTED region,
    // spoiler-gated (an unseen entry publishes only its number, never a name/icon/caught state --
    // the native silhouette convention) and a detail card for the selected entry (seen entries
    // only).
    // Change-driven and throttled (review H3): the ~39 KB of Zukan blocks change only when a
    // species is seen/caught, so they are re-read at most every kSlowRefreshTicks (at once on a
    // selection change or when the evolution table finishes loading); the ~3000 row/detail keys
    // are re-rendered only when the blocks or the selection changed, else replayed.
    std::uint64_t dex_read_tick_ = 0;
    std::uint64_t dex_sel_seen_ = ~0ull;
    void SampleDex(const EdenDsmodHostApi& host, u64 heap_begin) {
        const auto* a_galar = sw_addresses::Find("dex.galar");
        const auto* a_armor = sw_addresses::Find("dex.isle_of_armor");
        const auto* a_crown = sw_addresses::Find("dex.crown_tundra");
        PublishDomainStatus(host, "dex", a_galar);
        if (!a_galar || !a_galar->verified || !a_armor || !a_armor->verified || !a_crown ||
            !a_crown->verified)
            return;

        const std::uint64_t sel = static_cast<std::uint64_t>(dex_sel_region_) << 32 |
                                  static_cast<std::uint32_t>(dex_sel_row_) | (evo_table_ready_ ? 1ull << 40 : 0) |
                                  static_cast<std::uint64_t>(dex_detail_tab_) << 41; // tab change re-publishes
        if (dex_tape_.valid && sel == dex_sel_seen_ && tick_ - dex_read_tick_ < kSlowRefreshTicks) {
            dex_tape_.Replay(host);
            return;
        }
        std::vector<std::uint8_t> galar(a_galar->size), armor(a_armor->size), crown(a_crown->size);
        if (!ReadStable(host, sw_addresses::Resolve(heap_begin, *a_galar), galar.data(), galar.size()) ||
            !ReadStable(host, sw_addresses::Resolve(heap_begin, *a_armor), armor.data(), armor.size()) ||
            !ReadStable(host, sw_addresses::Resolve(heap_begin, *a_crown), crown.data(), crown.size())) {
            dex_tape_.Clear();
            return;
        }
        dex_read_tick_ = tick_;
        dex_sel_seen_ = sel;
        std::uint64_t sig = HashBytes(galar.data(), galar.size());
        sig = HashBytes(armor.data(), armor.size(), sig);
        sig = HashBytes(crown.data(), crown.size(), sig);
        if (!dex_blocks_ok_ || sig != dex_blocks_sig_) { // the route grid's caught balls
            dex_galar_ = galar;
            dex_armor_ = armor;
            dex_crown_ = crown;
            dex_blocks_sig_ = sig;
            dex_blocks_ok_ = true;
        }
        Taped(dex_tape_, HashValue(sel, sig), host, [&] { RenderDex(host, galar, armor, crown); });
    }

    void RenderDex(const EdenDsmodHostApi& host, const std::vector<std::uint8_t>& galar,
                   const std::vector<std::uint8_t>& armor, const std::vector<std::uint8_t>& crown) {
        EnsureSpeciesNames(host);
        EnsureDexFlavorText(host);
        EnsureDexReverseIndex(host);
        EnsureItemNames(host);   // evolution-row method text ("Use item")
        EnsureMoveNames(host);   // evolution-row method text ("knows a move")
        // The reverse index is no longer optional: a region's own block only ever holds the
        // species actually HOMED there (Zukan8's single-home-per-species model -- see
        // sw_dex_index.h's header comment), which can be far fewer than that region's real
        // DISPLAY count (e.g. this module's own fixture: Crown Tundra displays 210 species but
        // only 74 are Crown-homed, the other 136 are Galar/Armor-homed "visitors"). Without the
        // reverse index there is no way to resolve a displayed species back to its real flags,
        // so every domain output fails closed rather than silently reporting the undercounted
        // per-block-only total the first version of this function shipped with.
        if (!dex_reverse_ready_) return;

        // Resolves a species' REAL seen/caught state via its HOME block (which may differ from
        // whichever region is currently being displayed/counted) -- the fix for the bug a
        // game-session review caught live (Crown Tundra read 74/210 instead of the real 210/210).
        auto resolve = [&](std::uint16_t species) -> sw_zukan::EntryState {
            if (species == 0) return {};
            const auto home = dex_reverse_.home_region[species];
            const std::uint16_t idx = dex_reverse_.home_index[species]; // 1-based
            if (idx == 0) return {};
            switch (home) {
                case sw_dex_index::Home::kGalar:
                    return sw_zukan::DecodeEntry(galar.data(), galar.size(), idx - 1);
                case sw_dex_index::Home::kArmor:
                    return sw_zukan::DecodeEntry(armor.data(), armor.size(), idx - 1);
                case sw_dex_index::Home::kCrown:
                    return sw_zukan::DecodeEntry(crown.data(), crown.size(), idx - 1);
                default:
                    return {};
            }
        };

        struct Region {
            int count;
            const std::uint16_t* display; // display[i] (0-based) = species shown at "No. i+1"
        };
        // Galar's display list IS its home list (Galar always wins Zukan8's priority check, so
        // no species can display in Galar with a different home) -- Armor/Crown need their own
        // DISPLAY arrays (built from each species' own <Region>DexIndex field, independent of
        // home; see sw_dex_index.h).
        const Region regions[3]{
            {sw_dex_index::kGalarCount, dex_reverse_.galar_home.data()},
            {sw_dex_index::kArmorCount, dex_reverse_.armor_display.data()},
            {sw_dex_index::kCrownCount, dex_reverse_.crown_display.data()},
        };

        char key[72];
        for (int r = 0; r < 3; ++r) {
            std::snprintf(key, sizeof key, "sw.dex.region.%d.available", r);
            PubI(host, key, dex_region_available_[static_cast<std::size_t>(r)] ? 1 : 0);
            if (!dex_region_available_[static_cast<std::size_t>(r)]) continue;
            int seen = 0, caught = 0;
            for (int i = 0; i < regions[r].count; ++i) {
                const auto st = resolve(regions[r].display[i]);
                if (st.seen) ++seen;
                if (st.caught) ++caught;
            }
            std::snprintf(key, sizeof key, "sw.dex.region.%d.seen", r);
            if (host.publish_i64) PubI(host, key, seen);
            std::snprintf(key, sizeof key, "sw.dex.region.%d.caught", r);
            if (host.publish_i64) PubI(host, key, caught);
            std::snprintf(key, sizeof key, "sw.dex.region.%d.total", r);
            if (host.publish_i64) PubI(host, key, regions[r].count);
        }

        int sel_region = dex_sel_region_;
        if (sel_region < 0 || sel_region > 2) sel_region = 0;
        if (host.publish_i64) PubI(host, "sw.dex.sel_region", sel_region);
        for (int r = 0; r < 3; ++r) {
            std::snprintf(key, sizeof key, "sw.dex.region.%d.selected", r);
            if (host.publish_i64) PubI(host, key, r == sel_region ? 1 : 0);
        }

        const Region& reg = regions[sel_region];
        const int row_count = std::min(reg.count, kMaxDexRows);
        if (host.publish_i64) PubI(host, "sw.dex.row_count", row_count);

        int sel_row = dex_sel_row_;
        if (row_count <= 0) sel_row = -1;
        else if (sel_row < 0 || sel_row >= row_count) sel_row = 0;
        if (host.publish_i64) PubI(host, "sw.dex.sel_row", sel_row);

        std::uint16_t detail_species = 0;
        bool detail_seen = false, detail_caught = false;
        // Same as the Bag rows: nothing past row_count (the repeat is bound to row_count).
        for (int i = 0; i < row_count && i < kMaxDexRows; ++i) {
            const std::uint16_t species = reg.display[i];
            const sw_zukan::EntryState st = resolve(species);

            std::snprintf(key, sizeof key, "sw.dex.row.%d.seen", i);
            if (host.publish_i64) PubI(host, key, st.seen ? 1 : 0);
            std::snprintf(key, sizeof key, "sw.dex.row.%d.caught", i);
            if (host.publish_i64) PubI(host, key, st.caught ? 1 : 0);
            std::snprintf(key, sizeof key, "sw.dex.row.%d.selected", i);
            if (host.publish_i64) PubI(host, key, i == sel_row ? 1 : 0);

            char no_text[16];
            std::snprintf(no_text, sizeof no_text, sw_str::S(sw_str::Id::kDexNumber), i + 1);
            std::snprintf(key, sizeof key, "sw.dex.row.%d.dex_no_text", i);
            if (host.publish_text) PubT(host, key, no_text);

            if (i == sel_row) {
                detail_species = species;
                detail_seen = st.seen;
                detail_caught = st.caught;
            }

            // Spoiler rule: an unseen entry publishes nothing beyond number/seen/caught above --
            // no species identity (name/icon) leaks for a species the save has never seen.
            if (!st.seen || species == 0) continue;
            std::string name;
            if (species_names_ready_ && sw_species_names::SpeciesName(species_names_, species, name) &&
                host.publish_text) {
                std::snprintf(key, sizeof key, "sw.dex.row.%d.name", i);
                PubT(host, key, name.c_str());
            }
            char icon_src[160]{};
            if (sw_species_icons::IconSource(species, icon_src, sizeof icon_src)) {
                std::snprintf(key, sizeof key, "sw.dex.row.%d.icon_ready", i);
                if (host.publish_i64) PubI(host, key, 1);
                std::snprintf(key, sizeof key, "sw.dex.row.%d.icon_src", i);
                if (host.publish_text) PubT(host, key, icon_src);
                std::snprintf(key, sizeof key, "sw.dex.row.%d", i);
                PublishIconOffset(host, key, species);
            }
        }

        // Detail view: same spoiler rule -- an unseen/unresolved selected entry publishes only
        // sw.dex.detail.ready=0.
        const bool detail_ok = sel_row >= 0 && sel_row < row_count && detail_seen && detail_species != 0;
        PubI(host, "sw.dex.detail.description_ready", 0);
        if (host.publish_i64) PubI(host, "sw.dex.detail.ready", detail_ok ? 1 : 0);
        for (int j = 0; j < 3; ++j) {
            char tk[32];
            std::snprintf(tk, sizeof tk, "sw.dex.detail.tab%d", j);
            PubI(host, tk, dex_detail_tab_ == j ? 1 : 0);
        }
        PubI(host, "sw.dex.detail.info_ready", 0);
        if (!detail_ok) return;
        PublishDexInfo(host, detail_species);
        PubI(host, "sw.dex.detail.species", detail_species);

        char no_text[16];
        std::snprintf(no_text, sizeof no_text, sw_str::S(sw_str::Id::kDexNumber), sel_row + 1);
        if (host.publish_text) PubT(host, "sw.dex.detail.dex_no_text", no_text);
        if (host.publish_i64) PubI(host, "sw.dex.detail.caught", detail_caught ? 1 : 0);

        std::string name;
        if (species_names_ready_ && sw_species_names::SpeciesName(species_names_, detail_species, name) &&
            host.publish_text)
            PubT(host, "sw.dex.detail.name", name.c_str());
        char icon_src[160]{};
        if (sw_species_icons::IconSource(detail_species, icon_src, sizeof icon_src)) {
            if (host.publish_i64) PubI(host, "sw.dex.detail.icon_ready", 1);
            if (host.publish_text) PubT(host, "sw.dex.detail.icon_src", icon_src);
            PublishIconOffset(host, "sw.dex.detail", detail_species);
        } else if (host.publish_i64) {
            PubI(host, "sw.dex.detail.icon_ready", 0);
        }

        // Types + weakness/immunity board, reusing the Pokémon page's own helper (same fields:
        // type1/2, type1/2_src, type1/2_label_src, weak<j>/immune<j> discs).
        PublishWeaknessBoard(host, "sw.dex.detail", detail_species);

        std::string flavor;
        if (dex_flavor_ready_ && sw_dex_text::FlavorText(dex_flavor_, detail_species, flavor) &&
            host.publish_text) {
            // Live-verified (label g0): the detail card's flavor-text slot is only ~90px tall
            // before the "Super effective"/weakness-board row starts (y 432 vs that row's own
            // y 524) -- matches the original D2 mockup's own 2-line convention for this box
            // (dir_a.page_dex's Bergmite example: two text() calls at y 432/472, never more) --
            // so 2 lines, not 4, avoids the board overlap a 3rd+ line caused live.
            const std::string full_description = FlatGameText(flavor);
            PubT(host, "sw.dex.detail.description", full_description.c_str());
            PubI(host, "sw.dex.detail.description_ready", !full_description.empty() ? 1 : 0);
        }

        // Show discovered members only. Unknown silhouettes/counts disclose whether further
        // stages exist; filtering first also keeps the selected Eevee branch in the window.
        // Methods require a known parent in the row; arrows require actual adjacent
        // parent -> child pairs, never an omitted unknown stage or sibling evolutions.
        if (evo_table_ready_) {
            const auto chain = sw_evolution::KnownChain(evo_table_, detail_species, [&](int species) {
                return resolve(static_cast<std::uint16_t>(species)).seen;
            });
            const int evo_n = std::min(static_cast<int>(chain.size()), kMaxEvoMembers);
            int evo_focus = 0;
            for (int i = 0; i < evo_n; ++i)
                if (chain[static_cast<std::size_t>(i)].species == detail_species) evo_focus = i;
            PubI(host, "sw.dex.detail.evo_focus", evo_focus);
            if (host.publish_i64) PubI(host, "sw.dex.detail.evo_count", evo_n);
            for (int i = 0; i < kMaxEvoMembers; ++i) {
                const bool slot_ready = i < evo_n;
                std::snprintf(key, sizeof key, "sw.dex.detail.evo.%d.ready", i);
                if (host.publish_i64) PubI(host, key, slot_ready ? 1 : 0);
                if (!slot_ready) continue;

                const auto& m = chain[static_cast<std::size_t>(i)];
                const bool is_selected = m.species == static_cast<int>(detail_species);
                const sw_zukan::EntryState st =
                    is_selected ? sw_zukan::EntryState{detail_seen, detail_caught}
                                : resolve(static_cast<std::uint16_t>(m.species));
                std::snprintf(key, sizeof key, "sw.dex.detail.evo.%d.seen", i);
                if (host.publish_i64) PubI(host, key, st.seen ? 1 : 0);
                std::snprintf(key, sizeof key, "sw.dex.detail.evo.%d.selected", i);
                if (host.publish_i64) PubI(host, key, is_selected ? 1 : 0);
                // The sprite's art size class (sw_route::IconClass): the manifest scales each
                // class into the member's 136x112 box. -1 = no sprite (unseen / no icon).
                std::snprintf(key, sizeof key, "sw.dex.detail.evo.%d.icon_class", i);
                if (host.publish_i64) PubI(host, key, -1);

                std::snprintf(key, sizeof key, "sw.dex.detail.evo.%d.method_ready", i);
                const bool method_ready = sw_evolution::KnownParent(evo_table_, chain, i);
                if (host.publish_i64) PubI(host, key, method_ready ? 1 : 0);
                std::snprintf(key, sizeof key, "sw.dex.detail.evo.%d.link_ready", i);
                if (host.publish_i64) PubI(host, key, sw_evolution::ConsecutiveEvolution(evo_table_, chain, i) ? 1 : 0);
                if (method_ready) {
                    const std::string text = sw_evolution::MethodText(
                        m.via,
                        [&](int item_id, std::string& out_name) {
                            return item_names_ready_ &&
                                   sw_item_names::ItemName(item_names_,
                                                           static_cast<std::uint16_t>(item_id), out_name);
                        },
                        [&](int move_id, std::string& out_name) {
                            return move_names_ready_ &&
                                   sw_move_names::MoveName(move_names_,
                                                           static_cast<std::uint16_t>(move_id), out_name);
                        });
                    std::snprintf(key, sizeof key, "sw.dex.detail.evo.%d.method_text", i);
                    if (host.publish_text) PubT(host, key, sw_text_fit::BreakLongHyphenated(text, 4, kEvoMethodWidth).c_str());
                }

                if (!st.seen) continue; // silhouette: nothing beyond ready/seen/selected above

                std::string evo_name;
                if (species_names_ready_ &&
                    sw_species_names::SpeciesName(species_names_, static_cast<std::uint16_t>(m.species),
                                                  evo_name) &&
                    host.publish_text) {
                    std::snprintf(key, sizeof key, "sw.dex.detail.evo.%d.name", i);
                    PubT(host, key, evo_name.c_str());
                }
                char evo_icon_src[160]{};
                if (sw_species_icons::IconSource(static_cast<std::uint16_t>(m.species), evo_icon_src,
                                                 sizeof evo_icon_src)) {
                    std::snprintf(key, sizeof key, "sw.dex.detail.evo.%d.icon_ready", i);
                    if (host.publish_i64) PubI(host, key, 1);
                    std::snprintf(key, sizeof key, "sw.dex.detail.evo.%d.icon_src", i);
                    if (host.publish_text) PubT(host, key, evo_icon_src);
                    std::snprintf(key, sizeof key, "sw.dex.detail.evo.%d", i);
                    PublishIconOffset(host, key, static_cast<std::uint16_t>(m.species));
                    std::snprintf(key, sizeof key, "sw.dex.detail.evo.%d.icon_class", i);
                    if (host.publish_i64) PubI(host, key, sw_route::IconClass(static_cast<std::uint16_t>(m.species)));
                }
            }
        } else if (host.publish_i64) {
            PubI(host, "sw.dex.detail.evo_count", 0);
        }
    }
    // Current-zone place-name popup (sw_addresses.h's "location.zone_name", sw_zone.h's
    // DecodeZoneName) -- the domain's one verified lead, so PublishDomainStatus now reports
    // against IT, not "location.coordinates" (Coordinates8 itself remains a separate,
    // unresolved gap -- LIVE-READERS-132.md's position-reader hunt; this domain has no live X/Y,
    // only the place-name string).
    void SampleLocation(const EdenDsmodHostApi& host, u64 heap_begin) {
        const auto* a = sw_addresses::Find("location.zone_name");
        PublishDomainStatus(host, "location", a);

        zone_cache_ready_ = false;
        if (!a || !a->verified) return;

        std::array<std::uint8_t, 0x98> raw{};
        if (raw.size() != a->size) return; // keep the read bound and the anchor's own size in lockstep
        if (!ReadStable(host, sw_addresses::Resolve(heap_begin, *a), raw.data(), raw.size())) return;

        std::string zone;
        if (!sw_zone::DecodeZoneName(raw.data(), raw.size(), &zone)) return; // tag mismatch/empty: fail closed

        if (zone != zone_cache_ || !zone_id_known_) {
            zone_id_cache_ = sw_zone::FindZoneId(zone, EnsurePlaceLines(host));
            zone_id_known_ = true;
        }
        zone_cache_ = zone;
        zone_cache_ready_ = true;

        const int zone_id = zone_id_cache_;
        if (host.publish_i64) PubI(host, "sw.location.zone_ready", 1);
        // The popup is in the game's language; shown in the companion's when the name is a known
        // place (a Wild Area zone, or any Town Map place / facility name of an offered language).
        if (host.publish_text) {
            std::string shown = zone_id >= 0 ? sw_zone::ZoneName(place_lines_, zone_id, sw_lang::Current()) : std::string{};
            if (shown.empty()) shown = LocalizePlace(host, zone);
            PubT(host, "sw.location.zone_name", shown.c_str());
        }

        if (zone_id >= 0) {
            // A real Wild Area / Isle of Armor / Crown Tundra sub-zone: publish the region
            // subtitle. A town or any other unmapped place (Wyndon, a Pokémon Center interior,
            // Camp, ...) correctly finds no match here -- the region key is simply never
            // published this tick, which the manifest's need_bind turns into "omit", not a guess.
            if (host.publish_text)
                PubT(host, "sw.location.region", sw_str::RegionName(sw_zone::kZoneRegion[zone_id]));
            if (host.publish_i64) PubI(host, "sw.location.region_ready", 1);
        }
    }

    void SampleWorldWeather(const EdenDsmodHostApi& host) {
        if (!sw_overworld::kWeatherRuntimeVerified) return;
        std::uint64_t slot = 0, global = 0, manager = 0, weather = 0, parameters = 0, recheck = 0;
        std::uint32_t value = 0;
        if (!ReadStable(host, host.main_base + sw_variant::kMain.field_slot, &slot, sizeof slot) || !slot ||
            !ReadStable(host, slot, &global, sizeof global) || !global ||
            !ReadStable(host, global + 0x90, &manager, sizeof manager) || !manager ||
            !ReadStable(host, manager + 0x408, &weather, sizeof weather) || !weather ||
            !ReadStable(host, weather + 0x3F8, &parameters, sizeof parameters) || !parameters ||
            !ReadStable(host, parameters + 0x60, &value, sizeof value) || value >= sw_overworld::kWeatherCount ||
            !ReadStable(host, weather + 0x3F8, &recheck, sizeof recheck) || recheck != parameters) return;
        PubI(host, "sw.location.weather.ready", 1);
        PubI(host, "sw.location.weather.id", value);
        live_weather_ = static_cast<int>(value);
        const std::string icon = sw_overworld::WeatherIcon(value);
        PubT(host, "sw.location.weather.icon_src", icon.c_str());
    }

    std::uint64_t world_player_ = 0, world_signature_ = 0, world_scan_tick_ = 0;
    bool bike_state_ready_ = false, bike_riding_ = false;
    bool IsNativePlayer(const EdenDsmodHostApi& host, std::uint64_t object) {
        std::uint64_t table = 0, getter = 0;
        if (!object || !ReadStable(host, object + 0x50, &table, sizeof table) || !table ||
            !ReadStable(host, table, &getter, sizeof getter)) return false;
        for (auto offset : sw_overworld::kPlayerTypeGetters)
            if (getter == host.main_base + offset) return true;
        return false;
    }
    void SampleWorldPlayer(const EdenDsmodHostApi& host) {
        if (!sw_overworld::kPositionRuntimeVerified) return;
        std::uint64_t slot = 0, world = 0;
        std::array<std::uint64_t, 2> vector{};
        if (!ReadStable(host, host.main_base + sw_variant::kMain.world_slot, &slot, sizeof slot) || !slot ||
            !ReadStable(host, slot, &world, sizeof world) || !world ||
            !ReadStable(host, world + 0xB0, vector.data(), sizeof vector) || vector[0] == 0 ||
            vector[1] < vector[0] || (vector[1] - vector[0]) % 8 ||
            vector[1] - vector[0] > 2048 * sizeof(std::uint64_t)) return;
        const std::size_t count = (vector[1] - vector[0]) / sizeof(std::uint64_t);
        if (count == 0) return;
        std::vector<std::uint64_t> objects(count);
        if (!ReadStable(host, vector[0], objects.data(), count * sizeof(std::uint64_t))) return;
        const std::uint64_t signature = HashValue(world, HashBytes(objects.data(), count * sizeof(std::uint64_t)));
        if (signature != world_signature_ || world_player_ == 0) {
            if (signature == world_signature_ && tick_ - world_scan_tick_ < kSlowRefreshTicks) return;
            world_player_ = 0;
            int candidates = 0;
            for (const auto object : objects) {
                if (!IsNativePlayer(host, object)) continue;
                world_player_ = object;
                ++candidates;
            }
            if (candidates != 1) world_player_ = 0; // never pick arbitrarily among several players
            world_signature_ = signature;
            world_scan_tick_ = tick_;
        }
        if (!world_player_ || !IsNativePlayer(host, world_player_)) {
            world_player_ = 0;
            return;
        }
        // World-space xyz is read and validated only to confirm world_player_ is a live, sane
        // player object before trusting it for the bike check below; nothing on any page shows
        // raw player position (no gen_manifest.py binding), so it is no longer published.
        std::array<float, 3> xyz{};
        if (!ReadStable(host, world_player_ + 0xB0, xyz.data(), sizeof xyz) ||
            !sw_overworld::ValidPosition(xyz)) return;
        // Mounted state is independent of whether the bike is legal to summon here. Do not
        // infer a native-action readiness gate from this byte or show an invented charge value.
        std::uint64_t wrapper = 0, component = 0;
        std::uint8_t mounted = 0;
        if (sw_overworld::kBikeRuntimeVerified &&
            ReadStable(host, world_player_ + 0x588, &wrapper, sizeof wrapper) && wrapper &&
            ReadStable(host, wrapper + 0x60, &component, sizeof component) && component &&
            ReadStable(host, component + 0x498, &mounted, sizeof mounted) && mounted <= 1) {
            bike_state_ready_ = true;
            bike_riding_ = mounted != 0;
            PubI(host, "sw.bike.state_ready", 1);
            PubI(host, "sw.bike.riding", mounted);
        }
    }
    void SampleOverworld(const EdenDsmodHostApi& host) {
        PubI(host, "sw.location.weather.ready", 0);
        live_weather_ = -1;
        bike_state_ready_ = false;
        bike_riding_ = false;
        PubI(host, "sw.bike.state_ready", 0);
        bool map_ok = false;
        if (!host.main_base || !phase_known_ ||
            (phase_ != Phase::kField && phase_ != Phase::kMenu)) {
            world_player_ = 0;
            world_signature_ = 0;
            if (phase_ == Phase::kTitle) {
                SetMapZoneHint(0, 0);
                map_last_.valid = false;
            }
        } else {
            SampleWorldWeather(host);
            SampleWorldPlayer(host);
            map_ok = SampleTownMap(host);
        }
        // Door/floor transitions: an interior zone has no Town Map pin of its own until the
        // field helper's native parent-zone hint for it arrives (next FLY_QUERY), and the load
        // between them is not field. Hold the last resolved pin through that gap instead of
        // dropping the map (which fell back to the zoomed-out overview, then zoomed back in).
        if (!map_ok) {
            if (map_last_.valid && phase_known_ && phase_ != Phase::kTitle) PublishTownMapPoint(host, map_last_);
            else PubI(host, "sw.location.map.ready", 0);
        }
    }

    sw_town_map::Table town_map_;
    sw_map_selection::Catalog map_catalog_;
    sw_map_selection::Selection map_selection_;
    bool map_catalog_attempted_ = false, map_catalog_ready_ = false;
    // Place names for the Fly list (FlyName), loaded once per language.
    std::vector<sw_map_selection::NamedZone> map_names_;
    std::string battle_tower_name_;
    bool place_names_attempted_ = false;
    void EnsurePlaceNames(const EdenDsmodHostApi& host) {
        if (place_names_attempted_) return;
        place_names_attempted_ = true;
        if (!sw_map_selection::LoadNames(host, map_names_)) map_names_.clear();
        battle_tower_name_ = sw_map_selection::FacilityName(host, "townmap_factory_33");
    }
    // A place name in any of the game's languages -> the same line in the companion language
    // (script/place_name, common/townmap_facility: identical line order in every language).
    // Unknown text is returned unchanged.
    // Also the Wild Area zone names (sw_zone FindZoneId / ZoneName: the module has none of its own).
    sw_zone::PlaceLines place_lines_, facility_lines_;
    bool place_lines_attempted_ = false;
    int zone_id_cache_ = -1;
    bool zone_id_known_ = false;
    const sw_zone::PlaceLines& EnsurePlaceLines(const EdenDsmodHostApi& host) {
        if (!place_lines_attempted_) {
            place_lines_attempted_ = true;
            for (int l = 0; l < sw_lang::kCount; ++l) {
                const auto lang = static_cast<sw_lang::Lang>(l);
                sw_party_details::LoadText(host, sw_lang::MessagePath(lang, "place_name.dat", "script").c_str(), place_lines_[l]);
                sw_party_details::LoadText(host, sw_lang::MessagePath(lang, "townmap_facility.dat").c_str(), facility_lines_[l]);
            }
        }
        return place_lines_;
    }
    std::string LocalizePlace(const EdenDsmodHostApi& host, const std::string& text) {
        EnsurePlaceLines(host);
        const int cur = sw_lang::Index(sw_lang::Current());
        for (const auto* table : {&place_lines_, &facility_lines_}) {
            for (int l = 0; l < sw_lang::kCount; ++l) {
                const auto& lines = (*table)[l];
                for (std::size_t i = 0; i < lines.size(); ++i) {
                    if (lines[i].empty() || !sw_zone::SameZoneName(lines[i], text)) continue;
                    const auto& mine = (*table)[cur];
                    if (i < mine.size() && sw_map_selection::PlainName(mine[i])) return mine[i];
                    return text;
                }
            }
        }
        return text;
    }
    std::string PlaceNameOf(std::uint64_t zone_hash) const {
        for (const auto& n : map_names_) if (n.zone_hash == zone_hash) return n.name;
        return {};
    }
    int map_area_ = -1;
    std::uint64_t map_zone_ = 0;
    bool town_map_attempted_ = false, town_map_ready_ = false;
    std::uint64_t map_hint_zone_ = 0, map_hint_parent_ = 0;
    bool SampleTownMap(const EdenDsmodHostApi& host) {
        // Native D7E2E0: current field-zone hash from the active registry's first row. This
        // exact main-relative chain was read on two independent g0/battle boots with distinct
        // ASLR, resolving SlipperySlope and z_wr0110. Unknown/interior parent aliases stay
        // unpinned until the native parent-zone lookup is verified; never guess from XYZ.
        if (!world_player_ || !host.publish_f64) return false;
        std::uint64_t slot = 0, registry = 0, row = 0, zone = 0, recheck = 0;
        std::array<std::uint64_t, 2> rows{};
        if (!ReadStable(host, host.main_base + sw_variant::kMain.zone_registry_slot, &slot, sizeof slot) || !slot ||
            !ReadStable(host, slot, &registry, sizeof registry) || !registry ||
            !ReadStable(host, registry + 0x148, rows.data(), sizeof rows) || rows[0] == 0 ||
            rows[1] <= rows[0] || (rows[1] - rows[0]) % 8 || rows[1] - rows[0] > 256 * 8 ||
            !ReadStable(host, rows[0], &row, sizeof row) || !row ||
            !ReadStable(host, row + 0x3A0, &zone, sizeof zone) || zone == sw_town_map::kZoneHashBasis ||
            !ReadStable(host, rows[0], &recheck, sizeof recheck) || recheck != row) return false;
        field_zone_ = zone; // the raw zone (an interior stays itself: no wild list there)
        if (!town_map_attempted_) {
            town_map_attempted_ = true;
            town_map_ready_ = sw_town_map::Load(host, town_map_);
        }
        if (!town_map_ready_) return false;
        // The field helper obtains the native display-zone parent with F096D0. A hint
        // belongs only to the exact current-zone hash sampled alongside it. Once that zone
        // changes, discard it permanently rather than letting it reappear on a later visit.
        if (map_hint_zone_ && map_hint_zone_ != zone) SetMapZoneHint(0, 0);
        const auto display_zone = map_hint_zone_ == zone && map_hint_parent_ &&
                                  map_hint_parent_ != sw_town_map::kZoneHashBasis ? map_hint_parent_ : zone;
        const auto* point = sw_town_map::Find(town_map_, display_zone);
        if (!point) return false;
        if (map_zone_ != zone) map_selection_.Clear();
        map_zone_ = zone;
        map_area_ = point->area;
        map_last_ = {true, point->area, point->x, point->y, town_map_.areas[point->area].height};
        PublishTownMapPoint(host, map_last_);
        return true;
    }
    struct MapPoint { bool valid = false; int area = 0; float x = 0, y = 0, area_h = 0; };
    MapPoint map_last_;
    void PublishTownMapPoint(const EdenDsmodHostApi& host, const MapPoint& p) {
        if (!host.publish_f64) return;
        PubI(host, "sw.location.map.ready", 1);
        PubI(host, "sw.location.map.area_id", p.area);
        host.publish_f64(host.userdata, "sw.location.map.x", p.x);
        host.publish_f64(host.userdata, "sw.location.map.y", p.y);
        // The area's own height (native bitmap texels): lets the default framing stop at the
        // map's top/bottom edge instead of panning past it under the chip / party strip.
        host.publish_f64(host.userdata, "sw.location.map.area_h", p.area_h);
        PubT(host, "sw.location.map.marker_src",
             "romfs:/bin/appli/townmap/bin/townmap_top_00.arc#timg/__Combined.bntx#townmap_icon_player_00^q");
    }

    void SampleMapPlaces(const EdenDsmodHostApi& host) {
        if (map_area_ >= 0 && !map_catalog_attempted_) {
            map_catalog_attempted_ = true;
            map_catalog_ready_ = sw_map_selection::Load(host, map_catalog_);
            // Build() names the two native zones without a place label (Battle Tower, Route 10's
            // second zone) from sw_fly's English table; give them the companion language's name.
            EnsurePlaceNames(host);
            for (auto& anchor : map_catalog_.anchors) {
                if (anchor.zone_hash == sw_town_map::ZoneHash("z_c0306") && !battle_tower_name_.empty())
                    anchor.name = battle_tower_name_;
                else if (anchor.zone_hash == sw_town_map::ZoneHash("z_r1002"))
                    if (auto n = PlaceNameOf(sw_town_map::ZoneHash("z_r1001")); !n.empty()) anchor.name = std::move(n);
            }
        }
        map_selection_.Reconcile(map_catalog_, map_area_);
        PubI(host, "sw.map.place.count", map_catalog_ready_ ? map_catalog_.anchors.size() : 0);
        // The invisible tap targets' picture (manifest "icon_src_bind", the same key for every slot).
        // Not a one-frame "frames" list: an animated marker group makes the whole Town Map redraw
        // on the clock (~15 whole-map draws a second at idle).
        if (map_catalog_ready_) PubT(host, "sw.map.place.hit_src", "file:img/rr28.png");
        for (std::size_t i = 0; i < map_catalog_.anchors.size(); ++i) {
            const auto& anchor = map_catalog_.anchors[i];
            const auto p = "sw.map.place." + std::to_string(i) + ".";
            PubI(host, (p + "ready").c_str(), map_area_ == anchor.area ? 1 : 0);
            PubI(host, (p + "id").c_str(), anchor.id);
            PubI(host, (p + "area").c_str(), anchor.area);
            PubI(host, (p + "selected").c_str(), map_selection_.index() == static_cast<int>(i) ? 1 : 0);
            PubT(host, (p + "name").c_str(), anchor.name.c_str());
            if (host.publish_f64) {
                host.publish_f64(host.userdata, (p + "x").c_str(), anchor.x);
                host.publish_f64(host.userdata, (p + "y").c_str(), anchor.y);
            }
        }
        const auto* selected = map_ready() ? map_selection_.Get(map_catalog_) : nullptr;
        PubI(host, "sw.map.selected.ready", selected ? 1 : 0);
        if (!selected) return;
        PubI(host, "sw.map.selected.index", map_selection_.index());
        PubI(host, "sw.map.selected.id", selected->id);
        PubI(host, "sw.map.selected.area", selected->area);
        PubT(host, "sw.map.selected.name", selected->name.c_str());
        PubT(host, "sw.map.selected.icon_src", "romfs:/bin/appli/townmap/bin/townmap_top_00.arc#timg/__Combined.bntx#townmap_icon_cursor_00^q");
        if (host.publish_f64) {
            host.publish_f64(host.userdata, "sw.map.selected.x", selected->x);
            host.publish_f64(host.userdata, "sw.map.selected.y", selected->y);
        }
    }

    // --- Wild Pokemon: the Town Map row and the route grid page (owner 2026-11-01) ------------
    // Every species of the place with its icon and name, no seen gating (owner: "show everything",
    // Lumi behaviour); the red ball marks a species the player has caught (Pokédex flags).
    static constexpr int kRouteMaxRows = 28; // = gen_manifest.py ROUTE_MAX_ROWS (most in one weather: 26)
    static constexpr int kWildStrip = 5;     // preview icons on the Town Map row
    std::uint64_t field_zone_ = 0;           // raw current zone (SampleTownMap), 0 = unknown
    int live_weather_ = -1;                  // sw.location.weather.id this tick, -1 = unknown
    bool champion_known_ = false, champion_ = false;
    bool route_open_ = false;
    std::uint64_t route_zone_ = 0, wild_target_ = 0;
    int route_weather_ = -1;
    std::int64_t route_serial_ = 0; // a new place / weather: the grid scrolls back to the top
    Tape route_tape_, wild_tape_;
    std::vector<std::uint8_t> dex_galar_, dex_armor_, dex_crown_;
    std::uint64_t dex_blocks_sig_ = 0;
    bool dex_blocks_ok_ = false;

    // Hall of Fame entered (save block KEnteredHallOfFame, sw_addresses "progress.hall_of_fame"):
    // after it, Wild Area / Isle of Armor / Crown Tundra wild Pokemon are Lv. 60 or more.
    void SampleProgress(const EdenDsmodHostApi& host, u64 heap_begin) {
        const auto* a = sw_addresses::Find("progress.hall_of_fame");
        std::uint64_t when = 0;
        champion_known_ = false;
        // A unix time (the first Hall of Fame entry) or 0; anything past 2100 is not this block.
        if (a && a->verified && ReadStable(host, sw_addresses::Resolve(heap_begin, *a), &when, sizeof when) &&
            when < 4102444800ull) {
            champion_known_ = true;
            champion_ = when != 0;
        }
        PubI(host, "sw.progress.champion_known", champion_known_ ? 1 : 0);
        PubI(host, "sw.progress.champion", champion_known_ && champion_ ? 1 : 0);
    }

    bool Caught(std::uint16_t species) const {
        if (!dex_blocks_ok_ || !dex_reverse_ready_ || species == 0 || species >= dex_reverse_.home_region.size())
            return false;
        const std::uint16_t idx = dex_reverse_.home_index[species];
        if (idx == 0) return false;
        switch (dex_reverse_.home_region[species]) {
            case sw_dex_index::Home::kGalar: return sw_zukan::DecodeEntry(dex_galar_.data(), dex_galar_.size(), idx - 1).caught;
            case sw_dex_index::Home::kArmor: return sw_zukan::DecodeEntry(dex_armor_.data(), dex_armor_.size(), idx - 1).caught;
            case sw_dex_index::Home::kCrown: return sw_zukan::DecodeEntry(dex_crown_.data(), dex_crown_.size(), idx - 1).caught;
            default: return false;
        }
    }

    // The place a list is about: its Town Map name in the companion language (the catalogue's,
    // which also names Route 2's lakeside and the Weald's second part like their _01 sibling).
    std::string RoutePlaceName(const EdenDsmodHostApi& host, std::uint64_t zone) {
        for (const auto& anchor : map_catalog_.anchors) if (anchor.zone_hash == zone) return anchor.name;
        EnsurePlaceNames(host);
        return PlaceNameOf(zone);
    }

    static std::string RouteLevelText(std::pair<int, int> lv) {
        return lv.first == lv.second ? sw_str::Format(sw_str::Id::kEvoLevel, lv.first)
                                     : sw_str::Format(sw_str::Id::kRouteLevels, lv.first, lv.second);
    }
    // "Wanders 30% · Grass (!) 25%": a card's first two methods; a third (45 of ~4,900 cards, e.g.
    // Wanders · Surfing · special) goes to the level line (`first` = 2), where there is room.
    static std::string RouteMethodText(const sw_route::Row& row, std::size_t first = 0, std::size_t last = 2) {
        static constexpr sw_str::Id kNames[] = {sw_str::Id::kRouteMethod0, sw_str::Id::kRouteMethod1,
            sw_str::Id::kRouteMethod2, sw_str::Id::kRouteMethod3, sw_str::Id::kRouteMethod4, sw_str::Id::kRouteMethod5};
        std::string out;
        const auto methods = sw_route::MethodsOf(row);
        for (std::size_t i = first; i < methods.size() && i < last; ++i) {
            const auto m = methods[i];
            if (!out.empty()) out += " \xC2\xB7 "; // " · "
            out += sw_str::Format(sw_str::Id::kRouteRate, sw_str::S(kNames[static_cast<int>(m)]),
                                  static_cast<int>(row.rate[static_cast<std::size_t>(m)]));
        }
        return out;
    }
    // The card's method glyph (first method): Surfing = Rotom Bike (its water mode), Berry tree =
    // a Berry, Fishing = the Pokédex distribution page's own fishing icon. "" = none.
    static const char* RouteMethodIcon(const sw_route::Row& row) {
        const auto methods = sw_route::MethodsOf(row);
        if (methods.empty()) return "";
        switch (methods.front()) {
            case sw_route::Shown::Surfing: return "romfs:/bin/appli/icon_item/item_1081.bntx#item_1081";
            case sw_route::Shown::Tree: return "romfs:/bin/appli/icon_item/item_0149.bntx#item_0149";
            case sw_route::Shown::Fishing:
                return "romfs:/bin/appli/pokedex/bin/pokedex_distribution.arc#timg/__Combined.bntx#weather_icon_s_10^t";
            default: return "";
        }
    }
    static std::string RouteWeatherIcon(int weather) {
        char src[128];
        std::snprintf(src, sizeof src,
                      "romfs:/bin/appli/pokedex/bin/pokedex_distribution.arc#timg/__Combined.bntx#weather_icon_s_%02d^t",
                      weather + 1);
        return src;
    }

    sw_route_encounters::Cache encounters_;

    void SampleRoute(const EdenDsmodHostApi& host) {
        encounters_.Ensure(host);
        const char* page_c = host.get_text ? host.get_text(host.userdata, "__page") : nullptr;
        const std::string_view page = page_c ? std::string_view{page_c} : std::string_view{};
        // Leaving the grid through the app rail forgets it (the Fly page's rule): the next Wild
        // Pokemon tap opens it again, and nothing sends the player back to it.
        if (!page.empty() && page != "map" && page != "route") route_open_ = false;
        PubI(host, "sw.route.list_open", route_open_ ? 1 : 0);
        PubI(host, "sw.route.return_map", page == "route" && !route_open_ ? 1 : 0);
        PubI(host, "sw.route.serial", route_serial_);

        // Town Map row: the selected place, else where the player stands (raw zone: an interior
        // has no table, so the row hides there, like a town without wild Pokemon).
        const auto* selected = map_ready() ? map_selection_.Get(map_catalog_) : nullptr;
        wild_target_ = selected ? selected->zone_hash : field_zone_;
        const auto* zone = sw_route::Find(encounters_.data, wild_target_);
        const bool here = wild_target_ == field_zone_;
        const int weather = zone ? sw_route::DefaultWeather(*zone, here ? live_weather_ : -1) : -1;
        const bool map_page = page.empty() || page == "map";
        std::uint64_t sig = HashValue(wild_target_, HashValue(static_cast<std::uint64_t>(weather + 1),
                            HashValue(lang_epoch_, HashValue(map_page, HashValue(selected && !here, 0)))));
        Taped(wild_tape_, sig, host, [&] {
            PubI(host, "sw.map.wild.ready", zone ? 1 : 0);
            if (!zone || !map_page) return;
            EnsureSpeciesNames(host);
            const auto rows = sw_route::Rows(encounters_.data, *zone, weather);
            const int count = static_cast<int>(rows.size());
            PubI(host, "sw.map.wild.count", count);
            // The row names the place only when it is not where the player stands (the card's
            // title already is the current place).
            const std::string place = selected && !here ? selected->name : std::string{};
            PubI(host, "sw.map.wild.place_ready", place.empty() ? 0 : 1);
            PubT(host, "sw.map.wild.place", place.c_str());
            char key[48], src[160];
            for (int i = 0; i < kWildStrip; ++i) {
                const bool ok = i < count && sw_route::IconSource(rows[static_cast<std::size_t>(i)].species,
                                                                              rows[static_cast<std::size_t>(i)].form, src, sizeof src);
                std::snprintf(key, sizeof key, "sw.map.wild.strip.%d.ready", i);
                PubI(host, key, ok ? 1 : 0);
                std::snprintf(key, sizeof key, "sw.map.wild.strip.%d.icon_src", i);
                PubT(host, key, ok ? src : "");
                std::snprintf(key, sizeof key, "sw.map.wild.strip.%d.icon_class", i);
                PubI(host, key, ok ? sw_route::IconClass(rows[static_cast<std::size_t>(i)].species) : 0);
                std::snprintf(key, sizeof key, "sw.map.wild.strip.%d", i);
                if (ok) PublishIconOffset(host, key, rows[static_cast<std::size_t>(i)].species);
            }
            PubI(host, "sw.map.wild.more_ready", count > kWildStrip ? 1 : 0);
            const std::string more = "+" + std::to_string(count > kWildStrip ? count - kWildStrip : 0);
            PubT(host, "sw.map.wild.more", more.c_str());
        });

        // Grid page (latched place + chosen weather).
        const auto* rz = route_open_ ? sw_route::Find(encounters_.data, route_zone_) : nullptr;
        const bool route_page = page.empty() || page == "route";
        if (rz && !sw_route::Possible(*rz, route_weather_)) route_weather_ = sw_route::DefaultWeather(*rz, -1);
        const bool route_here = rz && route_zone_ == field_zone_;
        const int now = route_here && sw_route::Possible(*rz, live_weather_) ? live_weather_ : -1;
        std::uint64_t rsig = HashValue(rz ? route_zone_ : 0, HashValue(static_cast<std::uint64_t>(route_weather_ + 1),
                             HashValue(static_cast<std::uint64_t>(now + 1), HashValue(lang_epoch_,
                             HashValue(route_page, HashValue(champion_known_ && champion_,
                             HashValue(dex_blocks_ok_ && dex_reverse_ready_ ? dex_blocks_sig_ : 0, 0)))))));
        Taped(route_tape_, rsig, host, [&] {
            PubI(host, "sw.route.ready", rz ? 1 : 0);
            if (!rz || !route_page) return;
            EnsureSpeciesNames(host);
            EnsureDexReverseIndex(host);
            PubT(host, "sw.route.name", RoutePlaceName(host, route_zone_).c_str());
            // Weather chips: only zones whose tables differ between the weathers that can happen.
            const bool varies = sw_route::Varies(encounters_.data, *rz);
            PubI(host, "sw.route.weather.shown", varies ? 1 : 0);
            char src[160];
            int k = 0;
            for (const int w : sw_route::kChipOrder) {
                if (!varies || !sw_route::Possible(*rz, w)) continue;
                const std::string p = "sw.route.weather." + std::to_string(k) + ".";
                PubI(host, (p + "ready").c_str(), 1);
                PubI(host, (p + "selected").c_str(), w == route_weather_ ? 1 : 0);
                PubI(host, (p + "now").c_str(), w == now ? 1 : 0);
                PubT(host, (p + "icon_src").c_str(), RouteWeatherIcon(w).c_str());
                ++k;
            }
            PubI(host, "sw.route.weather.count", k);
            static constexpr sw_str::Id kWeatherNames[] = {sw_str::Id::kRouteWeather0, sw_str::Id::kRouteWeather1,
                sw_str::Id::kRouteWeather2, sw_str::Id::kRouteWeather3, sw_str::Id::kRouteWeather4,
                sw_str::Id::kRouteWeather5, sw_str::Id::kRouteWeather6, sw_str::Id::kRouteWeather7,
                sw_str::Id::kRouteWeather8};
            PubT(host, "sw.route.weather.name", varies && route_weather_ >= 0 && route_weather_ < 9
                                                    ? sw_str::S(kWeatherNames[route_weather_]) : "");
            const auto rows = sw_route::Rows(encounters_.data, *rz, route_weather_);
            const int count = std::min(static_cast<int>(rows.size()), kRouteMaxRows);
            PubI(host, "sw.route.count", count);
            const bool champion = champion_known_ && champion_;
            for (int i = 0; i < count; ++i) {
                const auto& row = rows[static_cast<std::size_t>(i)];
                const std::string p = "sw.route." + std::to_string(i) + ".";
                const bool icon = sw_route::IconSource(row.species, row.form, src, sizeof src);
                PubI(host, (p + "icon_ready").c_str(), icon ? 1 : 0);
                // large art (class 2) draws smaller so it stays clear of the text column
                const bool big = sw_route::IconClass(row.species) == 2;
                PubI(host, (p + "icon_std").c_str(), icon && !big ? 1 : 0);
                PubI(host, (p + "icon_big").c_str(), icon && big ? 1 : 0);
                PubT(host, (p + "icon_src").c_str(), icon ? src : "");
                if (icon) PublishIconOffset(host, ("sw.route." + std::to_string(i)).c_str(), row.species);
                std::string name;
                if (!(species_names_ready_ && sw_species_names::SpeciesName(species_names_, row.species, name)))
                    name = "#" + std::to_string(row.species);
                PubT(host, (p + "name").c_str(), name.c_str());
                const std::string method = RouteMethodText(row);
                PubT(host, (p + "method").c_str(), method.c_str());
                PubT(host, (p + "method_after_icon").c_str(), method.c_str()); // the glyph variant's label
                PubI(host, (p + "method2_ready").c_str(), row.methods > 2 ? 1 : 0);
                PubT(host, (p + "method2").c_str(), RouteMethodText(row, 2, 3).c_str());
                const char* micon = RouteMethodIcon(row);
                PubI(host, (p + "micon_ready").c_str(), *micon ? 1 : 0);
                PubT(host, (p + "micon_src").c_str(), micon);
                PubT(host, (p + "level").c_str(), RouteLevelText(sw_route::Levels(*rz, row, champion)).c_str());
                PubI(host, (p + "caught").c_str(), Caught(row.species) ? 1 : 0);
            }
        });
    }

    // Caps the compact den strip to the manifest's fixed 5-marker row (gen_manifest.py's
    // page_map(), `for i in range(5): b += den_marker(...)`); the reader never publishes beyond
    // this, matching the existing BAG_MAX_ROWS/DEX_MAX_ROWS convention of a reader-side cap the
    // manifest's own static widget count must match.
    static constexpr int kMaxDenStripSlots = 5;

    // Max Raid dens (sw_addresses.h's "dens.vanilla"/"dens.isle_of_armor"/"dens.crown_tundra" --
    // SAVE-SNAPSHOT, see that file's domain header comment): shows only the active dens whose
    // zone (via sw_zone::DenSlotZoneId) matches THIS tick's current zone (zone_cache_, populated
    // by SampleLocation above -- Sample()'s call order guarantees it is fresh). Never publishes a
    // species (RaidSpawnDetail has no species field at all -- structurally spoiler-safe, not a
    // policy choice this function has to enforce). A region block that fails to read is skipped,
    // not treated as "abort the whole domain" -- the other two regions still get a chance.
    std::uint64_t dens_read_tick_ = 0;
    std::string dens_zone_seen_;
    void SampleDens(const EdenDsmodHostApi& host, u64 heap_begin) {
        const auto* a_galar = sw_addresses::Find("dens.vanilla");
        const auto* a_ioa = sw_addresses::Find("dens.isle_of_armor");
        const auto* a_ct = sw_addresses::Find("dens.crown_tundra");
        PublishDomainStatus(host, "dens", a_galar);
        const auto none = [&] {
            dens_tape_.Clear();
            PubI(host, "sw.dens.has_any", 0);
        };
        if (!a_galar || !a_galar->verified || !a_ioa || !a_ioa->verified || !a_ct || !a_ct->verified)
            return none();
        if (!zone_cache_ready_) return none(); // no current zone this tick -- strip hidden, not guessed
        const int zone_id = zone_id_cache_; // SampleLocation's lookup of this tick's zone_cache_
        if (zone_id < 0) return none(); // a town/unmapped zone legitimately has no dens -- not an error

        // Throttled like the dex: den records change on a day roll or a raid, not per frame.
        if (dens_tape_.valid && zone_cache_ == dens_zone_seen_ && tick_ - dens_read_tick_ < kSlowRefreshTicks) {
            dens_tape_.Replay(host);
            return;
        }
        struct RegionSpec {
            const sw_addresses::Anchor* anchor;
            sw_zone::DenRegion region;
            int slot_count;
        };
        const RegionSpec specs[3] = {
            {a_galar, sw_zone::DenRegion::kGalar, 100},
            {a_ioa, sw_zone::DenRegion::kIoa, 90},
            {a_ct, sw_zone::DenRegion::kCt, 86},
        };
        constexpr std::size_t kSlotSize = 0x18;
        constexpr int kCapacity = 111; // CountAll per region -- see sw_addresses.h's dens.* note
        std::array<std::array<std::uint8_t, kSlotSize * kCapacity>, 3> raws{};
        std::array<bool, 3> ok{};
        std::uint64_t sig = HashBytes(zone_cache_.data(), zone_cache_.size());
        for (int r = 0; r < 3; ++r) {
            // An anchor size drift or a failed read skips that region, never a guess.
            ok[static_cast<std::size_t>(r)] = raws[0].size() == specs[r].anchor->size &&
                ReadStable(host, sw_addresses::Resolve(heap_begin, *specs[r].anchor),
                           raws[static_cast<std::size_t>(r)].data(), raws[0].size());
            sig = HashValue(ok[static_cast<std::size_t>(r)] ? 1 : 0, sig);
            if (ok[static_cast<std::size_t>(r)])
                sig = HashBytes(raws[static_cast<std::size_t>(r)].data(), raws[0].size(), sig);
        }
        dens_read_tick_ = tick_;
        dens_zone_seen_ = zone_cache_;
        Taped(dens_tape_, sig, host, [&] {
            PubI(host, "sw.dens.has_any", 0);
            int published = 0;
            for (int r = 0; r < 3 && published < kMaxDenStripSlots; ++r) {
                if (!ok[static_cast<std::size_t>(r)]) continue;
                for (int slot = 0; slot < specs[r].slot_count && published < kMaxDenStripSlots; ++slot) {
                    if (sw_zone::DenSlotZoneId(specs[r].region, slot) != zone_id) continue;
                    const std::uint8_t* rec = raws[static_cast<std::size_t>(r)].data() +
                                              static_cast<std::size_t>(slot) * kSlotSize;
                    const std::uint8_t stars = rec[0x10];
                    const std::uint8_t den_type = rec[0x12];
                    if (den_type == 0 || den_type > 6 || stars > 4) continue; // inactive/garbage: skip
                    PublishDenSlot(host, published, stars, den_type);
                    ++published;
                }
            }
            if (published > 0) PubI(host, "sw.dens.has_any", 1);
        });
    }

    void PublishDenSlot(const EdenDsmodHostApi& host, int i, std::uint8_t stars, std::uint8_t den_type) {
        char prefix[24];
        std::snprintf(prefix, sizeof prefix, "sw.dens.%d", i);
        // Rare-tier (Rare/RareWish/DynamaxCrystal) gets the real native "dungeon" marker texture
        // (dir_a.py's own den_square(rare=True) convention); everything else gets the plain "wr"
        // marker. Both confirmed present in this build's townmap_top_00.arc (tools/bntx_decode.py
        // dump) -- see gen_manifest.py's build_sprite_map() for the matching sprite_map entries a
        // src_bind value here must resolve against.
        const bool rare_tier = (den_type == 2 || den_type == 4 || den_type == 6);
        const char* icon = rare_tier
            ? "romfs:/bin/appli/townmap/bin/townmap_top_00.arc#timg/__Combined.bntx#townmap_icon_dungeon_00^q"
            : "romfs:/bin/appli/townmap/bin/townmap_top_00.arc#timg/__Combined.bntx#townmap_icon_wr_00^q";

        char key[40];
        if (host.publish_i64) {
            std::snprintf(key, sizeof key, "%s.ready", prefix);
            PubI(host, key, 1);
            std::snprintf(key, sizeof key, "%s.stars", prefix);
            PubI(host, key, stars);
            std::snprintf(key, sizeof key, "%s.den_type", prefix);
            PubI(host, key, den_type);
        }
        if (host.publish_text) {
            std::snprintf(key, sizeof key, "%s.icon_src", prefix);
            PubT(host, key, icon);
            // RaidSpawnDetail's Stars byte is the 0-based rank (0..4 = one to five stars,
            // LIVE-READERS-132.md); the strip shows the rank the game shows.
            char stars_text[8];
            std::snprintf(stars_text, sizeof stars_text, "%u", static_cast<unsigned>(stars) + 1);
            std::snprintf(key, sizeof key, "%s.stars_text", prefix);
            PubT(host, key, stars_text);
        }
    }

    // CurrentScreen -> title/loading/field/battle/menu page-switch signal (sw_addresses.h's
    // "location.current_screen" anchor). Originally field<->battle only (VERIFIED 2026-10-25,
    // "BATTLE AUTO-SWITCH RELIABILITY FIX" -- see git history); EXTENDED 2026-10-25 (companion
    // worktree task "title + loading state detection") to also classify title and loading,
    // replacing the Town-Map-with-stale-live-data bug those two states had (no detection existed
    // at all -- the manifest's title/loading pages were built but never bound to anything).
    //
    // DESIGN REVISION 2 (same task, owner game-session review of the first CurrentScreen-only
    // design): that first design used ONLY `location.current_screen` for every edge, including
    // "have we left the title screen" -- and it was caught live, twice, publishing
    // `sw.state.field=1` WHILE STILL ON THE TITLE SCREEN, because the title screen's own animated
    // demo aliases this address with continuously drifting values (a dedicated characterisation
    // tool, this task, confirmed small-increment drift between 0.14s-apart reads) that can
    // apparently hold dead still for several real seconds at an unpredictable point -- long
    // enough to defeat any time-based debounce that still has to stay under a genuine load's
    // ~8-9s duration. The fix that made the title/loading bug SAFE (never trust an unnamed
    // reading as "arrived") then introduced a SECOND bug the owner flagged: a first-ever load
    // into a destination with no named CurrentScreen sentinel (an ordinary town, confirmed live
    // on one of this task's own boots) got stuck showing the loading spinner INDEFINITELY, since
    // nothing on this one noisy address could ever be trusted to mean "the load finished".
    //
    // The actual fix (this revision): stop using CurrentScreen ALONE for the one judgement it
    // cannot make reliably -- "has the title screen genuinely been left" -- and add the two
    // independent, non-aliased "save actually loaded" signals the owner specified, both already
    // computed earlier in Sample() by domains this module already ships:
    //   - `zone_cache_ready_`/`zone_cache_` (SampleLocation, `sw_addresses.h`'s
    //     "location.zone_name" anchor): true only once `sw_zone::DecodeZoneName` validates the
    //     8-byte tag AND decodes a non-empty UTF-16LE name right after it. VERIFIED LIVE this
    //     revision, 2 independent cold boots (`g0`, fixture `gb-635571`): `sw.location.zone_ready`
    //     reads ABSENT (decode fails, tag or name invalid) for the entire ~45s title-idle wait on
    //     BOTH boots, then becomes `1` with the real destination name (`"Wyndon"`) within ~2-3s of
    //     pressing Continue, well before the load visually finishes (~8-9s). This address is a
    //     completely different allocation from CurrentScreen and was never observed to exhibit
    //     any title-screen aliasing.
    //   - `AnyLivePartySlotValid()` (SampleLiveParty, `sw_addresses.h`'s "party.array_root"
    //     anchor): true once at least one live party slot's pointer resolves and its encrypted
    //     PK8 "stored" blob's own checksum validates (`sw_pk8::DecodeLiveStored`). VERIFIED LIVE
    //     this revision, 2 independent boots: **unlike the zone tag, this reads VALID (checksum-
    //     correct, `sw.party.0.ready=1`, live_count=5) at the title screen too** -- the title
    //     screen's own cosmetic stadium render decrypts the real party to pose it, so this signal
    //     by itself cannot distinguish title from field, exactly as the owner's review
    //     anticipated ("if the party array is populated at title from a previous session, fall
    //     back to zone tag + Continue edge"). It is still required (ANDed) for the FIELD
    //     confirmation below, as explicit extra insurance against a torn/partial read, but the
    //     TITLE/LOADING boundary itself is decided by the zone tag alone.
    //
    // Phase machine (replaces the single candidate+threshold loop the first design used -- the
    // rules differ enough per edge, and per source phase, that one shared loop no longer reads
    // clearly):
    //   kTitle   -> kLoading the instant `zone_cache_ready_` is true (save decode success is not
    //              itself debounced -- it already went through ReadStable's own torn-read
    //              double-check). CurrentScreen is NOT consulted for this edge at all any more --
    //              it cannot be, see above.
    //   (Battle signal: the guest helper's battle beat once the helper has spoken this boot, the
    //   CurrentScreen sentinel only without a helper -- see is_battle below.)
    //   kLoading -> kBattle instantly on the battle signal; -> kField once BOTH
    //              party and zone are valid AND EITHER the field sentinel (0xFFFF5127) is read,
    //              OR -- the owner's explicit "timeout fallback so loading can NEVER stick" --
    //              party+zone have been continuously valid, with no battle/overlay sentinel seen,
    //              for `kFieldStableTicks` (2s). This closes the exact "stuck on loading forever"
    //              bug: a town spawn has no named field sentinel, but DOES have a valid zone name,
    //              so the stability path alone, with no CurrentScreen cooperation required, is
    //              enough. The overlay bucket (0xFF000000/0xFFFFCF63/0xFF262626 -- the boot-to-
    //              world load screen AND, confirmed live, the X-menu/Town Map too) resets the
    //              stability counter rather than ending it, so the companion does not flash field
    //              a few ticks before the load visually finishes just because zone_valid landed
    //              early (confirmed live: ~2-3s into an ~8-9s load).
    //   kField   -> kLoading if the CONFIRMED zone name changes (a Fly/warp changed the live zone
    //              buffer in place -- `sw_zone.h`'s own header comment already proved this is a
    //              reused mutable buffer, not a fresh allocation), or if CurrentScreen reads
    //              something other than the field sentinel for more than `kReloadTriggerTicks`
    //              (300ms) AND party/zone no longer confirm the same place. A normal town's
    //              unnamed camera remains in field; it is not a menu/reload signal.
    //              -> kBattle on the battle signal;
    //              -> kMenu on the overlay bucket (checked AFTER the zone-change/reload checks,
    //              so a Fly opened from the Town Map is read as a reload, not swallowed as "just
    //              menu").
    //   kBattle  -> kField once the battle sentinel stops AND party+zone are (still) valid --
    //              no stability wait on the way BACK to an already-confirmed location, matching
    //              the original proven instant battle<->field toggle; falls back to kLoading only
    //              if party/zone somehow are not valid (re-runs the normal confirmation path).
    //   kMenu    -> the same zone-change reload check as kField, then -> kBattle, then -> kField
    //              (instant, same reasoning as kBattle above) or kLoading once the overlay bucket
    //              stops.
    // A failed CurrentScreen read (or `!a->verified`) leaves `phase_` untouched and republishes
    // it, unchanged from the original fix (`ModRuntime::DrivePageBinds` disarms on an absent
    // point, not a repeated one -- see this file's header comment).
    enum class Phase : int { kField = 0, kBattle = 1, kTitle = 2, kLoading = 3, kMenu = 4 };

    static constexpr int kFieldStableTicks = 120;     // 2 s @ 60 Hz -- owner's timeout fallback
    static constexpr int kReloadTriggerTicks = 18;    // 300 ms @ 60 Hz -- owner's reload trigger
    static constexpr int kBattleEndTicks = 30;        // 0.5 s after the helper's battle beat stopped
    // ... and only once the guest has run the field for a few frames since (field hook beats):
    // an emulation stall (shader build, a slow device's hitch) silences the battle beat too, and
    // must not flip the page to the field mid-battle (desktop 2026-11-07: battle -> map -> battle
    // ~1 s, every 30-60 s, on a slow GPU). A battle that ends where no field frame shows up within
    // kBattleStallTicks still ends (never stuck on the battle page).
    static constexpr std::uint64_t kBattleEndFieldFrames = 3;
    static constexpr int kBattleStallTicks = 300;     // 5 s of a silent battle beat

    bool AnyLivePartySlotValid() const {
        for (const auto& slot : live_party_cache_)
            if (slot.valid) return true;
        return false;
    }

    void SampleState(const EdenDsmodHostApi& host, u64 heap_begin) {
        const auto* a = sw_addresses::Find("location.current_screen");
        PublishDomainStatus(host, "state", a);
        if (!a || !a->verified) return;

        std::uint32_t screen = 0;
        const bool read_ok = ReadStable(host, sw_addresses::Resolve(heap_begin, *a), &screen, sizeof screen);

        if (read_ok) {
            // battle_hint_: the guest helper's "command menu waiting" beat (set by the module
            // before Sample) -- direct proof the battle client is selecting a command, used
            // because CurrentScreen was seen reading 0 (not the 0xFFFFFFFF sentinel) with the
            // command menu up on 2026-10-26.
            // With no Pokemon there is no battle: on a new game CurrentScreen read the battle
            // sentinel outdoors (owner Thor 2026-10-31: dimmed battle page outside at the start).
            // The sentinel is not battle-specific with a party either (Shield owner 2026-11-07,
            // early game after Hop's battle near Magnolia's house: the field kept reading
            // 0xFFFFFFFF, field -> battle on the sentinel, battle -> field 30 ticks after the
            // absent helper beat, repeating every ~0.52 s). Once the guest helper has spoken
            // this boot, the battle client's own beat is the ONLY battle entry/exit signal; the
            // sentinel decides only without a helper (legacy).
            const bool sentinel_battle = screen == 0xFFFFFFFFu && !live_party_empty_;
            const bool is_battle = helper_seen_ ? battle_hint_ : sentinel_battle;
            screen_sentinel_battle_ = sentinel_battle;
            const bool is_field_sentinel = (screen == 0xFFFF5127u);
            const bool is_overlay = (screen == 0xFF000000u || screen == 0xFFFFCF63u || screen == 0xFF262626u);
            // zone_cache_ready_/zone_cache_ (SampleLocation) and live_party_cache_
            // (SampleLiveParty) were both already populated earlier THIS SAME Sample() call --
            // Sample()'s own call order guarantees it, same pattern SampleDens already relies on.
            const bool zone_valid = zone_cache_ready_;
            const bool party_valid = AnyLivePartySlotValid() || live_party_empty_;

            switch (phase_) {
                case Phase::kTitle:
                    if (zone_valid) {
                        phase_ = Phase::kLoading;
                        boot_load_ = true; // the only load that shows the loading page
                        field_stable_ticks_ = 0;
                    }
                    break;
                case Phase::kLoading:
                    if (is_battle) {
                        EnterBattle();
                    } else if (party_valid && zone_valid && !confirmed_zone_name_.empty() &&
                               zone_cache_ == confirmed_zone_name_) {
                        // LIVE-CAUGHT FIX, REVISION 2 (this task, g0 verification -- see
                        // sw_addresses.h's anchor note): the SAME place as before this boot ever
                        // entered kLoading -- never a real reload (a real one changes the zone
                        // name, which kField/kMenu's OWN separate check already catches; this
                        // branch is only even reachable with the OLD name still reading valid).
                        // Resolve immediately, from the zone/party signals ALONE, regardless of
                        // what CurrentScreen is doing. Revision 1 of this fix required CurrentScreen
                        // to read the EXACT overlay-bucket constant first -- live-caught STILL
                        // stuck (a second verification boot): the X-menu does not just occasionally
                        // delay settling to that constant (the original finding), it can fail to
                        // settle to ANY named value at all for the whole time it stays open, the
                        // SAME kind of continuously-drifting aliasing this task already proved the
                        // TITLE screen has on this address (see the file's "DESIGN REVISION 2"
                        // comment) -- evidently not unique to the title screen after all. Since the
                        // zone/party signals are independently confirmed reliable at every menu
                        // screen tested (never observed to drift), trusting them ALONE here,
                        // without waiting on CurrentScreen to cooperate, is the robust fix.
                        phase_ = is_field_sentinel ? Phase::kField : Phase::kMenu;
                        field_stable_ticks_ = 0;
                    } else if (party_valid && zone_valid) {
                        if (is_field_sentinel) {
                            phase_ = Phase::kField;
                            confirmed_zone_name_ = zone_cache_;
                            field_stable_ticks_ = 0;
                        } else if (!is_overlay) {
                            if (++field_stable_ticks_ >= kFieldStableTicks) {
                                phase_ = Phase::kField;
                                confirmed_zone_name_ = zone_cache_;
                                field_stable_ticks_ = 0;
                            }
                        } else {
                            field_stable_ticks_ = 0; // still showing the load/overlay -- not yet
                        }
                    } else {
                        field_stable_ticks_ = 0;
                    }
                    break;
                case Phase::kField:
                    if (zone_valid && zone_cache_ != confirmed_zone_name_) {
                        // The live zone-name buffer changed in place (Fly/warp) -- sw_zone.h's own
                        // header comment already proved this is a reused mutable buffer, not a
                        // fresh allocation, so a changed name IS the destination changing, live.
                        phase_ = Phase::kLoading;
                        field_stable_ticks_ = 0;
                        reload_nonfield_ticks_ = 0;
                    } else if (is_battle) {
                        EnterBattle();
                    } else if (is_overlay) {
                        phase_ = Phase::kMenu;
                    } else if (!is_field_sentinel) {
                        if (zone_valid && party_valid && !confirmed_zone_name_.empty() &&
                            zone_cache_ == confirmed_zone_name_) {
                            // A normal town camera is screen=0. The previous classifier sent
                            // it to kMenu after 18 ticks, then back to kField after 120, repeating
                            // every 2.3 seconds and forcibly reopening Town Map through its bind.
                            // Once the independent party+zone signals confirm this same place,
                            // an unnamed screen value is no evidence that the field was left.
                            reload_nonfield_ticks_ = 0;
                        } else if (++reload_nonfield_ticks_ >= kReloadTriggerTicks) {
                            phase_ = Phase::kLoading;
                            field_stable_ticks_ = 0;
                            reload_nonfield_ticks_ = 0;
                        }
                    } else {
                        reload_nonfield_ticks_ = 0;
                    }
                    break;
                case Phase::kBattle:
                    // Phase 2 (2026-10-26): with the guest helper installed, the battle client's
                    // per-frame beat (battle_hint_) is the authority. CurrentScreen reads 0 during
                    // turn animations/messages and the overlay value inside the native party/bag
                    // screens -- neither may leave the battle page. The battle is over only once
                    // the beat has stopped (the client is gone); then the field sentinel exits at
                    // once, otherwise party+zone valid for kBattleEndTicks.
                    if (helper_seen_) {
                        if (battle_hint_) {
                            battle_end_ticks_ = 0;
                            battle_silent_ticks_ = 0;
                            battle_end_field_frames_ = field_frames_;
                            field_stable_ticks_ = 0;
                            break;
                        }
                        ++battle_silent_ticks_;
                        const bool field_ran = field_frames_ - battle_end_field_frames_ >= kBattleEndFieldFrames ||
                                               battle_silent_ticks_ >= kBattleStallTicks;
                        if (is_field_sentinel && field_ran) {
                            phase_ = Phase::kField;
                            if (zone_valid) confirmed_zone_name_ = zone_cache_;
                            field_stable_ticks_ = 0;
                        } else if (party_valid && zone_valid) {
                            if (++battle_end_ticks_ >= kBattleEndTicks && field_ran) {
                                phase_ = Phase::kField;
                                confirmed_zone_name_ = zone_cache_;
                                field_stable_ticks_ = 0;
                            }
                        } else {
                            battle_end_ticks_ = 0;
                        }
                        break;
                    }
                    // An EXPLICIT signal (the field sentinel, or the overlay bucket -- battle
                    // ending straight into a menu) still exits instantly, matching the original
                    // proven toggle. A merely UNRECOGNIZED reading does NOT, even once -- that is
                    // exactly the original battle<->field fix's own "an unclassified tick must
                    // republish the LAST real classification" guarantee
                    // (TestInBattleStickyAcrossUnclassifiedTick), which an instant exit on ANY
                    // non-0xFFFFFFFF reading would silently break (a single torn/transition-frame
                    // tick mid-battle is exactly what that fix exists to absorb). Only a
                    // SUSTAINED unrecognized reading (2s, reusing kFieldStableTicks -- the same
                    // reasoning as kLoading's own timeout fallback: a battle that genuinely ended
                    // into a town/unnamed camera has no sentinel to wait for either) is trusted,
                    // and only once party+zone are independently valid.
                    if (is_field_sentinel) {
                        phase_ = Phase::kField;
                        if (zone_valid) confirmed_zone_name_ = zone_cache_;
                        field_stable_ticks_ = 0;
                    } else if (is_overlay) {
                        phase_ = Phase::kMenu;
                        field_stable_ticks_ = 0;
                    } else if (is_battle) {
                        field_stable_ticks_ = 0;
                    } else if (party_valid && zone_valid) {
                        if (++field_stable_ticks_ >= kFieldStableTicks) {
                            phase_ = Phase::kField;
                            confirmed_zone_name_ = zone_cache_;
                            field_stable_ticks_ = 0;
                        }
                    } else {
                        field_stable_ticks_ = 0;
                    }
                    break;
                case Phase::kMenu:
                    if (zone_valid && zone_cache_ != confirmed_zone_name_) {
                        phase_ = Phase::kLoading; // e.g. a Fly confirmed from the Town Map
                        field_stable_ticks_ = 0;
                    } else if (is_battle) {
                        EnterBattle();
                    } else if (is_field_sentinel) {
                        phase_ = Phase::kField; // explicit, trustworthy -- instant, as always
                        field_stable_ticks_ = 0;
                    } else if (!is_overlay && party_valid && zone_valid) {
                        // Possibly closed back to an un-sentineled field (a town) -- require this
                        // SAME reading to hold for kFieldStableTicks before trusting it, not a
                        // single tick: the overlay screen's own CurrentScreen reading can drift
                        // through values that briefly look "not overlay" while still genuinely
                        // open (confirmed live, this task -- see kLoading's own identical
                        // reasoning, the "DESIGN REVISION 2"/"REVISION 2" comments). A single
                        // drifting tick must not be mistaken for the menu having closed.
                        if (++field_stable_ticks_ >= kFieldStableTicks) {
                            phase_ = Phase::kField;
                            field_stable_ticks_ = 0;
                        }
                    } else {
                        field_stable_ticks_ = 0;
                    }
                    break;
            }
            phase_known_ = true;
            const bool now_battle = phase_ == Phase::kBattle;
            if (now_battle != last_phase_battle_) ++phase_battle_edges_;
            last_phase_battle_ = now_battle;
        }
        // else: a failed read -- keep whatever `phase_` already holds and republish it below
        // (same sticky-republish rule as this function's original in_battle logic; see the file
        // header comment on why a tick must still republish rather than go silent).

        if (phase_ != Phase::kLoading) boot_load_ = false;
        if (phase_known_ && host.publish_i64) {
            PubI(host, "sw.state.ready", 1);
            // Field pages remain available through native menus. Binding page navigation to
            // this stable signal preserves the user's Bag/Pokedex selection when a menu
            // opens/closes, while genuine loading/battle returns still produce an edge.
            // Owner Thor 2026-10-31: a location change (door, route edge, Fly) keeps the field page
            // up; only the first load after the title shows the loading page (sw.state.boot_loading).
            const bool loc_load = phase_ == Phase::kLoading && !boot_load_;
            PubI(host, "sw.state.field_page",
                 phase_ == Phase::kField || phase_ == Phase::kMenu || loc_load ? 1 : 0);
            PubI(host, "sw.state.boot_loading", phase_ == Phase::kLoading && boot_load_ ? 1 : 0);
            PubI(host, "sw.state.title",   phase_ == Phase::kTitle   ? 1 : 0);
            PubI(host, "sw.state.loading", phase_ == Phase::kLoading ? 1 : 0);
            PubI(host, "sw.state.field",   phase_ == Phase::kField   ? 1 : 0);
            PubI(host, "sw.state.battle",  phase_ == Phase::kBattle  ? 1 : 0);
            PubI(host, "sw.state.menu",    phase_ == Phase::kMenu    ? 1 : 0);
            // Back-compat: SampleBattle (below) re-derives "in battle" itself from a fresh read
            // (see its own comment) and does not depend on this, but it is cheap to keep publishing
            // for any other consumer (tests, tools) already reading this key.
            PubI(host, "sw.state.in_battle", phase_ == Phase::kBattle ? 1 : 0);
            // Diagnostics (a Thor log / harness `value` read): the raw battle-sentinel reading,
            // the helper's battle beat, and the number of field<->battle edges this session.
            PubI(host, "sw.state.screen_battle", screen_sentinel_battle_ ? 1 : 0);
            PubI(host, "sw.state.helper_seen", helper_seen_ ? 1 : 0);
            PubI(host, "sw.state.battle_hint", battle_hint_ ? 1 : 0);
            PubI(host, "sw.state.battle_edges", phase_battle_edges_);
        }
        // Before the first successful classification this boot, nothing is published at all --
        // `sw.state.ready` (the page_binds "ready_bind") correctly stays unset, matching the
        // existing "not-before-game-loaded" behaviour this module already relies on elsewhere.
    }

    // Sticky classification for SampleState -- see that function's comment. Not reset per-tick
    // (this is the Reader's own persistent member state, outliving one Sample() call, unlike the
    // host's per-tick StateSnapshot). `phase_` starts at kTitle: that is the correct state before
    // any guest read has even succeeded, and gen_manifest.py puts the title page first in the
    // manifest's own page list so this default matches what `current_page` (mod_runtime.h) shows
    // before any page_bind has ever armed, with no edge needed.
    Phase phase_ = Phase::kTitle;
    bool phase_known_ = false;
    bool screen_sentinel_battle_ = false; // this tick's CurrentScreen read the battle sentinel
    bool last_phase_battle_ = false;
    std::int64_t phase_battle_edges_ = 0; // field<->battle phase edges this session (diagnostics)
    bool boot_load_ = false;          // kLoading entered from kTitle (not a location change)
    int field_stable_ticks_ = 0;      // kLoading's "party+zone stable" timeout counter
    int reload_nonfield_ticks_ = 0;   // kField's "CurrentScreen drifted off the sentinel" counter
    // The zone name AS OF the last time this boot committed to kField -- compared against the
    // live zone_cache_ to detect a Fly/warp (the name changing in place) from kField/kMenu.
    std::string confirmed_zone_name_;

    // Battle struct (sw_battle.h, sw_addresses.h's "battle.own_mon" anchor): own + foe mon HP/
    // level/PP, read only while SampleState classified this same tick as "in battle" (the
    // address holds stale prior-battle memory otherwise -- MODULE_GUIDE.md "never stale data").
    // Move identity and PP both come from the live battle struct (sw_battle.h's Move::move_id,
    // verified 2026-10-25) -- no party-slot matching.
    static constexpr u64 kBppStride = 0x7A0;      // bpp array stride (own 0..5, foe at 12)
    static constexpr u64 kPassBOffset = 0x18910;  // the client's bpp copy (cl+0x78 points here)

    // Battle bench from the live bpp array, in the client's battle-party order minus the active
    // battler (= the native party screen's order). Each cell also publishes its pokeID and
    // fainted flag (diagnostics/tests). Status dot from the live party (save order = pokeID order).
    // An own battle member's party Egg twin (pokeID -> live slot, species-matched like the
    // status dot): an Egg sits in the bpp array but is never a bench/switch/medicine target.
    // Shiny mark (owner 2026-11-01): an own battle member's species-matched live party slot.
    bool OwnMonShiny(int pid, std::uint16_t species) const {
        if (pid < 0 || pid > 5) return false;
        const auto& live = live_party_cache_[static_cast<std::size_t>(pid)];
        return live.valid && !live.egg && live.species == species && live.shiny;
    }

    // A wild foe's shiny mark: the game's wild-encounter PK8 (party format, encrypted) at
    // heap+0x8FEA3648 (SysBot.Pokemon WildPokemonOffset == CaptureSight Pk8Offset::Wild,
    // contracts/MEMORY-LEADS-132.md section 7). Fail-closed: only a checksum-valid, non-Egg PK8 of
    // the shown foe's species counts;
    // any other battle (raid; a trainer's mon of another species than the last wild one) reads "not
    // shiny". Cached per foe identity. Live 2026-11-01: the buffer holds the current wild foe.
    static constexpr u64 kWildPk8Offset = 0x8FEA3648;
    // Keyed on the battle epoch too: the next wild battle can reuse the client address and meet
    // the same species / level / max HP (review 3 C1), and must re-read its own PK8.
    struct WildShiny { std::uint64_t epoch = 0; u64 client = 0; std::uint16_t species = 0, max_hp = 0;
                       std::uint8_t level = 0; bool matched = false, shiny = false; };
    WildShiny wild_shiny_{};
    bool battle_wild_single_ = false;  // PublishBattleBall's rule check, this battle tick
    bool WildFoeShiny(const EdenDsmodHostApi& host, u64 heap_begin, const sw_battle::Mon& foe) {
        auto& c = wild_shiny_;
        if (c.matched && c.epoch == battle_epoch_ && c.client == battle_client_ && c.species == foe.species &&
            c.level == foe.level && c.max_hp == foe.max_hp) return c.shiny;
        c = {battle_epoch_, battle_client_, foe.species, foe.max_hp, foe.level, false, false};
        std::array<std::uint8_t, sw_pk8::kSizeParty> raw{};
        if (!heap_begin || !ReadStable(host, heap_begin + kWildPk8Offset, raw.data(), raw.size())) return false;
        const auto pk = sw_pk8::DecodeParty(raw.data());
        // The buffer's 0x10-byte party tail is not the live stat block (live 2026-11-01: a Lv.65
        // Boltund's PK8 read species 836 with a valid checksum but tail "level" 111), so the match
        // is the checksum-covered species alone.
        if (!pk.checksum_valid || pk.egg || pk.species != foe.species) return false;
        c.matched = true;
        c.shiny = sw_pk8::IsShiny(pk.pid, pk.tid, pk.sid);
        return c.shiny;
    }

    bool BattleMemberIsEgg(int pid, const sw_battle::Mon& m) const {
        if (pid < 0 || pid > 5) return false;
        const auto& live = live_party_cache_[static_cast<std::size_t>(pid)];
        return live.valid && live.egg && live.species == m.species;
    }

    // Own Pokemon's display name: its nickname (PK8 +0x58; equals the species name when not
    // nicknamed) from the species-matched live party slot, else the species name.
    bool OwnMonName(int pid, std::uint16_t species, std::string& out) const {
        out.clear();
        if (pid >= 0 && pid < 6) {
            const auto& live = live_party_cache_[static_cast<std::size_t>(pid)];
            const auto& d = party_detail_mons_[static_cast<std::size_t>(pid)];
            if (live.valid && live.egg && live.species == species) { out = sw_str::S(sw_str::Id::kEgg); return true; }
            if (live.valid && live.species == species && sw_font::CoversText(d.nickname)) { out = d.nickname; return true; }
        }
        return species_names_ready_ && sw_species_names::SpeciesName(species_names_, species, out);
    }

    void PublishBattleBench(const EdenDsmodHostApi& host, const std::array<sw_battle::Mon, 6>& members,
                            const std::array<int, 6>& order, int order_n, int bench_from = 1) {
        constexpr int kBench = 5;
        int n = 0;
        char key[48], pk[64];
        std::array<int, kBench> bench_pids{-1, -1, -1, -1, -1};
        std::uint64_t icon_sig = 0;
        for (int k = bench_from; k < order_n && n < kBench; ++k) {
            const int pid = order[static_cast<std::size_t>(k)];
            if (pid < 0 || pid > 5) continue;
            const auto& m = members[static_cast<std::size_t>(pid)];
            if (!m.valid || BattleMemberIsEgg(pid, m)) continue;
            icon_sig = HashValue(static_cast<std::uint64_t>(n) << 32 | m.species, icon_sig);
            bench_pids[static_cast<std::size_t>(n)] = pid;
            ++n;
        }
        // Which slot holds which species rarely changes tick-to-tick (only on switch/faint); the
        // icon lookup is cached on that identity alone (review OPTIMIZE #8). HP/status below stay
        // live every call -- a bench member's HP/fainted state must never lag behind the raw read.
        Taped(bench_icon_tape_, icon_sig, host, [&] {
            for (int i = 0; i < n; ++i) {
                std::snprintf(key, sizeof key, "sw.battle.bench.%d", i);
                const int bpid = bench_pids[static_cast<std::size_t>(i)];
                const auto species = members[static_cast<std::size_t>(bpid)].species;
                PublishIcon(host, key, species);
                // Field tab party list (owner dev.23 4e): name (nickname), native type labels.
                std::string name;
                if (host.publish_text && OwnMonName(bpid, species, name)) {
                    std::snprintf(pk, sizeof pk, "%s.name", key);
                    PubT(host, pk, name.c_str());
                }
                std::uint8_t t1 = 0, t2 = 0;
                const bool types_ok = CachedTypes(host, species, t1, t2);
                char src[160]{};
                std::snprintf(pk, sizeof pk, "%s.types_ready", key);
                if (host.publish_i64) PubI(host, pk, types_ok ? 1 : 0);
                std::snprintf(pk, sizeof pk, "%s.type2_show", key);
                if (host.publish_i64) PubI(host, pk, types_ok && t1 != t2 ? 1 : 0);
                if (types_ok && host.publish_text && sw_species_types::TypeImageSource(t1, true, src, sizeof src)) {
                    std::snprintf(pk, sizeof pk, "%s.type1_label_src", key);
                    PubT(host, pk, src);
                }
                if (types_ok && t1 != t2 && host.publish_text && sw_species_types::TypeImageSource(t2, true, src, sizeof src)) {
                    std::snprintf(pk, sizeof pk, "%s.type2_label_src", key);
                    PubT(host, pk, src);
                }
            }
        });
        for (int i = 0; i < n; ++i) {
            const int pid = bench_pids[static_cast<std::size_t>(i)];
            const auto& m = members[static_cast<std::size_t>(pid)];
            std::snprintf(key, sizeof key, "sw.battle.bench.%d", i);
            const int pct = m.max_hp > 0 ? std::clamp(static_cast<int>(m.cur_hp) * 100 / m.max_hp, 0, 100) : 0;
            if (host.publish_i64) {
                std::snprintf(pk, sizeof pk, "%s.hp_pct", key);
                PubI(host, pk, pct);
                std::snprintf(pk, sizeof pk, "%s.pid", key);
                PubI(host, pk, pid);
                std::snprintf(pk, sizeof pk, "%s.fainted", key);
                PubI(host, pk, m.cur_hp == 0 ? 1 : 0);
                std::snprintf(pk, sizeof pk, "%s.cur_hp", key);
                PubI(host, pk, m.cur_hp);
                std::snprintf(pk, sizeof pk, "%s.max_hp", key);
                PubI(host, pk, m.max_hp);
                for (const auto [suffix, on] : {std::pair{"hpg", pct > 50}, std::pair{"hpy", pct > 20 && pct <= 50},
                                                std::pair{"hpr", pct <= 20}}) {
                    std::snprintf(pk, sizeof pk, "%s.%s", key, suffix);
                    PubI(host, pk, on ? 1 : 0);
                }
            }
            if (host.publish_text) {
                char hp_text[16];
                std::snprintf(hp_text, sizeof hp_text, "%u/%u", static_cast<unsigned>(m.cur_hp), static_cast<unsigned>(m.max_hp));
                std::snprintf(pk, sizeof pk, "%s.hp_text", key);
                PubT(host, pk, hp_text);
            }
            if (host.publish_text) {
                char pct_text[8];
                std::snprintf(pct_text, sizeof pct_text, "%d%%", pct);
                std::snprintf(pk, sizeof pk, "%s.hp_pct_text", key);
                PubT(host, pk, pct_text);
            }
            const auto& live = live_party_cache_[static_cast<std::size_t>(pid)];
            const std::uint32_t status = (live.valid && live.species == m.species) ? live.status : 0;
            PublishStatusIcon(host, key, status, m.cur_hp, m.max_hp);
            std::snprintf(pk, sizeof pk, "%s.shiny", key);
            PubI(host, pk, OwnMonShiny(pid, m.species) ? 1 : 0);
        }
        if (!host.publish_i64) return;
        PubI(host, "sw.battle.bench.count", n);
        for (int k = 0; k < kBench; ++k) {
            std::snprintf(key, sizeof key, "sw.battle.bench.%d.ready", k);
            PubI(host, key, k < n ? 1 : 0);
        }
    }

    // Matchup-card tabs (Foe / Own / Field).
    void PublishBattleTabs(const EdenDsmodHostApi& host) {
        if (!host.publish_i64) return;
        char key[48];
        for (int t = 0; t < 3; ++t) {
            std::snprintf(key, sizeof key, "sw.battle.tab.%d", t);
            PubI(host, key, battle_tab_ == t ? 1 : 0);
        }
        PubI(host, "sw.battle.tab", battle_tab_);
    }

    std::vector<std::string> field_strings_;
    bool field_strings_attempted_ = false;
    struct FieldRow {
        std::string name;
        std::string description;
        sw_battle_field::Timer timer;
    };
    std::vector<FieldRow> field_rows_;
    static constexpr int kMaxFieldRows = 48;
    void AppendFieldRow(int state, const sw_battle_field::Timer& timer, const std::string& prefix = {}) {
        std::string name, description;
        if (!sw_battle_field::StateStrings(field_strings_, state, name, description)) return;
        field_rows_.push_back({prefix + name, std::move(description), timer});
    }
    void PublishFieldRows(const EdenDsmodHostApi& host) {
        const int n = std::min(static_cast<int>(field_rows_.size()), kMaxFieldRows);
        PubI(host, "sw.battle.field.row_count", n);
        PubI(host, "sw.battle.field.extra_count", static_cast<int>(field_rows_.size()) - n);
        char row[40], key[64];
        for (int i = 0; i < n; ++i) {
            std::snprintf(row, sizeof row, "sw.battle.field.row.%d", i);
            const auto& data = field_rows_[static_cast<std::size_t>(i)];
            std::snprintf(key, sizeof key, "%s.ready", row); PubI(host, key, 1);
            std::snprintf(key, sizeof key, "%s.name", row); PubT(host, key, data.name.c_str());
            std::snprintf(key, sizeof key, "%s.description", row); PubT(host, key, data.description.c_str());
            PublishFieldTimer(host, row, data.timer);
        }
    }
    void PublishFieldTimer(const EdenDsmodHostApi& host, const char* prefix,
                           const sw_battle_field::Timer& timer) {
        char key[72];
        std::snprintf(key, sizeof key, "%s.turns_ready", prefix); PubI(host, key, timer.known ? 1 : 0);
        if (!timer.known) return;
        std::snprintf(key, sizeof key, "%s.indefinite", prefix); PubI(host, key, timer.indefinite ? 1 : 0);
        std::snprintf(key, sizeof key, "%s.turns", prefix); PubI(host, key, timer.remaining);
        std::snprintf(key, sizeof key, "%s.turns_text", prefix);
        if (timer.indefinite) PubT(host, key, sw_str::S(sw_str::Id::kFieldOngoing));
        else {
            PubT(host, key, sw_str::Format(timer.remaining == 1 ? sw_str::Id::kFieldTurn : sw_str::Id::kFieldTurns,
                                           timer.remaining).c_str());
        }
    }
    void PublishFieldState(const EdenDsmodHostApi& host, const char* prefix,
                           bool active, int state, const sw_battle_field::Timer& timer) {
        char key[72];
        std::snprintf(key, sizeof key, "%s.active", prefix); PubI(host, key, active ? 1 : 0);
        std::string name, description;
        const bool ready = active && sw_battle_field::StateStrings(field_strings_, state, name, description);
        std::snprintf(key, sizeof key, "%s.ready", prefix); PubI(host, key, ready ? 1 : 0);
        if (!ready) return;
        std::snprintf(key, sizeof key, "%s.name", prefix); PubT(host, key, name.c_str());
        std::snprintf(key, sizeof key, "%s.description", prefix); PubT(host, key, description.c_str());
        PublishFieldTimer(host, prefix, timer);
    }
    void SampleBattleFieldOwnSide(const EdenDsmodHostApi& host) {
        field_own_side_ = -1;
        if (sw_battle_field::kSideIdentityVerified) {
            std::uint64_t main = 0, setup = 0;
            std::uint32_t rule = 0;
            std::uint8_t participant = 0, mode = 0;
            if (ReadStable(host, battle_client_ + 0x58, &main, sizeof main) && main &&
                ReadStable(host, main + 0x150, &rule, sizeof rule) &&
                ReadStable(host, battle_client_ + 0x2A6, &participant, sizeof participant)) {
                const bool mode_ok = rule == 0 ||
                    (ReadStable(host, main + 0x88, &setup, sizeof setup) && setup &&
                     ReadStable(host, setup + 0x3B9, &mode, sizeof mode));
                if (mode_ok) field_own_side_ = sw_battle_field::SideForParticipant(rule, mode, participant);
            }
        }
        PubI(host, "sw.battle.field.own_side_ready", field_own_side_ >= 0 ? 1 : 0);
        PubI(host, "sw.battle.field.own_side", field_own_side_);
    }
    void SampleBattleFieldSides(const EdenDsmodHostApi& host) {
        // Pass container+0x48 owns both side-condition tables; each 0x20-byte table slot has
        // a condition-object pointer at +8 (native getter 0x835320), not inline condition data.
        std::uint64_t pass = 0, manager = 0, recheck = 0;
        if (!ReadStable(host, battle_client_ + 0x70, &pass, sizeof pass) || !pass ||
            !ReadStable(host, pass + 0x48, &manager, sizeof manager) || !manager) return;
        std::array<std::uint8_t, sw_battle_field::kSideTableBytes> slots{}, slots_again{};
        if (!ReadStable(host, manager, slots.data(), slots.size())) return;
        std::array<std::array<sw_battle_field::SideEffect, sw_battle_field::kSideEffects.size()>, 2> sides{};
        std::array<bool, 2> valid{true, true};
        for (int side = 0; side < 2; ++side) {
            for (std::size_t i = 0; i < sw_battle_field::kSideEffects.size(); ++i) {
                const auto& spec = sw_battle_field::kSideEffects[i];
                const auto pointer = sw_battle_field::U64(slots.data() + side * 0x300 + spec.key * 0x20 + 8);
                if (!pointer) continue;
                std::array<std::uint8_t, 0x20> raw{};
                if (!ReadStable(host, pointer, raw.data(), raw.size())) {
                    valid[static_cast<std::size_t>(side)] = false;
                    break;
                }
                sides[static_cast<std::size_t>(side)][i] =
                    sw_battle_field::DecodeSideEffect(raw.data(), raw.size(), spec.mode);
            }
        }
        // Never combine condition records across a pass/manager/table replacement.
        if (!ReadStable(host, manager, slots_again.data(), slots_again.size()) || slots != slots_again ||
            !ReadStable(host, pass + 0x48, &recheck, sizeof recheck) || recheck != manager ||
            !ReadStable(host, battle_client_ + 0x70, &recheck, sizeof recheck) || recheck != pass) return;
        for (int side = 0; side < 2; ++side) {
            char prefix[40];
            std::snprintf(prefix, sizeof prefix, "sw.battle.field.side.%d", side);
            char key[72];
            std::snprintf(key, sizeof key, "%s.ready", prefix);
            PubI(host, key, valid[static_cast<std::size_t>(side)] ? 1 : 0);
            if (!valid[static_cast<std::size_t>(side)]) continue;
            // field_rows_ (the flat "all active conditions" list PublishFieldRows reads later
            // this same tick) must stay live every tick, so it is rebuilt unconditionally here;
            // only the heavier per-effect detail publish below (name/description/turns text) is
            // cached on a signature of this side's decoded effects (field values, not raw struct
            // bytes -- SideEffect/Timer have bool+uint32_t padding that is not reliably
            // deterministic to hash).
            int n = 0;
            std::uint64_t sig = static_cast<std::uint64_t>(side);
            for (std::size_t i = 0; i < sw_battle_field::kSideEffects.size(); ++i) {
                const auto& effect = sides[static_cast<std::size_t>(side)][i];
                const auto& spec = sw_battle_field::kSideEffects[i];
                sig = HashValue(effect.active ? 1u : 0u, sig);
                sig = HashValue(effect.layers, sig);
                sig = HashValue(effect.timer.known ? 1u : 0u, sig);
                sig = HashValue(effect.timer.indefinite ? 1u : 0u, sig);
                sig = HashValue(effect.timer.remaining, sig);
                if (!effect.active) continue;
                if (field_own_side_ >= 0) {
                    const std::string side_name = sw_str::S(side == field_own_side_ ? sw_str::Id::kFieldYourSide : sw_str::Id::kFieldOpposingSide);
                    const auto previous_rows = field_rows_.size();
                    AppendFieldRow(spec.text_state, effect.timer, side_name);
                    if (spec.mode == 3 && field_rows_.size() > previous_rows)
                        field_rows_.back().name += " (" + std::to_string(effect.layers) +
                                                  (effect.layers == 1 ? " layer)" : " layers)");
                }
                ++n;
            }
            Taped(field_side_tape_[static_cast<std::size_t>(side)], sig, host, [&] {
                int m = 0;
                for (std::size_t i = 0; i < sw_battle_field::kSideEffects.size(); ++i) {
                    const auto& effect = sides[static_cast<std::size_t>(side)][i];
                    const auto& spec = sw_battle_field::kSideEffects[i];
                    if (!effect.active) continue;
                    char row[56];
                    std::snprintf(row, sizeof row, "%s.effect.%d", prefix, m);
                    PublishFieldState(host, row, true, spec.text_state, effect.timer);
                    char rk[72];
                    std::snprintf(rk, sizeof rk, "%s.layers_ready", row);
                    PubI(host, rk, spec.mode == 3 ? 1 : 0);
                    if (spec.mode == 3) {
                        std::snprintf(rk, sizeof rk, "%s.layers", row);
                        PubI(host, rk, effect.layers);
                        char text[24];
                        std::snprintf(text, sizeof text, "%u %s", effect.layers,
                                      effect.layers == 1 ? "layer" : "layers");
                        std::snprintf(rk, sizeof rk, "%s.layers_text", row);
                        PubT(host, rk, text);
                    }
                    ++m;
                }
            });
            std::snprintf(key, sizeof key, "%s.effect_count", prefix);
            PubI(host, key, n);
        }
    }
    void SampleBattleField(const EdenDsmodHostApi& host, bool in_battle) {
        field_rows_.clear();
        PubI(host, "sw.battle.field.row_count", 0);
        PubI(host, "sw.battle.field.extra_count", 0);
        PubI(host, "sw.battle.field.ready", 0);
        PubI(host, "sw.battle.field.own_side_ready", 0);
        PubI(host, "sw.battle.field.own_side", -1);
        PubI(host, "sw.battle.field.weather.ready", 0);
        PubI(host, "sw.battle.field.terrain.ready", 0);
        PubI(host, "sw.battle.field.effect_count", 0);
        for (int side = 0; side < 2; ++side) {
            const std::string prefix = "sw.battle.field.side." + std::to_string(side);
            PubI(host, (prefix + ".ready").c_str(), 0);
            PubI(host, (prefix + ".effect_count").c_str(), 0);
        }
        if (!sw_battle_field::kRuntimeVerified || !in_battle || !battle_client_) return;
        std::uint64_t field = 0, recheck = 0;
        std::array<std::uint8_t, sw_battle_field::kBytes> raw{};
        if (!ReadStable(host, battle_client_ + sw_battle_field::kClientFieldOffset, &field, sizeof field) ||
            !field || !ReadStable(host, field, raw.data(), raw.size()) ||
            !ReadStable(host, battle_client_ + sw_battle_field::kClientFieldOffset, &recheck, sizeof recheck) ||
            recheck != field) return;
        const auto data = sw_battle_field::Decode(raw.data(), raw.size());
        if (!data.valid) return;
        if (!field_strings_attempted_) {
            field_strings_attempted_ = true;
            sw_battle_field::LoadStrings(host, field_strings_);
        }
        PubI(host, "sw.battle.field.ready", 1);
        SampleBattleFieldOwnSide(host);
        PubI(host, "sw.battle.field.weather.id", data.weather);
        PubI(host, "sw.battle.field.terrain.id", data.terrain);
        PublishFieldState(host, "sw.battle.field.weather", data.weather != 0,
                          sw_battle_field::WeatherTextState(data.weather), data.weather_timer);
        PublishFieldState(host, "sw.battle.field.terrain", data.terrain != 0,
                          sw_battle_field::TerrainTextState(data.terrain), data.terrain_timer);
        if (data.weather == 0) field_rows_.push_back({sw_str::S(sw_str::Id::kFieldNoWeather), {}, {}});
        else AppendFieldRow(sw_battle_field::WeatherTextState(data.weather), data.weather_timer);
        if (data.terrain == 0) field_rows_.push_back({sw_str::S(sw_str::Id::kFieldNoTerrain), {}, {}});
        else AppendFieldRow(sw_battle_field::TerrainTextState(data.terrain), data.terrain_timer);
        int n = 0;
        for (std::size_t i = 0; i < sw_battle_field::kEffects.size(); ++i) {
            if (!data.effects[i]) continue;
            char key[48];
            std::snprintf(key, sizeof key, "sw.battle.field.effect.%d", n);
            PublishFieldState(host, key, true, sw_battle_field::kEffects[i].text_state, data.effect_timers[i]);
            AppendFieldRow(sw_battle_field::kEffects[i].text_state, data.effect_timers[i]);
            ++n;
        }
        PubI(host, "sw.battle.field.effect_count", n);
        SampleBattleFieldSides(host);
        PublishFieldRows(host);
    }

    struct DmaxPreview {
        bool wanted = false;
        std::uint64_t client = 0, actor = 0;
        std::array<std::uint32_t, 4> max_ids{}, powers{}, base_ids{};
        std::array<std::int32_t, 4> gmax_indices{-1, -1, -1, -1};
    } dmax_preview_;
    bool battle_raid_ready_ = false, battle_dmax_active_ = false;
    std::uint64_t battle_own_actor_ = 0;
    // Foe tab of the command page's left pane (owner Thor 2026-10-31: "exactly like Lumi"): the
    // foe's own moves from its live battle struct -- Lumi's Foe page lists every move the battle
    // data holds (type, name, how it hits the Pokemon on the Own side, PP), no reveal gate.
    void PublishFoeMoves(const EdenDsmodHostApi& host, const sw_battle::Mon& foe, bool foe_ok,
                         bool own_types_ready, sw_species_types::u8 own_type1, sw_species_types::u8 own_type2) {
        for (int i = 0; i < sw_battle::kMoveSlots; ++i) {
            const auto& mv = foe.moves[i];
            const std::string q = "sw.battle.foe.move" + std::to_string(i + 1) + ".";
            const bool ready = foe_ok && mv.move_id != 0 && mv.pp_max > 0;
            PubI(host, (q + "ready").c_str(), ready ? 1 : 0);
            if (!ready) {
                for (const auto* suffix : {"name_ready", "show_fallback", "type_ready", "effectiveness_ready"})
                    PubI(host, (q + suffix).c_str(), 0);
                continue;
            }
            PubI(host, (q + "id").c_str(), mv.move_id);
            PubT(host, (q + "pp_text").c_str(), (std::to_string(mv.pp_current) + "/" + std::to_string(mv.pp_max)).c_str());
            std::string name;
            const bool name_ready = move_names_ready_ && sw_move_names::MoveName(move_names_, mv.move_id, name);
            if (name_ready) PubT(host, (q + "name").c_str(), name.c_str());
            PubI(host, (q + "name_ready").c_str(), name_ready ? 1 : 0);
            PubI(host, (q + "show_fallback").c_str(), name_ready ? 0 : 1);
            sw_move_data::MoveRecord rec;
            const bool data_ok = CachedMove(host, mv.move_id, rec);
            char src[160];
            const bool type_ok = data_ok && sw_species_types::TypeImageSource(rec.type, false, src, sizeof src);
            PubI(host, (q + "type_ready").c_str(), type_ok ? 1 : 0);
            if (type_ok) {
                PubT(host, (q + "type_src").c_str(), src);
                PubI(host, (q + "type_id").c_str(), rec.type);
            }
            bool eff_ok = false;
            if (data_ok && rec.category != 0 && own_types_ready) {
                const std::uint32_t percent = sw_type_chart::EffectivenessPercentDual(
                    sw_species_types::ToChartType(rec.type), sw_species_types::ToChartType(own_type1),
                    sw_species_types::ToChartType(own_type2));
                std::string word;
                eff_ok = effectiveness_text_ready_ &&
                         sw_effectiveness_text::Label(effectiveness_text_, sw_effectiveness_text::TierForPercent(percent), word);
                if (eff_ok)
                    PubT(host, (q + "effectiveness_text").c_str(),
                         (word + " " + sw_effectiveness_text::MultiplierText(percent)).c_str());
            }
            PubI(host, (q + "effectiveness_ready").c_str(), eff_ok ? 1 : 0);
        }
    }

    void ClearBattleMoveGates(const EdenDsmodHostApi& host, int slot) {
        const auto prefix = "sw.battle.own.move" + std::to_string(slot) + ".";
        for (const auto* suffix : {"ready", "name_ready", "show_fallback", "type_ready",
                                   "effectiveness_ready", "preview_ready", "power_ready", "description_ready",
                                   "catpow_ready"})
            PubI(host, (prefix + suffix).c_str(), 0);
    }
    bool gmax_strings_attempted_ = false;
    std::vector<std::string> gmax_names_, gmax_descriptions_;
    static bool LoadGmaxStrings(const EdenDsmodHostApi& host, const char* path,
                                 std::vector<std::string>& out) {
        if (!host.read_romfs) return false;
        const auto size = host.read_romfs(host.userdata, path, 0, nullptr, 0);
        if (!size || size > 256 * 1024) return false;
        std::vector<std::uint8_t> bytes(size);
        std::vector<std::string> strings;
        if (host.read_romfs(host.userdata, path, 0, bytes.data(), bytes.size()) != size ||
            !sw_text::ReadDat(bytes.data(), bytes.size(), strings) || strings.size() != 33) return false;
        out = std::move(strings);
        return true;
    }
    void EnsureGmaxStrings(const EdenDsmodHostApi& host) {
        if (gmax_strings_attempted_) return;
        gmax_strings_attempted_ = true;
        // Native865A90 supplies message indices0..32, not ordinary move IDs. Both native
        // tables contain33 entries; index0 is already G-Max Wildfire, with no placeholder.
        LoadGmaxStrings(host, sw_lang::MessagePath("gwazaname.dat").c_str(), gmax_names_);
        LoadGmaxStrings(host, sw_lang::MessagePath("gwazainfo.dat").c_str(), gmax_descriptions_);
    }

    // `tape_index` selects this prefix's own slot in raid_mon_tape_ (own=0, boss=1, ally=2..4) --
    // each prefix must keep a stable slot so an unrelated prefix's signature never collides with
    // this one's cache. Identity (species/level/name/icon) is cached on a species+level signature
    // that excludes HP/PP; cur_hp/max_hp/hp_pct/fainted are recomputed and published every call.
    void PublishRaidMon(const EdenDsmodHostApi& host, const char* prefix, int tape_index,
                        const sw_battle_raid::Member& member) {
        char key[80];
        const auto& mon = member.mon;
        const int pct = mon.max_hp > 0 ? static_cast<int>(mon.cur_hp) * 100 / mon.max_hp : 0;
        std::snprintf(key, sizeof key, "%s.ready", prefix); PubI(host, key, 1);
        std::snprintf(key, sizeof key, "%s.cur_hp", prefix); PubI(host, key, mon.cur_hp);
        std::snprintf(key, sizeof key, "%s.current_hp", prefix); PubI(host, key, mon.cur_hp);
        std::snprintf(key, sizeof key, "%s.max_hp", prefix); PubI(host, key, mon.max_hp);
        std::snprintf(key, sizeof key, "%s.hp_pct", prefix); PubI(host, key, pct);
        std::snprintf(key, sizeof key, "%s.fainted", prefix); PubI(host, key, mon.cur_hp == 0 ? 1 : 0);
        char text[24];
        std::snprintf(text, sizeof text, "%u/%u", mon.cur_hp, mon.max_hp);
        std::snprintf(key, sizeof key, "%s.hp_text", prefix); PubT(host, key, text);
        std::snprintf(text, sizeof text, "%d%%", pct);
        std::snprintf(key, sizeof key, "%s.hp_pct_text", prefix); PubT(host, key, text);

        const auto sig = HashValue(static_cast<std::uint64_t>(mon.level) << 32 | mon.species, 0);
        Taped(raid_mon_tape_[static_cast<std::size_t>(tape_index)], sig, host, [&] {
            std::snprintf(key, sizeof key, "%s.species", prefix); PubI(host, key, mon.species);
            std::snprintf(key, sizeof key, "%s.level", prefix); PubI(host, key, mon.level);
            std::string name;
            const bool named = species_names_ready_ &&
                               sw_species_names::SpeciesName(species_names_, mon.species, name);
            std::snprintf(key, sizeof key, "%s.name_ready", prefix); PubI(host, key, named ? 1 : 0);
            if (named) { std::snprintf(key, sizeof key, "%s.name", prefix); PubT(host, key, name.c_str()); }
            PublishIcon(host, prefix, mon.species);
        });
    }

    sw_battle_raid::Snapshot SampleBattleRaid(const EdenDsmodHostApi& host, bool in_battle) {
        sw_battle_raid::Snapshot raid;
        if (sw_battle_raid::kRuntimeVerified && in_battle && battle_client_)
            raid = sw_battle_raid::Read(battle_client_, [&](u64 address, void* data, std::size_t bytes) {
                return ReadStable(host, address, data, bytes);
            });
        battle_raid_ready_ = raid.ready;
        battle_dmax_active_ = raid.ready && sw_battle_raid::kDmaxVerified && raid.participants[0].dmax;
        PubI(host, "sw.battle.raid.known", raid.kind_known ? 1 : 0);
        PubI(host, "sw.battle.raid.active", raid.active ? 1 : 0);
        PubI(host, "sw.battle.raid.ready", raid.ready ? 1 : 0);
        PubI(host, "sw.battle.raid.boss.ready", raid.ready ? 1 : 0);
        PubI(host, "sw.battle.raid.own.ready", raid.ready ? 1 : 0);
        PubI(host, "sw.battle.raid.ally_count", raid.ready ? 3 : 0);
        for (int i = 0; i < sw_battle_raid::kAllies; ++i)
            PubI(host, ("sw.battle.raid.ally." + std::to_string(i) + ".ready").c_str(), raid.ready ? 1 : 0);
        const bool shield_ready = raid.ready && sw_battle_raid::kShieldVerified && raid.shield.valid;
        PubI(host, "sw.battle.raid.shield.ready", shield_ready ? 1 : 0);
        if (shield_ready) {
            PubI(host, "sw.battle.raid.shield.active", raid.shield.active ? 1 : 0);
            // Inactive configuration can contain the next barrier's size. Do not reveal it.
            if (raid.shield.active) {
                PubI(host, "sw.battle.raid.shield.remaining", raid.shield.remaining);
                PubI(host, "sw.battle.raid.shield.total", raid.shield.total);
                const auto text = std::to_string(raid.shield.remaining) + "/" + std::to_string(raid.shield.total);
                PubT(host, "sw.battle.raid.shield.text", text.c_str());
            }
        }
        const bool dmax_ready = raid.ready && sw_battle_raid::kDmaxVerified;
        PubI(host, "sw.battle.raid.dmax.ready", dmax_ready ? 1 : 0);
        PubI(host, "sw.battle.raid.dmax.turns_ready", dmax_ready && raid.participants[0].dmax ? 1 : 0);
        if (!raid.ready) return raid;
        EnsureSpeciesNames(host);
        PublishRaidMon(host, "sw.battle.raid.own", 0, raid.participants[0]);
        PublishRaidMon(host, "sw.battle.raid.boss", 1, raid.participants[4]);
        const auto boss_species = raid.participants[4].mon.species;
        Taped(raid_boss_board_tape_, boss_species, host, [&] { // species-only: no HP/PP in this board
            PublishWeaknessBoard(host, "sw.battle.raid.boss", boss_species);
        });
        for (int i = 0; i < sw_battle_raid::kAllies; ++i) {
            char ally_prefix[32];
            std::snprintf(ally_prefix, sizeof ally_prefix, "sw.battle.raid.ally.%d", i);
            PublishRaidMon(host, ally_prefix, 2 + i, raid.participants[i + 1]);
        }
        if (dmax_ready) {
            const auto& own = raid.participants[0];
            PubI(host, "sw.battle.raid.dmax.active", own.dmax ? 1 : 0);
            if (own.dmax) {
                const int remaining = 3 - static_cast<int>(own.dmax_elapsed);
                PubI(host, "sw.battle.raid.dmax.turns", remaining);
                const auto text = sw_str::Format(remaining == 1 ? sw_str::Id::kFieldTurn : sw_str::Id::kFieldTurns, remaining);
                PubT(host, "sw.battle.raid.dmax.turns_text", text.c_str());
            }
        }
        return raid;
    }

    void PublishBattleBall(const EdenDsmodHostApi& host, bool in_battle) {
        battle_ball_count_ = 0;
        u64 main = 0, setup = 0;
        std::uint32_t rule = 3, competitor = 3;
        std::uint8_t comm = 1;
        const bool wild_single = in_battle && battle_client_ &&
            ReadStable(host, battle_client_ + 0x58, &main, sizeof main) && main &&
            ReadStable(host, main + 0x150, &rule, sizeof rule) && rule == 0 &&
            ReadStable(host, main + 0x88, &setup, sizeof setup) && setup &&
            ReadStable(host, setup, &competitor, sizeof competitor) && competitor == 0 &&
            ReadStable(host, setup + 0x3B8, &comm, sizeof comm) && comm == 0;
        battle_wild_single_ = wild_single;
        if (wild_single && bag_raw_ok_) {
            EnsureItemNames(host);
            EnsureItemDescriptions(host);
            // Actual Balls pocket slot order, independent from the capped medicine picker.
            for (std::size_t i = 0; i < sw_item_pouch::kPouches[1].slot_count; ++i) {
                const auto slot = sw_item_pouch::DecodeSlotAt(bag_raw_.data(), bag_raw_.size(), 1, i);
                if (!sw_helper::IsBattleBall(slot.item_id) || !slot.count) continue;
                if (std::find(battle_balls_.begin(), battle_balls_.begin() + battle_ball_count_, slot.item_id) !=
                    battle_balls_.begin() + battle_ball_count_) continue;
                battle_balls_[battle_ball_count_++] = slot.item_id;
            }
        }
        if (std::find(battle_balls_.begin(), battle_balls_.begin() + battle_ball_count_, battle_ball_id_) ==
            battle_balls_.begin() + battle_ball_count_) {
            if (battle_screen_ == 2 && battle_ball_id_ != 0) SetBattleScreen(0);
            battle_ball_id_ = battle_ball_count_ ? battle_balls_[0] : 0;
        }
        const auto publish = [&](const std::string& prefix, int id) {
            const int count = BattleItemCount(id);
            char icon[160]{};
            std::string name, description;
            const bool ready = id && count > 0 && item_names_ready_ &&
                sw_item_names::ItemName(item_names_, id, name) && sw_item_icons::IconSource(id, icon, sizeof icon);
            PubI(host, (prefix + ".ready").c_str(), ready ? 1 : 0);
            if (!ready) return;
            PubI(host, (prefix + ".id").c_str(), id);
            PubI(host, (prefix + ".count").c_str(), count);
            PubT(host, (prefix + ".icon_src").c_str(), icon);
            PubT(host, (prefix + ".name").c_str(), name.c_str());
            const bool desc_ready = item_descs_ready_ &&
                sw_item_descriptions::ItemDescription(item_descs_, id, description);
            PubI(host, (prefix + ".description_ready").c_str(), desc_ready ? 1 : 0);
            if (desc_ready) {
                const auto text = sw_party_details::OneLine(description);
                PubT(host, (prefix + ".description").c_str(), text.c_str());
            }
        };
        int selected = -1;
        for (int i = 0; i < battle_ball_count_; ++i)
            if (battle_balls_[i] == battle_ball_id_) selected = i;
        // Only the selected ball and the X-shortcut's first ball are bound. Their name/icon/
        // description lookups replay from a tape until the pair, a count or a table changes
        // (this ran for 30 per-index slots on every battle sample before, none of them bound).
        const int first = battle_ball_count_ ? battle_balls_[0] : 0;
        std::uint64_t sig = HashValue(static_cast<std::uint64_t>(first) << 32 | static_cast<std::uint32_t>(battle_ball_id_), 0);
        sig = HashValue(static_cast<std::uint64_t>(BattleItemCount(first)) << 32 |
                        static_cast<std::uint32_t>(BattleItemCount(battle_ball_id_)), sig);
        sig = HashValue((item_names_ready_ ? 1u : 0u) | (item_descs_ready_ ? 2u : 0u), sig);
        Taped(ball_tape_, sig, host, [&] {
            publish("sw.battle.ball.first", first);
            publish("sw.battle.ball", battle_ball_id_);
        });
        PubI(host, "sw.battle.ball.index", selected);
        PubI(host, "sw.battle.ball.total", battle_ball_count_);
        PubI(host, "sw.battle.ball.multi", battle_ball_count_ > 1 ? 1 : 0);
        PubI(host, "sw.battle.ball.empty", battle_ball_count_ == 0 ? 1 : 0);
    }

    void PublishSwitchSuggestions(const EdenDsmodHostApi& host, bool open,
                                  const std::array<sw_battle::Mon, 6>& members,
                                  const std::array<int, 6>& order, int order_n,
                                  const sw_battle::Mon& foe, bool foe_ok, std::uint8_t foe_form) {
        struct Pick { int pid; int score; std::array<std::string, 3> reasons; };
        std::vector<Pick> picks;
        sw_pk8::LivePartyMon foe_personal{};
        foe_personal.species = foe.species;
        foe_personal.form = foe_form;
        sw_party_details::Personal fp;
        if (open && foe_ok && foe.cur_hp > 0 && CachedDetailPersonal(host, foe_personal, fp)) {
            for (int k = 1; k < order_n; ++k) {
                const int pid = order[k];
                if (pid < 0 || pid > 5) continue;
                const auto& mon = members[pid];
                const auto& live = live_party_cache_[pid];
                const auto& detail = party_detail_mons_[pid];
                sw_party_details::Personal cp;
                // Only matching live party identities supply form-aware personal types.
                // Native selection is still authoritative about traps/other switch restrictions.
                if (!mon.valid || !mon.cur_hp || !live.valid || detail.egg ||
                    live.species != mon.species || live.max_hp != mon.max_hp ||
                    !CachedDetailPersonal(host, detail, cp)) continue;
                int worst = -1, threat_type = -1, best = -1;
                std::string move_name;
                for (const auto& move : foe.moves) {
                    sw_move_data::MoveRecord rec;
                    if (!move.move_id || !move.pp_current || !CachedMove(host, move.move_id, rec) ||
                        !rec.category || rec.power <= 1) continue;
                    const int e = sw_type_chart::EffectivenessPercentDual(sw_species_types::ToChartType(rec.type),
                        sw_species_types::ToChartType(cp.type1), sw_species_types::ToChartType(cp.type2));
                    if (e > worst) { worst = e; threat_type = rec.type; }
                }
                for (const auto& move : mon.moves) {
                    sw_move_data::MoveRecord rec;
                    if (!move.move_id || !move.pp_current || !CachedMove(host, move.move_id, rec) ||
                        !rec.category || rec.power <= 1) continue;
                    const int e = sw_type_chart::EffectivenessPercentDual(sw_species_types::ToChartType(rec.type),
                        sw_species_types::ToChartType(fp.type1), sw_species_types::ToChartType(fp.type2));
                    if (e > best) {
                        best = e;
                        move_name.clear();
                        if (move_names_ready_) sw_move_names::MoveName(move_names_, move.move_id, move_name);
                    }
                }
                if (worst < 0 && best < 0) continue; // no known damaging move: don't invent one
                Pick pick{pid, (best < 0 ? 0 : best) - (worst < 0 ? 100 : worst), {}};
                int r = 0;
                if (threat_type >= 0) {
                    const auto type = std::string(sw_str::TypeName(threat_type));
                    pick.reasons[r++] = worst == 0 ? std::string(sw_str::S(sw_str::Id::kSwitchImmune)) :
                        worst < 100 ? std::string(sw_str::S(sw_str::Id::kSwitchResists)) : worst > 100 ?
                        sw_str::S(sw_str::Id::kSwitchWeakTo) + type : std::string(sw_str::S(sw_str::Id::kSwitchNeutral));
                }
                if (best > 100 && r < 3) pick.reasons[r++] =
                    (move_name.empty() ? std::string(sw_str::S(sw_str::Id::kSwitchBestMove)) : move_name) + (best == 400 ? ": ×4" : ": ×2");
                if (mon.cur_hp * 4 < mon.max_hp && r < 3) pick.reasons[r++] = sw_str::S(sw_str::Id::kSwitchLowHp);
                if (r == 0) pick.reasons[0] = sw_str::S(sw_str::Id::kSwitchNoAdvantage);
                // HP only breaks equal type matchups, preserving the stated ranking semantics.
                pick.score = pick.score * 101 + (mon.max_hp > 0 ? mon.cur_hp * 100 / mon.max_hp : 0);
                picks.push_back(std::move(pick));
            }
        }
        std::stable_sort(picks.begin(), picks.end(), [](const Pick& a, const Pick& b) { return a.score > b.score; });
        if (picks.size() > 3) picks.resize(3);
        PubI(host, "sw.battle.switch_suggest.on", open ? 1 : 0);
        PubI(host, "sw.battle.switch_suggest.count", picks.size());
        PubI(host, "sw.battle.switch_suggest.none", open && picks.empty() ? 1 : 0);
        PubT(host, "sw.battle.switch_suggest.note",
             sw_str::S(sw_str::Id::kSwitchNote));
        char prefix[40], key[72];
        for (int i = 0; i < 3; ++i) {
            std::snprintf(prefix, sizeof prefix, "sw.battle.switch_suggest.%d", i);
            std::snprintf(key, sizeof key, "%s.ready", prefix);
            PubI(host, key, i < static_cast<int>(picks.size()) ? 1 : 0);
            if (i >= static_cast<int>(picks.size())) continue;
            const auto& pick = picks[i];
            const auto& mon = members[pick.pid];
            // Name/icon are species-only (review OPTIMIZE #8): cached per shown slot on species
            // identity. The pick itself (order/reasons/pid), and HP below, stay fully live --
            // both can change from a tie-break or a plain HP tick without the species changing.
            std::uint64_t name_sig = mon.species;
            for (char c : party_detail_mons_[static_cast<std::size_t>(pick.pid)].nickname)
                name_sig = HashValue(static_cast<std::uint8_t>(c), name_sig);
            Taped(switch_pick_tape_[static_cast<std::size_t>(i)], name_sig, host, [&] {
                EnsureSpeciesNames(host);
                std::string name;
                if (OwnMonName(pick.pid, mon.species, name)) {
                    std::snprintf(key, sizeof key, "%s.name", prefix);
                    PubT(host, key, name.c_str());
                }
                PublishIcon(host, prefix, mon.species);
            });
            std::snprintf(key, sizeof key, "%s.pid", prefix); PubI(host, key, pick.pid);
            std::snprintf(key, sizeof key, "%s.shiny", prefix); PubI(host, key, OwnMonShiny(pick.pid, mon.species) ? 1 : 0);
            std::snprintf(key, sizeof key, "%s.hp_pct", prefix);
            PubI(host, key, mon.max_hp > 0 ? mon.cur_hp * 100 / mon.max_hp : 0);
            char hp_text[24];
            std::snprintf(hp_text, sizeof hp_text, "%u/%u", mon.cur_hp, mon.max_hp);
            std::snprintf(key, sizeof key, "%s.hp_text", prefix); PubT(host, key, hp_text);
            for (int j = 0; j < 3; ++j) {
                std::snprintf(key, sizeof key, "%s.reason%d", prefix, j);
                PubT(host, key, pick.reasons[static_cast<std::size_t>(j)].c_str());
            }
        }
    }

    // PassB bpp array base from the battle client's battle party (client+0x340: u64[count]
    // PassB bpp*, count u8 at +0x30). The party lists every battle member exactly once (active
    // first), so the lowest pointer is pokeID 0; accepted only if the members cover pokeIDs
    // 0..count-1 exactly at kBppStride (live 2026-10-25 wild, 2026-10-30 Battle Tower).
    static bool ClientBppBase(const EdenDsmodHostApi& host, u64 client, u64* out) {
        u64 party = 0;
        std::uint8_t count = 0;
        if (!ReadStable(host, client + 0x340, &party, sizeof party) || !party ||
            !ReadStable(host, party + 0x30, &count, 1) || count < 1 || count > 6) return false;
        std::array<u64, 6> ptrs{};
        if (!ReadStable(host, party, ptrs.data(), count * sizeof(u64))) return false;
        u64 base = ptrs[0];
        for (int i = 1; i < count; ++i) base = std::min(base, ptrs[static_cast<std::size_t>(i)]);
        if (!base) return false;
        unsigned seen = 0;
        for (int i = 0; i < count; ++i) {
            const u64 d = ptrs[static_cast<std::size_t>(i)] - base;
            if (d % kBppStride != 0 || d / kBppStride >= static_cast<u64>(count)) return false;
            seen |= 1u << (d / kBppStride);
        }
        if (seen != (1u << count) - 1) return false;
        *out = base;
        return true;
    }
    void SampleBattle(const EdenDsmodHostApi& host, u64 heap_begin) {
        const auto* a = sw_addresses::Find("battle.own_mon");
        battle_view_ = {};
        battle_own_actor_ = 0;
        battle_command_waiting_ = false;
        native_cursor_ = {};
        target_live_ = false;
        PubI(host, "sw.battle.move_detail.ready", 0);
        PubI(host, "sw.battle.move_detail.description_ready", 0);
        PublishDomainStatus(host, "battle", a);
        if (host.publish_i64)
            PubI(host, "sw.type_chart.ready", 1); // pure data, no reader needed
        if (!a || !a->verified) return;

        // In battle = this tick's (sticky) SampleState classification. Phase 2: SampleState keeps
        // kBattle through turn animations/messages/native party+bag screens while the helper's
        // battle beat runs, so the page keeps live data (HP changes as the turn plays out) instead
        // of going blank whenever CurrentScreen leaves the command-menu sentinel.
        const bool in_battle = phase_known_ && phase_ == Phase::kBattle;
        if (in_battle && !epoch_in_battle_) ++battle_epoch_; // a new battle (also without the helper)
        epoch_in_battle_ = in_battle;
        // Native main8039C0 waits at step2 after opening Bag; main8032C0 waits at
        // step1 after opening the Pokemon list. Read the whole FSM identity twice:
        // command-wait heartbeat silence alone also means animations and messages.
        std::array<std::uint8_t, 0x14> native_state{};
        const bool window_ready = in_battle && battle_client_ && host.main_base &&
            ReadStable(host, battle_client_ + 0x560, native_state.data(), native_state.size());
        u64 state_fn = 0;
        std::uint32_t state_step = 0;
        if (window_ready) {
            std::memcpy(&state_fn, native_state.data(), sizeof state_fn);
            std::memcpy(&state_step, native_state.data() + 0x10, sizeof state_step);
        }
        bool native_bag = window_ready && state_fn == host.main_base + sw_variant::kMain.battle_bag_fn && state_step == 2;
        // The forced-replacement event runs through the outer event callback, not +560.
        // Native7FCF44 dispatches it only in outer phase1. Event6's callback7FF830
        // opens the Pokemon list, then step3 waits for the native selection result.
        std::array<u64, 3> event_state{};
        std::uint8_t event_phase = 0;
        const bool event_ready = in_battle && battle_client_ && host.main_base &&
            ReadStable(host, battle_client_ + 0x548, event_state.data(), sizeof event_state) &&
            ReadStable(host, battle_client_ + 0x2A8, &event_phase, sizeof event_phase);
        const bool forced_party = event_ready && event_phase == 1 &&
            event_state[0] == host.main_base + sw_variant::kMain.battle_forced_party_fn && event_state[1] == 0 &&
            static_cast<std::uint32_t>(event_state[2]) == 3;
        const bool native_party = forced_party ||
            (window_ready && state_fn == host.main_base + sw_variant::kMain.battle_pokemon_fn && state_step == 1);
        battle_command_waiting_ = window_ready && !forced_party &&
            state_fn == host.main_base + sw_variant::kMain.battle_command_wait_fn && state_step == 5;
        // Native top-screen cursor (command menu / move list), fail closed to -1.
        if (window_ready && !forced_party)
            native_cursor_ = sw_battle_cursor::Sample(
                [&](u64 address, void* out, std::size_t size) { return ReadStable(host, address, out, size); },
                host.main_base, battle_client_);
        if (in_battle && battle_client_ && host.main_base && !window_ready) {
            ++diag_window_fail_;
            if (window_misses_ <= kMenuHoldTicks) ++window_misses_;
        } else {
            window_misses_ = 0;
        }
        HoldNativeMenu(host.main_base, in_battle, window_ready && !forced_party, forced_party, state_fn, state_step);
        // Every menu's cursor Layout by ownership (hide phase 2), whatever the client state, and
        // the raid catch prompt (never hidden).
        menu_layouts_ = {};
        catch_prompt_ = false;
        if (window_ready) {
            const auto rd = [&](u64 address, void* out, std::size_t size) { return ReadStable(host, address, out, size); };
            menu_layouts_ = sw_battle_cursor::SampleLayouts(rd, host.main_base, battle_client_, layout_cache_);
            // Unknown (a pane read failed) is "shown" -- the prompt is never hidden -- except
            // within kMenuHoldTicks of a reading of "not shown" in this battle: one failed read
            // must not drop the whole menu hide for a frame (owner Thor 2026-11-01 flicker).
            const int prompt = menu_layouts_.command_menu
                ? sw_battle_cursor::SampleCatchPrompt(rd, host.main_base, menu_layouts_.command_menu) : 0;
            if (prompt >= 0) {
                catch_client_ = battle_client_;
                catch_known_ = prompt;
                catch_misses_ = 0;
            }
            catch_prompt_ = prompt > 0 ||
                (prompt < 0 && !(catch_client_ == battle_client_ && catch_known_ == 0 && ++catch_misses_ <= kMenuHoldTicks));
        }
        if (!in_battle || command_cells_client_ != battle_client_) {
            command_cells_ = -1;
            command_cells_client_ = battle_client_;
        }
        if (native_cursor_.menu == sw_battle_cursor::Menu::Command && menu_layouts_.command_menu) {
            const auto rd = [&](u64 address, void* out, std::size_t size) { return ReadStable(host, address, out, size); };
            native_cursor_.cells = sw_battle_cursor::SampleCells(rd, host.main_base, menu_layouts_.command_menu);
            if (native_cursor_.cells >= 0) command_cells_ = native_cursor_.cells;
        }
        if (!in_battle) target_cursor_ok_ = true; // (in target select: set from its cards below)
        {
            // Native command set (battle-kinds bug 5): the bottom shows exactly the cells the native
            // menu shows (Tower: no Bag; doubles 2nd Pokemon: Back for Run; raid: Cheer On after a
            // faint). Unknown -> the regular four.
            const int cells = command_cells_ >= 0 ? command_cells_ : 0b11101;
            static constexpr const char* kCmdKeys[6] = {"fight", "cheer", "pokemon", "bag", "run", "back"};
            int shown = 0;
            for (int i = 0; i < 6; ++i) {
                char key[48];
                std::snprintf(key, sizeof key, "sw.battle.cmd.%s", kCmdKeys[i]);
                PubI(host, key, (cells >> i) & 1);
                if (i != 1 && ((cells >> i) & 1)) ++shown;
            }
            PubI(host, "sw.battle.cmd.known", command_cells_ >= 0 ? 1 : 0);
            PubI(host, "sw.battle.cmd.count", shown);
        }
        battle_rule_ = -1;
        if (in_battle && battle_client_) {
            u64 main = 0;
            std::uint32_t rule = 0;
            if (ReadStable(host, battle_client_ + 0x58, &main, sizeof main) && main &&
                ReadStable(host, main + 0x150, &rule, sizeof rule) && rule <= 0xFF)
                battle_rule_ = static_cast<int>(rule);
        }
        const bool doubles = battle_rule_ == 1;
        PubI(host, "sw.battle.doubles", doubles ? 1 : 0);
        PubI(host, "sw.battle.native.target", native_cursor_.target);
        PubI(host, "sw.battle.native.target_known", native_cursor_.menu == sw_battle_cursor::Menu::Target &&
                                                     native_cursor_.target >= 0 ? 1 : 0);
        for (int t = 0; t < 4; ++t) {
            char key[48];
            std::snprintf(key, sizeof key, "sw.battle.native.target%d", t);
            PubI(host, key, native_cursor_.menu == sw_battle_cursor::Menu::Target && native_cursor_.target == t ? 1 : 0);
        }
        // Native quick-ball picker (X on top): the bottom follows to its ball page showing the
        // same ball; it returns to the command page when the picker closes.
        const bool native_balls = native_cursor_.menu == sw_battle_cursor::Menu::Balls;
        if (native_balls) {
            battle_screen_ = 2;
            battle_ball_id_ = native_cursor_.ball_item;
            ball_screen_native_ = true;
        } else if (ball_screen_native_) {
            ball_screen_native_ = false;
            if (battle_screen_ == 2) SetBattleScreen(0);
        }
        PubI(host, "sw.battle.native.balls", native_balls ? 1 : 0);
        PubI(host, "sw.battle.native.menu", static_cast<int>(native_cursor_.menu));
        PubI(host, "sw.battle.native.command", native_cursor_.command);
        PubI(host, "sw.battle.native.move", native_cursor_.move);
        PubI(host, "sw.battle.native.max_list", native_cursor_.menu == sw_battle_cursor::Menu::Fight ? native_cursor_.max_list : -1);
        PubI(host, "sw.battle.native.dmax_cell", native_cursor_.menu == sw_battle_cursor::Menu::Fight && native_cursor_.dmax_cell ? 1 : 0);
        PubI(host, "sw.battle.native.raw_slot", native_cursor_.raw_slot);
        PubI(host, "sw.battle.native.raw_grid", native_cursor_.raw_grid);
        PubI(host, "sw.battle.native_command_waiting", battle_command_waiting_ ? 1 : 0);
        PubI(host, "sw.battle.native_window_ready", window_ready ? 1 : 0);
        if (native_balls) native_bag = false;   // the quick-ball picker, not the full Bag
        PubI(host, "sw.battle.native_bag", native_bag ? 1 : 0);
        PubI(host, "sw.battle.native_party", native_party ? 1 : 0);
        PubI(host, "sw.battle.native_party_forced", forced_party ? 1 : 0);
        if (!in_battle || dmax_preview_.client != battle_client_) dmax_preview_ = {};
        const auto raid = SampleBattleRaid(host, in_battle);
        const bool regular_allowed = !raid.active &&
            (!sw_battle_raid::kRuntimeVerified || !battle_client_ || raid.kind_known);
        SampleBattleField(host, in_battle);
        // Battle-mon root. Preferred: the live battle client's own battle party (client+0x340,
        // entries = PassB bpp*), which holds for wild, trainer AND institution (Battle Tower)
        // battles -- the fixed heap anchor below only matches wild/overworld-trainer battles
        // (LIVE-READERS-132.md 2026-10-30 tower-root section). Fallback: the fixed anchor, only
        // while no valid client party view exists; every read is still Decode-validated.
        u64 own_base = sw_addresses::Resolve(heap_begin, *a);
        u64 party_base = own_base + kPassBOffset; // where client+0x340's entries point
        int root_source = 1; // 1 = fixed heap anchor, 2 = battle client party (PassA), 3 = (PassB)
        if (!raid.ready && in_battle && battle_client_ && regular_allowed) {
            u64 pass_b = 0;
            std::array<std::uint8_t, sw_battle::kStructBytes> b_raw{}, a_raw{};
            if (ClientBppBase(host, battle_client_, &pass_b) &&
                ReadStable(host, pass_b, b_raw.data(), b_raw.size())) {
                const auto b_mon = sw_battle::Decode(b_raw.data());
                const bool a_ok = b_mon.valid && pass_b > kPassBOffset &&
                    ReadStable(host, pass_b - kPassBOffset, a_raw.data(), a_raw.size());
                const auto a_mon = a_ok ? sw_battle::Decode(a_raw.data()) : sw_battle::Mon{};
                if (b_mon.valid) {
                    // PassA (display mirror) when it mirrors PassB's pokeID 0, else PassB itself.
                    const bool mirror = a_mon.valid && a_mon.species == b_mon.species &&
                                        a_mon.level == b_mon.level;
                    own_base = mirror ? pass_b - kPassBOffset : pass_b;
                    party_base = pass_b;
                    root_source = mirror ? 2 : 3;
                }
            } // else: no valid client party view this tick -- fixed anchor, still Decode-validated
            // The client's bpp array does not move within a battle: a view read once stands
            // through up to kMenuHoldTicks failed ticks (the fixed anchor is another battle's
            // memory in an institution battle -- one torn read must not switch every card to it).
            if (root_source != 1) {
                root_hold_ = {battle_client_, own_base, party_base, root_source, 0};
            } else if (root_hold_.client == battle_client_ && root_hold_.misses < kMenuHoldTicks) {
                ++root_hold_.misses;
                own_base = root_hold_.own_base;
                party_base = root_hold_.party_base;
                root_source = root_hold_.source;
            }
        }
        if (!in_battle || root_hold_.client != battle_client_) root_hold_ = {};
        PubI(host, "sw.battle.root_source", root_source);

        // Every active battler by battle position (sw_battle_foe.h): singles, one-trainer doubles,
        // two-trainer doubles (1 vs 2) and multi battles. The roster's members are PassB bpps; each
        // must sit at exactly party_base + pokeID*stride, and is read from the same pass the own
        // side uses. Without a validated roster view: the fixed foe lead (pokeID 12), and doubles
        // fall back to "one trainer per side" (party[0..1] active).
        sw_battle_foe::Snapshot sides;
        if (!raid.active && in_battle && battle_client_ && regular_allowed && root_source != 1) {
            sides = sw_battle_foe::Read(battle_client_, [&](u64 address, void* data, std::size_t bytes) {
                return ReadStable(host, address, data, bytes);
            });
            for (const auto& party : sides.parties)
                for (int i = 0; i < party.count; ++i) {
                    const auto& m = party.members[static_cast<std::size_t>(i)];
                    if (m.address != party_base + static_cast<u64>(m.poke_id) * kBppStride) sides.ready = false;
                }
        }
        if (!sides.ready) sides = {};
        // Held like the bpp root: the roster (who stands where) read once in this battle stands
        // through up to kMenuHoldTicks ticks whose read failed (each battler's HP is read fresh).
        const bool sides_tried = !raid.active && in_battle && battle_client_ && regular_allowed;
        if (sides.ready) {
            sides_last_ = sides;
            sides_client_ = battle_client_;
            sides_misses_ = 0;
        } else if (sides_tried && root_source != 1 && sides_last_.ready && sides_client_ == battle_client_ &&
                   sides_misses_ < kMenuHoldTicks) {
            ++sides_misses_;
            ++diag_sides_fail_;
            sides = sides_last_;
        } else if (sides_tried) {
            ++diag_sides_fail_;
            sides_misses_ = kMenuHoldTicks; // the next success starts a new hold
        }
        if (!in_battle || sides_client_ != battle_client_) sides_last_ = {};
        const auto bpp = [&](const sw_battle_foe::Member& m) {
            return own_base + static_cast<u64>(m.poke_id) * kBppStride;
        };
        // The own client holds two positions in a double and in 1 vs 2 (party[0..1] active), one in
        // a multi battle (party[0]; the partner's lead is the other near battler).
        const bool own_two = sides.ready
            ? sides.parties[static_cast<std::size_t>(sides.own_client)].positions == 2 : doubles;

        // Active own battler: the client's battle party (client+0x340) lists the active member
        // first -- the game reorders it on every switch (live 2026-10-26: [Charizard, Pikachu,
        // Venusaur, ...] after Pikachu fainted and Venusaur was swapped for Charizard). Its
        // entries point into the "pass B" bpp array (own_base + 0x18910, stride 0x7A0, index =
        // pokeID); pass A (own_base) holds identical data. Without the helper: slot 0 (phase 1).
        std::array<int, 6> order{-1, -1, -1, -1, -1, -1};
        int order_n = 0;
        u64 regular_actor = 0;
        std::array<int, 2> own_positions{-1, -1}; // own near left / right pokeIDs (two-position side)
        int choosing = 0, bench_from = 1;
        if (raid.ready) {
            active_idx_ = static_cast<int>(raid.participants[0].poke_id);
            order[0] = active_idx_;
            order_n = 1;
        } else if (in_battle && battle_client_ && regular_allowed) {
            const u64 pass_b = party_base;
            u64 party = 0;
            std::uint8_t count = 0;
            if (ReadStable(host, battle_client_ + 0x340, &party, sizeof party) && party &&
                ReadStable(host, party + 0x30, &count, 1) && count >= 1 && count <= 6) {
                std::array<u64, 6> ptrs{};
                if (ReadStable(host, party, ptrs.data(), count * sizeof(u64))) {
                    bool ok = true;
                    for (int i = 0; i < count; ++i) {
                        const u64 p = ptrs[static_cast<std::size_t>(i)];
                        if (p < pass_b || (p - pass_b) % kBppStride != 0 || (p - pass_b) / kBppStride > 5) {
                            ok = false;
                            break;
                        }
                        order[static_cast<std::size_t>(i)] = static_cast<int>((p - pass_b) / kBppStride);
                    }
                    if (ok) {
                        order_n = count;
                        active_idx_ = order[0];
                        regular_actor = ptrs[0]; // actual native PassB actor, not PassA display mirror
                        own_positions = {order[0], own_two && count >= 2 ? order[1] : -1};
                        // Two own positions: party[0..1] are both active (left, right). The one
                        // choosing a command now is the client's current mon (client+0x78, the
                        // helper's "current mon"); the header, Own tab and moves follow it.
                        u64 current = 0;
                        if (own_two && count >= 2 &&
                            ReadStable(host, battle_client_ + 0x78, &current, sizeof current) &&
                            current == ptrs[1]) {
                            std::swap(order[0], order[1]);
                            active_idx_ = order[0];
                            regular_actor = ptrs[1];
                            choosing = 1;
                        }
                        if (own_two && count >= 2) bench_from = 2;
                    }
                }
            }
        }
        if (order_n == 0) { // no client view: pokeID order, active first
            order[0] = active_idx_;
            order_n = 1;
            for (int i = 0; i < 6; ++i)
                if (i != active_idx_) order[static_cast<std::size_t>(order_n++)] = i;
        }

        // All six own bpps, live (bench HP, fainted flags, the direct SWITCH/ITEM preconditions).
        std::array<sw_battle::Mon, 6> members{};
        if (raid.ready) {
            members[static_cast<std::size_t>(active_idx_)] = raid.participants[0].mon;
        } else if (in_battle && regular_allowed) {
            std::array<std::uint8_t, sw_battle::kStructBytes> raw{};
            for (int i = 0; i < 6; ++i)
                if (ReadStable(host, own_base + static_cast<u64>(i) * kBppStride, raw.data(), raw.size()))
                    members[static_cast<std::size_t>(i)] = sw_battle::Decode(raw.data());
        }

        const u64 own_addr = own_base + static_cast<u64>(active_idx_) * kBppStride;
        // Active foe(s): the far battlers of the roster view -- a trainer's next Pokemon after a
        // faint is followed, and a second trainer's Pokemon in a 1 vs 2 / multi battle is the
        // other active foe (owner Thor 2026-11-01: Maractus far-left, Toxtricity far-right, one
        // Pokemon per trainer). Without a validated roster view: the fixed lead (pokeID 12).
        u64 foe_addr = own_base + sw_battle::kFoeOffset;
        int foe_source = 0, foe_count = 0;
        u64 foe_other_addr = 0;               // two active foes: the one the header does not show
        std::array<u64, 2> foe_positions{};   // far left / right (target cards)
        struct FoeBall { bool fainted = false; int dx = 0; };
        std::array<FoeBall, 5> foe_balls{};
        int foe_ball_n = 0;
        bool foe_two = false;
        if (sides.ready) {
            const auto& left = sides.Slot(0);
            const auto& right = sides.Slot(1);
            foe_positions = {left.valid ? bpp(left.member) : 0, right.valid ? bpp(right.member) : 0};
            foe_two = left.valid && right.valid;
            if (left.valid || right.valid) {
                foe_source = 1;
                // Selection 0 = far right (the first foe position: a one-trainer double's party[0],
                // live Tower doubles Vileplume left / Gourgeist right), 1 = far left. A tab switch,
                // or the native target cursor resting on a foe card, picks it.
                if (foe_two && native_cursor_.menu == sw_battle_cursor::Menu::Target && native_cursor_.target >= 0 &&
                    native_cursor_.target <= 1 && native_cursor_.target != native_target_last_)
                    battle_foe_sel_ = 1 - native_cursor_.target;
                if (!foe_two) foe_addr = right.valid ? foe_positions[1] : foe_positions[0];
                else {
                    foe_addr = battle_foe_sel_ == 1 ? foe_positions[0] : foe_positions[1];
                    foe_other_addr = battle_foe_sel_ == 1 ? foe_positions[1] : foe_positions[0];
                }
            }
            // Bench balls, one group per foe trainer in screen order (far-left's first), alive
            // before fainted; two trainers' groups are split by a gap. They follow the other-foe
            // chip when two foes are active, so the row holds at most 4 balls (3 with a gap).
            std::array<int, 2> foe_clients{left.valid ? left.client : -1, right.valid ? right.client : -1};
            if (foe_clients[0] == foe_clients[1]) foe_clients[0] = -1;
            std::array<std::array<int, 2>, 2> groups{}; // {alive, fainted} per trainer
            for (int g = 0; g < 2; ++g) {
                const int c = foe_clients[static_cast<std::size_t>(g)];
                if (c < 0) continue;
                const auto& party = sides.parties[static_cast<std::size_t>(c)];
                foe_count += party.count;
                for (int i = party.positions; i < party.count; ++i)
                    ++groups[static_cast<std::size_t>(g)][party.members[static_cast<std::size_t>(i)].mon.cur_hp > 0 ? 0 : 1];
            }
            const int na = groups[0][0] + groups[0][1], nb = groups[1][0] + groups[1][1];
            const bool split = na > 0 && nb > 0;
            const int cap = split ? 3 : foe_two ? 4 : 5;
            const int show_a = std::min(na, cap - std::min(nb, 1));
            const int show_b = std::min(nb, cap - show_a);
            for (int g = 0; g < 2; ++g) {
                const auto& grp = groups[static_cast<std::size_t>(g)];
                const int show = g == 0 ? show_a : show_b;
                for (int i = 0; i < show && foe_ball_n < 5; ++i)
                    foe_balls[static_cast<std::size_t>(foe_ball_n++)] = {i >= grp[0], split && g == 1 ? 24 : 0};
            }
        }
        native_target_last_ = native_cursor_.menu == sw_battle_cursor::Menu::Target ? native_cursor_.target : -1;
        foe_second_ready_ = foe_two;
        if (!foe_second_ready_) battle_foe_sel_ = 0;
        target_sides_ok_ = sides.ready; // (held above)
        PubI(host, "sw.battle.foe.sel", battle_foe_sel_);
        PubI(host, "sw.battle.foe.sel0", foe_second_ready_ && battle_foe_sel_ == 0 ? 1 : 0);
        PubI(host, "sw.battle.foe.sel1", foe_second_ready_ && battle_foe_sel_ == 1 ? 1 : 0);
        PubI(host, "sw.battle.foe.duo", foe_second_ready_ ? 1 : 0);
        PubI(host, "sw.battle.sides.mode", sides.ready ? sides.mode : -1);
        {   // The other active battler of each side (doubles): a compact chip in the header row,
            // and the four native target cards (far left/right, near left/right by position).
            std::array<std::uint8_t, sw_battle::kStructBytes> raw{};
            const auto summary = [&](const EdenDsmodHostApi& host, const char* prefix, u64 addr, bool own_side, int pid) {
                sw_battle::Mon mon;
                const bool ok = addr && ReadStable(host, addr, raw.data(), raw.size()) &&
                                (mon = sw_battle::Decode(raw.data())).valid;
                char key[64];
                std::snprintf(key, sizeof key, "%s.ready", prefix);
                PubI(host, key, ok ? 1 : 0);
                if (!ok) return false;
                std::string name;
                EnsureSpeciesNames(host);
                const bool named = own_side ? OwnMonName(pid, mon.species, name)
                                            : species_names_ready_ && sw_species_names::SpeciesName(species_names_, mon.species, name);
                std::snprintf(key, sizeof key, "%s.name", prefix);
                PubT(host, key, named ? name.c_str() : ""); // never a previous battler's name
                std::snprintf(key, sizeof key, "%s.name_ready", prefix);
                PubI(host, key, named ? 1 : 0);
                const int pct = mon.max_hp ? std::clamp(static_cast<int>(mon.cur_hp) * 100 / mon.max_hp, 0, 100) : 0;
                std::snprintf(key, sizeof key, "%s.hp_pct", prefix);
                PubI(host, key, pct);
                std::snprintf(key, sizeof key, "%s.fainted", prefix);
                PubI(host, key, mon.cur_hp == 0 ? 1 : 0);
                char text[24];
                std::snprintf(text, sizeof text, "%d%%", pct);
                std::snprintf(key, sizeof key, "%s.hp_pct_text", prefix);
                PubT(host, key, text);
                for (const auto [suffix, on] : {std::pair{"hpg", pct > 50}, std::pair{"hpy", pct > 20 && pct <= 50},
                                                std::pair{"hpr", pct <= 20}}) {
                    std::snprintf(key, sizeof key, "%s.%s", prefix, suffix);
                    PubI(host, key, on ? 1 : 0);
                }
                PublishIcon(host, prefix, mon.species);
                std::snprintf(key, sizeof key, "%s.shiny", prefix);
                PubI(host, key, own_side && OwnMonShiny(pid, mon.species) ? 1 : 0);
                return true;
            };
            // The partner: your own second active (two positions), or in a multi battle the NPC
            // partner's lead (another client: species name, never your party's nickname/shiny).
            const auto& partner = sides.Partner();
            const bool own_duo = own_two ? own_positions[1] >= 0 : partner.valid;
            const u64 partner_addr = !own_duo ? 0 : own_two ? own_base + static_cast<u64>(order[1]) * kBppStride
                                                            : bpp(partner.member);
            summary(host, "sw.battle.foe.other", foe_second_ready_ ? foe_other_addr : 0, false, -1);
            summary(host, "sw.battle.partner", partner_addr, true, own_duo && own_two ? order[1] : -1);
            PubI(host, "sw.battle.own.duo2", own_duo ? 1 : 0);
            PubI(host, "sw.battle.own.choosing", choosing);
            const bool cards = native_cursor_.menu == sw_battle_cursor::Menu::Target;
            bool cards_ok = cards && sides.ready;
            // The four cards are rendered onto a tape first (published only below): a complete
            // set is published and kept; an incomplete one (a read failed this tick) while the
            // same target select is still open replays the kept set instead, for up to
            // kMenuHoldTicks -- the bottom never alternates cards / notice per tick (owner Thor
            // 2026-11-01). Nothing complete to keep -> this tick's (incomplete) set and the notice.
            Tape fresh;
            EdenDsmodHostApi quiet = host;
            quiet.publish_i64 = [](void*, const char*, std::int64_t) {};
            quiet.publish_text = [](void*, const char*, const char*) {};
            Tape* const outer_tape = tape_;
            tape_ = &fresh;
            for (int t = 0; t < 4; ++t) {
                char prefix[40];
                std::snprintf(prefix, sizeof prefix, "sw.battle.target.%d", t);
                u64 addr = 0;
                int pid = -1;
                bool expected = false;
                if (cards && sides.ready) {
                    const auto& b = sides.Slot(t);
                    expected = b.valid;
                    if (b.valid) {
                        addr = bpp(b.member);
                        if (b.client == sides.own_client) pid = static_cast<int>(b.member.poke_id);
                    }
                } else if (cards && t >= 2) { // no roster view: your own party order
                    pid = own_positions[static_cast<std::size_t>(t - 2)];
                    addr = pid >= 0 ? own_base + static_cast<u64>(pid) * kBppStride : 0;
                }
                const bool ok = summary(quiet, prefix, addr, t >= 2, pid);
                if (expected && !ok) cards_ok = false;
                char key[48];
                std::snprintf(key, sizeof key, "%s.user", prefix);
                PubI(quiet, key, cards && t >= 2 && pid >= 0 && pid == active_idx_ ? 1 : 0);
            }
            tape_ = outer_tape;
            fresh.valid = true;
            bool live = false;
            if (cards_ok) {
                target_cards_ = std::move(fresh);
                target_cards_client_ = battle_client_;
                target_cards_misses_ = 0;
                target_cards_.Replay(host);
                live = true;
            } else if (cards && native_cursor_.target >= 0 && target_cards_.valid &&
                       target_cards_client_ == battle_client_ && target_cards_misses_ < kMenuHoldTicks) {
                ++target_cards_misses_;
                ++diag_cards_held_;
                target_cards_.Replay(host);
                live = true;
            } else {
                if (!cards) target_cards_.Clear(); // the next target select starts from its own read
                fresh.Replay(host);
            }
            // The bottom target page is the choice surface only when every battler on the field
            // has its card (sw.battle.target.live); else the notice, and the top stays visible.
            PubI(host, "sw.battle.target.live", live ? 1 : 0);
            target_live_ = cards && live && native_cursor_.target >= 0;
            if (cards) target_cursor_ok_ = target_live_;
            PubI(host, "sw.battle.hold.menu", diag_menu_held_);
            PubI(host, "sw.battle.hold.cards", diag_cards_held_);
            PubI(host, "sw.battle.hold.sides_fail", diag_sides_fail_);
            PubI(host, "sw.battle.hold.window_fail", diag_window_fail_);
        }
        PubI(host, "sw.battle.foe.source", foe_source);
        PubI(host, "sw.battle.foe.party_ready", foe_source);
        PubI(host, "sw.battle.foe.party_count", foe_count);
        for (int i = 0; i < 5; ++i) {
            const auto& ball = foe_balls[static_cast<std::size_t>(i)];
            char key[48];
            std::snprintf(key, sizeof key, "sw.battle.foe.bench.%d.shown", i);
            PubI(host, key, i < foe_ball_n ? 1 : 0);
            std::snprintf(key, sizeof key, "sw.battle.foe.bench.%d.fainted", i);
            PubI(host, key, i < foe_ball_n && ball.fainted ? 1 : 0);
            std::snprintf(key, sizeof key, "sw.battle.foe.bench.%d.dx", i);
            PubI(host, key, i < foe_ball_n ? ball.dx : 0);
        }
        std::array<std::uint8_t, sw_battle::kStructBytes> own_raw{}, foe_raw{};
        const bool own_read = raid.ready || (in_battle && regular_allowed && ReadStable(host, own_addr, own_raw.data(), own_raw.size()));
        const bool foe_read = raid.ready || (in_battle && regular_allowed && ReadStable(host, foe_addr, foe_raw.data(), foe_raw.size()));
        const sw_battle::Mon own = raid.ready ? raid.participants[0].mon :
                                  own_read ? sw_battle::Decode(own_raw.data()) : sw_battle::Mon{};
        const sw_battle::Mon foe = raid.ready ? raid.participants[4].mon :
                                  foe_read ? sw_battle::Decode(foe_raw.data()) : sw_battle::Mon{};
        const bool own_ok = own_read && own.valid;
        const bool foe_ok = foe_read && foe.valid;
        PublishSwitchSuggestions(host, native_party && !raid.active, members, order, order_n,
                                 foe, foe_ok, foe_raw[0x33D]);
        PublishBattleBall(host, in_battle);
        if (own_ok) battle_own_actor_ = raid.ready ? raid.participants[0].address : regular_actor;
        std::array<std::uint16_t, 4> current_moves{};
        if (own_ok) for (int i = 0; i < 4; ++i) current_moves[i] = own.moves[i].move_id;
        if (battle_inspect_actor_ != battle_own_actor_ || battle_inspect_ids_ != current_moves) {
            battle_inspect_move_ = 0;
            for (int i = 0; i < 4; ++i) if (current_moves[i]) { battle_inspect_move_ = i; break; }
            battle_inspect_actor_ = battle_own_actor_;
            battle_inspect_ids_ = current_moves;
        }
        SyncNativeCursor(current_moves);
        if (own_ok && !raid.active && regular_actor) {
            // Native7E0A10 is the common BattleMon Dynamax getter. Single native commands use
            // the checked PassB actor; the PassA display mirror cannot supply actor identity.
            // Read the actual active bit through animations and expiry, independently from
            // a cached menu opportunity/eligibility query. Unknown bytes stay withheld.
            std::uint8_t active = 0;
            battle_dmax_active_ = ReadStable(host, regular_actor + 0x298, &active, sizeof active) && active == 1;
        }
        battle_view_ = {};
        battle_view_.in_battle = in_battle;
        battle_view_.own_ok = own_ok;
        battle_view_.foe_ok = foe_ok;
        battle_view_.foe_fainted = foe_ok && foe.cur_hp == 0;
        battle_view_.active_pid = own_ok ? active_idx_ : -1;
        for (int i = 0; i < 6; ++i) {
            const auto& m = members[static_cast<std::size_t>(i)];
            auto& v = battle_view_.members[static_cast<std::size_t>(i)];
            v.ok = m.valid && !BattleMemberIsEgg(i, m);
            v.fainted = v.ok && m.cur_hp == 0;
            v.hp = m.cur_hp;
            v.max_hp = m.max_hp;
            const auto& live = live_party_cache_[static_cast<std::size_t>(i)];
            v.has_status = m.valid && live.valid && live.species == m.species && (live.status & 0xFFu) != 0;
        }
        if (own_ok) {
            for (int i = 0; i < sw_battle::kMoveSlots; ++i) {
                const bool real = own.moves[i].move_id != 0 && own.moves[i].pp_max > 0;
                battle_view_.move_id[static_cast<std::size_t>(i)] = real ? own.moves[i].move_id : 0;
                battle_view_.pp[static_cast<std::size_t>(i)] = real ? own.moves[i].pp_current : 0;
            }
        }

        // "ready" gates toggle every tick (never sticky-true on a failed read), matching
        // SampleParty's own per-slot pattern -- the manifest hides a stale row via this flag.
        if (host.publish_i64) {
            PubI(host, "sw.battle.own.ready", own_ok ? 1 : 0);
            PubI(host, "sw.battle.foe.ready", foe_ok ? 1 : 0);
            if (!own_ok) {
                // Own move rows are published inside the `own_ok` block below -- if the whole
                // own side is not ready this tick, reset every move slot's own gates too, so a
                // page never shows a stale move/PP/name from a previous, different battle.
                for (int i = 1; i <= sw_battle::kMoveSlots; ++i) {
                    ClearBattleMoveGates(host, i);
                }
            }
        }
        if (!own_ok) PublishBattleBench(host, members, order, 0);
        PublishBattleTabs(host);
        if (!foe_ok) PublishFoeMoves(host, foe, false, false, 0, 0);
        if (!own_ok && !foe_ok) return;

        EnsureSpeciesNames(host);
        EnsureMoveNames(host);
        EnsureEffectivenessText(host);
        if (!battle_move_text_attempted_) {
            battle_move_text_attempted_ = true;
            sw_party_details::LoadText(host, sw_lang::MessagePath("wazainfo.dat").c_str(), move_descs_);
        }

        // Foe's own Type1/Type2 (personal-table order), needed by the own-moves loop below to
        // compute each move's effectiveness against THIS foe -- resolved once here (not inside
        // the loop) and reused for every move slot. Reading the foe's own card (PublishWeaknessBoard,
        // later in this function) does the same lookup internally for its own purposes; this is a
        // second, independent read because that helper does not hand its decoded types back to
        // the caller. A foe that is not ready, or whose species the personal table rejects, leaves
        // `foe_types_ready` false -- every move's effectiveness_ready then also stays false
        // (never a guessed matchup).
        const int own_shown = FollowHp(host, heap_begin, gauge_own_, own, own_ok, in_battle);
        const int foe_shown = FollowHp(host, heap_begin, gauge_foe_, foe, foe_ok, in_battle);
        PublishBattleStats(host, "sw.battle.own", own_raw.data(), own_ok && !raid.ready, own_shown);
        PublishBattleStats(host, "sw.battle.foe", foe_raw.data(), foe_ok && !raid.ready, foe_shown);
        bool foe_types_ready = false;
        sw_species_types::u8 foe_type1 = 0, foe_type2 = 0;
        if (foe_ok) foe_types_ready = CachedTypes(host, foe.species, foe_type1, foe_type2);
        if (foe_ok) {
            sw_species_types::u8 own_type1 = 0, own_type2 = 0;
            const bool own_types_ready = own_ok && CachedTypes(host, own.species, own_type1, own_type2);
            PublishFoeMoves(host, foe, true, own_types_ready, own_type1, own_type2);
        }

        if (own_ok) {
            std::string name;
            const bool have_name = OwnMonName(active_idx_, own.species, name);
            const int hp_pct = own.max_hp > 0
                                    ? std::clamp(own_shown * 100 / own.max_hp, 0, 100)
                                    : 0;
            if (host.publish_i64) {
                PubI(host, "sw.battle.own.species", own.species);
                PubI(host, "sw.battle.own.level", own.level);
                PubI(host, "sw.battle.own.cur_hp", own_shown);
                PubI(host, "sw.battle.own.max_hp", own.max_hp);
                PubI(host, "sw.battle.own.hp_pct", hp_pct);
                PubI(host, "sw.battle.own.shiny", OwnMonShiny(active_idx_, own.species) ? 1 : 0);
            }
            if (host.publish_i64) PubI(host, "sw.battle.own.name_ready", have_name ? 1 : 0);
            if (host.publish_text) {
                PubT(host, "sw.battle.own.name",
                                  have_name ? name.c_str() : "");
                char hp_text[24];
                std::snprintf(hp_text, sizeof hp_text, "%d/%u", own_shown, own.max_hp);
                PubT(host, "sw.battle.own.hp_text", hp_text);
                PubT(host, "sw.battle.own.level_text", sw_str::Level(own.level).c_str());
            }

            PublishIcon(host, "sw.battle.own", own.species);
            // The active Pokemon's status badge (battle Bag card, 2026-11-01), from the same party
            // record the bench rows use (the battle struct carries no status); fainted follows
            // the live battle HP.
            {
                std::uint32_t own_status = 0;
                if (active_idx_ >= 0 && active_idx_ < 6) {
                    const auto& live = live_party_cache_[static_cast<std::size_t>(active_idx_)];
                    if (live.valid && live.species == own.species) own_status = live.status;
                }
                PublishStatusIcon(host, "sw.battle.own", own_status,
                                  static_cast<std::uint16_t>(std::max(own_shown, 0)), own.max_hp);
            }
            PublishBattleBench(host, members, order, order_n, bench_from);
            Taped(own_board_tape_, own.species, host, [&] { // Own tab of the matchup card
                PublishWeaknessBoard(host, "sw.battle.ownb", own.species);
            });

            for (int i = 0; i < sw_battle::kMoveSlots; ++i) {
                const auto& mv = own.moves[i];
                // A slot is real only if BOTH the live move id and the live max-PP the game
                // itself computed are nonzero -- either one alone reading 0 is this struct's own
                // "unused slot" convention (sw_battle.h's header comment); requiring both is a
                // cheap extra fail-closed check, not a heuristic (a genuinely-used slot always has
                // both, per every live boot this task observed).
                const bool slot_ready = mv.move_id != 0 && mv.pp_max > 0;
                char key[48];
                std::snprintf(key, sizeof key, "sw.battle.own.move%d.ready", i + 1);
                if (host.publish_i64) PubI(host, key, slot_ready ? 1 : 0);
                if (!slot_ready) {
                    ClearBattleMoveGates(host, i + 1);
                    continue;
                }

                const bool preview_wanted = dmax_preview_.wanted;
                const int gmax_index = dmax_preview_.gmax_indices[static_cast<std::size_t>(i)];
                const auto max_id = dmax_preview_.max_ids[static_cast<std::size_t>(i)];
                const bool preview_match = preview_wanted && max_id > 0 && max_id <= sw_move_data::kMaxMoveId &&
                    dmax_preview_.actor != 0 && dmax_preview_.actor == battle_own_actor_ &&
                    gmax_index >= -1 && gmax_index < 33 &&
                    dmax_preview_.base_ids[static_cast<std::size_t>(i)] == mv.move_id;
                const auto display_id = static_cast<std::uint16_t>(preview_wanted ?
                                            (preview_match ? max_id : 0) : mv.move_id);
                sw_move_data::MoveRecord rec;
                const bool move_data_ok = display_id != 0 && CachedMove(host, display_id, rec);
                const bool preview_ok = preview_match && move_data_ok;
                std::snprintf(key, sizeof key, "sw.battle.own.move%d.preview_ready", i + 1);
                PubI(host, key, preview_ok ? 1 : 0);
                std::snprintf(key, sizeof key, "sw.battle.own.move%d.base_id", i + 1);
                PubI(host, key, mv.move_id);
                std::snprintf(key, sizeof key, "sw.battle.own.move%d.display_id", i + 1);
                PubI(host, key, display_id);

                if (host.publish_text) {
                    char pp_text[16];
                    std::snprintf(pp_text, sizeof pp_text, "%u/%u", mv.pp_current, mv.pp_max);
                    std::snprintf(key, sizeof key, "sw.battle.own.move%d.pp_text", i + 1);
                    PubT(host, key, pp_text);
                }
                // The native PP number colour tier (battle_button_skill_00_pp.bflan T_pp_00 frames):
                // 0 white, 1 at half or less, 2 at a quarter or less, 3 none left.
                std::snprintf(key, sizeof key, "sw.battle.own.move%d.pp_tier", i + 1);
                PubI(host, key, PpTier(mv.pp_current, mv.pp_max));

                // Move identity now comes straight from the live battle struct's own move id
                // (sw_battle.h's Move::move_id, found+verified this task) -- no more matching the
                // active battler against a party slot by species+level (that heuristic is gone;
                // this is strictly more correct, since it also resolves for a species the party
                // doesn't currently hold, e.g. after a switch this reader hasn't re-cached yet).
                bool name_ready = false;
                std::string move_name;
                if (preview_ok && gmax_index >= 0) {
                    EnsureGmaxStrings(host);
                    if (gmax_index < static_cast<int>(gmax_names_.size()) && !gmax_names_[gmax_index].empty()) {
                        move_name = gmax_names_[gmax_index];
                        name_ready = true;
                    }
                } else if ((!preview_wanted || preview_ok) && move_names_ready_) {
                    name_ready = sw_move_names::MoveName(move_names_, display_id, move_name);
                }
                if (name_ready) {
                        std::snprintf(key, sizeof key, "sw.battle.own.move%d.name", i + 1);
                        if (host.publish_text) PubT(host, key, move_name.c_str());
                }
                std::snprintf(key, sizeof key, "sw.battle.own.move%d.name_ready", i + 1);
                if (host.publish_i64) PubI(host, key, name_ready ? 1 : 0);
                // A single combined flag for the manifest's fallback "Move N" label (shown only
                // when this slot is real but the name lookup didn't resolve it) -- a widget binds
                // one hide_bind point, so this is computed here rather than asking a page to AND
                // two conditions together.
                std::snprintf(key, sizeof key, "sw.battle.own.move%d.show_fallback", i + 1);
                if (host.publish_i64) PubI(host, key, name_ready ? 0 : 1);

                // Move type disc + effectiveness vs the foe (sw_move_data.h/sw_type_chart.h/
                // sw_effectiveness_text.h): only published once the move's own data decodes AND
                // the foe's types are known this tick -- never a guessed matchup. Status moves
                // (category 0) get a type disc but no effectiveness word/multiplier, matching the
                // game's own move-select hint (it only rates damaging moves).
                const bool power_ready = move_data_ok && rec.category != 0 &&
                    (preview_wanted ? preview_ok && gmax_index < 0 &&
                        dmax_preview_.powers[static_cast<std::size_t>(i)] > 1 &&
                        dmax_preview_.powers[static_cast<std::size_t>(i)] <= 65535 : rec.power > 1);
                std::snprintf(key, sizeof key, "sw.battle.own.move%d.power_ready", i + 1);
                PubI(host, key, power_ready ? 1 : 0);
                if (power_ready) {
                    const auto power = preview_wanted ? dmax_preview_.powers[static_cast<std::size_t>(i)] : rec.power;
                    std::snprintf(key, sizeof key, "sw.battle.own.move%d.power", i + 1);
                    PubI(host, key, power);
                    const auto text = std::to_string(power);
                    std::snprintf(key, sizeof key, "sw.battle.own.move%d.power_text", i + 1);
                    PubT(host, key, text.c_str());
                }
                const bool description_ready = preview_ok && gmax_index >= 0 &&
                    gmax_index < static_cast<int>(gmax_descriptions_.size()) && !gmax_descriptions_[gmax_index].empty();
                std::snprintf(key, sizeof key, "sw.battle.own.move%d.description_ready", i + 1);
                PubI(host, key, description_ready ? 1 : 0);
                if (description_ready) {
                    const auto description = FlatGameText(gmax_descriptions_[gmax_index]);
                    std::snprintf(key, sizeof key, "sw.battle.own.move%d.description", i + 1);
                    PubT(host, key, description.c_str());
                }
                char type_src[160];
                const bool type_src_ok =
                    move_data_ok && sw_species_types::TypeImageSource(rec.type, false, type_src, sizeof type_src);
                std::snprintf(key, sizeof key, "sw.battle.own.move%d.type_ready", i + 1);
                if (host.publish_i64) PubI(host, key, type_src_ok ? 1 : 0);
                if (type_src_ok) {
                    std::snprintf(key, sizeof key, "sw.battle.own.move%d.type_src", i + 1);
                    if (host.publish_text) PubT(host, key, type_src);
                    std::snprintf(key, sizeof key, "sw.battle.own.move%d.type_id", i + 1);
                    if (host.publish_i64) PubI(host, key, rec.type);
                }
                // Collapsed move pills (D3): the move's category and power on one line, the
                // detail pane's own wording ("Special · 90", "Status · —").
                std::snprintf(key, sizeof key, "sw.battle.own.move%d.catpow_ready", i + 1);
                if (host.publish_i64) PubI(host, key, move_data_ok ? 1 : 0);
                if (move_data_ok && host.publish_text) {
                    const std::string power = power_ready ?
                        std::to_string(preview_wanted ? dmax_preview_.powers[static_cast<std::size_t>(i)] : rec.power) :
                        std::string("—");
                    // A Max move takes its base move's category (the Max move records are all
                    // Physical in the table); Max Guard (a status base) stays Status.
                    int category = rec.category;
                    sw_move_data::MoveRecord base_rec;
                    if (preview_ok && mv.move_id != display_id && CachedMove(host, mv.move_id, base_rec))
                        category = base_rec.category == 0 ? rec.category : base_rec.category;
                    const std::string catpow = std::string(MoveCategoryText(category)) + " · " + power;
                    std::snprintf(key, sizeof key, "sw.battle.own.move%d.catpow_text", i + 1);
                    PubT(host, key, catpow.c_str());
                }

                const bool want_effectiveness = move_data_ok && rec.category != 0 /* not Status */ &&
                                                 foe_types_ready;
                if (want_effectiveness) {
                    const auto atk = sw_species_types::ToChartType(rec.type);
                    const auto def1 = sw_species_types::ToChartType(foe_type1);
                    const auto def2 = sw_species_types::ToChartType(foe_type2);
                    const std::uint32_t percent = sw_type_chart::EffectivenessPercentDual(atk, def1, def2);
                    std::string word;
                    const bool have_word = effectiveness_text_ready_ &&
                        sw_effectiveness_text::Label(effectiveness_text_,
                                                      sw_effectiveness_text::TierForPercent(percent), word);
                    std::snprintf(key, sizeof key, "sw.battle.own.move%d.effectiveness_ready", i + 1);
                    if (host.publish_i64) PubI(host, key, have_word ? 1 : 0);
                    if (have_word && host.publish_text) {
                        // One combined field for the manifest to bind directly -- the native word
                        // (sw_effectiveness_text.h, the game's own 4-tier label) plus this
                        // project's own finer x4/x2/x1/half/quarter/x0 multiplier (task spec:
                        // "with the game's effectiveness labels"), e.g. "Not very effective x1/2".
                        char combined[64];
                        std::snprintf(combined, sizeof combined, "%s %s", word.c_str(),
                                      sw_effectiveness_text::MultiplierText(percent));
                        std::snprintf(key, sizeof key, "sw.battle.own.move%d.effectiveness_text", i + 1);
                        PubT(host, key, combined);
                        std::snprintf(key, sizeof key, "sw.battle.own.move%d.effectiveness_multiplier", i + 1);
                        PubT(host, key, sw_effectiveness_text::MultiplierText(percent));
                    }
                } else {
                    std::snprintf(key, sizeof key, "sw.battle.own.move%d.effectiveness_ready", i + 1);
                    if (host.publish_i64) PubI(host, key, 0);
                }
                std::snprintf(key, sizeof key, "sw.battle.own.move%d.inspected", i + 1);
                PubI(host, key, i == battle_inspect_move_ ? 1 : 0);
                if (i == battle_inspect_move_ && name_ready && move_data_ok) {
                    const std::string p = "sw.battle.move_detail.";
                    const auto pi = [&](const char* suffix, std::int64_t v) { PubI(host, (p + suffix).c_str(), v); };
                    const auto pt = [&](const char* suffix, const std::string& v) { PubT(host, (p + suffix).c_str(), v.c_str()); };
                    pi("ready", 1);
                    pi("index", i);
                    pi("type_id", rec.type);
                    pt("name", move_name);
                    if (type_src_ok) pt("type_src", type_src);
                    char label_src[160]{};
                    if (sw_species_types::TypeImageSource(rec.type, true, label_src, sizeof label_src))
                        pt("type_label_src", label_src);
                    pt("pp_text", std::to_string(mv.pp_current) + "/" + std::to_string(mv.pp_max));
                    pi("power_ready", power_ready ? 1 : 0);
                    pt("power_text", power_ready ? std::to_string(preview_wanted ? dmax_preview_.powers[i] : rec.power) : "—");
                    pt("category_text", MoveCategoryText(rec.category));
                    std::string description;
                    if (description_ready) description = FlatGameText(gmax_descriptions_[gmax_index]);
                    else if ((!preview_wanted || preview_ok) && display_id < move_descs_.size())
                        description = sw_party_details::OneLine(move_descs_[display_id]);
                    pi("description_ready", description.empty() ? 0 : 1);
                    pt("description", description);
                    std::string effectiveness;
                    if (want_effectiveness) {
                        const int percent = sw_type_chart::EffectivenessPercentDual(sw_species_types::ToChartType(rec.type),
                            sw_species_types::ToChartType(foe_type1), sw_species_types::ToChartType(foe_type2));
                        if (effectiveness_text_ready_ && sw_effectiveness_text::Label(effectiveness_text_,
                                sw_effectiveness_text::TierForPercent(percent), effectiveness))
                            effectiveness += std::string(" ") + sw_effectiveness_text::MultiplierText(percent);
                    }
                    pi("effectiveness_ready", effectiveness.empty() ? 0 : 1);
                    pt("effectiveness_text", effectiveness);
                }
            }
        }

        if (foe_ok) {
            std::string name;
            const bool have_name = species_names_ready_ &&
                                   sw_species_names::SpeciesName(species_names_, foe.species, name);
            // Foe HP is intentionally published as a percentage only, never the raw cur/max
            // numbers -- matching the native battle screen, which never shows the foe's exact HP
            // either (task spec: "foe bar (name, level, HP %)").
            const int hp_pct = foe.max_hp > 0
                                    ? std::clamp(foe_shown * 100 / foe.max_hp, 0, 100)
                                    : 0;
            if (host.publish_i64) {
                PubI(host, "sw.battle.foe.species", foe.species);
                PubI(host, "sw.battle.foe.level", foe.level);
                PubI(host, "sw.battle.foe.hp_pct", hp_pct);
                PubI(host, "sw.battle.foe.current_hp", foe_shown);
                PubI(host, "sw.battle.foe.max_hp", foe.max_hp);
            }
            if (host.publish_i64) PubI(host, "sw.battle.foe.name_ready", have_name ? 1 : 0);
            if (host.publish_i64) {
                // Only a wild single's foe comes from the encounter PK8: a trainer's Pokemon of the same
                // species as the last wild encounter must not inherit its shininess.
                PubI(host, "sw.battle.foe.shiny", !raid.active && battle_wild_single_ && WildFoeShiny(host, heap_begin, foe) ? 1 : 0);
            }
            if (host.publish_text) {
                PubT(host, "sw.battle.foe.name",
                                  have_name ? name.c_str() : "");
                PubT(host, "sw.battle.foe.level_text", sw_str::Level(foe.level).c_str());
                char pct_text[8];
                std::snprintf(pct_text, sizeof pct_text, "%d%%", hp_pct);
                PubT(host, "sw.battle.foe.hp_pct_text", pct_text);
                const auto hp_text = std::to_string(foe_shown)+"/"+std::to_string(foe.max_hp);
                PubT(host, "sw.battle.foe.hp_text", hp_text.c_str());
            }

            // Matchup card: the foe's own types + what it's weak/immune to (personal data, same
            // helper the Pokémon page's weakness board uses) -- real data, no move-type guess
            // needed for this half of the card.
            Taped(foe_board_tape_, foe.species, host, [&] {
                PublishWeaknessBoard(host, "sw.battle.foe", foe.species);
                PublishIcon(host, "sw.battle.foe", foe.species);
            });
        }
    }
};

} // namespace sw_reader
