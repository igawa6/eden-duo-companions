// SPDX-FileCopyrightText: Copyright 2026 jadesigns.
// SPDX-License-Identifier: GPL-3.0-or-later
// SO2R 0.9.38: targeted tests for the minimap markers -- the destination "!" (DestTrack, its relative-coordinate
// fallback), the world map's list geometry (ListGeo) and the minimap state (MiniFromState). It compiles the module's
// own source (diag build) and drives the real ReadMarks / EndMarks / WorldIconCache against a small fake guest
// memory laid out like the game's (the offsets the module reads).
//
//   clang++ -std=c++20 -O1 -DSO2R_DIAG=1 -I<eden>/src -Inative/modules/so2r \
//       native/modules/so2r/so2r_marks_test.cpp -o so2r_marks_test && ./so2r_marks_test
//
// These are synthetic: they check the logic, not the game. The live behaviour is checked on the Thor.
#include <cstdio>
#include <cstring>
#include <vector>

#include "010065301A2E0000.cpp"

namespace t {
int fails = 0, checks = 0;
#define T_CHECK(c) do { ++t::checks; if (!(c)) { ++t::fails; std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

// ---- a fake guest memory --------------------------------------------------------------------------------------
std::vector<uint8_t> mem(1 << 22);
constexpr uint64_t kBase = 0x10000000;
uint64_t top = kBase + 0x100;
uint64_t alloc(size_t n) { const uint64_t a = (top + 15) & ~uint64_t(15); top = a + n; return a; }
template <class V> void put(uint64_t a, V v) { std::memcpy(&mem[a - kBase], &v, sizeof v); }
uint64_t rd64(uint64_t a) { uint64_t v; std::memcpy(&v, &mem[a - kBase], 8); return v; }
EdenDsmodBool read(void*, uint64_t a, void* o, size_t n) {
    if (a < kBase || a + n > kBase + mem.size()) return EDEN_DSMOD_FALSE;
    std::memcpy(o, &mem[a - kBase], n);
    return EDEN_DSMOD_TRUE;
}

// one minimap icon: type, used, its GameImage colour (alpha), its native transform's local position
struct Icon { uint64_t ip, trs, gi; };
Icon icon(int type, float lx, float ly, float alpha = 1) {
    Icon c{};
    c.ip = alloc(0x190);
    put<int32_t>(c.ip + kIpType, type); put<uint8_t>(c.ip + kIpUsed, 1);
    c.gi = alloc(0x40);
    put<float>(c.gi + kGraphicColor, 1); put<float>(c.gi + kGraphicColor + 4, 1); put<float>(c.gi + kGraphicColor + 8, 1);
    put<float>(c.gi + kGraphicColor + 12, alpha);
    put<uint64_t>(c.ip + 0xF0, c.gi);
    const uint64_t nat = alloc(0x40), gobj = alloc(0x80), comps = alloc(0x20), tr = alloc(0x50), hier = alloc(0x30), par = alloc(0x10);
    c.trs = alloc(0x60);
    put<uint64_t>(c.ip + 0x10, nat); put<uint64_t>(nat + 0x30, gobj); put<uint64_t>(gobj + 0x30, comps);
    put<uint64_t>(comps + 8, tr); put<uint64_t>(tr + 0x38, hier); put<int32_t>(tr + 0x40, 0);
    put<uint64_t>(hier + 0x18, c.trs); put<uint64_t>(hier + 0x20, par); put<int32_t>(par, -1);
    put<float>(c.trs, lx); put<float>(c.trs + 4, ly);
    return c;
}
void move_icon(const Icon& c, float lx, float ly) { put<float>(c.trs, lx); put<float>(c.trs + 4, ly); }
void alpha(const Icon& c, float a) { put<float>(c.gi + kGraphicColor + 12, a); }
void used(const Icon& c, bool u) { put<uint8_t>(c.ip + kIpUsed, u ? 1 : 0); }
// a presenter whose icon list holds these icons
uint64_t presenter(std::initializer_list<const Icon*> icons) {
    const uint64_t pres = alloc(0x190), list = alloc(0x20), arr = alloc(0x20 + 8 * icons.size());
    put<uint64_t>(pres + kMpIconList, list); put<uint64_t>(list + kListItems, arr); put<int32_t>(list + kListSize, (int32_t)icons.size());
    int i = 0;
    for (const Icon* c : icons) put<uint64_t>(arr + kArrItems + 8 * i++, c->ip);
    return pres;
}
void set_list(uint64_t pres, std::initializer_list<const Icon*> icons) {
    const uint64_t list = rd64(pres + kMpIconList), arr = alloc(0x20 + 8 * icons.size());
    put<uint64_t>(list + kListItems, arr); put<int32_t>(list + kListSize, (int32_t)icons.size());
    int i = 0;
    for (const Icon* c : icons) put<uint64_t>(arr + kArrItems + 8 * i++, c->ip);
}
// what ReadMap does each pass with UIFieldController.minimapState (0.9.39)
void state_pass(MapState& m, int st) { m.mstate_run = st == m.mstate ? m.mstate_run + 1 : 0; m.mstate = st; }
EdenDsmodHostApi host() { EdenDsmodHostApi h{}; h.read_memory = read; return h; }

const MapState::Mark* find(const MapState& m, int type) {
    for (int i = 0; i < m.nmarks; ++i) if (m.marks[i].type == type) return &m.marks[i];
    return nullptr;
}
bool near(const MapState::Mark* k, float x, float y, float tol = 0.6f) { return k && AbsF(k->x - x) <= tol && AbsF(k->y - y) <= tol; }
void reset_state() {
    g_dest = {}; g_nmemo = 0; g_memo_area[0] = 0; g_nmovers = 0; g_wicons = {}; for (auto& wg : g_wgeo) wg = {};
    g_whole = 0; g_after_battle = 0; g_disagree = 0;
}
}  // namespace t
using t::put;

