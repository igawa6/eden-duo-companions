// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "xcx_assets.h"
#include "xcx_battle.h"
#include "xcx_lifecycle.h"
#include "xcx_maptiles.h"
#include "xcx_reader.h"
#include "xcx_segments.h"
#include <atomic>
#include <deque>
#include <map>
#include <memory>
#include <string>
namespace xcx {
struct View {
    int page{}, selected{}, zoom{};
    // The Skell button at (900, 820, 308, 112) on the Map and Skell pages. The default is the
    // read-only prototype's disabled "Return to Skell" face, which is what every page shows
    // while the guest helper is absent, stale or not eligible.
    int button_face{};     // 0 Return to Skell (disabled, prototype), 1 Board, 2 Exit,
                           // 3 Return to Skell enabled (a 600 ms hold acts)
    bool button_enabled{}; // Board / Exit / Return can be enabled
    int button_note{};     // 0 none, 1 "Not available now", 2 "Working...", 3 "Hold" (Return)
    // The Set-as-controlled plate on the Party page: 0 = the prototype's disabled face.
    int set_face{};
    bool set_enabled{};
    // Why a set_face plate is disabled: 0 "Working...", 1 "Not available now" (the picked member rides
    // its Skell: a switch to it locks control, BATTLE-LIVE-2).
    int set_note{};
    // Party page profile tabs for the selected member (owner 2026-11-27 item 2): 0 Overview, 1 Gear.
    int party_tab{};
    // Map page right rail (owner 2026-11-27): a tap on a member switches to it through the Set path.
    // The party slot whose row shows "Not available now" after a refused tap, else -1.
    int rail_note{-1};
    // Settings page: the player's Reduced motion choice (the runtime flag motion.reduced).
    bool reduced_motion{};
    // Settings page: the top-HUD toggles (flags xcx.hud_hide_map / xcx.hud_hide_fuel, 1 = hidden on
    // top while the companion runs) and the controller's state (xcx_hud.h State; 4 = Failed).
    bool hud_hide_map{true}, hud_hide_fuel{true};
    std::uint8_t hud_map_state{}, hud_fuel_state{};
    // Map page: the mode tab (the flag xcx.map_mode: 0 Overworld, 1 Area) and which native map widget
    // shows under the page picture (xcx_maptiles.h). The picture leaves that widget's area transparent.
    int map_mode{};
    tiles::Widget widget{tiles::Widget::None};
    // Battle page (page 4, xcx_battle.h). `return_blocked`: the Skell button's Return face is drawn
    // disabled with "Not available now" (only while xcx_battle.h ReturnAllowed refuses). `commands` = the
    // Battle Commands plate is enabled. `dim`: a native menu or loading screen dims the page.
    battle::Flag battle_flag{battle::Flag::Unknown};
    battle::Dim dim{battle::Dim::None};
    bool return_blocked{};
    bool commands{};
    // A battle is detected (the debounced in-combat flag): the Battle tab's 6 px underline.
    bool battle_underline{};
    // The non-gameplay state (xcx_lifecycle.h). Title: the title view only. Loading: the page with
    // every live value "--", dimmed, the loading plate instead of the tabs. `menu`: a native menu is
    // open over the live field (Map / Party / Skell pages dim with a "Menu open" plate; the Battle
    // page uses `dim`).
    Stage stage{Stage::Live};
    bool menu{};
};
// Finished Skell stills (xcx_still.h, StillOptions defaults: 700 x 760, transparent). Absent
// pictures leave every page exactly as it was.
struct StillPictures {
    std::shared_ptr<const Image> controlled; // the controlled member's equipped frame
    std::shared_ptr<const Image> selected;   // the Party page's selected member's frame
    // Opacity of each picture while it fades in (1 = drawn as it is).
    float controlled_alpha{1.f}, selected_alpha{1.f};
};
class Renderer {
    Assets assets;
    Font standard, numeric;
    // The avatar portrait is composed once per distinct record (reader snapshot -> avatar_look).
    AvatarLook avatar_look, avatar_key;
    Image avatar_image;
    bool avatar_ready{};
    // The FrontierNav lattice from the player's common.bdat, read once (xcx_segments.h).
    SegmentMap segments;
    // The title view's logo (the English title logo, scaled once) and whether it was tried.
    Image logo;
    bool logo_tried{};
    // Loading: every live value on the page is drawn as "--" and every gauge empty.
    bool stale{};
    std::string Num(long long n) const { return stale ? std::string("--") : std::to_string(n); }
    void TitleArt(Image &, const EdenDsmodHostApi &, bool loading);
    void LoadingPlate(Image &, const EdenDsmodHostApi &);
    void StatePlate(Image &, const EdenDsmodHostApi &, int x, int y, int w, std::string_view);
    // Item, effect and class names from the player's BDAT files (xcx_profile.h), loaded once.
    profile::Names names;
    bool names_tried{};
    const profile::Names &EnsureNames(const EdenDsmodHostApi &);
    void PartyProfile(Image &, const EdenDsmodHostApi &, const Member &, const View &);
    Ownership ownership;
    bool segments_tried{};
    void Text(Image &, int, int, std::string_view, int = 23, std::uint32_t = 0xf8f8f8ff, int = 0,
              char = 'l');
    void Hint(Image &, int x, int y, std::string_view, std::uint32_t colour, int width, char align = 'l');
    void Three(Image &, const EdenDsmodHostApi &, std::string_view, int, int, int, double = 1.);
    void Panel(Image &, const EdenDsmodHostApi &, int, int, int, int,
               std::string_view = "04083_parameter_bg01", bool fill_center = true);
    void Around(Image &, const EdenDsmodHostApi &, std::string_view, int, int, int, int);
    void Face(Image &, const EdenDsmodHostApi &, unsigned, int, int, int, bool);
    void Gauge(Image &, const EdenDsmodHostApi &, int, int, int, double);
    std::string Name(const EdenDsmodHostApi &, const Actor &);
    void Tabs(Image &, const EdenDsmodHostApi &, int, bool, bool = false);
    void Map(Image &, const EdenDsmodHostApi &, const Snapshot &, const View &);
    void WorldOverlay(Image &, const EdenDsmodHostApi &, const Snapshot &, int, int, double, int, int);
    void SkellButton(Image &, const EdenDsmodHostApi &, const View &);
    void Battle(Image &, const EdenDsmodHostApi &, const Snapshot &, const View &);
    void BattleCard(Image &, const EdenDsmodHostApi &, const Member &, int x, int y, bool selected);

