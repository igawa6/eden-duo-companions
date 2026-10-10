// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Pokémon Brilliant Diamond 1.3.0 companion module, built for the Luminescent Platinum 2.2F mod
// (whose exefs IPS keeps the 1.3.0 build id). Reads the party and the battle state live and
// drives the battle menus from the second screen:
//   - Battle / move choices: write the menu's CurrentIndex, then one A press (the game's own
//     Submit runs, so every side effect is native); the top-screen command and move lists are
//     parked off-screen by re-pointing their slide transition (always: runtime 18
//     cannot deliver a settings flag to modules);
//   - Pokémon / Bag / Run use the game's own top-screen windows;
//   - X opens the native Poké Ball list off-screen. The second screen recreates its selector,
//     description and Use button; Use submits the native entry with one A press.
// Art and the game font are decoded from the player's own romfs (lp_assets); nothing is shipped.

#include "core/mods/dsmod_module_abi.h"
#include "core/mods/dsmod_module_extensions.h"
#include "core/mods/modules/dsmod_module_sdk.h"
#include "lp_anim.h"
#include "lp_field_actions.h"
#include "lp_assets.h"
#include "lp_battle_drive.h"
#include "lp_dex_text.h"
#include "lp_guest.h"
#include "lp_lang.h"
#include "lp_live.h"
#include "lp_map.h"
#include "lp_poketch.h"
#include "lp_poketch_display.h"
#include "lp_strings_data.h"
#include "lp_switch.h"
#include "lp_text.h"
#include "lp_vanilla.h"
#include "lp_profile.h"

#include <mutex>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <chrono>
#include <cmath>
#include <ctime>
#include <memory>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>

namespace {
using namespace dsmod_sdk::int_types;

constexpr u64 TitleId = lp_profile::Active.title_id;
constexpr std::string_view BuildId = lp_profile::Active.build_id;
constexpr bool Pearl = TitleId == UINT64_C(0x010018E011D92000);
// A file only the Luminescent Platinum romfs carries (lp_vanilla.h): read_romfs takes a C string,
// and the view is a whole string literal, so its data() is terminated.
static_assert(lp_vanilla::LumiMarker.data()[lp_vanilla::LumiMarker.size()] == '\0');

// Values no manifest binds, read only by the e2e harness scripts and console checks (lp.ready,
// lp.delta, lp.guest, lp.battle, lp.field, lp.lang.*, lp.drive.*, ...): published only by a build
// with -DLP_DEBUG_KEYS=1 (CMake option LP_DEBUG_KEYS).
#ifndef LP_DEBUG_KEYS
#define LP_DEBUG_KEYS 0
#endif
constexpr bool DebugKeys = LP_DEBUG_KEYS != 0;

std::string BuildHex(const EdenDsmodHostApi& h) {
    static constexpr char Hex[] = "0123456789ABCDEF";
    std::string s;
    for (u8 b : h.build_id) {
        s.push_back(Hex[b >> 4]);
        s.push_back(Hex[b & 15]);
    }
    return s;
}

enum class Btn { None, A, B, X, Plus, DUp, DDown, DLeft, DRight };

// One queued battle operation. Every step checks the game state it expects (menu focus, window
// open) and gives up after a timeout, so a dialog or animation can never make it press blindly.
struct Drive {
    enum class Kind { None, Move, OpenMoves, BackMoves, Command, Ball, Target } kind = Kind::None;
    int arg = 0;
    int step = 0;
    u64 step_tick = 0;
    u64 cursor_tick = 0; // latest menu-cursor write; retries do not reset step_tick/deadline
    int result = 0;       // last finished drive: 1 ok, -1 timed out / refused (not bound: for console checks)
    unsigned pressed = 0; // bit n: the press scheduled at slot n of this step was sent
};

class Module {
public:
    explicit Module(const EdenDsmodHostApi& api)
        : host{api}, live{MakeGuest(this)}, assets{lp_assets::MakeRomfsReader(api), Pearl} {
        supported = BuildHex(api) == BuildId;
        lp_poketch::ResetSurfaces(); // a recreated module must not show the last session's cells
        strings.Build(lp_strings::Rows, lp_lang::Variant::En, nullptr); // English until a catalog is read
        for (const auto& row : lp_strings::Rows)
            string_names.push_back("lp.t." + std::string{row.key});
    }
    ~Module() {
        // The game keeps the parked lists for the whole session: put them back before unloading
        // (destroy runs on the tick thread while the host is still alive).
        try {
            RestoreShortcut();
            if (menus_parked)
                live.HideBattleMenus(false);
        } catch (...) {
        }
        assets.Stop();
    }
    Module(const Module&) = delete;
    Module& operator=(const Module&) = delete;

    void UpdateHostOnly(const EdenDsmodHostApi& api) {
        host = api;
    }
    void UpdateHost(const EdenDsmodHostApi& api) {
        host = api;
        tick = host.get_tick ? host.get_tick(host.userdata) : tick + 1;
    }

    void ConfigureWrite(const EdenDsmodHostWriteApi* api) {
        if (api && api->version == EDEN_DSMOD_WRITE_EXT_VERSION && api->abi_hash == EDEN_DSMOD_WRITE_EXT_HASH &&
            api->struct_size >= sizeof(*api) && api->write_batch)
            write_api = *api;
    }

    bool Batch(const std::vector<lp_live::BatchOp>& ops) const {
        if (!write_api.write_batch || ops.empty() || ops.size() > EDEN_DSMOD_WRITE_BATCH_MAX_OPS)
            return false;
        std::vector<EdenDsmodWriteOp> w;
        for (const auto& o : ops) {
            if (o.value.empty() || o.value.size() > EDEN_DSMOD_WRITE_BATCH_MAX_BYTES ||
                (!o.expect.empty() && o.expect.size() != o.value.size()))
                return false;
            w.push_back({o.addr, static_cast<uint32_t>(o.value.size()), 0,
                         o.expect.empty() ? nullptr : o.expect.data(), o.value.data()});
        }
        return write_api.write_batch(write_api.userdata, w.data(), static_cast<uint32_t>(w.size())) != 0;
    }

    void Configure(const EdenDsmodHostExtensions* ext) {
        if (ext && ext->version == EDEN_DSMOD_EXT_VERSION && ext->abi_hash == EDEN_DSMOD_EXT_HASH &&
            ext->struct_size >= sizeof(*ext)) {
            assets.SetAstcDecoder(ext->userdata, ext->decode_astc);
            if (ext->mailbox_address && ext->mailbox_size >= 0x1000 && ext->load_u32 && ext->store_u32 &&
                ext->load_u64 && ext->store_u64) {
                host_ext = *ext;
                const auto* e = &host_ext;
                guest.Configure({e->mailbox_address,
                                 [e](u32 o, u32* v) { return e->load_u32(e->userdata, o, v) != 0; },
                                 [e](u32 o, u32 v) { return e->store_u32(e->userdata, o, v) != 0; },
                                 [e](u32 o, u64 v) { return e->store_u64(e->userdata, o, v) != 0; },
                                 [e](u32 o, u64* v) { return e->load_u64(e->userdata, o, v) != 0; }});
            }
        }
    }

    void Sample() {
        I64("lp.supported", supported ? 1 : 0); // the start page's derived values
        if (!supported)
            return;
        if (lumi < 0)
            lumi = host.read_romfs && host.read_romfs(host.userdata, lp_vanilla::LumiMarker.data(), 0, nullptr, 0) > 0 ? 1 : 0;
        I64("lp.lumi", lumi);
        assets.StartCatalog(tick);
        I64("lp.catalog", assets.CatalogReady() ? 1 : 0);
        PublishLanguage();
        const int64_t host_delta =
            host.get_i64 ? host.get_i64(host.userdata, "__relocation_delta", 0) : 0;
        if (!live.Resolve(host_delta)) {
            if constexpr (DebugKeys) I64("lp.ready", 0);
            return;
        }
        if constexpr (DebugKeys) I64("lp.ready", 1);
        last_sample_tick = tick;
        if constexpr (DebugKeys) I64("lp.delta", live.Delta());
        live.SetLuminescent(lumi == 1);
        snap = live.Sample();
        if (snap.demo_active) {
            // Discard delayed native menu navigation across cinematic transitions.
            pending_press = Btn::None;
            Finish(-1);
            open_state = 0;
            if (key_state) RestoreShortcut();
        }
        I64("lp.player.sex", snap.player_sex);
        if (snap.player_ok && snap.game_version >= 0)
            assets.SetDataVersion(snap.game_version != 0);
        // The save's language, once a save runs (before that, and on loading screens, the last one
        // seen; English until then): the catalog, the art and the font follow it.
        if (snap.player_ok && snap.field && lp_lang::Valid(snap.msg_lang_id)) {
            lang_id = snap.msg_lang_id;
            lang_kanji = snap.is_kanji;
        }
        assets.SetLanguage(lang_id, lang_kanji);
        guest.Poll();
        if constexpr (DebugKeys) I64("lp.guest", guest.Present() ? guest.Alive() ? 2 : 1 : 0);
        if (field_refresh_tick && tick >= field_refresh_tick) { field_refresh_tick = 0; ++scene_revision; }
        PollMapTap();
        StepKeyItem();
        StepBagOpen();
        PublishField();
        PublishBattle();
        PublishAnim();
        StepDrive();
        PublishDrive();
    }

    // Runs every tick, also while the second screen is hidden (the module has on_action). Without
    // the companion the top-screen battle menus must be visible again, so a parked menu is
    // restored once samples stop arriving.
    void Tick() {
        if (!supported || !host.get_tick)
            return;
        tick = host.get_tick(host.userdata);
        if (tick > last_sample_tick + 10) {
            open_state = 0;
            RestoreShortcut();
        }
        // The parked lists outlive the battle (UISystem keeps them all session), so the flag stays set
        // after a battle ends: a later battle without samples needs them back. Between battles there
        // is no battle UI to restore, so a refused restore is retried every RestoreRetryTicks only.
        if (menus_parked && tick > last_sample_tick + 10 && tick >= menus_retry_tick) {
            if (live.HideBattleMenus(false))
                menus_parked = false;
            else
                menus_retry_tick = tick + RestoreRetryTicks;
        }
    }

    EdenDsmodBool OnAction(std::string_view action, int64_t arg) {
        if (!supported)
            return EDEN_DSMOD_FALSE;
        if (action == "battleview") {
            if (!snap.in_battle || !(snap.action.focus || snap.waza.focus))
                return EDEN_DSMOD_FALSE;
            if (drive.kind != Drive::Kind::None)
                return EDEN_DSMOD_FALSE;
            battle_view = arg ? 1 : 0;
            ++scene_revision;
            if ((arg && snap.action.focus) || (!arg && snap.waza.focus)) {
                drive = {};
                drive.kind = arg ? Drive::Kind::OpenMoves : Drive::Kind::BackMoves;
                drive.step_tick = tick;
            }
            return EDEN_DSMOD_TRUE;
        }
        // The battle pane (left card of the command and move pages): companion-only state, no
        // button press, so it works while a drive runs or the game animates.
        if (action == "battletab") {
            // 3 Move (move page), 0 Foe, 1 You, 2 Field (command page, while the field state is
            // read); 10 / 11 = swipe
            if (!snap.in_battle)
                return EDEN_DSMOD_FALSE;
            if (DoubleOn()) {
                // doubles: Move (3) on the move page, then the focused battler (20); on the command
                // page the focused battler, then Field (2); a swipe flips between them
                const int side = FocusSide();
                int& tab = PaneTab();
                const int extra = battle_view == 1 ? 3 : snap.battle_field.valid ? 2 : -1;
                if (arg == 20 || ((arg == 10 || arg == 11) && tab == extra))
                    tab = side;
                else if (extra >= 0 && (arg == extra || arg == 10 || arg == 11))
                    tab = extra;
                else
                    return EDEN_DSMOD_FALSE;
                ++scene_revision;
                return EDEN_DSMOD_TRUE;
            }
            const auto order = PaneTabs();
            const int n = static_cast<int>(order.size());
            int& tab = PaneTab();
            int at = static_cast<int>(std::find(order.begin(), order.end(), tab) - order.begin());
            if (at >= n)
                at = 0;
            if (arg == 10)
                tab = order[static_cast<std::size_t>((at + 1) % n)];
            else if (arg == 11)
                tab = order[static_cast<std::size_t>((at + n - 1) % n)];
            else if (std::find(order.begin(), order.end(), static_cast<int>(arg)) != order.end())
                tab = static_cast<int>(arg);
            else
                return EDEN_DSMOD_FALSE;
            ++scene_revision;
            return EDEN_DSMOD_TRUE;
        }
        if (action == "battlefocus") {
            // a card of the double-battle overview: show that battler until the game's choice changes
            if (!DoubleOn() || arg < 0 || arg > 3 || !snap.dbl.view[static_cast<std::size_t>(arg)].exists)
                return EDEN_DSMOD_FALSE;
            if (snap.dbl.target_open) {
                if (drive.kind != Drive::Kind::None || !live.CanTargetInput(static_cast<int>(arg)))
                    return EDEN_DSMOD_FALSE;
                drive = {};
                drive.kind = Drive::Kind::Target;
                drive.arg = static_cast<int>(arg);
                drive.step_tick = tick;
                return EDEN_DSMOD_TRUE;
            }
            dbl_manual = static_cast<int>(arg);
            SetFocus(dbl_manual);
            if (battle_view == 1) // a tap on the move page shows the battler, also over the Move tab
                tab_fight = SideOf(dbl_manual);
            ++scene_revision;
            return EDEN_DSMOD_TRUE;
        }
        if (action == "battleinspect") {
            // a move chip on the command page: show that move on the Move tab (no drive)
            if (!snap.in_battle || battle_view != 0 || snap.waza.focus || drive.kind != Drive::Kind::None ||
                arg < 0 || arg >= moves_count)
                return EDEN_DSMOD_FALSE;
            pick = static_cast<int>(arg);
            ++scene_revision;
            return EDEN_DSMOD_TRUE;
        }
        // UI state the module needs (the module's snapshot view has no @flag:/@sel: names).
        if (action == "select") {
            if (!FieldContext() || field_view != 0 || bag_popup || arg < 0 ||
                arg >= snap.party_count || !snap.party[arg].valid) return EDEN_DSMOD_FALSE;
            sel_party = static_cast<int>(arg);
            party_move = -1;
            anim_party.Hop(AnimNow()); // also a tap on the selected plate
            return EDEN_DSMOD_TRUE;
        }
        if (action.starts_with("moveswap") && action.size() == 9) {
            const int to = action[8] - '0';
            const int from = lp_field_actions::MoveSlot(arg, move_drag_token);
            if (from < 0 || field_view != 0 || field_tab != 1 || bag_popup || !FieldFree() ||
                sel_party != move_drag_slot || !live.SwapMoves(sel_party, from, to, move_drag_owner))
                return EDEN_DSMOD_FALSE;
            party_move = -1;
            snap = live.Sample();
            ++scene_revision;
            return EDEN_DSMOD_TRUE;
        }
        if (action == "party_move" || action == "party_move_close") {
            if (action == "party_move_close") { party_move = -1; ++scene_revision; return EDEN_DSMOD_TRUE; }
            arg = lp_field_actions::MoveSlot(arg, move_drag_token);
            if (arg < 0 || sel_party != move_drag_slot ||
                !SameMoveLayout(move_drag_owner, snap.party[sel_party].mon)) return EDEN_DSMOD_FALSE;
            if (snap.in_battle || field_view != 0 || field_tab != 1 || bag_popup ||
                sel_party < 0 || sel_party >= snap.party_count || !snap.party[sel_party].valid ||
                snap.party[sel_party].mon.is_egg || arg < 0 || arg >= 4 ||
                snap.party[sel_party].mon.moves[arg] <= 0) return EDEN_DSMOD_FALSE;
            party_move_owner = snap.party[sel_party].mon;
            party_move_id = party_move_owner.moves[arg];
            party_move = static_cast<int>(arg); ++scene_revision;
            return EDEN_DSMOD_TRUE;
        }
        if (action == "fieldview") {
            if (!snap.field || !snap.player_ok || snap.in_battle || snap.is_battling || arg < 0 || arg > 5)
                return EDEN_DSMOD_FALSE;
            int next = static_cast<int>(arg);
            if (next == 2 && !snap.map_acquired) next=1;
            if (next == 2) { route_zone = -1; route_cell.reset(); } // where you are (a map tap picks another place)
            pkt_full = false;
            OpenFieldView(next);
            ++scene_revision;
            return EDEN_DSMOD_TRUE;
        }
        if (action == "bag_pocket" || action == "bag_select" || action == "dex_select" || action == "dex_sort" || action == "dex_mode") {
            if (!snap.field || snap.in_battle || snap.is_battling) return EDEN_DSMOD_FALSE;
            if (action == "bag_pocket" && arg >= 0 && arg < lp_vanilla::BagTabs(lumi != 0)) { bag_pocket=static_cast<int>(arg); bag_selected=0; }
            else if (action == "bag_pocket" && (arg == 10 || arg == 11)) { bag_pocket=lp_vanilla::StepTab(bag_pocket, arg == 10, lumi != 0); bag_selected=0; } // next / previous pocket
            else if (action == "bag_select" && arg >= 0 && arg < static_cast<int64_t>(bag_rows.size())) bag_selected=static_cast<int>(arg);
            else if (action == "dex_select" && snap.pokedex_acquired && arg >= 0 && arg < static_cast<int64_t>(dex_rows.size())) { dex_species=dex_rows[arg]; dex_form=0; anim_dex.Hop(AnimNow()); }
            else if (action == "dex_sort" && snap.pokedex_acquired) dex_sort=(dex_sort+1)%3;
            else if (action == "dex_mode" && snap.pokedex_acquired) dex_national = !dex_national;
            else return EDEN_DSMOD_FALSE;
            ++scene_revision; return EDEN_DSMOD_TRUE;
        }
        // One touch grid over the display (40 x 30 cells of 16 LCD px, the same in the casing and full
        // screen): the module finds the app control under the touch.
        if (action == "pkt_touch" && arg >= 0 && arg < 40 * 30)
            return LcdTouch(static_cast<int>(arg % 40) * 16 + 8, static_cast<int>(arg / 40) * 16 + 8) ? EDEN_DSMOD_TRUE
                                                                                                    : EDEN_DSMOD_FALSE;
        if (action == "pkt_full") {
            // 0 back to the casing, 1 full screen, 2 toggle (long press on the display)
            if (field_view != 3 || pkt_app < 0 || arg < 0 || arg > 2) return EDEN_DSMOD_FALSE;
            pkt_full = arg == 2 ? !pkt_full : arg == 1;
            PoketchSound(pkt_full ? PktZoomIn : PktZoomOut);
            return EDEN_DSMOD_TRUE;
        }
        if (action == "pkt_dot" && pkt_app == lp_poketch::DotArtist && arg >= 0 && arg < 768) {
            if (!dot_modified) { dots.assign(768, 0); dot_modified = true; } // first touch clears the default art
            dots[arg] = (dots[arg] + 1) & 3;
            lp_poketch::SetSurface(lp_poketch::DotArtSurface, dots);
            ++dot_rev;
            return EDEN_DSMOD_TRUE;
        }
        if (action == "pkt_mkmove" && pkt_app == lp_poketch::MarkingMap && arg >= 0 && arg < 32 * 24) {
            // a tap on a marker picks it up (again: puts it back); the next tap places it
            const int tx = static_cast<int>(arg % 32) * 20 + 10, ty = static_cast<int>(arg / 32) * 20 + 10;
            int hit = -1;
            for (int k = 0; k < 6; ++k) {
                const auto [mx, my] = MarkPos(k);
                if (std::abs(mx - tx) <= 22 && std::abs(my - ty) <= 22) hit = k;
            }
            if (hit >= 0 && (mark_sel < 0 || hit == mark_sel)) mark_sel = hit == mark_sel ? -1 : hit;
            else if (mark_sel >= 0) {
                marks[mark_sel] = {std::clamp(static_cast<int>(arg % 32) * 20 + 10, 17, 623),
                                   std::clamp(static_cast<int>(arg / 32) * 20 + 10, 17, 463)};
                mark_sel = -1;
            } else return EDEN_DSMOD_FALSE;
            return EDEN_DSMOD_TRUE;
        }
        if (action == "pkt_memo" || action == "pkt_tool" || action == "pkt_rdraw" || action == "pkt_roul" ||
            action == "pkt_timer") {
            if (!PoketchTouch(action, static_cast<int>(arg))) {
                if (action == "pkt_timer") PoketchSound(PktError);
                return EDEN_DSMOD_FALSE;
            }
            if (action == "pkt_tool" || action == "pkt_roul") PoketchSound(PktClick);
            return EDEN_DSMOD_TRUE;
        }
        if (action == "apppick" || action == "pkt_next" || action == "pkt_prev" || action == "pkt_key" || action == "pkt_plus" ||
            action == "pkt_clear" || action == "pkt_coin" || action == "pkt_colour") {
            if (pkt_rows.empty()) return EDEN_DSMOD_FALSE;
            const int before = pkt_app;
            if (action == "apppick") {
                if (arg < 0 || arg >= static_cast<int64_t>(pkt_rows.size())) return EDEN_DSMOD_FALSE;
                pkt_app = pkt_rows[arg];
            } else if (action == "pkt_next") {
                const auto it = std::find(pkt_rows.begin(), pkt_rows.end(), pkt_app);
                pkt_app = it == pkt_rows.end() || it + 1 == pkt_rows.end() ? pkt_rows.front() : *(it + 1);
            } else if (action == "pkt_prev") {
                const auto it = std::find(pkt_rows.begin(), pkt_rows.end(), pkt_app);
                pkt_app = it == pkt_rows.end() || it == pkt_rows.begin() ? pkt_rows.back() : *(it - 1);
            } else if (action == "pkt_key" && pkt_app == lp_poketch::Calculator && arg >= 0 && arg <= 16) {
                pkt_calc.Press(static_cast<int>(arg));
            } else if (action == "pkt_plus" && pkt_app == lp_poketch::Counter) {
                pkt_counter = (pkt_counter + 1) % 10000;
            } else if (action == "pkt_clear" && pkt_app == lp_poketch::Pedometer) {
                if (!live.ResetPedometer()) return EDEN_DSMOD_FALSE;
                snap.steps = 0;
            } else if (action == "pkt_coin" && pkt_app == lp_poketch::CoinToss) {
                const auto now = std::chrono::steady_clock::now();
                if (lp_poketch::CoinFrame(std::chrono::duration_cast<std::chrono::milliseconds>(now - pkt_coin_at).count()) >= 0)
                    return EDEN_DSMOD_FALSE;
                pkt_coin = static_cast<int>((tick * 2654435761u >> 7) & 1);
                pkt_coin_at = now;
            } else if (action == "pkt_colour" && pkt_app == lp_poketch::ColorChanger && arg >= 0 && arg < 8) {
                pkt_colour = static_cast<int>(arg);
            } else {
                return EDEN_DSMOD_FALSE;
            }
            PoketchSound(pkt_app != before ? PktAppSwitch : action == "pkt_coin" ? PktCoin : PktClick);
            if (pkt_app != before) { pkt_hop_slot = -1; pkt_coin_at = {}; }
            if (pkt_app != before && before == lp_poketch::KitchenTimer) {
                // the native Kitchen Timer resets when the watch switches to another app
                timer_running = timer_alarm = false;
                timer_left_ms = timer_set_ms = 0;
            }
            return EDEN_DSMOD_TRUE;
        }
        if (action == "dex_page") {
            // 0 entry, 1 weaknesses, 2 stats, 3 evolution, 4 area; 10 / 11 = swipe to the next / previous page
            dex_page = arg == 10 ? (dex_page + 1) % DexPages : arg == 11 ? (dex_page + DexPages - 1) % DexPages
                                 : static_cast<int>(std::clamp<int64_t>(arg, 0, DexPages - 1));
            ++scene_revision;
            return EDEN_DSMOD_TRUE;
        }
        if (action == "dex_form" || action == "dex_evo" || action == "dex_area") {
            if (!snap.field || snap.in_battle || snap.is_battling || !snap.pokedex_acquired) return EDEN_DSMOD_FALSE;
            if (action == "dex_form") {
                // a form chip of the Entry page: the card shows that form
                if (arg < 0 || arg >= static_cast<int64_t>(dex_view.forms.size())) return EDEN_DSMOD_FALSE;
                dex_form = dex_view.forms[static_cast<std::size_t>(arg)];
            } else if (action == "dex_evo") {
                // an evolution member: select its species (and form) when it has been seen
                if (arg < 0 || arg >= static_cast<int64_t>(dex_view.chain.size())) return EDEN_DSMOD_FALSE;
                const auto& m = dex_view.chain[static_cast<std::size_t>(arg)];
                if (std::find(dex_rows.begin(), dex_rows.end(), m.species) == dex_rows.end()) return EDEN_DSMOD_FALSE;
                dex_species = m.species;
                dex_form = m.form;
                anim_dex.Hop(AnimNow());
            } else {
                // a place of the Area page: select it on the Town Map and open the Map
                if (!snap.map_acquired || arg < 0 || arg >= static_cast<int64_t>(dex_view.areas.size()))
                    return EDEN_DSMOD_FALSE;
                const int zone = dex_view.areas[static_cast<std::size_t>(arg)].zone;
                const auto cell = assets.ZoneCell(zone);
                if (!cell) return EDEN_DSMOD_FALSE;
                map_sel = cell;
                map_sel_zone = zone;
                const auto spot = assets.FlySpotAt(cell->first, cell->second);
                map_sel_name = assets.AreaName(spot ? spot->zone : zone);
                OpenFieldView(1);
            }
            ++scene_revision;
            return EDEN_DSMOD_TRUE;
        }
        if (action == "map_tap") return EDEN_DSMOD_TRUE; // the position is read on the next sample
        if (action == "field_waza") {
            // the selected member's field move `arg`: Soft-Boiled / Milk Drink pick who receives the
            // HP; Dig, Teleport, Flash, Sweet Scent and Defog run through the game itself
            const auto moves = FieldMovesOf(sel_party);
            if (!FieldFree() || arg < 0 || arg >= static_cast<int64_t>(moves.size())) return EDEN_DSMOD_FALSE;
            const int waza = moves[arg];
            if (waza == 135 || waza == 208) {
                field_waza_user = sel_party;
                field_waza_owner = snap.party[sel_party].mon;
                bag_popup_owner = live.PlayerWorkObject();
                bag_purpose = 1; bag_popup = 1; ++scene_revision;
                return EDEN_DSMOD_TRUE;
            }
            return UseFieldMove(waza) ? EDEN_DSMOD_TRUE : EDEN_DSMOD_FALSE;
        }
        if (action == "fly") return FlyToRoutePlace() ? EDEN_DSMOD_TRUE : EDEN_DSMOD_FALSE;
        if (action == "map_route" || action == "map_fly") {
            // the Map tab's buttons act on the selected place (Route Pokémon: where you are without one)
            if (map_sel) { route_zone = map_sel_zone; route_cell = map_sel; }
            else { route_zone = -1; route_cell.reset(); }
            if (action == "map_fly") return FlyToRoutePlace() ? EDEN_DSMOD_TRUE : EDEN_DSMOD_FALSE;
            field_view = 2; field_refresh_tick = tick + 8; ++scene_revision;
            return EDEN_DSMOD_TRUE;
        }
        if (action == "bag_use" || action == "bag_target" || action == "bag_move" || action == "bag_cancel" ||
            action == "bag_open")
            return BagAction(action, static_cast<int>(arg)) ? EDEN_DSMOD_TRUE : EDEN_DSMOD_FALSE;
        if (action == "fieldtab") {
            // 0 stats, 1 moves, 2 weaknesses; 10 / 11 = swipe to the next / previous page
            field_tab = arg == 10 ? (field_tab + 1) % 3 : arg == 11 ? (field_tab + 2) % 3
                                                                    : static_cast<int>(std::clamp<int64_t>(arg, 0, 2));
            field_refresh_tick = tick + 8;
            ++scene_revision;
            return EDEN_DSMOD_TRUE;
        }
        if (action.starts_with("swap") && action.size() == 5) {
            // drag party slot `arg` onto slot N ("swapN")
            const int to = action[4] - '0';
            if (AnimNow() - party_swap_at < lp_anim::PartySwapMs || arg == to ||
                field_view != 0 || bag_popup || !FieldFree() ||
                !live.SwapParty(static_cast<int>(arg), to))
                return EDEN_DSMOD_FALSE;
            party_swap_from = static_cast<int>(arg);
            party_swap_to = to;
            party_swap_at = AnimNow();
            sel_party = to;
            party_move = -1;
            Toast(S(K::party_reordered));
            return EDEN_DSMOD_TRUE;
        }
        if (!snap.in_battle)
            return EDEN_DSMOD_FALSE;
        if (drive.kind != Drive::Kind::None)
            return EDEN_DSMOD_FALSE; // one operation at a time
        Drive d;
        d.arg = static_cast<int>(arg);
        if (action == "targetconfirm" || action == "targetback") {
            if (pending_press != Btn::None || !live.CanTargetInput(action == "targetconfirm" ? -1 : -2))
                return EDEN_DSMOD_FALSE;
            Press(action == "targetconfirm" ? Btn::A : Btn::B);
            return EDEN_DSMOD_TRUE;
        } else if (action == "battlecontinue") {
            // One native A, with a fresh focus check: a stale rendered button cannot select
            // Fight, a double-battle target, a Bag item or a yes/no answer after dialogue ends.
            if (!live.CanContinueBattle() || pending_press != Btn::None)
                return EDEN_DSMOD_FALSE;
            Press(Btn::A);
            return EDEN_DSMOD_TRUE;
        } else if (action == "move") {
            // A tap submits this move once; native controller focus supplies the detail selection.
            if (arg < 0 || arg >= snap.front[0].waza_count || !(snap.action.focus || snap.waza.focus))
                return EDEN_DSMOD_FALSE;
            pick = static_cast<int>(arg);
            if (snap.front[0].pp[arg] <= 0)
                return EDEN_DSMOD_FALSE; // the game would refuse with a message; keep it quiet
            d.kind = Drive::Kind::Move;
        } else if (action == "command") {
            // Native order: 1 Pokémon, 2 Bag (game windows on the top screen), 3 Run
            if (arg < 1 || arg > 3 || arg < snap.action_min || arg > snap.action_max || !snap.action.focus)
                return EDEN_DSMOD_FALSE;
            d.kind = Drive::Kind::Command;
        } else if (action == "ballkey") {
            // X opens the native list off-screen; its focus drives the companion page
            if (!snap.action.focus || !snap.ball_enabled || snap.balls.empty())
                return EDEN_DSMOD_FALSE;
            Press(Btn::X);
            return EDEN_DSMOD_TRUE;
        } else if (action == "ballprev" || action == "ballnext") {
            if (!snap.ball.focus || snap.balls.empty())
                return EDEN_DSMOD_FALSE;
            const int n = static_cast<int>(snap.balls.size());
            const int index = (std::clamp(snap.ball.current, 0, n - 1) +
                               (action == "ballnext" ? 1 : n - 1)) % n;
            if (!live.SetBallIndex(index))
                return EDEN_DSMOD_FALSE;
            ++scene_revision;
            return EDEN_DSMOD_TRUE;
        } else if (action == "ballback") {
            if (!snap.ball.focus)
                return EDEN_DSMOD_FALSE;
            Press(Btn::B);
            return EDEN_DSMOD_TRUE;
        } else if (action == "balluse") {
            if (!snap.ball_enabled || !snap.ball.focus || snap.ball.current < 0 ||
                snap.ball.current >= static_cast<int>(snap.balls.size()))
                return EDEN_DSMOD_FALSE;
            d.kind = Drive::Kind::Ball;
            d.arg = snap.balls[snap.ball.current]; // item identity, not a stale list index
        } else {
            return EDEN_DSMOD_FALSE;
        }
        d.step_tick = tick;
        d.result = drive.result;
        drive = d;
        return EDEN_DSMOD_TRUE;
    }