// ---- an area: the destination "!" from the relative-coordinate fallback ------------------------------------------
// The player icon sits at the minimap's centre (0, 0); the "!" icon's local position is its offset from the player
// in minimap px (the game updates it while it shows the minimap). Picture px per minimap px: pk / 5 = 3.
struct Area {
    EdenDsmodHostApi h = t::host();
    Guest g{&h};
    MapState m{};
    t::Icon player, dest, inn, enemy;
    uint64_t pres;
    float dx = 400, dy = 300;            // the destination's real place on the picture
    ObjId id{20, 1201000, 0xA0};
    bool live = true;                    // the game is updating the "!" icon's position
    int state = 0;                       // the game's minimap state (Normal 0, Partial 1, Hide 2)
    uint64_t pm = 0;                     // 0.9.40 a ParameterManager with a map destination table (0: none)
    int fieldmap = 33, hero = 1;
    Area() {
        m.w = 1024; m.h = 2048; m.pk = 15; m.pmode = 1; m.pox = 0; m.poy = 0;
        std::strcpy(m.area, "minimap_mf_0008_01a001");
        m.x = 500; m.y = 500;
        player = t::icon(1, 0, 0);
        dest = t::icon(9, (dx - m.x) / 3, (dy - m.y) / 3);
        inn = t::icon(20, 0, 0); put<float>(inn.ip + kIpWorld, 10.0f); put<float>(inn.ip + kIpWorld + 8, 20.0f);
        enemy = t::icon(3, 5, 5);
        pres = t::presenter({&player, &dest, &inn});
    }
    void step(float mx, float my) {
        m.x = mx; m.y = my;
        if (live) t::move_icon(dest, (dx - m.x) / 3, (dy - m.y) / 3);
        t::state_pass(m, state);
        BeginMarks(m);
        DestIdentity(g_dest, m.area, id);
        LookupTableDest(g, pm, fieldmap, hero, id.prog);
        ReadMarks(m, g, pres, 1, 0, 60, 60, false);
        EndMarks(m, &g);
    }
    void minimap(bool on) {   // the game's minimap shown / hidden: every icon see-through, the "!" no longer updated
        for (t::Icon* c : {&player, &dest, &inn, &enemy}) t::alpha(*c, on ? 1.0f : 0.0f);
        live = on; state = on ? 0 : 2;
    }
    const MapState::Mark* bang() const { return t::find(m, 9); }
};

void test_minimap_state() {
    T_CHECK(MiniFromState(0) == kMiniOn);       // Normal
    T_CHECK(MiniFromState(1) == kMiniOn);       // Partial
    T_CHECK(MiniFromState(2) == kMiniOff);      // Hide
    T_CHECK(MiniFromState(-1) == kMiniUnknown && MiniFromState(7) == kMiniUnknown);
}

