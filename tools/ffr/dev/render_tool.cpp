// SPDX-FileCopyrightText: Copyright 2026 Daniel Vergara
// SPDX-License-Identifier: GPL-3.0-or-later
// Desktop render of the companion pages with sample state and the player's own RomFS (development only).
// usage: render_tool <romfs dir> <out dir>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include "../../../native/modules/ffr_pages.h"
int main(int argc, char** argv) {
    if (argc < 3) return 2;
    std::string root = argv[1], out = argv[2];
    ffr::RomRead rd = [&](const char* p, uint64_t off, void* dst, size_t n) -> size_t {
        std::string path = p; if (path.rfind("romfs:/", 0) == 0) path = path.substr(7);
        FILE* f = std::fopen((root + "/" + path).c_str(), "rb"); if (!f) return 0;
        std::fseek(f, 0, SEEK_END); size_t sz = size_t(std::ftell(f));
        if (!dst) { std::fclose(f); return sz; }
        std::fseek(f, long(off), SEEK_SET); size_t got = std::fread(dst, 1, n, f); std::fclose(f); return got; };
    ffr::IoStore io; ffr::Pak pak; std::string err;
    io.Open(rd, "romfs:/FFRS/Content/Paks/FFRS-Switch.utoc", "romfs:/FFRS/Content/Paks/FFRS-Switch.ucas", err);
    pak.Open(rd, "romfs:/FFRS/Content/Paks/FFRS-Switch.pak", err);
    std::unordered_map<std::string, std::string> loc; std::vector<uint8_t> b;
    pak.Read("FFRS/Content/Localization/Game/en/Game.locres", b); ffr::ParseLocres(b, loc);
    auto font = [&](const char* f) { ffr::Font ft; std::vector<uint8_t> t; pak.Read(std::string("FFRS/Content/UI/Font/FontFace/SE/") + f, b); ffr::UnwrapFontFace(b, t); ft.Load(t); return ft; };
    ffr::Font reg = font("SE-VOGEN.ufont"), bold = font("SE-VOGEB___.ufont"), title = font("SE-GARRITHR.ufont");
    std::map<std::string, ffr::Image> cache;
    ffr::Art art;
    art.image = [&](const std::string& p) -> const ffr::Image* {
        auto it = cache.find(p); if (it != cache.end()) return it->second.ok() ? &it->second : nullptr;
        ffr::Image im; std::vector<uint8_t> d; if (io.Read(p, d)) ffr::DecodeTexture(d, im); else std::printf("missing %s\n", p.c_str());
        auto& r = cache[p] = std::move(im); return r.ok() ? &r : nullptr; };
    art.text = [&](const std::string& ns, const std::string& k) { auto it = loc.find(ns + '\x1f' + k); return it == loc.end() ? std::string() : it->second; };
    art.regular = &reg; art.bold = &bold; art.title = &title;
    ffr::GameState s; s.in_game = true; s.save_ok = true; s.money = 1000; s.play_time = 1233; s.party = {1001, 1002};
    int ids[] = {1001, 1002, 1003, 1004}; int hp[] = {270, 240, 355, 600}; int mp[] = {35, 40, 93, 94}; int w[] = {10100, 10200, 10300, 10400}; int ar[] = {11000, 11000, 11500, 11007};
    for (int i = 0; i < 4; ++i) { ffr::UnitState u; u.id = ids[i]; u.hp = hp[i]; u.mp = mp[i]; u.weapon = w[i]; u.armor = ar[i]; u.join = i < 2 ? 0 : 1; u.xp = 1013 * i; u.max_hp = hp[i] + 30; u.max_mp = mp[i] + 10; u.level = 5 + i; s.units.push_back(u); }
    s.units[0].accessories = {12000}; s.units[0].vision = 13017; s.units[1].vision = 13024; s.units[2].vision = 13100; for (int k = 0; k < 6; ++k) s.units[0].stats[k] = 40 + 7 * k;
    s.inventory = {{1, 4}, {3, 2}, {4, 2}, {500, 3}, {30004, 1}, {30013, 1}, {30010, 1}, {30000, 1}, {30018, 1}, {10100, 1}, {11000, 2}, {12000, 2}, {10200, 1}, {13017, 1}, {13024, 1}, {13100, 1}};
    s.map.valid = true; s.map.map_id = 2014; s.map.area_id = 5; s.map.texture = "FFRS/Content/UI/Textures/MapCapture/Town_01Gra_22/Town_01Gra_22_5.uasset"; s.map.wx = 150; s.map.wy = 12400; s.map.bw = 15700; s.map.bh = 4000; s.map.px = 3213; s.map.py = 11623; s.map.yaw = 90;
    struct V { const char* n; ffr::View v; };
    ffr::View party; party.tab = ffr::kTabParty;
    ffr::View detail = party; detail.member = 0;
    ffr::View items; items.tab = ffr::kTabItems; items.item_sel = 1;
    ffr::View art_; art_.tab = ffr::kTabItems; art_.item_cat = 3;
    ffr::View map; map.tab = ffr::kTabMap;
    ffr::View equip; equip.tab = ffr::kTabItems; equip.item_cat = 1; equip.item_sel = 0;
    ffr::View btl; btl.tab = ffr::kTabBattle;
    for (auto& [n, v] : std::vector<V>{{"party", party}, {"detail", detail}, {"items", items}, {"artifacts", art_}, {"equip", equip}, {"map", map}, {"battle", btl}}) {
        ffr::Canvas c(ffr::kW, ffr::kH); ffr::ComposePage(c, s, v, art);
        FILE* f = std::fopen((out + "/page_" + n + ".rgba").c_str(), "wb"); std::fwrite(c.pixels().data(), 1, c.pixels().size(), f); std::fclose(f);
    }
    {  // a battle in progress: three enemies (one down) against the party
        ffr::GameState t = s; t.in_battle = true;
        auto unit = [](int id, bool enemy, int hp, int mhp, int mp, int mmp) { ffr::BattleUnit b; b.id = id; b.enemy = enemy; b.hp = hp; b.max_hp = mhp; b.mp = mp; b.max_mp = mmp; return b; };
        t.battle = {unit(5, true, 412, 640, 0, 0), unit(6, true, 0, 520, 0, 0), unit(118, true, 9120, 12000, 0, 0), unit(1001, false, 211, 300, 22, 45), unit(1002, false, 240, 270, 40, 50), unit(1016, false, 500, 500, 60, 60)};  // 1016: a guest
        for (int n : {5, 9}) {
            if (n == 9) for (int i = 0; i < 4; ++i) { t.battle.push_back(unit(2 + i, true, 100 * i, 400, 0, 0)); }
            if (n == 9) { t.battle.push_back(unit(1003, false, 0, 385, 0, 103)); t.battle.push_back(unit(1004, false, 600, 630, 90, 104)); }
            ffr::View v; v.tab = ffr::kTabBattle; ffr::Canvas c(ffr::kW, ffr::kH); ffr::ComposePage(c, t, v, art);
            FILE* f = std::fopen((out + "/page_battle_live" + std::to_string(n) + ".rgba").c_str(), "wb"); std::fwrite(c.pixels().data(), 1, c.pixels().size(), f); std::fclose(f);
        }
    }
    {  // the command window: Rain's skills list, then choosing a target (the second goblin)
        ffr::GameState t = s; t.in_battle = true;
        auto unit = [](int id, bool enemy, int hp, int mhp, int mp, int mmp, int uid) { ffr::BattleUnit b; b.id = id; b.enemy = enemy; b.hp = hp; b.max_hp = mhp; b.mp = mp; b.max_mp = mmp; b.unit = uint64_t(uid); return b; };
        t.battle = {unit(5, true, 412, 640, 0, 0, 10), unit(5, true, 300, 640, 0, 0, 11), unit(118, true, 9120, 12000, 0, 0, 12),
                    unit(1001, false, 211, 300, 22, 45, 1), unit(1003, false, 0, 385, 0, 103, 3), unit(1002, false, 240, 270, 40, 50, 2), unit(1004, false, 600, 630, 90, 104, 4)};
        t.battle[4].owner = 1, t.battle[6].owner = 2;          // visions equipped to the first two
        t.battle[0].staggered = true, t.battle[0].bp = 0, t.battle[0].max_bp = 6;
        t.menu.valid = t.menu.open = true; t.menu.actor = 1001;
        t.menu.path = {{0, 1}};
        t.menu.entries = {{1, 401010}, {1, 1}, {1, 400010}, {1, 7}, {1, 400020}, {1, 10}, {1, 402130}, {1, 402140}, {2, 1}};
        t.menu.cursor = 1;
        for (int st : {0, 1, 2}) {
            t.menu.state = st; if (st == 1) t.menu.targets = {11}, t.menu.sub = {10};
            t.menu.path = st == 1 ? std::vector<ffr::ActionRef>{{1, 1}, {1, 402130}} : st == 2 ? std::vector<ffr::ActionRef>{} : std::vector<ffr::ActionRef>{{1, 1}};
            if (st == 2) {
                t.menu.entries.clear(), t.menu.targets.clear(), t.menu.sub.clear();
                ffr::BattleMenu::Finish a; a.enable = true; a.skill = 414100; a.dir = 0; a.unit = 3;
                ffr::BattleMenu::Finish b; b.enable = true; b.skill = 414090; b.dir = 2; b.unit = 4;
                ffr::BattleMenu::Finish c2; c2.enable = false; c2.skill = 414110; c2.dir = 1;
                t.menu.finish = {a, b, c2};
            }
            ffr::View v; v.tab = ffr::kTabBattle; ffr::Canvas c(ffr::kW, ffr::kH); ffr::ComposePage(c, t, v, art);
            FILE* f = std::fopen((out + "/page_battle_menu" + std::to_string(st) + ".rgba").c_str(), "wb"); std::fwrite(c.pixels().data(), 1, c.pixels().size(), f); std::fclose(f);
            if (st == 0) { ffr::GameState w = t; w.menu.valid = w.menu.open = false; ffr::Canvas c2(ffr::kW, ffr::kH); ffr::ComposePage(c2, w, v, art);
                FILE* f2 = std::fopen((out + "/page_battle_wait.rgba").c_str(), "wb"); std::fwrite(c2.pixels().data(), 1, c2.pixels().size(), f2); std::fclose(f2); }
            if (st == 2) { auto h2 = ffr::BattleHit(t, 450, 790); std::printf("wheel hit kind %d index %d\n", int(h2.kind), h2.index); }
            auto hit = ffr::BattleHit(t, 900, 700); std::printf("hit(900,700) kind %d index %d\n", int(hit.kind), hit.index);
        }
    }
    { ffr::GameState t; ffr::View v; ffr::Canvas c(ffr::kW, ffr::kH); ffr::ComposePage(c, t, v, art);
      FILE* f = std::fopen((out + "/page_title.rgba").c_str(), "wb"); std::fwrite(c.pixels().data(), 1, c.pixels().size(), f); std::fclose(f); }
    std::printf("rendered\n");
}
