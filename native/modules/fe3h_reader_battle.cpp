// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Fire Emblem: Three Houses 1.2.0 reader (fe3h_reader.h): the battle: overview, cursor drive, units, forecast, danger overlays.

#include "fe3h_reader_internal.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <string_view>
#include <unordered_set>

namespace Fe3hReader {

// ---- battle overview (Minus overview 0x138930): title, conditions, counts, untapped ----

/// 0x9CCE0: a unit is shown unless fog (G+0x451) is on, +0x2F8 b0 is clear, the army is 1 / 3
/// and the tiles it covers have +0x27 b2 (fogged). Small units: the tile of the position floats
/// (+0x150 / +0x158 minus the map origin, / 1000; outside the map bounds -> tile 0); large:
/// +0xDC b4 -> shown, b3 -> tile (x, y) only, else shown unless all four tiles are fogged.
bool Reader::UnitVisible(const Unit& un, u8 fog, const std::array<s32, 4>& bounds) {
    const u8* b = un.b.data();
    if (!fog || (b[UArmy] | 2) != 3)
        return true;
    const auto flag = Get<u8>(un.at + 0x2F8);
    const auto grid = Ptr(gl.grid_var);
    if (!flag || !grid || (*flag & 1))
        return true;
    const auto fogged_at = [&](u64 t) {
        const auto v = Get<u8>(t + 0x27);
        return v && (*v & 4);
    };
    const auto tile = [&](s64 x, s64 y, bool first) {
        const bool in = first ? static_cast<u64>(x | y) <= 0x1F : x >= 0 && y >= 0 && x <= 0x1F && y <= 0x1F;
        return in ? *grid + static_cast<u64>(y) * 0x600 + static_cast<u64>(x) * 0x30 : *grid;
    };
    if (!(b[UFlags2] & 0x10)) {
        std::array<float, 3> pos{};
        const auto ox = Get<float>(gl.fog_ox), oz = Get<float>(gl.fog_oz);
        if (!Read(un.at + 0x150, pos.data(), sizeof(pos)) || !ox || !oz)
            return true;
        const s32 tx = static_cast<s32>((pos[0] - *ox) / 1000.0f);
        const s32 ty = static_cast<s32>((pos[2] - *oz) / 1000.0f);
        const bool in = bounds[3] > ty && bounds[1] <= ty && bounds[2] > tx && bounds[0] <= tx;
        const u64 t = in ? *grid + static_cast<u64>(static_cast<s64>(ty) * 0x600 + static_cast<s64>(tx) * 0x30)
                         : *grid;
        return !fogged_at(t);
    }
    if (b[UFootprint] & 0x10)
        return true;
    const s64 x = b[UX], y = b[UY];
    if (!fogged_at(tile(x, y, true)))
        return true;
    if (b[UFootprint] & 8)
        return false;
    return !(fogged_at(tile(x + 1, y, false)) && fogged_at(tile(x, y + 1, false)) &&
             fogged_at(tile(x + 1, y + 1, false)));
}

/// Defeat condition index (0xAAB80): G+0x453, except in classic mode (0x3DDE10: S+0x2449D == 1)
/// on the maps 0x3D..0x50 / 0x51..0x55, where the lord rule gives 0x14 / 0x15 / 0x16 + k / the
/// table 0xCDC63C[k] from Byleth and the route's lord (pid 2 / 3 / 4 for route 0|3 / 1 / 2): from
/// the save records while G+0x3D5E is set (map SC+8), else from the map units (map S+0x244A0).
int Reader::DefeatIndex(u64 s, u8 g453, bool live, const std::vector<Unit>& units) {
    const auto classic = Get<u8>(s + SClassic);
    if (!classic)
        return -1;
    if (*classic != 1)
        return g453;
    u32 m = 0;
    if (live) {
        const auto v = Get<u16>(gl.sc + ScMap);
        if (!v)
            return -1;
        m = *v;
    } else {
        const auto v = Get<u8>(s + SMap);
        if (!v)
            return -1;
        m = *v;
    }
    if (!DefeatMapRange(m))
        return g453; // map 0x62 also returns G+0x453 (w20 = 0)
    const auto route = Get<s8>(s + SRoute);
    if (!route)
        return -1;
    const int k = *route == 1 ? 3 : *route == 2 ? 4 : 2;
    const bool lord_route = *route >= 0 && *route <= 3;
    bool w21 = false, w9 = false;
    if (live) {
        const auto* ps = Persons(s);
        if (!ps)
            return -1;
        const auto find = [&](int pid) { return FindPerson(*ps, pid); };
        if (const u8* p = find(1))
            w21 = (p[PRoster2] & 4) && !(p[PRoster2] & 0x10);
        if (lord_route)
            if (const u8* p = find(k))
                w9 = (p[PRoster2] & 4) && !(p[PRoster2] & 0x10);
    } else {
        for (const Unit& un : units) {
            const int pid = dsmod_sdk::Le16(un.b.data() + UPid);
            w21 |= pid <= 1;
            w9 |= lord_route && pid == k;
        }
    }
    const int w8 = k - 2;
    if (w9 && w21)
        return w8 + 0x16;
    if (!w21 && w9) {
        const auto v = Get<s32>(base + DefeatAltTable + static_cast<u64>(w8) * 4);
        return v ? *v : -1;
    }
    return 0x14 + (w21 ? 1 : 0);
}

/// Battle condition text: ESC 'S' '0' = the player's name; other markup is not drawn.
std::string Reader::BattleText(u32 index, bool& exact) {
    const std::string t = Text(TextPart2, index);
    std::string out;
    for (std::size_t i = 0; i < t.size(); ++i) {
        if (t[i] == '\x1b' && i + 2 < t.size() && t[i + 1] == 'S' && t[i + 2] == '0') {
            out += PersonName(0);
            exact = false; // the renderer's ESC 'S' handler is not traced
            i += 2;
            continue;
        }
        out.push_back(t[i]);
    }
    return StripMarkup(out);
}

void Reader::ReadOverview(Out& o, u64 s, const std::vector<Unit>& units, const std::array<s32, 4>& bounds) {
    o.I("bt.ov_ok", overview_ok ? 1 : 0);
    if (!overview_ok)
        return;
    std::array<u8, 4> gv{}; // G+0x450..+0x453: fog +0x451, victory +0x452, defeat +0x453
    const auto live = Get<u8>(gl.g + GDefeatLive);
    if (!Read(gl.g + 0x450, gv.data(), gv.size()) || !live)
        return;
    // counts (0x138EC4): list units shown (0x9CCE0), by army; untapped (0x138B08): army 0 and
    // (+0xD6 | +0xDB) b2 clear
    std::array<int, 4> n{};
    int untapped = 0;
    for (const Unit& un : units) {
        const u8* b = un.b.data();
        if (!((b[UFlags] | b[UExcl]) & 4) && b[UArmy] == 0)
            ++untapped;
        if (b[UArmy] <= 3 && UnitVisible(un, gv[GFogFlag - 0x450], bounds))
            ++n[b[UArmy]];
    }
    for (std::size_t a = 0; a < 4; ++a)
        o.I("bt.ov_n" + std::to_string(a), n[a]);
    o.I("bt.untapped", untapped);
    const auto turn = Get<u16>(gl.g + GTurn);
    o.I("bt.untapped_on", turn && *turn != 0 ? 1 : 0); // the box is drawn only when the turn != 0
    // texts: refreshed on a key change or every 60 samples
    const std::array<s64, 6> key{gv[2], gv[3], *live, Get<u8>(s + SQuestBattle).value_or(0xFF),
                                 Get<u16>(gl.sc + ScName).value_or(0xFFFF),
                                 Get<u8>(s + SClassic).value_or(0xFF)};
    if (!ov_cache || key != ov_key || samples - ov_at >= 60) {
        ov_cache = std::make_unique<Out>();
        ov_key = key;
        ov_at = samples;
        Out& t = *ov_cache;
        bool exact = true;
        // title (0x138AA0): quest battle S+0x2555F <= 0x96 -> quest title (0x3FA140), else
        // 0x3FA9D0(SC+0xA): chapter type 2 on the lord maps / 0x62 -> 100; part2[id]
        std::string title;
        const auto q = Get<u8>(s + SQuestBattle);
        const auto m = Get<u16>(gl.sc + ScName);
        if (q && *q <= 0x96) {
            title = Text(TextPart2, TxtBattleQuest + *q);
        } else if (m && *m <= 0x63) {
            u32 id = *m;
            const auto route = Get<s8>(s + SRoute);
            const auto chapter = Get<s32>(s + SChapter);
            const auto over = Get<u8>(s + SChapterType);
            const auto ch = route && chapter ? ChapterRecord(*route, *chapter) : std::nullopt;
            const auto ty = ch ? Get<u8>(*ch + ChType) : std::nullopt;
            if (ty && over) {
                const u8 type = *over == 0x64 ? *ty : *over;
                if (type == 2 && (DefeatMapRange(id) || id == 0x62))
                    id = 100;
                title = Text(TextPart2, id);
            } else {
                exact = false;
            }
        }
        t.T("bt.title", StripMarkup(title));
        t.T("bt.victory", BattleText(TxtConditions + gv[2], exact));
        const int d = DefeatIndex(s, gv[3], *live != 0, units);
        if (d == 0xAC || d == 0xAD)
            exact = false; // "%d turns pass": the count (0x550440) is not reproduced
        t.T("bt.defeat", d >= 0 ? BattleText(TxtConditions + static_cast<u32>(d), exact) : std::string{});
        t.I("bt.ov_exact", exact && d >= 0 ? 1 : 0);
    }
    for (const auto& [k, v] : ov_cache->ints)
        o.R(k, v);
    for (const auto& [k, v] : ov_cache->texts)
        o.R(k, v);
}

// ---- bt_goto: drive the native battle cursor with D-pad presses (manifest enforce rules) ----

/// One sample of the cursor drive. The module never writes the game: it raises bt.drv.pending
/// (the manifest's enforce_gate) and one bt.drv.<dir> for exactly one sample per step; the
/// package's enforce rules press that D-pad button (one short press = one tile, 0x1829E0). The
/// next press waits for the cursor to move and settle. Aborts on leaving the free cursor
/// (G+0x3CC0 != 0), the enemy phase, an open forecast, a wrong / missing move, or a timeout.
void Reader::DriveCursor(Out& o, bool battle) {
    int press = -1;
    // bt_goto_tap: the War map tap's world position, read in the sample after the action (host
    // values are readable only while the module samples). The map area is the 1024-px overview
    // frame with y up: tile (tx, ty) covers pixels [72 + 24 tx, 72 + 24 (tx + 1)) (MinimapTile
    // origin / pixels), so tx = floor((wx - 72) / 24), ty = floor((-wy - 72) / 24).
    if (drv_tap_pending) {
        drv_tap_pending = false;
        const double nan = std::numeric_limits<double>::quiet_NaN();
        const double wx = host.get_f64 ? host.get_f64(host.userdata, "@map_tap_x", nan) : nan;
        const double wy = host.get_f64 ? host.get_f64(host.userdata, "@map_tap_y", nan) : nan;
        if (!std::isfinite(wx) || !std::isfinite(wy)) {
            drv = {};
            drv.result = 3;
            drv.status = "refused: no map tap position (@map_tap_x / @map_tap_y not readable)";
        } else {
            const double fx = std::floor((wx - 72.0) / 24.0), fy = std::floor((-wy - 72.0) / 24.0);
            const s64 tx = fx >= 0 && fx < 32 ? static_cast<s64>(fx) : -1;
            const s64 ty = fy >= 0 && fy < 32 ? static_cast<s64>(fy) : -1;
            drv_tap = {tx, ty};
            if (tx < 0 || ty < 0 || !battle || !StartDrive(ty * 32 + tx)) {
                drv = {};
                drv.tx = static_cast<int>(tx);
                drv.ty = static_cast<int>(ty);
                drv.result = 3;
                drv.status = "refused: tile outside the map or not in the free cursor";
            }
        }
    }
    const auto finish = [&](int result, const char* why) {
        drv.active = false;
        drv.result = result;
        drv.status = why;
    };
    if (drv.active) {
        std::array<s32, 3> cur{}; // G+0, +4 x, +8 y
        const auto st = battle ? Get<s32>(gl.g + GState) : std::nullopt;
        const auto phase = battle ? Get<s8>(gl.g + GPhase) : std::nullopt;
        bool fc = false;
        if (const auto obj = Ptr(gl.ui_var)) {
            const auto a = Get<u8>(*obj + FcWindowOff + WinActive);
            fc = a && *a;
        }
        if (!battle || !st || !phase || !Read(gl.g, cur.data(), sizeof(cur))) {
            finish(3, "not in battle");
        } else if (*st != 0 || *phase != 0 || fc) {
            finish(3, "left the free cursor");
        } else if (++drv.samples > DrvMaxSamples) {
            finish(3, "timed out");
        } else {
            const int x = cur[1], y = cur[2];
            if (drv.waiting) {
                if (x != drv.px || y != drv.py) {
                    const bool right = (drv.dir == 0 && x == drv.px && y == drv.py - 1) ||
                                       (drv.dir == 1 && x == drv.px && y == drv.py + 1) ||
                                       (drv.dir == 2 && x == drv.px - 1 && y == drv.py) ||
                                       (drv.dir == 3 && x == drv.px + 1 && y == drv.py);
                    if (!right)
                        finish(3, "cursor moved unexpectedly");
                    else if (++drv.wait >= DrvSettle)
                        drv.waiting = false;
                } else if (++drv.idle > DrvBlocked) {
                    finish(3, "blocked (the cursor did not move)");
                }
            }
            if (drv.active && !drv.waiting) {
                if (x == drv.tx && y == drv.ty) {
                    finish(2, "reached");
                } else {
                    press = x != drv.tx ? (x < drv.tx ? 3 : 2) : (y < drv.ty ? 1 : 0);
                    drv.dir = press;
                    drv.px = x;
                    drv.py = y;
                    drv.waiting = true;
                    drv.wait = drv.idle = 0;
                    ++drv.presses;
                }
            }
        }
    }
    // the package's single enforce_gate: the drive, or the quests page (its qs.view sync rules)
    o.I("fe.enforce", drv.active || ui_page == PageQuests ? 1 : 0);
    for (int d = 0; d < 4; ++d)
        o.R(DrvKeys[d], press == d ? 1 : 0);
    // the drive's state (drv.result: 0 idle, 2 reached, 3 aborted; drv.status the reason) is
    // not published: the package binds only the press gates
}

bool Reader::StartDrive(s64 argument) {
    if (argument == -1) {
        drv_tap_pending = false;
        if (drv.active) {
            drv.active = false;
            drv.result = 3;
            drv.status = "cancelled";
        }
        return true;
    }
    if (argument < 0 || argument >= 32 * 32 || !battle_ok)
        return false;
    const int x = static_cast<int>(argument % 32), y = static_cast<int>(argument / 32);
    std::array<s32, 4> b{}; // G+0x3B18 xmin, ymin, xmax (excl), ymax (excl)
    const auto st = Get<s32>(gl.g + GState);
    const auto phase = Get<s8>(gl.g + GPhase);
    if (!Read(gl.g + GXMin, b.data(), sizeof(b)) || !st || !phase)
        return false;
    if (x < b[0] || y < b[1] || x >= b[2] || y >= b[3] || *st != 0 || *phase != 0)
        return false;
    drv = {};
    drv.active = true;
    drv.tx = x;
    drv.ty = y;
    drv.status = "driving";
    return true;
}

void Reader::ReadBattle(Out& o, u64 s) {
#ifndef NDEBUG
    const auto t_battle0 = std::chrono::steady_clock::now();
#endif
    const u64 g = gl.g;
    // G blocks, read twice
    struct GBlocks {
        std::array<u8, 0x10> head{};
        std::array<u8, 0x50> map{};                         // G+0x3B18..0x3B68
        std::array<u8, 0x14> turn{};                        // G+0x3D40..0x3D54
        std::array<u8, GBlockAEnd - GBlockA> fc{};          // G+0x1C90..0x1DE8
        bool operator==(const GBlocks&) const = default;
    };
    const auto read_g = [&](GBlocks& b) {
        return Read(g, b.head.data(), b.head.size()) &&
               Read(g + GXMin, b.map.data(), b.map.size()) &&
               Read(g + GTurn, b.turn.data(), b.turn.size()) &&
               Read(g + GBlockA, b.fc.data(), b.fc.size());
    };
    GBlocks gb, gb2;
    const auto mgr = Ptr(gl.unit_var);
    std::array<u64, MaxUnits> ptrs{}, ptrs2{};
    const bool g_ok = read_g(gb);
    if (!g_ok || !mgr || !Read(*mgr + ListUnits, ptrs.data(), sizeof(ptrs))) {
        char why[96];
        std::snprintf(why, sizeof(why), "%s G=%llx list=%llx", !g_ok ? "G unreadable" : !mgr ? "unit list null" : "unit slots unreadable",
                      static_cast<unsigned long long>(gl.g),
                      static_cast<unsigned long long>(mgr.value_or(0)));
        o.T("bt.diag", why);
        o.I("bt.ready", 0);
        return;
    }
    std::vector<Unit> units;
    for (const u64 p : ptrs) {
        if (!p)
            continue;
        Unit u;
        u.at = p;
        // the flag bytes first (+0xD6..+0xDB): only on-map units are read in full
        std::array<u8, 6> fl{};
        if (!Read(p + UFlags, fl.data(), fl.size()))
            continue;
        if ((fl[0] & 0x20) || (fl[0] & 3) != 3 || (fl[UExcl - UFlags] & 4))
            continue;
        if (!Read(p, u.b.data(), u.b.size()))
            continue;
        // on-map test (main+0x54C940): +0xD6 b5 clear, b0 and b1 set; +0xDB b2 clear; id valid
        const u8 f = u.b[UFlags];
        if ((f & 0x20) || (f & 3) != 3 || (u.b[UExcl] & 4))
            continue;
        if (dsmod_sdk::Le16(&u.b[UPid]) == 0xFFFF)
            continue;
        units.push_back(u);
    }
#ifndef NDEBUG
    const auto t_pass2 = std::chrono::steady_clock::now();
#endif
    // second pass: the same blocks again (torn -> not ready)
    bool stable = read_g(gb2) && gb == gb2 && Read(*mgr + ListUnits, ptrs2.data(), sizeof(ptrs2)) &&
                  ptrs == ptrs2;
    for (std::size_t i = 0; stable && i < units.size(); ++i) {
        std::array<u8, UnitBytes> again{};
        stable = Read(units[i].at, again.data(), again.size()) && again == units[i].b;
    }
    if (!stable) {
        o.T("bt.diag", "torn");
        o.I("bt.ready", 0);
        o.I("bt.torn", 1);
        return;
    }
    const auto g32 = [&](const u8* p) { return static_cast<s32>(dsmod_sdk::Le32(p)); };
    const auto m = [&](u64 off) { return &gb.map[off - GXMin]; };
    const auto fc = [&](u64 off) { return &gb.fc[off - GBlockA]; };
    o.I("bt.cx", g32(&gb.head[GCursorX]));
    o.I("bt.cy", g32(&gb.head[GCursorX + 4]));
    o.I("bt.w", g32(m(GXMax)) - g32(m(GXMin)));
    o.I("bt.h", g32(m(GYMax)) - g32(m(GYMin)));
    o.I("bt.turn", dsmod_sdk::Le16(&gb.turn[0]));
    o.I("bt.phase", static_cast<s8>(gb.turn[GPhase - GTurn]));
    // overview image: stage = SC+4, flag = minimap object +0x30544 (assets rule MinimapIndex)
    std::string img;
    const auto stage = Get<u32>(gl.sc + ScStage);
    const auto flag = Get<u8>(gl.minimap + MinimapFlag);
    if (stage && flag && fe3h_assets::MinimapIndex(*stage, *flag != 0) >= 0)
        img = fe3h_assets::BmapKey(*stage, *flag != 0);
    o.T("bt.img", img);

    // unit under the cursor = the cursor tile's occupant (main+0xA9AC4): tile = grid + y*0x600 +
    // x*0x30 for 0 <= x, y <= 0x1F, else tile 0; tile+0 = unit*. (G+0x3B60 is the acting /
    // forecast attacker, not the cursor's unit: live, forecast open on the Thief -> Eden.)
#ifndef NDEBUG
    const auto t_list = std::chrono::steady_clock::now();
#endif
    u64 under = 0;
    {
        const s32 cx = g32(&gb.head[GCursorX]), cy = g32(&gb.head[GCursorX + 4]);
        if (const auto grid = Ptr(gl.grid_var)) {
            const bool in = (cx | cy) >= 0 && cx <= 0x1F && cy <= 0x1F;
            const u64 tile = in ? *grid + static_cast<u64>(cy) * 0x600 + static_cast<u64>(cx) * 0x30
                                : *grid;
            under = Get<u64>(tile).value_or(0);
        }
    }
    int sel = -1, pick = -1;
    std::array<int, 4> per_army{};
    bt_last_units.clear();
    if (unit_cache.size() > 256)
        unit_cache.clear();
    for (std::size_t i = 0; i < units.size(); ++i) {
        const u8* b = units[i].b.data();
        const UnitKeys& key = UnitKeysFor(i);
        const int pid = dsmod_sdk::Le16(&b[UPid]);
        o.R(key.x, b[UX]);
        o.R(key.y, b[UY]);
        o.R(key.army, b[UArmy]);
        o.R(key.hp, b[UCurHp]);
        // derived per-unit values (max HP, names, portrait keys): recomputed when an input byte
        // changes, else at most every UnitCacheSamples samples (they read fixed data + text).
        // The cache is pruned before the loop (the published text references stay valid).
        const auto [it, fresh] = unit_cache.try_emplace(units[i].at);
        UnitDerived& d = it->second;
        std::array<u8, 12> k{b[UPid], b[UPid + 1], b[UClass], b[UHp], b[UHpExtra], b[UHpBonus],
                             b[UHpBonus + 1], b[UHpBonus + 2], b[UHpBonus + 3], b[UHpBar],
                             b[UGender], b[UArmy]};
        if (d.key != k || samples - d.at >= UnitCacheSamples) {
            d.key = k;
            // a unit seen for the first time gets an earlier first expiry by its list slot, so the
            // refreshes of a battle's units spread over the period instead of one sample
            d.at = fresh ? samples - i % UnitCacheSamples : samples;
            d.hpmax = UnitMaxHp(b, s);
            d.name = Names(pid, b[UClass]).first;
            d.cls = UnitClassName(b);
            // the small forecast diamond (the large one for armies 2 / 3: no small pane)
            const std::string still = UnitPortrait(b, s);
            const u32 army = b[UArmy];
            d.dport_s = still.empty() ? std::string{} : fe3h_assets::DportKey(army, still, army < 2);
        }
        o.R(key.hpmax, d.hpmax);
        o.R(key.acted, (b[UFlags] >> 2) & 1);
        o.R(key.lv, b[ULevel]);
        o.R(key.name, d.name);
        o.R(key.cls, d.cls);
        o.R(key.dport_s, d.dport_s);
        if (b[UArmy] < per_army.size())
            ++per_army[b[UArmy]];
        if (units[i].at == under)
            sel = static_cast<int>(i);
        if (bt_pick_at && units[i].at == bt_pick_at)
            pick = static_cast<int>(i);
        bt_last_units.push_back(units[i].at);
    }
    if (pick < 0)
        bt_pick_at = 0; // the tapped unit left the map: follow the cursor again
#ifndef NDEBUG
    const auto t_units = std::chrono::steady_clock::now();
#endif
    o.I("bt.n", static_cast<s64>(units.size()));
    for (std::size_t a = 0; a < per_army.size(); ++a)
        o.I("bt.n" + std::to_string(a), per_army[a]);
    ReadOverview(o, s, units,
                 {g32(m(GXMin)), g32(m(GYMin)), g32(m(GXMax)), g32(m(GYMax))});
    o.I("bt.pick", pick);
    const int shown = pick >= 0 ? pick : sel; // bt.s.*: the tapped unit, else the cursor's
    o.I("bt.s.idx", shown);
    if (shown >= 0) {
        const Unit& su = units[static_cast<std::size_t>(shown)];
        const u64 skey = dsmod_sdk::Fnv1a64(su.b.data(), su.b.size(), su.at);
        // the Details block (ReadDetails2) is refreshed on the sample after the unit window, so
        // one sample never pays for both (it shows the previous values for that one sample)
#ifndef NDEBUG
        const auto tc0 = std::chrono::steady_clock::now();
#endif
        if (!sel_cache || sel_key != skey || samples - sel_at >= UnitCacheSamples) {
            sel_cache = std::make_unique<Out>();
            sel_key = skey;
            sel_at = samples;
            ReadSelected(*sel_cache, su.b.data(), s, su.at);
            dt_pending = true;
#ifndef NDEBUG
            sel_max_us = std::max(sel_max_us, std::chrono::duration<double, std::micro>(
                                                  std::chrono::steady_clock::now() - tc0).count());
#endif
        } else if (dt_pending || !dt_cache) {
            dt_cache = std::make_unique<Out>();
            ReadDetails2(*dt_cache, su.b.data(), s);
            dt_pending = false;
#ifndef NDEBUG
            dt_max_us = std::max(dt_max_us, std::chrono::duration<double, std::micro>(
                                                std::chrono::steady_clock::now() - tc0).count());
#endif
        }
        for (const Out* c : {sel_cache.get(), dt_cache.get()}) {
            if (!c)
                continue;
            for (const auto& [k, v] : c->ints)
                o.R(k, v);
            for (const auto& [k, v] : c->texts)
                o.R(k, v);
        }
    }
#ifndef NDEBUG
    const auto t_sel = std::chrono::steady_clock::now();
#endif
    // forecast: the forecast window (FcWindow) is active (+0x21, WinActive) with its layout
    // (+0x80), which the draw routine 0x1435C0 requires together with both combat units
    bool fc_on = false;
    const u64 ua = dsmod_sdk::Le64(fc(GFcA)), ub = dsmod_sdk::Le64(fc(GFcB));
    if (const auto obj = Ptr(gl.ui_var)) {
        const u64 win = *obj + FcWindowOff;
        const auto active = Get<u8>(win + WinActive);
        const auto layout = Get<u64>(win + WinLayout);
        fc_on = active && *active && layout && *layout && ua && ub;
    }
    o.I("bt.fc.on", fc_on ? 1 : 0);
    if (fc_on) {
        const struct {
            const char* p;
            u64 unit, hit, dmg, crit, other_total, item, count;
        } sides[] = {{"bt.fc.a.", ua, GFcHitA, GFcDmgA, GFcCritA, GFcTotalB, GFcItemA, GFcCountA},
                     {"bt.fc.b.", ub, GFcHitB, GFcDmgB, GFcCritB, GFcTotalA, GFcItemB, GFcCountB}};
        // the attacker's combat-art line (0x144BB0): art record G+0x3B68 -> art id (0x41AB90)
        std::string art = Text(TextPart1, TxtNoArts);
        if (const auto rec = Get<u64>(g + GFcArt); rec && *rec) {
            int id = 0;
            for (int i = 0; i <= ArtMax; ++i) {
                const auto r = Record(art_tbl, i);
                if (r && *r == *rec) {
                    id = i;
                    break;
                }
            }
            if (const auto e = Entry(art_tbl, id))
                if (const auto t = Get<u32>(*e + 0x10))
                    art = Text(TextPart2, *t + TxtArt);
        }
        o.T("bt.fc.a.art", art);
        o.T("bt.fc.b.art", "");
        for (const auto& sd : sides) {
            const std::string p = sd.p;
            o.I(p + "hit", g32(fc(sd.hit)));
            o.I(p + "dmg", g32(fc(sd.dmg)));
            o.I(p + "crit", g32(fc(sd.crit)));
            // attacks per round (FcCount, CS side +0x7C): the native shows "x<n>" when n >= 2
            o.I(p + "count", g32(fc(sd.count)));
            std::array<u8, UnitBytes> b{};
            if (!Read(sd.unit, b.data(), b.size()))
                continue;
            const int pid = dsmod_sdk::Le16(&b[UPid]);
            o.T(p + "name", Names(pid, b[UClass]).first);
            {
                const std::string still = UnitPortrait(b.data(), s);
                o.T(p + "dport",
                    still.empty() ? std::string{} : fe3h_assets::DportKey(b[UArmy], still));
            }
            o.I(p + "hp", b[UCurHp]);
            const int maxhp = UnitMaxHp(b.data(), s);
            o.I(p + "hpmax", maxhp);
            // the forecast's HP bubble (0x145F30 / 0x1460B4): hp - the other side's total damage
            // (CS side +0x74), 0 when that reaches hp
            const s32 taken = g32(fc(sd.other_total));
            const int after = taken < b[UCurHp] ? b[UCurHp] - taken : 0;
            o.I(p + "hp_after", after);
            // per-mille of the forecast's max HP (bar fill / after-HP bubble position)
            const auto pm = [&](int v) { return maxhp > 0 ? std::clamp(v * 1000 / maxhp, 0, 1000) : 0; };
            o.I(p + "after_pm", pm(after));
            // the item line (0x144590): side +8 = the item used (a weapon slot, or a combat art's
            // pseudo item 6000..); none -> part1[0x13E] "None"
            const u64 item = dsmod_sdk::Le64(fc(sd.item));
            std::string wpn, icon;
            s64 uses = -1, max = -1;
            if (!item) {
                wpn = Text(TextPart1, TxtNoWeapon);
            } else if (const auto slot = Get<u32>(item)) {
                const int id = static_cast<int>(*slot & 0xFFFF);
                const int u = static_cast<int>((*slot >> 16) & 0xFF);
                wpn = ItemName(id);
                if (static_cast<u32>(id - 6000) >= 0x50) // arts draw another icon (0x5D9B10)
                    icon = ItemIcon(id, u);
                // uses / max (0xC5BA0), shown when the record's max <= 99
                const int rec_max = ItemMaxUses(id);
                if (rec_max >= 0 && rec_max <= 0x63) {
                    if (static_cast<u32>(id - 4000) <= 0x25) {
                        // spells: the unit's per-battle spell uses (+0x94 + slot of id in
                        // +0xA0..+0xAB) / max 0x4108C0
                        for (int k = 0; k < 12; ++k)
                            if (b[0xA0 + k] == id - 4000) {
                                uses = b[0x94 + k];
                                max = SpellMax(b.data(), s, k, id - 4000);
                                break;
                            }
                    } else {
                        uses = u;
                        max = rec_max;
                    }
                }
            }
            o.T(p + "wpn", wpn);
            o.T(p + "wpn_icon", icon);
            o.I(p + "wpn_uses", uses);
            o.I(p + "wpn_max", max);
        }
    }
    // danger area / ranges from the tile grid (one row = 32 tiles of 0x30 bytes), drawn by the
    // overlay images (DangerStore): all enemies' reach (+0x27 b0), marked enemies' reach (+0x27
    // b1), the hovered/selected unit's move tiles (+0x26 b0) and attack reach (+0x26 b1).
    // bt.dz_on = the game's own danger display toggle (G+0x3D52).
    o.I("bt.dz_on", gb.turn[GDangerOn - GTurn]);
    // bt.dz.native = the game's own ZR "Danger Area" toggle G+0x3D52 (0x182FC4; also flipped in
    // states 1 / 2 at 0x1853BC / 0x1980DC / 0x19C824, cleared on battle start 0x93A9C / 0x1941CC);
    // the minimap draws tile +0x27 b0 while it is set (0x10AEC0), the same tiles as bt.dz_img.
    o.I("bt.dz.native", gb.turn[GDangerOn - GTurn] ? 1 : 0);
#ifndef NDEBUG
    const auto t_grid = std::chrono::steady_clock::now();
    const auto us_ = [](auto a, auto b) { return std::chrono::duration<double, std::micro>(b - a).count(); };
    part_us[4] += us_(t_battle0, t_pass2);
    part_us[5] += us_(t_pass2, t_list);
    part_us[6] += us_(t_list, t_units);
    part_us[7] += us_(t_units, t_sel);
#endif
    DangerLayers layers{};
    if (const auto grid = Ptr(gl.grid_var)) {
        const int rows = std::clamp(g32(m(GYMax)), 0, 32);
        std::vector<u8> row(TileRow);
        for (int y = 0; y < rows; ++y) {
            if (!Read(*grid + static_cast<u64>(y) * TileRow, row.data(), row.size()))
                break;
            s64 dz = 0, mk = 0, mv = 0, at = 0;
            for (int x = 0; x < 32; ++x) {
                const u8 r = row[static_cast<std::size_t>(x) * TileSize + TileRange];
                const u8 d = row[static_cast<std::size_t>(x) * TileSize + TileDanger];
                dz |= s64{d & 1} << x;
                mk |= s64{(d >> 1) & 1} << x;
                mv |= s64{r & 1} << x;
                at |= s64{(r >> 1) & 1} << x;
            }
            layers.dz[static_cast<std::size_t>(y)] = static_cast<u32>(dz);
            layers.mk[static_cast<std::size_t>(y)] = static_cast<u32>(mk);
            layers.mv[static_cast<std::size_t>(y)] = static_cast<u32>(mv);
            layers.at[static_cast<std::size_t>(y)] = static_cast<u32>(at);
        }
    }
#ifndef NDEBUG
    part_us[3] += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t_grid).count();
#endif
    o.T("bt.dz_img", danger->Key(DangerStore::Dz, layers));
    o.T("bt.sel_img", danger->Key(DangerStore::Sel, layers));
    o.T("bt.mk_img", danger->Key(DangerStore::Mk, layers));
    o.I("bt.ready", 1);
}