void test_area_dest() {
    t::reset_state();
    Area a;
    // acquiring: a UI-relative position is not shown until it stays put while the player moves
    a.step(500, 500);
    T_CHECK(!a.bang() && g_dest.why == kDwWaiting && g_dest.rsrc == kSrcLocal);
    a.step(500, 500);
    T_CHECK(!a.bang());                                   // standing still: nothing to verify against
    a.step(510, 500);
    T_CHECK(t::near(a.bang(), 400, 300) && g_dest.why == kDwVerified);

    // 1. minimap off / on while standing still: the verified "!" stays, no walking needed to get it back
    a.minimap(false);
    for (int i = 0; i < 200; ++i) a.step(510, 500);
    T_CHECK(t::near(a.bang(), 400, 300) && g_dest.why == kDwHidden && a.m.mini == kMiniOff);
    a.minimap(true);
    a.step(510, 500);
    T_CHECK(t::near(a.bang(), 400, 300) && g_dest.why == kDwAgrees && a.m.mini == kMiniOn);

    // 2. toggled while walking: hidden readings are not used; a live one agrees at once
    a.minimap(false);
    for (int i = 0; i < 30; ++i) a.step(510 + i * 3, 500 + i * 2);
    T_CHECK(t::near(a.bang(), 400, 300));
    a.minimap(true);
    a.step(600, 560);
    T_CHECK(t::near(a.bang(), 400, 300) && g_dest.why == kDwAgrees);

    // 5. stale / inconsistent UI numbers while the icon is shown (the minimap coming back): the icon keeps its
    //    last offset while the player walks, so it slides -- never verified, the "!" stays where it was
    a.live = false;
    int moved_off = 0, slid = 0;
    for (int i = 0; i < 40; ++i) {           // several speeds: never verified, never moved
        a.step(600 + i * (1 + i % 5), 560 + (i % 3));
        moved_off += !t::near(a.bang(), 400, 300);
        slid += g_dest.why == kDwSlid;
        T_CHECK(g_dest.why != kDwReplaced && g_dest.why != kDwVerified);
    }
    T_CHECK(moved_off == 0 && slid > 0);
    // a slow walk (2 px a pass): a stale icon slides 2 px a pass too -- compared with the reading at the start of
    // the move, never with the last one (that would pass it as "stayed put")
    {
        const float sx = a.m.x;
        for (int i = 1; i <= 30; ++i) {
            a.step(sx + 2 * i, 560);
            T_CHECK(t::near(a.bang(), 400, 300) && g_dest.why != kDwReplaced);
        }
    }
    a.step(680, 560); a.step(680, 560);
    a.live = true;
    a.step(680, 560);
    T_CHECK(t::near(a.bang(), 400, 300) && g_dest.why == kDwAgrees);
    // ... and one wild reading (a frame of garbage) does not move it either
    t::move_icon(a.dest, 999, 999);
    a.m.x = 680;
    BeginMarks(a.m); DestIdentity(g_dest, a.m.area, a.id); ReadMarks(a.m, a.g, a.pres, 1, 0, 60, 60, false); EndMarks(a.m, &a.g);
    T_CHECK(t::near(a.bang(), 400, 300) && g_dest.why == kDwWaiting);
    a.step(680, 560);
    T_CHECK(t::near(a.bang(), 400, 300) && g_dest.why == kDwAgrees);

    // 3. the game rebuilds the "!" icon (a new object, same objective): the same place, shown at once
    t::Icon dest2 = t::icon(9, (a.dx - 680) / 3, (a.dy - 560) / 3);
    t::set_list(a.pres, {&a.player, &dest2, &a.inn});
    a.dest = dest2;
    a.step(680, 560);
    T_CHECK(t::near(a.bang(), 400, 300) && g_dest.why == kDwAgrees);

    // 9. a battle: no reads at all (ReadMap returns), then the field again -- same objective, still shown
    a.minimap(false);
    a.step(680, 560);
    T_CHECK(t::near(a.bang(), 400, 300));
    a.minimap(true);

    // 4. the same icon object reused for a DIFFERENT objective: it does not inherit the old place
    a.id = ObjId{21, 1300000, 0xA0};
    a.dx = 200; a.dy = 900;
    a.step(680, 560);
    T_CHECK(!a.bang() && g_dest.why == kDwWaiting);
    a.step(690, 560);
    T_CHECK(t::near(a.bang(), 200, 900) && g_dest.why == kDwVerified);

    // 7. the objective completed (no destination row): no "!"
    a.id = ObjId{-1, 1400000, 0xA0};
    a.step(690, 560);
    T_CHECK(!a.bang() && g_dest.why == kDwNoObj);
    a.id = ObjId{21, 1300000, 0xA0};      // (back to an objective: verified again from scratch)
    a.step(690, 560);
    T_CHECK(!a.bang());
    a.step(700, 560);
    T_CHECK(t::near(a.bang(), 200, 900));

    // 8. another floor, or another save: starts over
    std::strcpy(a.m.area, "minimap_mf_0008_01a002");
    a.step(700, 560);
    T_CHECK(!a.bang() && eq(g_dest.area, "minimap_mf_0008_01a002"));
    std::strcpy(a.m.area, "minimap_mf_0008_01a001");
    a.step(700, 560);
    a.step(710, 560);
    T_CHECK(t::near(a.bang(), 200, 900));
    a.id.save = 0xB0;
    a.step(710, 560);
    T_CHECK(!a.bang() && g_dest.why == kDwWaiting);

    // a world position (the "!" object's own) is taken at once
    a.id.save = 0xA0;
    put<float>(a.dest.ip + kIpWorld, 30.0f); put<float>(a.dest.ip + kIpWorld + 8, 40.0f);
    a.step(710, 560);
    T_CHECK(t::near(a.bang(), 1024 / 2 + 30 * 15, 2048 / 2 + 40 * 15) && g_dest.why == kDwWorld && g_dest.src == kSrcWorld);
}

// 6. no moving icons while the minimap is shown: the memo still forgets an icon the game stopped using (0.9.36
//    took "no moving icons" for "minimap off" and kept it forever); with the minimap off it is kept
void test_memo_no_movers() {
    t::reset_state();
    Area a;
    for (int i = 0; i < 5; ++i) a.step(500, 500);
    T_CHECK(t::find(a.m, 20) != nullptr && g_nmovers == 0);
    t::used(a.inn, false);
    a.minimap(false);
    for (int i = 0; i < 200; ++i) a.step(500, 500);
    T_CHECK(t::find(a.m, 20) != nullptr);              // minimap off: nothing forgotten
    a.minimap(true);
    for (int i = 0; i < 89; ++i) a.step(500, 500);
    T_CHECK(t::find(a.m, 20) != nullptr);
    for (int i = 0; i < 2; ++i) a.step(500, 500);
    T_CHECK(t::find(a.m, 20) == nullptr && a.m.mini == kMiniOn);
}

