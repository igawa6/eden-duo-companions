// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Pokémon Sword module: ABI glue, the direct battle commands and the field actions.
//   sw_variant.h        the game-variant record: title, supported build + its main offsets
//                       (generated from tools/pokemon-sword/sw_variants.py)
//   sw_build.h          supported/known build ids, the module's own (stricter) build gate
//   sw_addresses.h      one table of live-RAM anchors + the heap_begin+offset formula every
//                       domain reader uses (each anchor carries its own verification note)
//   sw_pk8.h            PK8 (party/box Pokémon) decrypt, ported from a save-decoding script
//   sw_text.h           GameFreak .tbl/.dat message-table decoder (decodes real romfs bytes)
//   sw_type_chart.h     the 18x18 type-effectiveness chart for the weakness boards
//   sw_reader.h         Reader::Sample: one publish set per domain, change-driven
//   sw_helper_mailbox.h the guest helper's mailbox (battle channel, HUD hide, field channel)
// Supported build: Sword 1.3.2, main A3B75BCD3311385AEED67FBEEB79CBB7BF02F471 (sw_build.h).
// Isle of Armor / Crown Tundra AOC (...9001/...9002) are optional: nothing here requires them,
// and the package's own "aoc:" source use (if any) degrades quietly when they are absent
// (PACKAGE_FORMAT.md section 3.11, EDEN_DSMOD_CAP_SOURCE_AOC).

#include "core/mods/dsmod_module_abi.h"
#include "core/mods/dsmod_module_extensions.h"
#include "core/mods/modules/dsmod_module_sdk.h"
#include "sw_build.h"
#include "sw_fly.h"
#include "sw_helper_mailbox.h"
#include "sw_dmax_native.h"
#include "sw_modfont.h"
#include "sw_ui_images.h"
#include "sw_reader.h"
#include "sw_trainer_portrait.h"
#include "sw_portrait_control.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace {
using namespace dsmod_sdk::int_types;

// The CMake target's TITLE (the .so name and the package's title) and the variant record agree.
#ifdef SW_MODULE_TITLE_ID
static_assert(sw_variant::kActive.title_id == SW_MODULE_TITLE_ID, "CMake TITLE != the variant's title id");
#endif

// Host mailbox primitives (configure()'s extension table), bound into sw_helper::Bridge.
struct HelperIo {
    void* userdata{};
    EdenDsmodBool (*load_u32)(void*, std::uint32_t, std::uint32_t*){};
    EdenDsmodBool (*store_u32)(void*, std::uint32_t, std::uint32_t){};
};
struct HelperLoad32 {
    HelperIo io;
    bool operator()(std::uint32_t o, std::uint32_t* v) const { return io.load_u32(io.userdata, o, v) == EDEN_DSMOD_TRUE; }
};
struct HelperStore32 {
    HelperIo io;
    bool operator()(std::uint32_t o, std::uint32_t v) const { return io.store_u32(io.userdata, o, v) == EDEN_DSMOD_TRUE; }
};
using HelperBridge = sw_helper::Bridge<HelperLoad32, HelperStore32>;
using HelperHud = sw_helper::HudHide<HelperLoad32, HelperStore32>;
using HelperField = sw_helper::FieldBridge<HelperLoad32, HelperStore32>;
using HelperCursor = sw_helper::CursorChannel<HelperLoad32, HelperStore32>;
using HelperMenu = sw_helper::MenuHide<HelperLoad32, HelperStore32>;
using PortraitControl = sw_portrait_control::Control<HelperLoad32, HelperStore32>;

// The page the companion shows (host get_text("__page"), Eden Duo 1.2.0 on); empty when the host
// cannot tell, in which case callers keep their page-independent behaviour.
std::string_view CurrentPage(const EdenDsmodHostApi& host) {
    const char* page = host.get_text ? host.get_text(host.userdata, "__page") : nullptr;
    return page ? std::string_view{page} : std::string_view{};
}

std::int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Doubles / institution battles (battle-kinds bugs 2-5): offline, not a raid, not the verified
// offline single. The bottom drives the NATIVE menus there with the game's own inputs: a tap moves
// the native cursor (cursor channel: command menu, move list, target select), a tap on the
// highlighted choice is the A button, Back is B -- never a helper-made decision.
bool NativeDrive(const sw_helper::Status& st) {
    return st.helper_present && st.kind_current && !st.offline_single && !st.offline_raid &&
           ((st.battle_kind >> 16) & 0xFFu) == 0 && (st.battle_kind & 0xFFu) != 2;
}

// Direct battle commands (FIGHT, RUN, SWITCH, ITEM), the battle HP-plate hide and the field
// actions, all through the guest helper. Sample, tick and on_action all run serialized on the
// emulator's timing thread (dsmod_module_extensions.h); `mutex` guards the state anyway because
// Destroy/Configure may come from the loader thread.
struct Direct {
    std::mutex mutex;
    std::optional<HelperBridge> bridge;
    // Bottom -> top cursor sync: moves the native move-list cursor (never commits).
    std::optional<HelperCursor> cursor;
    sw_helper::BattleView view{};
    std::string status;          // last outcome text for the battle page ("" = nothing to say)
    std::int64_t status_until = 0;
    std::uint32_t committed = 0; // successful commits this session (diagnostics/verification)
    bool dmax_wanted = false;
    int selected_command = 0; // companion command: Fight/Pokemon/Bag/Run
    std::uint32_t command_serial = 0;
    sw_helper::Status::Preview last_preview{};
    std::uint32_t preview_serial = 0;
    // Battle HP plates remain visible by default; the setting can explicitly hide them while
    // the companion shows the validated full roster. `hp_flag` is the setting
    // (1 on, 0 off, -1 unknown to this host), read by TickCallback, which runs shown or hidden and
    // combines it with __aux_present (sw_helper::HudWant).
    std::optional<HelperHud> hud;
    std::int64_t hp_flag = 1;
    // "Battle menu on top" (@flag:battle.menu_top, default off = the native battle menus hidden
    // while the bottom battle/raid page is the choice surface; sw_helper::MenuWant).
    std::optional<HelperMenu> menu;
    sw_helper::MenuWantFilter menu_want; // the request, held through failed guest reads
    std::int64_t menu_flag = 1;
    // Field actions (2026-10-26): Bag medicine / Repel and Fly through the guest's field hook.
    // fly_mask/fly_state come from the game's own checks (FLY_QUERY, re-asked every few seconds
    // while the field runs): only destinations the game would let the player pick are listed.
    std::optional<HelperField> field;
    std::optional<PortraitControl> portrait;
    std::uint64_t mailbox_address{};
    std::uint64_t fly_mask = 0;
    bool fly_known = false;
    bool bike_known = false, bike_usable = false;
    std::uint32_t fly_state = 1;
    std::int64_t fly_query_at = -10000;
    std::array<int, sw_fly::kDestinations.size()> fly_rows{};
    int fly_row_count = 0;
    int map_fly_selection = -1;
    bool map_fly_open = false;
    std::string field_status;     // toast text (game's own refusal text), "" = nothing
    std::int64_t field_status_until = 0;
    int field_pp_target = -1;
    int field_pp_item = 0;
    bool field_target_popup = false;
    int field_target_item = 0;
    std::uint32_t field_pp_target_ec = 0;
    std::uint32_t field_committed = 0;
    // Owner 2026-11-01 ("when item in bag used, hold first ... show that hp increase then close
    // the pop up"): a committed medicine / PP / candy use keeps the target popup open on the
    // chosen member -- first while the guest applies it, then kUseShowMs showing the change
    // (HP, max HP, level and PP count up from the values before the use, the status badge clears)
    // -- and only then closes. A refused use closes at once with the game's text (the toast).
    struct UseShow {
        int slot = -1;
        int item = 0;
        int move = -1;            // the PP item's move (-1: every move / not a PP item)
        bool pp_view = false;     // a PP item: the result shows the member's moves
        std::int64_t staged_at = 0;
        std::int64_t start = 0;   // 0 = the guest has not answered yet
        int hp0 = 0, max0 = 0, level0 = 0;
        bool status0 = false;
        std::string status_src0;
        std::array<int, 4> pp0{};
    };
    std::optional<UseShow> use_show;
    // Every member's values as of the first tick the popup was open (the action callback has no
    // host to read them at tap time): the "before" of a use staged now.
    std::array<UseShow, 6> use_before{};
    bool use_before_taken = false; // use_before holds this picker opening's snapshot
    // A tap that lands while the periodic FLY_QUERY is in flight (one guest frame) is staged
    // right after it, instead of being refused.
    struct Deferred {
        sw_helper::FieldOp op;
        std::uint32_t arg, expect;
        std::uint64_t zone, landing;
        std::uint32_t ec;
        std::uint32_t move = 0;
        std::int64_t until = 0;  // dropped if the field is not free again by then (0 = at once)
    };
    std::optional<Deferred> field_deferred;
    // Host write extension (kept configured; the party reorder now runs on the guest's field
    // channel through the party's own exchange -- see SampleSwap).
    EdenDsmodHostWriteApi write_api{};
    std::uint32_t swap_committed = 0;
    int swap_from = -1, swap_to = -1;      // the last staged swap (rows that slide in once it lands)
    std::int64_t swap_flash_until = 0;    // sw.party.swap.flashN window after a committed swap
    bool swap_ready = false;
};

struct Module {
    sw_reader::Reader reader;
    sw_trainer_portrait::Sampler portrait;
    Direct direct;
    const char* build_label = nullptr; // "" = supported build; non-null non-empty = known-old
    // The host sets config["_build_match"] (mod_module_host.cpp:488) to whether it found a
    // per-build <BUILD16>.json data file for the title's *running* executable -- this package
    // ships exactly one, A3B75BCD3311385A.json, matching only the one build sw_build.h supports
    // (see that file's own "_note" and contracts/LIVE-PACKAGE-TEST.md section 2). The host only
    // ever *publishes* its own "build_match" snapshot int on an error path (mod_module_host.cpp
    // RunGameModule, both =0); on every other tick the module itself owns that publish, the same
    // way MetroidDread's module does (010093801237C000.cpp's ParseMap/SampleCallback). Without
    // this, "build_match" is never published at all on a normal tick, so the manifest's
    // hide_bind:"build_match"/hide_eq:1 overlay (the map page's "Update Required" screen) never
    // sees it equal 1 and stays up forever, even on the fully supported build -- the bug this
    // field and SampleCallback's publish below fix.
    bool build_match = false;
    bool sample_failure_reported = false; // log a sample() exception once per module instance
    // Japanese in the game's own font (sw_modfont.h): built when the companion language is
    // Japanese, served through decode_font / load_image ("module:sw:font/<p>").
    sw_modfont::Font font;
    sw_ui_images::Images images;
    std::int64_t font_logged_epoch = 0;
    // Korean / Chinese are offered only when this host returns their shared system font
    // (sw_modfont::ProbeSysfonts, once per module instance = per boot).
    bool sysfont_probed = false;
};

EdenDsmodBool SupportsBuild(const char* build_id_hex) {
    return sw_build::SupportsBuildHex(build_id_hex) ? EDEN_DSMOD_TRUE : EDEN_DSMOD_FALSE;
}