// ---- danger / range overlay images ------------------------------------------------------------
std::string DangerStore::Key(Layer layer, const DangerLayers& l) {
    if (layer < Dz || layer > Mk)
        return {};
    std::array<u32, 64> m{};
    switch (layer) {
    case Dz:
        std::copy(l.dz.begin(), l.dz.end(), m.begin());
        break;
    case Mk:
        std::copy(l.mk.begin(), l.mk.end(), m.begin());
        break;
    case Sel:
        std::copy(l.mv.begin(), l.mv.end(), m.begin());
        std::copy(l.at.begin(), l.at.end(), m.begin() + 32);
        break;
    }
    if (std::all_of(m.begin(), m.end(), [](u32 v) { return v == 0; }))
        return {};
    auto& last = last_masks[layer];
    const u64 h = dsmod_sdk::Fnv1a64(reinterpret_cast<const u8*>(m.data()), sizeof(m),
                                     dsmod_sdk::Fnv1a64Step(dsmod_sdk::Fnv1a64Basis, u8(layer)));
    {
        std::scoped_lock lk{mutex};
        // A stable layer may have been evicted while another layer changed repeatedly.
        // Reinsert it before publishing its cached key; otherwise that layer never recovers.
        if (last.first == m && !last.second.empty() && entries.contains(h))
            return last.second;
        auto& slot = entries[h];
        slot = m;
        order.push_back(h);
        while (order.size() > 32) { // bounded: the last few layer states
            const u64 old = order.front();
            order.pop_front();
            if (std::count(order.begin(), order.end(), old) == 0)
                entries.erase(old);
        }
    }
    static constexpr const char* Names[] = {"dz", "sel", "mk"};
    char hex[17];
    std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(h));
    last = {m, std::string{"module:fe3h:danger/"} + Names[layer] + "/" + hex};
    return last.second;
}