// ---- the world map: list geometry and the kept set ---------------------------------------------------------------
// L2 (whole world): picture = (1400, 1100) + local x 5. L1 (round): its own locals, picture = (1000, 900) + local x 4.
struct World {
    EdenDsmodHostApi h = t::host();
    Guest g{&h};
    MapState m{};
    t::Icon p1, p2, town1, town2, bang1, bang2;
    uint64_t pres1, pres2;
    ObjId id{22, 1400000, 0xA0};
    bool live2 = true, live1 = true, round_on = false;
    int state = -1;                      // the game's minimap state (-1: not read -- the move-only path)
    float bx = 1200, by = 1500;          // the destination
    World() {
        m.w = 2800; m.h = 2200; m.pmode = 1; m.pk = 1; m.x = 1500; m.y = 1000;
        std::strcpy(m.area, "map_expel");
        p2 = t::icon(1, (m.x - 1400) / 5, (m.y - 1100) / 5);
        town2 = t::icon(20, 100, 50);                      // picture (1900, 1350)
        bang2 = t::icon(9, (bx - 1400) / 5, (by - 1100) / 5);
        p1 = t::icon(1, (m.x - 1000) / 4, (m.y - 900) / 4);
        town1 = t::icon(20, (1900 - 1000) / 4.0f, (1350 - 900) / 4.0f);
        bang1 = t::icon(9, (bx - 1000) / 4, (by - 900) / 4);
        pres2 = t::presenter({&p2, &town2, &bang2});
        pres1 = t::presenter({});
    }
    void round(bool on) { round_on = on; t::set_list(pres1, on ? std::initializer_list<const t::Icon*>{&p1, &town1, &bang1} : std::initializer_list<const t::Icon*>{}); }
    void step(float mx, float my) {
        m.x = mx; m.y = my;
        if (live2) t::move_icon(p2, (m.x - 1400) / 5, (m.y - 1100) / 5);
        if (live1) t::move_icon(p1, (m.x - 1000) / 4, (m.y - 900) / 4);
        if (round_on) {      // the round minimap pins a far "!" to its rim (175 minimap px window, at 0.9 of it)
            const float ox = (bx - m.x) / 4, oy = (by - m.y) / 4, d2 = ox * ox + oy * oy;
            if (d2 > 157.5f * 157.5f) { float s = 1, q = d2; for (int i = 0; i < 20; ++i) s = 0.5f * (s + q / s);
                t::move_icon(bang1, (m.x - 1000) / 4 + ox * 157.5f / s, (m.y - 900) / 4 + oy * 157.5f / s); }
            else t::move_icon(bang1, (bx - 1000) / 4, (by - 900) / 4);
        }
        t::state_pass(m, state);
        BeginMarks(m);
        ReadMarks(m, g, pres1, 1, 4, 175, 175, true);
        ReadMarks(m, g, pres2, 2, 4, 0, 0, false, 5.0f);
        EndMarks(m);
        WorldIconCache(m, id);
    }
};

void test_world() {
    t::reset_state();
    World w;
    // not verified yet: nothing placed from the player's icon
    w.step(1500, 1000);
    T_CHECK(!t::find(w.m, 20) && w.m.unverified[2] == 2 && GeoFor("map_expel", 2)->why == kGeoWaiting);
    w.step(1500, 1000);
    T_CHECK(!t::find(w.m, 20));
    w.step(1510, 1004);                                    // the player moves; its icon follows: verified
    const ListGeo* G2 = GeoFor("map_expel", 2);
    T_CHECK(G2->ok && G2->fresh > 0 && AbsF(G2->cx - 1400) < 0.5f && AbsF(G2->cy - 1100) < 0.5f);
    T_CHECK(t::near(t::find(w.m, 20), 1900, 1350) && t::near(t::find(w.m, 9), 1200, 1500));

    // the game's minimap off: its player icon stops following (0.9.37 on the Thor) -- every icon stayed put
    // here, where 0.9.37 moved them all by the player's walk since
    w.live2 = false;
    for (int i = 0; i < 40; ++i) w.step(1510 + i * 5, 1004 - i * 2);
    T_CHECK(G2->fresh < 0 && G2->why == kGeoStale);
    T_CHECK(t::near(t::find(w.m, 20), 1900, 1350) && t::near(t::find(w.m, 9), 1200, 1500));
    // ... and when the minimap comes back, the live icon agrees again
    w.live2 = true;
    w.step(1700, 920);
    w.step(1702, 920);
    T_CHECK(G2->why == kGeoAgrees && t::near(t::find(w.m, 20), 1900, 1350));

    // 10. the round minimap: its own list, its own geometry; a far "!" pinned to the rim is left out and the
    //     verified one is kept (0.9.36 drew the rim one beside the player)
    t::set_list(w.pres2, {});                              // (the whole-world list empty in round mode)
    w.round(true);
    w.step(1700, 920);
    w.step(1710, 925);
    const ListGeo* G1 = GeoFor("map_expel", 1);
    T_CHECK(G1->ok && AbsF(G1->cx - 1000) < 0.5f && AbsF(G1->cy - 900) < 0.5f);
    T_CHECK(w.m.clamped == 1 && t::near(t::find(w.m, 9), 1200, 1500) && t::near(t::find(w.m, 20), 1900, 1350));
    // walking near the destination: the round minimap shows it where it is
    w.step(1300, 1400);
    w.step(1305, 1400);
    T_CHECK(w.m.clamped == 0 && t::near(t::find(w.m, 9), 1200, 1500));

    // 7. a new objective: the kept "!" is the old one's -- not drawn for the new one until read
    w.round(false);
    t::set_list(w.pres2, {&w.p2, &w.town2});               // the new objective's "!" not given yet
    w.id = ObjId{23, 1500000, 0xA0};
    w.step(1305, 1400);
    w.step(1310, 1400);
    T_CHECK(!t::find(w.m, 9) && t::near(t::find(w.m, 20), 1900, 1350));
    w.bx = 2000; w.by = 600;
    t::move_icon(w.bang2, (w.bx - 1400) / 5, (w.by - 1100) / 5);
    t::set_list(w.pres2, {&w.p2, &w.town2, &w.bang2});
    w.step(1310, 1400);
    T_CHECK(t::near(t::find(w.m, 9), 2000, 600));

    // 9. after a battle the kept set is drawn until a read agrees (0.9.27), and the geometry is kept
    g_after_battle = 1;
    w.step(1310, 1400);
    T_CHECK(t::near(t::find(w.m, 20), 1900, 1350) && t::near(t::find(w.m, 9), 2000, 600) && g_after_battle == 0);
}