void* Create(const EdenDsmodHostApi* host, const char* config_json) {
    try {
        if (!dsmod_sdk::HostAbiMatches(host) || host->title_id != sw_build::kTitleId ||
            !host->is_mapped || !host->read_memory || !host->get_heap_begin)
            return nullptr;
        const char* label = sw_build::BuildLabel(host->build_id);
        if (!label) return nullptr; // an id this module has never catalogued: let the host's
                                     // generic "module failed to load" diagnostics show instead
        auto* m = new Module{};
        m->build_label = *label ? label : nullptr; // "" -> nullptr means "the supported build"
        // A malformed/absent config fails closed to false (matching MetroidDread's own
        // `config.value("_build_match", false)`): the manifest's "Update Required" overlay stays
        // up rather than a parse hiccup silently waving through an unverified build.
        if (config_json && *config_json) {
            try {
                const auto config = nlohmann::json::parse(config_json);
                m->build_match = config.value("_build_match", false);
            } catch (...) {
                m->build_match = false;
            }
        }
        return m;
    } catch (...) {
        return nullptr;
    }
}

void Destroy(void* p) {
    try {
        if (auto* m = static_cast<Module*>(p)) {
            std::lock_guard lock{m->direct.mutex};
            // Review L1: nothing staged may run once the module is gone -- cancel both request
            // channels and the photo worker, give the plates back, and withdraw magic last.
            // An already active photo worker must see explicit cancellation to finish cleanup.
            if (m->direct.portrait) m->direct.portrait->Shutdown();
            if (m->direct.field) m->direct.field->Shutdown();
            if (m->direct.hud) m->direct.hud->Release(); // plates back on the top screen
            if (m->direct.menu) m->direct.menu->Release(); // battle menus back on the top screen
            if (m->direct.bridge) m->direct.bridge->Shutdown();
        }
        delete static_cast<Module*>(p);
    } catch (...) {
    }
}

// One tick of the direct battle commands: bridge upkeep (helper presence, "menu waiting" from the
// guest's per-frame beat, adaptive windows), outcome text, and the page's lock/unlock gates.
void DirectUpkeep(Module& m, const EdenDsmodHostApi& host) {
    const std::int64_t now = NowMs();
    std::lock_guard lock{m.direct.mutex};
    auto& d = m.direct;
    d.view = m.reader.battle_view();
    sw_helper::Status st{};
    if (d.bridge) {
        st = d.bridge->status(); // Update() ran at the top of SampleCallback
        d.view.max_active = m.reader.battle_dmax_active() || (st.preview.valid && st.preview.active);

        sw_helper::Outcome out;
        if (d.bridge->TakeOutcome(out)) {
            if (out.answered && out.code == sw_helper::Result::Ok) {
                if (out.op != sw_helper::Op::OpenBag && out.op != sw_helper::Op::OpenPokemon &&
                    out.op != sw_helper::Op::Decide) ++d.committed;
                d.status.clear();
            } else {
                d.dmax_wanted = false;
                // B-helper.md finding #1: a staged OpenBag/OpenPokemon the guest later refuses
                // (a race -- the menu stopped waiting between tap and guest execution) must not
                // leave the Bag/Pokemon capsule shown "selected" over a command that never ran.
                if (out.op == sw_helper::Op::OpenBag || out.op == sw_helper::Op::OpenPokemon ||
                    out.op == sw_helper::Op::Decide)
                    d.selected_command = 0;
                d.status = out.answered ? sw_helper::ResultText(out.code) : sw_str::S(sw_str::Id::kResCancelled);
                d.status_until = now + 3000;
            }
            if (host.log) {
                static constexpr const char* kOpNames[] = {"none", "fight", "run", "switch", "item", "maxfight", "openbag", "openpokemon", "decide"};
                const auto op_i = static_cast<unsigned>(out.op);
                char line[128];
                std::snprintf(line, sizeof line, SW_VARIANT_NAME " direct %s: %s code=%u value=%u",
                              op_i < 9 ? kOpNames[op_i] : "?",
                              out.answered ? "answered" : "unanswered",
                              static_cast<unsigned>(out.code), static_cast<unsigned>(out.value));
                host.log(host.userdata, EDEN_DSMOD_LOG_INFO, line);
            }
        }
    }
    if (d.cursor) {
        sw_helper::CursorResult code{};
        if (d.cursor->Poll(now, &code) && host.log) {
            char line[96];
            std::snprintf(line, sizeof line, SW_VARIANT_NAME " native cursor: code=%u", static_cast<unsigned>(code));
            host.log(host.userdata, EDEN_DSMOD_LOG_INFO, line);
        }
    }
    // The battle client held through an emulation stall (Reader::SetBattleHint): a stall keeps the
    // page's selection and the Max-move preview; the battle ending or a new client resets them.
    const std::uint64_t client = m.reader.battle_client();
    if (!d.view.in_battle || !client || d.command_serial != st.battle_serial) {
        d.selected_command = 0;
        d.command_serial = st.battle_serial;
    }
    if (!d.view.in_battle || !client ||
        (d.last_preview.valid && (d.last_preview.client != client ||
                                 d.last_preview.mon != m.reader.battle_own_actor()))) {
        d.dmax_wanted = false;
        d.last_preview = {};
    }
    if (d.view.max_active || (st.menu_waiting && st.preview.valid && !st.preview.eligible))
        d.dmax_wanted = false;
    // The native move list (top in FIGHT input) keeps the companion's move list up (cursor sync).
    const bool native_fight = m.reader.native_cursor().menu == sw_battle_cursor::Menu::Fight ||
                              m.reader.native_cursor().menu == sw_battle_cursor::Menu::Target;
    if (!d.view.in_battle || !client ||
        !(m.reader.native_battle_command_waiting() || (native_fight && m.reader.battle_screen() == 1) ||
          (m.reader.native_cursor().menu == sw_battle_cursor::Menu::Balls && m.reader.battle_screen() == 2)))
        m.reader.SetBattleScreen(0);
    // Top -> bottom: the native command cursor drives the bottom command highlight while the
    // native command menu waits; otherwise the companion-owned selection stands (fail closed).
    if (m.reader.native_battle_command_waiting() && m.reader.native_cursor().command >= 0)
        d.selected_command = m.reader.native_cursor().command;
    m.reader.PublishBattleScreen(host);
    if (!d.status.empty() && now > d.status_until) d.status.clear();
    if (!host.publish_i64) return;
    // Review OPT1: everything below (CheckFight x4/CheckBall x2/CheckOpenMenu, Dynamax
    // eligibility, ~80 publish calls) only feeds the battle/raid pages' own bindings
    // (sw.direct.*/sw.battle.*, sw.hud.* diagnostics) -- skip it while neither is on screen, the
    // page-gate TickCallback's HUD logic already uses (same page set, raid pages added since both
    // also bind sw.direct.*). A host that cannot name the page (CurrentPage empty) keeps the old
    // always-on behaviour.
    {
        const std::string_view page = CurrentPage(host);
        const bool relevant_page = page.empty() || page == "battle" || page == "battle_moves" ||
                                   page == "battle_balls" || page == "raid" || page == "raid_moves";
        if (!relevant_page) return;
    }
    host.publish_i64(host.userdata, "sw.battle.command.index", d.selected_command);
    for (int i = 0; i < 6; ++i) { // Fight, Pokemon, Bag, Run, Back, Cheer On
        const auto key = "sw.battle.command.selected" + std::to_string(i);
        host.publish_i64(host.userdata, key.c_str(), d.selected_command == i ? 1 : 0);
    }
    const bool native_max = sw_dmax_native::NativeMaxSelected(m.reader.native_cursor());
    const bool selected = d.dmax_wanted || d.view.max_active || native_max;
    bool base = false;
    host.publish_i64(host.userdata, "sw.direct.helper", st.helper_present ? 1 : 0);
    host.publish_i64(host.userdata, "sw.direct.menu_waiting", st.menu_waiting ? 1 : 0);
    host.publish_i64(host.userdata, "sw.direct.busy", st.busy ? 1 : 0);
    host.publish_i64(host.userdata, "sw.direct.kind", st.battle_kind); // diagnostics: rule|competitor|comm
    host.publish_i64(host.userdata, "sw.direct.offline_single", st.offline_single ? 1 : 0);
    host.publish_i64(host.userdata, "sw.direct.offline_raid", st.offline_raid ? 1 : 0);
    const bool open_ready = m.reader.native_battle_command_waiting() &&
                           sw_helper::CheckOpenMenu(st, d.view) == sw_helper::Refusal::None;
    host.publish_i64(host.userdata, "sw.direct.bag_open.ready", open_ready ? 1 : 0);
    host.publish_i64(host.userdata, "sw.direct.pokemon_open.ready", open_ready ? 1 : 0);
    host.publish_i64(host.userdata, "sw.direct.info.ready", m.reader.native_battle_command_waiting() &&
                     st.helper_present && d.view.in_battle && st.menu_waiting && !st.busy &&
                     (st.offline_single || (sw_helper::RaidDirectVerified && st.offline_raid)) ? 1 : 0);
    // X opens the companion picker. Only its explicit Use action submits a native ball throw.
    host.publish_i64(host.userdata, "sw.direct.ball_open.ready",
                     open_ready && m.reader.battle_has_balls() &&
                     sw_helper::CheckBall(st, d.view, m.reader.battle_ball_id(),
                         m.reader.BattleItemCount(m.reader.battle_ball_id())) == sw_helper::Refusal::None ? 1 : 0);
    const bool ball_input = m.reader.battle_screen() == 2 && open_ready && m.reader.battle_has_balls();
    const bool ball_use = ball_input && sw_helper::CheckBall(st, d.view, m.reader.battle_ball_id(),
        m.reader.BattleItemCount(m.reader.battle_ball_id())) == sw_helper::Refusal::None;
    host.publish_i64(host.userdata, "sw.battle.ball.use_ready", ball_use ? 1 : 0);
    host.publish_i64(host.userdata, "sw.direct.dmax.selected", selected ? 1 : 0);
    const bool max_ready = sw_helper::DynamaxDirectVerified && st.helper_present &&
                           d.view.in_battle && d.view.own_ok && st.menu_waiting && !st.busy &&
                           st.preview.valid && st.preview.eligible && !d.view.max_active &&
                           sw_helper::RaidDirectVerified && st.offline_raid;
    host.publish_i64(host.userdata, "sw.direct.dmax.ready", max_ready ? 1 : 0);
    // The native Fight screen's own Dynamax button is on offer (the native opportunity query of
    // this command turn, for the Pokemon now choosing): the bottom shows its toggle and drives
    // the native button (battle_dynamax below), any battle kind -- Gym, trainer, Battle Tower.
    const bool dmax_native = sw_dmax_native::NativeDmaxOffered(m.reader.native_cursor(), st, d.last_preview,
                                                               d.view, m.reader.battle_own_actor(),
                                                               m.reader.battle_client());
    host.publish_i64(host.userdata, "sw.direct.dmax.native", dmax_native ? 1 : 0);
    host.publish_i64(host.userdata, "sw.direct.dmax.native_on", native_max ? 1 : 0);
    host.publish_i64(host.userdata, "sw.direct.committed", d.committed);
    for (int k = 0; k < 4; ++k) {
        char key[40];
        std::snprintf(key, sizeof key, "sw.direct.move%d.ready", k + 1);
        const bool ready = sw_helper::CheckFight(st, d.view, k, d.dmax_wanted || d.view.max_active) ==
                           sw_helper::Refusal::None;
        base = base || ready;
        host.publish_i64(host.userdata, key, ready ? 1 : 0);
    }
    host.publish_i64(host.userdata, "sw.direct.run.ready",
                     sw_helper::CheckRun(st, d.view) == sw_helper::Refusal::None ? 1 : 0);
    // D2 20-battle-turn-animation-dimmed: in battle with the helper present but the command menu
    // not waiting (turn animation, messages, native party/bag screen) -> the page keeps its last
    // data, dimmed, commands locked.
    host.publish_i64(host.userdata, "sw.battle.dimmed",
                     st.helper_present && d.view.in_battle && !st.menu_waiting ? 1 : 0);
    host.publish_i64(host.userdata, "sw.direct.ready", base ? 1 : 0);
    // Native drive (doubles / institution): the bottom's capsules, move rows and target cards move
    // the native cursor; the highlighted one is the A button (manifest d.drive.*).
    host.publish_i64(host.userdata, "sw.battle.drive", NativeDrive(st) ? 1 : 0);
    host.publish_i64(host.userdata, "sw.battle.drive_cmd", NativeDrive(st) || st.offline_raid ? 1 : 0);
    // HUD hide status (diagnostics; the page does not bind them).
    const sw_helper::HudStatus hs = d.hud ? d.hud->status() : sw_helper::HudStatus{};
    host.publish_i64(host.userdata, "sw.hud.helper", d.hud ? 1 : 0);
    host.publish_i64(host.userdata, "sw.hud.hp_top", d.hp_flag);
    host.publish_i64(host.userdata, "sw.hud.menu_top", d.menu_flag);
    {
        const sw_helper::MenuStatus ms = d.menu ? d.menu->status() : sw_helper::MenuStatus{};
        host.publish_i64(host.userdata, "sw.hud.menu.helper", d.menu ? 1 : 0);
        host.publish_i64(host.userdata, "sw.hud.menu.requested", ms.requested);
        host.publish_i64(host.userdata, "sw.hud.menu.hidden", ms.hidden);
        host.publish_i64(host.userdata, "sw.hud.menu.catch_prompt", m.reader.catch_prompt() ? 1 : 0);
    }
    host.publish_i64(host.userdata, "sw.hud.requested", hs.requested ? 1 : 0);
    host.publish_i64(host.userdata, "sw.hud.layout_live", hs.layout_live ? 1 : 0);
    host.publish_i64(host.userdata, "sw.hud.plates_found", sw_helper::HudCount(hs.found));
    host.publish_i64(host.userdata, "sw.hud.plates_hidden", sw_helper::HudCount(hs.hidden));
    host.publish_i64(host.userdata, "sw.direct.status_ready", d.status.empty() ? 0 : 1);
    if (host.publish_text) host.publish_text(host.userdata, "sw.direct.status", d.status.c_str());
}