    EdenDsmodBool LoadImage(const EdenDsmodHostApi* image_host, const char* key, void* receiver,
                            EdenDsmodImageSink sink) {
        if (!image_host || !key || !sink)
            return EDEN_DSMOD_FALSE;
        auto img = assets.Image(key);
        if (!img || img->rgba.empty())
            return EDEN_DSMOD_FALSE;
        sink(receiver, img->w, img->h, img->rgba.data(), img->rgba.size());
        if (std::string_view{key}.starts_with("module:lp:poketch")) pkt_frames.Ready(key);
        return EDEN_DSMOD_TRUE;
    }

    EdenDsmodBool DecodeFont(const u8* bytes, size_t size, void* receiver, EdenDsmodFontSink sink) {
        if (!bytes || !sink)
            return EDEN_DSMOD_FALSE;
        auto font = assets.FontMetrics({reinterpret_cast<const char*>(bytes), size});
        if (!font || font->glyphs.empty())
            return EDEN_DSMOD_FALSE;
        sink(receiver, font->line_height, font->first_codepoint, font->glyphs.data(),
             static_cast<u32>(font->glyphs.size()));
        // keep the metrics the runtime draws with: scroll lengths are measured with them (lp_text.h)
        std::lock_guard lock{text_font_mutex};
        text_font = *font;
        text_measures.clear();
        ++text_font_rev;
        return EDEN_DSMOD_TRUE;
    }

private:
    // The accessors read through `self->host`, the copy UpdateHost refreshes every sample.
    static lp_live::Guest MakeGuest(Module* self) {
        lp_live::Guest g;
        g.main_base = self->host.main_base;
        g.read = [self](u64 a, void* out, size_t n) {
            const auto& h = self->host;
            return h.read_memory && h.read_memory(h.userdata, a, out, n) != 0;
        };
        g.batch = [self](const std::vector<lp_live::BatchOp>& ops) { return self->Batch(ops); };
        g.write = [self](u64 a, const void* in, size_t n) {
            const auto& h = self->host;
            return h.write_memory && h.write_memory(h.userdata, a, in, n) != 0;
        };
        return g;
    }

private:
    // Literal names publish without building a std::string (no heap allocation per key).
    void I64(const char* name, int64_t v) const {
        if (host.publish_i64)
            host.publish_i64(host.userdata, name, v);
    }
    void I64(const std::string& name, int64_t v) const {
        I64(name.c_str(), v);
    }
    void Text(const char* name, const std::string& v) const {
        // text beyond ASCII may need glyphs the font atlas lacks (nicknames, other languages)
        if (std::any_of(v.begin(), v.end(), [](char c) { return static_cast<unsigned char>(c) >= 0x80; }))
            assets.NoteText(v);
        if (host.publish_text)
            host.publish_text(host.userdata, name, v.c_str());
    }
    void Text(const std::string& name, const std::string& v) const {
        Text(name.c_str(), v);
    }
    void PublishMoveRow(const std::string& p, int waza, int pp, int pp_max) {
        const auto info = assets.Waza(waza);
        I64(p + "id", waza);
        I64(p + "pp", pp);
        I64(p + "ppmax", pp_max);
        I64(p + "type", info ? info->type : -1);
        Text(p + "name", waza > 0 ? assets.WazaName(waza) : std::string{});
        Text(p + "tag", info ? TypeTag(info->type) : std::string{});
    }

    // `chart`: also publish the defensive type chart (weak* / immune*; the selected member and the foe).
    void PublishMon(const std::string& p, u16 species, u8 form, u8 gender, int level, int hp, int hp_max,
                    int item, bool shiny, const std::string& nick, bool chart, bool egg = false) {
        I64(p + "egg", egg);
        I64(p + "details", !egg);
        I64(p + "level", level);
        I64(p + "hp", hp);
        I64(p + "hpmax", hp_max);
        const int zone = HpZone(hp, hp_max); // the game's three gauge colours
        I64(p + "hpg", zone == 3 ? 1 : 0);
        I64(p + "hpy", zone == 2 ? 1 : 0);
        I64(p + "hpr", zone == 1 ? 1 : 0);
        Text(p + "gendericon", gender < 2 ? lp_assets::UiKey("sharedui", gender == 0 ? "cmn_icon_sex_01_00"
                                                                                   : "cmn_icon_sex_01_01")
                                          : std::string{});
        Text(p + "name", egg ? S(K::party_egg) : !nick.empty() ? nick : assets.SpeciesName(species));
        Text(p + "icon", egg ? lp_assets::UiKey("texturemass", "pm0000_00_00_00") : lp_assets::IconKey(species, form, gender, shiny));
        Text(p + "itemicon", item > 0 ? lp_assets::ItemKey(item) : std::string{});
        const auto types = egg ? std::nullopt : assets.SpeciesTypes(species, form);
        Text(p + "tag1", types ? TypeTag(types->first) : std::string{});
        Text(p + "tag2", types && types->second != types->first ? TypeTag(types->second)
                                                                   : std::string{});
        if (!chart)
            return;
        // the party card's Weak. tab
        PublishChart(p, "immune", types, PartyWeakTop, PartyWeakRect, 2);
        I64(p + "weak.unavailable", !types);
    }

    // A defensive type chart (the party card's and the Pokédex's Weak. tab) under `p`: weak<i>. /
    // res<i>. (tag, factor, strong: 4× drawn bold, ¼× is not) and <imm><i>.tag (the No effect rows:
    // "immune" on the party card, "imm" on the Pokédex), each with .count and .none. Abilities, held
    // items and battle effects are not applied. Resists / No effect follow the rows above them
    // (imm.dy / res.dy: offsets from the manifest's one-row-each layout, `cols` tags a row) and
    // weak.rows is the scroll length to the last row (top / rect: WeakGridAt's top, the scroll rect).
    void PublishChart(const std::string& p, const std::string& imm, const std::optional<std::pair<int, int>>& types,
                      int top, int rect, int cols) {
        int weak = 0, resist = 0, immune = 0;
        if (types)
            for (int attack = 0; attack < 18; ++attack) {
                const int f = lp_assets::Effectiveness(attack, types->first, types->second);
                if (f > 4) {
                    const auto row = p + "weak" + std::to_string(weak++) + ".";
                    Text(row + "tag", TypeTag(attack));
                    Text(row + "factor", f == 16 ? "4×" : "2×");
                    I64(row + "strong", f == 16);
                } else if (f == 0) {
                    Text(p + imm + std::to_string(immune++) + ".tag", TypeTag(attack));
                } else if (f < 4) {
                    const auto row = p + "res" + std::to_string(resist++) + ".";
                    Text(row + "tag", TypeTag(attack));
                    Text(row + "factor", f == 1 ? "¼×" : "½×");
                    I64(row + "strong", 0);
                }
            }
        I64(p + "weak.count", weak);
        I64(p + "res.count", resist);
        I64(p + imm + ".count", immune);
        I64(p + "weak.none", types && weak == 0);
        I64(p + "res.none", types && resist == 0);
        I64(p + imm + ".none", types && immune == 0);
        const auto at = WeakGridAt(top, weak, resist, immune, cols), ref = WeakGridAt(top, 1, 1, 1, cols);
        I64(p + "res.dy", at.res_y - ref.res_y);
        I64(p + "imm.dy", at.imm_y - ref.imm_y);
        I64(p + "weak.rows", (at.end + WeakPad - rect + 9) / 10);
    }

    // The independent Pokétch: the companion's own app choice and app state, drawn through
    // lp_poketch from the game's art. Available apps follow the save's app flags; the game's watch
    // is never switched or read from the screen.
    // A touch at LCD pixel (x, y) (640x480, y down): the native control of the current app under it,
    // turned into that app's action (prefab positions, contracts/poketch-apps.md).
    bool LcdTouch(int x, int y) {
        namespace pk = lp_poketch;
        if (field_view != 3 || !snap.field || !snap.player_ok || snap.in_battle || snap.is_battling ||
            !snap.poketch_acquired || x < 0 || x >= pk::Width || y < 0 || y >= pk::Height) return false;
        const auto in = [&](int cx, int cy, int w, int h) { return std::abs(x - cx) * 2 <= w && std::abs(y - cy) * 2 <= h; };
        const auto act = [&](std::string_view a, int64_t v) { return OnAction(a, v) == EDEN_DSMOD_TRUE; };
        const auto hop = [&](int slot) {
            pkt_hop_slot = slot;
            pkt_hop_at = std::chrono::steady_clock::now();
            PoketchSound(PktClick);
            return true;
        };
        switch (pkt_app) {
        case pk::PokemonList: case pk::Friendship:
            for (int i = 0; i < snap.party_count && i < 6; ++i) {
                if (!snap.party[i].valid || snap.party[i].mon.is_egg) continue;
                const bool list = pkt_app == pk::PokemonList;
                const int cx = list ? 92 + (i % 2 ? 356 : 100) - 4 : 320 + (i % 2 ? 104 : -104);
                const int cy = list ? 77 + (i / 2) * 154 - 14 : 100 + (i / 2) * 146 - 20;
                if (in(cx, cy, list ? 112 : 140, list ? 112 : 140)) return hop(i);
            }
            return false;
        case pk::EggMonitor:
            for (int i = 0; i < 2; ++i)
                if (snap.daycare[i].valid && !snap.daycare[i].mon.is_egg && in(i ? 480 : 160, 300, 150, 150)) return hop(i);
            return snap.egg_exists && in(320, 380, 110, 110) && hop(2);
        case pk::History:
            for (int i = 0; i < 12; ++i)
                if (snap.history[i][0] > 0 && in(80 + 160 * (i % 4), 108 + 148 * (i / 4), 128, 128)) return hop(i);
            return false;
        case pk::Calculator:
            for (const auto& k : pk::CalcKeys)
                if (in(320 + k.x, 284 - k.y, k.w, 88)) return act("pkt_key", k.code);
            return false;
        case pk::Pedometer: return in(320, 343, 196, 166) && act("pkt_clear", 0);
        case pk::Counter: return in(320, 334, 196, 166) && act("pkt_plus", 0);
        case pk::CoinToss: return act("pkt_coin", 0);
        case pk::ColorChanger:
            for (int k = 0; k < 8; ++k)
                if (in(320 - 182 + k * 52, 404, 52, 112)) return act("pkt_colour", k);
            return false;
        case pk::MemoPad:
            if (x < 510 && y < 450) return act("pkt_memo", (y / pk::MemoCell) * pk::MemoCols + x / pk::MemoCell);
            if (in(575, 122, 96, 220)) return act("pkt_tool", 1);
            if (in(575, 358, 96, 220)) return act("pkt_tool", 0);
            return false;
        case pk::Dowsing:
            if (y < 60 || y >= 420) return false;
            StartDowsing(x, y);
            return true;
        case pk::DotArtist: return act("pkt_dot", (y / 20) * 32 + x / 20);
        case pk::HiddenMoves: {
            // buttons in two columns, rows at y 171 + 78 k; Fly picks its town on the Map tab
            static constexpr int Type[8] = {0, 1, 5, 2, 3, 4, 6, 7};
            const int row = (y - 132) / 78, b = row * 2 + (x < 320 ? 0 : 1);
            if (y < 132 || row > 3) return false;
            if (snap.hidden_moves[b] != 2 || !guest.Present()) {
                Toast(S(K::toast_cant_now));
                PoketchSound(PktError);
                return false;
            }
            if (Type[b] == 2) {
                if (!snap.map_acquired) { Toast(S(K::toast_cant_now)); return false; }
                field_view = 1; pkt_full = false; field_refresh_tick = tick + 8;
                Toast(S(K::toast_fly_hint));
                return true;
            }
            return UseHiddenMove(Type[b]);
        }
        case pk::MarkingMap: return act("pkt_mkmove", (y / 20) * 32 + x / 20);
        case pk::Roulette:
            if (in(578, 158, 96, 108)) return act("pkt_roul", 0);
            if (in(578, 294, 96, 108)) return act("pkt_roul", 1);
            if (in(578, 430, 96, 108)) return act("pkt_roul", 2);
            if (x >= 26 && x < 486 && y >= 10 && y < 470)
                return act("pkt_rdraw", ((y - 10) / pk::WheelCell) * pk::WheelCols + (x - 26) / pk::WheelCell);
            return false;
        case pk::KitchenTimer: {
            static constexpr int Offsets[4] = {-96, -46, 46, 96};
            for (int k = 0; k < 4; ++k) {
                if (in(320 + Offsets[k], 222, 50, 56)) return act("pkt_timer", 10 + k);
                if (in(320 + Offsets[k], 354, 50, 56)) return act("pkt_timer", 20 + k);
            }
            return y >= 392 && act("pkt_timer", x < 213 ? 0 : x < 427 ? 1 : 2);
        }
        default: return false;
        }
    }

    // Native uint event IDs (AK.EVENTS), through the same field-thread mailbox as
    // field moves. Never queue audio behind gameplay actions or initialize native UI.
    static constexpr u32 PktClick = 235681195, PktError = 235681193,
        PktAlarm = 235681192, PktCoin = 235681190, PktAppSwitch = 218903615,
        PktZoomIn = 393955717, PktZoomOut = 2435049356;
    void PoketchSound(u32 event) {
        const auto now = std::chrono::steady_clock::now();
        if (!snap.field || !snap.player_ok || snap.in_battle || snap.is_battling ||
            !snap.poketch_acquired || field_view != 3 || !guest.Alive() || guest.Busy() ||
            now - pkt_sound_at < std::chrono::milliseconds{60}) return;
        const auto audio = live.AudioManagerObject();
        if (!audio) return;
        auto step = Call(lp_profile::Active.AudioPlaySe, [this] { return live.AudioManagerObject(); },
            [event](u64 instance) { return std::array<u64,4>{instance,event,0,0}; }, [](u64 result) { return result != 0; }, "");
        if (guest.Run({std::move(step)}, [this](bool ok, const std::string&) {
                ++pkt_sound_done; pkt_sound_ok = ok;
            })) {
            pkt_sound_at = now; ++pkt_sound_issued; pkt_sound_event = event;
        }
    }

    // Touch on the drawing / timer apps (cells and buttons from LcdTouch's hit test).
    bool PoketchTouch(std::string_view action, int arg) {
        if (action == "pkt_tool" && pkt_app == lp_poketch::MemoPad && (arg == 0 || arg == 1)) {
            memo_tool = arg;
        } else if (action == "pkt_memo" && pkt_app == lp_poketch::MemoPad &&
                   arg >= 0 && arg < lp_poketch::MemoCols * lp_poketch::MemoRows) {
            memo.resize(lp_poketch::MemoCols * lp_poketch::MemoRows);
            const int cx = arg % lp_poketch::MemoCols, cy = arg / lp_poketch::MemoCols, r = memo_tool ? 1 : 0;
            for (int y = cy - r; y <= cy + r; ++y)
                for (int x = cx - r; x <= cx + r; ++x)
                    if (x >= 0 && y >= 0 && x < lp_poketch::MemoCols && y < lp_poketch::MemoRows)
                        memo[y * lp_poketch::MemoCols + x] = memo_tool ? 0 : 1;
            lp_poketch::SetSurface(lp_poketch::MemoSurface, memo);
            ++memo_rev;
        } else if (action == "pkt_rdraw" && pkt_app == lp_poketch::Roulette &&
                   arg >= 0 && arg < lp_poketch::WheelCols * lp_poketch::WheelRows) {
            wheel.resize(lp_poketch::WheelCols * lp_poketch::WheelRows);
            wheel[arg] = 1;
            lp_poketch::SetSurface(lp_poketch::RouletteSurface, wheel);
            ++roul_rev;
        } else if (action == "pkt_roul" && pkt_app == lp_poketch::Roulette && arg >= 0 && arg <= 2) {
            if (arg == 0 && roul_state == 0) { roul_state = 1; roul_speed = 24.0; }
            else if (arg == 1 && roul_state == 1) roul_state = 2;
            else if (arg == 2 && roul_state == 0) {
                wheel.assign(lp_poketch::WheelCols * lp_poketch::WheelRows, 0);
                lp_poketch::SetSurface(lp_poketch::RouletteSurface, wheel);
                ++roul_rev;
            } else return false;
        } else if (action == "pkt_timer" && pkt_app == lp_poketch::KitchenTimer) {
            const bool error = (arg == 1 && !timer_running && !timer_alarm) ||
                               (arg == 2 && !timer_running && !timer_alarm && timer_left_ms == 0);
            if (arg == 0 && !timer_running && !timer_alarm && timer_left_ms > 0) {
                timer_running = true; timer_last = std::chrono::steady_clock::now();
            } else if (arg == 1) {
                if (timer_alarm) { timer_alarm = false; timer_left_ms = timer_set_ms; }
                timer_running = false;
            } else if (arg == 2) {
                timer_running = false; timer_alarm = false; timer_left_ms = timer_set_ms = 0;
            } else if ((arg / 10 == 1 || arg / 10 == 2) && arg % 10 < 4 && !timer_running && !timer_alarm) {
                // digits mm:ss, each wraps on its own like the native arrows
                int left = static_cast<int>((timer_left_ms + 999) / 1000); // as displayed (rounded up)
                int d[4] = {left / 600 % 10, left / 60 % 10, left % 60 / 10, left % 10};
                const int k = arg % 10, top = k == 2 ? 6 : 10;
                d[k] = (d[k] + (arg / 10 == 1 ? 1 : top - 1)) % top;
                left = (d[0] * 10 + d[1]) * 60 + d[2] * 10 + d[3];
                timer_left_ms = timer_set_ms = left * 1000LL;
            } else return false;
            PoketchSound(error ? PktError : PktClick);
        } else {
            return false;
        }
        return true;
    }

    // The native sonar (FieldPoketch.Dowsing): the tap picks a tile of the 18x14 window around
    // the player; items within the rounded diamond around it count, and are pinpointed when their
    // level is within the item's own dowsing level.
    void StartDowsing(int x, int y) {
        static constexpr int Level[5][5] = {{1, 2, 2, 3, 3}, {2, 2, 2, 3, 3}, {2, 2, 3, 3, 0}, {3, 3, 3, 0, 0}, {3, 3, 0, 0, 0}};
        dowse_markers.clear();
        dowse_found = false;
        if (!snap.position_ok) return;
        const int ltx = snap.grid_x - 8, lty = snap.grid_y - 6, rbx = ltx + 17, rby = lty + 13;
        const double nx = x / 640.0, ny = std::max(0.0, (y - 60) / 360.0 - 0.07);
        const int cx = ltx + std::min(static_cast<int>(nx * 17), 17), cy = lty + std::min(static_cast<int>(ny * 13), 13);
        for (const auto& it : live.HiddenItems()) {
            if (it.grid_x < ltx || it.grid_y < lty || it.grid_x > rbx || it.grid_y > rby) continue;
            const int dx = std::abs(it.grid_x - cx), dy = std::abs(it.grid_y - cy);
            if (dx >= 5 || dy >= 5 || Level[dy][dx] == 0) continue;
            dowse_found = true;
            if (Level[dy][dx] > it.dowsing || dowse_markers.size() >= 8) continue;
            const double px = (it.grid_x - ltx + 1) / 18.0, py = (it.grid_y - lty) / 12.0;
            dowse_markers.push_back({320 + static_cast<int>(std::ceil((px - 0.5) * 100) / 100 * 640),
                                     240 - static_cast<int>(((1 - py) - 0.5 - 0.07) * 360)});
        }
        dowse_x = x; dowse_y = y; dowse_at = std::chrono::steady_clock::now(); dowse_active = true;
        dowse_gx = snap.grid_x; dowse_gy = snap.grid_y;
    }

    // Stop the LCD sonar when the player moves or an empty search finishes.
    void UpdateDowsing() {
        if (dowse_active && (snap.grid_x != dowse_gx || snap.grid_y != dowse_gy)) dowse_active = false;
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - dowse_at).count();
        if (dowse_active && secs >= 2.0 && !dowse_found) dowse_active = false;
    }