// 0.9.39 (0.9.38 on the Thor: no world icons after loading until the player moved): with the game's state saying
// the minimap is shown, the list in use is verified standing still; Hide never verifies and keeps what was verified
// 0.9.52 (0.9.51 on the Thor: the round minimap's icons moved with the player while walking, back in place after a
// toggle): the game's round list is drawn around the player -- its player icon stays at 0 and the other icons move
// as the player walks. Placed from the player's icon in the same pass, a town stays where it is all the walk.
void test_round_walk() {
    t::reset_state();
    World w;
    t::set_list(w.pres2, {});
    w.live1 = false;                                     // (the World's own round model is not used here)
    w.state = 1;                                         // Partial: the round view
    auto centred = [&](float px, float py) {             // the game's round list around the player, k 4
        t::move_icon(w.p1, 0, 0);
        t::move_icon(w.town1, (1900 - px) / 4, (1350 - py) / 4);
        t::move_icon(w.bang1, (1600 - px) / 4, (1100 - py) / 4);
    };
    t::set_list(w.pres1, {&w.p1, &w.town1, &w.bang1});
    w.bx = 1600; w.by = 1100;
    for (int i = 0; i < 12; ++i) { centred(1500, 1000); w.step(1500, 1000); }   // standing still
    T_CHECK(t::near(t::find(w.m, 20), 1900, 1350));
    for (int i = 0; i < 40; ++i) {                       // walking: every pass the list moves with the player
        const float px = 1500 + i * 6, py = 1000 + i * 4;
        centred(px, py); w.step(px, py);
        if (!t::near(t::find(w.m, 20), 1900, 1350)) { T_CHECK(false); break; }
    }
    T_CHECK(t::near(t::find(w.m, 20), 1900, 1350));
    // Hide: the round list stops; the kept set stays where it was
    w.state = 2; t::set_list(w.pres1, {});
    for (int i = 0; i < 20; ++i) w.step(1800 + i * 5, 1200);
    T_CHECK(t::near(t::find(w.m, 20), 1900, 1350));
}

void test_world_state() {
    t::reset_state();
    World w;
    w.state = 0;                                           // Normal (the whole-world view)
    for (int i = 0; i < 9; ++i) w.step(1500, 1000);
    T_CHECK(!t::find(w.m, 20));                            // not yet: the state has to hold 10 passes
    for (int i = 0; i < 12; ++i) w.step(1500, 1000);
    const ListGeo* G2 = GeoFor("map_expel", 2);
    T_CHECK(G2->ok && G2->why == kGeoAgrees && AbsF(G2->cx - 1400) < 0.5f);
    T_CHECK(t::near(t::find(w.m, 20), 1900, 1350) && t::near(t::find(w.m, 9), 1200, 1500));
    // the round list has no icons in this view: not verified by the state
    T_CHECK(!GeoFor("map_expel", 1)->ok);
    // Hide: the player's icon stops following; nothing re-verified, every icon stays put
    w.state = 2; w.live2 = false;
    for (int i = 0; i < 40; ++i) w.step(1500 + i * 5, 1000 + i * 3);
    T_CHECK(G2->why == kGeoHidden && AbsF(G2->cx - 1400) < 0.5f);
    T_CHECK(t::near(t::find(w.m, 20), 1900, 1350) && t::near(t::find(w.m, 9), 1200, 1500));
    // a STALE player icon that stays still under a shown state for a moment (the toggle's first frames) is not
    // taken: it disagrees with the verified C and the state has not held 10 passes
    w.state = 0;
    for (int i = 0; i < 5; ++i) w.step(1700, 1120);
    T_CHECK(AbsF(G2->cx - 1400) < 0.5f && t::near(t::find(w.m, 20), 1900, 1350));
    w.live2 = true;
    w.step(1700, 1120);
    T_CHECK(G2->why == kGeoAgrees);
}

// 0.9.39 the area "!": a reading while the game says Hide is not used, even if the icon still looks visible
void test_area_state() {
    t::reset_state();
    Area a;
    a.step(500, 500); a.step(510, 500);
    T_CHECK(t::near(a.bang(), 400, 300));
    a.state = 2; a.live = false;                           // Hide, icon colour untouched, numbers stale
    for (int i = 0; i < 20; ++i) a.step(510 + 4 * i, 500);
    T_CHECK(t::near(a.bang(), 400, 300) && g_dest.why == kDwHidden && a.m.mini == kMiniOff);
    a.state = 0; a.live = true;
    a.step(590, 500);
    T_CHECK(t::near(a.bang(), 400, 300) && g_dest.why == kDwAgrees && a.m.mini == kMiniOn);
}