// The Bag use result (owner 2026-11-01): the count-up runs kUseCountMs (ease-out), the popup
// closes kUseShowMs after the guest's OK (Reduced motion: the final values at once, closed after
// kUseShowReducedMs). A use the guest never answers is dropped after kUseWaitMs.
constexpr std::int64_t kUseCountMs = 800, kUseShowMs = 1400, kUseShowReducedMs = 800, kUseWaitMs = 5000;

std::int64_t HostInt(const EdenDsmodHostApi& host, const std::string& key, std::int64_t fallback = 0) {
    return host.get_i64 ? host.get_i64(host.userdata, key.c_str(), fallback) : fallback;
}

std::string HostText(const EdenDsmodHostApi& host, const std::string& key) {
    const char* t = host.get_text ? host.get_text(host.userdata, key.c_str()) : nullptr;
    return t ? std::string(t) : std::string{};
}

// A member's values as the reader publishes them (the "before" of a use).
Direct::UseShow SnapshotUse(const EdenDsmodHostApi& host, int slot) {
    Direct::UseShow u;
    const std::string p = "sw.party." + std::to_string(slot) + ".";
    u.hp0 = static_cast<int>(HostInt(host, p + "current_hp"));
    u.max0 = static_cast<int>(HostInt(host, p + "max_hp"));
    u.level0 = static_cast<int>(HostInt(host, p + "level"));
    u.status0 = HostInt(host, p + "status_ready") != 0;
    u.status_src0 = HostText(host, p + "status_src");
    for (int j = 0; j < 4; ++j)
        u.pp0[static_cast<std::size_t>(j)] = static_cast<int>(HostInt(host, p + "move" + std::to_string(j + 1) + ".pp"));
    return u;
}

// sw.bag.use.res.*: the chosen member's HP / max HP / level / PP counting up from the values
// before the use to the live ones (which the reader updates once the game applied the item),
// and the popup's close once the result has been shown. Returns whether the result shows the
// member's moves (a PP item).
bool FieldPublishUse(Module& m, const EdenDsmodHostApi& host) {
    auto& d = m.direct;
    const std::int64_t now = NowMs();
    const bool reduced = HostInt(host, "@flag:motion.reduced") != 0;
    if (d.use_show) {
        const auto& u = *d.use_show;
        const bool done = u.start ? now - u.start >= (reduced ? kUseShowReducedMs : kUseShowMs)
                                  : now - u.staged_at >= kUseWaitMs;
        if (done || !m.reader.live_party_slot_ok(u.slot)) {
            d.use_show.reset();
            d.field_target_popup = false;
            d.field_target_item = 0;
            d.field_pp_target = -1;
        }
    }
    const auto pub = [&](const char* key, std::int64_t v) { host.publish_i64(host.userdata, key, v); };
    const auto pubt = [&](const char* key, const std::string& v) {
        if (host.publish_text) host.publish_text(host.userdata, key, v.c_str());
    };
    pub("sw.bag.use.res", d.use_show ? 1 : 0);
    if (!d.use_show) {
        // The "before" values: taken once when the target picker opens (its open edge), not on
        // every tick it stays open -- nothing changes passively while a field menu is open, and
        // the picker closes after each use.
        if (d.field_target_popup && !d.use_before_taken)
            for (int i = 0; i < 6; ++i) d.use_before[static_cast<std::size_t>(i)] = SnapshotUse(host, i);
        d.use_before_taken = d.field_target_popup;
        return false;
    }
    const auto& u = *d.use_show;
    const std::string p = "sw.party." + std::to_string(u.slot) + ".";
    // 0 while the guest applies it, then an ease-out 0..1 over kUseCountMs (Reduced motion: 1).
    double k = 0;
    if (u.start) {
        const double t = std::min(1.0, static_cast<double>(now - u.start) / kUseCountMs);
        k = reduced ? 1.0 : 1.0 - (1.0 - t) * (1.0 - t) * (1.0 - t);
    }
    const auto lerp = [k](int a, int b) { return a + static_cast<int>((b - a) * k + (b >= a ? 0.5 : -0.5)); };
    const int hp = lerp(u.hp0, static_cast<int>(HostInt(host, p + "current_hp", u.hp0)));
    const int max_hp = std::max(1, lerp(u.max0, static_cast<int>(HostInt(host, p + "max_hp", u.max0))));
    const int level = lerp(u.level0, static_cast<int>(HostInt(host, p + "level", u.level0)));
    const int pct = std::clamp(hp * 100 / max_hp, 0, 100);
    pub("sw.bag.use.res.slot", u.slot);
    pub("sw.bag.use.res.move", u.move);
    pub("sw.bag.use.res.shown", u.start ? 1 : 0);
    pub("sw.bag.use.res.hp", std::clamp(hp, 0, max_hp));
    pub("sw.bag.use.res.max_hp", max_hp);
    pub("sw.bag.use.res.hpg", pct > 50 ? 1 : 0);
    pub("sw.bag.use.res.hpy", pct > 20 && pct <= 50 ? 1 : 0);
    pub("sw.bag.use.res.hpr", pct <= 20 ? 1 : 0);
    char text[24];
    std::snprintf(text, sizeof text, "%d%%", pct);
    pubt("sw.bag.use.res.hp_pct_text", text);
    pubt("sw.bag.use.res.level_text", sw_str::Level(level));
    // The status badge: the live one when the member still has a status, else the one it had
    // before the use until the count-up is half done (it fades out as the ailment clears).
    const bool live_status = HostInt(host, p + "status_ready") != 0;
    pub("sw.bag.use.res.status_ready", live_status || (u.status0 && k < 0.5) ? 1 : 0);
    pubt("sw.bag.use.res.status_src", live_status ? HostText(host, p + "status_src") : u.status_src0);
    // The change itself (critic recheck 2026-11-01), from the before values to the live ones the
    // reader shows once the game applied the item -- nothing while the guest is still applying it:
    // revived (fainted -> HP), "+N HP", "+N Lv.", cured (a status gone, not by a revive), PP up.
    {
        const int hp_live = static_cast<int>(HostInt(host, p + "current_hp", u.hp0));
        const int level_live = static_cast<int>(HostInt(host, p + "level", u.level0));
        const bool applied = u.start != 0;
        const bool revived = applied && u.hp0 == 0 && hp_live > 0;
        std::string gain;
        if (applied && level_live > u.level0) { // a candy: its HP rise is the level's, not a heal
            std::string prefix = sw_str::S(sw_str::Id::kLevelPrefix);
            while (!prefix.empty() && prefix.back() == ' ') prefix.pop_back();
            gain = "+" + std::to_string(level_live - u.level0) + " " + prefix;
        } else if (applied && !revived && hp_live > u.hp0) {
            gain = "+" + std::to_string(hp_live - u.hp0) + " " + sw_str::S(sw_str::Id::kStatHp);
        }
        bool pp_up = false;
        for (int j = 0; j < 4; ++j)
            pp_up = pp_up || HostInt(host, p + "move" + std::to_string(j + 1) + ".pp", u.pp0[static_cast<std::size_t>(j)]) >
                                 u.pp0[static_cast<std::size_t>(j)];
        pub("sw.bag.use.res.revived", revived ? 1 : 0);
        pub("sw.bag.use.res.gain_ready", gain.empty() ? 0 : 1);
        pubt("sw.bag.use.res.gain_text", gain);
        pub("sw.bag.use.res.cured", applied && !revived && u.status0 && !live_status ? 1 : 0);
        pub("sw.bag.use.res.pp", applied && pp_up ? 1 : 0);
    }
    for (int j = 0; j < 4; ++j) {
        const std::string mv = p + "move" + std::to_string(j + 1) + ".";
        char key[48];
        const int pp = lerp(u.pp0[static_cast<std::size_t>(j)],
                            static_cast<int>(HostInt(host, mv + "pp", u.pp0[static_cast<std::size_t>(j)])));
        std::snprintf(text, sizeof text, "%d/%d", pp, static_cast<int>(HostInt(host, mv + "pp_max")));
        std::snprintf(key, sizeof key, "sw.bag.use.res.move%d.pp_text", j + 1);
        pubt(key, text);
    }
    return u.pp_view;
}