    // Open the game's Bag: X (the menu opens on Bag), then A once the menu is up.
    bool FieldContext() const {
        return snap.field && snap.player_ok && !snap.in_battle && !snap.is_battling;
    }
    void StepBagOpen() {
        using lp_field_actions::Input;
        const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - open_at).count();
        switch (lp_field_actions::BagStep(open_state, age, FieldContext() && field_view == 5,
                                         FieldFree(), snap.menu_open)) {
        case Input::OpenMenu: Press(Btn::X); open_state = 2; break;
        case Input::Accept: Press(Btn::A); open_state = 0; break;
        case Input::Cancel: open_state = 0; break;
        default: break;
        }
    }

    void RestoreShortcut() {
        if (!key_state) return;
        // Never apply an old save's shortcut restoration to a different PlayerWork.
        if (key_owner && live.PlayerWorkObject() == key_owner && key_restore != key_item &&
            !live.SetShortcut(key_slot, key_item, key_restore)) {
            const auto current = live.Shortcuts();
            if (key_slot >= 0 && key_slot < static_cast<int>(current.size()) && current[key_slot] == key_item) {
                key_state = 4; // restore only: retry a transient failure without replaying input
                return;
            }
        }
        key_state = 0;
        key_owner = 0;
    }
    // Registered-item native wheel: delayed inputs stop on field loss; restoration
    // also runs if sampling stops or the module unloads midway through the sequence.
    void StepKeyItem() {
        using lp_field_actions::Input;
        const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - key_at).count();
        const bool same_field = FieldContext() && key_owner && live.PlayerWorkObject() == key_owner;
        static constexpr Btn Dir[4] = {Btn::DUp, Btn::DDown, Btn::DLeft, Btn::DRight};
        switch (lp_field_actions::ShortcutStep(key_state, age, same_field, FieldFree())) {
        case Input::Wheel: Press(Btn::Plus); key_state = 2; break;
        case Input::Direction: Press(Dir[key_slot]); key_state = 3; break;
        case Input::Restore: RestoreShortcut(); break;
        default: break;
        }
    }

    // Per-sample motion: the roulette arrow (spin, then ease to a stop) and the timer countdown.
    void StepPoketch() {
        if (roul_state == 1) roul_angle += roul_speed;
        else if (roul_state == 2) {
            roul_speed *= 0.965;
            roul_angle += roul_speed;
            if (roul_speed < 0.25) roul_state = 0;
        }
        roul_angle = std::fmod(roul_angle, 360.0);
        if (timer_running) {
            const auto now = std::chrono::steady_clock::now();
            timer_left_ms -= std::chrono::duration_cast<std::chrono::milliseconds>(now - timer_last).count();
            timer_last = now;
            if (timer_left_ms <= 0) {
                timer_left_ms = 0; timer_running = false; timer_alarm = true; timer_alarm_tick = tick;
            }
        }
        if (timer_alarm && field_view == 3 && pkt_app == lp_poketch::KitchenTimer) {
            const auto now = std::chrono::steady_clock::now();
            if (now - pkt_alarm_at >= std::chrono::milliseconds{300}) {
                pkt_alarm_at = now;
                PoketchSound(PktAlarm);
            }
        }
    }

    // A tap on the Town Map (runtime map widget, world = picture px with y up, 1266x732): open
    // Route Pokémon for the place under it.
    void PollMapTap() {
        if (!host.get_i64 || !host.get_f64) return;
        // @map_tap_seq exists only after a tap (1, 2, ...); unchanged = nothing new
        const int64_t seq = host.get_i64(host.userdata, "@map_tap_seq", 0);
        if (seq <= 0 || seq == last_map_tap_seq) return;
        last_map_tap_seq = seq;
        if (field_view != 1) return;
        const double px = host.get_f64(host.userdata, "@map_tap_x", -1), py = MapPicH - host.get_f64(host.userdata, "@map_tap_y", -1);
        auto [cx, cy] = CellAt(px, py);
        // selects the place (a name cloud over it; Route Pokémon / Fly act on it); the same place
        // again keeps the selection. A town Fly can reach names the town; otherwise a place with
        // wild Pokémon first. A tap just beside a place snaps to the nearest one within about two
        // cells at the base view (fewer as you zoom in: ceil(2 / zoom)); only a tap with nothing
        // in reach clears the selection.
        auto zones = assets.ZonesAtCell(cx, cy);
        if (zones.empty()) {
            const double zoom = host.get_f64(host.userdata, "@map_view_ppw:townmap", MapBasePpw) / MapBasePpw;
            const auto snapped = NearestMapCell(px, py, static_cast<int>(std::ceil(2.0 / std::max(zoom, 1.0))));
            if (snapped) {
                cx = snapped->first; cy = snapped->second;
                zones = assets.ZonesAtCell(cx, cy);
            }
        }
        if (zones.empty()) {
            map_sel.reset();
        } else {
            const auto spot = assets.FlySpotAt(cx, cy);
            const auto wild = std::find_if(zones.begin(), zones.end(), [&](int z) { return !assets.Encounters(z).empty(); });
            map_sel_zone = wild != zones.end() ? *wild : zones.front();
            map_sel_name = assets.AreaName(spot ? spot->zone : map_sel_zone);
            map_sel = std::pair{cx, cy};
        }
        ++scene_revision;
    }

    // The Town Map cell with places nearest to picture point (px, py) (picture px, y down, as in
    // PollMapTap), at most `reach` cells away in x and y; distance to the cell's centre.
    std::optional<std::pair<int, int>> NearestMapCell(double px, double py, int reach) const {
        const auto [tx, ty] = CellAt(px, py);
        std::optional<std::pair<int, int>> best;
        double best_d = 0;
        for (int y = ty - reach; y <= ty + reach; ++y)
            for (int x = tx - reach; x <= tx + reach; ++x) {
                const auto [mx, my] = CellCentre(x, y);
                const double dx = mx - px, dy = my - py, d = dx * dx + dy * dy;
                if ((!best || d < best_d) && !assets.ZonesAtCell(x, y).empty()) {
                    best = std::pair{x, y};
                    best_d = d;
                }
            }
        return best;
    }

    // Bag Use: medicine on a party member (the verified UseMedicine write batch), a pop-up picks the
    // member and, for single-move PP items, the move.
    // How the companion uses an item from the Bag (contracts/field-item-move-uses.md 2):
    enum class BagUse { None, Medicine, SacredAsh, PpUp, Repel, KeyItem, FieldItem };
    // Key items the game lets you register for the shortcut (flags0 bit 2): Explorer Kit, Poké Radar,
    // Point Card, Guidebook, Vs. Seeker, rods, Sprayduck, Poffin Case, Bike, DS Sounds.
    static bool ShortcutItem(int item) {
        static constexpr int Items[] = {428, 431, 432, 433, 443, 445, 446, 447, 448, 449, 450, 1822};
        return std::find(std::begin(Items), std::end(Items), item) != std::end(Items);
    }
    // LP hooks shortcut availability and use for these items. The guest path's earlier
    // vanilla UI_onUseFieldItem precheck can reject them; the registered-item wheel uses
    // LP's availability hook and preserves its own event, unlock checks, audio and dialogs.
    bool CustomShortcutItem(int item) const {
        return lumi == 1 && (item == 126 || item == 1301 || item == 1303);
    }
    BagUse BagUseKind(int item) const {
        const auto d = assets.Item(item);
        if (!d) return BagUse::None;
        if (item == 76 || item == 77 || item == 79) return BagUse::Repel;
        if (ShortcutItem(item) || CustomShortcutItem(item)) return BagUse::KeyItem;
        // Escape Rope / Honey: the game's own field use, through the guest mailbox
        if (item == 78 || item == 94) return guest.Present() ? BagUse::FieldItem : BagUse::None;
        if (d->field_func != 1 && d->field_func != 12) return BagUse::None;
        if (d->flags & (1u << 23)) return BagUse::SacredAsh;              // revives every fainted member
        if (d->flags & ((1u << 12) | (1u << 13))) return BagUse::PpUp;    // PP Up / PP Max
        const bool medicine = (d->hp_rcv > 0 || d->pp_rcv > 0 || (d->flags & 0xf8000)) &&
            std::all_of(d->friend_change.begin(), d->friend_change.end(), [](int n) { return n <= 0; });
        return medicine ? BagUse::Medicine : BagUse::None;
    }
    bool BagUsable(int item) const { return BagUseKind(item) != BagUse::None; }
    // Items only the game's own Bag can use (Rare Candy, evolution stones, TMs, vitamins, mints...):
    // the companion opens that Bag on the item instead.
    bool BagOpenable(int item) const {
        const auto d = assets.Item(item);
        // pockets 0-6 only (Treasures is 6; lp_vanilla::BagFieldPockets): not Foods 7, Key Items 8 or None 9
        return d && !BagUsable(item) && d->pocket >= 0 && d->pocket < 7 &&
               (d->field_func != 0 || d->pocket == 5);
    }

    // Free on the field: the player is on the map, no battle, no native menu open.
    bool FieldFree() const {
        return snap.field && snap.player_ok && !snap.in_battle && !snap.is_battling && !snap.menu_open && !snap.demo_active;
    }

    // Where Marking Map marker k sits (LCD px): its own spot, or its tray slot while unplaced.
    std::pair<int, int> MarkPos(int k) const {
        const bool tray = marks[k].first <= 0 || marks[k].second <= 0;
        return tray ? std::pair<int, int>{365 + 51 * k, 455} : marks[k];
    }

    // Keeps the session's bag counts fresh (every 30 ticks; a use resets bag_tick).
    void RefreshBag() {
        if (bag.empty() || tick >= bag_tick) {
            auto counts = live.BagCounts();
            if (counts != bag) { bag = std::move(counts); ++bag_revision; }
            bag_tick = tick + 30;
        }
    }

    // Max PP of move slot k with `ups` PP Ups (Gen 8: base * (5 + ups) / 5).
    int MaxPpFor(const lp_pk8::Mon& mon, int k, int ups) const {
        const auto mv = assets.Waza(mon.moves[k]);
        return mv ? lp_pk8::MaxPp(mv->base_pp, ups) : 0;
    }

    void Toast(std::string text) {
        bag_toast = std::move(text);
        bag_toast_at = std::chrono::steady_clock::now();
        bag_tick = 0;
        ++scene_revision;
    }

    bool SameMoveLayout(const lp_pk8::Mon& expected, const lp_pk8::Mon& current) const {
        return lp_pk8::SameIdentity(expected, current) && expected.moves == current.moves &&
               expected.pp == current.pp && expected.pp_ups == current.pp_ups;
    }

    void ValidateBagPopup() {
        if (!bag_popup) return;
        bool valid = FieldFree() && bag_popup_owner && bag_popup_owner == live.PlayerWorkObject();
        if (valid && bag_purpose == 1)
            valid = field_waza_user >= 0 && field_waza_user < snap.party_count &&
                snap.party[field_waza_user].valid &&
                SameMoveLayout(field_waza_owner, snap.party[field_waza_user].mon);
        if (valid && bag_popup == 2)
            valid = bag_target >= 0 && bag_target < snap.party_count && snap.party[bag_target].valid &&
                !snap.party[bag_target].mon.is_egg && SameMoveLayout(bag_target_owner, snap.party[bag_target].mon);
        if (!valid) { bag_popup = 0; ++scene_revision; }
    }

    bool BagAction(std::string_view action, int arg) {
        ValidateBagPopup();
        if ((action == "bag_use" || action == "bag_open") && field_view != 5) return false;
        const int item = bag_selected >= 0 && bag_selected < static_cast<int>(bag_rows.size())
            ? bag_rows[bag_selected] : 0;
        const bool field_ok = FieldFree();
        if (action == "bag_cancel") { bag_popup = 0; ++scene_revision; return true; }
        if (action == "bag_open") {
            // the game's own Bag (X menu -> Bag) with this item highlighted: write the Bag's cursor
            // memory and the X menu's selection, then press X and A
            const auto d = assets.Item(item);
            if (!field_ok || !d || !BagOpenable(item) || open_state != 0 || key_state != 0 || snap.poketch_large)
                return false;
            if (!live.SetBagCursor(d->pocket, item) || !live.SelectFieldMenu(2)) {
                Toast(S(K::toast_cant_open));
                return false;
            }
            open_state = 1; open_at = std::chrono::steady_clock::now();
            Toast(S(K::toast_opening_bag));
            return true;
        }
        if (action == "bag_use") {
            if (!field_ok || !item || !BagUsable(item)) return false;
            bag_item = item;
            switch (BagUseKind(item)) {
            case BagUse::Repel: {
                static constexpr std::pair<int, int> Repel[3] = {{20, 1}, {40, 2}, {50, 3}}; // Repel, Super, Max
                const auto [units, type] = Repel[item == 79 ? 0 : item == 76 ? 1 : 2];
                if (snap.repel_units > 0) { Toast(S(K::toast_repel_active)); return false; }
                const bool ok = live.UseRepel(item, units, type);
                Toast(ok ? F(K::toast_repel_used, {{"item", assets.ItemName(item)}}) : S(K::toast_cant_now));
                return ok;
            }
            case BagUse::FieldItem:
                return UseFieldItem(item);
            case BagUse::KeyItem: {
                if (guest.Alive() && !CustomShortcutItem(item)) return UseFieldItem(item);
                // Without the guest stub: the game's shortcut wheel. Register the item in the slot that
                // already holds it (else slot 0), press +, then that slot's direction (0 up, 1 down,
                // 2 left, 3 right), then restore the player's item (StepKeyItem).
                const auto owner = live.PlayerWorkObject();
                const auto slots = live.Shortcuts();
                if (!owner || slots.size() < 4 || key_state != 0 || open_state != 0) {
                    Toast(S(K::toast_cant_now)); return false;
                }
                const auto it = std::find(slots.begin(), slots.begin() + 4, item);
                key_slot = it != slots.begin() + 4 ? static_cast<int>(it - slots.begin()) : 0;
                key_restore = slots[key_slot];
                if (slots[key_slot] != item && !live.SetShortcut(key_slot, slots[key_slot], item)) {
                    Toast(S(K::toast_cant_now)); return false;
                }
                key_owner = owner; key_item = item; key_state = 1; key_at = std::chrono::steady_clock::now();
                Toast(F(K::toast_using, {{"item", assets.ItemName(item)}}));
                return true;
            }
            case BagUse::SacredAsh: {
                std::vector<std::pair<int, lp_live::Reader::MonEdit>> edits;
                for (int i = 0; i < snap.party_count; ++i)
                    if (snap.party[i].valid && !snap.party[i].mon.is_egg && snap.party[i].mon.hp == 0)
                        edits.emplace_back(i, [expected = snap.party[i].mon](std::array<u8, lp_pk8::CoreSize>& p, const lp_pk8::Mon& mon) {
                            if (!lp_pk8::SameIdentity(expected, mon) || mon.is_egg || mon.hp != 0 || mon.hp_max == 0)
                                return false;
                            p[0x8A] = static_cast<u8>(mon.hp_max); p[0x8B] = static_cast<u8>(mon.hp_max >> 8);
                            std::fill_n(p.data() + 0x94, 4, 0);
                            return true;
                        });
                const bool ok = !edits.empty() && live.EditParty(edits, item);
                Toast(ok ? S(K::toast_revived) : S(K::toast_no_effect));
                return ok;
            }
            default:
                bag_popup_owner = live.PlayerWorkObject();
                bag_popup = 1; bag_purpose = 0; ++scene_revision;
                return true;
            }
        }
        if (action == "bag_target") {
            if (!field_ok || bag_popup != 1 || arg < 0 || arg >= snap.party_count || !snap.party[arg].valid || snap.party[arg].mon.is_egg) return false;
            bag_target = arg;
            bag_target_owner = snap.party[arg].mon;
            if (bag_purpose == 1) return ApplyFieldWaza(arg); // Soft-Boiled / Milk Drink target
            if (BagUseKind(bag_item) == BagUse::PpUp) { bag_popup = 2; ++scene_revision; return true; }
            const auto d = assets.Item(bag_item);
            // single-move PP items (flag bit 10: Ether / Max Ether) pick a move first; the rest apply at once
            if (d && d->pp_rcv > 0 && (d->flags & (1u << 10))) { bag_popup = 2; ++scene_revision; return true; }
            return ApplyBagItem(0);
        }
        if (action == "bag_move") {
            if (!field_ok || bag_popup != 2 || arg < 0 || arg >= 4) return false;
            return BagUseKind(bag_item) == BagUse::PpUp ? ApplyPpUp(arg) : ApplyBagItem(arg);
        }
        return false;
    }

    // PP Up (+1 up) / PP Max (to 3 ups): the max PP grows and current PP by the same amount.
    bool ApplyPpUp(int k) {
        const auto d = assets.Item(bag_item);
        if (!d || bag_target < 0 || bag_target >= snap.party_count) return false;
        const auto& mon = snap.party[bag_target].mon;
        const bool max = (d->flags & (1u << 13)) != 0;
        const auto mv = assets.Waza(mon.moves[k]);
        const int ups = mon.pp_ups[k] & 3, next = max ? 3 : ups + 1;
        bool ok = false;
        if (mv && mv->base_pp >= 5 && ups < 3) {
            const int grow = MaxPpFor(mon, k, next) - MaxPpFor(mon, k, ups);
            ok = live.EditParty({{bag_target, [=](std::array<u8, lp_pk8::CoreSize>& p, const lp_pk8::Mon& m) {
                if (!lp_pk8::SameIdentity(mon, m) || m.moves[k] != mon.moves[k] ||
                    (m.pp_ups[k] & 3) != ups || m.is_egg) return false;
                p[0x7E + k] = static_cast<u8>((p[0x7E + k] & ~3) | next);
                p[0x7A + k] = static_cast<u8>(m.pp[k] + grow);
                return true;
            }}}, bag_item);
        }
        Toast(ok ? F(K::ppup_toast, {{"move", assets.WazaName(mon.moves[k])}}) : S(K::toast_no_effect));
        bag_popup = 0;
        return ok;
    }

    // Soft-Boiled / Milk Drink: the user gives up to a fifth of its max HP to the target
    // (PokemonWindow.UseFieldWaza 0x19F9750).
    bool ApplyFieldWaza(int target) {
        bag_popup = 0;
        const int user = field_waza_user;
        if (user < 0 || user >= snap.party_count || target < 0 || target == user ||
            target >= snap.party_count || !snap.party[user].valid || !snap.party[target].valid ||
            !SameMoveLayout(field_waza_owner, snap.party[user].mon)) return false;
        const auto& u = snap.party[user].mon;
        const auto& t = snap.party[target].mon;
        const int amount = std::min<int>(u.hp_max / 5, t.hp_max - t.hp);
        bool ok = false;
        if (u.hp > u.hp_max / 5 && t.hp > 0 && amount > 0 && !t.is_egg) {
            const auto set_hp = [](std::array<u8, lp_pk8::CoreSize>& p, int hp) {
                p[0x8A] = static_cast<u8>(hp); p[0x8B] = static_cast<u8>(hp >> 8);
            };
            ok = live.EditParty({{user, [=](auto& p, const lp_pk8::Mon& m) { if (!lp_pk8::SameIdentity(u, m) || m.hp != u.hp || m.hp_max != u.hp_max || m.is_egg) return false; set_hp(p, m.hp - amount); return true; }},
                                 {target, [=](auto& p, const lp_pk8::Mon& m) { if (!lp_pk8::SameIdentity(t, m) || m.hp != t.hp || m.hp_max != t.hp_max || m.is_egg) return false; set_hp(p, m.hp + amount); return true; }}},
                                0);
        }
        Toast(ok ? F(K::toast_recovered, {{"who", lp_pk8::Utf8(t.nickname)}, {"n", std::to_string(amount)}})
                 : S(K::toast_cant_now));
        return ok;
    }

    bool ApplyBagItem(int move) {
        const auto d = assets.Item(bag_item);
        if (!d || bag_target < 0 || bag_target >= snap.party_count) return false;
        const auto& mon = snap.party[bag_target].mon;
        std::array<int, 4> ppmax{};
        for (int k = 0; k < 4; ++k) ppmax[k] = MaxPpFor(mon, k, mon.pp_ups[k]);
        const int done = live.UseMedicine(bag_target, bag_item, d->hp_rcv, d->flags, d->pp_rcv, ppmax, move, d->friend_change, &bag_target_owner);
        const auto who = lp_pk8::Utf8(mon.nickname);
        bag_popup = 0;
        Toast(done > 1 ? F(K::toast_recovered, {{"who", who}, {"n", std::to_string(done)}})
              : done == 1 ? F(K::toast_used_on, {{"item", assets.ItemName(bag_item)}, {"who", who}})
                          : S(K::toast_no_effect));
        return done > 0;
    }

    // The party member's field moves in move order: Soft-Boiled 135 / Milk Drink 208 (HP sharing,
    // a data edit) and, with the guest stub, Dig 91, Teleport 100, Flash 148, Sweet Scent 230 and
    // Defog 432 (FieldManager.UI_SelectWaza). The card's button uses the first; the Moves page
    // has a Use pill on each.
    std::vector<int> FieldMovesOf(int slot) const {
        std::vector<int> out;
        if (slot < 0 || slot >= snap.party_count || !snap.party[slot].valid || snap.party[slot].mon.is_egg) return out;
        for (const int m : snap.party[slot].mon.moves) {
            const bool share = m == 135 || m == 208;
            const bool native = m == 91 || m == 100 || m == 148 || m == 230 || m == 432;
            if ((share || (native && guest.Present())) && std::find(out.begin(), out.end(), m) == out.end())
                out.push_back(m);
        }
        return out;
    }

    // ---- game-thread calls (lp_guest.h): each job first waits for no running event ----
    lp_guest::Step Call(u64 rva, std::function<u64()> self, std::function<std::array<u64, 4>(u64)> args,
                        std::function<bool(u64)> ok, std::string refusal = "toast_cant_use_here") {
        // `self` gives the instance / first argument; a missing one (0) refuses the step
        auto first = std::make_shared<u64>(0);
        return {[this, rva, self, first] {
                    *first = self ? self() : 1;
                    return *first ? live.Code(rva) : 0;
                },
                [args, first] { return args(*first); }, std::move(ok), std::move(refusal)};
    }
    std::function<u64()> FieldManagerArg() { return [this] { return live.FieldManagerObject(); }; }

    // `done`: runs once the whole job ran (every step's result accepted), before the success toast.
    bool GuestJob(std::vector<lp_guest::Step> steps, std::string success, std::function<void()> done = {}) {
        if (!FieldFree() || !guest.Alive() || guest.Busy()) {
            Toast(guest.Busy() ? S(K::toast_wait) : S(K::toast_cant_now));
            return false;
        }
        steps.insert(steps.begin(), Call(lp_guest::rva::IsRunningEvent, [this] { return live.EvDataManagerObject(); },
                                         [](u64 ev) { return std::array<u64, 4>{ev, 0, 0, 0}; }, lp_guest::False,
                                         "toast_cant_now"));
        return guest.Run(std::move(steps), [this, success, done = std::move(done)](bool ok, const std::string& refusal) {
            if (ok && done) done();
            if (ok && !success.empty()) Toast(success);
            else if (!ok && !refusal.empty()) Toast(SKey(refusal));
        });
    }

    // Bag field items (Escape Rope, Honey, key items): FieldManager.UI_onUseFieldItem says whether
    // it works here, UseFieldItem starts it (the item's own event script, as from the Bag).
    // Escape Rope / Honey: the game's Bag takes one for the use (the item's event does not; verified
    // live with Honey). The companion takes it only once UseFieldItem has run, by a data write over
    // the count it reads then (the stub writes the call's result in the same field frame, so an
    // Escape Rope warp cannot hold it back). A refused check, a timed-out or unissued call never
    // costs the item.
    bool UseFieldItem(int item) {
        namespace r = lp_guest::rva;
        const auto fm = FieldManagerArg();
        const u64 id = static_cast<u64>(item);
        const bool consumed = lp_field_actions::ConsumedOnUse(item);
        const u64 owner = live.PlayerWorkObject();
        if (consumed && (!owner || live.BagCount(item) <= 0)) {
            Toast(S(K::toast_cant_now));
            return false;
        }
        std::vector<lp_guest::Step> steps{
            Call(r::UiOnUseFieldItem, fm, [id](u64 f) { return std::array<u64, 4>{f, id, 0, 0}; }, lp_guest::Zero),
            Call(r::UseFieldItem, fm, [id](u64 f) { return std::array<u64, 4>{f, id, 0, 0}; }, lp_guest::Any)};
        std::function<void()> take;
        if (consumed)
            take = [this, owner, item] { live.ConsumeItem(owner, item); };
        return GuestJob(std::move(steps), consumed ? F(K::toast_used, {{"item", assets.ItemName(item)}}) : "",
                        std::move(take));
    }

    // Party field moves: UI_onFieldWaza checks it with a FieldWazaParam (only wazaNo +0x10 is read
    // for these moves; a zeroed object in the mailbox scratch serves), UI_SelectWaza runs it.
    bool UseFieldMove(int waza) {
        namespace r = lp_guest::rva;
        const auto fm = FieldManagerArg();
        const u64 w = static_cast<u64>(waza);
        auto check = Call(r::UiOnFieldWaza, fm, [this](u64 f) {
            return std::array<u64, 4>{f, guest.Address(lp_guest::MbScratch), 0, 0};
        }, lp_guest::Zero);
        auto fn = check.fn;
        check.fn = [this, fn, waza] {
            for (u32 o = 0; o < 0x28; o += 8) guest.Store64(lp_guest::MbScratch + o, 0);
            guest.Store32(lp_guest::MbScratch + 0x10, static_cast<u32>(waza));
            return fn();
        };
        return GuestJob({std::move(check),
                         Call(r::UiSelectWaza, fm, [w](u64 f) { return std::array<u64, 4>{f, w, 0, 0}; }, lp_guest::Any)},
                        "");
    }

    // Pokétch Hidden Moves: FieldPoketch.CanUseHidenWaza (the game shows its own "can't" text),
    // then UseHidenWaza. Types: 0 Rock Smash, 1 Cut, 2 Fly, 3 Defog, 4 Surf, 5 Strength, 6 Rock
    // Climb, 7 Waterfall.
    bool UseHiddenMove(int type) {
        namespace r = lp_guest::rva;
        const u64 t = static_cast<u64>(type);
        return GuestJob({Call(r::CanUseHidenWaza, {}, [t](u64) { return std::array<u64, 4>{t, 0, 0, 0}; },
                              lp_guest::True),
                         Call(r::UseHidenWaza, {}, [t](u64) { return std::array<u64, 4>{t, 0, 0, 0}; }, lp_guest::Any)},
                        "");
    }

    // Fly to the place picked on the Town Map (Route Pokémon page): a town Fly can reach and that
    // the player has arrived at, with Fly usable (badge + HM; FieldPoketch checks the rest).
    // The Map tab: the selected place's name cloud (its screen position follows the map's live view),
    // whether Fly can take you there, the legend's player.
    void PublishMapSelection() {
        const auto guide = assets.MapGuide(snap.map_guide);
        // UIManager.SetupCurrentTownmapGuideMessage fills WordSet slots1/2 with the rival/supporter.
        const auto supporter = assets.Label("dp_characters", snap.player_sex ? "DP_CHARACTERS_205" : "DP_CHARACTERS_204");
        const auto message = guide ? lp_strings::MapGuide(assets.Label(guide->table, guide->label),
                                                          snap.rival_name, supporter) : std::string{};
        const auto layout = lp_text::FitMapGoal(TextHeight(message, 5, lp_text::MapGoalWidth),
                                               TextHeight(message, 4, lp_text::MapGoalWidth));
        I64("lp.map.goal.text5", layout.scale == 5);
        I64("lp.map.goal.text4", layout.scale == 4);
        I64("lp.map.goal.text.y", (layout.height - layout.text_height) / 2);
        I64("lp.map.goal.flag.y", (layout.height - 40) / 2);
        for (const int h : {64, 76, 89, 108, 129})
            I64("lp.map.goal.height" + std::to_string(h), layout.height == h);
        I64("lp.map.sel.top", !message.empty() ? layout.height : 0);
        if (!map_sel) I64("lp.map.sel.vis", 0);
        Text("lp.map.sel.name", map_sel ? map_sel_name : std::string{});
        Text("lp.map.header.name", map_sel && !map_sel_name.empty() ? map_sel_name : assets.AreaName(snap.zone));
        if (map_sel) {
            // Publish game-space inputs only. The manifest projects them after PublishMapState:
            // @map_view_* is unavailable during the module sample, before that host phase.
            const auto [mx, my] = CellCentre(map_sel->first, map_sel->second);
            I64("lp.map.sel.wx", mx);
            I64("lp.map.sel.wy", MapPicH - my);
            I64("lp.map.sel.half", TextWidth(map_sel_name, 5) / 2 + 22);
            I64("lp.map.sel.vis", snap.map_acquired);
            I64("lp.map.sel.fly", FlySpotFor(map_sel_zone, map_sel).has_value());
        } else {
            I64("lp.map.sel.fly", 0);
        }
        // the legend: your head icon, captioned "You" (fixed in the manifest)
        Text("lp.map.head", "module:lp:maphead/" + std::to_string(snap.fashion) + "/" + std::to_string(snap.body_type) + "/" + std::to_string(snap.player_sex));
        I64("lp.map.me.on", snap.map_acquired && map_cell.has_value());
        if (map_cell) {
            const auto [x, y] = CellCentre(map_cell->first, map_cell->second);
            I64("lp.map.me.x", x); I64("lp.map.me.y", MapPicH - y);
        }
        Text("lp.map.goal.message", message);
        Text("lp.map.goal.icon", "module:lp:ui/map/map_ico_goal_01");
        I64("lp.map.goal.message.on", snap.map_acquired && !message.empty());
        const bool goal = guide && guide->x >= 0 && guide->x < 34 && guide->y >= 0 && guide->y < 27;
        I64("lp.map.goal.on", snap.map_acquired && goal);
        if (goal) {
            const auto [x, y] = CellCentre(guide->x, guide->y);
            I64("lp.map.goal.x", x); I64("lp.map.goal.y", MapPicH - y);
        }
    }

    std::optional<lp_assets::Catalog::FlySpot> RouteFlySpot() const {
        return FlySpotFor(route_zone, route_cell);
    }
    // The Fly destination of Town Map cell `cell` picked for `zone` (-1 / no cell: none).
    std::optional<lp_assets::Catalog::FlySpot> FlySpotFor(int zone, const std::optional<std::pair<int, int>>& cell) const {
        if (!cell || zone < 0 || snap.hidden_moves[3] != 2 || !guest.Present()) return std::nullopt;
        auto spot = assets.FlySpotAt(cell->first, cell->second);
        if (!spot || !lp_map::Arrived(spot->gate.view, spot->gate.color, spot->gate.town, snap.map_work, snap.map_sys))
            return std::nullopt;
        return spot;
    }
    bool FlyToRoutePlace() {
        namespace r = lp_guest::rva;
        const auto spot = RouteFlySpot();
        if (!spot) return false;
        const u64 zone = static_cast<u64>(spot->zone), locator = static_cast<u64>(spot->locator);
        return GuestJob({Call(r::CanUseHidenWaza, {}, [](u64) { return std::array<u64, 4>{2, 0, 0, 0}; },
                              lp_guest::True, ""),
                         Call(r::UiOnWazaFly, FieldManagerArg(),
                              [zone, locator](u64 f) { return std::array<u64, 4>{f, zone, locator, 0}; }, lp_guest::Any)},
                        "");
    }

    void PublishPoketch() {
        StepPoketch();
        if constexpr (DebugKeys) {
            I64("lp.pkt.sound.issued", pkt_sound_issued);
            I64("lp.pkt.sound.done", pkt_sound_done);
            I64("lp.pkt.sound.ok", pkt_sound_ok);
            I64("lp.pkt.sound.event", pkt_sound_event);
        }
        std::vector<int> apps;
        for (int i = 0; i < lp_poketch::AppCount; ++i)
            if (snap.poketch_acquired && snap.poketch_ok && snap.poketch_apps[i]) apps.push_back(i);
        if (!pkt_init && snap.poketch_ok) { pkt_colour = snap.poketch_colour; pkt_init = true; }
        if (std::find(apps.begin(), apps.end(), pkt_app) == apps.end()) pkt_app = apps.empty() ? -1 : apps.front();
        pkt_rows = apps;
        // only the Pokétch pages (casing and full screen) read the rest
        if (field_view != 3) return;
        I64("lp.poketch.count", static_cast<int64_t>(apps.size()));
        // the selected app's row: the list scrolls to keep it in view (the red button, a swipe)
        {
            const auto it = std::find(apps.begin(), apps.end(), pkt_app);
            I64("lp.poketch.sel", it == apps.end() ? -1 : static_cast<int64_t>(it - apps.begin()));
        }
        for (size_t r = 0; r < apps.size(); ++r) {
            const auto row = "lp.poketch.app" + std::to_string(r);
            Text(row + ".name", AppName(apps[r]));
            I64(row + ".selected", apps[r] == pkt_app);
        }
        I64("lp.pkt.one", 1);
        // the only lp.pkt.is<k> the manifest binds: Egg Monitor, History, Roulette
        for (int k : {lp_poketch::EggMonitor, lp_poketch::History, lp_poketch::Roulette}) I64("lp.pkt.is" + std::to_string(k), pkt_app == k);
        Text("lp.pkt.name", pkt_app >= 0 ? AppName(pkt_app) : "");
        if (pkt_app < 0) {
            for (const char* k : {"lp.pkt.image", "lp.pkt.imagec", "lp.pktx.image", "lp.pktx.imagec"}) Text(k, "");
            pkt_full = false;
            return;
        }
        const std::time_t now = std::time(nullptr);
        std::tm lt{};
#ifdef _WIN32
        localtime_s(&lt, &now);
#else
        localtime_r(&now, &lt);
#endif
        const auto animation_now = std::chrono::steady_clock::now();
        const auto elapsed = [animation_now](auto at) {
            return std::chrono::duration_cast<std::chrono::milliseconds>(animation_now - at).count();
        };
        const int hop_offset = lp_poketch::HopOffset(elapsed(pkt_hop_at));
        const int hop_slot = hop_offset ? pkt_hop_slot : -1;
        std::vector<int> v{pkt_app, pkt_colour};
        switch (pkt_app) {
        case lp_poketch::DigitalWatch: case lp_poketch::AnalogWatch: v.insert(v.end(), {lt.tm_hour, lt.tm_min}); break;
        case lp_poketch::Calculator: {
            v.push_back(pkt_calc.op);
            const auto codes = pkt_calc.Codes();
            v.insert(v.end(), codes.begin(), codes.end());
            break;
        }
        case lp_poketch::Pedometer: v.push_back(static_cast<int>(snap.steps)); break;
        case lp_poketch::Counter: v.push_back(pkt_counter); break;
        case lp_poketch::CoinToss:
            v.insert(v.end(), {pkt_coin, lp_poketch::CoinFrame(elapsed(pkt_coin_at))});
            break;
        case lp_poketch::MemoPad: v.insert(v.end(), {memo_tool, memo_rev}); break;
        case lp_poketch::Dowsing: {
            const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - dowse_at).count();
            const bool on = dowse_active && (secs < 2.0 || dowse_found) &&
                            snap.grid_x == dowse_gx && snap.grid_y == dowse_gy;
            const double phase = std::fmod(secs, 2.0) / 2.0;
            // Thirteen radii per sweep; fixed LCD dots, bounded image churn.
            const int radius = on && phase < 0.97 ?
                std::clamp(static_cast<int>(std::sin(phase * 1.5707963) * 200) / 16 * 16, 16, 192) : 0;
            const bool blink = on && secs >= 2.0 && std::sin(phase * 3.14159) > 0.25;
            v.insert(v.end(), {radius, dowse_x, dowse_y, blink ? static_cast<int>(dowse_markers.size()) : 0});
            if (blink) for (const auto& [mx,my] : dowse_markers) v.insert(v.end(), {mx,my});
            break;
        }
        case lp_poketch::EggMonitor:
            for (const auto& d : snap.daycare)
                v.insert(v.end(), {d.valid && !d.mon.is_egg ? d.mon.species : 0, d.valid && !d.mon.is_egg ? d.mon.form : 0, d.valid && !d.mon.is_egg ? d.mon.gender : 0, d.valid && !d.mon.is_egg});
            v.push_back(snap.egg_exists);
            v.insert(v.end(), {hop_slot, hop_offset});
            break;
        case lp_poketch::History:
            for (const auto& h : snap.history) {
                v.insert(v.end(), {h[0], h[1]});
            }
            v.insert(v.end(), {hop_slot, hop_offset});
            break;
        case lp_poketch::MarkingMap: {
            if (!marks_init && snap.poketch_ok) {
                for (int k = 0; k < 6; ++k) marks[k] = {snap.marks[k][0], snap.marks[k][1]};
                marks_init = true;
            }
            const auto px = [](const std::optional<std::pair<int, int>>& c) {
                return c ? std::pair{static_cast<int>(33.5 + 17 * c->first), static_cast<int>(4.5 + 17 * c->second)}
                         : std::pair{-1, -1};
            };
            const auto me = px(map_cell);
            std::array<std::pair<int, int>, 2> roam{std::pair{-1, -1}, std::pair{-1, -1}};
            for (int i = 0; i < 2; ++i)
                if (snap.roamers[i].status == 1) roam[i] = px(assets.RoamerCell(snap.roamers[i].zone_index));
            // native blink: Cresselia, Mesprit, then you, 0.4 s each
            static constexpr int Phase[3] = {2, 1, 0};
            int bits = 0;
            for (int i = 0; i < 4; ++i) bits |= (snap.map_works[i] != 0) << i;
            v.insert(v.end(), {me.first, me.second, roam[0].first, roam[0].second, roam[1].first, roam[1].second,
                               Phase[tick / 24 % 3], bits});
            for (const auto& [x, y] : marks) v.insert(v.end(), {x, y});
            v.push_back(mark_sel);  // keeps the key fresh when a marker is picked up
            break;
        }
        case lp_poketch::DotArtist:
            if (!dot_init && snap.dotart_ok) {
                dot_modified = snap.dotart_modified;
                dots = dot_modified ? lp_poketch::UnpackDotArt(snap.dotart.data()) : lp_poketch::DefaultDotArt();
                lp_poketch::SetSurface(lp_poketch::DotArtSurface, dots);
                dot_init = true;
                ++dot_rev; // a new key, so a blank picture drawn before the save was readable reloads
            }
            v.push_back(dot_rev);
            break;
        case lp_poketch::ChainCounter:
            v.insert(v.end(), {snap.chain_count > 0 ? snap.chain_mons : 0, snap.chain_count});
            for (const auto& r : snap.chain_ranking) v.insert(v.end(), {r[0], r[1]});
            break;
        case lp_poketch::HiddenMoves:
            v.insert(v.end(), snap.hidden_moves.begin(), snap.hidden_moves.end());
            break;
        case lp_poketch::Roulette:
            // Twelve cached angular frames, sampled on the LCD grid after rotation.
            v.insert(v.end(), {roul_rev, static_cast<int>(roul_angle) % 360 / 30 * 30});
            break;
        case lp_poketch::KitchenTimer: {
            const int left = static_cast<int>((timer_left_ms + 999) / 1000);
            const int frame = timer_alarm ? static_cast<int>((tick - timer_alarm_tick) / 20 % 4) : -1;
            v.insert(v.end(), {left / 60, left % 60, timer_running ? 1 : 0, frame});
            break;
        }
        case lp_poketch::PokemonList: case lp_poketch::Friendship:
            for (int i = 0; i < 6; ++i) {
                const bool ok = i < snap.party_count && snap.party[i].valid && !snap.party[i].mon.is_egg;
                const auto& m = snap.party[i].mon;
                if (pkt_app == lp_poketch::PokemonList)
                    v.insert(v.end(), {ok ? m.species : 0, ok ? m.form : 0, ok ? m.gender : 0,
                                       ok && m.hp_max ? m.hp * 1000 / m.hp_max : 0, ok && m.held_item ? 1 : 0, ok});
                else
                    v.insert(v.end(), {ok ? m.species : 0, ok ? m.form : 0, ok ? m.gender : 0,
                                       !ok ? 0 : m.friendship >= 150 ? 2 : m.friendship >= 70 ? 1 : 0, ok});
            }
            v.insert(v.end(), {hop_slot, hop_offset});
            break;
        case lp_poketch::Calendar: {
            std::tm next = lt;
            next.tm_mon += 1; next.tm_mday = 0; next.tm_hour = 12;
            std::mktime(&next);
            const int first = ((lt.tm_wday - (lt.tm_mday - 1) % 7) % 7 + 7) % 7;
            v.insert(v.end(), {lt.tm_mon + 1, lt.tm_mday, first, next.tm_mday,
                               static_cast<int>(snap.calendar_marks[lt.tm_mon] & 0x7FFFFFFF)});
            break;
        }
        default: break;
        }
        UpdateDowsing();
        const auto key = LangKey(lp_poketch::Key(v));
        // classic Platinum theme: the default colour is Platinum's teal display
        if (pkt_colour == 0) v[1] = 8;
        const auto keyc = pkt_colour == 0 ? LangKey(lp_poketch::Key(v)) : key;
        Text("lp.pkt.image", key);
        Text("lp.pkt.imagec", keyc);
        // full screen: the same screens drawn at 1.875x (only while shown: 4 MB each)
        const auto full = [&](const std::string& k) {
            return pkt_full ? "module:lp:poketchx/" + std::to_string(lp_poketch::FullScale) + k.substr(17) : std::string{};
        };
        Text("lp.pktx.imagec", full(keyc));
        Text("lp.pktx.image", full(key));
        Text("lp.pkt.fallback", pkt_frames.Fallback(0, key, ""));
        Text("lp.pkt.fallbackc", pkt_frames.Fallback(1, keyc, ""));
        if (!pkt_full) { pkt_frames.Inactive(2); pkt_frames.Inactive(3); }
        Text("lp.pktx.fallback", pkt_full ? pkt_frames.Fallback(2, full(key), key) : "");
        Text("lp.pktx.fallbackc", pkt_full ? pkt_frames.Fallback(3, full(keyc), keyc) : "");

    }

    void PublishField() {
        ValidateBagPopup();
        I64("lp.money", snap.money);
        const int party_state = snap.party_ok && snap.party_count == 0 ? 0 :
            std::any_of(snap.party.begin(), snap.party.end(), [](const auto& m) { return m.valid; }) ? 1 : 2;
        if (party_state != last_party_state) { last_party_state = party_state; ++scene_revision; }
        I64("lp.party.empty", snap.party_ok && snap.party_count == 0);
        // The member chosen by the Party card's select action.
        int sel = sel_party;
        if (sel < 0 || sel >= snap.party_count || !snap.party[sel].valid)
            sel = 0;
        I64("lp.party.selected", snap.party_count > 0 && snap.party[sel].valid);
        I64("lp.party.unavailable", !snap.party_ok || (snap.party_count > 0 && !snap.party[sel].valid));
        I64("lp.sel", sel);
        sel_party = sel;
        if (sel != move_drag_slot || !lp_pk8::SameIdentity(move_drag_owner, snap.party[sel].mon) ||
            move_drag_owner.moves != snap.party[sel].mon.moves ||
            move_drag_owner.pp != snap.party[sel].mon.pp || move_drag_owner.pp_ups != snap.party[sel].mon.pp_ups) {
            move_drag_slot = sel;
            move_drag_owner = snap.party[sel].mon;
            move_drag_token = lp_field_actions::NextMoveToken(move_drag_token);
        }
        for (int i = 0; i < 4; ++i)
            I64("lp.sel.m" + std::to_string(i) + ".payload", 100 + move_drag_token * 4 + i);
        if constexpr (DebugKeys) I64("lp.demo.active", snap.demo_active);
        const bool move_popup = party_move >= 0 && party_move < 4 && field_view == 0 && field_tab == 1 &&
            !snap.in_battle && !bag_popup && snap.party_count > sel && snap.party[sel].valid &&
            !snap.party[sel].mon.is_egg && lp_pk8::SameIdentity(party_move_owner, snap.party[sel].mon) &&
            snap.party[sel].mon.moves[party_move] == party_move_id && party_move_id > 0;
        if (!move_popup) party_move = -1;
        I64("lp.party.move.on", move_popup);
        if (move_popup) {
            const auto& mon = snap.party[sel].mon;
            const int id = mon.moves[party_move];
            const auto info = assets.Waza(id);
            PublishMoveRow("lp.party.move.", id, mon.pp[party_move], MaxPpFor(mon, party_move, mon.pp_ups[party_move]));
            Text("lp.party.move.desc", assets.WazaDescription(id));
            Text("lp.party.move.power", !info || info->power <= 1 ? "—" : std::to_string(info->power));
            Text("lp.party.move.accuracy", !info || info->accuracy <= 0 || info->accuracy > 100 ? "—" : std::to_string(info->accuracy));
            Text("lp.party.move.category", info ? lp_assets::UiKey("common", "cmn_ico_wazacategory_01_0" +
                std::to_string(info->category == 1 ? 1 : info->category == 2 ? 2 : 3)) : std::string{});
        } else {
            // Clear identity-bearing strings as well as hiding the popup when selecting an Egg.
            for (const auto* key : {"name", "tag", "desc", "power", "accuracy", "category"})
                Text(std::string("lp.party.move.") + key, "");
            I64("lp.party.move.id", 0);
        }
        I64("lp.fld.tab", field_tab);
        const auto fws = FieldMovesOf(sel_party);
        // the card's field-move buttons (the HP row on the card: the plate on the left shows HP)
        for (size_t k = 0; k < 4; ++k) {
            const auto key = "lp.sel.fw" + std::to_string(k);
            I64(key + ".on", k < fws.size());
            Text(key + ".name", k < fws.size() ? assets.WazaName(fws[k]) : std::string{});
        }
        I64("lp.bag.popup.any", bag_popup != 0);
        I64("lp.bag.popup.party", bag_popup == 1);
        // Publish every sample so a closed or invalidated popup cannot leave stale move rows.
        I64("lp.bag.popup.moves", bag_popup == 2);
        if (bag_popup == 2 && bag_target >= 0 && bag_target < snap.party_count && !snap.party[bag_target].mon.is_egg)
            for (int k = 0; k < 4; ++k) {
                const auto& mon = snap.party[bag_target].mon;
                PublishMoveRow("lp.bag.m" + std::to_string(k) + ".", mon.moves[k], mon.pp[k],
                               MaxPpFor(mon, k, mon.pp_ups[k]));
            }
        Text("lp.bag.popup.title.f", S(K::popup_give_hp));
        const auto toast_age = std::chrono::steady_clock::now() - bag_toast_at;
        const bool toast = !bag_toast.empty() && toast_age < std::chrono::seconds(3);
        // the text and panel size stay ToastTailMs longer, so the toast slides out with its message
        const bool toast_text = !bag_toast.empty() &&
                                toast_age < std::chrono::seconds(3) + std::chrono::milliseconds(ToastTailMs);
        I64("lp.bag.toast.on", toast);
        Text("lp.bag.toast", toast_text ? bag_toast : "");
        // a toast too wide for one line even at 4 (its fit_text floor) takes two lines in a taller panel
        for (const int wrap : ToastWraps)
            I64("lp.bag.toast.two." + std::to_string(wrap), toast_text && TextWidth(bag_toast, 4) > wrap);
        I64("lp.fld.view", field_view);
        // Outfit-aware resolver supplies the native default bag when outfit artwork is missing.
        Text("lp.nav.bag.on", "module:lp:square/bag/" + std::to_string(snap.fashion) + "/" +
                              std::to_string(snap.player_sex));
        I64("lp.pokedex.acquired",snap.pokedex_acquired);
        I64("lp.pokedex.unavailable",!snap.pokedex_acquired);
        I64("lp.poketch.acquired",snap.poketch_acquired);
        I64("lp.poketch.unavailable",!snap.poketch_acquired);
        I64("lp.fld.ok", FieldFree() ? 1 : 0);
        RefreshBag();
        if (field_view == 5) PublishBag();
        else if (field_view == 4) PublishDex();
        if constexpr (DebugKeys) Text("lp.area.name", assets.AreaName(snap.zone));
        const auto current = snap.position_ok ? assets.MapCell(snap.zone, snap.grid_x, snap.grid_y) : std::nullopt;
        const auto outdoor = snap.map_zone ? assets.MapCell(snap.map_zone, snap.map_grid_x, snap.map_grid_y) : std::nullopt;
        using Location = lp_map::PlayerLocation::Location;
        const auto location = map_player.Update(snap.field && snap.player_ok ? live.PlayerWorkObject() : 0,
            snap.map_acquired, assets.MapRoom(snap.zone),
            current ? std::optional<Location>{{snap.zone, *current}} : std::nullopt,
            outdoor ? std::optional<Location>{{snap.map_zone, *outdoor}} : std::nullopt);
        map_cell = location ? std::optional{location->cell} : std::nullopt;
        const int map_zone = location ? location->zone : snap.zone;
        I64("lp.map.acquired",snap.map_acquired);
        I64("lp.map.unavailable",!snap.map_acquired);
        const auto map_masks=assets.MapMasks(snap.map_work,snap.map_sys);
        // The player is a live map marker, not pixels baked into a replaceable map image.
        const auto map_key = "module:lp:map/" + std::to_string(map_zone) + "/-1/-1/" +
            std::to_string(map_masks.first) + "/" + std::to_string(map_masks.second) + "/" +
            std::to_string(snap.fashion) + "/" + std::to_string(snap.body_type);
        Text("lp.map.image", map_key);
        // the map's base view: full picture height at the widget's 1168:756 aspect, centred (the
        // places span x 226..1066 of 1266, so only the sea/cloud margins are cropped)
        I64("lp.map.v.x0", 67); I64("lp.map.v.y0", 0); I64("lp.map.v.x1", 1199); I64("lp.map.v.y1", 732);
        if (map_key != last_map_key) { last_map_key = map_key; ++scene_revision; }
        PublishPoketch();
        if (field_view == 2) PublishRoute();
        PublishMapSelection();
        if (snap.party_count > 0 && snap.party[sel].valid) {
            const auto m = lp_pk8::DisplayMon(snap.party[sel].mon);
            PublishMon("lp.sel.", m.species, m.form, m.gender, m.level, m.hp, m.hp_max, m.held_item, m.shiny,
                       lp_pk8::Utf8(m.nickname), true, m.is_egg);
            Text("lp.sel.itemname", m.held_item > 0 ? assets.ItemName(m.held_item) : std::string{"-"});
            Text("lp.sel.ability", assets.AbilityName(m.ability));
            const auto desc = assets.AbilityDescription(m.ability);
            Text("lp.sel.ability.desc", desc);
            // the card draws it at 5 in up to three lines, else at 4 (up to five)
            I64("lp.sel.ability.big", TextHeight(desc, 5, PartyDescW) <= 5 * 5 + 2 * (5 * 5 + 5 * 3));
            Text("lp.sel.status", m.hp == 0 ? S(K::fainted) : StatusName(lp_pk8::Sick(m.status)));
            I64("lp.sel.atk", m.atk);
            I64("lp.sel.def", m.def);
            I64("lp.sel.spa", m.spa);
            I64("lp.sel.spd", m.spd);
            I64("lp.sel.spe", m.spe);
            for (int k = 0; k < 4; ++k) {
                const int mx = MaxPpFor(m, k, m.pp_ups[k]); // unknown move: its current PP
                PublishMoveRow("lp.sel.m" + std::to_string(k) + ".", m.moves[k], m.pp[k], mx ? mx : m.pp[k]);
            }
        }
        for (int i = 0; i < 6; ++i) {
            const std::string p = "lp.p" + std::to_string(i) + ".";
            const auto& pm = snap.party[i];
            I64(p + "valid", pm.valid ? 1 : 0);
            if (!pm.valid) {
                Text(p + "icon", "");
                Text(p + "sick", "");
                continue;
            }
            const auto m = lp_pk8::DisplayMon(pm.mon);
            PublishMon(p, m.species, m.form, m.gender, m.level, m.hp, m.hp_max, m.held_item, m.shiny,
                       lp_pk8::Utf8(m.nickname), false, m.is_egg);
            Text(p + "sick", m.is_egg ? std::string{} : SickTagKey(m.hp, m.status));
        }
    }

    // Route Pokémon (Town Map place or where you are): only the route page (field view 2) reads it.
    void PublishRoute() {
        I64("lp.route.ready", assets.EncounterDataReady());
        const int rzone = route_zone >= 0 ? route_zone : snap.zone;
        Text("lp.route.name", assets.AreaName(rzone));
        I64("lp.route.fly", RouteFlySpot().has_value());
        const auto encounters = assets.Encounters(rzone);
        const int encounter_count = static_cast<int>(std::min<size_t>(encounters.size(), 128));
        I64("lp.route.count", encounter_count);
        I64("lp.route.rows", (encounter_count + 3) / 4);
        for (int i = 0; i < encounter_count; ++i) {
            const auto& e = encounters[i];
            const auto p = "lp.route" + std::to_string(i) + ".";
            Text(p + "name", assets.SpeciesName(e.species));
            Text(p + "icon", lp_assets::IconKey(e.species, 0, 0, false));
            Text(p + "method", MethodName(e.method));
            Text(p + "level", LevelRange(e.min_level, e.max_level));
        }
    }

    void PublishBattle() {
        // Keep page changes and the scene refresh in the same sample so shared cards repaint.
        if (!snap.in_battle) {
            last_cmd_focus = false;
            battle_view = 0;
        }
        if (snap.in_battle && !pane_in_battle) {
            // a new battle: the command page's pane opens on the foe, the move page's on the move
            tab_cmd = 0;
            tab_fight = 3;
            ++battle_seq;
            foe_dex_species = -1;
            foe_caught = -1;
            // and a double starts from the game's choice, not the last battle's tapped focus
            dbl_focus = dbl_game = dbl_manual = dbl_last_foe = -1;
            dbl_turn = -2;
        }
        pane_in_battle = snap.in_battle;
        if (snap.action.focus && !last_cmd_focus) {
            battle_view = 0;
            ++scene_revision;
        }
        if (snap.waza.focus && battle_view != 1 && drive.kind != Drive::Kind::BackMoves) {
            // the move page opens on its Move tab every time (a double's next chooser too): a
            // battler shown there earlier (a card tap, a swipe) must not hide the move card for
            // the rest of the battle
            battle_view = 1;
            tab_fight = 3;
            ++scene_revision;
        }
        // A committed move (a button tap or the game's A) takes the move list's focus away; the turn
        // then plays behind the command page (with its busy veil) until the next command menu. The
        // target select (doubles) keeps the move page, also while it slides in (the move list slides
        // out first, ~0.3 s without any focus): the return waits until neither list has focus or
        // moves, for MoveDoneTicks samples.
        if (battle_view == 1 && snap.waza.focus)
            waza_seen = true;
        if (battle_view != 1 || snap.waza.focus || snap.waza.transition || snap.dbl.target_open ||
            snap.dbl.target_moving || snap.action.focus ||
            drive.kind == Drive::Kind::OpenMoves || drive.kind == Drive::Kind::BackMoves)
            move_done_ticks = 0;
        else if (waza_seen && ++move_done_ticks >= MoveDoneTicks) {
            battle_view = 0;
            move_done_ticks = 0;
            ++scene_revision;
        }
        if (battle_view != 1)
            waza_seen = false;
        // Native windows settle over several samples. Refresh once after a focus transition
        // so an earlier page draw cannot leave shared status cards outside the dirty region.
        const int focus_mask = snap.in_battle ? (snap.action.focus ? 1 : 0) |
            (snap.waza.focus ? 2 : 0) | (snap.ball.focus ? 4 : 0) : 0;
        if (focus_mask != last_focus_mask) {
            last_focus_mask = focus_mask;
            scene_refresh_ticks = 8;
        }
        if (scene_refresh_ticks > 0 && --scene_refresh_ticks == 0)
            ++scene_revision;
        if (snap.waza.focus && snap.waza.current != last_waza_index) {
            last_waza_index = snap.waza.current;
            ++scene_revision;
        }
        if (snap.action.focus && snap.action.current != last_action_index) {
            last_action_index = snap.action.current;
            ++scene_revision;
        }
        I64("lp.bt.view", snap.in_battle ? battle_view : -1);
        I64("lp.bt.scene_revision", scene_revision);

        const bool battle = snap.in_battle;
        if constexpr (DebugKeys) I64("lp.battle", battle ? 1 : 0);
        // 0 title / loading, 1 party, 2 battle, 3 map, 4 route, 5 Pokétch, 6 Pokédex, 7 Bag, 8 Pokétch full screen
        I64("lp.page", battle ? 2 : (snap.field && snap.player_ok ? (field_view == 1 ? 3 : field_view == 2 ? 4 : field_view == 3 ? (pkt_full ? 8 : 5) : field_view == 4 ? 6 : field_view == 5 ? 7 : 1) : 0));
        if constexpr (DebugKeys) I64("lp.field", snap.field ? 1 : 0);
        if (!battle)
            return;
        // runtime 18 runs modules before PublishFlags: no flag is visible here, so the menus are always parked
        live.HideBattleMenus(true);
        menus_parked = true;
        I64("lp.bt.cmd.focus", snap.action.focus && snap.action.show ? 1 : 0);
        I64("lp.bt.cmd.index", snap.action.current);
        for (int k = 0; k < 4; ++k)
            I64("lp.bt.cmd" + std::to_string(k) + ".available", k >= snap.action_min && k <= snap.action_max);
        I64("lp.bt.ball.focus", snap.ball.focus ? 1 : 0);
        I64("lp.bt.ball.input", snap.ball.focus && drive.kind == Drive::Kind::None ? 1 : 0);
        I64("lp.bt.continue", snap.battle_continue && drive.kind == Drive::Kind::None ? 1 : 0);
        I64("lp.bt.target.on", snap.dbl.target_open);
        I64("lp.bt.target.input", snap.dbl.target_open && !snap.dbl.target_moving && drive.kind == Drive::Kind::None);
        I64("lp.bt.target.spread", snap.dbl.target_open && !snap.dbl.target_single);
        I64("lp.bt.menu.opening", drive.kind == Drive::Kind::OpenMoves || drive.kind == Drive::Kind::BackMoves);
        I64("lp.bt.input", (snap.action.focus || snap.waza.focus) && drive.kind == Drive::Kind::None ? 1 : 0);
        // a new command phase forgets the move picked in the last one
        const bool cmd_focus = snap.action.focus && snap.action.show;
        if (cmd_focus && !last_cmd_focus && drive.kind == Drive::Kind::None)
            pick = -1;
        last_cmd_focus = cmd_focus;
        // the pane's battlers: you / the foe (a double: the focused battler and its counterpart) and
        // the Pokémon whose moves are listed (the move page: the one choosing its command)
        const DoubleView dv = PublishDouble();
        for (int c = 0; c < 2; ++c) {
            const auto& m = c == 0 ? dv.own : dv.foe;
            const std::string p = c == 0 ? "lp.bt.own." : "lp.bt.foe.";
            if (m.valid)
                PublishMon(p, m.species, static_cast<u8>(m.form), m.gender, m.level, ShownHp(m), m.hp_max, m.item, m.shiny,
                           c == 0 ? (dv.own_client == 0 ? OwnNick(m.species) : assets.SpeciesName(m.species))
                                  : std::string{},
                           false);
            if (dv.dbl)
                for (const char* z : {"hpg", "hpy", "hpr"})
                    I64(p + z, 0); // the single-battle header is hidden: no gauge
        }
        const auto& own = dv.moves;
        if (dv.moves_key != moves_key) {
            moves_key = dv.moves_key;
            if (!snap.waza.focus)
                pick = -1;
        }
        moves_count = own.waza_count;
        if (snap.waza.focus && drive.kind == Drive::Kind::None &&
            snap.waza.current >= 0 && snap.waza.current < own.waza_count)
            pick = snap.waza.current;
        // what a move is rated against: the foe (a double: the target, the focused foe or both foes)
        for (int k = 0; k < 4; ++k) {
            const std::string p = "lp.bt.m" + std::to_string(k) + ".";
            PublishMoveRow(p, k < own.waza_count ? own.waza[k] : 0, own.pp[k], own.pp_max[k]);
            I64(p + "picked", pick == k ? 1 : 0);
            const auto wi = k < own.waza_count ? assets.Waza(own.waza[k]) : std::nullopt;
            const auto r = Rate(wi, dv.targets);
            Text(p + "eff", r.text);
            Text(p + "effs", r.short_text);
            I64(p + "effgood", r.good ? 1 : 0);
        }
        // the picked move (or the first) for the info panel
        const int shown = pick >= 0 && pick < own.waza_count ? pick : 0;
        PublishMoveRow("lp.bt.pk.", own.waza_count > 0 ? own.waza[shown] : 0, own.pp[shown], own.pp_max[shown]);
        const auto info = own.waza_count > 0 ? assets.Waza(own.waza[shown]) : std::nullopt;
        // as the game prints them: "—" for no base power and for moves that never miss (101)
        Text("lp.bt.pk.powertxt", !info || info->power <= 1 ? "—" : std::to_string(info->power));
        Text("lp.bt.pk.acctxt", !info || info->accuracy <= 0 || info->accuracy > 100 ? "—"
                                                                                        : std::to_string(info->accuracy));
        Text("lp.bt.pk.catkey", !info ? std::string{}
                                      : lp_assets::UiKey("common", "cmn_ico_wazacategory_01_0" +
                                                                       std::to_string(info->category == 1 ? 1
                                                                                      : info->category == 2 ? 2
                                                                                                            : 3)));
        // the long form for the pane's Move page: "2× Super effective", "Status move"; against two
        // battlers "Name 2× · Name ½×"
        const auto rated = Rate(info, dv.targets);
        I64("lp.bt.pk.effgood", rated.good ? 1 : 0);
        Text("lp.bt.pk.efftxt", rated.long_text);
        PublishPane(dv.own, dv.foe, dv.moves, shown, dv.moves_on, dv.foe_line);
        I64("lp.bt.ball.any", snap.ball_enabled && !snap.balls.empty() ? 1 : 0);
        // the shortcut shows the game's last-used ball (first in its list)
        Text("lp.bt.ball0.icon", snap.balls.empty() ? std::string{} : lp_assets::ItemKey(snap.balls[0]));
        I64("lp.bt.ball.multi", snap.balls.size() > 1 ? 1 : 0);
        const int index = snap.ball.current;
        const int item = index >= 0 && index < static_cast<int>(snap.balls.size()) ? snap.balls[index] : 0;
        Text("lp.bt.ball.icon", lp_assets::ItemKey(item));
        Text("lp.bt.ball.name", assets.ItemPlural(item));
        Text("lp.bt.ball.desc", assets.ItemDescription(item));
        I64("lp.bt.ball.count", snap.ball.focus && item > 0 ? live.BagCount(item) : 0);
        PublishBattleWindow();
    }

    // ---- the game's Bag / Pokémon windows over the battle menu (top screen) ---------------------
    // The Bag: a note at the bottom of the battle pages to look at the main screen. The Pokémon
    // list: up to three switch suggestions (lp_switch) against the foe(s) in play.
    lp_switch::Battler SwitchBattler(const lp_live::BattleMon& m, int slot, bool staged) const {
        lp_switch::Battler b;
        b.slot = slot;
        b.types = BattleTypes(m);
        for (int i = 0; i < 5; ++i)
            b.stats[static_cast<std::size_t>(i)] = staged && m.staged[static_cast<std::size_t>(i)] >= 0
                                                       ? m.staged[static_cast<std::size_t>(i)]
                                                       : m.stats[static_cast<std::size_t>(i)];
        b.hp = m.hp;
        b.hp_max = m.hp_max;
        b.status = std::max(0, m.status);
        for (int k = 0; k < m.waza_count && k < 4; ++k) {
            const auto info = assets.Waza(m.waza[static_cast<std::size_t>(k)]);
            if (info)
                b.moves.push_back({info->type, info->category, info->power, assets.WazaName(m.waza[static_cast<std::size_t>(k)])});
        }
        return b;
    }

    void PublishBattleWindow() {
        const int window = snap.battle_window;
        if (window != last_battle_window) {
            last_battle_window = window;
            ++scene_revision; // the overlays come and go over the shared cards
        }
        I64("lp.bt.bagwin", window == lp_live::off::WindowIdBag ? 1 : 0);
        Text("lp.bt.bagwin.icon", "module:lp:square/bag/" + std::to_string(snap.fashion) + "/" +
                                 std::to_string(snap.player_sex));
        const bool open = window == lp_live::off::WindowIdPokemonBattle;
        I64("lp.bt.sw.on", open ? 1 : 0);
        std::vector<lp_switch::Suggestion> picks;
        std::string vs;
        if (open) {
            // the foes in play (a double: both present foes) and the benched, conscious own members
            std::vector<lp_switch::Battler> foes;
            std::vector<bool> active(6, false);
            if (DoubleOn()) {
                // foes by view position (odd = the far side), as PublishDouble picks them
                for (std::size_t k = 0; k < snap.dbl.view.size(); ++k) {
                    const auto& slot = snap.dbl.view[k];
                    if (!slot.exists)
                        continue;
                    if (slot.client == snap.dbl.my_client && slot.index >= 0 && slot.index < 6)
                        active[static_cast<std::size_t>(slot.index)] = true;
                    else if ((k & 1) && slot.Present()) {
                        foes.push_back(SwitchBattler(slot.mon, -1, true));
                        vs += (vs.empty() ? std::string{} : std::string{ListSep()}) + assets.SpeciesName(slot.mon.species);
                    }
                }
            } else {
                active[0] = true;
                const auto& opponent = snap.switch_foe.valid ? snap.switch_foe : snap.front[1];
                if (opponent.valid && !opponent.is_egg && opponent.hp > 0) {
                    foes.push_back(SwitchBattler(opponent, -1, !snap.switch_foe.valid));
                    vs = assets.SpeciesName(opponent.species);
                }
            }
            std::vector<lp_switch::Battler> bench;
            for (int k = 0; k < snap.own_count && k < 6; ++k) {
                const auto& m = snap.own[static_cast<std::size_t>(k)];
                if (m.valid && !m.is_egg && !active[static_cast<std::size_t>(k)])
                    bench.push_back(SwitchBattler(m, k, false));
            }
            picks = lp_switch::Suggest(bench, foes, &Module::Eff16);
        }
        Text("lp.bt.sw.vs", vs.empty() ? std::string{} : F(K::switch_vs, {{"foe", vs}}));
        I64("lp.bt.sw.none", open && picks.empty() ? 1 : 0);
        for (int k = 0; k < 3; ++k) {
            const std::string p = "lp.bt.sw" + std::to_string(k) + ".";
            const bool on = k < static_cast<int>(picks.size());
            I64(p + "on", on ? 1 : 0);
            if (!on)
                continue;
            const auto& pick = picks[static_cast<std::size_t>(k)];
            const auto& m = snap.own[static_cast<std::size_t>(pick.slot)];
            PublishMon(p, m.species, static_cast<u8>(m.form), m.gender, m.level, m.hp, m.hp_max, 0, m.shiny, OwnNick(m.species),
                       false);
            if constexpr (DebugKeys) { // the ranking and the slot, for console checks
                I64(p + "score", pick.score);
                I64(p + "slot", pick.slot);
            }
            for (int r = 0; r < 3; ++r) {
                const bool has = r < static_cast<int>(pick.reasons.size());
                const auto& reason = has ? pick.reasons[static_cast<std::size_t>(r)] : lp_switch::Reason{};
                const std::string why = has ? ReasonText(reason) : std::string{};
                // a warning in the game's red
                Text(p + "r" + std::to_string(r), has && reason.Warning() ? "{c:#FFC83C32}" + why + "{/c}" : why);
            }
        }
    }

    // ---- double battles (Option C): the 2x2 overview in the header, the focused battler below -----
    // A move is rated against `Target`s: one in a single battle (the foe), in a double the target
    // under the cursor, the focused foe, or both foes.
    struct Target {
        std::string name;
        std::array<int, 3> types;
    };
    struct DoubleView {
        bool dbl = false;
        lp_live::BattleMon own, foe, moves; // the pane's You / Foe pages and the listed moves' owner
        int own_client = 0;                  // the You page's client (2 = a multi-battle partner)
        bool moves_on = true;                // the You page shows the listed moves (its own)
        int moves_key = 0;
        std::vector<Target> targets;
        std::string foe_line;                // a double's Foe page: the move in play against it
    };
    struct Rating {
        std::string text, long_text;
        std::string short_text; // the factor form ("2×", "1× · 2×"): the command page's move chips
        bool good = false;
    };

    bool DoubleOn() const {
        return snap.in_battle && snap.dbl.valid && snap.dbl.is_double;
    }
    // the pane page of a view position: far (odd) = the foe page 0, near = the You page 1
    static int SideOf(int view) {
        return view & 1 ? 0 : 1;
    }
    int FocusSide() const {
        return SideOf(dbl_focus < 0 ? 0 : dbl_focus);
    }
    // focus a battler: both pages show its page (the command page keeps Field when it is open)
    void SetFocus(int view) {
        dbl_focus = view;
        if (tab_fight != 3) // the move page keeps its Move tab
            tab_fight = SideOf(view);
        tab_cmd = SideOf(view);
    }
    static std::string FactorShort(int e) {
        return e == 16 ? std::string{"1×"} : e == 0 ? std::string{"0×"} : FactorText(e);
    }

    Rating Rate(const std::optional<lp_assets::WazaInfo>& info, const std::vector<Target>& targets) const {
        Rating r;
        if (!info)
            return r;
        if (info->category == 0) {
            r.long_text = S(K::status_move);
            if (!targets.empty())
                r.text = r.short_text = S(K::status_short);
            return r;
        }
        if (targets.size() == 1) {
            const int e = Eff16(info->type, targets[0].types);
            if (e < 0)
                return r;
            r.text = EffLabel16(e);
            r.short_text = FactorShort(e);
            r.good = e > 16;
            r.long_text = EffLong(e);
            return r;
        }
        for (const auto& t : targets) {
            const int e = Eff16(info->type, t.types);
            if (e < 0)
                continue;
            const auto f = FactorShort(e);
            r.text += (r.text.empty() ? "" : " · ") + f;
            r.short_text = r.text;
            r.long_text += (r.long_text.empty() ? "" : "  ·  ") + t.name + " " + f;
            r.good = r.good || e > 16;
        }
        return r;
    }

    std::string BattlerName(const lp_live::DoubleSlot& slot) const {
        if (!slot.mon.valid)
            return {};
        return slot.client == 0 ? OwnNick(slot.mon.species) : assets.SpeciesName(slot.mon.species);
    }

    // Publishes the overview cards (lp.bt.d<view>.*), the double tab header binds and the gates,
    // follows the game's choice, and picks the pane's battlers. In a single battle: front[0] /
    // front[1] as before.
    DoubleView PublishDouble() {
        DoubleView v;
        const auto& d = snap.dbl;
        v.dbl = DoubleOn();
        const bool fld = snap.battle_field.valid;
        I64("lp.bt.dbl", v.dbl ? 1 : 0);
        I64("lp.bt.sgl", v.dbl ? 0 : 1);
        I64("lp.bt.tabs.sf", !v.dbl && fld ? 1 : 0);
        I64("lp.bt.tabs.sn", !v.dbl && !fld ? 1 : 0);
        I64("lp.bt.tabs.df", v.dbl && fld ? 1 : 0);
        I64("lp.bt.tabs.dn", v.dbl && !fld ? 1 : 0);
        const auto typed = [this](const lp_live::BattleMon& m) {
            const auto t = BattleTypes(m);
            return m.valid && Eff16(0, t) >= 0;
        };
        if (!v.dbl) {
            dbl_focus = dbl_game = dbl_manual = dbl_last_foe = -1;
            dbl_turn = -2;
            v.own = snap.front[0];
            v.foe = snap.front[1];
            v.moves = snap.front[0];
            v.moves_key = snap.front[0].species;
            if (typed(snap.front[1]))
                v.targets.push_back({std::string{}, BattleTypes(snap.front[1])});
            for (int k = 0; k < 4; ++k)
                I64("lp.bt.d" + std::to_string(k) + ".on", 0);
            return v;
        }
        const auto slot = [&d](int k) -> const lp_live::DoubleSlot& { return d.view[static_cast<std::size_t>(k)]; };
        // the game's choice: the target under the cursor, else the Pokémon choosing its command
        // (kept through the animations); a new turn or a new choice ends a tapped override
        if (snap.battle_field.turn != dbl_turn) {
            dbl_turn = snap.battle_field.turn;
            dbl_game = -1;
        }
        int game = -1;
        if (d.target_open && d.target_single && d.target_view >= 0 && slot(d.target_view).exists)
            game = d.target_view;
        else if (d.proc_view >= 0 && (snap.action.focus || snap.waza.focus || d.target_open))
            game = d.proc_view;
        if (game >= 0 && game != dbl_game) {
            dbl_game = game;
            dbl_manual = -1;
            SetFocus(game);
            ++scene_revision;
        }
        int focus = dbl_manual >= 0 ? dbl_manual : dbl_game;
        if (focus < 0 || !slot(focus).exists) {
            focus = 0;
            for (int k : {0, 2, 1, 3})
                if (slot(k).Present()) {
                    focus = k;
                    break;
                }
            SetFocus(focus);
        }
        dbl_focus = focus;
        // the command page shows the focused battler's page or Field; the move page only the former
        if (tab_fight != 3)
            tab_fight = SideOf(focus);
        if (tab_cmd != 2 || !fld)
            tab_cmd = SideOf(focus);
        if (focus & 1)
            dbl_last_foe = focus;
        // the pane: the focused battler; its counterpart is the one choosing (yours) or the last
        // focused / first standing foe
        const int chooser = d.proc_view >= 0 ? d.proc_view : slot(0).mon.valid ? 0 : 2;
        int foe_view = focus & 1 ? focus : dbl_last_foe;
        if (foe_view < 0 || !slot(foe_view).Present())
            foe_view = slot(1).Present() || !slot(3).Present() ? 1 : 3;
        const int own_view = focus & 1 ? chooser : focus;
        v.own = slot(own_view).mon;
        v.own_client = slot(own_view).client;
        v.foe = slot(foe_view).mon;
        // the listed moves: the move page lists the chooser's (they are the buttons), the command
        // page the focused Pokémon's own (its chips)
        const int moves_view = battle_view == 1 ? chooser : own_view;
        v.moves = slot(moves_view).mon;
        v.moves_on = moves_view == own_view;
        v.moves_key = moves_view * 10000 + v.moves.species;
        // a move in play: the move page's highlighted move, also while its target is picked
        const auto& chooser_mon = slot(chooser).mon;
        const bool move_page = battle_view == 1 && (snap.waza.focus || d.target_open);
        const int in_play = move_page && pick >= 0 && pick < chooser_mon.waza_count ? chooser_mon.waza[pick] : 0;
        const auto play = in_play > 0 ? assets.Waza(in_play) : std::nullopt;
        // the battlers the moves are rated against: the target, else the focused foe, else both
        std::vector<int> against;
        if (d.target_open && d.target_single && d.target_view >= 0)
            against.push_back(d.target_view);
        else if ((focus & 1) && slot(focus).Present() && !d.target_open)
            against.push_back(focus);
        else
            for (int k : {3, 1})
                if (slot(k).Present())
                    against.push_back(k);
        for (int k : against)
            if (typed(slot(k).mon))
                v.targets.push_back({BattlerName(slot(k)), BattleTypes(slot(k).mon)});
        // the Foe page's line: the move in play against this foe
        if (play && play->category != 0 && (focus & 1) &&
            std::find(against.begin(), against.end(), focus) != against.end()) {
            const int e = Eff16(play->type, BattleTypes(slot(focus).mon));
            if (e >= 0)
                v.foe_line = "{c:#FF8A8E96}" + assets.WazaName(in_play) + "{/c}   " + EffLong(e);
        }
        // the cards
        for (int k = 0; k < 4; ++k) {
            const auto& sl = slot(k);
            const std::string p = "lp.bt.d" + std::to_string(k) + ".";
            const auto& m = sl.mon;
            I64(p + "on", sl.exists ? 1 : 0);
            I64(p + "valid", m.valid ? 1 : 0);
            // HP, the fainted dim and "Fainted" follow the top screen's gauge (ShownHp)
            const int hp = ShownHp(m);
            I64(p + "dim", sl.exists && !(m.valid && hp > 0) ? 1 : 0);
            I64(p + "focus", k == focus ? 1 : 0);
            // the accent: the one choosing its command, the target under the cursor
            const bool act = (d.target_open && d.target_single && k == d.target_view) ||
                             (k == d.proc_view && (snap.action.focus || snap.waza.focus || d.target_open));
            I64(p + "act", act ? 1 : 0);
            I64(p + "target", d.target_enabled[k]);
            I64(p + "target.dim", d.target_open && d.target_single && !d.target_enabled[k]);
            Text(p + "name", m.valid ? BattlerName(sl) : std::string{sl.exists ? "—" : ""});
            Text(p + "icon", m.valid ? lp_assets::IconKey(m.species, m.form, m.gender, m.shiny) : std::string{});
            Text(p + "gendericon", m.valid && m.gender < 2 ? lp_assets::UiKey("sharedui", m.gender == 0 ? "cmn_icon_sex_01_00" : "cmn_icon_sex_01_01") : std::string{});
            I64(p + "level", m.valid ? m.level : 0);
            I64(p + "lvon", m.valid ? 1 : 0);
            I64(p + "hp", hp);
            I64(p + "hpmax", m.hp_max);
            const int zone = m.valid ? HpZone(hp, m.hp_max) : 0;
            I64(p + "hpg", zone == 3);
            I64(p + "hpy", zone == 2);
            I64(p + "hpr", zone == 1);
            I64(p + "hpon", m.valid ? 1 : 0); // every card shows its HP (as the single-battle header does for the foe)
            std::string cond = Condition(m);
            if (cond == S(K::healthy))
                cond.clear();
            Text(p + "cond", cond);
            // the move in play against this card (the target's, or each foe's)
            int e = -1;
            if (play && play->category != 0 && sl.Present() &&
                std::find(against.begin(), against.end(), k) != against.end())
                e = Eff16(play->type, BattleTypes(m));
            Text(p + "eff", e >= 0 ? FactorShort(e) : std::string{});
            I64(p + "e.good", e > 16);
            I64(p + "e.even", e == 16);
            I64(p + "e.weak", e > 0 && e < 16);
            I64(p + "e.none", e == 0);
        }
        // the tab header: the focused battler's name, then Field
        Text("lp.bt.dname", BattlerName(slot(focus)));
        I64("lp.bt.dsel0", PaneTab() != 2 && PaneTab() != 3 ? 1 : 0);
        I64("lp.bt.dsel1", PaneTab() == 2 ? 1 : 0);
        return v;
    }

    // ---- the battle pane (Foe / You / Field) ---------------------------------------------------
    // Layout constants shared with gen_manifest.py (battle_pane): px from the pane's content top.
    // Sections after a list of variable length move by the y offsets published here (y_bind), and
    // each page's scroll length is published in 10 px rows for both pane widths: "n" the move page
    // (content 448 px, tabs Foe / You), "w" the command page (652 px, tabs Foe / You / Field, the
    // move chips in the You tab).
    struct PaneGeom {
        const char* sfx;
        int cw, cols;      // content width, type-grid columns (Weak to / Resists / No effect)
        bool chips, field; // move chips in the You tab, the Field tab
    };
    static constexpr PaneGeom PaneGeoms[2] = {{"n", 448, 2, false, false}, {"w", 652, 3, true, true}};
    static constexpr int PanePad = 12, SecH = 50, SecGap = 30, FoeTop = 252, GridDy = 46,
                         StatsTop = 200, OwnStatDy = 52, AccLine = 44, ChipDy = 98,
                         MoveDescY = 470, FieldEffY = 342, FieldEffDy = 44, PartyDy = 80, PartyNameW = 190, FoeMoveDy = 50,
                         BodyScale = 5,
                         PartyDescW = 536; // the party card's ability description (manifest PARTY_DESC_W)
    // Field-page geometry shared with the manifest (Map, Route, Party and Bag).
    static constexpr double MapBasePpw = 1168.0 / 1132.0; // the Town Map's screen px per picture px at zoom 1
    // The Town Map picture (1266x732 px, y down): the place grid's cells are MapCellPx square from
    // (MapX0, MapY0); the map widget's world is the picture with y up (world y = MapPicH - y).
    static constexpr int MapX0 = 226, MapY0 = 30, MapCellPx = 24, MapPicH = 732;
    // the cell under picture point (px, py)
    static std::pair<int, int> CellAt(double px, double py) {
        return {static_cast<int>(std::floor((px - MapX0) / MapCellPx)), static_cast<int>(std::floor((py - MapY0) / MapCellPx))};
    }
    // the picture point at the centre of cell (x, y)
    static std::pair<double, double> CellCentre(int x, int y) {
        return {MapX0 + x * MapCellPx + MapCellPx / 2, MapY0 + y * MapCellPx + MapCellPx / 2};
    }
    static constexpr int ToastWraps[] = {680, 512, 432};
    // How long a toast's text outlives lp.bag.toast.on: longer than the manifest's toast slide-out
    // (gen_manifest.py TOAST_ANIM_MS), so the panel leaves with its message instead of empty.
    static constexpr int ToastTailMs = 250;
    // The game's status tag for a party member (common_lang.<sfx> cmn_ico_txt_sick_01_XX: 01 paralysis,
    // 02 sleep, 03 freeze, 04 burn, 05 poison, 06 bad poison, 07 fainted); none when healthy.
    std::string SickTagKey(int hp, std::uint32_t status) const {
        const int sick = lp_pk8::Sick(status);
        const int n = hp == 0 ? 7 : sick > 0 ? sick : 0;
        return n ? lp_assets::UiKey("common_lang." + lang_sfx, "cmn_ico_txt_sick_01_0" + std::to_string(n)) : std::string{};
    }

    // The Weak to / Resists / No effect grids of the Pokédex Weak. tab and the party card's Weak. tab
    // (manifest WK_*): a section header, then the type tags two to a row; an empty section keeps one
    // row (its "Nothing"). Tops and scroll rects: the Pokédex in screen y, the party card from its top.
    static constexpr int WkSecH = 50, WkRowDy = 50, WkGap = 26, WkTagH = 36, WeakPad = 12,
                         DexWeakTop = 204, DexWeakRect = 190, PartyWeakTop = 444, PartyWeakRect = 392;
    struct WeakGrid { int res_y, imm_y, end; }; // the Resists / No effect headers, the last row's bottom
    // cols: tags to a row (the party card 2, the Pokédex DexWeakCols)
    static constexpr WeakGrid WeakGridAt(int top, int weak, int resist, int immune, int cols = 2) {
        const auto rows = [cols](int n) { return n > cols ? (n + cols - 1) / cols : 1; };
        const int res_y = top + WkSecH + rows(weak) * WkRowDy + WkGap;
        const int imm_y = res_y + WkSecH + rows(resist) * WkRowDy + WkGap;
        return {res_y, imm_y, imm_y + WkSecH + (rows(immune) - 1) * WkRowDy + WkTagH};
    }

    // The tab of the page in view (3 move, 0 foe, 1 you, 2 field): the command page and the move
    // page each keep their own. PaneTabs: the page's tabs in header order (the move page: Move,
    // Foe, You; the command page: Foe, You and Field while the field state is read).
    int& PaneTab() {
        return battle_view == 1 ? tab_fight : tab_cmd;
    }
    std::vector<int> PaneTabs() const {
        if (battle_view == 1)
            return {3, 0, 1};
        return snap.battle_field.valid ? std::vector<int>{0, 1, 2} : std::vector<int>{0, 1};
    }

    // Width (px) of `text` as the runtime measures it (colour tags skipped): exact with the decoded
    // game font, else the glyph-advance estimate (3% wide).
    // The TYPE pill of the game's language.
    std::string TypeTag(int type) const {
        return lp_assets::TypeTagKey(type, lang_sfx);
    }
    // A composed picture with text art (the Pokétch) drawn in the game's language: its own key.
    std::string LangKey(std::string key) const {
        return lang_sfx == "en" ? key : key + "@" + lang_sfx;
    }

    // lp.lang (msg_lang_id of the text in effect), lp.lang.variant (en, pt-BR, fr, de, es-ES,
    // es-419, ko, zh-Hans, zh-Hant, ja, it), lp.lang.mod (a translation mod fills the slot),
    // lp.lang.want (the save's language, while the catalog for it is being built), the title
    // textures of that language and __font_epoch (runtime 18: the font follows the language).
    void PublishLanguage() {
        const auto l = assets.Language();
        lang_sfx = l.sfx;
        if constexpr (DebugKeys) { // lp.lang*: not bound, for console checks
            I64("lp.lang", l.id);
            I64("lp.lang.kanji", l.kanji ? 1 : 0);
            Text("lp.lang.variant", std::string{lp_lang::Code(l.variant)});
            I64("lp.lang.mod", l.modded && lumi == 1 ? 1 : 0); // vanilla BD's own languages are not mods
            I64("lp.lang.want", lang_id);
        }
        Text("lp.tex.logo", "module:lp:tex/startlogo/" + l.logo_sfx);
        if (l.generation != lang_generation || !strings_built) {
            lang_generation = l.generation; // every text is republished below from the new catalog
            ++scene_revision;
            BuildStrings(l.variant);
        }
        PublishStrings(); // before PumpFont: a new catalog's first font build has their glyphs
        assets.PumpFont(tick);
        I64("__font_epoch", assets.FontEpoch());
    }

    // ---- the companion's words (lp_strings_data.h) ------------------------------------------------
    using K = lp_strings::Key;
    const std::string& S(K k) const {
        return strings.Get(static_cast<std::size_t>(k));
    }
    std::string F(K k, std::initializer_list<lp_strings::Slot> slots) const {
        return lp_strings::Fill(S(k), slots);
    }
    // A row by its key name (lp_guest's refusals); "" when there is none.
    std::string SKey(std::string_view key) const {
        for (std::size_t i = 0; i < lp_strings::Rows.size(); ++i)
            if (lp_strings::Rows[i].key == key)
                return strings.Get(i);
        return {};
    }
    // Every row in the catalog's language: the game's labels, else our table, else English. The
    // texts' code points go to the font's glyph set before its first build for the language.
    void BuildStrings(lp_lang::Variant v) {
        strings.Build(lp_strings::Rows, v,
                      [this](std::string_view table, std::string_view label) { return assets.Label(table, label); });
        for (std::size_t i = 0; i < strings.Size(); ++i)
            assets.NoteText(strings.Get(i));
        strings_built = true;
        widths_rev = -1;
        name_pos.clear();
    }
    // lp.t.<key> every sample (the runtime clears the module's values each tick), and the widths the
    // pages position by (lp.t.<key>.w<scale>, lp.t.sum.<name>), measured again when the strings or
    // the decoded font change.
    void PublishStrings() {
        if (host.publish_text)
            for (std::size_t i = 0; i < string_names.size(); ++i)
                host.publish_text(host.userdata, string_names[i].c_str(), strings.Get(i).c_str());
        if (const int rev = text_font_rev.load(); rev != widths_rev) {
            widths_rev = rev;
            width_values.clear();
            const auto width = [this](std::uint16_t key, int scale, int min_scale, int wrap) {
                const auto& text = strings.Get(key);
                return wrap > 0 ? FitWidth(text, scale, min_scale, wrap) : TextWidth(text, scale);
            };
            for (const auto& w : lp_strings::Widths)
                if (w.key < lp_strings::Rows.size())
                    width_values.emplace_back("lp.t." + std::string{lp_strings::Rows[w.key].key} + ".w" +
                                                  std::to_string(w.scale),
                                              width(w.key, w.scale, w.min_scale, w.wrap));
            for (const auto& s : lp_strings::Sums) {
                if (s.name.empty())
                    continue;
                int sum = 0;
                for (const auto k : s.parts)
                    if (k < lp_strings::Rows.size())
                        sum += TextWidth(strings.Get(k), s.scale);
                width_values.emplace_back("lp.t.sum." + std::string{s.name}, sum);
                if (s.limit > 0)
                    width_values.emplace_back("lp.t.sum." + std::string{s.name} + ".fits", sum <= s.limit ? 1 : 0);
            }
        }
        for (const auto& [name, v] : width_values)
            I64(name, v);
    }
    const std::string& AppName(int app) const {
        static_assert(static_cast<int>(K::poketch_app_19) - static_cast<int>(K::poketch_app_0) == 19);
        return S(static_cast<K>(static_cast<int>(K::poketch_app_0) + std::clamp(app, 0, 19)));
    }
    // How a wild Pokémon is met: our words, the move Surf, the rods' and the Poké Radar's names.
    std::string MethodName(lp_assets::Method m) const {
        return lp_dex_text::MethodName(DexCtx(), m); // shared with the Pokédex's Area page
    }
    // "Lv. 3–5" in the language's form ("N. 3–5", "Lv.3–5"); one level when they are the same.
    std::string LevelRange(int low, int high) const {
        return lp_dex_text::LevelRange(DexCtx(), low, high);
    }
    // Between names in a list: " · ", in Chinese "、".
    std::string_view ListSep() const {
        const auto v = strings.Variant();
        return v == lp_lang::Variant::ZhHans || v == lp_lang::Variant::ZhHant ? "、" : " · ";
    }
    std::string ReasonText(const lp_switch::Reason& r) const {
        using R = lp_switch::Reason;
        const auto type = [this](int t) { return assets.TypeName(t); };
        switch (r.kind) {
        case R::Immune: return F(K::switch_immune, {{"type", type(r.type)}});
        case R::Resists: return F(K::switch_resists, {{"type", type(r.type)}});
        case R::Super: return F(K::switch_super, {{"move", r.move.empty() ? type(r.type) : r.move}});
        case R::Faster: return S(K::switch_faster);
        case R::Weak: return F(K::switch_weak, {{"type", type(r.type)}});
        case R::Status: return StatusName(r.status);
        case R::LowHp: return S(K::switch_low_hp);
        default: return S(K::switch_even);
        }
    }
    // The Pokédex's A–Z order in the game's language (name_pos: species -> place, built once per
    // catalog). Latin scripts: accents fold to their letter (lp_strings::FoldKey). Korean: code-point
    // order, which is 가나다 order. Chinese: the game's own order (its search index: pinyin in
    // Simplified, strokes in Traditional) for the 493 species it has; a later species follows the
    // indexed names that start with the same character, else comes last, by code point.
    void SortByName(std::vector<int>& rows) {
        if (name_pos.empty()) {
            const auto v = strings.Variant();
            const bool zh = v == lp_lang::Variant::ZhHans || v == lp_lang::Variant::ZhHant;
            const auto rank = zh ? assets.NameRank() : std::vector<int>{};
            const auto rank_of = [&rank](int id) {
                return id >= 0 && id < static_cast<int>(rank.size()) ? rank[static_cast<std::size_t>(id)] : -1;
            };
            const auto first_cp = [](const std::string& s) {
                std::size_t i = 0;
                return s.empty() ? char32_t{0} : lp_lang::NextCodepoint(s, i);
            };
            struct Entry {
                int group, rank;
                std::string key;
                int id;
            };
            std::vector<Entry> all;
            std::map<char32_t, int> first_rank; // a first character's earliest place in the game's index
            for (int id = 1; id < 2048; ++id) {
                auto name = assets.SpeciesName(id);
                if (name.empty())
                    continue;
                if (const int r = rank_of(id); r >= 0) {
                    const auto [it, fresh] = first_rank.emplace(first_cp(name), r);
                    if (!fresh)
                        it->second = std::min(it->second, r);
                }
                all.push_back({0, 0, zh ? name : lp_strings::FoldKey(name) + '\x01' + name, id});
            }
            if (zh)
                for (auto& e : all) {
                    const auto f = first_rank.find(first_cp(e.key));
                    e.group = f == first_rank.end() ? 1 << 20 : f->second;
                    const int r = rank_of(e.id);
                    e.rank = r >= 0 ? r : 1 << 20;
                }
            std::sort(all.begin(), all.end(), [](const Entry& a, const Entry& b) {
                return std::tie(a.group, a.rank, a.key, a.id) < std::tie(b.group, b.rank, b.key, b.id);
            });
            name_pos.assign(2048, 1 << 20);
            for (std::size_t i = 0; i < all.size(); ++i)
                name_pos[static_cast<std::size_t>(all[i].id)] = static_cast<int>(i);
        }
        std::stable_sort(rows.begin(), rows.end(), [this](int a, int b) {
            const auto pos = [this](int id) { return id > 0 && id < 2048 ? name_pos[static_cast<std::size_t>(id)] : 1 << 20; };
            return pos(a) != pos(b) ? pos(a) < pos(b) : a < b;
        });
    }

    // An exact measure with the decoded font ('w' width, 'h' height wrapped to `width`), memoised
    // until the font changes; nullopt before the runtime has asked for the font.
    std::optional<int> FontMeasure(char kind, const std::string& text, int scale, int width) const {
        std::lock_guard lock{text_font_mutex};
        if (!text_font)
            return std::nullopt;
        std::string key;
        key.reserve(1 + 2 * sizeof(int) + text.size());
        key += kind;
        key.append(reinterpret_cast<const char*>(&scale), sizeof scale);
        key.append(reinterpret_cast<const char*>(&width), sizeof width);
        key += text;
        if (const auto it = text_measures.find(key); it != text_measures.end())
            return it->second;
        const lp_text::Measure m{*text_font};
        const int v = kind == 'w' ? m.Width(text, scale) : m.Height(text, scale, width);
        if (text_measures.size() >= 4096)
            text_measures.clear();
        text_measures.emplace(std::move(key), v);
        return v;
    }

    int TextWidth(const std::string& text, int scale) const {
        if (const auto w = FontMeasure('w', text, scale, 0))
            return *w;
        double n = 0;
        for (size_t i = 0; i < text.size();) {
            if (text.compare(i, 3, "{c:") == 0 || text.compare(i, 4, "{/c}") == 0) {
                const size_t end = text.find('}', i);
                if (end != std::string::npos) { i = end + 1; continue; }
            }
            const auto c = static_cast<unsigned char>(text[i++]);
            if (c >= 32 && c < 127) n += FontAdv[c - 32];
            else if ((c & 0xC0) != 0x80) n += 300;
        }
        return static_cast<int>(n * scale * 1.03 / 100.0 + 0.999);
    }
    // The width a fit_text label (scale `scale` down to `min_scale`, wrap `wrap`) draws `text` at.
    int FitWidth(const std::string& text, int scale, int min_scale, int wrap) const {
        for (int s = scale; s >= min_scale; --s)
            if (const int w = TextWidth(text, s); w <= wrap || s == min_scale)
                return std::min(w, wrap);
        return wrap;
    }

    // Glyph advances of the game font for ASCII 32..126 in 1/100 px per text-scale unit (the runtime
    // draws the font at a cap height of 5 * scale; measured from the font the generator uses, see
    // gen_manifest.text_width). Other code points (é, ’ ...) count as 3.00.
    static constexpr unsigned short FontAdv[95] = {
        161, 135, 199, 334, 289, 429, 369, 120, 211, 211, 237, 299, 164, 208, 147, 238,
        334, 334, 334, 334, 334, 334, 334, 334, 334, 334, 188, 188, 277, 296, 277, 269,
        443, 374, 322, 377, 411, 295, 300, 434, 409, 156, 235, 354, 278, 483, 402, 453,
        303, 453, 328, 287, 318, 397, 372, 533, 377, 359, 338, 206, 238, 206, 249, 238,
        173, 284, 321, 259, 321, 297, 181, 301, 290, 142, 136, 275, 145, 422, 290, 319,
        321, 321, 180, 214, 187, 290, 262, 368, 269, 267, 242, 179, 147, 179, 258};

    // Height (px) of `text` drawn at `scale` wrapped to `width`: greedy word wrap with the font's
    // glyph advances, counted 3% wider so the scroll length never falls short.
    // Colour markup is not drawn; the line pitch is the runtime's scale * 5 + scale * 3.
    // Height (px) of `text` as the runtime lays it out: exact with the decoded game font (lp_text.h),
    // the estimate below until the runtime has asked for the font.
    int TextHeight(const std::string& text, int scale, int width) const {
        if (const auto h = FontMeasure('h', text, scale, width))
            return *h;
        return TextHeightEstimate(text, scale, width);
    }
    static int TextHeightEstimate(const std::string& text, int scale, int width) {
        std::string plain;
        for (size_t i = 0; i < text.size();) {
            if (text.compare(i, 3, "{c:") == 0 || text.compare(i, 4, "{/c}") == 0) {
                const size_t end = text.find('}', i);
                if (end != std::string::npos) {
                    i = end + 1;
                    continue;
                }
            }
            plain += text[i++];
        }
        if (plain.empty())
            return 0;
        const double unit = scale * 1.03 / 100.0;
        const auto advance = [&plain, unit](size_t a, size_t b) {
            int n = 0;
            for (size_t i = a; i < b; ++i) {
                const auto c = static_cast<unsigned char>(plain[i]);
                if (c >= 32 && c < 127)
                    n += FontAdv[c - 32];
                else if ((c & 0xC0) != 0x80)
                    n += 300; // the lead byte of a UTF-8 code point
            }
            return n * unit;
        };
        const double ch = FontAdv[0] * unit; // a space
        int lines = 0;
        for (size_t pos = 0;;) {
            const size_t nl = std::min(plain.find('\n', pos), plain.size());
            double w = 0;
            int n = 1;
            for (size_t at = pos; at < nl;) {
                size_t sp = plain.find(' ', at);
                if (sp == std::string::npos || sp > nl)
                    sp = nl;
                const double ww = advance(at, sp);
                if (ww > 0) {
                    if (w > 0 && w + ch + ww > width) {
                        ++n;
                        w = ww;
                    } else {
                        w += (w > 0 ? ch : 0) + ww;
                    }
                    for (; w > width; w -= width)
                        ++n; // a word longer than the line
                }
                at = sp + 1;
            }
            lines += n;
            if (nl >= plain.size())
                break;
            pos = nl + 1;
        }
        return (lines - 1) * scale * 8 + scale * 5;
    }

    // Types as the battle has them: the reader's (Soak, Roost, Burn Up; 18 = none) when read, else
    // the species'; [2] is a third type (Forest's Curse, Trick-or-Treat) or -1.
    std::array<int, 3> BattleTypes(const lp_live::BattleMon& m) const {
        std::array<int, 3> t{-1, -1, -1};
        if (!m.valid)
            return t;
        if (m.types[0] >= 0 && m.types[0] <= 18 && m.types[1] >= 0 && m.types[1] <= 18) {
            t[0] = m.types[0];
            t[1] = m.types[1];
        } else if (const auto s = assets.SpeciesTypes(m.species, m.form)) {
            t[0] = s->first;
            t[1] = s->second;
        }
        if (m.types_raw[2] >= 0 && m.types_raw[2] < 18)
            t[2] = m.types_raw[2];
        return t;
    }

    // Effectiveness of an attack type on battle types, in sixteenths: 16 neutral, 32 / 64 / 128
    // super effective, 8 / 4 / 2 resisted, 0 no effect; -1 when the types are unknown. Typeless (both
    // 18) takes neutral damage.
    static int Eff16(int attack, const std::array<int, 3>& t) {
        if (!IsType(attack) || t[0] < 0 || t[1] < 0)
            return -1;
        const int q = IsType(t[0]) ? lp_assets::Effectiveness(attack, t[0], t[1])
                    : IsType(t[1]) ? lp_assets::Effectiveness(attack, t[1], t[1])
                                   : 4;
        const int r = IsType(t[2]) && t[2] != t[0] && t[2] != t[1] ? lp_assets::Effectiveness(attack, t[2], t[2]) : 4;
        return q * r;
    }
    const std::string& EffLabel16(int e) const {
        return S(e == 0 ? K::no_effect : e > 16 ? K::eff_super : e < 16 ? K::eff_not_very : K::eff_neutral);
    }
    // "2× Super effective"; "No effect" / "Neutral" without a factor
    std::string EffLong(int e) const {
        return e == 0 || e == 16 ? EffLabel16(e) : FactorText(e) + " " + EffLabel16(e);
    }
    static std::string FactorText(int e) {
        switch (e) {
        case 128: return "8×";
        case 64: return "4×";
        case 32: return "2×";
        case 8: return "½×";
        case 4: return "¼×";
        case 2: return "1/8×"; // the font has no ⅛
        default: return {};
        }
    }

    // A battle stat: "—" unknown, "raw » staged" when its stage changes it, else raw.
    static std::string StatText(int raw, int staged) {
        return raw < 0 ? "—" : staged >= 0 && staged != raw ? std::to_string(raw) + " » " + std::to_string(staged)
                                                            : std::to_string(raw);
    }

    // a type number with a tag (0..17; 18 is "none", -1 unknown)
    static constexpr bool IsType(int v) {
        return v >= 0 && v < 18;
    }
    // ty1 / ty2: the first two battle types' tags (the second only when it differs)
    void TypeTags12(const std::string& p, const std::array<int, 3>& t) {
        Text(p + "ty1", IsType(t[0]) ? TypeTag(t[0]) : std::string{});
        Text(p + "ty2", IsType(t[1]) && t[1] != t[0] ? TypeTag(t[1]) : std::string{});
    }
    void PaneTypeTags(const std::string& p, const std::array<int, 3>& t) {
        TypeTags12(p, t);
        const bool third = IsType(t[2]) && t[2] != t[0] && t[2] != t[1];
        Text(p + "ty3", third ? TypeTag(t[2]) : std::string{});
        I64(p + "ty3on", third ? 1 : 0); // the narrow pane draws three smaller tags
    }

    // "Burned · Confused" (colour markup); empty when the reader has no status.
    std::string Condition(const lp_live::BattleMon& m) const {
        if (!m.valid)
            return {};
        std::string s;
        if (ShownHp(m) == 0) { // fainted once the top screen's gauge is empty
            s = "{c:#FF7A7A7A}" + S(K::fainted) + "{/c}";
        } else {
            static constexpr const char* Colours[] = {"", "#FFC89A00", "#FF6E7486", "#FF2E8CC0", "#FFE0602C",
                                                      "#FF9050B0"};
            if (m.status >= 1 && m.status <= 5)
                s = std::string{"{c:"} + Colours[m.status] + "}" +
                    (m.status == 5 && m.toxic ? S(K::status_bad_poison) : StatusName(m.status)) + "{/c}";
            if (m.confused)
                s += std::string{s.empty() ? "" : " · "} + "{c:#FFB04080}" + S(K::status_confused) + "{/c}";
            if (m.volatile_on[11]) // Taunt is useful beside the immediately visible status line.
                s += std::string{s.empty() ? "" : " · "} + "{c:#FFB04080}" + assets.WazaName(269) + "{/c}";
            if (s.empty() && m.status == 0)
                s = S(K::healthy);
        }
        return s;
    }
    std::string VolatileEffects(const lp_live::BattleMon& m) const {
        if (!m.valid || m.is_egg || ShownHp(m) == 0) return {};
        // Native WAZASICK slots, named through the installed game's localized move table.
        // Do not show internal locking/target bookkeeping as player-facing conditions.
        static constexpr std::pair<int, int> Moves[] = {
            {7,213}, {9,171}, {10,174}, {11,269}, {12,259}, {13,50}, {14,281},
            {15,377}, {16,380}, {17,193}, {18,73}, {19,373}, {20,195}, {21,275},
            {22,335}, {23,227}, {24,675}, {25,355}, {33,477}, {36,392},
            {38,600}, {39,253}, {41,673}, {42,749}, {43,753}, {44,746}};
        std::string text;
        for (const auto& [slot, move] : Moves) {
            if (!m.volatile_on[slot]) continue;
            const auto name = assets.WazaName(move);
            if (!name.empty()) text += (text.empty() ? "" : "\n") + name;
        }
        return text;
    }
    // A major status by the battle's numbering (1 paralysis, 2 sleep, 3 freeze, 4 burn, 5 poison).
    const std::string& StatusName(int status) const {
        static constexpr K Names[] = {K::healthy, K::status_paralyzed, K::status_asleep, K::status_frozen,
                                      K::status_burned, K::status_poisoned};
        return S(Names[status >= 0 && status <= 5 ? status : 0]);
    }

    // Stage k (0 atk .. 4 spe, 5 accuracy, 6 evasion) as "+2" / "-1"; empty for 0 or unknown.
    static std::string StageText(const lp_live::BattleMon& m, int k) {
        if (!m.stages_ok || k < 0 || k > 6 || m.stages[k] == 0)
            return {};
        return (m.stages[k] > 0 ? "+" : "") + std::to_string(m.stages[k]);
    }
    void PublishStage(const std::string& q, const lp_live::BattleMon& m, int k) {
        const auto s = StageText(m, k);
        Text(q + "stage", s);
        I64(q + "up", !s.empty() && m.stages[k] > 0);
        I64(q + "down", !s.empty() && m.stages[k] < 0);
    }
    std::string AccEvLine(const lp_live::BattleMon& m) const {
        std::string s;
        for (int k : {5, 6}) {
            const auto t = StageText(m, k);
            if (t.empty())
                continue;
            if (!s.empty())
                s += "   ";
            s += "{c:#FF5A6070}" + S(k == 5 ? K::stage_accuracy : K::evasion) + "{/c} " + t;
        }
        return s;
    }

    // "{c:dark}Name{/c}\nwhat it does" (the Pokédex Stats page's recipe)
    std::string AbilityText(int ability, bool hidden = false) const {
        const auto name = assets.AbilityName(ability);
        return "{c:#FF203040}" + (hidden ? F(K::hidden_ability, {{"ability", name}}) : name) + "{/c}\n" +
               assets.AbilityDescription(ability);
    }

    const lp_pk8::Mon* PartyCopy(u16 species) const {
        for (const auto& pm : snap.party)
            if (pm.valid && !pm.mon.is_egg && pm.mon.species == species)
                return &pm.mon;
        return nullptr;
    }

    // The HP to draw for a battler: what the top screen's gauge shows now (it drains after the
    // hit), else the real HP (no status window for it).
    static int ShownHp(const lp_live::BattleMon& m) {
        return m.hp_view >= 0 ? m.hp_view : m.hp;
    }

    // the game's three gauge colours: 3 green (> 1/2), 2 yellow (> 1/5), 1 red, 0 empty
    static int HpZone(int hp, int hp_max) {
        return hp == 0 ? 0 : (hp * 2 > hp_max ? 3 : (hp * 5 > hp_max ? 2 : 1));
    }

    std::string TurnsLeft(int n) const {
        return F(lp_strings::PluralOne(strings.Variant(), n) ? K::turns_left_one : K::turns_left_other,
                 {{"n", std::to_string(n)}});
    }

    // The condition and the held item on one line ("Healthy · [icon] Razor Claw", "Asleep · No held
    // item"): p+"cond" is the text before the item, p+"itemx" the item icon's x offset after it.
    void PublishCondItem(const std::string& p, const lp_live::BattleMon& m) {
        const std::string cond = Condition(m);
        const int item = m.valid ? m.item : 0;
        const std::string item_name = item > 0 ? assets.ItemName(item) : std::string{};
        const std::string sep = cond.empty() ? std::string{} : std::string{"{c:#FF8A8E96}  ·  {/c}"};
        const bool has = m.valid && item > 0;
        const std::string line = !m.valid ? std::string{}
                                 : has    ? cond + sep
                                          : cond + sep + "{c:#FF8A8E96}" + S(K::no_held_item) + "{/c}";
        Text(p + "cond", line);
        Text(p + "itemicon", has ? lp_assets::ItemKey(item) : std::string{});
        Text(p + "itemname", has ? item_name : std::string{});
        // per pane width: the largest scale (5, 4, 3) at which the line fits (icon 8 * scale + 2 px
        // before the item name), and the icon's x after the condition at that scale
        for (const auto& g : PaneGeoms) {
            int scale = 3;
            for (int sc = BodyScale; sc > 3; --sc) {
                const int wid = TextWidth(line, sc) + (has ? 8 * sc + 2 + TextWidth(item_name, sc) : 0);
                if (wid <= g.cw) { scale = sc; break; }
            }
            const std::string s = g.sfx;
            for (int sc = 3; sc <= 5; ++sc)
                I64(p + "cs" + std::to_string(sc) + "." + s, m.valid && sc == scale ? 1 : 0);
            I64(p + "itemx." + s, has && !cond.empty() ? TextWidth(line, scale) : 0);
        }
    }

    // One party row of the Field tab (yours or the opponent's): icon, name, type stamps after the
    // name (x offset p+"tx"), HP, level, dimmed when fainted.
    void PublishPartyRow(const std::string& q, const lp_live::BattleMon& pm, const std::string& name) {
        I64(q + "egg", pm.is_egg);
        I64(q + "details", !pm.is_egg);
        Text(q + "gendericon", pm.valid && !pm.is_egg && pm.gender < 2 ?
            lp_assets::UiKey("sharedui", pm.gender == 0 ? "cmn_icon_sex_01_00" : "cmn_icon_sex_01_01") : "");
        if (pm.is_egg) {
            Text(q + "icon", lp_assets::UiKey("texturemass", "pm0000_00_00_00"));
            Text(q + "name", S(K::party_egg));
            Text(q + "ty1", ""); Text(q + "ty2", "");
            I64(q + "hp", 0); I64(q + "hpmax", 0); I64(q + "level", 0);
            I64(q + "hpg", 0); I64(q + "hpy", 0); I64(q + "hpr", 0); I64(q + "fainted", 0);
            return;
        }
        Text(q + "icon", pm.valid ? lp_assets::IconKey(pm.species, pm.form, pm.gender, pm.shiny) : std::string{});
        Text(q + "name", pm.valid ? name : std::string{});
        I64(q + "tx", pm.valid ? FitWidth(name, BodyScale, 3, PartyNameW) + 12 : 0);
        TypeTags12(q, BattleTypes(pm));
        I64(q + "hp", pm.hp);
        I64(q + "hpmax", pm.hp_max);
        I64(q + "level", pm.level);
        const int zone = HpZone(pm.hp, pm.hp_max);
        I64(q + "hpg", zone == 3);
        I64(q + "hpy", zone == 2);
        I64(q + "hpr", zone == 1);
        I64(q + "fainted", pm.valid && pm.hp == 0);
    }

    // moves_on: the You page lists `own`'s moves (a double's move page lists only the chooser's);
    // foe_line replaces the Foe page's speed line (a double: the move in play against this foe).
    // mover: the listed moves' owner (the move card's move and STAB; a double's move page: the chooser,
    // also while the pane shows another battler).
    void PublishPane(const lp_live::BattleMon& own, const lp_live::BattleMon& foe, const lp_live::BattleMon& mover,
                     int shown, bool moves_on,
                     const std::string& foe_line) {
        const auto& fld = snap.battle_field;
        int& tab = PaneTab();
        {
            const auto order = PaneTabs();
            if (std::find(order.begin(), order.end(), tab) == order.end())
                tab = order.front();
        }
        for (int k = 0; k < 4; ++k)
            I64("lp.bt.t" + std::to_string(k), tab == k ? 1 : 0);
        I64("lp.bt.seq", battle_seq);
        const auto rows = [](int px) { return (px + 9) / 10; };
        const auto own_t = BattleTypes(own), foe_t = BattleTypes(foe);
        static constexpr K StatNames[6] = {K::hp, K::stat_attack, K::stat_defense, K::stat_sp_atk, K::stat_sp_def,
                                           K::stat_speed};

        // Foe: types (+ the caught ball), condition and item, speed, matchups, battle stats + stages,
        // its moves, ability
        const std::string f = "lp.bt.foe.";
        const bool foe_on = tab == 0;
        I64(f + "rk", (foe.valid ? foe.species : 0) * 8 + (DoubleOn() ? 4 + (dbl_focus & 3) : 0));
        PaneTypeTags(f, foe_t);
        PublishCondItem(f, foe);
        if (foe.valid && (foe.species != foe_dex_species || (foe_caught < 0 && tick > foe_dex_tick + 60))) {
            foe_dex_species = foe.species;
            foe_dex_tick = tick;
            const auto st = live.DexStatuses(lumi == 1);
            foe_caught = foe.species < st.size() ? (st[foe.species] == 3 ? 1 : 0) : -1;
        }
        I64(f + "caughtv", foe.valid && foe_caught == 1);
        {
            // who moves first among same-priority moves: speed after stages, halved by paralysis,
            // reversed by Trick Room (items, abilities and priority are not counted)
            std::string speed;
            if (own.valid && foe.valid && own.staged[4] >= 0 && foe.staged[4] >= 0) {
                const double a = own.staged[4] * (own.status == 1 ? 0.5 : 1.0);
                const double b = foe.staged[4] * (foe.status == 1 ? 0.5 : 1.0);
                const bool trick = fld.valid && fld.effect_on[lp_live::EffTrickRoom];
                speed = "{c:#FF8A8E96}" + S(K::stat_speed) + "{/c}   " +
                        S(a == b ? K::speed_tie : ((a > b) != trick) ? K::speed_you_first : K::speed_foe_first) +
                        (trick ? F(K::speed_trick_room, {{"effect", S(K::effect_trick_room)}}) : std::string{});
            }
            Text(f + "speed", foe_line.empty() ? speed : foe_line);
        }
        int nw = 0, nr = 0, ni = 0;
        const bool foe_typed = foe.valid && Eff16(0, foe_t) >= 0;
        if (foe_typed)
            for (int attack = 0; attack < 18; ++attack) {
                const int e = Eff16(attack, foe_t);
                const auto tag = TypeTag(attack);
                if (e > 16) {
                    const auto q = f + "wk" + std::to_string(nw++) + ".";
                    Text(q + "tag", tag);
                    Text(q + "f", FactorText(e));
                    I64(q + "strong", e >= 64); // 4× and up, drawn bold
                } else if (e == 0) {
                    Text(f + "im" + std::to_string(ni++) + ".tag", tag);
                } else if (e >= 0 && e < 16) {
                    const auto q = f + "rs" + std::to_string(nr++) + ".";
                    Text(q + "tag", tag);
                    Text(q + "f", FactorText(e));
                    I64(q + "strong", 0); // resists are never bold (only 4× is)
                }
            }
        // list counts are 0 while the tab is not shown (the lists' tab gate)
        I64(f + "wk.count", foe_on ? nw : 0);
        I64(f + "rs.count", foe_on ? nr : 0);
        I64(f + "im.count", foe_on ? ni : 0);
        I64(f + "wk.none", foe_typed && nw == 0);
        I64(f + "rs.none", foe_typed && nr == 0);
        I64(f + "im.none", foe_typed && ni == 0);
        // its battle stats: HP, then each stat before its stage and after it (as the You page)
        for (int k = 0; k < 6; ++k) {
            const auto q = f + "st" + std::to_string(k) + ".";
            Text(q + "name", S(StatNames[k]));
            std::string v;
            if (k == 0) {
                v = std::to_string(ShownHp(foe)) + "/" + std::to_string(foe.hp_max);
            } else {
                v = StatText(foe.stats[k - 1], foe.staged[k - 1]);
            }
            Text(q + "v", v);
            PublishStage(q, foe, k - 1); // HP has no stage
        }
        I64(f + "st.count", foe_on && foe.valid ? 6 : 0);
        // its moves: type, name, how each hits the Pokémon on the You page, PP
        int nm = 0;
        for (int k = 0; foe.valid && k < foe.waza_count && k < 4; ++k) {
            if (foe.waza[k] <= 0)
                continue;
            const auto q = f + "mv" + std::to_string(nm++) + ".";
            PublishMoveRow(q, foe.waza[k], foe.pp[k], foe.pp_max[k]);
            const auto wi = assets.Waza(foe.waza[k]);
            const int e = wi && wi->category != 0 && own.valid ? Eff16(wi->type, own_t) : -1;
            Text(q + "eff", e >= 0 ? FactorShort(e) : std::string{});
            I64(q + "effgood", e > 16);
        }
        I64(f + "mv.count", foe_on ? nm : 0);
        I64(f + "mv.on", foe_on && nm > 0);
        const auto foe_accev = AccEvLine(foe);
        Text(f + "accev", foe_accev);
        I64(f + "accev.on", !foe_accev.empty());
        std::string foe_ab;
        const bool ab_known = foe.valid && foe.ability > 0;
        if (ab_known) {
            foe_ab = AbilityText(foe.ability);
        } else if (foe.valid) {
            const auto ab = assets.SpeciesAbilities(foe.species, foe.form);
            for (int k = 0; k < 3; ++k) {
                if (ab[k] <= 0 || std::find(ab.begin(), ab.begin() + k, ab[k]) != ab.begin() + k)
                    continue;
                foe_ab += (foe_ab.empty() ? "" : "\n\n") + AbilityText(ab[k], k == 2);
            }
        }
        Text(f + "ab.text", foe_ab);
        const auto foe_effects = VolatileEffects(foe);
        Text(f + "effects", foe_effects);
        I64(f + "effects.on", !foe_effects.empty());
        I64(f + "ab.known", ab_known);
        for (const auto& g : PaneGeoms) {
            const auto r = [&g](int n) { return std::max(1, (n + g.cols - 1) / g.cols); };
            const int y1 = FoeTop + SecH + r(nw) * GridDy + SecGap;
            const int y2 = y1 + SecH + r(nr) * GridDy + SecGap;
            const int y3 = y2 + SecH + r(ni) * GridDy + SecGap;
            const int y5 = y3 + SecH + 6 * OwnStatDy + (foe_accev.empty() ? 0 : AccLine) + SecGap;
            const int y4 = nm > 0 ? y5 + SecH + nm * FoeMoveDy + SecGap : y5;
            const std::string s = g.sfx;
            I64(f + "y5." + s, y5);
            I64(f + "y1." + s, y1);
            I64(f + "y2." + s, y2);
            I64(f + "y3." + s, y3);
            I64(f + "y4." + s, y4);
            const int effects_y = y4 + SecH + TextHeight(foe_ab, BodyScale, g.cw) + SecGap;
            I64(f + "effects.y." + s, effects_y);
            I64("lp.bt.rows.foe." + s, rows(PanePad + effects_y +
                (foe_effects.empty() ? 0 : SecH + TextHeight(foe_effects, BodyScale, g.cw)) + 24));
        }

        // You: types, condition and item, battle stats with stages (one column), the moves (chips
        // on the command page) and the picked move's details, ability
        const std::string o = "lp.bt.own.";
        const bool own_on = tab == 1;
        const auto* copy = own.valid ? PartyCopy(own.species) : nullptr;
        I64(o + "rk", (own.valid ? own.species : 0) * 8 + (DoubleOn() ? 4 + (dbl_focus & 3) : 0));
        PaneTypeTags(o, own_t);
        PublishCondItem(o, own);
        for (int k = 0; k < 6; ++k) {
            const auto q = o + "st" + std::to_string(k) + ".";
            Text(q + "name", S(StatNames[k]));
            std::string v;
            if (k == 0) {
                v = std::to_string(ShownHp(own)) + "/" + std::to_string(own.hp_max);
            } else {
                // battle stat before its stage (the party copy's when the reader has none), then after
                int raw = own.stats[k - 1];
                if (raw < 0 && copy)
                    raw = std::array<int, 5>{copy->atk, copy->def, copy->spa, copy->spd, copy->spe}[k - 1];
                v = StatText(raw, own.staged[k - 1]);
            }
            Text(q + "v", v);
            PublishStage(q, own, k - 1);
        }
        I64(o + "st.count", own_on && own.valid ? 6 : 0);
        const auto own_accev = AccEvLine(own);
        Text(o + "accev", own_accev);
        I64(o + "accev.on", !own_accev.empty());
        // the move part: the picked (or first) move; chips for every move on the command page
        const int waza = shown >= 0 && shown < mover.waza_count ? mover.waza[shown] : 0;
        const auto info = waza > 0 ? assets.Waza(waza) : std::nullopt;
        const bool typed = info && info->type >= 0 && info->type < 18;
        const auto mover_t = BattleTypes(mover);
        I64("lp.bt.pk.ok", typed ? 1 : 0);
        I64("lp.bt.pk.stab", typed && info->category != 0 &&
                                 std::find(mover_t.begin(), mover_t.end(), info->type) != mover_t.end());
        // the moves listed on the You page (the command page; the move page has its Move tab)
        const int wc = moves_on && battle_view == 0 ? own.waza_count : 0;
        I64("lp.bt.chip.count", own_on ? wc : 0);
        I64("lp.bt.mv.on", own_on && wc > 0 ? 1 : 0);
        for (int k = 0; k < 4; ++k)
            I64("lp.bt.m" + std::to_string(k) + ".shown", wc > 0 && k == shown);
        const auto desc = waza > 0 ? assets.WazaDescription(waza) : std::string{};
        Text("lp.bt.pk.desc", desc);
        // the move page's Move tab: the highlighted move's card from the top (a new move scrolls back)
        I64("lp.bt.pk.rk", waza * 8 + (shown & 7));
        I64("lp.bt.rows.move.n", rows(PanePad + MoveDescY + TextHeight(desc, BodyScale, PaneGeoms[0].cw) + 24));
        const int ym = StatsTop + SecH + 6 * OwnStatDy + (own_accev.empty() ? 0 : AccLine) + SecGap;
        I64(o + "ym", ym);
        const int own_ability = own.ability > 0 ? own.ability : copy ? copy->ability : 0;
        const auto own_ab = own_ability > 0 ? AbilityText(own_ability) : std::string{};
        Text(o + "ab.text", own_ab);
        const auto own_effects = VolatileEffects(own);
        Text(o + "effects", own_effects);
        I64(o + "effects.on", !own_effects.empty());
        for (const auto& g : PaneGeoms) {
            const std::string s = g.sfx;
            const int chips = g.chips ? std::max(1, (wc + 1) / 2) * ChipDy + 6 : 0;
            const int yd = ym + SecH + chips;
            const int ya = wc > 0 ? yd + MoveDescY + TextHeight(desc, BodyScale, g.cw) + 8 + SecGap : ym;
            I64(o + "yd." + s, yd);
            I64(o + "ya." + s, ya);
            const int effects_y = ya + SecH + TextHeight(own_ab, BodyScale, g.cw) + SecGap;
            I64(o + "effects.y." + s, effects_y);
            I64("lp.bt.rows.own." + s, rows(PanePad + effects_y +
                (own_effects.empty() ? 0 : SecH + TextHeight(own_effects, BodyScale, g.cw)) + 24));
        }

        // Field (command page only): turn, weather, terrain, field effects, your party, the
        // opponent's party (a trainer battle, or more than one opponent)
        const std::string fl = "lp.bt.fld.";
        const bool fld_on = tab == 2;
        // Active battlefield positions, rather than the first four entries in the full rosters.
        for (int i = 0; i < 4; ++i) {
            static constexpr int Views[] = {0, 2, 1, 3};
            const lp_live::BattleMon* mon = nullptr;
            if (DoubleOn()) {
                const auto& slot = snap.dbl.view[Views[i]];
                if (slot.Present()) mon = &slot.mon;
            } else if ((i == 0 || i == 2) && snap.front[i / 2].valid && snap.front[i / 2].hp > 0) {
                mon = &snap.front[i / 2];
            }
            const auto q = fl + "active" + std::to_string(i) + ".";
            I64(q + "on", mon && !mon->is_egg);
            Text(q + "icon", mon && !mon->is_egg ? lp_assets::IconKey(mon->species, mon->form, mon->gender, mon->shiny) : "");
            Text(q + "gendericon", mon && !mon->is_egg && mon->gender < 2 ? lp_assets::UiKey("sharedui", mon->gender == 0 ? "cmn_icon_sex_01_00" : "cmn_icon_sex_01_01") : "");
            Text(q + "name", mon && !mon->is_egg ? (i < 2 ? OwnNick(mon->species) : assets.SpeciesName(mon->species)) : "");
        }
        // BATTLE_TURN_COUNT counts finished turns (0 on the first command menu; not checked live)
        Text(fl + "turn", fld.turn >= 0 ? F(K::turn, {{"n", std::to_string(fld.turn + 1)}}) : std::string{});
        {
            static constexpr K Weather[] = {K::weather_none,       K::weather_sun,         K::weather_rain,
                                            K::weather_hail,       K::weather_sand,        K::weather_heavy_rain,
                                            K::weather_extreme_sun, K::weather_strong_winds};
            static constexpr K Terrain[] = {K::terrain_none, K::terrain_grassy, K::terrain_misty, K::terrain_electric,
                                            K::terrain_psychic};
            std::string w, t;
            if (fld.valid && fld.weather >= 0 && fld.weather <= 7) {
                w = S(Weather[fld.weather]);
                if (fld.weather > 0 && fld.weather_turns > 0 && fld.weather_turns < 255)
                    w += " · " + TurnsLeft(fld.weather_turns);
            }
            if (fld.valid && fld.terrain >= 0 && fld.terrain <= 4) {
                t = S(Terrain[fld.terrain]);
                if (fld.terrain > 0 && fld.terrain_turns > 0)
                    t += " · " + TurnsLeft(fld.terrain_turns);
            }
            Text(fl + "weather", w);
            Text(fl + "terrain", t);
        }
        static constexpr std::pair<int, K> Effects[] = {
            {1, K::effect_trick_room}, {2, K::effect_gravity},    {3, K::effect_imprison},   {4, K::effect_wonder_room},
            {5, K::effect_magic_room}, {6, K::effect_ion_deluge}, {7, K::effect_fairy_lock},
            {9, K::effect_neutralizing_gas}};
        int ne = 0;
        if (fld.valid)
            for (const auto& [e, name] : Effects) {
                if (!fld.effect_on[e])
                    continue;
                const auto q = fl + "e" + std::to_string(ne++) + ".";
                Text(q + "name", S(name));
                Text(q + "turns", fld.effect_turns[e] > 0 ? TurnsLeft(fld.effect_turns[e]) : S(K::effect_active));
            }
        I64(fl + "e.count", fld_on ? ne : 0);
        I64(fl + "e.none", fld.valid && ne == 0);
        const int yp = FieldEffY + std::max(1, ne) * FieldEffDy + SecGap;
        I64(fl + "yp", yp);
        const int party = std::clamp(snap.own_count, 0, 6);
        for (int i = 0; i < party; ++i) {
            const auto& pm = snap.own[i];
            PublishPartyRow("lp.bt.pt" + std::to_string(i) + ".", pm,
                            OwnNick(pm.species));
        }
        I64("lp.bt.pt.rows", fld_on ? party : 0);
        // the opponent's party: listed in a trainer battle (the game turns the Poké Ball command off,
        // BUIActionList._isBallEnable) or whenever the opponent side has more than one member
        const int foes = static_cast<int>(std::min<std::size_t>(snap.opponents.size(), 12));
        const bool trainer = !snap.ball_enabled || foes > 1;
        for (int i = 0; i < foes; ++i) {
            const auto& pm = snap.opponents[static_cast<std::size_t>(i)];
            PublishPartyRow("lp.bt.ep" + std::to_string(i) + ".", pm,
                            assets.SpeciesName(pm.species));
        }
        const int ep = trainer ? foes : 0;
        I64("lp.bt.ep.rows", fld_on ? ep : 0);
        I64(fl + "ep.on", fld_on && ep > 0 ? 1 : 0);
        const int ye = yp + SecH + party * PartyDy + SecGap;
        I64(fl + "ye", ye);
        for (const auto& g : PaneGeoms)
            I64(std::string{"lp.bt.rows.field."} + g.sfx,
                rows(PanePad + (ep > 0 ? ye + SecH + ep * PartyDy : ye - SecGap) + SecGap));

        // anything that moves the pane's sections: repaint the whole card once
        const std::string key = std::to_string(tab) + "/" + std::to_string(shown) + "/" +
                                std::to_string(own.species) + "/" + std::to_string(foe.species) + "/" +
                                std::to_string(nw) + "." + std::to_string(nr) + "." + std::to_string(ni) + "/" +
                                std::to_string(ne) + "/" + std::to_string(party) + "." + std::to_string(ep) + "/" +
                                std::to_string(foe_accev.size()) + "." + std::to_string(own_accev.size()) + "/" +
                                std::to_string(fld.valid) + "/" + std::to_string(own.waza_count) + "/" +
                                std::to_string(own.item) + "." + std::to_string(foe.item) + "/" +
                                std::to_string(foe_caught) + "/" + std::to_string(dbl_focus) + "/" +
                                std::to_string(moves_on) + "/" + std::to_string(foe_line.size()) + "/" +
                                own_effects + "/" + foe_effects;
        if (key != last_pane_key) {
            last_pane_key = key;
            ++scene_revision;
        }
    }

    // Independent companion views: read inventory/records; never open the native X menu.
    // The Bag page (field view 5).
    void PublishBag() {
        static constexpr K PocketNames[] = {K::pocket_medicine, K::pocket_balls, K::pocket_battle, K::pocket_berries,
                                            K::pocket_other, K::pocket_tms, K::pocket_treasures, K::pocket_key,
                                            K::pocket_extra};
        // Luminescent Platinum: 9 tabs (Extra Items last); vanilla Brilliant Diamond: the game's 8
        const bool lp_tabs = lumi != 0;
        bag_pocket = lp_vanilla::ClampTab(bag_pocket, lp_tabs);
        // Inventory/catalog changes invalidate the sorted row list; selection alone does not.
        if (bag_rows_revision != bag_revision || bag_rows_pocket != bag_pocket ||
            bag_rows_generation != lang_generation) {
            std::vector<std::pair<int,int>> rows;
            for (int id = 1; id < static_cast<int>(bag.size()); ++id) {
                if (bag[id] <= 0) continue;
                const auto data = assets.Item(id);
                if (data && data->pocket == lp_vanilla::BagFieldPockets[bag_pocket]) rows.emplace_back(data->sort, id);
            }
            std::sort(rows.begin(), rows.end());
            bag_rows.clear();
            for (const auto& row : rows) bag_rows.push_back(row.second);
            bag_rows_revision = bag_revision;
            bag_rows_pocket = bag_pocket;
            bag_rows_generation = lang_generation;
        }
        bag_selected=bag_rows.empty()?0:std::clamp(bag_selected,0,static_cast<int>(bag_rows.size())-1);
        I64("lp.bag.pocket.id",bag_pocket);
        I64("lp.bag.count",bag_rows.size());I64("lp.bag.empty",bag_rows.empty());
        Text("lp.bag.pocket",S(PocketNames[bag_pocket]));
        for (int i=0;i<9;++i) {I64("lp.bag.tab"+std::to_string(i)+".on",i==bag_pocket);}
        // the header's two layouts (lp.bag.l9 / lp.bag.l8): only the game's own shows its tiles
        for (const int n : {9, 8}) {
            const bool shown = lp_vanilla::BagTabs(lp_tabs) == n;
            const std::string l = "lp.bag.l" + std::to_string(n);
            I64(l, shown);
            for (int i = 0; i < n; ++i) {
                I64(l + ".tab" + std::to_string(i) + ".on", shown && i == bag_pocket);
                I64(l + ".tab" + std::to_string(i) + ".off", shown && i != bag_pocket);
            }
        }
        for (int i=0;i<static_cast<int>(bag_rows.size());++i) {
            const auto p="lp.bag"+std::to_string(i)+".";const int id=bag_rows[i];
            Text(p+"name",assets.ItemName(id));Text(p+"icon",lp_assets::ItemKey(id));
            Text(p+"quantity","×"+std::to_string(bag[id]));I64(p+"selected",i==bag_selected);
        }
        const int item=bag_rows.empty()?0:bag_rows[bag_selected];
        Text("lp.bag.selected.name",assets.ItemName(item));Text("lp.bag.selected.icon",item?lp_assets::ItemKey(item):"");
        Text("lp.bag.description",item?assets.ItemDescription(item):"");
        Text("lp.bag.inbag", item && item < static_cast<int>(bag.size())
                                 ? F(K::bag_in_bag, {{"n", std::to_string(bag[item])}}) : std::string{});
        I64("lp.bag.can_use", item && BagUsable(item));
        I64("lp.bag.can_open", item && BagOpenable(item));
        Text("lp.bag.popup.title", bag_purpose == 1 ? S(K::popup_give_hp)
                                   : bag_popup == 2 ? S(BagUseKind(bag_item) == BagUse::PpUp ? K::popup_pp_up : K::popup_pp_restore)
                                                    : F(K::popup_use_on, {{"item", assets.ItemName(bag_item)}}));
        // what else the selected item shows: repel steps, Vs. Seeker / Poké Radar charge
        std::string extra;
        if (item == 76 || item == 77 || item == 79)
            extra = snap.repel_units > 0 ? F(K::bag_repel_active, {{"n", std::to_string(snap.repel_units * 5)}}) : "";
        else if (item == 443) extra = F(K::bag_battery, {{"n", std::to_string(snap.seeker_charge)}});
        else if (item == 431) extra = F(K::bag_battery, {{"n", std::to_string(snap.radar_charge)}});
        else if (item == 432) extra = F(K::bag_bp, {{"n", std::to_string(snap.battle_points)}});
        Text("lp.bag.extra", extra);
        I64("lp.bag.extra.on", !extra.empty());
    }

    // The Pokédex page (field view 4). The manifest's DEX_* constants (gen_manifest.py) in the same names:
    // the tab pages' scroll region top, content width and the y of their parts.
    static constexpr int DexPages = 5, DexRectTop = 190, DexTop = 196, DexW = 736, DexDescY = 604,
                         DexFormGap = 44, DexFormDy = 124, DexFormCols = 6, DexFormMax = 32, DexWeakCols = 3,
                         DexStatsEnd = 1300, DexEvoDy = 128, DexEvoIndent = 56, DexEvoMax = 16,
                         DexAreaDy = 136, DexAreaMax = 96, DexPad = 16,
                         DexEvoTextW = DexW - 16 - 88 - 12 - 16 - 2 * DexEvoIndent, DexAreaTextW = DexW - 40 - 56;
    lp_dex_text::Ctx DexCtx() const {
        return {strings, assets};
    }
    // The chain, form chips and places of the selected species' form (kept for the taps).
    void BuildDexView() {
        const auto gen = assets.Language().generation;
        if (dex_view.species == dex_species && dex_view.form == dex_form && dex_view.generation == gen)
            return;
        const auto c = DexCtx();
        dex_view = {};
        dex_view.species = dex_species;
        dex_view.form = dex_form;
        dex_view.generation = gen;
        const auto forms = assets.ValidForms(dex_species);
        if (forms.size() > 1)
            dex_view.forms.assign(forms.begin(), forms.begin() + std::min<std::size_t>(forms.size(), DexFormMax));
        dex_view.chain = assets.EvolutionChain(dex_species, dex_form);
        if (dex_view.chain.size() > DexEvoMax)
            dex_view.chain.resize(DexEvoMax);
        for (const auto& m : dex_view.chain)
            dex_view.chain_cond.push_back(lp_dex_text::WaysText(c, m.via));
        for (auto& r : lp_dex_text::Areas(c, dex_species)) {
            if (dex_view.areas.size() >= DexAreaMax)
                break;
            dex_view.areas.push_back({r.zone});
            dex_view.area_text.emplace_back(std::move(r.name), std::move(r.way));
        }
    }
    void PublishDex() {
        I64("lp.dex.sort.id",dex_sort);
        if constexpr (DebugKeys) I64("lp.dex.national", dex_national);
        Text("lp.dex.title", S(dex_national ? K::dex_national : K::dex_sinnoh));
        if (!snap.pokedex_acquired) {I64("lp.dex.count",0);return;}
        // The statuses change only through the game (a catch, a trade): read them when the page
        // opens, for another save, and then every DexStatusTicks (each sample until they are read).
        const u64 owner = live.PlayerWorkObject();
        if (dex_statuses.empty() || owner != dex_statuses_owner || tick != dex_seen_tick + 1 ||
            tick >= dex_statuses_tick + DexStatusTicks || tick < dex_statuses_tick) {
            dex_statuses = live.DexStatuses(lumi == 1);
            dex_statuses_owner = owner;
            dex_statuses_tick = tick;
        }
        dex_seen_tick = tick;
        const auto& statuses = dex_statuses;
        // the rows, seen and obtained follow the statuses, the sort and the catalog (names, A–Z)
        auto& dc = dex_rows_cache;
        if (!dc.valid || lang_generation != dc.generation) dc.regional = assets.SinnohNumbers();
        const auto& regional = dc.regional;
        if (!dc.valid || statuses != dc.statuses || dex_sort != dc.sort || lumi != dc.lumi ||
            lang_generation != dc.generation || dex_national != dc.national) {
            dc.valid = true; dc.statuses = statuses; dc.sort = dex_sort; dc.lumi = lumi; ++dc.serial;
            dc.generation = lang_generation; dc.seen = dc.obtained = 0; dc.national = dex_national;
            dex_rows.clear();
            for (int id=1;id<static_cast<int>(statuses.size());++id) {
                if (!dex_national && regional.find(id) == regional.end()) continue;
                if(statuses[id]>=2) {++dc.seen;if(!assets.SpeciesName(id).empty())dex_rows.push_back(id);}
                if(statuses[id]==3)++dc.obtained;
            }
            if (!dex_national) std::stable_sort(dex_rows.begin(),dex_rows.end(),[&](int a,int b){return regional.at(a)<regional.at(b);});
            if(dex_sort==1) SortByName(dex_rows);
            if(dex_sort==2) std::stable_sort(dex_rows.begin(),dex_rows.end(),[&](int a,int b){return statuses[a]>statuses[b];});
        }
        const int seen = dc.seen, obtained = dc.obtained;
        if(std::find(dex_rows.begin(),dex_rows.end(),dex_species)==dex_rows.end()) {
            dex_species=dex_rows.empty()?0:dex_rows.front();
            dex_form=0;
        }
        I64("lp.dex.seen",seen);I64("lp.dex.obtained",obtained);I64("lp.dex.count",dex_rows.size());
        I64("lp.dex.valid",dex_species>0);
        I64("lp.dex.ready",!statuses.empty());
        static constexpr K SortNames[]={K::dex_sort_number,K::dex_sort_az,K::dex_sort_caught};Text("lp.dex.sort",S(SortNames[dex_sort]));
        // The rows' names and texts are built once per row list (dex_rows changes with the statuses,
        // the sort, the mode and the catalog) and republished from there every sample.
        if (dex_pub_serial != dc.serial) {
            dex_pub_serial = dc.serial;
            dex_pub.resize(dex_rows.size());
            for (std::size_t i = 0; i < dex_rows.size(); ++i) {
                const int id = dex_rows[i];
                auto& row = dex_pub[i];
                if (row.number_key.empty()) {
                    const auto p = "lp.dex" + std::to_string(i) + ".";
                    row.number_key = p + "number";
                    row.name_key = p + "name";
                    row.obtained_key = p + "obtained";
                    row.selected_key = p + "selected";
                }
                char number[16];
                std::snprintf(number, sizeof(number), "%04d", dex_national ? id : regional.at(id));
                row.number = number;
                row.name = assets.SpeciesName(id);
                if (std::any_of(row.name.begin(), row.name.end(),
                                [](char c) { return static_cast<unsigned char>(c) >= 0x80; }))
                    assets.NoteText(row.name); // as Text() does
                row.species = id;
                row.obtained = statuses[id] == 3;
            }
        }
        if (host.publish_text && host.publish_i64)
            for (const auto& row : dex_pub) {
                host.publish_text(host.userdata, row.number_key.c_str(), row.number.c_str());
                host.publish_text(host.userdata, row.name_key.c_str(), row.name.c_str());
                host.publish_i64(host.userdata, row.obtained_key.c_str(), row.obtained);
                host.publish_i64(host.userdata, row.selected_key.c_str(), row.species == dex_species);
            }
        BuildDexView();
        const auto c = DexCtx();
        const bool has_forms = !dex_view.forms.empty();
        if (std::find(dex_view.forms.begin(), dex_view.forms.end(), dex_form) == dex_view.forms.end())
            dex_form = 0;
        const int form = dex_form;
        for (int k = 0; k < DexPages; ++k) I64("lp.dexp" + std::to_string(k), dex_page == k);
        // a new species or form starts every page at its top
        I64("lp.dex.selected.key", dex_species * 64 + form);
        {
            const auto it = std::find(dex_rows.begin(), dex_rows.end(), dex_species);
            I64("lp.dex.selected.row", it == dex_rows.end() ? -1 : static_cast<int64_t>(it - dex_rows.begin()));
        }

        // ---- Entry: name, form, category, types, state; height | weight | gender; the text; forms ----
        Text("lp.dex.selected.name",assets.SpeciesName(dex_species));
        Text("lp.dex.selected.icon",dex_species?lp_assets::IconKey(dex_species,form,0,false):"");
        const auto subtitle = dex_species ? lp_dex_text::FormSubtitle(c, dex_species, form, has_forms) : std::string{};
        Text("lp.dex.form.name", subtitle);
        I64("lp.dex.form.on", !subtitle.empty());
        Text("lp.dex.category", dex_species ? lp_dex_text::Category(c, dex_species) : std::string{});
        I64("lp.dex.cat.dy", subtitle.empty() ? 0 : 38);
        const int state=dex_species?statuses[dex_species]:0;
        Text("lp.dex.selected.state",state==3?S(K::dex_state_caught):state==2?S(K::dex_state_seen):std::string{});
        I64("lp.dex.state.caught", state == 3);
        const auto types=assets.SpeciesTypes(dex_species,form);
        Text("lp.dex.type1",types?TypeTag(types->first):"");
        Text("lp.dex.type2",types && types->first!=types->second?TypeTag(types->second):"");
        const auto info = dex_species ? assets.DexInfo(dex_species, form) : std::nullopt;
        Text("lp.dex.height", dex_species ? lp_dex_text::Height(c, dex_species, form) : std::string{});
        Text("lp.dex.weight", dex_species ? lp_dex_text::Weight(c, dex_species, form) : std::string{});
        {
            const auto g = lp_dex::GenderOf(info ? info->sex : 255);
            const bool bar = info && g.kind != lp_dex::Gender::Genderless;
            I64("lp.dex.g.bar", bar);
            I64("lp.dex.g.none", info && !bar);
            I64("lp.dex.g.male", g.male_permille);
            Text("lp.dex.g.mtext", bar && g.male_permille > 0 ? "♂ " + lp_dex_text::Percent(c, g.male_permille) : "");
            Text("lp.dex.g.ftext", bar && g.male_permille < 1000 ? "♀ " + lp_dex_text::Percent(c, 1000 - g.male_permille) : "");
        }
        const std::string dex_desc=dex_species?assets.DexDescription(dex_species):S(statuses.empty()?K::dex_no_data:K::dex_empty);
        Text("lp.dex.description",dex_desc);
        // the text as the runtime lays it out at 6 in DexW; the Forms section follows it
        const int desc_h = dex_desc.empty() ? 0 : TextHeight(dex_desc, 6, DexW);
        I64("lp.dex.fm.dy", desc_h);
        const int nforms = static_cast<int>(dex_view.forms.size());
        I64("lp.dex.forms.any", dex_page == 0 && nforms > 0);
        I64("lp.dex.fm.n", dex_page == 0 ? nforms : 0);
        for (int i = 0; i < nforms; ++i) {
            const auto p = "lp.dex.fm" + std::to_string(i) + ".";
            Text(p + "icon", lp_assets::IconKey(dex_species, dex_view.forms[i], 0, false));
            I64(p + "sel", dex_view.forms[i] == form);
        }
        {
            int end = DexDescY + desc_h;
            if (nforms > 0)
                end += DexFormGap + WkSecH + ((nforms + DexFormCols - 1) / DexFormCols) * DexFormDy - (DexFormDy - 112);
            I64("lp.dex.entry.rows", (end + DexPad - DexRectTop + 9) / 10);
        }

        // ---- Weak. (DexWeakCols tags a row) ----
        PublishChart("lp.dex.", "imm", types, DexWeakTop, DexWeakRect, DexWeakCols);

        // ---- Stats: base stats, abilities, Training, Breeding ----
        const auto base = dex_species ? assets.BaseStats(dex_species, form) : std::nullopt;
        int total = 0;
        // base stats as list rows (the Stats page scrolls, and only list rows follow a scroll); row 6 = total
        static constexpr K StatNames[7] = {K::hp, K::stat_attack, K::stat_defense, K::stat_sp_atk, K::stat_sp_def,
                                           K::stat_speed, K::stat_total};
        for (int k = 0; k < 7; ++k) {
            const int v = k < 6 ? (base ? (*base)[k] : 0) : total;
            if (k < 6) total += v;
            const auto row = "lp.dex.bs" + std::to_string(k) + ".";
            Text(row + "name", S(StatNames[k]));
            I64(row + "v", k < 6 ? v : total);
            I64(row + "max", k < 6 ? 255 : 720);
        }
        I64("lp.dex.bs.count", 7);
        // the species' abilities (slot 3 = hidden), each once, as one text for the Stats page: names in
        // the dark text colour, descriptions after them, a blank line between abilities
        std::string ab_text;
        {
            const auto ab = assets.SpeciesAbilities(dex_species, form);
            for (int k = 0; k < 3; ++k) {
                if (ab[k] <= 0 || std::find(ab.begin(), ab.begin() + k, ab[k]) != ab.begin() + k) continue;
                const auto desc = assets.AbilityDescription(ab[k]);
                if (!ab_text.empty()) ab_text += "\n\n";
                const auto name = assets.AbilityName(ab[k]);
                ab_text += "{c:#FF35567A}" + (k == 2 ? F(K::hidden_ability, {{"ability", name}}) : name) + "{/c}\n" + desc;
            }
            Text("lp.dex.ab.text", ab_text);
            I64("lp.dex.ab.any", !ab_text.empty());
        }
        // Training and Breeding follow the abilities text as the runtime lays it out (5, DexW wide)
        const int ab_h = ab_text.empty() ? 0 : TextHeight(ab_text, 5, DexW);
        I64("lp.dex.tr.dy", ab_h);
        Text("lp.dex.tr.ev", info ? lp_dex_text::EvYieldText(c, *info) : std::string{});
        Text("lp.dex.tr.catch", info ? std::to_string(info->catch_rate) : std::string{});
        Text("lp.dex.tr.friend", info ? std::to_string(info->friendship) : std::string{});
        Text("lp.dex.tr.exp", info ? std::to_string(info->base_exp) : std::string{});
        Text("lp.dex.tr.growth", info ? lp_dex_text::Growth(c, info->growth) : std::string{});
        Text("lp.dex.tr.egg", info ? lp_dex_text::EggGroups(c, *info) : std::string{});
        Text("lp.dex.tr.hatch", info ? lp_dex_text::HatchSteps(c, *info) : std::string{});
        I64("lp.dex.stats.rows", (DexStatsEnd + ab_h + DexPad - DexRectTop + 9) / 10);

        // ---- Evolution: the chain as an indented tree; the selected member highlighted ----
        const int nev = static_cast<int>(dex_view.chain.size());
        I64("lp.dex.ev.n", dex_page == 3 ? nev : 0);
        I64("lp.dex.ev.none", dex_page == 3 && nev <= 1);
        for (int i = 0; i < nev; ++i) {
            const auto& m = dex_view.chain[i];
            const auto p = "lp.dex.ev" + std::to_string(i) + ".";
            // seen, or past the Pokédex's range (LP-added species it does not record: never a silhouette)
            const bool known = m.species >= static_cast<int>(statuses.size()) || statuses[m.species] >= 2;
            const auto icon = lp_assets::IconKey(m.species, m.form, 0, false);
            Text(p + "icon", known ? icon : "module:lp:mask/" + icon.substr(std::string_view{"module:lp:"}.size()));
            I64(p + "seen", known);
            const bool forms = assets.ValidForms(m.species).size() > 1;
            Text(p + "name", known ? lp_dex_text::DisplayName(c, m.species, m.form, forms) : S(K::dex_unknown));
            Text(p + "cond", dex_view.chain_cond[i]);
            // at 5 when it fits the row's two lines (DexEvoTextW), else at 4
            I64(p + "big", TextHeight(dex_view.chain_cond[i], 5, DexEvoTextW) <= 5 * 5 + 5 * 8);
            I64(p + "dx", std::min(m.depth, 2) * DexEvoIndent);
            I64(p + "child", m.depth > 0);
            I64(p + "sel", m.species == dex_species && m.form == form);
        }
        I64("lp.dex.evo.rows", ((nev <= 1 ? DexTop + 8 + DexEvoDy + 16 + 40 : DexTop + 8 + nev * DexEvoDy - 8) +
                                DexPad - DexRectTop + 9) / 10);

        // ---- Area: where it lives in the wild ----
        const int nar = static_cast<int>(dex_view.areas.size());
        I64("lp.dex.ar.n", dex_page == 4 ? nar : 0);
        I64("lp.dex.ar.none", dex_page == 4 && nar == 0 && assets.EncounterDataReady());
        for (int i = 0; i < nar; ++i) {
            const auto p = "lp.dex.ar" + std::to_string(i) + ".";
            Text(p + "name", dex_view.area_text[i].first);
            Text(p + "way", dex_view.area_text[i].second);
            I64(p + "big", TextHeight(dex_view.area_text[i].second, 5, DexAreaTextW) <= 5 * 5 + 5 * 8);
            // the Map shows the place when it has a Town Map cell (the row's map icon)
            I64(p + "map", snap.map_acquired && assets.ZoneCell(dex_view.areas[i].zone).has_value());
        }
        I64("lp.dex.area.rows", nar == 0 ? 0 : (DexTop + 8 + nar * DexAreaDy - 8 + DexPad - DexRectTop + 9) / 10);
    }

    // Battle Pokémon carry no nickname; take it from the party copy of the same species.
    std::string OwnNick(u16 species) const {
        for (const auto& pm : snap.party)
            if (pm.valid && !pm.mon.is_egg && pm.mon.species == species)
                return lp_pk8::Utf8(pm.mon.nickname);
        return assets.SpeciesName(species);
    }

    void Press(Btn b) {
        if (snap.demo_active) return;
        pending_press = b;
    }

    void Next(int step) {
        drive.step = step;
        drive.step_tick = tick;
        drive.pressed = 0;
    }

    // Sends `b` once, the first sample at or after `at` ticks into the step (samples can skip
    // ticks, so an exact tick match could miss). `slot` tells several presses of one step apart.
    // Returns true on the sample it pressed.
    bool PressOnce(Btn b, u64 at, unsigned slot = 0, bool when = true) {
        if (!when || Age() < at || (drive.pressed & (1u << slot)))
            return false;
        drive.pressed |= 1u << slot;
        Press(b);
        return true;
    }

    void Finish(int result) {
        drive.kind = Drive::Kind::None;
        drive.result = result;
        drive.step = 0;
    }

    // Ticks since the current step began.
    u64 Age() const {
        return tick - drive.step_tick;
    }

    // Passive tabs never drive the native X menu. Leaving Bag cancels a pending
    // Open in Bag request so its delayed X/A cannot fire over another bottom page.
    void OpenFieldView(int v) {
        if (v != 5) open_state = 0;
        if (v != field_view) { bag_popup = 0; party_move = -1; }
        field_view = v;
        field_refresh_tick = tick + 8;
    }

    void StepDrive() {
        if (drive.kind == Drive::Kind::None)
            return;
        if (!snap.in_battle) {
            Finish(-1);
            return;
        }
        constexpr u64 Settle = 4;    // ticks after a write before pressing (lets the game see it)
        constexpr u64 Timeout = 150; // 2.5 s per step
        const auto& a = snap.action;
        const auto& w = snap.waza;
        switch (drive.kind) {
        case Drive::Kind::Target:
            if ((drive.pressed & 1u) && !snap.dbl.target_open) { Finish(1); break; }
            if (Age() > Timeout || !snap.dbl.target_open) { Finish(-1); break; }
            if (drive.pressed & 1u) break;
            if (!live.CanTargetInput(drive.arg)) { Finish(-1); break; }
            if (snap.dbl.target_view != drive.arg) {
                if (live.SetTargetIndex(drive.arg)) drive.cursor_tick = tick;
            } else if (tick - drive.cursor_tick >= Settle) {
                PressOnce(Btn::A, 0);
            }
            break;
        case Drive::Kind::BackMoves:
            if (a.focus) {
                Finish(1);
            } else if (w.focus) {
                PressOnce(Btn::B, Settle);
                if (Age() > Timeout) Finish(-1);
            } else if (Age() > Timeout) {
                Finish(-1);
            }
            break;
        case Drive::Kind::OpenMoves:
        case Drive::Kind::Move:
            switch (drive.step) {
            case 0: // open the move list (silently: it is parked off-screen)
                if (w.focus) {
                    Next(2);
                } else if (a.focus) {
                    // The native cursor is usually already on Battle. Avoid an unnecessary
                    // guest write and its settle delay in that common path.
                    const bool already_selected = a.current == 0 && !a.transition;
                    if (already_selected || live.SetActionIndex(0)) {
                        Next(1);
                        if (already_selected) PressOnce(Btn::A, 0);
                    }
                } else if (Age() > Timeout) {
                    Finish(-1);
                }
                break;
            case 1:
                PressOnce(Btn::A, Settle, 0, a.focus && !a.transition && a.current == 0);
                if (w.focus)
                    Next(2);
                else if (Age() > Timeout)
                    Finish(-1);
                break;
            case 2:
                if (drive.kind == Drive::Kind::OpenMoves) {
                    // Keep the opening appearance until the native slide finishes; focus
                    // alone can arrive early and would briefly restore the dark busy veil.
                    if (w.focus && w.show && !w.transition) Finish(1);
                    else if (Age() > Timeout) Finish(-1);
                    break;
                }
                if (Age() > Timeout) {
                    Finish(-1);
                } else if (w.focus && !w.transition && live.SetWazaIndex(drive.arg)) {
                    Next(3);
                    drive.cursor_tick = tick;
                }
                break;
            case 3:
                // Setup may overwrite an early cursor store. Reapply before (never after) A;
                // settle again after each write, while the original step timeout stays fixed.
                switch (lp_battle_drive::MenuCursor(Age(), tick - drive.cursor_tick,
                            w.focus, w.transition, w.current, drive.arg, (drive.pressed & 1u) != 0)) {
                case lp_battle_drive::CursorStep::Write:
                    if (live.SetWazaIndex(drive.arg)) drive.cursor_tick = tick;
                    break;
                case lp_battle_drive::CursorStep::Press:
                    PressOnce(Btn::A, Settle);
                    break;
                case lp_battle_drive::CursorStep::Done: Finish(1); break;
                case lp_battle_drive::CursorStep::Timeout: Finish(-1); break;
                case lp_battle_drive::CursorStep::Wait: break;
                }
                break;
            }
            break;
        case Drive::Kind::Command:
            // The action list can reset its cursor during setup too. A lost focus alone
            // is not success: only finish after our own A was sent on the settled item.
            switch (lp_battle_drive::MenuCursor(Age(), tick - drive.cursor_tick,
                        a.focus, a.transition, a.current, drive.arg, (drive.pressed & 1u) != 0)) {
            case lp_battle_drive::CursorStep::Write:
                if (live.SetActionIndex(drive.arg)) drive.cursor_tick = tick;
                break;
            case lp_battle_drive::CursorStep::Press:
                PressOnce(Btn::A, Settle);
                break;
            case lp_battle_drive::CursorStep::Done: Finish(1); break;
            case lp_battle_drive::CursorStep::Timeout: Finish(-1); break;
            case lp_battle_drive::CursorStep::Wait: break;
            }
            break;
        case Drive::Kind::Ball:
            // Submit the native entry only while it is still the selected item. The list can
            // reorder when it opens, so match the item number before sending A.
            if (!snap.ball.focus || snap.ball.current < 0 ||
                snap.ball.current >= static_cast<int>(snap.balls.size())) {
                Finish((drive.pressed & 1u) ? 1 : -1);
            } else if (snap.balls[snap.ball.current] != drive.arg) {
                Finish(-1);
            } else {
                PressOnce(Btn::A, Settle);
                if (Age() > Timeout)
                    Finish(-1);
            }
            break;
        case Drive::Kind::None:
            break;
        }
    }

    // ---- sprite motion (lp_anim) ----------------------------------------------------------------
    // The poses of the few sprites in view, every sample, from a monotonic clock: the battle header's
    // two sprites (a single) or the four card sprites (a double), the Party card's sprite (its plate
    // icon follows the hop) and the Pokédex Entry sprite. Out of view everything is at rest, so a
    // hidden page never changes a value. The manifest gates each channel with the Animations setting
    // (lp.an.* = @flag:lp_anim ? lp.am.* : rest).
    double AnimNow() const {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - anim_epoch).count();
    }
    void PublishPose(const std::string& p, const lp_anim::Pose& pose, bool turn, bool scale) {
        I64(p + "x", pose.x);
        I64(p + "y", pose.y);
        if (turn) {
            I64(p + "r", pose.rot);
            I64(p + "a", pose.fade);
        }
        if (scale)
            I64(p + "s", pose.scale);
    }
    void PublishAnim() {
        const double now = AnimNow();
        const bool battle = snap.in_battle;
        const bool dbl = battle && DoubleOn();
        const auto observe = [now](lp_anim::Battler& b, const lp_live::BattleMon& m, bool present) {
            b.Observe(now, (static_cast<std::int64_t>(m.id) << 16) | m.species, present && m.valid, ShownHp(m),
                      m.hp_max, m.status);
        };
        for (int c = 0; c < 2; ++c) {
            auto& b = anim_single[static_cast<std::size_t>(c)];
            observe(b, snap.front[static_cast<std::size_t>(c)], battle && !dbl);
            PublishPose(c == 0 ? "lp.am.bo." : "lp.am.bf.", b.At(now, lp_anim::FaintPx), true, false);
        }
        for (int k = 0; k < 4; ++k) {
            auto& b = anim_double[static_cast<std::size_t>(k)];
            const auto& sl = snap.dbl.view[static_cast<std::size_t>(k)];
            observe(b, sl.mon, dbl && sl.exists);
            PublishPose("lp.am.d" + std::to_string(k) + ".", b.At(now, lp_anim::FaintPxCard), true, false);
        }
        // the Party page (lp.page 1) and the Pokédex Entry (lp.page 6, page 0)
        const bool field = !battle && snap.field && snap.player_ok;
        const bool party_on = field && field_view == 0 && snap.party_count > 0;
        I64("lp.party.reordering", party_on && now - party_swap_at < lp_anim::PartySwapMs);
        for (int slot = 0; slot < 6; ++slot) {
            const auto pose = party_on ? lp_anim::PartySwap(now - party_swap_at, slot, party_swap_from, party_swap_to)
                                       : lp_anim::Pose{};
            PublishPose("lp.am.swap" + std::to_string(slot) + ".", pose, false, false);
        }
        anim_party.Observe(now, sel_party, party_on);
        PublishPose("lp.am.sel.", anim_party.At(now), false, true);
        const bool dex_on = field && field_view == 4 && dex_page == 0 && snap.pokedex_acquired && dex_species > 0;
        anim_dex.Observe(now, dex_species * 1000 + dex_form, dex_on);
        PublishPose("lp.am.dex.", anim_dex.At(now), false, true);
    }

    void PublishDrive() {
        if (snap.demo_active) pending_press = Btn::None;
        // One press per request: the gate is open for exactly this sample, the manifest's enforce
        // rule turns it into a 5-frame press.
        I64("pdrv.a", pending_press == Btn::A ? 1 : 0);
        I64("pdrv.b", pending_press == Btn::B ? 1 : 0);
        I64("pdrv.x", pending_press == Btn::X ? 1 : 0);
        I64("pdrv.plus", pending_press == Btn::Plus ? 1 : 0);
        I64("pdrv.dup", pending_press == Btn::DUp ? 1 : 0);
        I64("pdrv.ddown", pending_press == Btn::DDown ? 1 : 0);
        I64("pdrv.dleft", pending_press == Btn::DLeft ? 1 : 0);
        I64("pdrv.dright", pending_press == Btn::DRight ? 1 : 0);
        I64("pdrv.pending", pending_press != Btn::None ? 1 : 0);
        pending_press = Btn::None;
        // not bound: for console checks
        if constexpr (DebugKeys) {
            I64("lp.drive.busy", drive.kind != Drive::Kind::None ? 1 : 0);
            I64("lp.drive.result", drive.result);
        }
    }

    EdenDsmodHostApi host;
    lp_live::Reader live;
    lp_assets::Assets assets;
    int lang_id = lp_lang::English;   // the save's language, the last one seen
    bool lang_kanji = false;
    std::string lang_sfx = "en";      // UI suffix of the catalog in effect
    std::uint64_t lang_generation = 0;
    lp_strings::Table strings;              // every lp_strings row in the catalog's language
    std::vector<std::string> string_names;  // "lp.t.<key>" by row
    bool strings_built = false;
    std::atomic<int> text_font_rev{0};      // counts DecodeFont calls (the published widths follow)
    int widths_rev = -1;
    std::vector<std::pair<std::string, int64_t>> width_values;
    std::vector<int> name_pos;              // the Pokédex A–Z place by species (SortByName)
    bool supported = false;
    int last_party_state = -1;
    int lumi = -1;
    u64 tick = 0;
    lp_live::Snapshot snap;
    Drive drive;
    Btn pending_press = Btn::None;
    std::vector<int> bag;
    u64 bag_tick = 0, bag_revision = 0;
    u64 bag_rows_revision = std::numeric_limits<u64>::max(), bag_rows_generation = 0;
    int bag_rows_pocket = -1;
    EdenDsmodHostWriteApi write_api{};
    std::vector<int> bag_rows,dex_rows;
    struct DexRowsCache {      // the inputs dex_rows was built from (PublishDex)
        bool valid = false;
        std::vector<int> statuses;
        int sort = 0, lumi = -1;
        std::uint64_t generation = 0;
        int seen = 0, obtained = 0;
        bool national = false;
        std::map<int, int> regional;
        std::uint64_t serial = 0;  // +1 whenever dex_rows is rebuilt
    } dex_rows_cache;
    struct DexRowPub {             // one published Pokédex row (PublishDex), its names built once
        std::string number_key, name_key, obtained_key, selected_key, number, name;
        int species = 0;
        bool obtained = false;
    };
    std::vector<DexRowPub> dex_pub;
    std::uint64_t dex_pub_serial = 0;
    std::vector<int> dex_statuses;  // the last DexStatuses read (PublishDex)
    u64 dex_statuses_owner = 0, dex_statuses_tick = 0, dex_seen_tick = 0;
    static constexpr u64 DexStatusTicks = 30;
    bool dex_national = false;
    int bag_pocket=0,bag_selected=0,dex_species=0,dex_sort=0;
    int field_view = 0;         // 0 party, 1 Town Map, 2 Route Pokémon, 3 Pokétch, 4 Pokédex, 5 Bag
    u64 field_refresh_tick = 0;

    std::string last_map_key;
    int field_tab = 0;          // 0 stats, 1 moves, 2 defensive type weaknesses
    int party_swap_from = -1, party_swap_to = -1;
    double party_swap_at = lp_anim::Never;
    lp_pk8::Mon move_drag_owner;
    int move_drag_slot = -1;
    int64_t move_drag_token = 0;
    lp_pk8::Mon party_move_owner;
    int party_move_id = 0;
    int party_move = -1;        // inspection only; never submits/uses a move
    int pick = -1;               // highlighted native move or most recently submitted tap (-1 none)
    bool last_cmd_focus = false;
    int battle_view = 0;
    // the move page saw the game's move list focused (a commit can return it to the command page),
    // and the samples since the list and the target select lost their focus
    bool waza_seen = false;
    int move_done_ticks = 0;
    static constexpr int MoveDoneTicks = 6;
    // battle pane: each page's tab (0 Foe, 1 You, 2 Field), reset to the foe at every battle start
    int tab_cmd = 0, tab_fight = 0;
    int64_t battle_seq = 0;
    bool pane_in_battle = false;
    int foe_dex_species = -1, foe_caught = -1; // Pokédex obtained state of the foe's species (-1 unknown)
    u64 foe_dex_tick = 0;
    std::string last_pane_key;
    // double battles: the focused view position (0 near 1, 1 far 1, 2 near 2, 3 far 2): the game's
    // choice (the Pokémon choosing its command, the target cursor) unless a card was tapped since
    int dbl_focus = -1, dbl_game = -1, dbl_manual = -1, dbl_last_foe = -1;
    int64_t dbl_turn = -2;
    int moves_key = -1, moves_count = 0; // the Pokémon whose moves the pane lists (pick belongs to it)
    u64 scene_revision = 0;
    int last_focus_mask = 0;
    int last_battle_window = 0; // the game's Bag / Pokémon window last sample (lp_live Snapshot)
    int last_waza_index = -1;
    int last_action_index = -1;
    int scene_refresh_ticks = 0;
    int sel_party = 0;
    // sprite motion: the battle header's own / foe, a double's four cards, the Party card, the Pokédex entry
    std::chrono::steady_clock::time_point anim_epoch = std::chrono::steady_clock::now();
    std::array<lp_anim::Battler, 2> anim_single{};
    std::array<lp_anim::Battler, 4> anim_double{};
    lp_anim::Focus anim_party, anim_dex;
    u64 last_sample_tick = 0;
    bool menus_parked = false;
    u64 menus_retry_tick = 0; // the next tick Tick may try to restore the parked menus
    static constexpr u64 RestoreRetryTicks = 30;
    // independent Pokétch
    int pkt_app = -1, pkt_colour = 0, pkt_counter = 0, pkt_coin = 0;
    int pkt_hop_slot = -1;
    std::chrono::steady_clock::time_point pkt_coin_at{}, pkt_hop_at{};
    bool pkt_full = false; // the display alone, full screen
    int dex_page = 0;                 // Pokédex left card: 0 entry, 1 weaknesses, 2 stats, 3 evolution, 4 area
    int dex_form = 0;                 // the form the Pokédex card shows (a form chip / an evolution member)
    // What the Pokédex card shows, kept for its taps and rebuilt when the species, form or text changes
    struct DexArea { int zone = 0; };
    struct DexView {
        int species = -1, form = -1;
        std::uint64_t generation = 0;
        std::vector<int> forms;                 // the form chips (valid forms; empty for one form)
        std::vector<lp_dex::Member> chain;      // the evolution chain
        std::vector<std::string> chain_cond;    // each member's ways in, in the game's language
        std::vector<DexArea> areas;             // the Area rows' zones
        std::vector<std::pair<std::string, std::string>> area_text; // name, way
    } dex_view;
    int route_zone = -1;              // Route Pokémon for a tapped Town Map place (-1 = where you are)
    int64_t last_map_tap_seq = 0;
    mutable std::mutex text_font_mutex;      // DecodeFont runs on the runtime's font thread
    std::optional<lp_assets::Font> text_font; // the game font as decoded for the runtime
    mutable std::unordered_map<std::string, int> text_measures; // FontMeasure memo (text_font_mutex)
    std::optional<std::pair<int, int>> route_cell;   // Town Map cell of a tapped place (Fly here)
    std::optional<std::pair<int, int>> map_sel;      // the place selected on the Map tab
    int map_sel_zone = -1;
    std::string map_sel_name;
    EdenDsmodHostExtensions host_ext{};
    lp_guest::Mailbox guest;
    u64 bag_popup_owner = 0;
    lp_pk8::Mon bag_target_owner, field_waza_owner;
    int bag_popup = 0, bag_item = 0, bag_target = -1; // Bag Use pop-up: 1 pick a member, 2 pick a move
    int bag_purpose = 0;                              // 0 a Bag item, 1 Soft-Boiled / Milk Drink
    int field_waza_user = -1;
    u64 key_owner = 0;
    int key_state = 0, key_item = 0, key_restore = 0, key_slot = 0; // shortcut use: 1 set, 2 wheel, 3 used
    std::chrono::steady_clock::time_point key_at{};
    int open_state = 0; // Open in Bag: 1 cursor written, 2 X pressed
    std::chrono::steady_clock::time_point open_at{};
    std::string bag_toast;
    std::chrono::steady_clock::time_point bag_toast_at{};
    bool pkt_init = false;
    lp_poketch::Calc pkt_calc;
    lp_poketch::Frames pkt_frames;
    std::chrono::steady_clock::time_point pkt_sound_at{}, pkt_alarm_at{};
    u64 pkt_sound_issued = 0, pkt_sound_done = 0;
    u32 pkt_sound_event = 0;
    bool pkt_sound_ok = false;
    std::vector<u8> memo, wheel;
    std::vector<std::pair<int, int>> dowse_markers;
    lp_map::PlayerLocation map_player;
    std::optional<std::pair<int, int>> map_cell;   // town-map cell (Map tab), also the Marking Map cursor
    std::array<std::pair<int, int>, 6> marks{};    // Marking Map markers (companion copy, from the save)
    bool marks_init = false;
    int mark_sel = -1;
    std::vector<u8> dots;                          // Dot Artist (companion copy, from the save)
    bool dot_init = false, dot_modified = false;
    int dot_rev = 0;
    int dowse_x = 0, dowse_y = 0, dowse_gx = 0, dowse_gy = 0;
    bool dowse_active = false, dowse_found = false;
    std::chrono::steady_clock::time_point dowse_at{};
    int memo_tool = 0, memo_rev = 0, roul_rev = 0, roul_state = 0; // roul_state: 0 idle, 1 spin, 2 stopping
    double roul_angle = 0, roul_speed = 0;
    long long timer_left_ms = 0, timer_set_ms = 0;
    bool timer_running = false, timer_alarm = false;
    u64 timer_alarm_tick = 0;
    std::chrono::steady_clock::time_point timer_last{};
    std::vector<int> pkt_rows;
};