// 0.9.40 the game's map destination table: rows {fieldmapID, heroID, type, start, end, isOverridePosition, position}
struct Row { int field, hero, type, start, end; bool over; float x, y, z; };
uint64_t make_pm(std::initializer_list<Row> rows) {
    const uint64_t pm = t::alloc(0x800), sp = t::alloc(0x20), list = t::alloc(0x20), arr = t::alloc(0x20 + 8 * rows.size());
    put<uint64_t>(pm + kPmMapDest, sp); put<uint64_t>(sp + kMpList, list);
    put<uint64_t>(list + kListItems, arr); put<int32_t>(list + kListSize, (int32_t)rows.size());
    int i = 0;
    for (const Row& r : rows) {
        const uint64_t a = t::alloc(0x60);
        put<int32_t>(a + kMdField, r.field); put<int32_t>(a + kMdHero, r.hero); put<int32_t>(a + 0x18, r.type);
        put<int32_t>(a + kMdStart, r.start); put<int32_t>(a + kMdEnd, r.end); put<uint8_t>(a + kMdOverride, r.over ? 1 : 0);
        put<float>(a + kMdPos, r.x); put<float>(a + kMdPos + 4, r.y); put<float>(a + kMdPos + 8, r.z);
        put<uint64_t>(arr + kArrItems + 8 * i++, a);
    }
    return pm;
}
void test_table_dest() {
    t::reset_state(); g_tdest = {};
    Area a;
    // Krosse Cave's row (r47 [2]); picture = centre + world x pk (15): (10, 6, 20) -> (662, 1324)
    a.pm = make_pm({{210, 1, 3, 502000, 503000, false, 0, 0, 0},
                    {33, 0, 0, 1201000, 1300000, true, 10, 6, 20}});
    a.id = ObjId{20, 1201000, 0xA0};
    a.step(500, 500);                                      // standing still: shown at once, from the table
    T_CHECK(t::near(a.bang(), 662, 1324) && g_dest.why == kDwTable && g_dest.src == kSrcTable && g_tdest.row == 1);
    // the minimap's own numbers disagree (stale, sliding): the table's position stands
    a.live = false;
    for (int i = 0; i < 10; ++i) a.step(500 + 7 * i, 500);
    T_CHECK(t::near(a.bang(), 662, 1324));
    // minimap hidden (the icon gone from the list): kept
    a.state = 2; t::set_list(a.pres, {&a.player, &a.inn});
    for (int i = 0; i < 50; ++i) a.step(560, 500);
    T_CHECK(t::near(a.bang(), 662, 1324));
    t::set_list(a.pres, {&a.player, &a.dest, &a.inn}); a.state = 0; a.live = true;
    // another hero's row only, or the progress past the range: no table position
    g_tdest = {};
    a.hero = 2; a.pm = make_pm({{33, 1, 0, 1201000, 1300000, true, 10, 6, 20}});
    a.step(560, 500);
    T_CHECK(!g_tdest.ok && g_tdest.nmatch == 0);
    a.hero = 1; a.id = ObjId{21, 1300000, 0xA0};
    a.step(560, 500);
    T_CHECK(!g_tdest.ok && !g_dest.ok && !a.bang());
    // two rows match (an override and an NPC): which one the game shows is not known -- the UI path instead
    g_tdest = {};
    a.id = ObjId{20, 1201000, 0xA0};
    a.pm = make_pm({{33, 0, 0, 1201000, 1300000, true, 10, 6, 20}, {33, 0, 1, 1201000, 1300000, false, 0, 0, 0}});
    a.step(560, 500);
    T_CHECK(!g_tdest.ok && g_tdest.nmatch == 2 && !a.bang() && g_dest.why == kDwWaiting);
    // no "!" shown by the game yet for this objective (its icon not in use): nothing, table or not
    g_tdest = {};
    a.pm = make_pm({{33, 0, 0, 1201000, 1300000, true, 10, 6, 20}});
    a.id = ObjId{22, 1201000, 0xA0};
    t::set_list(a.pres, {&a.player, &a.inn});
    a.step(560, 500);
    T_CHECK(g_tdest.ok && !a.bang());
    t::set_list(a.pres, {&a.player, &a.dest, &a.inn});
    a.step(560, 500);
    T_CHECK(t::near(a.bang(), 662, 1324) && g_dest.seen);
}

