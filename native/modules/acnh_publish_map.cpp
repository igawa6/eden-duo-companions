// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// ACNH companion publishing: the Map page (acnh_publish.h). Island raster submission, structure
// icons and their zoom bands, residents in the Map app's order, facility selection.

#include "acnh_publish.h"
#include "acnh_publish_detail.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include "acnh_bcsv.h"
#include "acnh_catalog.h"
#include "acnh_lang.h"
#include "acnh_lang_grammar.h"
#include "acnh_map.h"
#include "acnh_msbt.h"
#include "acnh_romfs.h"

namespace acnh {
using namespace pub;
namespace {

// the page's map-marker template boxes (one template per size, gen_manifest)
constexpr long IconBoxes[] = {32, 48, 64, 96, 128, 192, 256};
/// The smallest template box that holds `px` (the largest when none does).
long BoxFor(long px) {
    for (const long b : IconBoxes)
        if (b >= px)
            return b;
    return IconBoxes[std::size(IconBoxes) - 1];
}

/// The Map app's resident sort key (main 0x255eba0 -> 0x262eca4 -> nn::time::ToPosixTimeFromUtc):
/// BirthDate at 05:00 when nn::time::IsValidDate(y, m, d), else 2000-01-01 00:00. In hours.
int64_t BornKey(const live::LiveSnapshot::Resident& r) {
    static constexpr int Mdays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const int y = r.born_y, m = r.born_m, d = r.born_d;
    const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    const bool valid = m >= 1 && m <= 12 && d >= 1 && d <= Mdays[m - 1] + (m == 2 && leap);
    return valid ? DaysFromCivil(y, m, d) * 24 + 5 : DaysFromCivil(2000, 1, 1) * 24;
}

} // namespace

// ---- map pin (r9 indoor) -----------------------------------------------------------------------
// Outdoors (scene MainField) the pin is the player's world position. Inside a building the
// position is in room coordinates (LIVE: (160, 205) in the player's house), so the pin goes on that
// building's map icon centre, where the game's own Map app puts it (LIVE, research/acnh/impl/fix/
// r9-indoor.md: player house, Cole's house, Nook's Cranny, Able Sisters, ...): the scene's stage
// name -> its structure (live::SceneStructure) -> the icon BuildMap placed for it. Any other scene
// (dream, Redd's ship, tours, Kapp'n's island, HHP, photo studio) or a building without an icon:
// no pin (the marker skips a slot with no position).
void Publisher::PublishPin(const EdenDsmodHostApi& host, const live::LiveSnapshot& s) {
    if (!s.pos_ok || !host.publish_f64)
        return;
    const std::string_view scene = s.stage_name;
    double x = s.px, z = s.pz;
    if (scene != "MainField") {
        const auto a = pin_anchor.find(live::SceneStructure(scene));
        if (a == pin_anchor.end())
            return;
        x = a->second.first, z = a->second.second;
    }
    host.publish_f64(host.userdata, "player.x", x);
    host.publish_f64(host.userdata, "player.z", z);
}

// ---- map page --------------------------------------------------------------------------------
void Publisher::BuildMap(const live::LiveSnapshot& s) {
    Catalog* cat = services.catalog;
    // island raster for the map lane
    // the submission revision also follows the hotel stage: the zoom map pictures draw the hotel
    // and its pier art into the island (mapzoom lane), so a stage change re-renders them
    const uint32_t map_rev = s.island_rev * 4u + (static_cast<uint32_t>(xs.hotel_state + 1) & 3u);
    if (s.island_rev && map_rev != island_rev_sent &&
        s.land_making.size() == live::M::LandMakingSize &&
        s.structures.size() == live::M::StructureSize &&
        s.field_blocks.size() == live::M::FieldBlockSize) {
        IslandInput in;
        in.land_making = s.land_making;
        in.structures = s.structures;
        in.field_blocks = s.field_blocks;
        if (xs.houses_ok)
            in.npc_houses = xs.npc_houses;
        in.hotel_state = xs.hotel_state;
        MapSubmitIsland(map_rev, std::move(in));
        island_rev_sent = map_rev;
    }
    uint64_t key = Mix(0x3a9, static_cast<uint64_t>(lang + 1));
    key = Mix(key, island_rev_sent);
    key = MixBytes(key, s.structures.data(), s.structures.size());
    key = MixBytes(key, s.field_blocks.data(), s.field_blocks.size());
    for (const auto& r : s.residents) {
        key = Mix(key, (uint64_t(r.present) << 24) | (uint64_t(r.species) << 8) | r.variant);
        key = Mix(key, (uint64_t(r.born_ok) << 32) | (uint64_t(r.born_y) << 16) |
                           (uint64_t(r.born_m) << 8) | r.born_d);
    }
    key = Mix(key, s.order_ok);
    key = Mix(key, xs.houses_rev); // not the 48 KB every sample: the reader bumps it on a change
    key = Mix(key, static_cast<uint64_t>(xs.hotel_state + 2));
    key = Mix(key, (uint64_t(xs.office_construction1 + 1) << 48) |
                       (uint64_t(xs.market_construction2 + 1) << 32) |
                       (uint64_t(xs.museum_construction[0] + 1) << 16) |
                       uint64_t(xs.museum_construction[1] + 1));
    key = Mix(key, static_cast<uint64_t>(xs.museum_construction[2] + 1));
    for (const auto& pl : xs.players) {
        key = Mix(key, (uint64_t(pl.no + 1) << 32) | uint32_t(pl.days + 1));
        key = MixStr(key, Utf16ToUtf8(pl.name));
    }
    key = Mix(key, (uint64_t(xs.shop_level_map + 1) << 48) | (uint64_t(xs.tailor_level + 1) << 32) |
                       (uint64_t(xs.museum_level + 1) << 16) | uint64_t(xs.camp_level + 1));
    key = Mix(key, static_cast<uint64_t>(s.player_no + 1));
    key = MixStr(key, Utf16ToUtf8(s.island_name));
    key = MixStr(key, Utf16ToUtf8(s.player_name));
    // the player pin changes every frame: published outside the cache
    if (o_map.key != key) {
        o_map.Clear();
        o_map.key = key;
        const std::string island = Utf16ToUtf8(s.island_name);
        o_map.T("island.name", island);
        o_map.I("island.name.w", widths.Measure(island));
        o_map.I("island.rev", island_rev_sent);
        if (island_rev_sent) {
            const std::string isl =
                std::string{Key} + "map/island/" + std::to_string(island_rev_sent);
            // zoom page (mapzoom lane, acnh_map.h "zoomable page view"): base + 2x / 3x tiles; tile
            // t's top-left corner in world units (map widget: x = X, y = -Z)
            o_map.T("map.base.key", isl + "?z=1");
            o_map.T("map.pin.key", std::string{Key} + "map/icon/player?w=44&h=59&s=64");
            for (int t = 0; t < kZoomTilesX * kZoomTilesY; ++t) {
                const std::string p = "map.t." + std::to_string(t) + ".";
                o_map.T(p + "k2", isl + "?z=2&t=" + std::to_string(t));
                o_map.T(p + "k3", isl + "?z=3&t=" + std::to_string(t));
                o_map.I(p + "x",
                        std::lround(kZoomX0 + kZoomTile * static_cast<float>(t % kZoomTilesX)));
                o_map.I(p + "y",
                        std::lround(kZoomZ0 + kZoomTile * static_cast<float>(t / kZoomTilesX)));
            }
        }
        o_map.T("player.name", Utf16ToUtf8(s.player_name));
        // structure icons (map lane: MapIcons positions in raster px, per-icon image key + size;
        // villager house owner + level from NpcHouseList, hotel stage from the Land flags)
        IslandInput in;
        in.structures = s.structures;
        in.field_blocks = s.field_blocks;
        if (xs.houses_ok)
            in.npc_houses = xs.npc_houses;
        in.hotel_state = xs.hotel_state;
        std::map<int, std::pair<float, float>> house_of_owner; // villager slot -> its house icon
        std::map<int, int> house_stage;                        // villager slot -> its house level
        std::map<long, int> per_box;                           // zoom page: icons per box size
        const auto all_icons = MapIcons(in, kMapView, services.romfs);
        // r9 indoor: where the pin goes while the player is inside a building (PublishPin)
        pin_anchor.clear();
        for (const auto& ic : all_icons)
            if (ic.structure_id != 0 && !pin_anchor.contains(ic.structure_id))
                pin_anchor[ic.structure_id] = {ic.x / kMapView.px_per_unit + kMapView.x0,
                                               ic.y / kMapView.px_per_unit + kMapView.z0};
        for (const auto& ic : all_icons) {
            if (ic.kind == MapIconKind::NpcHouse && ic.owner >= 0) {
                house_of_owner[ic.owner] = {ic.x, ic.y};
                house_stage[ic.owner] = ic.stage;
            }
            // zoom page: the key sized to the zoom map's scale (kZoomPpu canvas px per world unit)
            // in a list per box size (one map-marker template each), the centre in tenths of world
            // units (raster px / 0.625 + 64) r7 mapfix: one key per zoom band (acnh_map.h "icon
            // zoom bands"): k<b> = the icon at IconBandScale(b) x its zoom-1 size in a box of the
            // zoom-1 box x the same factor. The hotel's pier art is in the island pictures, its
            // icon a marker like the others.
            if (!ic.key.empty()) {
                const double zk = kZoomPpu / kMapView.px_per_unit;
                const bool hotel = ic.kind == MapIconKind::Hotel;
                const double iw = hotel ? ic.w * kHotelIconOfComposite[0] : ic.w;
                const double ih = hotel ? ic.h * kHotelIconOfComposite[1] : ic.h;
                const long zw = std::lround(iw * zk), zh = std::lround(ih * zk);
                const long zbox = BoxFor(std::max(zw, zh));
                const int j = per_box[zbox]++;
                const std::string q =
                    "map.ib." + std::to_string(zbox) + "." + std::to_string(j) + ".";
                const std::string base = std::string{Key} + ic.key +
                                         (ic.key.find('?') != std::string::npos ? "&" : "?") +
                                         (hotel ? "part=1&" : "");
                for (int b = 0; b < kIconBands; ++b) {
                    const double f = IconBandScale(b);
                    const std::string k = base + "w=" + std::to_string(std::lround(iw * zk * f)) +
                                          "&h=" + std::to_string(std::lround(ih * zk * f)) +
                                          "&s=" + std::to_string(std::lround(zbox * f));
                    o_map.T(q + "k" + std::to_string(b), k);
                }
                o_map.I(q + "x", std::lround((ic.x / kMapView.px_per_unit + kMapView.x0) * 10.f));
                o_map.I(q + "y", std::lround((ic.y / kMapView.px_per_unit + kMapView.z0) * 10.f));
            }
        }
        // residents in the Map app's order (valid villagers in slot order, stable-sorted by
        // BirthDate: main 0x255eba0); empty slots have no NmlNpcParam row. Slot order when the
        // order code is not the pinned one.
        const Bcsv* race = cat ? cat->Table("NmlNpcRaceParam") : nullptr;
        std::vector<std::pair<size_t, std::string>> valid; // slot, label
        for (size_t slot = 0; slot < s.residents.size(); ++slot) {
            const auto& r = s.residents[slot];
            if (!r.present || !race || r.species >= race->Rows())
                continue;
            char label[24];
            std::snprintf(label, sizeof label, "%s%02d",
                          race->Str(r.species, Bcsv::Key("Label", "string8")).c_str(), r.variant);
            if (cat->FindVillager(label))
                valid.emplace_back(slot, label);
        }
        bool game_order = s.order_ok;
        for (const auto& v : valid)
            game_order = game_order && s.residents[v.first].born_ok;
        if (game_order)
            std::stable_sort(valid.begin(), valid.end(), [&](const auto& a, const auto& b) {
                return BornKey(s.residents[a.first]) < BornKey(s.residents[b.first]);
            });

        // ---- zoom page selections (mapzoom lane)
        // ------------------------------------------------- The Map app's resident list
        // (0x2e1a5e0..0x2e1ad00): the players first, oldest first (PastDaysFromMade descending,
        // account-table order on a tie: sort 0x256b370), each pointing at its house icon, then the
        // villagers in move-in order whose house icon is shown (the villager list above minus any
        // without a house). Up to 18 entries; the page lays them out by count like
        // MenuMapFull_TypeResident. Entry e = map selection slot e (0..17); facility name plates
        // are slots 20.. (the building column's facilities: SetTarget 0x2e175f0).
        const std::string folder =
            lang >= 0 ? std::string{LangFolder(static_cast<Lang>(lang))} : "USen";
        const char* face = folder == "KRko"   ? "ui_kr"
                           : folder == "CNzh" ? "ui_cn"
                           : folder == "TWzh" ? "ui_tw"
                                              : "ui";
        auto enc = [](const std::string& v) {
            std::string o;
            for (const unsigned char ch : v) {
                if (std::isalnum(ch)) {
                    o.push_back(static_cast<char>(ch));
                } else {
                    char hx[4];
                    std::snprintf(hx, sizeof hx, "%%%02X", ch);
                    o += hx;
                }
            }
            return o;
        };
        // player houses: icon of StructureList uid 1 + player number (PlayerHouse0-7, LEAD: the map
        // record of player i = map+0x508+i*0x68, 0x2a5e950)
        std::map<int, std::pair<float, float>> player_house;
        for (const auto& ic : all_icons)
            if (ic.kind == MapIconKind::PlayerHouse && ic.structure_id >= 1 && ic.structure_id <= 8)
                player_house[ic.structure_id - 1] = {ic.x, ic.y};
        struct Entry {
            std::string name, icon, selkey;
            bool house = false;
            float wx = 0, wz = 0;
            std::string fac_head; // a houseless player: map/facsel of the Resident Services icon
            double fw = 0, fh = 0;
        };
        std::vector<Entry> entries;
        auto world = [](std::pair<float, float> px, Entry& e) {
            e.house = true;
            e.wx = px.first / kMapView.px_per_unit + kMapView.x0;
            e.wz = px.second / kMapView.px_per_unit + kMapView.z0;
        };
        std::vector<live::ExtraSnapshot::MapPlayer> players = xs.players;
        if (players.empty() && s.player_no >= 0) { // account table not read: the local player alone
            live::ExtraSnapshot::MapPlayer me;
            me.no = s.player_no;
            me.name = s.player_name;
            players.push_back(me);
        }
        std::stable_sort(players.begin(), players.end(),
                         [](const auto& a, const auto& b) { return a.days > b.days; });
        for (const auto& pl : players) {
            Entry e;
            e.name = Utf16ToUtf8(pl.no == s.player_no && !s.player_name.empty() ? s.player_name
                                                                                : pl.name);
            // the local player's passport photo; another player's face is a runtime render in the
            // game (LPlayerIcon): its house icon stands in (LEAD, no multi-player save to check)
            e.icon = pl.no == s.player_no
                         ? std::string{Key} + "photo/passport?d=76&disc=1"
                         : std::string{Key} + "map/icon/playerhouse?w=60&h=42&s=86";
            e.selkey = std::string{Key} + "map/ressel?n=" + enc(e.name) + "&f=" + face + "&h=1";
            if (const auto h = player_house.find(pl.no); h != player_house.end()) {
                world(h->second, e);
            } else {
                // r7 mapfix (LIVE, 2-player test save without a second house): the Map app points a
                // houseless player's entry at the Resident Services icon (it blinks, the player's
                // name balloon over it; code 0x2e1a870..0x2e1a970 not decoded: LEAD for the RS
                // tent)
                for (const MapIconKind fk : {MapIconKind::Office, MapIconKind::InfoTent}) {
                    const auto it = std::find_if(all_icons.begin(), all_icons.end(),
                                                 [&](const MapIcon& ic) { return ic.kind == fk; });
                    if (it == all_icons.end())
                        continue;
                    world({it->x, it->y}, e);
                    const double zk = kZoomPpu / kMapView.px_per_unit;
                    e.fw = it->w * zk;
                    e.fh = it->h * zk;
                    e.fac_head = std::string{Key} + "map/facsel?k=" + std::string{MapIconName(fk)} +
                                 "&n=" + enc(e.name) + "&f=" + face;
                    e.selkey = e.fac_head + "&w=" + std::to_string(std::lround(e.fw)) +
                               "&h=" + std::to_string(std::lround(e.fh));
                    break;
                }
            }
            entries.push_back(std::move(e));
        }
        for (const auto& [slot, label] : valid) {
            const auto h = house_of_owner.find(static_cast<int>(slot));
            if (h == house_of_owner.end())
                continue; // the Map app lists a villager only with its house icon shown
            Entry e;
            e.name = cat->VillagerName(static_cast<Lang>(std::max(lang, 0)), label);
            e.icon = std::string{Key} + "icon/npc/" + label + "?s=86";
            world(h->second, e);
            const auto st = house_stage.find(static_cast<int>(slot));
            e.selkey = std::string{Key} + "map/ressel?n=" + enc(e.name) + "&f=" + face +
                       "&st=" + std::to_string(st != house_stage.end() ? st->second : -1);
            entries.push_back(std::move(e));
        }
        if (entries.size() > 18)
            entries.resize(18);
        o_map.I("mres.n", static_cast<int64_t>(entries.size()));
        for (size_t e = 0; e < entries.size(); ++e) {
            const Entry& en = entries[e];
            const std::string p = "mres." + std::to_string(e) + ".";
            o_map.T(p + "name", en.name);
            o_map.T(p + "icon", en.icon);
            if (!en.house)
                continue;
            const std::string q = "map.sel." + std::to_string(e) + ".";
            o_map.I(q + "x", std::lround(en.wx * 10.f));
            o_map.I(q + "y", std::lround(en.wz * 10.f));
            // r7 mapfix: the highlight per zoom band (hk<b>) and the plate anchored above the icon
            // (pk)
            for (int b = 0; b < kIconBands; ++b) {
                const double f = IconBandScale(b);
                o_map.T(q + "hk" + std::to_string(b),
                        en.fac_head.empty()
                            ? en.selkey + "&part=1&sc=" + std::to_string(std::lround(f * 100.0))
                            : en.fac_head + "&w=" + std::to_string(std::lround(en.fw * f)) +
                                  "&h=" + std::to_string(std::lround(en.fh * f)) + "&part=1");
            }
            o_map.T(q + "pk", en.selkey + "&part=2&cap=0");
            const double half = en.fac_head.empty() ? 36 * kZoomPpu / 0.8 * 1.3 / 2 : en.fh / 2;
            o_map.I(q + "py", std::lround((en.wz - half / kZoomPpu) * 10));
            o_map.I("map.hit." + std::to_string(e) + ".x", std::lround(en.wx * 10));
            o_map.I("map.hit." + std::to_string(e) + ".y", std::lround(en.wz * 10));
            o_map.T("map.hit." + std::to_string(e) + ".key", std::string{Key} + "map/none");
            // the view a selection frames: the house centred at 2x (580 x 500 world units, map y =
            // -Z)
            o_map.I(p + "rx0", std::lround(en.wx - kZoomTile));
            o_map.I(p + "rx1", std::lround(en.wx + kZoomTile));
            o_map.I(p + "ry0", -std::lround(en.wz + kZoomH / 4.f));
            o_map.I(p + "ry1", -std::lround(en.wz - kZoomH / 4.f));
        }
        // facilities in the building column's order (static table 0x55bb048: Resident Services,
        // RS tent, Nook's Cranny, Able Sisters, Museum, Campsite, Airport, Hotel), each present one
        // a tappable map spot (map.hit.*) and a selection picture with its STR_Common name by stage
        // (0x2e21adc, table 0x55bb290, incl. the renovation / "(const.)" variants:
        // MapFacilityNameLabel)
        static constexpr MapIconKind kFacOrder[] = {
            MapIconKind::Office, MapIconKind::InfoTent, MapIconKind::Market,  MapIconKind::Tailor,
            MapIconKind::Museum, MapIconKind::Camp,     MapIconKind::Airport, MapIconKind::Hotel};
        // r7 mapfix: the name rule of the game's code (MapFacilityNameLabel, acnh_map.h); the hotel
        // flags from the live hotel stage, the renovation flags not read yet (LEAD: live lane)
        FacilityFlags fflags;
        fflags.hotel_construction = xs.hotel_state == 1 ? 1 : xs.hotel_state >= 0 ? 0 : -1;
        fflags.hotel_built = xs.hotel_state == 2 ? 1 : xs.hotel_state >= 0 ? 0 : -1;
        fflags.office_construction1 = xs.office_construction1;
        fflags.market_construction2 = xs.market_construction2;
        fflags.museum_construction1 = xs.museum_construction[0];
        fflags.museum_construction2 = xs.museum_construction[1];
        fflags.museum_construction3 = xs.museum_construction[2];
        auto fac_label = [&](MapIconKind k) -> std::string {
            const int stage = k == MapIconKind::Market   ? xs.shop_level_map
                              : k == MapIconKind::Museum ? xs.museum_level
                              : k == MapIconKind::Tailor ? xs.tailor_level
                              : k == MapIconKind::Camp   ? xs.camp_level
                                                         : -1;
            return MapFacilityNameLabel(k, stage, fflags);
        };
        int fslot = 20;
        for (const MapIconKind fk : kFacOrder) {
            for (const auto& ic : all_icons) {
                if (ic.kind != fk)
                    continue;
                const std::string label = fac_label(fk);
                const std::string name =
                    label.empty() ? std::string{} : Msg("String/STR_Common", label);
                const double zk = kZoomPpu / kMapView.px_per_unit; // the zoom map's icon scale
                const std::string head = std::string{Key} +
                                         "map/facsel?k=" + std::string{MapIconName(fk)} +
                                         "&n=" + enc(name) + "&f=" + face;
                std::string tail;
                if (fk == MapIconKind::Hotel) {
                    tail += "&side=" + std::to_string(MapPierSide(s.field_blocks));
                    if (xs.hotel_state >= 0)
                        tail += "&stage=" + std::to_string(xs.hotel_state);
                }
                const std::string q = "map.sel." + std::to_string(fslot) + ".";
                const long x10 = std::lround((ic.x / kMapView.px_per_unit + kMapView.x0) * 10.f);
                const long y10 = std::lround((ic.y / kMapView.px_per_unit + kMapView.z0) * 10.f);
                o_map.I(q + "x", x10);
                o_map.I(q + "y", y10);
                // r7 mapfix: the highlighted icon per zoom band (hk<b>, the hotel's icon alone) and
                // the name plate anchored above the icon (pk)
                const bool hotel = fk == MapIconKind::Hotel;
                const double iw = (hotel ? ic.w * kHotelIconOfComposite[0] : ic.w) * zk;
                const double ih = (hotel ? ic.h * kHotelIconOfComposite[1] : ic.h) * zk;
                for (int b = 0; b < kIconBands; ++b) {
                    const double f = IconBandScale(b);
                    o_map.T(q + "hk" + std::to_string(b),
                            head + "&w=" + std::to_string(std::lround(iw * f)) +
                                "&h=" + std::to_string(std::lround(ih * f)) + tail + "&part=1");
                }
                o_map.T(q + "pk", head + "&w=" + std::to_string(std::lround(iw)) + "&h=" +
                                      std::to_string(std::lround(ih)) + tail + "&part=2&cap=0");
                o_map.I(q + "py", std::lround((ic.y / kMapView.px_per_unit + kMapView.z0 -
                                               ih / (2 * kZoomPpu)) *
                                              10));
                const std::string h = "map.hit." + std::to_string(fslot) + ".";
                o_map.I(h + "x", x10);
                o_map.I(h + "y", y10);
                o_map.T(h + "key", std::string{Key} + "map/none");
                ++fslot;
                break; // one icon per facility kind
            }
        }
    }
}

} // namespace acnh
