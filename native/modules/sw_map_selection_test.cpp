// SPDX-License-Identifier: GPL-3.0-or-later
#include "sw_map_selection.h"
#include <cassert>
#include <iostream>
#include <limits>

using namespace sw_map_selection;
namespace {
sw_town_map::Table Map() {
    sw_town_map::Table map;
    map.areas = {{{0, 0, 1920, 4932}, {0, 0, 1540, 1540}, {0, 0, 1920, 2060}}};
    map.points = {
        {sw_fly::kDestinations[3].zone, 0, 911, 1713}, // Motostoke, two native landings
        {sw_town_map::ZoneHash("z_r0101"), 0, 977, 320}, // named Route1, no Fly landing
        {sw_fly::kDestinations[37].zone, 2, 500, 1200}, // Freezington
        {sw_town_map::ZoneHash("z_unknown"), 0, 100, 200},
    };
    return map;
}
Catalog Places() {
    Catalog result;
    // the names come from the player's romfs (LoadNames); the module has none of its own
    assert(Build(Map(), {{sw_fly::kDestinations[3].zone, "Motostoke"}, {sw_town_map::ZoneHash("z_r0101"), "Route 1"},
                         {sw_fly::kDestinations[37].zone, "Freezington"}}, result));
    assert(result.anchors.size() == 3); // unknown label is not guessed
    assert(result.anchors[0].name == "Motostoke"); // one anchor per zone, whatever its landings
    assert(result.anchors[1].name == "Route 1");
    assert(result.anchors[2].area == 2 && result.anchors[2].id == 2);
    return result;
}
// The two Fly zones without a place label of their own: Route 10's second landing zone shows
// z_r1001's name, Battle Tower its Town Map facility name (passed in; none -> not labelled).
void UnlabelledFlyZones() {
    sw_town_map::Table map;
    map.areas = {{{0, 0, 1920, 4932}, {0, 0, 1540, 1540}, {0, 0, 1920, 2060}}};
    map.points = {{sw_town_map::ZoneHash("z_r1002"), 0, 300, 300}, {sw_town_map::ZoneHash("z_c0306"), 0, 400, 400}};
    const std::vector<NamedZone> names{{sw_town_map::ZoneHash("z_r1001"), "Route 10"}};
    Catalog result;
    assert(Build(map, names, result, "Battle Tower") && result.anchors.size() == 2);
    assert(result.anchors[0].name == "Route 10" && result.anchors[1].name == "Battle Tower");
    assert(Build(map, names, result) && result.anchors.size() == 1 && result.anchors[0].name == "Route 10");
    assert(!Build(map, {}, result)); // nothing named
    // a landing's number among its zone's landings (the Fly list's " (2)")
    assert(sw_fly::kDestinations[3].number == 1 && sw_fly::kDestinations[4].number == 2 &&
           sw_fly::kDestinations[3].zone == sw_fly::kDestinations[4].zone);
    int later = 0;
    for (const auto& d : sw_fly::kDestinations) later += d.number > 1;
    assert(later == 10 && sw_fly::kDestinations[8].number == 3); // Hammerlocke (3)
    // Route 9's second landing is its own zone (z_r0904) shown as "Route 9 (2)"
    assert(sw_fly::kDestinations[24].number == 2 && sw_fly::kDestinations[24].zone != sw_fly::kDestinations[12].zone);
}
void Names() {
    std::vector<sw_text::TblEntry> labels{{"PLACENAME_Z_T0101_01", 0},
        {"PLACENAME_Z_R0101_01", 0}, {"PLACENAME_Z_R0101_02", 0},
        {"PLACENAME_Z_D0102_01", 0}, {"PLACENAME_Z_D0101_01", 0}};
    std::vector<std::string> lines{"in the town of Postwick", "on Route 1", "",
        "[~ 4]", "in the Slumbering Weald"};
    std::vector<NamedZone> names;
    assert(DecodeNames(labels, lines, names) && names.size() == 4);
    assert(names[0].zone_hash == sw_town_map::ZoneHash("z_t0101") && names[0].name == "Postwick");
    assert(names[1].name == "Route 1" && names[2].name == "Slumbering Weald");
    labels.push_back({"msg_place_name_indirect_max", 0});
    assert(DecodeNames(labels, lines, names) && names.size() == 4);
    labels.pop_back();
    std::string text = "untouched";
    assert(!ResolveLine({"[~ 1]", "[~ 0]"}, 0, text) && text == "untouched");
    assert(!ResolveLine({"[~ 99]"}, 0, text));
    assert(!ResolveLine({"[~ 0x]"}, 0, text));
    assert(!ResolveLine({"[VAR 0105]"}, 0, text));
    assert(!ResolveLine({"Unresolved\\nname"}, 0, text));
    assert(PlaceName("in the city of Wyndon") == "Wyndon");
    assert(PlaceName("at Wedgehurst Station") == "Wedgehurst Station");
    labels.push_back(labels[0]); lines.push_back("conflicting duplicate");
    assert(!DecodeNames(labels, lines, names));
    lines.pop_back();
    assert(!DecodeNames(labels, lines, names));
}
void SelectionAndPermission() {
    auto catalog = Places();
    Selection selected;
    assert(!selected.Get(catalog));
    assert(selected.Select(catalog, 0, 0));
    assert(selected.Get(catalog)->name == "Motostoke");
    const auto before = selected.index();
    assert(!selected.Select(catalog, -1, 0) && !selected.Select(catalog, 50, 0));
    assert(!selected.Select(catalog, 2, 0) && selected.index() == before);
    const auto both = (std::uint64_t{1} << 3) | (std::uint64_t{1} << 4);
    auto plan = Plan(catalog, selected, both, true, true);
    assert(plan.total == 2 && plan.indices == std::vector<int>({3, 4}));
    assert(plan.ready && plan.requires_choice && plan.single_index == -1);
    assert(selected.index() == before); // computing eligibility cannot commit/change selection
    plan = Plan(catalog, selected, std::uint64_t{1} << 4, true, true);
    assert(plan.ready && !plan.requires_choice && plan.single_index == 4);
    plan = Plan(catalog, selected, both, true, false); // indoors/refused here
    assert(!plan.ready && plan.indices.size() == 2);
    plan = Plan(catalog, selected, both, false, true); // query not yet trusted
    assert(!plan.ready && plan.indices.empty() && plan.total == 2);
    plan = Plan(catalog, selected, 0, true, true); // locked destination still selectable
    assert(!plan.ready && plan.total == 2 && selected.Get(catalog));
    assert(selected.Select(catalog, 0, 0) && selected.index() == -1); // second tap toggles off
    assert(selected.Select(catalog, 1, 0));
    plan = Plan(catalog, selected, ~std::uint64_t{}, true, true);
    assert(plan.total == 0 && !plan.ready && selected.Get(catalog)->name == "Route 1");
    selected.Reconcile(catalog, -1); // transient unreadable registry/player sample
    assert(selected.index() == 1 && selected.Get(catalog)->name == "Route 1");
    selected.Reconcile(catalog, 0); // verified same area returns without losing the destination
    assert(selected.index() == 1);
    selected.Reconcile(catalog, 2);
    assert(selected.index() == -1);
    assert(selected.Select(catalog, 2, 2));
    assert(Plan(catalog, selected, std::uint64_t{1} << 37, true, true).single_index == 37);
    selected.Clear();
    assert(!Plan(catalog, selected, ~std::uint64_t{}, true, true).ready);
    assert(selected.Select(catalog, 0, 0));
    catalog.anchors[0].zone_hash = catalog.anchors[1].zone_hash; // reloaded/reordered catalog
    assert(!selected.Get(catalog));
    assert(!Plan(catalog, selected, both, true, true).ready);
    selected.Reconcile(catalog, -1); // unknown area cannot preserve a replaced catalog identity
    assert(selected.index() == -1);
}
void InvalidGeometryAndMask() {
    Catalog untouched = Places();
    auto map = Map(); map.points[0].x = std::numeric_limits<float>::quiet_NaN();
    assert(!Build(map, {}, untouched) && untouched.anchors.size() == 3);
    map = Map(); map.points[0].area = 3;
    assert(!Build(map, {}, untouched));
    map = Map(); map.points[0].x = 1921;
    assert(!Build(map, {}, untouched));
    map = Map(); map.points.push_back(map.points[0]);
    assert(!Build(map, {}, untouched));
    assert(EligibleLandingIndices(0, ~std::uint64_t{}).empty());
    assert(EligibleLandingIndices(sw_fly::kDestinations[3].zone, std::uint64_t{1} << 63).empty());
    std::vector<NamedZone> names;
    assert(!LoadNames(EdenDsmodHostApi{}, names));
}
// Item F: why the Fly capsule is locked.
void FlyReasons() {
    auto catalog = Places();
    Selection sel;
    const auto both = (std::uint64_t{1} << 3) | (std::uint64_t{1} << 4);
    assert(Reason(Plan(catalog, sel, both, true, true)) == FlyReason::None);       // nothing selected
    assert(Reason(Plan(catalog, sel, both, true, false)) == FlyReason::NotHere);   // indoors
    assert(sel.Select(catalog, 0, 0));                                             // Motostoke: 2 stops
    assert(Reason(Plan(catalog, sel, both, true, true)) == FlyReason::Ready);
    assert(Reason(Plan(catalog, sel, 0, true, true)) == FlyReason::NotVisited);
    assert(Reason(Plan(catalog, sel, both, true, false)) == FlyReason::NotHere);
    assert(Reason(Plan(catalog, sel, both, false, true)) == FlyReason::None);      // query pending
    assert(sel.Select(catalog, 1, 0));                                             // Route 1: no stop
    assert(Reason(Plan(catalog, sel, ~std::uint64_t{}, true, true)) == FlyReason::NoStop);
    assert(Reason(Plan(catalog, sel, ~std::uint64_t{}, true, false)) == FlyReason::NoStop);
}
} // namespace
int main() {
    Names(); SelectionAndPermission(); UnlabelledFlyZones(); InvalidGeometryAndMask(); FlyReasons();
    std::cout << "Town Map selection/permission checks passed\n";
}