// Field actions: one tick of the field channel -- outcomes (toast / log), the periodic FLY_QUERY,
// and the Bag / Town Map / Fly page gates.
void FieldUpkeep(Module& m, const EdenDsmodHostApi& host) {
    const std::int64_t now = NowMs();
    std::lock_guard lock{m.direct.mutex};
    auto& d = m.direct;
    sw_helper::FieldStatus st{};
    if (d.field) {
        st = d.field->status();
        sw_helper::FieldOutcome out;
        if (d.field->TakeOutcome(out)) {
            if (out.op == sw_helper::FieldOp::FlyQuery) {
                if (out.answered && out.code == sw_helper::Result::Ok) {
                    d.fly_mask = out.fly_mask;
                    d.fly_state = out.fly_state;
                    d.fly_known = true;
                    d.bike_known = true;
                    d.bike_usable = out.bike_usable;
                    m.reader.SetMapZoneHint(out.map_zone, out.map_parent);
                }
            } else {
                const bool use_op = out.op == sw_helper::FieldOp::Medicine || out.op == sw_helper::FieldOp::Progression;
                if (use_op && d.use_show && !d.use_show->start) {
                    if (out.answered && out.code == sw_helper::Result::Ok) {
                        d.use_show->start = now;  // show the result, then close (FieldPublishUse)
                    } else {
                        d.use_show.reset();       // refused: close now, the toast tells why
                        d.field_target_popup = false;
                    }
                }
                if (out.answered && out.code == sw_helper::Result::Ok) {
                    ++d.field_committed;
                    d.field_status.clear();
                    if (out.op == sw_helper::FieldOp::PartySwap) { ++d.swap_committed; d.swap_flash_until = now + 150; }
                    if (out.op == sw_helper::FieldOp::Fly) d.fly_known = false; // the zone changes
                } else {
                    d.field_status = out.answered ? sw_helper::ResultText(out.code) : sw_str::S(sw_str::Id::kResCancelled);
                    d.field_status_until = now + 3500;
                }
                // Any Bike outcome changes (or re-tells) whether biking works here: re-ask at once.
                if (out.op == sw_helper::FieldOp::Bike) d.fly_query_at = now - 10000;
                if (host.log) {
                    static constexpr const char* kOpNames[] = {"none", "medicine", "repel", "fly", "fly_query", "bike", "progression", "party_swap"};
                    const auto op_i = static_cast<unsigned>(out.op);
                    char line[128];
                    std::snprintf(line, sizeof line, SW_VARIANT_NAME " field %s: %s code=%u value=%u",
                                  op_i < 8 ? kOpNames[op_i] : "?", out.answered ? "answered" : "unanswered",
                                  static_cast<unsigned>(out.code), static_cast<unsigned>(out.value));
                    host.log(host.userdata, EDEN_DSMOD_LOG_INFO, line);
                }
            }
        }
        st = d.field->status();
        if (d.field_deferred) {
            const bool field_ok_now = st.live && st.player_free && !m.reader.in_battle_phase() &&
                                      !m.reader.in_title_or_loading();
            const bool waiting = d.field_deferred->until > now && st.live && !m.reader.in_battle_phase() &&
                                 !m.reader.in_title_or_loading();
            if (!field_ok_now && waiting) {
                // a Bike tap made while the player was briefly not free (step/turn animation) waits
            } else if (!field_ok_now) {
                d.field_deferred.reset(); // the field stopped before it could run: dropped
            } else if (!st.busy) {
                const auto df = *d.field_deferred;
                d.field_deferred.reset();
                d.field->Stage(df.op, df.arg, df.expect, now, df.zone, df.landing, df.ec, df.move);
                st = d.field->status();
            }
        }
        // Re-ask the game which destinations are open (and whether flying is allowed here):
        // every 3 s while the field runs and the Town Map or Fly page is on screen (review L2:
        // ~45 lookups in one game frame are not worth it on any other page), sooner after a Fly
        // or the first beat.
        const std::string_view page = CurrentPage(host);
        const bool fly_page = page.empty() || page == "map" || page == "fly";
        const std::int64_t every = d.fly_known ? 3000 : 600;
        if (fly_page && st.live && !st.busy && !d.field_deferred &&
            !(d.portrait && d.portrait->busy()) && now - d.fly_query_at >= every) {
            std::uint64_t pairs[sw_fly::kDestinations.size()][2];
            for (std::size_t i = 0; i < sw_fly::kDestinations.size(); ++i) {
                pairs[i][0] = sw_fly::kDestinations[i].zone;
                pairs[i][1] = sw_fly::kDestinations[i].landing;
            }
            if (d.field->StageQuery(pairs, static_cast<std::uint32_t>(sw_fly::kDestinations.size()), now))
                d.fly_query_at = now;
            st = d.field->status();
        }
    }
    if (!d.field_status.empty() && now > d.field_status_until) d.field_status.clear();
    if (d.map_fly_selection != m.reader.map_selection().index()) {
        d.map_fly_selection = m.reader.map_selection().index();
        d.map_fly_open = false;
    }
    const auto map_plan = sw_map_selection::Plan(m.reader.map_catalog(), m.reader.map_selection(),
        d.fly_mask, d.fly_known, d.fly_state == 0);
    const auto map_page = CurrentPage(host);
    // Leaving the picker through the app rail must clear its opener without redirecting
    // the chosen app back to Town Map. Only a closed picker still on Fly returns to Map.
    if (map_page != "map" && map_page != "fly") d.map_fly_open = false;
    d.fly_row_count = 0;
    if (m.reader.map_selection().Get(m.reader.map_catalog())) {
        for (int index : map_plan.indices) d.fly_rows[static_cast<std::size_t>(d.fly_row_count++)] = index;
    } else if (d.fly_known)
        for (std::size_t i = 0; i < sw_fly::kDestinations.size(); ++i)
            if ((d.fly_mask >> i) & 1) d.fly_rows[static_cast<std::size_t>(d.fly_row_count++)] = static_cast<int>(i);
    if (!host.publish_i64) return;
    // Review H1: Fly / Use only while the player has control (the guest's field_free -- no event
    // script, dialogue, cutscene, warp or X-menu transition running); the guest re-checks it when
    // the request executes.
    const bool field_ok = st.helper_present && st.live && st.player_free && !m.reader.in_battle_phase() &&
                          !m.reader.in_title_or_loading();
    // The background FLY_QUERY (one guest frame every few seconds) never locks the page: a tap that
    // lands on it waits for it (FieldAction's deferred request).
    const bool free = field_ok && !(d.portrait && d.portrait->busy()) && (!st.busy || (d.field && d.field->pending_op() == sw_helper::FieldOp::FlyQuery &&
                                                !d.field_deferred));
    host.publish_i64(host.userdata, "sw.field.helper", st.helper_present ? 1 : 0);
    host.publish_i64(host.userdata, "sw.field.live", field_ok ? 1 : 0);
    host.publish_i64(host.userdata, "sw.field.free", st.player_free ? 1 : 0);
    host.publish_i64(host.userdata, "sw.field.busy", st.busy ? 1 : 0);
    host.publish_i64(host.userdata, "sw.field.committed", d.field_committed);
    host.publish_i64(host.userdata, "sw.bike.ready", free && d.bike_known && d.bike_usable ? 1 : 0);
    // Fly
    host.publish_i64(host.userdata, "sw.fly.known", d.fly_known ? 1 : 0);
    host.publish_i64(host.userdata, "sw.fly.here_ok", d.fly_known && d.fly_state == 0 ? 1 : 0);
    host.publish_i64(host.userdata, "sw.fly.ready",
                     free && d.fly_known && d.fly_state == 0 && d.fly_row_count > 0 ? 1 : 0);
    host.publish_i64(host.userdata, "sw.fly.count", d.fly_row_count);
    host.publish_i64(host.userdata, "sw.map.fly.ready", free && m.reader.map_ready() && map_plan.ready ? 1 : 0);
    host.publish_i64(host.userdata, "sw.map.fly.count", map_plan.indices.size());
    host.publish_i64(host.userdata, "sw.map.fly.total", map_plan.total);
    host.publish_i64(host.userdata, "sw.map.fly.reason", static_cast<int>(sw_map_selection::Reason(map_plan)));
    host.publish_i64(host.userdata, "sw.map.fly.multiple", map_plan.requires_choice ? 1 : 0);
    host.publish_i64(host.userdata, "sw.map.fly.list_open", d.map_fly_open && map_plan.requires_choice ? 1 : 0);
    host.publish_i64(host.userdata, "sw.map.fly.return_map",
                     map_page == "fly" && !(d.map_fly_open && map_plan.requires_choice) ? 1 : 0);
    // Review OPT2: only the Town Map/Fly page binds sw.fly.row.*, same as the FLY_QUERY gate
    // above -- publish up to 45 rows x3 fields only while one of them is on screen (map_page,
    // read once above; sw.fly.count stays unconditional, cheap, and is what a page not showing
    // any rows would need to know there is nothing to show).
    if (host.publish_text && (map_page.empty() || map_page == "map" || map_page == "fly")) {
        char key[48];
        for (int r = 0; r < d.fly_row_count; ++r) {
            const auto index = static_cast<std::size_t>(d.fly_rows[static_cast<std::size_t>(r)]);
            std::snprintf(key, sizeof key, "sw.fly.row.%d.name", r);
            host.publish_text(host.userdata, key, m.reader.FlyName(host, index).c_str());
            std::snprintf(key, sizeof key, "sw.fly.row.%d.region", r);
            host.publish_text(host.userdata, key, m.reader.FlyRegion(index));
            std::snprintf(key, sizeof key, "sw.fly.row.%d.native_index", r);
            host.publish_i64(host.userdata, key, d.fly_rows[static_cast<std::size_t>(r)]);
        }
    }
    // Bag "Use": medicine -> the party target grid, Repel -> the Use capsule, else locked.
    const int item = m.reader.bag_sel_item();
    const int count = m.reader.bag_sel_count();
    const bool progression = sw_helper::FieldProgressionVerified && sw_helper::IsFieldProgression(item);
    const int kind = sw_helper::IsFieldMedicine(item) || progression ? 1 : sw_helper::IsRepel(item) ? 2 : 0;
    // A use being applied / shown keeps the popup even though the selected Bag row may have run
    // out (the last Potion) or moved; only leaving the field or the Bag page closes it.
    const bool off_bag = m.reader.in_battle_phase() || m.reader.in_title_or_loading() ||
                         (!CurrentPage(host).empty() && CurrentPage(host) != "bag");
    if (off_bag) d.use_show.reset();
    if (!d.use_show && (kind != 1 || item != d.field_target_item || count <= 0 || off_bag)) {
        d.field_target_popup = false;
        d.field_pp_target = -1;
    }
    const bool use_pp_view = FieldPublishUse(m, host);
    host.publish_i64(host.userdata, "sw.bag.use.popup", d.field_target_popup ? 1 : 0);
    host.publish_i64(host.userdata, "sw.bag.use.kind", kind);
    host.publish_i64(host.userdata, "sw.bag.use.medicine", kind == 1 ? 1 : 0);
    const bool pp_single = sw_helper::IsFieldPpSingle(item);
    host.publish_i64(host.userdata, "sw.bag.use.pp_single", pp_single ? 1 : 0);
    if (d.field_pp_target >= 0 && (item != d.field_pp_item || !m.reader.live_party_slot_ok(d.field_pp_target) ||
        m.reader.live_party_ec(d.field_pp_target) != d.field_pp_target_ec)) d.field_pp_target = -1;
    // The move picker: a single-move PP item's chosen member, or the moves of the member a PP item
    // was just used on (the result view).
    const bool target_ready = (pp_single && d.field_pp_target >= 0) || use_pp_view;
    const int target_slot = use_pp_view ? d.use_show->slot : d.field_pp_target;
    host.publish_i64(host.userdata, "sw.bag.use.target_ready", target_ready ? 1 : 0);
    host.publish_i64(host.userdata, "sw.bag.use.target_slot", target_slot);
    if (host.publish_text) {
        char key[48];
        std::snprintf(key, sizeof key, "sw.party.%d.name", target_slot);
        const char* name = target_ready && host.get_text ? host.get_text(host.userdata, key) : nullptr;
        host.publish_text(host.userdata, "sw.bag.use.target_name", name ? name : "");
    }
    const bool med_ready = kind == 1 && free && count > 0 && !d.use_show; // one use at a time
    host.publish_i64(host.userdata, "sw.bag.use.med_ready", med_ready ? 1 : 0);
    // Per tile (review M1): a member's "use on" cell unlocks only on its own valid live slot.
    // Review OPT2: this grid is only ever drawn on the Bag page (d.bag.use.slot.N.locked's own
    // widget, gen_manifest.py); map_page (read once above) doubles as the gate.
    if (map_page.empty() || map_page == "bag") {
        for (int i = 0; i < 6; ++i) {
            char key[40];
            std::snprintf(key, sizeof key, "sw.bag.use.slot.%d.ready", i);
            host.publish_i64(host.userdata, key, med_ready && m.reader.live_party_slot_ok(i) ? 1 : 0);
        }
    }
    host.publish_i64(host.userdata, "sw.bag.use.repel_ready",
                     kind == 2 && free && count > 0 && m.reader.repel_steps() == 0 ? 1 : 0);
    // Toast (two lines: the game's own message text keeps its line break).
    const auto nl = d.field_status.find('\n');
    host.publish_i64(host.userdata, "sw.field.status_ready", d.field_status.empty() ? 0 : 1);
    host.publish_i64(host.userdata, "sw.field.status2_ready", nl != std::string::npos ? 1 : 0);
    if (host.publish_text) {
        host.publish_text(host.userdata, "sw.field.status", d.field_status.substr(0, nl).c_str());
        host.publish_text(host.userdata, "sw.field.status2", nl != std::string::npos ? d.field_status.c_str() + nl + 1 : "");
    }
}

