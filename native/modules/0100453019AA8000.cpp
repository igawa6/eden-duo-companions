// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/mods/dsmod_module_extensions.h"
#include "core/mods/modules/dsmod_module_sdk.h"
#include "xcx_helper_mailbox.h"
#include "xcx_hud.h"
#include "xcx_lifecycle.h"
#include "xcx_map.h"
#include "xcx_mapbuilder.h"
#include "xcx_maptiles.h"
#include "xcx_motion.h"
#include "xcx_reader.h"
#include "xcx_still_cache.h"
#include "xcx_view.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <optional>
#include <sstream>
#include <vector>
namespace {
// Test-only build switch (-DXCX_MAP_SYNC_PICTURES): show world pictures at once and build them on the host's asset
// worker, as before the map builder; the preview's burst test must fail with it (it reproduces the stale chrome).
#ifdef XCX_MAP_SYNC_PICTURES
constexpr bool SyncMapPictures = true;
#else
constexpr bool SyncMapPictures = false;
#endif
// Test-only environment switches (XCX_BATTLE_FLAG, XCX_STILL*, XCX_HUD_ASSUME_AUX): read only in the test build
// tree (CMake BUILD_TESTING, for the offline preview); the package build never reads the environment.
const char *TestEnv(const char *name) {
#ifdef XCX_TEST_SWITCHES
    return std::getenv(name);
#else
    (void)name;
    return nullptr;
#endif
}
using Clock = std::chrono::steady_clock;
std::int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch())
        .count();
}
// The guest helper's mailbox (xcx_guest_helper.S) through the host extension's acquire/release
// word accessors. Present only when the package's load plan matched this build and its helper code
// was installed; anything else leaves the Skell button disabled exactly as the prototype shows it.
struct HelperIo {
    void *userdata{};
    EdenDsmodBool (*load_u32)(void *, std::uint32_t, std::uint32_t *){};
    EdenDsmodBool (*store_u32)(void *, std::uint32_t, std::uint32_t){};
};
struct HelperLoad32 {
    HelperIo io;
    bool operator()(std::uint32_t o, std::uint32_t *v) const {
        return io.load_u32(io.userdata, o, v) == EDEN_DSMOD_TRUE;
    }
};
struct HelperStore32 {
    HelperIo io;
    bool operator()(std::uint32_t o, std::uint32_t v) const {
        return io.store_u32(io.userdata, o, v) == EDEN_DSMOD_TRUE;
    }
};
using HelperBridge = xcx_helper::Bridge<HelperLoad32, HelperStore32>;
bool ValidHost(const EdenDsmodHostApi *h) {
    return dsmod_sdk::HostAbiMatches(h) && h->title_id == xcx::TitleId &&
           xcx::SupportsBuild(h->build_id) && h->read_memory && h->is_mapped && h->read_romfs;
}
struct Frame {
    std::string key;
    xcx::Snapshot data;
    xcx::View view;
    std::uint64_t epoch;
    xcx::StillPictures stills; // the pictures that were ready when this page was queued
};
struct Module {
    xcx::Snapshot data;
    xcx::View view;
    // SDK dsmod_module_extensions.h: load_image runs on one asset worker; the host joins
    // that worker before destroy. Renderer/Assets/Fonts are used only on that worker.
    xcx::Renderer renderer;
    xcx::MapBuilder map_builder; // the world pictures, on its own thread
    std::mutex mutex;
    std::deque<Frame> frames;
    std::uint64_t epoch{1};
    Clock::time_point next_read{}, next_frame{};
    std::string key;
    // The last page picture the asset worker rendered (under `mutex`) and when `key` was queued.
    std::string served_key;
    // The Party page's Gear tab list (Renderer::GearList), a picture of its own that the host scrolls.
    std::string gear_key;
    xcx::Member gear_member;
    xcx::View gear_view;
    Clock::time_point key_time{};
    static constexpr auto FrameWait = std::chrono::milliseconds(400);
    bool force{true};
    // Guest helper (Board / Exit). Configure may come from the loader thread.
    std::mutex helper_mutex;
    std::optional<HelperBridge> bridge;
    xcx_helper::Controller controller;
    xcx_helper::Button button;
    xcx_helper::SwitchController switcher;
    xcx_helper::SetButton set_button;
    int selected_id{-1}; // the picked member's id: survives the reorder a switch causes
    bool helper_present{};
    // The helper has been fresh at least once since Configure (it beats on the title screen too). Without it
    // the minimap cannot be hidden (its commit needs the helper's field state): Settings says so.
    bool helper_seen{};
    int last_result{-1};
    std::uint32_t diag_heartbeat{}, diag_state{}, diag_answered{}, diag_slot0{}, diag_switches{};
    bool diag_switch_done{};
    std::array<std::uint32_t, xcx_helper::DiagWords> diag_words{};
    bool diag_can_return{}, diag_field_ok{};
    // Battle page: the helper's fresh state says the field takes input (no menu / scene / loading).
    // The page dims only after the state has held for DimDelayMs (boarding blinks input off).
    bool helper_field_ok{}, helper_input_ready{};
    xcx::battle::Dim dim_raw{xcx::battle::Dim::None}, dim_shown{xcx::battle::Dim::None};
    std::int64_t dim_since{};
    static constexpr std::int64_t DimDelayMs = 300;
    // Title / Loading / Live (xcx_lifecycle.h). `helper_object`: a world exists or is loading: the helper's
    // raw state word has StateValid (a controlled object) or StateGate (*(main+0x1feffc0)+0x158 != 0).
    // Live 2026-11-25 (contracts/STATES-1.md): title prompt, title menu and Load Game list 0x12800 (neither
    // bit); save load 0x12808, fast-travel load 0x12888, Return warp 0x1280b (gate set while the party
    // actors are gone). StateValid alone left a fast-travel load on the title view.
    bool helper_object{};
    xcx::StageFilter stage_filter;
    xcx::Stage stage{xcx::Stage::Title};
    // The automatic Battle view (xcx_battle.h AutoSwitch). `auto_page`: the page shown now was set by
    // it (published on xcx.pg_auto: 250 ms fade, no haptic) rather than by a tab tap.
    xcx::battle::AutoSwitch auto_switch;
    bool auto_page{};
    xcx::battle::Flag flag_raw{xcx::battle::Flag::Unknown};
    // The picked member rides its Skell: the Set plate is refused (BATTLE-LIVE-2, BUG 1).
    // Or a battle is on (xcx_battle.h SetBlockedInBattle): same refusal, owner 2026-11-22.
    bool set_blocked{}, set_in_battle{};
    // Map page right rail (owner 2026-11-27): a tap on a member's row switches to it (RailSwitch, the Set
    // plate's helper path and gates). A refused tap shows "Not available now" on that row for RailNoteMs.
    // While a finger pans / pinches one of the map widgets (runtime 25 @view_touch_down:<id>) and for
    // MapTouchTailMs after it lifts, the rows have no tap target, so only a deliberate row tap counts.
    // Every row tap (accepted or refused) also publishes the row on xcx.rail_press for RailPressMs: the
    // manifest's press flash over that row (a 70 ms fade in and out, an instant one under Reduced motion).
    int rail_note_row{-1}, rail_press_row{-1};
    std::int64_t rail_note_until{}, map_touch_until{}, rail_press_until{};
    bool map_gesture{};
    static constexpr std::int64_t RailNoteMs = 1500, MapTouchTailMs = 250, RailPressMs = 80;
    // Skell stills: rendered off the tick on their own thread, cached by frame id. Declared last so
    // it is joined first when the module is destroyed.
    // Motion (xcx_motion.h): HP numbers and gauges roll on a big jump, a landed Skell picture fades
    // in. Both are off while the player's Reduced motion flag is set; neither runs at rest.
    std::array<xcx::Roll, 4> pc_hp_roll, skell_hp_roll;
    std::array<int, 4> roll_member{-1, -1, -1, -1};
    xcx::Fade controlled_fade, selected_fade;
    bool reduced_motion{};
    bool motion_active{};
    int default_zoom{}; // legacy zoom action (the Map is a native pan/zoom widget now)
    // The native map widgets: the mode tab flag, and the area / segment flags the world pictures are
    // drawn from (read by the asset worker under map_mutex).
    int map_mode{};
    xcx::tiles::Pose last_pose{}; // the last valid pose of the controlled member
    std::mutex map_mutex;
    std::uint32_t map_zone{};
    std::vector<std::uint8_t> map_flags;
    std::string map_gen{"0"};
    std::optional<xcx::StillRequest> controlled_request, selected_request;
    std::string want_key;
    Clock::time_point next_want{};
    xcx::StillCache stills;
    // Top-screen HUD (xcx_hud.h): the game's own Display Settings values, hidden while wanted and
    // put back otherwise. `cleanup_host` / `write_api` are copies kept for the teardown restore.
    EdenDsmodHostApi cleanup_host{};
    EdenDsmodHostWriteApi write_api{};
    std::mutex hud_mutex;
    xcx::hud::Controller hud;
    Clock::time_point next_hud{};
    std::int64_t hud_diag{};
    explicit Module(const EdenDsmodHostApi &h) : cleanup_host(h) {}
    xcx::hud::Io HudIo(const EdenDsmodHostApi &h) {
        xcx::hud::Io io;
        if (h.read_memory)
            io.read = [&h](std::uint64_t a, void *out, std::size_t n) {
                return h.read_memory(h.userdata, a, out, n) == EDEN_DSMOD_TRUE;
            };
        if (write_api.write_batch)
            io.batch = [this](const xcx::hud::Io::Op *ops, std::size_t n) {
                std::array<EdenDsmodWriteOp, 8> w{};
                if (n > w.size())
                    return false;
                for (std::size_t i = 0; i < n; ++i)
                    w[i] = {ops[i].address, ops[i].size, 0, ops[i].expect, ops[i].value};
                return write_api.write_batch(write_api.userdata, w.data(), std::uint32_t(n)) == EDEN_DSMOD_TRUE;
            };
        return io;
    }
    void UpdateHud(const EdenDsmodHostApi &h, Clock::time_point now) {
        const auto flag = [&](const char *name) { return h.get_i64 && h.get_i64(h.userdata, name, 1) != 0; };
        view.hud_hide_map = flag("@flag:xcx.hud_hide_map");
        view.hud_hide_fuel = flag("@flag:xcx.hud_hide_fuel");
        if (now < next_hud)
            return;
        next_hud = now + std::chrono::milliseconds(250);
        // Hidden while the companion screen is up and its toggle is on (xcx_hud.h: runtime state only,
        // the player's saved Display Settings stay theirs). Minimap commits only while the field is
        // live (the helper's field state: no menu, loading or scene).
        // XCX_HUD_ASSUME_AUX=1: a desktop run without a virtual second screen counts as shown.
        const char *assume = TestEnv("XCX_HUD_ASSUME_AUX");
        const bool aux = (assume && std::string_view(assume) == "1") || !h.get_i64 ||
                         h.get_i64(h.userdata, "__aux_present", 1) == 1;
        const bool wanted = aux && data.valid;
        const bool live = helper_present && helper_field_ok;
        std::scoped_lock lock(hud_mutex);
        hud.Update(HudIo(h), h.main_base, {wanted && view.hud_hide_map, wanted && view.hud_hide_fuel}, live);
        if (hud.Busy()) // the game applies a commit on its next frame: write back right after it
            next_hud = now + std::chrono::milliseconds(20);
        // No helper on a loaded save (plan refused / not installed): the minimap toggle is "not available";
        // the fuel gauge needs no helper and keeps working.
        view.hud_map_state = data.valid && !helper_seen ? std::uint8_t(xcx::hud::State::Failed)
                                                        : std::uint8_t(hud.StateOf(xcx::hud::Map));
        view.hud_fuel_state = std::uint8_t(hud.StateOf(xcx::hud::Fuel));
        hud_diag = std::int64_t(view.hud_map_state) | std::int64_t(view.hud_fuel_state) << 4 |
                   std::int64_t(hud.Commits() & 0xfff) << 8 | std::int64_t(write_api.write_batch != nullptr) << 20 |
                   std::int64_t(aux) << 21 | std::int64_t(live) << 22 | std::int64_t(hud.Observer() != 0) << 23;
    }
    void SetWriteApi(const EdenDsmodHostWriteApi &api) {
        std::scoped_lock lock(hud_mutex);
        write_api = api;
    }
    ~Module() {
        try {
            std::scoped_lock lock(hud_mutex);
            if (cleanup_host.read_memory && write_api.write_batch)
                hud.Restore(HudIo(cleanup_host), cleanup_host.main_base);
        } catch (...) {
            // teardown never throws
        }
        std::scoped_lock lock(helper_mutex);
        if (bridge)
            bridge->Shutdown();
    }
    bool HasSkell() const {
        bool skell = false;
        for (const auto &m : data.party)
            skell |= m.pc.present && m.skell.present;
        return data.valid && skell;
    }
    // The party's in-combat flag of the last read (xcx_battle.h PartyFlag: any member in battle);
    // XCX_BATTLE_FLAG=in|out overrides it in the offline tests only.
    xcx::battle::Flag ReadFlag() const {
        auto flag = xcx::battle::PartyFlag(data);
        if (const char *f = TestEnv("XCX_BATTLE_FLAG"))
            flag = std::string_view(f) == "in"    ? xcx::battle::Flag::In
                   : std::string_view(f) == "out" ? xcx::battle::Flag::Out
                                                  : flag;
        return flag;
    }
    // Refresh the helper's status, the finished request and the button face.
    void UpdateHelper(std::int64_t now) {
        std::scoped_lock lock(helper_mutex);
        if (!bridge) {
            button = {};
            set_button = {};
            helper_present = false;
            helper_field_ok = helper_input_ready = helper_object = false;
            return;
        }
        bridge->Update(now);
        xcx_helper::Outcome outcome;
        if (bridge->TakeOutcome(outcome)) {
            controller.OnOutcome(outcome, now);
            switcher.OnOutcome(outcome, bridge->status(), now);
            // Live-test diagnostics: result code, +100 when the guest never answered.
            last_result = static_cast<int>(outcome.code) + (outcome.answered ? 0 : 100);
        }
        button = controller.Update(bridge->status(), HasSkell(), now);
        const bool sel_ok = view.selected >= 0 && view.selected < 4 && data.valid &&
                            data.party[view.selected].pc.present;
        const bool sel_controlled = sel_ok && data.party[view.selected].controlled;
        const auto flag = ReadFlag();
        set_in_battle = xcx::battle::SetBlockedInBattle(
            flag == xcx::battle::Flag::Unknown ? auto_switch.Last() : flag, auto_switch.Engaged(),
            !TestEnv("XCX_BATTLE_FLAG") && xcx::battle::PartyInBattle(data));
        const bool target_ok =
            sel_ok && xcx::battle::SwitchTargetAllowed(data.party[view.selected]) && !set_in_battle;
        set_blocked = sel_ok && !sel_controlled && !target_ok;
        set_button = switcher.Update(bridge->status(), target_ok, sel_controlled, now);
        diag_slot0 = bridge->status().slot0_token;
        diag_switches = bridge->status().submit_count;
        diag_switch_done = xcx_helper::SwitchController::Completed(bridge->status());
        diag_words = bridge->status().diag;
        diag_can_return = bridge->status().state.can_return;
        diag_field_ok = bridge->status().state.field_ok;
        helper_present = bridge->status().helper_present && bridge->status().state_fresh;
        helper_seen |= helper_present;
        helper_field_ok = helper_present && bridge->status().state.field_ok;
        helper_input_ready = helper_present && bridge->status().state.input_ready;
        helper_object = helper_present && (bridge->status().state_word & (xcx_helper::StateValid | xcx_helper::StateGate)) != 0;
        diag_heartbeat = bridge->status().heartbeat;
        diag_state = bridge->status().state_word;
        diag_answered = bridge->status().answered;
    }
    void Configure(const EdenDsmodHostExtensions *ext) {
        std::scoped_lock lock(helper_mutex);
        if (bridge)
            bridge->Shutdown();
        bridge.reset();
        helper_seen = false;
        controller = {};
        switcher = {};
        button = {};
        set_button = {};
        if (ext && ext->version == EDEN_DSMOD_EXT_VERSION && ext->struct_size == sizeof(*ext) &&
            ext->abi_hash == EDEN_DSMOD_EXT_HASH && ext->mailbox_address &&
            ext->mailbox_size >= xcx_helper::MailboxSize && ext->load_u32 && ext->store_u32) {
            const HelperIo io{ext->userdata, ext->load_u32, ext->store_u32};
            bridge.emplace(HelperLoad32{io}, HelperStore32{io});
            if (!bridge->Initialize())
                bridge.reset();
        }
    }
    // Ask the background renderer for the pictures the current page can show: the controlled
    // member's equipped frame, and the Party page's selected member's. Throttled; the cache is
    // keyed by frame id, so a frame that comes back is instant.
    void WantStills(const EdenDsmodHostApi &h, Clock::time_point now) {
        // Test/diagnostic switch: XCX_STILL=0 in the environment keeps every page exactly the
        // prototype's (the offline replay uses it for its unchanged-page comparison).
        if (const char *off = TestEnv("XCX_STILL"); off && std::string_view(off) == "0")
            return;
        if (!data.valid)
            return;
        const auto request = [](const xcx::Member &m) -> std::optional<xcx::StillRequest> {
            if (!m.owned || !m.skell.present)
                return std::nullopt;
            xcx::StillRequest r;
            r.frame = m.frame;
            // Default: the record's colour groups paint slots 0-3 (glow slots keep the factory
            // colours); a missing/invalid record, or XCX_STILL_PAINT=factory, means factory paint.
            const char *mode = TestEnv("XCX_STILL_PAINT");
            if (!(mode && std::string_view(mode) == "factory"))
                r.paint = xcx::PlayerPaintFromFloats(m.paint);
            // Equipped weapons, stowed on the field-idle pose (OPEN-SKELL-HUD.md); XCX_STILL_WEAPONS=off
            // draws the frame alone (diagnostics).
            const char *weapons = TestEnv("XCX_STILL_WEAPONS");
            if (!(weapons && std::string_view(weapons) == "off") &&
                std::any_of(m.weapons.begin(), m.weapons.end(), [](auto v) { return v != 0; }))
                r.weapons = m.weapons;
            return r;
        };
        {
            // Also read by the asset worker (the standalone picture key).
            std::scoped_lock lock(mutex);
            controlled_request = data.controlled >= 0 ? request(data.party[data.controlled])
                                                      : std::nullopt;
        }
        selected_request.reset(); // the Party page's detail card is the profile now (no Skell picture)
        std::string key = "c" + std::to_string(controlled_request ? controlled_request->frame : 0) +
                          (controlled_request ? 'y' : 'n') + "s" +
                          std::to_string(selected_request ? selected_request->frame : 0) +
                          (selected_request ? 'y' : 'n');
        if (key == want_key && now < next_want)
            return;
        want_key = key;
        next_want = now + std::chrono::seconds(2); // refresh the host copy now and then
        std::vector<xcx::StillRequest> wanted;
        if (controlled_request)
            wanted.push_back(*controlled_request);
        if (selected_request && (!controlled_request || selected_request->frame != controlled_request->frame))
            wanted.push_back(*selected_request);
        stills.Want(h, wanted);
    }
    xcx::StillPictures Pictures() const {
        xcx::StillPictures p;
        if (controlled_request)
            p.controlled = stills.Get(*controlled_request);
        if (selected_request)
            p.selected = stills.Get(*selected_request);
        return p;
    }
    void Invalidate() {
        data = {};
        key.clear();
        force = true;
        next_read = {};
        std::scoped_lock lock(mutex);
        frames.clear();
        ++epoch;
    }
    void Sample(const EdenDsmodHostApi &h) {
        const auto now = Clock::now();
        if (now >= next_read) {
            xcx::Snapshot next;
            try {
                next = xcx::Read(h);
            } catch (...) {
                Invalidate();
            }
            const bool changed_context =
                data.valid &&
                (!next.valid || data.zone != next.zone || data.controlled != next.controlled ||
                 data.party[data.controlled].pc.handle != next.party[next.controlled].pc.handle);
            if (changed_context)
                Invalidate();
            data = std::move(next);
            next_read = now + std::chrono::milliseconds(100);
            UpdateMapState();
        }
        // A switch reorders the party: follow the picked member by id, never by slot index.
        if (selected_id >= 0)
            for (int i = 0; i < 4; ++i)
                if (data.party[i].pc.present && data.party[i].pc.id == selected_id)
                    view.selected = i;
        if (view.selected < 0 || view.selected >= 4 || !data.party[view.selected].pc.present)
            view.selected = data.controlled < 0 ? 0 : data.controlled;
        UpdateHelper(NowMs());
        UpdateHud(h, now);
        WantStills(h, now);
        auto pictures = Pictures();
        // The player's Reduced motion choice lives in the runtime flag (persisted by the manifest).
        reduced_motion = h.get_i64 && h.get_i64(h.userdata, "@flag:motion.reduced", 0) != 0;
        view.reduced_motion = reduced_motion;
        map_mode = h.get_i64 && h.get_i64(h.userdata, "@flag:xcx.map_mode", 0) != 0 ? 1 : 0;
        view.map_mode = map_mode;
        view.widget = data.valid && data.controlled >= 0
                          ? xcx::tiles::WidgetFor(map_mode, xcx::map::KindForZone(data.zone))
                          : xcx::tiles::Widget::None;
        const std::int64_t clock = NowMs();
        // A finger on a map widget's pan / pinch (one tick behind, like every @view_* key).
        if (h.get_i64) {
            bool touch = false;
            for (const char *id : {"@view_touch_down:xcx-map-over", "@view_touch_down:xcx-map-home",
                                   "@view_touch_down:xcx-map-world", "@view_touch_down:xcx-map-nla",
                                   "@view_touch_down:xcx-map-barracks"})
                touch |= h.get_i64(h.userdata, id, 0) != 0;
            if (touch)
                map_touch_until = clock + MapTouchTailMs;
        }
        map_gesture = clock < map_touch_until;
        if (const int note = clock < rail_note_until && view.page == 0 ? rail_note_row : -1;
            note != view.rail_note) {
            view.rail_note = note;
            force = true;
        }
        const bool motion = !reduced_motion;
        // The Battle page's numbers follow the game at once (BATTLE-UX s2: no rolls in a fight).
        const bool roll = motion && view.page != 4;
        // What the page shows: the read values, with a big HP jump rolled over a short time.
        auto saved_flags = std::move(data.flags); // 10 KB the page never draws: not copied into shown / frames
        xcx::Snapshot shown = data;
        data.flags = std::move(saved_flags);
        bool moving = false;
        for (int i = 0; i < 4; ++i) {
            auto &member = shown.party[i];
            if (!data.valid || !member.pc.present) {
                pc_hp_roll[i] = {};
                skell_hp_roll[i] = {};
                roll_member[i] = -1;
                continue;
            }
            if (roll_member[i] != int(member.pc.id)) { // another member in this slot: no roll across
                pc_hp_roll[i] = {};
                skell_hp_roll[i] = {};
                roll_member[i] = int(member.pc.id);
            }
            member.pc.hp = int(std::lround(pc_hp_roll[i].Update(
                member.pc.hp, clock, roll, std::max(1, member.pc.max_hp) * 0.08)));
            moving |= pc_hp_roll[i].Active();
            if (member.skell.present) {
                member.skell.hp = int(std::lround(skell_hp_roll[i].Update(
                    member.skell.hp, clock, roll, std::max(1, member.skell.max_hp) * 0.08)));
                moving |= skell_hp_roll[i].Active();
            } else {
                skell_hp_roll[i] = {};
            }
        }
        pictures.controlled_alpha =
            controlled_fade.Update(pictures.controlled != nullptr, clock, motion);
        pictures.selected_alpha = selected_fade.Update(pictures.selected != nullptr, clock, motion);
        moving |= controlled_fade.Active() || selected_fade.Active();
        if (moving && !motion_active) // the first frame of a move does not wait for the 250 ms rest cadence
            force = true;
        motion_active = moving;
        // 3 = Return to Skell: enabled (a 600 ms hold acts), with a "Hold" hint after a short tap.
        const int face = button.face == xcx_helper::Face::Board  ? 1
                         : button.face == xcx_helper::Face::Exit ? 2
                         : button.return_ready                   ? 3
                                                                 : 0;
        const int note = button.hint ? 3
                         : button.reason == xcx_helper::Reason::NotNow ||
                                 button.reason == xcx_helper::Reason::Refused
                             ? 1
                         : button.reason == xcx_helper::Reason::Busy ||
                                 button.reason == xcx_helper::Reason::Settling ||
                                 button.reason == xcx_helper::Reason::Lockout
                             ? 2
                             : 0;
        // Only the Board / Exit faces differ from the prototype; a missing, stale or ineligible
        // helper leaves every field at its default, so the page is the prototype's.
        view.button_face = face;
        view.button_enabled = face == 3 || (face != 0 && button.enabled);
        view.button_note = face == 3 ? (button.hint ? 3 : 0) : face != 0 && !button.enabled ? note : 0;
        // Party page Set plate: the prototype's face unless the helper is live and the pick is another member.
        const bool set_working = set_button.reason == xcx_helper::Reason::Busy ||
                                 set_button.reason == xcx_helper::Reason::Lockout ||
                                 set_button.reason == xcx_helper::Reason::Settling;
        view.set_face = set_button.enabled || set_working || (helper_present && set_blocked) ? 1 : 0;
        view.set_enabled = set_button.enabled;
        view.set_note = set_blocked && !set_working ? 1 : 0;
        // Battle page (xcx_battle.h): the in-combat flag is the party's (PartyFlag).
        // XCX_BATTLE_FLAG=in|out overrides it in the offline tests only.
        auto flag = ReadFlag();
        {
            using xcx::battle::Dim;
            const auto known = flag == xcx::battle::Flag::Unknown ? auto_switch.Last() : flag;
            const bool eject = xcx::battle::EjectSequence(data, known);
            const Dim raw = xcx::battle::DimFor(helper_present, data.valid, helper_field_ok, helper_input_ready,
                                                eject);
            // Title / Loading / Live: held 300 ms like the dim (an actor handover must not flash it).
            const auto next_stage = stage_filter.Update(
                xcx::StageFor(data.valid, helper_present, helper_object, helper_field_ok, eject), clock);
            if (next_stage != stage) {
                stage = next_stage;
                force = true;
            }
            if (raw != dim_raw) {
                dim_raw = raw;
                dim_since = clock;
            }
            if (raw == Dim::None || clock - dim_since >= DimDelayMs) {
                if (dim_shown != raw)
                    force = true;
                dim_shown = raw;
            }
        }
        // The automatic Battle view: an unreadable sample keeps the last known flag (actor handover).
        flag_raw = flag;
        if (const int to = auto_switch.Update(clock, flag, dim_raw != xcx::battle::Dim::None, view.page);
            to >= 0 && to != view.page) {
            if (to == 4) { // a selection that was not sent is discarded on entry
                selected_id = -1;
                view.selected = data.controlled < 0 ? 0 : data.controlled;
            }
            view.page = to;
            auto_page = true;
            force = true;
        }
        if (flag == xcx::battle::Flag::Unknown)
            flag = auto_switch.Last();
        view.battle_flag = flag;
        view.battle_underline = auto_switch.Engaged();
        const bool live = stage == xcx::Stage::Live;
        view.stage = stage;
        // Live only: a loading screen is the stage's (the loading view), never the Battle page's chip.
        view.dim = view.page == 4 && live && dim_shown == xcx::battle::Dim::Menu ? dim_shown
                                                                                 : xcx::battle::Dim::None;
        view.menu = live && dim_shown == xcx::battle::Dim::Menu;
        if (!live) // no map widget, marker or position while no field is playable
            view.widget = xcx::tiles::Widget::None;
        view.return_blocked = view.page == 4 && !xcx::battle::ReturnAllowed(flag);
        // Pressing + while a menu or loading screen is up would do something else: no debounce here.
        const bool commands = xcx::battle::CommandsAvailable(data.valid, helper_present,
                                                             helper_field_ok, helper_input_ready);
        view.commands = view.page == 4 && commands;
        if (force || now >= next_frame) {
            std::ostringstream signature;
            signature << "xcx-b2 " << epoch << ' ' << view.page << ' ' << view.selected << ' '
                      << view.zoom << " u" << view.battle_underline << " st" << int(stage) << view.menu;
            signature << ' ' << data.valid << ' ' << data.zone << ' ' << data.controlled << ' '
                      << data.area;
            if (view.page == 0) // the chrome: mode tab and which widget shows
                signature << " map " << view.map_mode << int(view.widget);
            if (view.page == 1) // the profile tabs and the selected member's record + stats
                signature << " ptab " << view.party_tab << ' ' << ProfileText(shown.party[view.selected]);
            if (view.page == 0 && view.rail_note >= 0) // added only while shown: the old keys stay
                signature << " rail " << view.rail_note;
            if (view.reduced_motion && view.page == 3)
                signature << " reduced";
            if (view.page == 3) // the HUD toggles and whether the game state allows them
                signature << " hud " << view.hud_hide_map << view.hud_hide_fuel
                          << (view.hud_map_state == 4) << (view.hud_fuel_state == 4);
            if (view.page == 4) { // the Battle page's own inputs
                signature << " battle " << int(view.battle_flag) << int(view.dim)
                          << view.return_blocked << view.commands;
                for (const auto &m : shown.party)
                    signature << ' ' << m.mounted << m.skell.driven;
            }
            if (pictures.controlled_alpha < 1.f)
                signature << " fadec " << int(pictures.controlled_alpha * 32);
            if (pictures.selected_alpha < 1.f)
                signature << " fades " << int(pictures.selected_alpha * 32);
            for (const auto &m : shown.party) {
                signature << ' ' << m.pc.present << ' ' << m.pc.id << ' ' << m.pc.hp << ' '
                          << m.pc.max_hp << ' ' << m.pc.points << ' ' << m.pc.max_points;
                signature << ' ' << m.skell.present << ' ' << m.skell.hp << ' ' << m.skell.max_hp
                          << ' ' << m.skell.points << ' ' << m.skell.max_points;
                signature << ' ' << std::lround(m.skell.fuel) << ' '
                          << std::lround(m.skell.max_fuel) << ' ' << m.owned << ' '
                          << int(m.insurance) << ' ' << m.skell_name;
                signature << ' ' << int(m.controlled); // positions: map widget binds only (PublishMap)
            }
            if (view.set_face)
                signature << " set " << view.set_enabled << view.set_note;
            if (view.button_face)
                signature << " btn " << view.button_face << view.button_enabled << view.button_note;
            // Added only once a picture has landed, so a page without one keeps its old key.
            if (pictures.controlled)
                signature << " still " << controlled_request->frame;
            if (pictures.selected)
                signature << " sel " << selected_request->frame;
            std::uint64_t hash = 14695981039346656037ULL;
            for (unsigned char c : signature.str()) {
                hash ^= c;
                hash *= 1099511628211ULL;
            }
            std::ostringstream out;
            out << "module:xcx:page/" << std::hex << hash;
            const auto next = out.str();
            // Battle page: the host's single asset worker renders queued page pictures in order
            // (~120 ms each), so queueing one per 100 ms read built a backlog and the page trailed the
            // game by about a second (BATTLE-LIVE-2). A new picture is queued only once the last one
            // was rendered (or after FrameWait), so the newest values always come next.
            bool wait_for_render = false;
            if (next != key && view.page == 4) {
                std::scoped_lock lock(mutex);
                wait_for_render = !key.empty() && served_key != key && now - key_time < FrameWait;
            }
            if (wait_for_render) {
                next_frame = now + std::chrono::milliseconds(15); // try again on the next samples
            } else if (next != key) {
                std::scoped_lock lock(mutex);
                frames.push_back({next, shown, view, epoch, pictures});
                while (frames.size() > 16)
                    frames.pop_front();
                key = next;
                key_time = now;
            }
            force = false;
            // While something moves frames follow at the motion cadence; at rest it is the old 250 ms.
            // The Battle page follows every read (100 ms): its HP numbers must keep up with the top HUD.
            if (!wait_for_render)
                next_frame = now + std::chrono::milliseconds(motion_active ? xcx::MotionFrameMs
                                                             : view.page == 4 ? 100
                                                                              : 250);
        }
        bool skell = false;
        for (const auto &m : data.party)
            skell |= m.pc.present && m.skell.present;
        if (h.publish_text) {
            h.publish_text(h.userdata, "xcx.frame", key.c_str());
            // The equipped Skell's picture on its own, once it exists ("" until then).
            h.publish_text(h.userdata, "xcx.skell_still",
                           pictures.controlled
                               ? ("module:xcx:skell/" + std::to_string(controlled_request->frame)).c_str()
                               : "");
        }
        if (h.publish_i64) {
            h.publish_i64(h.userdata, "xcx.still_ready", pictures.controlled != nullptr);
            h.publish_i64(h.userdata, "xcx.page", view.page);
            // Which page_binds may fire: the fade set, or the cut set while motion is reduced.
            const auto points = xcx::PageBindPoints(view.page, reduced_motion, auto_page);
            h.publish_i64(h.userdata, "xcx.pg_fade", points.fade);
            h.publish_i64(h.userdata, "xcx.pg_cut", points.cut);
            h.publish_i64(h.userdata, "xcx.pg_auto", points.automatic);
            // The in-combat flag: raw (1 in, 0 out, -1 unknown) and the debounced battle state.
            h.publish_i64(h.userdata, "xcx.battle", auto_switch.Engaged());
            h.publish_i64(h.userdata, "xcx.dbg.bflag", flag_raw == xcx::battle::Flag::In    ? 1
                                                       : flag_raw == xcx::battle::Flag::Out ? 0
                                                                                            : -1);
            h.publish_i64(h.userdata, "xcx.dbg.boverride", auto_switch.Overridden());
            h.publish_i64(h.userdata, "xcx.dbg.bflicker", auto_switch.Flicker());
            h.publish_i64(h.userdata, "xcx.dbg.set_blocked", set_blocked);
            h.publish_i64(h.userdata, "xcx.dbg.set_in_battle", set_in_battle);
            // Per slot (diagnostics for the live HP / GP checks): the card's HP and points as drawn
            // (Skell values for a mounted member), the HUD row's, and every actor's raw battle state.
            for (int i = 0; i < 4; ++i) {
                char name[32];
                const auto card = xcx::battle::CardFor(data.party[i]);
                std::snprintf(name, sizeof name, "xcx.dbg.chp%d", i);
                h.publish_i64(h.userdata, name, card.present ? card.hp : -1);
                std::snprintf(name, sizeof name, "xcx.dbg.cpt%d", i);
                h.publish_i64(h.userdata, name, card.present ? card.points : -1);
                std::snprintf(name, sizeof name, "xcx.dbg.hhp%d", i);
                h.publish_i64(h.userdata, name, data.hud[i].hp);
                std::snprintf(name, sizeof name, "xcx.dbg.hpt%d", i);
                h.publish_i64(h.userdata, name, data.hud[i].points);
                std::snprintf(name, sizeof name, "xcx.dbg.bs%d", i);
                h.publish_i64(h.userdata, name,
                              data.party[i].pc.battle_state * 100 +
                                  (data.party[i].skell.present ? data.party[i].skell.battle_state : 99));
            }
            h.publish_i64(h.userdata, "xcx.motion_reduced", reduced_motion);
            h.publish_i64(h.userdata, "xcx.motion_active", motion_active);
            // Title / Loading: no tab and no page action can be tapped (both tab sets are gated on these,
            // every page action on xcx.data); the stage itself for logs and routes (0 live, 1 title, 2 loading).
            const bool tabs = xcx::TabsShown(stage);
            h.publish_i64(h.userdata, "xcx.stage", int(stage));
            h.publish_i64(h.userdata, "xcx.menu", view.menu);
            h.publish_i64(h.userdata, "xcx.has_skell", tabs && skell);
            h.publish_i64(h.userdata, "xcx.no_skell", tabs && !skell);
            h.publish_i64(h.userdata, "xcx.data", tabs && data.valid);
            PublishGear(h, shown);
            // Map rail rows that take a tap: another member, a live field, no finger on the map.
            for (int i = 0; i < 4; ++i) {
                char name[24];
                std::snprintf(name, sizeof name, "xcx.rail_tap%d", i);
                h.publish_i64(h.userdata, name,
                              tabs && data.valid && i > 0 && data.party[i].pc.present &&
                                  !data.party[i].controlled && !map_gesture);
            }
            h.publish_i64(h.userdata, "xcx.rail_press",
                          NowMs() < rail_press_until && view.page == 0 ? rail_press_row : -1);
            // The helper's own heartbeat and state word are fresh. The button's tap target exists
            // only then, so without the helper the page has the prototype's inert button.
            h.publish_i64(h.userdata, "xcx.helper", helper_present);
            h.publish_i64(h.userdata, "xcx.actions_available", helper_present);
            h.publish_i64(h.userdata, "xcx.skell_button_enabled", button.enabled);
            h.publish_i64(h.userdata, "xcx.dbg.heartbeat", diag_heartbeat);
            h.publish_i64(h.userdata, "xcx.dbg.state", diag_state);
            h.publish_i64(h.userdata, "xcx.dbg.answered", diag_answered);
            h.publish_i64(h.userdata, "xcx.dbg.last_result", last_result);
            h.publish_i64(h.userdata, "xcx.dbg.slot0_token", diag_slot0);
            h.publish_i64(h.userdata, "xcx.dbg.switches", diag_switches);
            h.publish_i64(h.userdata, "xcx.dbg.switch_done", diag_switch_done);
            h.publish_i64(h.userdata, "xcx.set_enabled", set_button.enabled);
            // Battle Commands: the + press is offered only while the field takes input.
            h.publish_i64(h.userdata, "xcx.battle_cmd_ok", commands);
            // Return to Skell for the view: face kind 0 disabled (prototype), 1 Board, 2 Exit, 3 Return
            // (a 600 ms hold acts; the host reports no hold progress to modules), and the "Hold" hint.
            h.publish_i64(h.userdata, "xcx.face_kind",
                          button.face == xcx_helper::Face::Board  ? 1
                          : button.face == xcx_helper::Face::Exit ? 2
                          : button.return_ready                   ? 3
                                                                  : 0);
            h.publish_i64(h.userdata, "xcx.return_ready", button.return_ready);
            h.publish_i64(h.userdata, "xcx.return_hint", button.hint);
            h.publish_i64(h.userdata, "xcx.dbg.can_return", diag_can_return);
            h.publish_i64(h.userdata, "xcx.dbg.field_ok", diag_field_ok);
            // Top HUD: map state | fuel state << 4 | commits << 8 | write api << 20 | aux << 21 | live << 22 |
            // field-HUD observer found << 23
            h.publish_i64(h.userdata, "xcx.dbg.hud", hud_diag);
            for (std::size_t i = 0; i < diag_words.size(); ++i) {
                char name[24];
                std::snprintf(name, sizeof name, "xcx.dbg.d%02zu", i);
                h.publish_i64(h.userdata, name, diag_words[i]);
            }
        }
        PublishMap(h);
    }
    // The map widgets' world pictures depend on the area and the saved flags only; keep a copy for the
    // asset worker and the key suffix that changes when they do.
    void UpdateMapState() {
        std::uint64_t hash = 14695981039346656037ULL;
        for (const auto byte : data.flags) {
            hash ^= byte;
            hash *= 1099511628211ULL;
        }
        const int here = xcx::ContinentOfZone(data.zone);
        char gen[40];
        std::snprintf(gen, sizeof gen, "%08x-%d", unsigned(hash ^ (hash >> 32)), here + 1);
        std::scoped_lock lock(map_mutex);
        map_zone = data.zone;
        if (map_flags != data.flags)
            map_flags = data.flags;
        map_gen = gen;
    }
    // Every sample: what the native map widgets bind to (manifest page "map"): which widget shows, the
    // live marker (world x, y = -z), its heading, the parked Skell, the picture keys and which of the
    // tiled overlays the current view needs.
    void PublishMap(const EdenDsmodHostApi &h) {
        using namespace xcx;
        if (!h.publish_i64 || !h.publish_f64 || !h.publish_text)
            return;
        const bool ok = data.valid && data.controlled >= 0;
        const auto kind = ok ? map::KindForZone(data.zone) : map::Kind::None;
        const auto widget = view.widget;
        h.publish_i64(h.userdata, "xcx.map_mode", map_mode);
        h.publish_i64(h.userdata, "xcx.mw.over", widget == tiles::Widget::Overworld);
        h.publish_i64(h.userdata, "xcx.mw.home", widget == tiles::Widget::OverworldHome);
        h.publish_i64(h.userdata, "xcx.mw.world", widget == tiles::Widget::AreaWorld);
        h.publish_i64(h.userdata, "xcx.mw.nla", widget == tiles::Widget::AreaNla);
        h.publish_i64(h.userdata, "xcx.mw.barracks", widget == tiles::Widget::AreaBarracks);
        h.publish_i64(h.userdata, "xcx.mw.none", ok && widget == tiles::Widget::AreaNone);
        const double nan = std::nan("");
        const bool located = ok && (kind == map::Kind::World || kind == map::Kind::Nla);
        const auto &pc = ok ? data.party[data.controlled].pc : Actor{};
        const auto &mount = ok ? data.party[data.controlled].skell : Actor{};
        // Where the controlled member is and which way it faces: the actor carrying the driven bit (the
        // character's own values are stale while it rides the Skell); in a transition the last known pose.
        const auto pose_of = [](const Actor &a) {
            return tiles::ActorPose{a.present, a.driven, a.yaw_valid, double(a.xyz[0]), double(a.xyz[2]), double(a.yaw)};
        };
        const auto pose = ok ? tiles::ChoosePose(pose_of(pc), pose_of(mount), last_pose) : tiles::Pose{};
        if (located && pose.valid)
            last_pose = pose;
        const bool mounted_pose = ok && mount.present && mount.driven;
        const double px = located ? (mounted_pose ? double(mount.xyz[0]) : double(pc.xyz[0])) : nan;
        const double py = located ? -(mounted_pose ? double(mount.xyz[2]) : double(pc.xyz[2])) : nan;
        // Inside the Barracks (on foot only) the marker is in the Barracks picture's own units
        // (contracts/BARRACKS-MARKER.md); the Overworld there stays the whole of Mira without a marker.
        const auto inside = ok && widget == tiles::Widget::AreaBarracks
                                ? tiles::BarracksWidgetPoint(double(pc.xyz[0]), double(pc.xyz[2]))
                                : std::nullopt;
        h.publish_f64(h.userdata, "xcx.pl_x", inside ? inside->x : px);
        h.publish_f64(h.userdata, "xcx.pl_y", inside ? inside->y : py);
        // The heading (radians, map-up clockwise, actor+0x64): valid only when the actor and serial checks
        // passed and the float is finite; otherwise the arrow points up (0).
        const bool yaw_valid = (located || inside) && pose.valid;
        h.publish_i64(h.userdata, "xcx.player_yaw_valid", yaw_valid);
        h.publish_f64(h.userdata, "xcx.player_yaw", yaw_valid ? pose.yaw : 0.);
        h.publish_i64(h.userdata, "xcx.player_yaw_deg", yaw_valid ? tiles::HeadingDegrees(pose.yaw) : 0);
        // The parked Skell (not drawn while the player is near it, so it never hides the arrow, or piloting it).
        bool show_skell = false;
        double sx = nan, sy = nan;
        if (located) {
            const auto &sk = data.party[data.controlled].skell;
            if (sk.present && std::isfinite(sk.xyz[0]) && std::isfinite(sk.xyz[2]) &&
                std::hypot(double(sk.xyz[0] - pc.xyz[0]), double(sk.xyz[2] - pc.xyz[2])) > 40.) {
                show_skell = true;
                sx = sk.xyz[0];
                sy = -double(sk.xyz[2]);
            }
        }
        h.publish_f64(h.userdata, "xcx.sk_x0", sx);
        h.publish_f64(h.userdata, "xcx.sk_y0", sy);
        h.publish_i64(h.userdata, "xcx.sk_kind0", show_skell ? 1 : 0);
        std::string gen;
        {
            std::scoped_lock lock(map_mutex);
            gen = map_gen;
        }
        h.publish_text(h.userdata, "xcx.mblank", tiles::KeyText({tiles::Set::Blank, 0, 0, "0"}).c_str());
        h.publish_text(h.userdata, "xcx.mb", tiles::KeyText({tiles::Set::Barracks, 0, 0, "0"}).c_str());
        h.publish_text(h.userdata, "xcx.msk", tiles::KeyText({tiles::Set::Skell, 0, 0, "0"}).c_str());
        // The area views as the runtime drew them (one tick behind); before the first draw, around the player.
        const auto view_rect = [&](const char *area, double width_m, double height_m) {
            const auto get = [&](const char *what) {
                char name[80];
                std::snprintf(name, sizeof name, "@map_view_%s:%s", what, area);
                return h.get_f64 ? h.get_f64(h.userdata, name, nan) : nan;
            };
            const double cx = get("cx"), cy = get("cy"), ppw = get("ppw"), w = get("w"), hh = get("h");
            if (std::isfinite(cx) && std::isfinite(cy) && std::isfinite(ppw) && ppw > 0 && std::isfinite(w) &&
                std::isfinite(hh))
                return tiles::ViewRect{cx, cy, w / ppw, hh / ppw};
            return tiles::ViewRect{std::isfinite(px) ? px : 0., std::isfinite(py) ? py : 0., width_m, height_m};
        };
        char name[48];
        const auto world_view = view_rect("xcx_area", 2400., 2700.);
        const auto nla_view = view_rect("xcx_nla", 520., 590.);
        const auto world_need = widget == tiles::Widget::AreaWorld && world_view.w <= tiles::HiMaxViewMetres
                                    ? tiles::Nearest(tiles::Set::Hi, world_view, .5, tiles::MaxVisible)
                                    : std::vector<int>{};
        // New Los Angeles: what the view needs, then the pictures around the player, so a view the runtime
        // reports oddly (or not at all) can never leave the city map empty; MaxVisible at most (tiles::NlaNeed).
        const auto nla_need =
            widget == tiles::Widget::AreaNla ? tiles::NlaNeed(nla_view, px, py) : std::vector<int>{};
        const auto contains = [](const std::vector<int> &v, int n) { return std::find(v.begin(), v.end(), n) != v.end(); };
        // What to build (most important first): the quarter-resolution quarters of the world (the player's
        // first) for every view that shows them, then the full-size pictures the view needs.
        std::uint32_t zone{};
        std::vector<std::uint8_t> flags;
        {
            std::scoped_lock lock(map_mutex);
            zone = map_zone;
            flags = map_flags;
        }
        std::vector<tiles::Key> wanted;
        const bool lo_wanted = widget == tiles::Widget::Overworld || widget == tiles::Widget::OverworldHome ||
                               widget == tiles::Widget::AreaWorld;
        std::vector<int> lo_order{0, 1, 2, 3};
        const double fx = std::isfinite(px) ? px : 0., fy = std::isfinite(py) ? py : 0.;
        std::sort(lo_order.begin(), lo_order.end(), [&](int a, int b) {
            const auto ca = tiles::LoBox(a % tiles::LoCols, a / tiles::LoCols), cb = tiles::LoBox(b % tiles::LoCols, b / tiles::LoCols);
            return std::hypot((ca.x0 + ca.x1) / 2 - fx, (ca.y0 + ca.y1) / 2 - fy) <
                   std::hypot((cb.x0 + cb.x1) / 2 - fx, (cb.y0 + cb.y1) / 2 - fy);
        });
        if (lo_wanted)
            for (const int n : lo_order)
                wanted.push_back({tiles::Set::Lo, n % tiles::LoCols, n / tiles::LoCols, gen});
        for (const int n : world_need)
            wanted.push_back({tiles::Set::Hi, n % tiles::HiCols, n / tiles::HiCols, gen});
        for (const int n : nla_need)
            wanted.push_back({tiles::Set::Nla, n % tiles::NlaCols, n / tiles::NlaCols, "0"});
        if (ok)
            map_builder.Want(h, zone, flags, wanted);
        // A picture is shown only once it is built (the host's image request is then answered at once).
        for (int n = 0; n < tiles::HiCols * tiles::HiRows; ++n) {
            const tiles::Key k{tiles::Set::Hi, n % tiles::HiCols, n / tiles::HiCols, gen};
            std::snprintf(name, sizeof name, "xcx.mh.%d", n);
            h.publish_text(h.userdata, name, tiles::KeyText(k).c_str());
            std::snprintf(name, sizeof name, "xcx.mhs.%d", n);
            h.publish_i64(h.userdata, name, contains(world_need, n) && (SyncMapPictures || map_builder.Ready(k)));
        }
        for (int n = 0; n < tiles::LoCols * tiles::LoRows; ++n) {
            const tiles::Key k{tiles::Set::Lo, n % tiles::LoCols, n / tiles::LoCols, gen};
            std::snprintf(name, sizeof name, "xcx.ml.%d", n);
            h.publish_text(h.userdata, name, tiles::KeyText(k).c_str());
            std::snprintf(name, sizeof name, "xcx.mls.%d", n);
            h.publish_i64(h.userdata, name, lo_wanted && (SyncMapPictures || map_builder.Ready(k)));
        }
        for (int n = 0; n < tiles::NlaCols * tiles::NlaRows; ++n) {
            const tiles::Key k{tiles::Set::Nla, n % tiles::NlaCols, n / tiles::NlaCols, "0"};
            std::snprintf(name, sizeof name, "xcx.mn.%d", n);
            h.publish_text(h.userdata, name, tiles::KeyText(k).c_str());
            std::snprintf(name, sizeof name, "xcx.mns.%d", n);
            h.publish_i64(h.userdata, name, contains(nla_need, n) && (SyncMapPictures || map_builder.Ready(k)));
        }
    }
    bool SkellButton() {
        // Serialized with sample/tick. A refusal here is the "refused" haptic; nothing is staged.
        if (!data.valid || (view.page != 0 && view.page != 2 && view.page != 4))
            return false;
        const auto now = NowMs();
        UpdateHelper(now);
        std::scoped_lock lock(helper_mutex);
        if (!bridge)
            return false;
        // Battle page: the Return face is disabled there while ReturnAllowed says so: refuse, no hint.
        if (view.page == 4 && button.return_ready && view.return_blocked)
            return false;
        const auto press = controller.OnPress(*bridge, button, now);
        if (!press.accepted)
            return false;
        button = controller.Update(bridge->status(), HasSkell(), now);
        force = true;
        return true;
    }
    // Hold on the Skell button's rect: the native Return to Skell (a long loading screen follows).
    bool ReturnHold() {
        if (!data.valid || (view.page != 0 && view.page != 2 && view.page != 4) ||
            (view.page == 4 && view.return_blocked))
            return false;
        const auto now = NowMs();
        UpdateHelper(now);
        std::scoped_lock lock(helper_mutex);
        if (!bridge)
            return false;
        const auto press = controller.OnReturnHold(*bridge, button, now);
        if (!press.accepted)
            return false;
        button = controller.Update(bridge->status(), HasSkell(), now);
        force = true;
        return true;
    }
    // The Set plate on the Party and Battle pages: switch the controlled character to the picked member.
    bool SwitchSelected() {
        if (!data.valid || (view.page != 1 && view.page != 4) || view.selected < 1 || view.selected > 3 ||
            !data.party[view.selected].pc.present)
            return false;
        const auto now = NowMs();
        UpdateHelper(now);
        std::scoped_lock lock(helper_mutex);
        if (!bridge)
            return false;
        const auto &member = data.party[view.selected];
        if (!xcx::battle::SwitchTargetAllowed(member)) // riding its Skell: a switch locks control
            return false;
        if (set_in_battle) // mid-battle: a switch ends the fight and kills the stick for ~1 min
            return false;
        const auto press = switcher.OnPress(*bridge, set_button, view.selected, member.pc.id, now);
        if (!press.accepted)
            return false;
        set_button = switcher.Update(bridge->status(), true, member.controlled, now);
        force = true;
        return true;
    }
    // A tap on a party row of the Map page's right rail: the Set plate's switch for that member. The
    // controlled member's row has no tap target (no-op, no haptic); a gated tap is refused with the hint.
    bool RailSwitch(int row) {
        if (!data.valid || view.page != 0 || row < 0 || row > 3 || !data.party[row].pc.present)
            return false;
        const auto &member = data.party[row];
        const auto now = NowMs();
        if (member.controlled || row == 0 || now < map_touch_until)
            return true; // a tap that raced the row's target going away: nothing to do
        rail_press_row = row;
        rail_press_until = now + RailPressMs;
        UpdateHelper(now);
        const auto refuse = [&] {
            rail_note_row = row;
            rail_note_until = now + RailNoteMs;
            force = true;
            return false;
        };
        std::scoped_lock lock(helper_mutex);
        if (!bridge)
            return refuse();
        // Riding its Skell (a switch locks control) or a battle on (party-wide): as the Set plate.
        if (!xcx::battle::SwitchTargetAllowed(member) || set_in_battle)
            return refuse();
        // Menu / loading / piloting / cooldown: the state's switch_ready and the switcher's lockout.
        const auto state = switcher.Update(bridge->status(), true, false, now);
        const auto press = switcher.OnPress(*bridge, state, row, member.pc.id, now);
        if (!press.accepted)
            return refuse();
        rail_note_row = -1;
        force = true;
        return true;
    }
    // Everything the Party page profile draws for a member, as text (page and Gear list keys).
    static std::string ProfileText(const xcx::Member &m) {
        std::ostringstream o;
        const auto &p = m.profile;
        const auto &st = m.pc.stats;
        o << m.pc.present << ' ' << m.pc.id << " r" << p.valid << ' ' << int(p.level) << ' ' << int(p.class_id) << ' '
          << p.rank;
        for (const auto &g : p.gear)
            o << ' ' << g.item << ':' << g.effects[0] << ',' << g.effects[1] << ',' << g.effects[2];
        o << " s" << st.valid << ' ' << st.melee << ' ' << st.ranged << ' ' << st.potential << ' ' << st.defense << ' '
          << st.melee_acc << ' ' << st.ranged_acc << ' ' << st.evasion;
        for (auto r : st.res)
            o << ' ' << r;
        o << ' ' << m.owned << ' ' << m.skell_name;
        return o.str();
    }
    // The Gear tab list: shown on the Party page's Gear tab while a save is loaded (values "--" while
    // loading, dimmed under a menu); the key changes with the member's gear, so a change redraws it.
    void PublishGear(const EdenDsmodHostApi &h, const xcx::Snapshot &shown) {
        const bool on = view.page == 1 && view.party_tab == 1 && data.valid && stage != xcx::Stage::Title &&
                        view.selected >= 0 && view.selected < 4 && shown.party[view.selected].pc.present;
        std::string next;
        int height = 0, member_id = -1;
        if (on) {
            const auto &m = shown.party[view.selected];
            member_id = m.pc.id;
            height = xcx::Renderer::GearListHeight(m);
            std::ostringstream sig;
            sig << "gear1 " << epoch << ' ' << int(stage) << view.menu << ' ' << ProfileText(m);
            std::uint64_t hash = 14695981039346656037ULL;
            for (unsigned char c : sig.str()) {
                hash ^= c;
                hash *= 1099511628211ULL;
            }
            std::ostringstream out;
            out << "module:xcx:gear/" << std::hex << hash;
            next = out.str();
            std::scoped_lock lock(mutex);
            if (next != gear_key) {
                gear_key = next;
                gear_member = m;
                gear_view = view;
            }
        }
        if (h.publish_text)
            h.publish_text(h.userdata, "xcx.gear", next.c_str());
        h.publish_i64(h.userdata, "xcx.gear_tab", on);
        h.publish_i64(h.userdata, "xcx.gear_h", height);
        h.publish_i64(h.userdata, "xcx.gear_member", member_id);
        h.publish_i64(h.userdata, "xcx.party_tab", view.party_tab);
    }
    bool Action(std::string_view a, std::int64_t arg) {
        // Title / Loading: no game action (the host shows no tap target either; this is the module's own
        // refusal for a tap that raced the stage change). Page and selection changes are companion-local.
        const bool game_action =
            a == "skell_button" || a == "return_hold" || a == "switch_set" || a == "rail_switch";
        if (game_action && stage != xcx::Stage::Live)
            return false;
        if (a == "skell_button")
            return SkellButton();
        if (a == "return_hold")
            return ReturnHold();
        if (a == "switch_set")
            return SwitchSelected();
        if (a == "rail_switch")
            return RailSwitch(int(arg));
        if (a == "page" && arg >= 0 && arg <= 4) {
            if (arg == 2) {
                bool present = false;
                for (const auto &m : data.party)
                    present |= m.skell.present;
                if (!present)
                    return false;
            }
            // A tab tap: leaves the automatic Battle view by hand (no re-entry this battle).
            auto_switch.OnTabTap(NowMs(), int(view.page), int(arg));
            auto_page = false;
            view.page = arg;
        } else if (a == "pick" && data.valid && arg >= 0 && arg < 4 && data.party[arg].pc.present) {
            view.selected = arg;
            selected_id = data.party[arg].pc.id;
            view.page = 1;
        } else if (a == "bpick" && view.page == 4 && data.valid && arg >= 0 && arg < 4 &&
                   data.party[arg].pc.present) {
            // A card on the Battle page: the same selection as the Party page, without leaving.
            view.selected = arg;
            selected_id = data.party[arg].pc.id;
        } else if (a == "ptab" && view.page == 1 && arg >= 0 && arg <= 1) {
            view.party_tab = int(arg); // companion-local: Overview / Gear
        } else if (a == "zoom" && arg >= 0 && arg <= 2)
            view.zoom = default_zoom = arg; // the Settings row: the Map's default zoom
        else if (a == "center" && data.valid)
            view.zoom = default_zoom; // Find Self returns to the chosen zoom, not to Near
        else
            return false; // Unproven guest actions and area-follow setting remain unavailable.
        force = true;
        return true;
    }
    bool Image(const EdenDsmodHostApi &h, const char *image_key, void *receiver,
               EdenDsmodImageSink sink) {
        if (const auto key = xcx::tiles::ParseKey(image_key)) {
            // A picture for a native map widget (world space, see xcx_maptiles.h).
            std::uint32_t zone{};
            std::vector<std::uint8_t> flags;
            {
                std::scoped_lock lock(map_mutex);
                zone = map_zone;
                flags = map_flags;
            }
            if (key->set == xcx::tiles::Set::Hi || key->set == xcx::tiles::Set::Lo || key->set == xcx::tiles::Set::Nla) {
                // Built on the map builder's thread (seconds on slow files); only ready ones are ever shown.
                const auto built = map_builder.Get(*key);
                if (!built && SyncMapPictures) { // the old behaviour, kept to demonstrate the stall in the preview
                    const auto picture = renderer.MapPicture(h, zone, flags, *key);
                    sink(receiver, picture.w, picture.h, picture.rgba.data(), picture.rgba.size());
                    return !picture.Empty();
                }
                if (!built)
                    return false;
                sink(receiver, built->w, built->h, built->rgba.data(), built->rgba.size());
                return true;
            }
            const auto picture = renderer.MapPicture(h, zone, flags, *key);
            if (picture.Empty())
                return false;
            sink(receiver, picture.w, picture.h, picture.rgba.data(), picture.rgba.size());
            return true;
        }
        if (std::string_view(image_key).starts_with("module:xcx:gear/")) {
            xcx::Member member;
            xcx::View v;
            {
                std::scoped_lock lock(mutex);
                if (gear_key != image_key)
                    return false;
                member = gear_member;
                v = gear_view;
            }
            const auto picture = renderer.GearList(h, member, v);
            sink(receiver, picture.w, picture.h, picture.rgba.data(), picture.rgba.size());
            return true;
        }
        constexpr std::string_view still_prefix = "module:xcx:skell/";
        if (std::string_view(image_key).starts_with(still_prefix)) {
            // The Skell picture alone (700 x 760, transparent): only for a frame that is ready.
            std::optional<xcx::StillRequest> request;
            {
                std::scoped_lock lock(mutex);
                request = controlled_request;
            }
            if (!request || std::string(image_key) !=
                                std::string(still_prefix) + std::to_string(request->frame))
                return false;
            const auto picture = stills.Get(*request);
            if (!picture)
                return false;
            sink(receiver, picture->w, picture->h, picture->rgba.data(), picture->rgba.size());
            return true;
        }
        Frame f;
        {
            std::scoped_lock lock(mutex);
            bool found = false;
            for (const auto &item : frames)
                if (item.key == image_key) {
                    f = item;
                    found = true;
                    break;
                }
            if (!found)
                return false;
        }
        const auto image = renderer.Render(h, f.data, f.view, f.stills);
        // A queued image from a revoked context must not be delivered after acquisition failed.
        std::scoped_lock lock(mutex);
        if (f.epoch != epoch)
            return false;
        served_key = f.key;
        sink(receiver, image.w, image.h, image.rgba.data(), image.rgba.size());
        return true;
    }
};
void PublishUnavailable(const EdenDsmodHostApi *h) {
    if (!dsmod_sdk::HostAbiMatches(h))
        return;
    if (h->publish_text)
        h->publish_text(h->userdata, "xcx.frame", "");
    if (h->publish_i64) {
        h->publish_i64(h->userdata, "xcx.data", 0);
        h->publish_i64(h->userdata, "xcx.has_skell", 0);
        h->publish_i64(h->userdata, "xcx.no_skell", 0);
        h->publish_i64(h->userdata, "xcx.actions_available", 0);
        h->publish_i64(h->userdata, "xcx.helper", 0);
        h->publish_i64(h->userdata, "xcx.skell_button_enabled", 0);
        h->publish_i64(h->userdata, "xcx.battle_cmd_ok", 0);
        h->publish_i64(h->userdata, "xcx.battle", 0);
    }
}
EdenDsmodBool Supports(const char *id) { return id && xcx::SupportsBuild(id); }
void *Create(const EdenDsmodHostApi *h, const char *) {
    if (!ValidHost(h))
        return nullptr;
    try {
        return new Module(*h);
    } catch (...) {
        return nullptr;
    }
}
void Destroy(void *m) { delete static_cast<Module *>(m); }
void Sample(void *p, const EdenDsmodHostApi *h) {
    if (!p)
        return;
    auto *m = static_cast<Module *>(p);
    if (!ValidHost(h)) {
        m->Invalidate();
        m->stills.Clear(); // a revoked host keeps no pictures and runs no job
        PublishUnavailable(h);
        return;
    }
    try {
        m->Sample(*h);
    } catch (...) {
        m->Invalidate();
        PublishUnavailable(h);
    }
}
void Tick(void *, const EdenDsmodHostApi *) {}
void Configure(void *m, const EdenDsmodHostExtensions *ext) {
    try {
        if (m)
            static_cast<Module *>(m)->Configure(ext);
    } catch (...) {
    }
}
EdenDsmodBool Action(void *m, const char *a, std::int64_t n) {
    try {
        return m && a && static_cast<Module *>(m)->Action(a, n);
    } catch (...) {
        return false;
    }
}
EdenDsmodBool Image(void *m, const EdenDsmodHostApi *h, const char *key, void *r,
                    EdenDsmodImageSink sink) {
    if (!m || !ValidHost(h) || !key || !sink)
        return false;
    try {
        return static_cast<Module *>(m)->Image(*h, key, r, sink);
    } catch (const std::exception &e) {
        if (h->log)
            h->log(h->userdata, EDEN_DSMOD_LOG_WARNING, e.what());
        return false;
    }
}
const EdenDsmodModuleApi Api{EDEN_DSMOD_MODULE_ABI_VERSION,
                             sizeof(EdenDsmodModuleApi),
                             0,
                             EDEN_DSMOD_MODULE_ABI_HASH,
                             xcx::TitleId,
                             "Xenoblade Chronicles X: Definitive Edition",
                             EDEN_DSMOD_CAP_EXTENSIONS | EDEN_DSMOD_CAP_ROMFS_READ,
                             Supports,
                             Create,
                             Destroy,
                             Sample,
                             Tick};