// 0.9.42 the objects the game marks: a managed component with a class name, its native component -> GameObject ->
// transform at a world position
uint64_t klass(const char* name) {
    const uint64_t k = t::alloc(0x40), str = t::alloc(64);
    std::memcpy(&t::mem[str - t::kBase], name, std::strlen(name) + 1);
    put<uint64_t>(k + kClassName, str);
    return k;
}
uint64_t marked_obj(uint64_t kl, float wx, float wy, float wz) {
    const uint64_t o = t::alloc(0x200), nat = t::alloc(0x40), gobj = t::alloc(0x40), comps = t::alloc(0x20), tr = t::alloc(0x50),
                   hier = t::alloc(0x30), trs = t::alloc(0x40), par = t::alloc(0x10);
    put<uint64_t>(o, kl); put<uint64_t>(o + 0x10, nat); put<uint64_t>(nat + 0x30, gobj); put<uint64_t>(gobj + 0x30, comps);
    put<uint64_t>(comps + 8, tr); put<uint64_t>(tr + 0x38, hier); put<int32_t>(tr + 0x40, 0);
    put<uint64_t>(hier + 0x18, trs); put<uint64_t>(hier + 0x20, par); put<int32_t>(par, 0);   // its own parent: the root
    put<float>(trs, wx); put<float>(trs + 4, wy); put<float>(trs + 8, wz);
    return o;
}
uint64_t field_manager(uint64_t list_off, std::initializer_list<uint64_t> objs) {
    const uint64_t fm = t::alloc(0x400), list = t::alloc(0x20), arr = t::alloc(0x20 + 8 * objs.size());
    put<uint64_t>(fm + list_off, list); put<uint64_t>(list + kListItems, arr); put<int32_t>(list + kListSize, (int32_t)objs.size());
    int i = 0;
    for (uint64_t o : objs) put<uint64_t>(arr + kArrItems + 8 * i++, o);
    return fm;
}
void test_marked() {
    t::reset_state(); g_tdest = {};
    EdenDsmodHostApi h = t::host();
    Guest g{&h};
    // area: two exits marked (subIconType 9), one not, and an object of another class with 9 at the same offset
    const uint64_t mj = klass("FieldMapjumpCollision"), other = klass("FieldGimmick16Switch");
    const uint64_t e1 = marked_obj(mj, 10, 0, 20), e2 = marked_obj(mj, -4, 0, 8), e3 = marked_obj(mj, 30, 0, 30), x1 = marked_obj(other, 1, 0, 1);
    put<int32_t>(e1 + kMjSubIcon, 9); put<int32_t>(e2 + kMjSubIcon, 9); put<int32_t>(e3 + kMjSubIcon, 4); put<int32_t>(x1 + kMjSubIcon, 9);
    const uint64_t fm = field_manager(kFmCollisionList, {e1, x1, e2, e3});
    ScanMarked(g, fm, false, 33);
    T_CHECK(g_marked.n == 2 && g_marked.total == 4 && !g_marked.world);
    MapState m{}; m.w = 1024; m.h = 2048; m.pmode = 1; m.pk = 15;
    T_CHECK(AddMarked(m) == 2);
    T_CHECK(t::near(t::find(m, 9), 512 + 150, 1024 + 300));                   // (10, 20) x 15 from the centre
    // ... and with exits marked, the tracked minimap "!" (a UI reading) is not drawn as well
    Area a;
    a.step(500, 500); a.step(510, 500);                                     // verified from the minimap numbers
    T_CHECK(g_dest.ok && g_dest.src == kSrcLocal && !t::find(a.m, 9));       // ... but not drawn: the exits are the "!"
    g_marked = {};
    a.step(510, 500);
    T_CHECK(t::near(a.bang(), 400, 300));                                   // no marked exits: the tracked one again
    // world: the location marked (iconType or subIconType 9) in fieldObjectList, other symbols not
    const uint64_t ws = klass("WorldmapSymbol");
    const uint64_t s1 = marked_obj(ws, 100, 0, -50), s2 = marked_obj(ws, -30, 0, 40);
    put<int32_t>(s1 + kWsSubIcon, 9); put<int32_t>(s2 + kWsIcon, 13);
    ScanMarked(g, field_manager(kFmObjectList, {s2, s1}), true, 1);
    T_CHECK(g_marked.n == 1 && g_marked.world && AbsF(g_marked.w[0][0] - 100) < 1e-3f && AbsF(g_marked.w[0][2] + 50) < 1e-3f);
    // rescanned only every 15 passes (or on a new map): a marked location that moves is followed
    put<int32_t>(s2 + kWsIcon, 9);
    ScanMarked(g, field_manager(kFmObjectList, {s2, s1}), true, 1);
    T_CHECK(g_marked.n == 1);
    for (int i = 0; i < 15; ++i) ScanMarked(g, field_manager(kFmObjectList, {s2, s1}), true, 1);
    T_CHECK(g_marked.n == 2);
    g_marked = {};
}

// 0.9.45 the whole-world geometry from the game's pictures: icons at load with the minimap hidden
void index_minimap(const char* name, uint32_t sx, uint32_t sy, uint32_t sw, uint32_t sh, uint32_t tw, uint32_t th) {
    g_ix = {}; g_ix.loaded = true;
    g_ix.nt = 1; g_ix.t[0] = {8, 0, 0, 0, tw, th, 12, 16};
    g_ix.ns = 1; std::strcpy(g_ix.s[0].name, name); g_ix.s[0].tex = 8;
    g_ix.s[0].x = sx; g_ix.s[0].y = sy; g_ix.s[0].w = sw; g_ix.s[0].h = sh;
}
void test_geo_pictures() {
    t::reset_state();
    index_minimap("minimap_expel001", 0, 0, 560, 440, 560, 440);
    World w;
    w.state = 2; w.live2 = false;                      // loaded with the minimap hidden: the player icon not following
    w.step(1500, 1000);
    const ListGeo* G2 = GeoFor("map_expel", 2);
    T_CHECK(G2->ok && G2->k == 5.0f && G2->cx == 1400 && G2->cy == 1100 && g_whole == 5.0f);
    T_CHECK(t::near(t::find(w.m, 20), 1900, 1350) && t::near(t::find(w.m, 9), 1200, 1500));   // the first pass
    // 0.9.51 the diag page shown in two towns first (0.9.50 on the Thor: they took both slots, the world map none)
    t::reset_state();
    T_CHECK(!GeoFind("field_salva", 2) && !GeoFor("field_salva", 2) && !GeoFor("field_arlia", 1));
    {
        World w0; w0.state = 2; w0.live2 = false;
        w0.step(1500, 1000);
        T_CHECK(GeoFor("map_expel", 2) && GeoFor("map_expel", 2)->ok && t::near(t::find(w0.m, 20), 1900, 1350));
        T_CHECK(GeoFind("map_expel", 2) == GeoFor("map_expel", 2));
    }
    t::reset_state();
    w.step(1500, 1000);
    G2 = GeoFor("map_expel", 2);
    // shown later: the live icon agrees
    w.state = 0; w.live2 = true;
    for (int i = 0; i < 12; ++i) w.step(1510, 1000);
    T_CHECK(G2->why == kGeoAgrees);
    // a trimmed minimap picture (nede's): not used, the live checks as before
    t::reset_state();
    index_minimap("minimap_expel001", 61, 50, 438, 340, 560, 440);
    World w2;
    w2.state = 2; w2.live2 = false;
    w2.step(1500, 1000);
    T_CHECK(!GeoFor("map_expel", 2)->ok && !t::find(w2.m, 20));
    // a scale that disagrees with what the game shows: a live reading over a move replaces it
    t::reset_state();
    index_minimap("minimap_expel001", 0, 0, 500, 392, 500, 392);   // 2800/500 = 5.6 (not the game's 5)
    World w3;
    w3.state = -1;
    w3.step(1500, 1000);
    T_CHECK(GeoFor("map_expel", 2)->ok && GeoFor("map_expel", 2)->k > 5.5f);
    g_whole = 5.0f;                                              // (as the player icon would estimate it)
    w3.step(1500, 1000); w3.step(1510, 1004);
    T_CHECK(GeoFor("map_expel", 2)->why == kGeoReplaced && AbsF(GeoFor("map_expel", 2)->cx - 1400) < 0.5f);
    g_ix = {};
}