EdenDsmodBool SupportsBuild(const char* build_id) {
    return build_id && std::string_view{build_id} == BuildId ? EDEN_DSMOD_TRUE : EDEN_DSMOD_FALSE;
}
void* Create(const EdenDsmodHostApi* host, const char*) {
    try {
        if (!dsmod_sdk::HostAbiMatches(host) || host->title_id != TitleId || !host->read_memory)
            return nullptr;
        return new Module{*host};
    } catch (...) {
        return nullptr;
    }
}
void Destroy(void* p) {
    try {
        delete static_cast<Module*>(p);
    } catch (...) {
    }
}
void SampleCallback(void* p, const EdenDsmodHostApi* host) {
    static std::atomic_flag reported = ATOMIC_FLAG_INIT;
    try {
        if (p && host) {
            auto& m = *static_cast<Module*>(p);
            m.UpdateHost(*host);
            m.Sample();
        }
    } catch (...) {
        if (!reported.test_and_set() && host && host->log)
            host->log(host->userdata, EDEN_DSMOD_LOG_ERROR, "LP DSMod sample callback failed");
    }
}
void TickCallback(void* p, const EdenDsmodHostApi* host) {
    try {
        if (p && host) {
            auto& m = *static_cast<Module*>(p);
            m.UpdateHostOnly(*host);
            m.Tick();
        }
    } catch (...) {
    }
}
void ConfigureCallback(void* p, const EdenDsmodHostExtensions* ext) {
    try {
        if (p)
            static_cast<Module*>(p)->Configure(ext);
    } catch (...) {
    }
}
EdenDsmodBool ActionCallback(void* p, const char* action, std::int64_t arg) {
    try {
        if (p && action)
            return static_cast<Module*>(p)->OnAction(action, arg);
    } catch (...) {
    }
    return EDEN_DSMOD_FALSE;
}
EdenDsmodBool LoadImageCallback(void* p, const EdenDsmodHostApi* host, const char* key, void* receiver,
                                EdenDsmodImageSink sink) {
    try {
        if (p)
            return static_cast<Module*>(p)->LoadImage(host, key, receiver, sink);
    } catch (...) {
    }
    return EDEN_DSMOD_FALSE;
}
EdenDsmodBool DecodeFontCallback(void* p, const std::uint8_t* bytes, size_t size, void* receiver,
                                 EdenDsmodFontSink sink) {
    try {
        if (p)
            return static_cast<Module*>(p)->DecodeFont(bytes, size, receiver, sink);
    } catch (...) {
    }
    return EDEN_DSMOD_FALSE;
}