// Stage a field action. false -> the package's `refused` haptic (+ the game's own text when it
// has something to say).
bool FieldAction(Module& m, std::string_view action, std::int64_t argument) {
    std::lock_guard lock{m.direct.mutex};
    auto& d = m.direct;
    if (action == "map_fly_close") { d.map_fly_open = false; return true; }
    const auto refuse = [&](sw_helper::Result r) {
        d.field_status = sw_helper::ResultText(r);
        d.field_status_until = NowMs() + 3500;
        return false;
    };
    if (action == "field_use_cancel") {
        d.use_show.reset();
        d.field_target_popup = false;
        d.field_target_item = 0;
        d.field_pp_target = -1;
        d.field_pp_target_ec = 0;
        d.field_pp_item = 0;
        return true; // companion-only: Back must work even while the game is in a menu/dialogue
    }
    if (!d.field) return false;
    if (d.portrait && d.portrait->busy()) return refuse(sw_helper::Result::NotFree);
    const auto& st = d.field->status();
    const bool field_ok = st.helper_present && st.live && !m.reader.in_battle_phase() && !m.reader.in_title_or_loading();
    if (!field_ok) return false;
    // Owner Thor 2026-10-31 ("bike is not instant, sometimes need to press again"): a Bike tap while
    // the player is momentarily not free waits up to 1.5 s for control instead of being refused.
    if (!st.player_free && action == "field_bike" && !d.field_deferred) {
        d.field_deferred = Direct::Deferred{sw_helper::FieldOp::Bike, 0, 0, 0, 0, 0, 0, NowMs() + 1500};
        return true;
    }
    if (!st.player_free) return refuse(sw_helper::Result::NotFree); // review H1
    Direct::Deferred req{sw_helper::FieldOp::None, 0, 0, 0, 0, 0};
    std::optional<Direct::UseShow> show;   // a medicine / PP / candy use: its "before" values
    if (action == "fly_go" || action == "map_fly") {
        // The game's own "can't call a Flying Taxi" outcome is told first (the Town Map's locked
        // Fly capsule sends -1 just for this).
        if (d.fly_known && d.fly_state != 0)
            return refuse(d.fly_state == 2 ? sw_helper::Result::FlyNotNow : sw_helper::Result::FlyNotHere);
        int index = -1;
        if (action == "map_fly") {
            if (!m.reader.map_ready()) return false;
            const auto plan = sw_map_selection::Plan(m.reader.map_catalog(), m.reader.map_selection(),
                d.fly_mask, d.fly_known, d.fly_state == 0);
            if (!plan.ready) return false;
            if (plan.requires_choice) { d.map_fly_open = true; return true; }
            index = plan.single_index;
        } else {
            if (!d.fly_known || argument < 0 || argument >= d.fly_row_count) return false;
            index = d.fly_rows[static_cast<std::size_t>(argument)];
        }
        if (index < 0 || index >= static_cast<int>(sw_fly::kDestinations.size())) return false;
        const auto& dest = sw_fly::kDestinations[static_cast<std::size_t>(index)];
        req = {sw_helper::FieldOp::Fly, 0, 0, dest.zone, dest.landing, 0};
    } else if (action == "field_bike") {
        // No refusal from the cached FLY_QUERY answer (it can be up to 3 s old, e.g. just after
        // leaving a building): the guest re-runs the game's own CanBike check when it executes.
        req = {sw_helper::FieldOp::Bike, 0, 0, 0, 0, 0};
    } else {
        const int item = m.reader.bag_sel_item();
        if (m.reader.bag_sel_count() <= 0) return false;
        if (action == "field_use_open") {
            if (d.field_deferred || (st.busy && d.field->pending_op() != sw_helper::FieldOp::FlyQuery))
                return refuse(sw_helper::Result::NotFree);
            if (!(sw_helper::IsFieldMedicine(item) ||
                  (sw_helper::FieldProgressionVerified && sw_helper::IsFieldProgression(item)))) return false;
            d.field_target_popup = true;
            d.field_target_item = item;
            d.field_pp_target = -1;
            d.use_before_taken = false; // FieldPublishUse snapshots the party on the next tick
            return true; // companion-only; choosing the member/move commits the native item
        } else if (action == "field_use") {
            if (!sw_helper::IsRepel(item)) return refuse(sw_helper::Result::NotUsable);
            if (m.reader.repel_steps() != 0) return refuse(sw_helper::Result::RepelActive);
            req = {sw_helper::FieldOp::Repel, static_cast<std::uint32_t>(item), 0, 0, 0, 0};
        } else if (action == "field_use_on" || action == "field_use_on_move") {
            if (!d.field_target_popup || item != d.field_target_item) return false;
            const bool with_move = action == "field_use_on_move";
            if (!(sw_helper::IsFieldMedicine(item) || (sw_helper::FieldProgressionVerified && sw_helper::IsFieldProgression(item))) || argument < 0 || argument > (with_move ? 23 : 5)) return false;
            const int picked_slot = static_cast<int>(with_move ? argument / 4 : argument);
            const int picked_move = static_cast<int>(with_move ? argument % 4 : 0);
            if (with_move && !sw_helper::IsFieldPpSingle(item)) return false;
            // Review M1: the target is the live slot the tile shows; the guest re-checks that the
            // same member (encryption constant) is still in that slot before CanUse.
            const int slot = picked_slot;
            if (!m.reader.live_party_slot_ok(slot)) return false;
            if (sw_helper::IsFieldPpSingle(item)) {
                if (!with_move) {
                    d.field_pp_target = slot;
                    d.field_pp_item = item;
                    d.field_pp_target_ec = m.reader.live_party_ec(slot);
                    return true; // companion-side target selection; the move tap commits the native item
                }
                if (d.field_pp_target != slot || d.field_pp_target_ec != m.reader.live_party_ec(slot))
                    return refuse(sw_helper::Result::TargetChanged);
            }
            req = {sw_helper::IsFieldProgression(item) ? sw_helper::FieldOp::Progression : sw_helper::FieldOp::Medicine, static_cast<std::uint32_t>(item), static_cast<std::uint32_t>(slot),
                   0, 0, m.reader.live_party_ec(slot), static_cast<std::uint32_t>(picked_move)};
            if (d.use_show) return false;  // one use at a time: the last one is still showing
            show = d.use_before[static_cast<std::size_t>(slot)];
            show->slot = slot;
            show->item = item;
            show->move = with_move ? picked_move : -1;
            show->pp_view = sw_helper::IsFieldPpSingle(item) || sw_helper::IsFieldPpAll(item);
            show->staged_at = NowMs();
            show->start = 0;
        } else {
            return false;
        }
    }
    if (st.busy) {
        // Only the background FLY_QUERY may be in flight for a tap to wait on it.
        if (d.field->pending_op() != sw_helper::FieldOp::FlyQuery || d.field_deferred) return false;
        d.field_deferred = req;
        if (show) d.use_show = show; else d.field_target_popup = false;
        if (req.op == sw_helper::FieldOp::Fly) d.map_fly_open = false;
        return true;
    }
    const bool staged = d.field->Stage(req.op, req.arg, req.expect, NowMs(), req.zone, req.landing, req.ec, req.move);
    if (staged && show) d.use_show = show;
    else if (staged) d.field_target_popup = false;
    if (staged && req.op == sw_helper::FieldOp::Fly) d.map_fly_open = false;
    return staged;
}

// Stage a direct battle command. Returns false (-> the package's `refused` haptic) whenever a
// precondition fails at tap time; a later guest refusal is reported through sw.direct.status.
// Bottom -> top: while the native move list waits for input (top in FIGHT), a move row moves the
// native cursor onto that move -- nothing is committed, the top's own A press still decides.
bool NativeMoveCursor(Module& m, int row) {
    auto& d = m.direct;
    const auto& nc = m.reader.native_cursor();
    if (!d.cursor || !d.bridge || nc.menu != sw_battle_cursor::Menu::Fight || row < 0 || row > 3 ||
        !m.reader.battle_view().move_id[static_cast<std::size_t>(row)])
        return false;
    if (nc.move == row) return true; // already there
    return d.cursor->Stage(row, nc.move_list, nc.move_view, d.bridge->status().alive_client, NowMs());
}