const EdenDsmodModuleExtensions Extensions{EDEN_DSMOD_EXT_VERSION,
                                           sizeof(EdenDsmodModuleExtensions),
                                           EDEN_DSMOD_EXT_HASH,
                                           Configure,
                                           Action,
                                           Image};
// Write batches: only the top-HUD Display Settings commit (xcx_hud.h) uses them.
void ConfigureWrite(void *m, const EdenDsmodHostWriteApi *api) {
    if (m && api && api->version == EDEN_DSMOD_WRITE_EXT_VERSION && api->struct_size == sizeof(*api) &&
        api->abi_hash == EDEN_DSMOD_WRITE_EXT_HASH && api->write_batch)
        static_cast<Module *>(m)->SetWriteApi(*api);
}
const EdenDsmodModuleWriteExtensions WriteExtensions{EDEN_DSMOD_WRITE_EXT_VERSION,
                                                     sizeof(EdenDsmodModuleWriteExtensions),
                                                     EDEN_DSMOD_WRITE_EXT_HASH, ConfigureWrite};
} // namespace
DSMOD_SDK_EXPORT_MODULE(Api)
DSMOD_SDK_EXPORT_EXTENSIONS(Extensions)
DSMOD_SDK_EXPORT_WRITE_EXTENSIONS(WriteExtensions)