/// Renders "fe3h:danger/<dz|sel|mk>/<hash>" (any thread): 1024 x 1024, transparent, tile (x, y)
/// filled at [72 + 24x, 72 + 24(x+1)) x [72 + 24y, ...), the overview map's frame
/// (fe3h_assets::MinimapTileOrigin / MinimapTilePx). Presentation colours (semi-transparent):
/// all-enemy reach purple, marked reach red, move blue, attack reach red.
bool DangerStore::Render(std::string_view key, std::vector<u8>& rgba, u32& w, u32& h) {
    if (!key.starts_with("fe3h:danger/"))
        return false;
    key.remove_prefix(12);
    const std::size_t slash = key.find('/');
    if (slash == std::string_view::npos)
        return false;
    const std::string_view name = key.substr(0, slash), hs = key.substr(slash + 1);
    u64 hash = 0;
    if (hs.size() != 16)
        return false;
    for (const char c : hs) {
        const int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (v < 0)
            return false;
        hash = hash << 4 | static_cast<u64>(v);
    }
    std::array<u32, 64> m{};
    {
        std::scoped_lock lk{mutex};
        const auto it = entries.find(hash);
        if (it == entries.end())
            return false;
        m = it->second;
    }
    struct Rgba {
        u8 r, g, b, a;
    };
    const bool sel = name == "sel";
    Rgba first{}, second{};
    if (name == "dz")
        first = {0x9A, 0x4C, 0xD8, 0x78};
    else if (name == "mk")
        first = {0xE0, 0x30, 0x30, 0x80};
    else if (sel) {
        first = {0x38, 0x78, 0xF0, 0x80};  // move
        second = {0xE0, 0x38, 0x38, 0x80}; // attack reach (where not a move tile)
    } else
        return false;
    constexpr u32 Size = 1024, Origin = fe3h_assets::MinimapTileOrigin,
                  Px = fe3h_assets::MinimapTilePx;
    w = h = Size;
    rgba.assign(static_cast<std::size_t>(Size) * Size * 4, 0);
    const auto fill = [&](int x, int y, Rgba c) {
        const u32 x0 = Origin + Px * static_cast<u32>(x), y0 = Origin + Px * static_cast<u32>(y);
        for (u32 yy = y0; yy < y0 + Px && yy < Size; ++yy)
            for (u32 xx = x0; xx < x0 + Px && xx < Size; ++xx) {
                u8* p = &rgba[(static_cast<std::size_t>(yy) * Size + xx) * 4];
                p[0] = c.r;
                p[1] = c.g;
                p[2] = c.b;
                p[3] = c.a;
            }
    };
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 32; ++x) {
            const bool a = (m[static_cast<std::size_t>(y)] >> x) & 1;
            const bool b = sel && ((m[32 + static_cast<std::size_t>(y)] >> x) & 1);
            if (a)
                fill(x, y, first);
            else if (b)
                fill(x, y, second);
        }
    return true;
}

} // namespace Fe3hReader