// 0.9.46 a texture found by its fingerprint in a bundle the update replaced. The numbers below are what the
// builder's own fingerprint() (Python) gives for the same 5000 bytes -- so this checks the two agree.
std::vector<uint8_t> g_file;   // the fake bundle file the host serves
size_t file_read(void*, const char*, uint64_t off, void* out, size_t n) {
    if (!out) return g_file.size();
    if (off >= g_file.size()) return 0;
    const size_t k = std::min(n, g_file.size() - static_cast<size_t>(off));
    std::memcpy(out, g_file.data() + off, k);
    return k;
}
void test_relocate() {
    std::vector<uint8_t> tex;
    uint32_t x = 2463534242u;
    for (int i = 0; i < 5000; ++i) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; tex.push_back(static_cast<uint8_t>(x & 0xFF)); }
    // three uncompressed blocks of 4096; the texture at the odd offset 3001 (across two block boundaries)
    g_file.assign(3 * 4096, 0);
    for (size_t i = 0; i < g_file.size(); ++i) g_file[i] = static_cast<uint8_t>(i * 7 + 3);
    std::memcpy(g_file.data() + 3001, tex.data(), tex.size());
    EdenDsmodHostApi h = t::host(); h.read_romfs = file_read;
    OpenBundle ob{}; ob.ok = true; ob.id = 99; ob.n = 3; ob.moved = true;
    for (uint32_t k = 0; k < 3; ++k) { ob.us[k] = ob.cs[k] = 4096; ob.fl[k] = 0; ob.ustart[k] = ob.fstart[k] = 4096ull * k; }
    ob.ustart[3] = 3 * 4096;
    IdxTex tt{}; tt.size = 5000; tt.sig_off = 2576ull; tt.sig_hash = 9857369862035269380ull; tt.full_hash = 6832932038771038638ull;
    for (auto& sl : g_slot) sl = {};
    T_CHECK(Relocate(&h, &ob, tt) == 3001);
    // one byte of the texture changed (another picture): the window may still match, the whole hash does not
    g_file[3001 + 4999] ^= 0x55;
    for (auto& sl : g_slot) sl = {};
    T_CHECK(Relocate(&h, &ob, tt) == -2);
    g_file.clear();
}

// the pure pieces on their own
void test_geo_unit() {
    ListGeo G{};
    GeoTrack(G, 10, 10, 100, 100); GeoVerify(G, 10, 10, 100, 100, 5);
    T_CHECK(!G.ok && G.why == kGeoWaiting);
    GeoTrack(G, 11.2f, 10, 106, 100); GeoVerify(G, 11.2f, 10, 106, 100, 5);
    T_CHECK(G.ok && G.why == kGeoVerified && AbsF(G.cx - 50) < 1e-3f);
    // a stale player icon never re-anchors the geometry, however long the walk
    for (int i = 1; i < 50; ++i) { GeoTrack(G, 11.2f, 10, 106 + i * 3, 100); GeoVerify(G, 11.2f, 10, 106 + i * 3, 100, 5); }
    T_CHECK(G.ok && G.why == kGeoStale && AbsF(G.cx - 50) < 1e-3f);
    // a real change of layout (a live icon, a new C held over a move) is taken
    GeoTrack(G, 12, 10, 300, 100); GeoVerify(G, 12, 10, 300, 100, 5);
    GeoTrack(G, 13.2f, 10, 306, 100); GeoVerify(G, 13.2f, 10, 306, 100, 5);
    T_CHECK(G.ok && G.why == kGeoReplaced && AbsF(G.cx - 240) < 1e-3f);
}

int main() {
    test_minimap_state();
    test_geo_unit();
    test_area_dest();
    test_memo_no_movers();
    test_world();
    test_world_state();
    test_round_walk();
    test_area_state();
    test_table_dest();
    test_marked();
    test_geo_pictures();
    test_relocate();
    std::printf("%s (%d checks, %d failures)\n", t::fails ? "FAIL" : "PASS", t::checks, t::fails);
    return t::fails ? 1 : 0;
}