bool DirectAction(Module& m, std::string_view action, std::int64_t argument) {
    std::lock_guard lock{m.direct.mutex};
    auto& d = m.direct;
    if (!d.bridge) return false;
    const auto& st = d.bridge->status();
    if (m.reader.native_cursor().menu == sw_battle_cursor::Menu::Fight &&
        (action == "battle_move" || action == "battle_move_inspect")) {
        const int row = static_cast<int>(argument) - (action == "battle_move" ? 1 : 0);
        const bool moved = NativeMoveCursor(m, row);
        if (action == "battle_move") return moved;
        // Hold-to-inspect keeps its companion behaviour; the native cursor follows when it can.
    }
    if (action == "battle_native_cmd") {
        // Doubles / institution / raid: move the NATIVE command cursor onto a visible cell; the
        // highlighted capsule's own button is the A press (manifest), Back is B.
        const auto& nc = m.reader.native_cursor();
        const int cell = static_cast<int>(argument);
        if (!d.cursor || nc.menu != sw_battle_cursor::Menu::Command || !nc.command_list || cell < 0 || cell > 5 ||
            m.reader.command_cells() < 0 || !((m.reader.command_cells() >> cell) & 1) ||
            !(NativeDrive(st) || st.offline_raid))
            return false;
        if (nc.command == sw_battle_cursor::CommandFromNative(static_cast<std::uint32_t>(cell))) return true;
        // The sampled client, not alive_client: in a raid four clients beat the alive hook (the
        // NPC allies too), live 2026-11-01; the guest acts only on the requested client's frame.
        return d.cursor->StageIndex(sw_helper::CursorList::Command, static_cast<std::uint32_t>(cell), nc.command_list,
                                    nc.command_view, m.reader.native_client(), NowMs());
    }
    if (action == "battle_target") {
        // Target select (doubles): move the NATIVE target cursor onto that card (A commits, from the
        // highlighted card's own button; the game refuses a card the move cannot target).
        const auto& nc = m.reader.native_cursor();
        const int grid = sw_battle_cursor::TargetGridFromSlot(static_cast<int>(argument), nc.grid_columns, nc.grid_rows);
        if (!d.cursor || nc.menu != sw_battle_cursor::Menu::Target || !nc.target_list || grid < 0) return false;
        if (nc.target == static_cast<int>(argument)) return true;
        return d.cursor->StageIndex(sw_helper::CursorList::Target, static_cast<std::uint32_t>(grid), nc.target_list,
                                    nc.target_view, m.reader.native_client(), NowMs());
    }
    if (action == "battle_foe_sel") return m.reader.SelectFoe(static_cast<int>(argument));
    if (action == "locked_cmd") {
        // A veiled command capsule (raid Cheer On, Run while unavailable) has no helper path right
        // now: it stays native. Tell the player instead of a silent refusal haptic.
        d.status = sw_str::S(sw_str::Id::kDirectChooseMain);
        d.status_until = NowMs() + 3000;
        return false;
    }
    if (action == "battle_move_inspect") return argument >= 0 && argument < 4 && m.reader.InspectBattleMove(static_cast<int>(argument));
    if (action == "battle_move_detail_tab") return argument >= 0 && argument < 4 && m.reader.SetBattleMoveDetailTab(static_cast<int>(argument));
    if (action == "battle_ball_back" || (action == "battle_view" && argument == 0)) {
        m.reader.SetBattleScreen(0);
        return true;
    }
    if (action == "battle_ball_open" || (action == "battle_view" && argument == 2)) {
        if (!m.reader.native_battle_command_waiting() || !m.reader.battle_has_balls() ||
            sw_helper::CheckBall(st, d.view, m.reader.battle_ball_id(),
                m.reader.BattleItemCount(m.reader.battle_ball_id())) != sw_helper::Refusal::None) return false;
        d.dmax_wanted = false;
        m.reader.SetBattleScreen(2);
        return true;
    }
    if (action == "battle_ball_prev" || action == "battle_ball_next") {
        if (!m.reader.native_battle_command_waiting() || st.busy) return false;
        return m.reader.CycleBattleBall(action == "battle_ball_next" ? 1 : -1);
    }
    if (action == "battle_ball_use") {
        const int id = m.reader.battle_ball_id(); // latch native item identity, never a list index
        if (m.reader.battle_screen() != 2 || !m.reader.native_battle_command_waiting() ||
            sw_helper::CheckBall(st, d.view, id, m.reader.BattleItemCount(id)) != sw_helper::Refusal::None) return false;
        const bool staged = d.bridge->Stage(sw_helper::Op::Item, static_cast<std::uint32_t>(id), 0, NowMs());
        if (staged) m.reader.SetBattleScreen(0);
        return staged;
    }
    if (action == "battle_view") {
        if (argument != 1 || !m.reader.native_battle_command_waiting() || st.busy) return false;
        bool ready = false;
        for (int slot = 0; slot < 4; ++slot)
            ready = ready || sw_helper::CheckFight(st, d.view, slot, d.dmax_wanted || d.view.max_active) == sw_helper::Refusal::None;
        if (!ready) return false;
        d.selected_command = 0;
        // Owner dev.23: Fight drives the top like its own A press -- the game opens its move list
        // and the bottom follows through the cursor sync (native.menu == Fight). Raids (and a
        // Dynamax choice staged here) keep the companion-side move page.
        if (!d.dmax_wanted && st.offline_single &&
            d.bridge->Stage(sw_helper::Op::Decide, sw_helper::DecideFight, 0, NowMs()))
            return true;
        m.reader.SetBattleScreen(1);
        return true;
    }
    if (action == "battle_open_bag" || action == "battle_open_pokemon") {
        if (!m.reader.native_battle_command_waiting() ||
            sw_helper::CheckOpenMenu(st, d.view) != sw_helper::Refusal::None) return false;
        d.dmax_wanted = false;
        m.reader.SetBattleScreen(0);
        const bool bag = action == "battle_open_bag";
        // Bag = the native A press (Decide 3: the game's own Bag dispatch, full bag); the direct
        // Bag-state entry (OpenBag) skipped the command UI's decide step. Pokemon keeps OpenPokemon.
        const bool staged = bag ? d.bridge->Stage(sw_helper::Op::Decide, sw_helper::DecideBag, 0, NowMs())
                                : d.bridge->Stage(sw_helper::Op::OpenPokemon, 0, 0, NowMs());
        if (staged) d.selected_command = bag ? 2 : 1;
        return staged;
    }
    if (action == "battle_dynamax") {
        const auto& nc = m.reader.native_cursor();
        if (nc.menu == sw_battle_cursor::Menu::Fight) {
            // The native move list waits (top in FIGHT): drive the top's own Dynamax button like
            // a move row -- the first tap moves the native cursor onto it, a tap while it is
            // highlighted is the A press (manifest battle_native_a on sw.battle.native.dmax_cell).
            // The bottom then mirrors the top's selection (NativeMaxSelected).
            const int cell = sw_dmax_native::DynamaxCell(nc);
            if (!d.cursor || cell < 0 ||
                !sw_dmax_native::NativeDmaxOffered(nc, st, d.last_preview, d.view, m.reader.battle_own_actor(),
                                                   m.reader.battle_client()))
                return false;
            if (nc.dmax_cell) return true;
            return d.cursor->StageIndex(sw_helper::CursorList::Move, static_cast<std::uint32_t>(cell),
                                        nc.move_list, nc.move_view, m.reader.battle_client(), NowMs());
        }
        if (d.view.max_active) return false;
        if (d.dmax_wanted) { d.dmax_wanted = false; return true; }
        if (!sw_helper::DynamaxDirectVerified || !st.preview.valid || !st.preview.eligible ||
            !st.helper_present || !d.view.in_battle || !d.view.own_ok || !st.menu_waiting || st.busy ||
            !(sw_helper::RaidDirectVerified && st.offline_raid)) return false;
        d.dmax_wanted = true;
        return true;
    }
    if (st.helper_present && d.view.in_battle && st.menu_waiting &&
        !st.offline_single && !(sw_helper::RaidDirectVerified && st.offline_raid && action == "battle_move")) {
        // Review H2/M4: doubles, raids, Dynamax Adventures and link battles stay native.
        d.status = sw_helper::RefusalText(sw_helper::Refusal::BattleType);
        d.status_until = NowMs() + 3000;
        return false;
    }
    if (action == "battle_move") {
        const int slot = static_cast<int>(argument) - 1; // payload is the 1-based row
        const bool max = d.dmax_wanted || d.view.max_active;
        if (sw_helper::CheckFight(st, d.view, slot, max) != sw_helper::Refusal::None) return false;
        const bool staged = d.bridge->Stage(max ? sw_helper::Op::MaxFight : sw_helper::Op::Fight, static_cast<std::uint32_t>(slot),
                                           d.view.move_id[static_cast<std::size_t>(slot)], NowMs());
        if (staged) m.reader.SetBattleScreen(0);
        return staged;
    }
    if (action == "battle_run") {
        if (sw_helper::CheckRun(st, d.view) != sw_helper::Refusal::None) return false;
        const bool staged = d.bridge->Stage(sw_helper::Op::Run, 0, 0, NowMs());
        if (staged) d.selected_command = 3;
        return staged;
    }
    return false;
}

// Party reorder (Pokemon page drag). The guest helper's field channel calls the field party's
// own position exchange (PokeParty vtable +0x40, main+0x7844F0: swaps the two slot pointers,
// packs, keeps the +0x89 index on its member) under the party lock, after re-checking both
// slots' encryption constants -- the save copy is written from this live array when the player
// saves. RE 2026-10-31: main+0x83c230 / main+0x83c870 are the in-battle roster's wrappers (battle
// position map, BattlePokemon flags) and are never used in the field. Field only: the field hook
// live, the player free (no menu, script, warp), the plain field phase, no other helper request
// in flight, the Pokemon page on screen.
constexpr bool kPartySwapVerified = true;
void SampleSwap(Module& m, const EdenDsmodHostApi& host) {
    std::lock_guard lock{m.direct.mutex};
    auto& d = m.direct;
    const auto st = d.field ? d.field->status() : sw_helper::FieldStatus{};
    const auto arr = m.reader.live_party_array();
    const std::string_view page = CurrentPage(host);
    const bool query_only = st.busy && d.field && d.field->pending_op() == sw_helper::FieldOp::FlyQuery;
    d.swap_ready = kPartySwapVerified && d.field && st.helper_present && st.live && st.player_free &&
                   (!st.busy || query_only) && !d.field_deferred && !(d.portrait && d.portrait->busy()) &&
                   !m.reader.in_battle_phase() && !m.reader.in_title_or_loading() &&
                   arr.ok && arr.count >= 2 && (page.empty() || page == "pokemon");
    if (host.publish_i64) {
        host.publish_i64(host.userdata, "sw.party.swap.ready", d.swap_ready ? 1 : 0);
        host.publish_i64(host.userdata, "sw.party.swap.committed", d.swap_committed);
        // The two swapped rows hide for ~150 ms and slide back in (manifest anim on !flashN).
        const bool flash = NowMs() < d.swap_flash_until;
        char key[32];
        for (int i = 0; i < 6; ++i) {
            std::snprintf(key, sizeof key, "sw.party.swap.flash%d", i);
            host.publish_i64(host.userdata, key, flash && (i == d.swap_from || i == d.swap_to) ? 1 : 0);
        }
    }
}

// The @settings language row (gen_manifest.py settings_page): the runtime's choice rows cycle
// through a fixed list, so the package draws its own row, which sets "lang" to "sw.lang.next" --
// the next offered value, so Korean / Chinese are never a choice on a host without their shared
// system font (runtime 22, no real firmware fonts). "sw.lang.effective" is the language in effect
// (a persisted Korean on such a host reads English, and the row shows a note).
void PublishLanguageChoice(Module& m, const EdenDsmodHostApi& host) {
    if (!m.sysfont_probed) {
        m.sysfont_probed = true;
        const std::uint32_t mask = sw_modfont::ProbeSysfonts(host);
        if (host.log) {
            char line[160];
            const auto on = [mask](sw_lang::Lang l) { return (mask >> static_cast<int>(l) & 1u) ? "offered" : "not offered"; };
            std::snprintf(line, sizeof line, SW_VARIANT_NAME " DSMod: system fonts (sysfont source %s): ko %s, zh-Hans %s, zh-Hant %s",
                          (host.capabilities & sw_modfont::kCapSourceSysfont) ? "present" : "absent",
                          on(sw_lang::Lang::Korean), on(sw_lang::Lang::ChineseSimplified), on(sw_lang::Lang::ChineseTraditional));
            host.log(host.userdata, EDEN_DSMOD_LOG_INFO, line);
        }
    }
    if (!host.publish_i64) return;
    const std::int64_t value = host.get_i64 ? host.get_i64(host.userdata, "@flag:lang", 0) : 0;
    const int effective = sw_lang::Index(sw_lang::FromSetting(value));
    host.publish_i64(host.userdata, "sw.lang.next", sw_lang::NextOffered(value));
    host.publish_i64(host.userdata, "sw.lang.effective", effective);
    // the manifest's labels follow d.lang = the flag unless this says it is not in effect
    host.publish_i64(host.userdata, "sw.lang.unavailable", value != effective ? 1 : 0);
    host.publish_i64(host.userdata, "sw.lang.offer", sw_lang::SysfontOfferRef().load());
}

