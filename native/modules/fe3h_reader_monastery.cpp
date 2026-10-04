// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Fire Emblem: Three Houses 1.2.0 reader (fe3h_reader.h): the monastery: GPS, People table, map markers and icons.

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
#include <set>
#include <unordered_set>

namespace Fe3hReader {

// ---- monastery: GPS, People, dialogue ---------------------------------------------------------

/// Reads n bytes of each object; objects laid out close together (the actor pool, the area
/// records) are fetched with one read per run instead of one per object.
void Reader::ReadObjects(std::span<const u64> at, std::size_t n,
                         const std::function<u8*(std::size_t)>& dst, std::vector<bool>& ok) {
    ok.assign(at.size(), false);
    std::vector<u8> span;
    std::size_t i = 0;
    while (i < at.size()) {
        if (!at[i]) {
            ++i;
            continue;
        }
        std::size_t j = i;
        while (j + 1 < at.size() && at[j + 1] > at[j] && at[j + 1] - at[j] <= 0x400 &&
               at[j + 1] + n - at[i] <= 0x40000)
            ++j;
        const u64 len = at[j] + n - at[i];
        span.resize(len);
        if (j > i && Read(at[i], span.data(), len)) {
            for (std::size_t k = i; k <= j; ++k) {
                std::memcpy(dst(k), span.data() + (at[k] - at[i]), n);
                ok[k] = true;
            }
        } else {
            for (std::size_t k = i; k <= j; ++k)
                ok[k] = Read(at[k], dst(k), n);
        }
        i = j + 1;
    }
}

std::string Reader::LocationName(const u8* rec, u8 part, u8 route) {
    // (0x201660's rec+0 b1 branch is its "cannot fast-travel there" notice, not the name)
    const bool two = rec[1] == 0x15 ? (part == 1 && route == 3) : part != 0;
    const u8 k = rec[two ? 0x11 : 0x10];
    return k <= 0x63 ? Text(TextPart1, TxtLocation + k) : std::string{};
}

/// The affiliation index (0x40F330): Part II roster persons by route (AffilPartTwo), Byleth /
/// Jeralt / Sothis by the chapter's route label, else persondata +0x24. -1 = unreadable.
int Reader::AffiliationIndex(int pid, u64 s) {
    if (!affil_ok)
        return -1;
    int a = -1;
    if (const auto* ps = Persons(s)) {
        if (const u8* p = FindPersonExact(*ps, pid)) {
            const int id = pid;
            const auto part = Get<u8>(s + SPart);
            const auto route = Get<s8>(s + SRoute);
            if (!part || !route)
                return -1;
            if (*part == 1 && (p[PRoster] & 2) && *route >= 0 && *route < 4) {
                const auto v = Get<u32>(base + AffilPartTwo + static_cast<u64>(*route) * 4);
                if (!v)
                    return -1;
                a = static_cast<int>(*v & 0xFF);
            } else if (id <= 0x23 && ((0x800000003ULL >> id) & 1)) {
                const auto chapter = Get<s32>(s + SChapter);
                const auto ch = chapter ? ChapterRecord(*route, *chapter) : std::nullopt;
                const auto k = ch ? Get<u8>(*ch + ChRouteLabel) : std::nullopt;
                if (!k)
                    return -1;
                if (*k == 5)
                    a = 0x10;
            }
        }
    }
    if (a < 0) {
        const auto rec = Record(persons_tbl, pid);
        const auto v = rec && *rec ? Get<u8>(*rec + PdAffil) : std::nullopt;
        if (!v)
            return -1;
        a = *v;
    }
    return a;
}

bool Reader::TalkOpen(bool& open) {
    open = false;
    const auto tm = Get<u64>(gl.tm_var);
    const auto t = Get<u64>(gl.t_var);
    if (!tm || !t)
        return false;
    if (*tm) {
        const auto v = Get<u8>(*tm + TmOpen);
        if (!v)
            return false;
        open |= *v != 0;
    }
    if (*t) {
        const auto v = Get<u8>(*t + TOpen);
        if (!v)
            return false;
        open |= *v != 0;
    }
    return true;
}

void Reader::ReadGps(Out& o, u64 s, u64 f) {
    const auto fail = [&](const char* why) {
        o.I("gps.ready", 0);
        o.T("gps.diag", why);
    };
    // one read each: S+0x2427C chapter .. +0x244A1 map id; F+0x380 player / +0x398 walking
    // state; F+0x28020 flags / +0x28023 fixed spots
    std::array<u8, SChapterType + 1 - SChapter> sb{};
    std::array<u64, 4> fp{};
    std::array<u8, 4> fb{};
    if (!f)
        return fail("no field controller");
    if (!Read(s + SChapter, sb.data(), sb.size()))
        return fail("save unreadable");
    if (!Read(f + FPlayer, fp.data(), sizeof(fp)) || !Read(f + FFlags, fb.data(), fb.size()))
        return fail("player unreadable");
    const auto chapter = std::optional<s32>{static_cast<s32>(dsmod_sdk::Le32(sb.data()))};
    MapCtx c;
    c.route = sb[SRoute - SChapter];
    c.part = sb[SPart - SChapter];
    c.m = sb[SChapterType - SChapter];
    if (c.m == 100) { // MapIs13 / MapIs46: the chapter's map when no override
        const s64 key = (s64{c.route} << 32) | static_cast<u32>(*chapter);
        std::optional<u8> t;
        if (key == gps_map_key && samples - gps_map_at < 60) {
            t = gps_map_type;
        } else {
            const auto ch = ChapterRecord(static_cast<s8>(c.route), *chapter);
            t = ch ? Get<u8>(*ch + ChType) : std::nullopt;
            if (t) {
                gps_map_key = key;
                gps_map_at = samples;
                gps_map_type = *t;
            }
        }
        if (!t)
            return fail("chapter record");
        c.m = *t;
    }
    const auto ff = std::optional<u8>{fb[0]};
    const auto fixed = std::optional<u8>{fb[FFixed - FFlags]};
    const auto kind = (*ff & 1) ? Get<u32>(f + FKind) : Get<u32>(gl.field_kind + 4);
    const auto pp = std::optional<u64>{fp[0]};
    if (!kind || !*pp)
        return fail("player unreadable");
    c.kind = *kind;
    // MinimapTex (0x30B900): sprite 0xF60 / 0xF61 / 0xF6A / 0xF5E + part; extent 32000 for
    // map 13 else 64000; texture 512 px for maps 13 / 0x46 else 1024
    double extent = 64000.0, px = 1024.0;
    if (c.m == 13) {
        c.tex = 2;
        extent = 32000.0;
        px = 512.0;
    } else if (c.m == 0x46) {
        c.tex = 3;
        px = 512.0;
    } else if (c.kind == KindAbyss) {
        c.tex = 5;
    } else {
        c.tex = c.part != 0 ? 1 : 0;
    }
    c.scale = extent / px;
    std::array<float, 0x15> pb{}; // P+0x00..+0x53
    if (!Read(*pp, pb.data(), sizeof(pb)))
        return fail("player unreadable");
    float x = pb[PPosX / 4], z = pb[PPosZ / 4], yaw = pb[PYaw / 4];
    if (*fixed & 1) { // ArrowPos: fixed spots while F+0x28023 b0 / b1
        x = 4750.0f;
        z = 31000.0f;
    } else if (*fixed & 2) {
        x = 4750.0f;
        z = 32000.0f;
    }
    if (!std::isfinite(x) || !std::isfinite(z) || !std::isfinite(yaw))
        return fail("player position not finite");
    if (yaw > Pi)
        yaw -= TwoPi;
    else if (yaw <= -Pi)
        yaw += TwoPi;
    // the map arrow points (sin yaw, cos yaw) in (x, z): clockwise from up = pi - yaw
    double deg = (static_cast<double>(Pi) - yaw) * 180.0 / 3.14159265358979323846;
    deg = std::fmod(deg, 360.0);
    if (deg < 0)
        deg += 360.0;
    const int k = static_cast<int>(std::lround(deg / 5.0)) % 72;
    o.I("gps.ready", 1);
    o.T("gps.area", "abbey" + std::to_string(c.tex));
    o.F("gps.x", x / c.scale);
    o.F("gps.y", z / c.scale);
    o.I("gps.arrow_k", k); // module:fe3h:gpsarrow/<5 k> in the package's arrow cells
    // LocTracker (0x1F68B0): W+0x2C, maintained only on Garreg Mach / the Abyss
    int loc = 100;
    if (c.kind == KindGarregMach || c.kind == KindAbyss) {
        const u64 w = fp[(FWalk - FPlayer) / 8];
        const auto v = w ? Get<u32>(w + WLocation) : std::nullopt;
        if (v && *v < 100)
            loc = static_cast<int>(*v);
    }
    // the current location's rect (LocContains 0x405E00): fixed 24 s1 row {+0 z, +2 x, +4 z, +6 x}
    // u16 cells, inclusive; cell = trunc(pos / 250 - F+0x610 / +0x614) (WorldToCell 0x1D7700)
    {
        std::array<s32, 2> org{};
        const bool ok = loc < 100 && LocCells() && Read(f + FCellOrigin, org.data(), sizeof(org));
        if (ok) {
            const auto& r = loc_cells[static_cast<std::size_t>(loc)];
            const double sc = 250.0 / c.scale;
            o.F("gps.room_x0", (org[0] + std::min(r[1], r[3])) * sc);
            o.F("gps.room_x1", (org[0] + std::max(r[1], r[3]) + 1) * sc);
            o.F("gps.room_y0", (org[1] + std::min(r[0], r[2])) * sc);
            o.F("gps.room_y1", (org[1] + std::max(r[0], r[2]) + 1) * sc);
        }
    }
    o.T("gps.loc_name", loc < 100 ? Text(TextPart2, TxtLocPopup + static_cast<u32>(loc)) : "");
    o.I("gps.loc", loc);
    if (ui_page != PageMonastery) {
        // the People / marker / icon lists only feed the monastery page
        people_job.reset();
        people_cache.reset();
    } else {
        // refresh at 2 Hz, one StepPeople per sample; the last complete result is republished
        if (people_scale != c.scale) {
            people_job.reset();
            people_cache.reset();
        }
        ++people_age;
        if (!people_job && (!people_cache || people_age >= 30)) {
            people_job = std::make_unique<PeopleJob>();
            people_age = 0;
            people_scale = c.scale;
        }
        if (people_job) {
#ifndef NDEBUG
            const auto t_people = std::chrono::steady_clock::now();
#endif
            int kind = 0;
            StepPeople(s, f, c.scale, kind);
#ifndef NDEBUG
            const double pus = std::chrono::duration<double, std::micro>(
                                   std::chrono::steady_clock::now() - t_people)
                                   .count();
            people_us += pus;
            people_max = std::max(people_max, pus);
            auto& part_max = people_parts[static_cast<std::size_t>(std::clamp(kind, 0, 7))];
            part_max = std::max(part_max, pus);
            ++people_n;
#endif
        }
        if (people_cache)
            o.pre.push_back(people_cache.get()); // prefiltered: published without Wants
    }
}



/// Step 0: the save / DLC / zone grid / area table / person reads and the actor pointer list.
bool Reader::PeopleGather(PeopleJob& j, u64 s, u64 f) {
    std::array<u8, 0x14> sb{}; // S+0x2449E .. S+0x244B1
    const auto chapter = Get<s32>(s + SChapter);
    if (!chapter || !Read(s + SRoute, sb.data(), sb.size()))
        return j.fail = "save", false;
    j.chapter = *chapter;
    j.route = sb[0];
    j.part = sb[1];
    j.m = sb[3];
    if (j.m == 100) {
        const auto ch = ChapterRecord(static_cast<s8>(j.route), *chapter);
        const auto t = ch ? Get<u8>(*ch + ChType) : std::nullopt;
        if (!t)
            return j.fail = "chapter record", false;
        j.m = *t;
    }
    j.anna_flag = (sb[SEventFlags - SRoute + (FlagAnna >> 3)] >> (FlagAnna & 7)) & 1;
    j.anna_var = (sb[SAnnaVar - SRoute] >> 5) & 1;
    const auto dlc = Ptr(gl.dlc_var);
    std::array<u8, DlcFlags + 1> db{};
    if (!dlc || !Read(*dlc, db.data(), db.size()))
        return j.fail = "dlc", false;
    j.dlc_anna = (db[DlcFlags] >> 6) & 1;
    j.dlc_2af = (dsmod_sdk::Le32(db.data() + DlcFlags2) >> 23) & 1;
    // zone grid (500-unit cells; the game indexes up to 180*180 + 180)
    j.grid.resize(180 * 180 + 181);
    const auto fixed = Get<u8>(f + FFixed);
    if (!fixed || !Read(f + FZone, j.grid.data(), j.grid.size()))
        return j.fail = "zone grid", false;
    j.fixed = *fixed;
    return true;
}

/// Step 1: the area table and location list, the actor pointers, the save persons.
bool Reader::PeopleGather2(PeopleJob& j, u64 s, u64 f) {
    // area records and the location list
    const auto a = Ptr(gl.area_var);
    if (!a)
        return j.fail = "area table", false;
    std::array<u64, AreaRecMax> rp{};
    const auto n = Get<s32>(*a + AreaCount);
    if (!n || *n < 0 || *n > AreaRecMax || !Read(*a + AreaRecs, rp.data(), sizeof(rp)))
        return j.fail = "area table", false;
    j.list.resize(static_cast<std::size_t>(*n));
    if (*n && !Read(*a + AreaList, j.list.data(), j.list.size() * 4))
        return j.fail = "area list", false;
    {
        std::vector<bool> ok;
        ReadObjects(std::span<const u64>{rp}, AreaRecBytes,
                    [&](std::size_t i) { return j.recs[i].data(); }, ok);
        for (std::size_t i = 0; i < rp.size(); ++i)
            if (!ok[i])
                return j.fail = "area record", false;
    }
    // actor pointers (the actors themselves are read over the next steps)
    const auto mgr = Ptr(gl.actor_var);
    if (!mgr)
        return j.fail = "actor manager", false;
    const auto cnt = Get<s32>(*mgr + ActCount);
    if (!cnt)
        return j.fail = "actor manager", false;
    j.actors.resize(static_cast<std::size_t>(std::clamp(*cnt, 0, ActMax)));
    if (!j.actors.empty() && !Read(*mgr + ActList, j.actors.data(), j.actors.size() * 8))
        return j.fail = "actor list", false;
    j.ab.resize(j.actors.size());
    j.ab_ok.assign(j.actors.size(), false);
    // save persons (PersonRecord) for the roster flag
    j.ps.resize(static_cast<std::size_t>(MaxPersons) * PersonSize);
    j.ps_ok = Read(s + SPersons, j.ps.data(), j.ps.size());
    const auto player = Get<u64>(f + FPlayer);
    if (!player)
        return j.fail = "player", false;
    j.player = *player;
    return true;
}

/// The People table (PeopleBuilder 0x202A80) and the map markers (0x2B6548 / 0x2B7230) from the
/// snapshot, then the People / Facilities rows and the gps.p* markers.
void Reader::PeopleBuild(PeopleJob& j) {
    const auto zone = [&](float x, float z) -> int {
        const float cx = x / 500.0f, cz = z / 500.0f;
        if (!(cx > -2147483648.0f && cx < 2147483648.0f && cz > -2147483648.0f && cz < 2147483648.0f))
            return 100;
        const s32 ix = static_cast<s32>(cx), iz = static_cast<s32>(cz);
        if (static_cast<u32>(ix) > 180 || static_cast<u32>(iz) > 180)
            return 100;
        return j.grid[static_cast<std::size_t>(ix * 180 + iz)];
    };
    const auto person_rec = [&](s32 cid) -> const u8* {
        return j.ps_ok ? FindPerson(j.ps, cid) : nullptr;
    };
    const int m = j.m;
    const auto named = [&](s32 cid) {
        return FacilityNpc(cid) || cid == 0x3ED ||
               (m == 0x46 && (static_cast<u32>(cid - 0x3F3) < 2 || cid == 0x25)) || cid < 100 ||
               static_cast<u32>(cid - 0x410) < 7;
    };
    const auto npc_var = [&](s32 cid) -> int { // NpcVar (0x3E6480, w1 = 0); -1 = not reproduced
        if (cid == 0x416)
            return j.anna_var;
        if (cid == 0x2AF)
            return j.dlc_2af ? -1 : 0;
        return FacilityNpc(cid) ? 1 : 0;
    };
    const auto& actors = j.actors;
    const auto& ab = j.ab;
    const auto& ab_ok = j.ab_ok;
    for (std::size_t ai = 0; ai < actors.size(); ++ai) {
        if (!ab_ok[ai] || !ab[ai][ActActive])
            continue;
        const auto& b = ab[ai];
        const s32 cid = static_cast<s32>(dsmod_sdk::Le32(b.data() + ActCid));
        if ((b[ActHide1] & 0x10) || (b[ActHide2] & 0x10))
            continue;
        float ax, az;
        std::memcpy(&ax, b.data() + PPosX, 4);
        std::memcpy(&az, b.data() + PPosZ, 4);
        // map marker (0x2B6548 filter, 0x2B7230 sprite rule); not the player, not +0x158
        if (actors[ai] != j.player && !b[ActNoMarker]) {
            int kind = -1;
            if (named(cid)) {
                const int v = npc_var(cid);
                if (v < 0) {
                    j.exact = false;
                } else if (v) {
                    kind = 2;
                } else {
                    const u8* p = person_rec(cid);
                    kind = p && (p[PRoster] & 1) ? 0 : 1;
                }
            } else if (static_cast<s16>(dsmod_sdk::Le16(b.data() + ActTalk)) != -1 ||
                       dsmod_sdk::Le32(b.data() + ActTalkDist) <= 0x96) {
                // generic NPC: an event on the root of its +0xBE chain -> 0x7B9, else 0x7BA
                std::size_t root = ai;
                for (int step = 0; step < ActMax; ++step) {
                    const u16 up = dsmod_sdk::Le16(ab[root].data() + ActParent);
                    if (up == 0x12C || up > 0x1F3 || up >= actors.size() || !actors[up])
                        break;
                    if (!ab_ok[up]) {
                        root = actors.size();
                        break;
                    }
                    root = up;
                    if (dsmod_sdk::Le16(ab[up].data() + ActParent) == 0x12C)
                        break;
                }
                if (root == actors.size()) {
                    j.exact = false;
                } else {
                    bool event = false;
                    for (u64 off = ActEvents; off < ActEventsEnd; off += 2)
                        event |= static_cast<s16>(dsmod_sdk::Le16(ab[root].data() + off)) != -1;
                    kind = event ? 3 : 4;
                }
            }
            if (kind >= 0)
                j.markers.push_back({cid, ax, az, kind, ai});
        }
        // People table (PeopleBuilder)
        const bool fac = FacilityNpc(cid);
        if (fac) {
            if (static_cast<u32>(cid - 0x15B) < 4)
                continue;
        } else if (cid != 0x3ED) {
            const bool m46 = m == 0x46 && (static_cast<u32>(cid - 0x3F3) < 2 || cid == 0x25);
            if (!m46 && !(cid < 100 || static_cast<u32>(cid - 0x410) <= 6))
                continue;
        }
        float x = ax, z = az;
        if (static_cast<u32>(cid) <= 1 && (j.fixed & 1)) {
            x = 4750.0f;
            z = 31000.0f;
        } else if (static_cast<u32>(cid) <= 1 && (j.fixed & 2)) {
            x = 4750.0f;
            z = 32000.0f;
        }
        const int za = zone(x, z);
        int best = 100;
        float bd = 3.40282347e38f;
        for (const s32 id : j.list) {
            if (id == 0x41)
                continue;
            const auto& r = j.RecOf(id);
            if (!r[0x26])
                continue;
            if (RecF(r, 0x14) <= x && x <= RecF(r, 0x18) && RecF(r, 0x1C) <= z && z <= RecF(r, 0x20)) {
                best = id;
                break;
            }
            const float rx = RecF(r, 4), rz = RecF(r, 8);
            float d = std::sqrt((x - rx) * (x - rx) + (z - rz) * (z - rz));
            if (zone(rx, rz) != za)
                d += 10000.0f;
            if (d < bd) {
                bd = d;
                best = id;
            }
        }
        if (best == 100)
            continue;
        const int var = npc_var(cid);
        if (var < 0) {
            j.exact = false; // 0x2AF: 0x1D6E60's second test (0x3E9090) is not reproduced
            continue;
        }
        if (cid == 0x2AF && var == 0)
            continue;
        if (cid == 0x416 && !j.dlc_anna && !j.anna_flag)
            continue;
        const auto add = [&](int v) {
            auto& row = j.table[{best, v}];
            if (row.size() < static_cast<std::size_t>(PeopleSlots))
                row.push_back(j.found.size());
        };
        add(var);
        if (cid == 0x416 && var && j.dlc_anna)
            add(0);
        j.found.push_back({cid, best, var});
    }
}

/// The People / Facilities rows (native tab order) and the location chevrons.
void Reader::PeopleRows(PeopleJob& j, u64 s, Out& o) {
    // People tab rows (var 0), in the location list order (the package has no Facilities tab).
    // v5: headers carry the location id, the people count and "here" (= the player's LocTracker
    // location, gps.loc); person rows also the big diamond (dport) and "me" (Byleth); and the
    // same list paired for the 2-column People cards: gps.q{j} kind 0 = header (name, count,
    // loc, here), kind 1 = a pair of cards a. / b. (on, name, dport, me; b.on 0 = no 2nd card).
    int qi = 0;
    for (const s32 id : j.list) {
        if (id == 0x41)
            continue;
        const auto it = j.table.find({id, 0});
        if (it == j.table.end() || it->second.empty())
            continue;
        const std::string lname = LocationName(j.RecOf(id).data(), j.part, j.route);
        // "here" = the location the People rule itself puts Byleth (cid 0 / 1) in. (The
        // LocTracker id gps.loc indexes the popup / cell tables, not this area list: live, loc 0
        // "Personal Quarters" while area id 0 is the Cathedral.)
        int here = 0;
        for (const std::size_t fi : it->second)
            here |= static_cast<u32>(j.found[fi].cid) <= 1 ? 1 : 0;
        const auto count = static_cast<s64>(it->second.size());
        {
            const std::string r = "gps.r" + std::to_string(j.ri++) + ".";
            o.I(r + "kind", 0);
            o.T(r + "name", lname);
            o.T(r + "dport_s", "");
            o.T(r + "dport", "");
            o.I(r + "count", count);
            o.I(r + "loc", id);
            o.I(r + "here", here);
            const std::string q = "gps.q" + std::to_string(qi++) + ".";
            o.I(q + "kind", 0);
            o.T(q + "name", lname);
            o.I(q + "count", count);
            o.I(q + "loc", id);
            o.I(q + "here", here);
        }
        for (std::size_t n = 0; n < it->second.size(); ++n) {
            const s32 cid = j.found[it->second[n]].cid;
            const auto& pk = PersonKeys(cid, s);
            const int me = static_cast<u32>(cid) <= 1 ? 1 : 0;
            const std::string r = "gps.r" + std::to_string(j.ri++) + ".";
            o.I(r + "kind", 1);
            o.T(r + "name", pk[0]);
            o.T(r + "dport_s", pk[2]);
            o.T(r + "dport", pk[3]);
            o.I(r + "me", me);
            const bool second = n % 2 == 1;
            const std::string q = "gps.q" + std::to_string(second ? qi - 1 : qi++) + ".";
            if (!second) {
                o.I(q + "kind", 1);
                o.I(q + "b.on", 0);
                o.T(q + "b.name", "");
                o.T(q + "b.dport", "");
                o.I(q + "b.me", 0);
            }
            const std::string side = q + (second ? "b." : "a.");
            o.I(side + "on", 1);
            o.T(side + "name", pk[0]);
            o.T(side + "dport", pk[3]);
            o.I(side + "me", me);
        }
    }
    o.I("gps.qn", qi);
    // location chevrons (0x2B3EF0 list marks, sprite 0x7B5 at the label point): the map object's
    // +0x7108 list = every enabled location of the area list (0x202870, ids <= 0x63); the native
    // map skips the location selected in its list (+0x7104), which the companion has none of
    int ci = 0;
    for (const s32 id : j.list) {
        if (id < 0 || id > 0x63)
            continue;
        const auto& r = j.RecOf(id);
        if (!r[0x26])
            continue;
        const std::string k = "gps.c" + std::to_string(ci++) + ".";
        o.F(k + "x", RecF(r, 4) / j.scale);
        o.F(k + "y", RecF(r, 8) / j.scale);
    }
}

/// The gps.p* map markers.
void Reader::PeopleMarkers(PeopleJob& j, Out& o) {
    for (const auto& mk : j.markers) {
        const std::string k = "gps.p" + std::to_string(j.pi++) + ".";
        o.F(k + "x", mk.x / j.scale);
        o.F(k + "y", mk.z / j.scale);
        o.I(k + "kind", mk.kind);
        int icon_k = -1;
        if (mk.kind == 2) {
            const int sp = fe3h_assets::FacilityMarkerSprite(static_cast<u32>(mk.cid));
            if (sp >= 0) {
                const auto at = std::find(std::begin(IconSprites), std::end(IconSprites), sp);
                icon_k = at == std::end(IconSprites) ? -1
                                                     : static_cast<int>(at - std::begin(IconSprites));
            }
        }
        o.I(k + "icon_k", icon_k);
    }
}

/// {name, portrait, dport_s, dport} of a person, cached (rebuilt when the part, route or chapter
/// changes, else every 20 s: the name substitutions follow story flags).
const std::array<std::string, 4>& Reader::PersonKeys(s32 cid, u64 s) {
    auto it = person_keys_cache.find(cid);
    if (it == person_keys_cache.end()) {
        std::array<std::string, 4> k;
        k[0] = StripNameTags(PersonName(cid));
        k[1] = PortraitKey(cid, s);
        k[2] = k[1].empty() ? std::string{} : fe3h_assets::DportKey(0, k[1], true);
        k[3] = k[1].empty() ? std::string{} : fe3h_assets::DportKey(0, k[1], false); // v5 cards
        it = person_keys_cache.emplace(cid, std::move(k)).first;
    }
    return it->second;
}

/// Map icons (RE round 4 safe layers, the monastery page only): 1 facility, 2 quest over a
/// character, 3 quest location glow, 4 visitor bubble. Never reads the spot bit arrays
/// (M+0x4D3 / M+0x4F9), spotdata or the inventory.
void Reader::PeopleIcons(PeopleJob& j, u64 s, u64 f, Out& o) {
    int ii = 0;
    bool icons_ok = true;
    const double scale = j.scale;
    if (ui_page == PageMonastery) {
        const auto icon_k = [](int sprite) {
            const auto at = std::find(std::begin(IconSprites), std::end(IconSprites), sprite);
            return at == std::end(IconSprites) ? -1 : static_cast<int>(at - std::begin(IconSprites));
        };
        // kinds (not published): 1 facility, 2 quest over a character, 3 location glow, 4 visitor
        const auto icon = [&](double x, double z, int sprite, bool glow) {
            const std::string k = "gps.i" + std::to_string(ii++) + ".";
            o.F(k + "x", x / scale);
            o.F(k + "y", z / scale);
            o.I(k + "icon_k", sprite >= 0 ? icon_k(sprite) : -1);
            o.I(k + "glow", glow ? 1 : 0);
        };
        // quest states M+0x3F4[151] (QuestState) and the quest records (fixed data, cached)
        std::array<u8, QuestCount> qs{};
        const bool q_ok = Read(s + SM + MQuestState, qs.data(), qs.size()) && QuestRecords();
        const auto fstate = Get<s32>(f + FState);
        if (!q_ok || !fstate)
            icons_ok = false;
        const auto& qr = quest_recs;
        const auto giver = [&](int i) { return dsmod_sdk::Le16(qr[static_cast<std::size_t>(i)].data() + 4); };
        const auto red = [&](int i) { return qr[static_cast<std::size_t>(i)][0x2B] != 0; };
        const auto blue = [](int i) { return static_cast<u32>(i - 0x6D) < 0x2A; };
        // one pass over the quests: offered (state 1) by giver, targeted (state 2) by person, both in
        // ascending quest order like 0x406E70 / 0x4070B0 (targets: a 5-entry buffer)
        std::unordered_map<u32, std::vector<int>> by_giver, by_target;
        if (q_ok) {
            for (int i = 0; i < QuestCount; ++i) {
                const u8 st = qs[static_cast<std::size_t>(i)];
                if (st == 1)
                    by_giver[giver(i)].push_back(i);
                else if (st == 2)
                    for (const u64 off : {u64{0x14}, u64{0x16}, u64{0x18}}) {
                        auto& v = by_target[dsmod_sdk::Le16(qr[static_cast<std::size_t>(i)].data() + off)];
                        if (v.size() < 5)
                            v.push_back(i);
                    }
            }
        }
        static const std::vector<int> no_quests;
        const auto offers = [&](s32 cid) -> const std::vector<int>& {
            const auto it = by_giver.find(static_cast<u32>(cid));
            return it == by_giver.end() ? no_quests : it->second;
        };
        const auto targeted = [&](s32 cid) -> const std::vector<int>& {
            if (static_cast<u32>(cid) > 0x4B0)
                return no_quests;
            const auto it = by_target.find(static_cast<u32>(cid));
            return it == by_target.end() ? no_quests : it->second;
        };
        const auto state_of = [&](u32 q) { return q <= 0x96 ? qs[q] : u8{7}; };
        for (const auto& mk : j.markers) {
            const auto& b = j.ab[mk.ai];
            if (mk.kind == 2) {
                const int sp = fe3h_assets::FacilityMarkerSprite(static_cast<u32>(mk.cid));
                icon(mk.x, mk.z, sp, false);
            }
            if (!icons_ok)
                continue;
            // 0x1DF010: the quest gate
            const u8 d3 = b[ActHide2];
            const u32 q = dsmod_sdk::Le32(b.data() + ActTalkDist);
            const auto& tg = targeted(mk.cid);
            const auto& of = offers(mk.cid);
            bool gate = false;
            if (*fstate != 0x12) {
                gate = (d3 & 0x4A) || (q <= 0x96 && (state_of(q) == 1 || state_of(q) == 3)) ||
                       !of.empty() || !tg.empty();
            }
            if (!gate)
                continue;
            // 0x5D9CF0: the quest icon
            int sp = 0x7C0;
            if (!tg.empty()) {
                int pick = tg[0];
                for (const int i : tg)
                    if (red(i)) {
                        pick = i;
                        break;
                    }
                sp = blue(pick) ? 0x6FD : red(pick) ? 0x7C3 : 0x7C0;
            } else if (d3 & 0x4A) {
                sp = (d3 & 0x40) ? 0x7C3 : 0x7C0;
            } else if (q <= 0x96) { // 0x5D9F80
                const u8 st = state_of(q);
                int w = -1;
                if (st == 1)
                    w = blue(static_cast<int>(q)) ? 0x6FB : red(static_cast<int>(q)) ? 0x7C1 : 0x7BE;
                else if (st == 3)
                    w = blue(static_cast<int>(q)) ? 0x6FC : red(static_cast<int>(q)) ? 0x7C2 : 0x7BF;
                if (st == 1 || st == 3) {
                    for (const int i : of)
                        if (red(i)) {
                            w = blue(i) ? 0x6FB : 0x7C1;
                            break;
                        }
                } else if (!of.empty()) {
                    w = 0x7BE;
                    for (const int i : of) {
                        if (blue(i)) {
                            w = 0x6FB;
                            break;
                        }
                        if (red(i)) {
                            w = 0x7C1;
                            break;
                        }
                    }
                }
                sp = w;
            }
            icon(mk.x, mk.z, sp, true);
        }
        // location centre (0x405FE0 -> 0x1D7630): cell rect {z0, x0, z1, x1} -> world + 125
        std::array<s32, 2> f610{};
        const bool f610_ok = Read(f + FCellOrigin, f610.data(), sizeof(f610)) && LocCells();
        const auto centre = [&](int loc, double& x, double& z) {
            if (loc < 0 || loc > 0x63 || !f610_ok)
                return false;
            const auto& r = loc_cells[static_cast<std::size_t>(loc)];
            const u32 cx = std::min(r[1], r[3]) + (std::max(r[1], r[3]) - std::min(r[1], r[3])) / 2u;
            const u32 cz = std::min(r[0], r[2]) + (std::max(r[0], r[2]) - std::min(r[0], r[2])) / 2u;
            x = static_cast<float>((f610[0] + static_cast<s32>(cx)) * 250) + 125.0f;
            z = static_cast<float>((f610[1] + static_cast<s32>(cz)) * 250) + 125.0f;
            return true;
        };
        // quest location glow (0x2B5AF0 / 0x3EEC90)
        std::array<u8, 100> m366{};
        const auto fkind = Get<u32>(f + FKind);
        if (Read(s + SM + MQuestLoc, m366.data(), m366.size()) && fkind) {
            for (int loc = 0; loc < 100; ++loc) {
                if (m366[static_cast<std::size_t>(loc)] == 0xFF)
                    continue;
                if ((static_cast<u32>(loc - 0x55) < 0xF) != (*fkind == KindAbyss))
                    continue;
                double x, z;
                if (centre(loc, x, z))
                    icon(x, z, 0x7BB, true);
            }
        } else {
            icons_ok = false;
        }
        // visitor bubble (0x2B5910): not on the special maps 0x50..0x63 (0x3E6590)
        if (static_cast<u32>(j.m - 0x50) >= 0x14) {
            std::array<u16, 7> who{};
            std::array<s32, 7> slot{}, locs{};
            if (Read(s + SM + MVisitors, who.data(), sizeof(who)) &&
                Read(base + TalkspotSlots, slot.data(), sizeof(slot)) &&
                Read(base + TalkspotLocs, locs.data(), sizeof(locs))) {
                for (int k = 0; k < 7; ++k) {
                    const s32 sl = slot[static_cast<std::size_t>(k)];
                    const u16 pid = sl >= 0 && sl <= 6 ? who[static_cast<std::size_t>(sl)] : 0xFFFF;
                    if (pid > 0x4B0)
                        continue;
                    double x, z;
                    if (centre(locs[static_cast<std::size_t>(k)], x, z))
                        icon(x, z, 0x7CB, false);
                }
            } else {
                icons_ok = false;
            }
        }
    }
    o.I("gps.icons_ok", icons_ok ? 1 : 0);
}

/// The questdata records (fixed data: re-read when the manager changes, else every 20 s).
bool Reader::QuestRecords() {
    const auto mgr = Get<u64>(gl.quest_var);
    if (!mgr || !*mgr)
        return false;
    if (quest_recs_ok && *mgr == quest_recs_mgr)
        return true;
    quest_recs_ok = false;
    quest_recs.resize(QuestCount);
    for (int i = 0; i < QuestCount; ++i) {
        const auto rec = Record(quest_tbl, i);
        if (!rec || !*rec || !Read(*rec, quest_recs[static_cast<std::size_t>(i)].data(), QuestRecBytes))
            return false;
    }
    quest_recs_ok = true;
    quest_recs_mgr = *mgr;
    quest_recs_at = samples;
    return true;
}

/// The location cell rects {z0, x0, z1, x1} (fixed data, cached like the quest records).
bool Reader::LocCells() {
    const auto mgr = Get<u64>(gl.loccell_var);
    if (!mgr || !*mgr)
        return false;
    if (loc_cells_ok && *mgr == loc_cells_mgr)
        return true;
    loc_cells_ok = false;
    for (int i = 0; i < 100; ++i) {
        const auto rec = Record(loccell_tbl, i);
        if (!rec || !*rec || !Read(*rec, loc_cells[static_cast<std::size_t>(i)].data(), 8))
            return false;
    }
    loc_cells_ok = true;
    loc_cells_mgr = *mgr;
    loc_cells_at = samples;
    return true;
}

/// One step of the People refresh: 0 gather, 1..n actors (ActorsPerStep each), then the build
/// (table, rows, markers), then the icons; true when people_cache was replaced.
bool Reader::StepPeople(u64 s, u64 f, double scale, int& kind) {
    auto& j = *people_job;
    kind = j.step;
    const auto finish_fail = [&] {
        auto o = std::make_unique<Out>();
        o->I("gps.rn", 0);
        o->I("gps.people_ok", 0);
        o->T("gps.people_diag", j.fail);
        people_cache = std::move(o);
        people_job.reset();
        return true;
    };
    switch (j.step) {
    case PeopleJob::GatherA:
        j.scale = scale;
        if (!PeopleGather(j, s, f))
            return finish_fail();
        j.step = PeopleJob::GatherB;
        return false;
    case PeopleJob::GatherB: {
        if (!PeopleGather2(j, s, f))
            return finish_fail();
        const u64 pk_key = (u64{j.part} << 40) | (u64{j.route} << 32) | static_cast<u32>(j.chapter);
        // names follow the part / route / chapter (and NameEpoch, cleared per sample), else
        // every 20 s: the name substitutions follow story flags
        if (pk_key != person_keys_key || samples - person_keys_at >= 1200) {
            person_keys_cache.clear();
            person_keys_key = pk_key;
            person_keys_at = samples;
        }
        j.step = PeopleJob::Actors;
        return false;
    }
    case PeopleJob::Actors: {
        const std::size_t to = std::min(j.actors.size(), j.next_actor + ActorsPerStep);
        if (to > j.next_actor) {
            std::vector<bool> ok;
            ReadObjects(std::span<const u64>{j.actors.data() + j.next_actor, to - j.next_actor},
                        ActBytes, [&](std::size_t i) { return j.ab[j.next_actor + i].data(); }, ok);
            for (std::size_t i = 0; i < ok.size(); ++i)
                j.ab_ok[j.next_actor + i] = ok[i];
        }
        j.next_actor = to;
        if (j.next_actor >= j.actors.size())
            j.step = PeopleJob::Compute;
        return false;
    }
    case PeopleJob::Compute:
        PeopleBuild(j);
        j.step = PeopleJob::Warm;
        return false;
    case PeopleJob::Warm: {
        // names / portraits of new persons, at most PersonKeysPerStep per sample
        int filled = 0;
        const auto warm = [&](s32 cid) {
            if (filled < PersonKeysPerStep && !person_keys_cache.contains(cid)) {
                PersonKeys(cid, s);
                ++filled;
            }
        };
        for (const auto& p : j.found) // the People rows' names / portraits
            warm(p.cid);
        if (filled < PersonKeysPerStep) {
            // the location names (text cache) and, once, the fixed quest / cell data
            if (!quest_recs_ok || !loc_cells_ok) {
                QuestRecords();
                LocCells();
            }
            j.step = PeopleJob::Rows;
        }
        return false;
    }
    case PeopleJob::Rows:
        j.out = std::make_unique<Out>();
        PeopleRows(j, s, *j.out);
        j.step = PeopleJob::Markers;
        return false;
    case PeopleJob::Markers:
        PeopleMarkers(j, *j.out);
        j.step = PeopleJob::Icons;
        return false;
    case PeopleJob::Icons:
    default:
        break;
    }
    PeopleIcons(j, s, f, *j.out);
    PeopleLegend(*j.out);
    j.out->I("gps.rn", j.ri);
    j.out->I("gps.people_ok", 1);
    j.out->I("gps.people_exact", j.exact ? 1 : 0);
    // keep only the values the package binds: the per-sample republish skips the filter
    auto& o = *j.out;
    std::erase_if(o.ints, [&](const auto& kv) { return !filter.Wants(kv.first.c_str()); });
    std::erase_if(o.texts, [&](const auto& kv) { return !filter.Wants(kv.first.c_str()); });
    std::erase_if(o.floats, [&](const auto& kv) { return !filter.Wants(kv.first.c_str()); });
    people_cache = std::move(j.out);
    people_job.reset();
    return true;
}

/// v5 Legend tab: the marker / icon kinds on the map now (from this refresh's outputs).
///   gps.lgm{k}.kind   marker kinds present, in legend order: 6 player (always), 5 location
///                     chevron (any gps.c*), then the people dots gps.p*.kind 0 roster ("your
///                     class"), 1 other named, 3 event NPC, 4 townsfolk;  gps.lgm_n
///   gps.lgf{k}.icon_k facility icons 0..20 present (map icons gps.i* and facility NPC markers
///                     gps.p*.icon_k), ascending;  gps.lgf_n
///   gps.lgq{k}.icon_k quest / visitor icons 21..29, 31 present, plus 30 when a quest glow is on
///                     the map, ascending;  gps.lgq_n
void Reader::PeopleLegend(Out& o) {
    std::set<int> people, fac, quest;
    bool chevron = false;
    for (const auto& [k, v] : o.ints) {
        const bool p = k.rfind("gps.p", 0) == 0, i = k.rfind("gps.i", 0) == 0;
        if (p && k.ends_with(".kind")) {
            people.insert(static_cast<int>(v));
        } else if ((p || i) && k.ends_with(".icon_k")) {
            if (v >= 0 && v <= 20)
                fac.insert(static_cast<int>(v));
            else if (v >= 21 && v <= 31)
                quest.insert(static_cast<int>(v));
        } else if (i && k.ends_with(".glow") && v == 1) {
            quest.insert(30);
        }
    }
    for (const auto& kv : o.floats)
        chevron = chevron || kv.first.rfind("gps.c", 0) == 0;
    int n = 0;
    const auto marker = [&](int kind) { o.I("gps.lgm" + std::to_string(n++) + ".kind", kind); };
    marker(6);
    if (chevron)
        marker(5);
    for (const int kind : {0, 1, 3, 4})
        if (people.contains(kind))
            marker(kind);
    o.I("gps.lgm_n", n);
    n = 0;
    for (const int k : fac)
        o.I("gps.lgf" + std::to_string(n++) + ".icon_k", k);
    o.I("gps.lgf_n", n);
    n = 0;
    for (const int k : quest)
        o.I("gps.lgq" + std::to_string(n++) + ".icon_k", k);
    o.I("gps.lgq_n", n);
}

Reader::~Reader() = default;

} // namespace Fe3hReader
