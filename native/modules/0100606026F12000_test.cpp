// FINAL FANTASY RESONANCE companion: synthetic checks (no game files needed).
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/mods/dsmod_module_abi.h"
#include "ffr_assets.h"
#include "ffr_pages.h"

extern "C" const EdenDsmodModuleApi* eden_dsmod_get_module(uint32_t v, uint64_t h);

namespace {

int g_checks = 0;
#define CHECK(x) (++g_checks, assert(x))

void Put32(std::vector<uint8_t>& b, int32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(uint8_t(uint32_t(v) >> (8 * i)));
}
void PutStr(std::vector<uint8_t>& b, const std::string& s) {  // FString, ANSI, NUL-terminated
    Put32(b, int32_t(s.size() + 1));
    b.insert(b.end(), s.begin(), s.end());
    b.push_back(0);
}

void Abi() {
    CHECK(eden_dsmod_get_module(EDEN_DSMOD_MODULE_ABI_VERSION + 1, EDEN_DSMOD_MODULE_ABI_HASH) == nullptr);
    CHECK(eden_dsmod_get_module(EDEN_DSMOD_MODULE_ABI_VERSION, EDEN_DSMOD_MODULE_ABI_HASH ^ 1) == nullptr);
    const EdenDsmodModuleApi* api = eden_dsmod_get_module(EDEN_DSMOD_MODULE_ABI_VERSION, EDEN_DSMOD_MODULE_ABI_HASH);
    CHECK(api && api->title_id == 0x0100606026F12000ull);
    CHECK(api->supports_build("84E182F72EE1DA4F477804BD05B54C1500000000000000000000000000000000") == EDEN_DSMOD_TRUE);
    CHECK(api->supports_build("84E182F72EE1DA4E477804BD05B54C1500000000000000000000000000000000") == EDEN_DSMOD_FALSE);
    CHECK(api->supports_build("") == EDEN_DSMOD_FALSE);
    CHECK(api->supports_build(nullptr) == EDEN_DSMOD_FALSE);
}

void Strings() {
    std::vector<uint8_t> b;
    PutStr(b, "Rain");
    Put32(b, -2);  // UTF-16: U+00E9, NUL
    b.insert(b.end(), {0xE9, 0x00, 0x00, 0x00});
    size_t o = 0;
    std::string s;
    CHECK(ffr::ReadFString(b.data(), b.size(), o, s) && s == "Rain" && o == 9);
    CHECK(ffr::ReadFString(b.data(), b.size(), o, s) && s == "\xC3\xA9" && o == b.size());
    std::vector<uint8_t> bad;
    Put32(bad, 100);  // longer than the buffer
    o = 0;
    CHECK(!ffr::ReadFString(bad.data(), bad.size(), o, s));
    bad.clear();
    Put32(bad, -(1 << 21));
    o = 0;
    CHECK(!ffr::ReadFString(bad.data(), bad.size(), o, s));
}

void Locres() {
    // Version 2: 16-byte magic, version, string-table offset, namespaces with keys, then the strings.
    std::vector<uint8_t> b(16, 0x0E);
    b.push_back(2);
    const size_t off_at = b.size();
    b.resize(b.size() + 8);
    Put32(b, 1);  // namespaces
    Put32(b, 0), PutStr(b, "ST_VisionName"), Put32(b, 2);
    Put32(b, 0), PutStr(b, "VisionName13017"), Put32(b, 0), Put32(b, 0);
    Put32(b, 0), PutStr(b, "VisionName13024"), Put32(b, 0), Put32(b, 7);  // out of range: skipped
    const uint64_t off = b.size();
    std::memcpy(b.data() + off_at, &off, 8);
    Put32(b, 1), PutStr(b, "Amelia"), Put32(b, 1);
    std::unordered_map<std::string, std::string> loc;
    CHECK(ffr::ParseLocres(b, loc));
    CHECK(loc.size() == 1 && loc["ST_VisionName\x1fVisionName13017"] == "Amelia");
    b[16] = 9;  // unknown version
    loc.clear();
    CHECK(!ffr::ParseLocres(b, loc));
    std::vector<uint8_t> tiny(20, 0);
    CHECK(!ffr::ParseLocres(tiny, loc));
}

ffr::GameState Sample() {
    ffr::GameState s;
    s.in_game = s.save_ok = true;
    s.party = {1002, 1001};
    for (int id : {1001, 1002, 1003, 1004}) {
        ffr::UnitState u;
        u.id = id, u.hp = 100, u.mp = 10, u.weapon = 10100, u.armor = 11000, u.join = id == 1004 ? 1 : 0;
        s.units.push_back(u);
    }
    s.units[0].vision = 13017;
    s.inventory = {{13017, 1}, {1, 4}, {10100, 1}, {30004, 1}, {20001, 1}, {11000, 0}};
    return s;
}

void Lists() {
    const ffr::GameState s = Sample();
    auto r = ffr::Roster(s);  // the party in order, then the others who joined (1004 has not)
    CHECK(r.size() == 3 && r[0]->id == 1002 && r[1]->id == 1001 && r[2]->id == 1003);
    auto eq = ffr::ItemsOf(s, 1);  // visions are equipment; an empty stack is left out
    CHECK(eq.size() == 2 && eq[0].id == 10100 && eq[1].id == 13017);
    CHECK(ffr::ItemsOf(s, 0).size() == 1 && ffr::ItemsOf(s, 2).size() == 1 && ffr::ItemsOf(s, 3).size() == 1);
}

void Hash() {
    const ffr::GameState s = Sample();
    const uint64_t h = s.Hash();
    CHECK(Sample().Hash() == h);
    auto changed = [&](auto edit) { ffr::GameState t = Sample(); edit(t); return t.Hash() != h; };
    CHECK(changed([](ffr::GameState& t) { t.units[1].vision = 13024; }));
    CHECK(changed([](ffr::GameState& t) { t.units[0].accessories = {12000}; }));
    CHECK(changed([](ffr::GameState& t) { t.units[0].stats[2] = 50; }));
    CHECK(changed([](ffr::GameState& t) { t.inventory[1].num = 3; }));
    CHECK(changed([](ffr::GameState& t) { t.guest = 1016; }));
}

ffr::BattleUnit Unit(int id, bool enemy, uint64_t unit) {
    ffr::BattleUnit b;
    b.id = id, b.enemy = enemy, b.hp = b.max_hp = 100, b.unit = unit;
    return b;
}

ffr::GameState Battle() {
    ffr::GameState s = Sample();
    s.in_battle = true;
    s.battle = {Unit(5, true, 10), Unit(6, true, 11), Unit(1001, false, 1), Unit(1002, false, 2), Unit(1016, false, 3)};  // 1016: a guest
    s.menu.valid = s.menu.open = true;
    s.menu.actor = 1001;
    s.menu.entries = {{1, 1}, {1, 400010}, {1, 7}, {2, 1}};
    s.menu.cursor = 0;
    return s;
}

// Every point of the battle page, hit-tested: what each kind and index is reachable at.
std::vector<std::vector<bool>> Reach(const ffr::GameState& s) {
    std::vector<std::vector<bool>> seen(5, std::vector<bool>(16, false));
    for (int y = ffr::kBattleY; y < ffr::kBattleY + ffr::kBattleH; y += 4)
        for (int x = 0; x < ffr::kW; x += 4) {
            const ffr::BattleTap t = ffr::BattleHit(s, x, y);
            if (t.kind != ffr::BattleTap::kNone) {
                assert(t.index >= -1 && t.index < 16);
                if (t.index >= 0) seen[size_t(t.kind)][size_t(t.index)] = true;
            }
        }
    return seen;
}

void Taps() {
    ffr::GameState s = Battle();
    auto seen = Reach(s);
    for (size_t i = 0; i < s.menu.entries.size(); ++i) CHECK(seen[ffr::BattleTap::kEntry][i]);
    for (size_t i = 0; i < s.battle.size(); ++i) CHECK(seen[ffr::BattleTap::kUnit][i]);
    // The resonance wheel: enabled choices with a direction are tappable, a disabled one is not.
    s.menu.state = 2, s.menu.entries.clear();
    ffr::BattleMenu::Finish a, b, c;
    a.enable = true, a.dir = 0, b.enable = true, b.dir = 2, c.enable = false, c.dir = 1;
    s.menu.finish = {a, b, c};
    seen = Reach(s);
    CHECK(seen[ffr::BattleTap::kFinish][0] && seen[ffr::BattleTap::kFinish][1] && !seen[ffr::BattleTap::kFinish][2]);
    CHECK(!seen[ffr::BattleTap::kEntry][0]);
    s.in_battle = false;  // nothing on the page reacts outside a battle
    CHECK(ffr::BattleHit(s, ffr::kW / 2, ffr::kBattleY + ffr::kBattleH / 2).kind == ffr::BattleTap::kNone);
}

void Pages() {
    // No game files: no art and fonts that failed to load. Every page still draws, opaque.
    ffr::Font none;
    ffr::Art art;
    art.image = [](const std::string&) -> const ffr::Image* { return nullptr; };
    art.text = [](const std::string&, const std::string&) { return std::string(); };
    art.regular = art.bold = art.title = &none;
    ffr::GameState title;
    const ffr::GameState s = Sample(), btl = Battle();
    std::vector<std::pair<const ffr::GameState*, ffr::View>> views;
    for (int tab = 0; tab < ffr::kTabCount; ++tab) {
        ffr::View v;
        v.tab = tab;
        views.push_back({&s, v});
        views.push_back({&btl, v});
    }
    ffr::View detail;
    detail.tab = ffr::kTabParty, detail.member = 0;
    views.push_back({&s, detail});
    detail.member = 99;  // out of range: the list
    views.push_back({&s, detail});
    for (int cat = 0; cat < 4; ++cat) {
        ffr::View v;
        v.tab = ffr::kTabItems, v.item_cat = cat, v.item_sel = 0, v.item_scroll = 5;
        views.push_back({&s, v});
    }
    views.push_back({&title, ffr::View{}});
    for (auto& [state, v] : views) {
        ffr::Canvas c(ffr::kW, ffr::kH);
        ffr::ComposePage(c, *state, v, art);
        bool opaque = true;
        for (size_t i = 3; i < c.pixels().size(); i += 4 * 997) opaque = opaque && c.pixels()[i] == 255;
        CHECK(opaque);
    }
}

}  // namespace

int main() {
    Abi();
    Strings();
    Locres();
    Lists();
    Hash();
    Taps();
    Pages();
    std::printf("ffr: %d checks passed\n", g_checks);
    return 0;
}