void SampleCallback(void* p, const EdenDsmodHostApi* host) {
    try {
        if (!p || !host) return;
        auto* m = static_cast<Module*>(p);
        if (host->publish_text)
            host->publish_text(host->userdata, "sw.build_label", m->build_label ? m->build_label : "");
        // The wrong-patch screen names the detected build only when there is one to name (a
        // rejected module publishes nothing: the line stays hidden, not an empty "-").
        if (host->publish_i64)
            host->publish_i64(host->userdata, "sw.build_label_ready", m->build_label && *m->build_label ? 1 : 0);
        // Published every sample, success or not -- the map page's "Update Required" overlay
        // (hide_bind:"build_match", hide_eq:1) depends on this landing as 1 on every tick of the
        // supported build, not just once at create() (LIVE-PACKAGE-TEST.md section 2's bug).
        if (host->publish_i64)
            host->publish_i64(host->userdata, "build_match", m->build_match ? 1 : 0);
        PublishLanguageChoice(*m, *host);
        if (m->build_label) {
            // A known-but-unsupported build: no reads, the manifest's wrong-patch page (gated on
            // the host-published "build_match") shows sw.build_label to name it.
            return;
        }
        {
            // Bridge upkeep first, so the reader's battle classification uses this tick's beats.
            std::lock_guard lock{m->direct.mutex};
            if (m->direct.field) {
                m->direct.field->Update(NowMs());
                m->reader.SetFieldFrames(m->direct.field->status().frames);
            }
            if (m->direct.bridge) {
                m->direct.bridge->Update(NowMs());
                const auto& st = m->direct.bridge->status();
                m->reader.SetBattleHint(st.battle_alive || st.menu_waiting, st.helper_present, st.alive_client);
                auto& d = m->direct;
                const std::uint64_t client = m->reader.battle_client(); // held through a stall
                if (!client || d.preview_serial != st.battle_serial ||
                    (d.last_preview.valid && d.last_preview.client != client)) {
                    d.dmax_wanted = false;
                    d.last_preview = {};
                }
                d.preview_serial = st.battle_serial;
                if (st.preview.valid) d.last_preview = st.preview;
                const auto& pv = d.last_preview;
                const bool wanted = sw_dmax_native::DisplayMax(d.dmax_wanted, st.preview.valid && st.preview.active,
                                                               m->reader.battle_dmax_active(),
                                                               m->reader.native_cursor());
                const bool show = wanted && pv.valid && pv.client == client;
                if (show) {
                    m->reader.SetDmaxPreview(true, pv.ids, pv.powers, pv.gmax, pv.base_ids, pv.mon);
                } else {
                    m->reader.SetDmaxPreview(false);
                }
                if (host->publish_i64) { // diagnostics: Max moves wanted / shown on the bottom
                    host->publish_i64(host->userdata, "sw.battle.dmax.wanted", wanted ? 1 : 0);
                    host->publish_i64(host->userdata, "sw.battle.dmax.preview", show ? 1 : 0);
                }
            } else {
                m->reader.SetBattleHint(false, false, 0);
                m->reader.SetDmaxPreview(false);
            }
            // Native Fly unlocks prove that the save has revealed a DLC region. Merely owning a
            // traded DLC species does not. Destination kinds/order: sw_fly.h (native Town Map).
            const auto mask = m->direct.fly_known ? m->direct.fly_mask : 0;
            m->reader.SetDexRegionAccess((mask & (0x1Full << 30)) != 0,
                                        (mask & (0x3FFull << 35)) != 0);
        }
        // Companion language (@settings "lang", sw_lang.h). Fallback 0 = English: a never-changed
        // setting is the manifest default, and an older host answers the fallback too. Korean /
        // Chinese without their shared system font read as English (PublishLanguageChoice).
        // The game's own words the companion shows (F13) are read from the player's romfs once per
        // language, before the language is put in effect; a shortfall is logged once.
        const auto lang = sw_lang::FromSetting(host->get_i64 ? host->get_i64(host->userdata, "@flag:lang", 0) : 0);
        if (const int missing = sw_str::LoadGame(*host, lang); missing > 0 && host->log) {
            char line[160];
            std::snprintf(line, sizeof line, SW_VARIANT_NAME " DSMod: %d game text line(s) unreadable in %s, companion placeholder shown",
                          missing, std::string(sw_lang::Code(lang)).c_str());
            host->log(host->userdata, EDEN_DSMOD_LOG_WARNING, line);
        }
        m->reader.SetLanguage(lang);
        // The font follows the language (Japanese: the module's Rodin atlas; Korean / Chinese: the
        // shared system fonts; a Latin language after those: the module's copy of the BFFNT). A
        // finished build changes what the module's own measuring sees (sw_font override), so
        // everything is published again.
        m->font.Update(*host, sw_lang::Current(), static_cast<std::uint64_t>(NowMs()));
        if (m->font.TakeChanged()) m->reader.Republish();
        if (host->publish_i64) host->publish_i64(host->userdata, "__font_epoch", m->font.epoch());
        if (m->font.epoch() != m->font_logged_epoch && host->log) {
            const auto st = m->font.stats();
            if (st.module_font) {
                m->font_logged_epoch = m->font.epoch();
                char line[200];
                std::snprintf(line, sizeof line,
                              SW_VARIANT_NAME " DSMod: font epoch %lld %s: %u glyphs (%u missing), cap %u px, %u page(s) %ux%u, %llu KiB, built in %llu ms",
                              static_cast<long long>(m->font.epoch()), sw_modfont::KindCode(st.kind),
                              st.glyphs, st.missing, st.cap_px, st.pages, st.width, st.height,
                              static_cast<unsigned long long>(st.atlas_bytes >> 10), static_cast<unsigned long long>(st.build_ms));
                host->log(host->userdata, EDEN_DSMOD_LOG_INFO, line);
            }
        }
        m->reader.SetTextNote([](void* ctx, std::string_view text) { static_cast<sw_modfont::Font*>(ctx)->Note(text); },
                              &m->font);
        {
            // Configure can reset the session from the loader thread. Keep the encounter
            // cache and its readers under the same lock as ResetSession and route actions.
            std::lock_guard lock{m->direct.mutex};
            m->reader.Sample(*host);
        }
        {
            std::lock_guard lock{m->direct.mutex};
            m->portrait.SetAutoHint(m->direct.mailbox_address,
                                    m->direct.portrait ? m->direct.portrait->sequence() : 0);
        }
        m->portrait.Publish(*host);
        DirectUpkeep(*m, *host);
        FieldUpkeep(*m, *host);
        SampleSwap(*m, *host);
    } catch (...) {
        auto* m = static_cast<Module*>(p);
        if (m && !m->sample_failure_reported && host && host->log) {
            m->sample_failure_reported = true;
            host->log(host->userdata, EDEN_DSMOD_LOG_ERROR, SW_VARIANT_NAME " DSMod sample failed");
        }
    }
}

// Every host tick, the second screen shown or hidden (on_action makes the host tick a hidden
// module): the battle HP plates are hidden on the top screen only while the companion is on
// screen and "Battle HP on top" is off; hiding the companion (4-finger hide) or turning the
// setting on gives them back at once (the guest restores on its next frame).
void TickCallback(void* p, const EdenDsmodHostApi* host) {
    try {
        if (!p || !host) return;
        auto* m = static_cast<Module*>(p);
        if (m->build_label) return;
        const bool shown = !host->get_i64 || host->get_i64(host->userdata, "__aux_present", 1) != 0;
        // "Battle HP on top": get_i64("@flag:...") reads the live settings table on runtime 22
        // hosts from Eden Duo 1.3.2 (runtime commit "a module reads companion settings"); an
        // older host answers the fallback; default remains visible on top.
        const std::int64_t raw_flag = host->get_i64 ? host->get_i64(host->userdata, "@flag:battle.hp_top", 1) : 1;
        const std::int64_t flag = raw_flag < 0 ? 1 : raw_flag;
        // Review L3: the plates are hidden only while the battle page itself is on screen -- a
        // player who navigates away mid-battle sees HP on top again (a host that cannot name the
        // page keeps the old rule).
        const std::string_view page = CurrentPage(*host);
        const bool battle_page = page.empty() || page == "battle" || page == "battle_moves" || page == "battle_balls";
        const bool raid_page = page == "raid" || page == "raid_moves";
        std::lock_guard lock{m->direct.mutex};
        auto& d = m->direct;
        if (!shown || !(battle_page || raid_page)) m->reader.SetBattleScreen(0);
        if (d.field) d.field->Update(NowMs()); // hidden ticks must still cancel safely
        if (d.portrait) {
            const auto fs = d.field ? d.field->status() : sw_helper::FieldStatus{};
            const bool want = shown && page == "league_card" && !m->reader.in_title_or_loading() &&
                              !m->reader.in_battle_phase();
            const bool cached = host->get_i64 && host->get_i64(host->userdata, "sw.card.image.ready", 0) != 0;
            d.portrait->Tick(want,
                !cached && fs.helper_present && fs.live && fs.player_free && !fs.busy && !d.field_deferred,
                m->portrait.AutoCapturedSeq(), NowMs());
            if (host->publish_i64) {
                host->publish_i64(host->userdata, "sw.card.image.phase", d.portrait->phase());
                host->publish_i64(host->userdata, "sw.card.image.sequence", d.portrait->sequence());
            }
        }
        m->direct.hp_flag = flag;
        const auto st = m->direct.bridge ? m->direct.bridge->status() : sw_helper::Status{};
        const bool single = battle_page && st.offline_single && m->direct.view.own_ok;
        const bool raid = raid_page && st.battle_kind == 2 && m->reader.battle_raid_ready();
        if (m->direct.hud)
            m->direct.hud->Tick(sw_helper::HudWantBattle(flag, shown && (battle_page || raid_page),
                                                       st.kind_current, single, raid), NowMs(), raid);
        if (m->direct.menu) {
            // Fallback 0 = the manifest default (hidden): the runtime-22 host answers "@flag:" only
            // for flags it restored from the persist file, so a never-toggled setting reads as the
            // fallback (live 2026-11-01: con showed 0, the module got the fallback). The package
            // needs runtime 22 (min_runtime), where a toggled value is always readable.
            const std::int64_t raw_menu = host->get_i64 ? host->get_i64(host->userdata, "@flag:battle.menu_top", 0) : 1;
            m->direct.menu_flag = raw_menu < 0 ? 1 : raw_menu;
            const auto& layouts = m->reader.menu_layouts();
            const bool drivable = st.helper_present && m->reader.in_battle_phase() &&
                (st.offline_single || (raid_page && st.offline_raid) || NativeDrive(st));
            const std::uint32_t want = m->direct.menu_want.Step(m->direct.menu_flag, shown && (battle_page || raid_page),
                st.kind_current, drivable, m->reader.catch_prompt(),
                !st.offline_raid && m->reader.target_cursor_ok());
            m->direct.menu->Tick(want, layouts.cursor);
        }
    } catch (...) {
    }
}