const EdenDsmodModuleApi ModuleApi{EDEN_DSMOD_MODULE_ABI_VERSION,
                                   sizeof(EdenDsmodModuleApi),
                                   0,
                                   EDEN_DSMOD_MODULE_ABI_HASH,
                                   TitleId,
                                   Pearl ? "Pokemon Shining Pearl / Luminescent Platinum DSMod"
                                         : "Pokemon Brilliant Diamond / Luminescent Platinum DSMod",
                                   EDEN_DSMOD_CAP_EXTENSIONS | EDEN_DSMOD_CAP_ROMFS_READ |
                                       EDEN_DSMOD_CAP_WRITE_MEMORY,
                                   &SupportsBuild,
                                   &Create,
                                   &Destroy,
                                   &SampleCallback,
                                   &TickCallback};
const EdenDsmodModuleExtensions ModuleExtensions{EDEN_DSMOD_EXT_VERSION, sizeof(EdenDsmodModuleExtensions),
                                                 EDEN_DSMOD_EXT_HASH,    &ConfigureCallback,
                                                 &ActionCallback,        &LoadImageCallback};
void ConfigureWriteCallback(void* p, const EdenDsmodHostWriteApi* api) {
    try {
        if (p)
            static_cast<Module*>(p)->ConfigureWrite(api);
    } catch (...) {
    }
}
const EdenDsmodModuleWriteExtensions WriteExtensions{EDEN_DSMOD_WRITE_EXT_VERSION,
                                                     sizeof(EdenDsmodModuleWriteExtensions),
                                                     EDEN_DSMOD_WRITE_EXT_HASH, &ConfigureWriteCallback};
const EdenDsmodFontExtensions FontExtensions{EDEN_DSMOD_FONT_EXT_VERSION, sizeof(EdenDsmodFontExtensions),
                                             EDEN_DSMOD_FONT_EXT_HASH, &DecodeFontCallback};
} // namespace

DSMOD_SDK_EXPORT_MODULE(ModuleApi)
DSMOD_SDK_EXPORT_EXTENSIONS(ModuleExtensions)
DSMOD_SDK_EXPORT_FONT_EXTENSIONS(FontExtensions)
DSMOD_SDK_EXPORT_WRITE_EXTENSIONS(WriteExtensions)