    void EnsureSegments(const EdenDsmodHostApi &);
    const Image &Source(const EdenDsmodHostApi &, const std::string &);
    void TrimSources(std::size_t keep);
    std::map<std::string, Image> sources;
    const std::atomic<bool> *cancel_{}; // set: Source() throws (the map builder is stopping)
    std::deque<std::string> source_order;
    std::array<Image, 4> lo_base; // the quarter-resolution world, undecorated (2.2 MB each), decoded once
    void Decorate(Image &, double u0, double v0, double step, const std::array<bool, Continents> &lit);

  public:
    // A flag that stops a MapPicture between source tiles (it throws); nullptr = never (the page renderer).
    void SetCancel(const std::atomic<bool> *cancel) { cancel_ = cancel; }
    // The world-space pictures of the native map widgets (module:xcx:map/...). `zone` and `flags` are
    // the player's area and saved segment flags (they decide which continents are lit). Empty on failure.
    Image MapPicture(const EdenDsmodHostApi &, std::uint32_t zone, const std::vector<std::uint8_t> &flags,
                     const tiles::Key &);
    Image Render(const EdenDsmodHostApi &, const Snapshot &, const View &,
                 const StillPictures & = {});
    // The Party page's Gear tab list (GearListW x GearListH, transparent below the content), drawn by
    // the host inside a scroll region over the detail card. Values "--" while loading, dimmed under a
    // native menu, empty for a member without a valid record.
    Image GearList(const EdenDsmodHostApi &, const Member &, const View &);
    static int GearListHeight(const Member &);
    // Seven slots of 112 px plus up to three 40 px effect lines (cap 23) each and 12 px gaps fit whole.
    static constexpr int GearListW = 748, GearEffectH = 40, GearListH = 8 + 7 * (112 + 3 * GearEffectH + 12);
    // The Party page's member list width (rows, Set plate); the profile card takes x 468..1216.
    static constexpr int ListW = 420;
};
} // namespace xcx