void ConfigureCallback(void* p, const EdenDsmodHostExtensions* ext) {
    // The host provides a mailbox only when the package's load plan matched this build and its
    // helper code was installed; anything else leaves the battle commands locked.
    try {
        if (!p) return;
        auto& m = *static_cast<Module*>(p);
        std::lock_guard lock{m.direct.mutex};
        if (m.direct.portrait) m.direct.portrait->Shutdown();
        m.images.Reset();
        m.reader.ResetSession(); // review L6: a new session forgets the last one's game state
        m.portrait.Reset();
        m.direct.fly_known = false;
        m.direct.fly_mask = 0;
        m.direct.map_fly_selection = -1;
        m.direct.map_fly_open = false;
        m.direct.bike_known = false;
        m.direct.bike_usable = false;
        m.direct.field_pp_target = -1;
        m.direct.field_pp_item = 0;
        m.direct.field_target_popup = false;
        m.direct.field_target_item = 0;
        m.direct.field_pp_target_ec = 0;
        m.direct.dmax_wanted = false;
        m.direct.last_preview = {};
        m.direct.preview_serial = 0;
        m.direct.bridge.reset();
        m.direct.cursor.reset();
        m.direct.hud.reset();
        m.direct.menu.reset();
        m.direct.menu_want.Reset();
        m.direct.field.reset();
        m.direct.portrait.reset();
        m.direct.mailbox_address = 0;
        if (ext && ext->version == EDEN_DSMOD_EXT_VERSION && ext->struct_size == sizeof(*ext) &&
            ext->abi_hash == EDEN_DSMOD_EXT_HASH && ext->mailbox_address &&
            ext->mailbox_size >= sw_helper::MailboxSize && ext->load_u32 && ext->store_u32) {
            const HelperIo io{ext->userdata, ext->load_u32, ext->store_u32};
            m.direct.bridge.emplace(HelperLoad32{io}, HelperStore32{io});
            if (!m.direct.bridge->Initialize()) m.direct.bridge.reset();
            m.direct.cursor.emplace(HelperLoad32{io}, HelperStore32{io});
            if (!m.direct.cursor->Initialize()) m.direct.cursor.reset();
            m.direct.hud.emplace(HelperLoad32{io}, HelperStore32{io});
            if (!m.direct.hud->Initialize()) m.direct.hud.reset();
            m.direct.menu.emplace(HelperLoad32{io}, HelperStore32{io});
            if (!m.direct.menu->Initialize()) m.direct.menu.reset();
            m.direct.field.emplace(HelperLoad32{io}, HelperStore32{io});
            if (!m.direct.field->Initialize()) m.direct.field.reset();
            if (sw_helper::FieldPortraitAutoVerified) {
                m.direct.portrait.emplace(HelperLoad32{io}, HelperStore32{io});
                if (!m.direct.portrait->Initialize()) m.direct.portrait.reset();
                else m.direct.mailbox_address = ext->mailbox_address;
            }
        }
    } catch (...) {
    }
}

void ConfigureWriteCallback(void* p, const EdenDsmodHostWriteApi* api) {
    try {
        if (!p) return;
        auto& m = *static_cast<Module*>(p);
        std::lock_guard lock{m.direct.mutex};
        m.direct.write_api = {};
        if (api && api->version == EDEN_DSMOD_WRITE_EXT_VERSION && api->abi_hash == EDEN_DSMOD_WRITE_EXT_HASH &&
            api->struct_size >= sizeof(*api) && api->write_batch)
            m.direct.write_api = *api;
    } catch (...) {
    }
}

// Drop of a Pokemon page row: argument = the dragged slot, the action's digit = the target row.
// Staged on the field channel (deferred behind a Fly/Bike query), refused when not ready.
bool PartySwapAction(Module& m, std::string_view action, std::int64_t argument) {
    std::lock_guard lock{m.direct.mutex};
    auto& d = m.direct;
    const int to = action.back() - '0';
    const int from = static_cast<int>(argument);
    if (!d.swap_ready || !d.field || from < 0 || from > 5 || to < 0 || to > 5 || from == to) return false;
    const auto arr = m.reader.live_party_array();
    if (!arr.ok || from >= arr.count || to >= arr.count) return false;
    const std::uint32_t ec_from = m.reader.live_party_ec(from), ec_to = m.reader.live_party_ec(to);
    if (!ec_from || !ec_to) return false;
    d.swap_from = from;
    d.swap_to = to;
    const Direct::Deferred req{sw_helper::FieldOp::PartySwap, static_cast<std::uint32_t>(from),
                               static_cast<std::uint32_t>(to), 0, 0, ec_from, ec_to};
    if (d.field->status().busy) {
        if (d.field->pending_op() != sw_helper::FieldOp::FlyQuery || d.field_deferred) return false;
        d.field_deferred = req;
        return true;
    }
    return d.field->Stage(req.op, req.arg, req.expect, NowMs(), 0, 0, req.ec, req.move);
}

EdenDsmodBool ActionCallback(void* instance, const char* action, std::int64_t argument) {
    // Direct battle commands (battle_move <1..4>, battle_run) go through the guest helper
    // (DirectAction above). Everything else is companion-side UI selection (bag_pocket/bag_row/
    // dex_region/dex_row), handled by sw_reader::Reader::DispatchAction with no guest memory
    // touched. Field actions (fly_go, field_use, field_use_on) go through the guest's field hook
    // (FieldAction above).
    try {
        if (!instance || !action) return EDEN_DSMOD_FALSE;
        auto* m = static_cast<Module*>(instance);
        const std::string_view a{action};
        if (a == "battle_move" || a == "battle_run" ||
            a == "battle_view" || a == "battle_ball_open" || a == "battle_ball_prev" || a == "battle_ball_next" ||
            a == "battle_ball_back" || a == "battle_ball_use" || a == "battle_move_inspect" || a == "battle_move_detail_tab" ||
            a == "battle_open_bag" || a == "battle_open_pokemon" ||
            a == "battle_dynamax" || a == "locked_cmd" || a == "battle_native_cmd" || a == "battle_target" ||
            a == "battle_foe_sel")
            return DirectAction(*m, a, argument) ? EDEN_DSMOD_TRUE : EDEN_DSMOD_FALSE;
        if (a.size() == 11 && a.starts_with("party_swap") && a.back() >= '0' && a.back() <= '5')
            return PartySwapAction(*m, a, argument) ? EDEN_DSMOD_TRUE : EDEN_DSMOD_FALSE;
        if (a == "fly_go" || a == "map_fly" || a == "map_fly_close" || a == "field_use" || a == "field_use_open" || a == "field_use_on" || a == "field_use_on_move" || a == "field_use_cancel" || a == "field_bike")
            return FieldAction(*m, a, argument) ? EDEN_DSMOD_TRUE : EDEN_DSMOD_FALSE;
        std::lock_guard lock{m->direct.mutex};
        return m->reader.DispatchAction(action, argument) ? EDEN_DSMOD_TRUE : EDEN_DSMOD_FALSE;
    } catch (...) {
        return EDEN_DSMOD_FALSE;
    }
}

EdenDsmodBool LoadImageCallback(void* instance, const EdenDsmodHostApi* host,
                               const char* key, void* receiver, EdenDsmodImageSink sink) {
    try {
        if (!instance || !key || !sink) return EDEN_DSMOD_FALSE;
        auto& m = *static_cast<Module*>(instance);
        if (m.build_label) return EDEN_DSMOD_FALSE;
        if (host) {
            bool handled = false;
            const bool image_ok = m.images.Load(*host, key, receiver, sink, handled);
            if (handled) return image_ok ? EDEN_DSMOD_TRUE : EDEN_DSMOD_FALSE;
            const bool ok = m.font.LoadImage(*host, key, receiver, sink, handled);
            if (handled) return ok ? EDEN_DSMOD_TRUE : EDEN_DSMOD_FALSE;
        }
        return m.portrait.LoadImage(key, receiver, sink) ? EDEN_DSMOD_TRUE : EDEN_DSMOD_FALSE;
    } catch (...) {
        return EDEN_DSMOD_FALSE;
    }
}

const EdenDsmodModuleApi ModuleApi{EDEN_DSMOD_MODULE_ABI_VERSION,
                                   sizeof(EdenDsmodModuleApi),
                                   0,
                                   EDEN_DSMOD_MODULE_ABI_HASH,
                                   sw_build::kTitleId,
                                   sw_variant::kActive.module_name,
                                   EDEN_DSMOD_CAP_EXTENSIONS | EDEN_DSMOD_CAP_ROMFS_READ |
                                       EDEN_DSMOD_CAP_WRITE_MEMORY,
                                   &SupportsBuild,
                                   &Create,
                                   &Destroy,
                                   &SampleCallback,
                                   &TickCallback};

const EdenDsmodModuleExtensions ModuleExtensions{
    EDEN_DSMOD_EXT_VERSION, sizeof(EdenDsmodModuleExtensions), EDEN_DSMOD_EXT_HASH,
    &ConfigureCallback,     &ActionCallback,                   &LoadImageCallback};

// decode_font: only ever asked again after "__font_epoch" moved (the boot font is the host's own
// BFFNT decode); the bytes are that BFFNT and are not needed, the module serves its newest build.
EdenDsmodBool DecodeFontCallback(void* instance, const std::uint8_t* /*bytes*/, std::size_t /*size*/,
                                 void* receiver, EdenDsmodFontSink sink) {
    try {
        if (!instance || !sink) return EDEN_DSMOD_FALSE;
        auto& m = *static_cast<Module*>(instance);
        if (m.build_label) return EDEN_DSMOD_FALSE;
        return m.font.Decode(receiver, sink) ? EDEN_DSMOD_TRUE : EDEN_DSMOD_FALSE;
    } catch (...) {
        return EDEN_DSMOD_FALSE;
    }
}

const EdenDsmodFontExtensions FontExtensions{EDEN_DSMOD_FONT_EXT_VERSION, sizeof(EdenDsmodFontExtensions),
                                             EDEN_DSMOD_FONT_EXT_HASH, &DecodeFontCallback};

const EdenDsmodModuleWriteExtensions WriteExtensions{EDEN_DSMOD_WRITE_EXT_VERSION,
                                                     sizeof(EdenDsmodModuleWriteExtensions),
                                                     EDEN_DSMOD_WRITE_EXT_HASH, &ConfigureWriteCallback};

} // namespace

DSMOD_SDK_EXPORT_MODULE(ModuleApi)
DSMOD_SDK_EXPORT_EXTENSIONS(ModuleExtensions)
DSMOD_SDK_EXPORT_WRITE_EXTENSIONS(WriteExtensions)
DSMOD_SDK_EXPORT_FONT_EXTENSIONS(FontExtensions)
