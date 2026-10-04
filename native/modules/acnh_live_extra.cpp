// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#include "acnh_live_extra.h"

#include <cstring>

#include "acnh_pins.h"

namespace acnh::live {
namespace {

using XPin = CodePin;

// Exact instruction words of the 3.0.3 main (build ff1d1c05670db602...), main-relative offsets.
// SaveDataMgr singleton read: [g]+0x453 alt, +0x10 set, set[0]/set+0x130, vcall +0x38 (field tree)
const XPin XPinSaveMgr{"SaveMgr",
                       0x6e19d8,
                       {0xf0026c88, 0x91010108, 0xf9400108, 0xb40002a8, 0x39514d09, 0xf9400908,
                        0x37000109, 0xf9400100, 0xf9400008, 0xf9401d08, 0xd63f0100, 0xb40001a0,
                        0x9101c000, 0x14000008, 0xb4000148, 0xf9409900}};
// save object vtable slot 2 (decrypted main.dat) / slot 7 (field handle tree)
const XPin XPinSaveSlotData{"SaveSlotData", 0x260bd28, {0xf9400800, 0xd65f03c0}};
const XPin XPinSaveSlotTree{"SaveSlotTree", 0x260bd50, {0xf9401000, 0xd65f03c0}};
// Celeste: online session global [main+0x527d558] +0x28 / +0x80 == 1
const XPin XPinCeleste{"Celeste",
                       0x27d619c,
                       {0xf0015528, 0x91156108, 0xf9400108, 0xb40000c8, 0xf9401509, 0xb4000089,
                        0xb9408108, 0x7100051f, 0x54fffec0}};
// Celeste: GetDayOfWeek(game day) == VisitorNpc+0x74
const XPin XPinCelesteDay{"CelesteDay",
                          0x27d6218,
                          {0xb9400128, 0x13003d00, 0xb9407673, 0x13105d01, 0x13187d02, 0x945cb501,
                           0xb9000be0, 0xb9400be8, 0x6b08027f, 0x1a9f17e0}};
// visitor list: Celeste is called with the VisitorNpc handle (save handle +0x1bf240)
const XPin XPinVisitorHandle{"VisitorHandle", 0x24aab18, {0xa94026e8, 0x8b080120, 0x940cad79}};
// Daisy Mae: save handle +0x1a0b50 (Land.Shop), ShopLevel (<4 else 0), >= 2
const XPin XPinDaisyShop{"DaisyShop", 0x27d4494, {0x91468008, 0x912d4113}};
const XPin XPinDaisyLevel{"DaisyLevel",
                          0x27d44c4,
                          {0xa9402a69, 0xb8696949, 0x7100113f, 0x1a9f3129, 0xb81fc3a9, 0xb85fc3b4}};
const XPin XPinDaisyCmp{"DaisyCmp", 0x27d44fc, {0x71000a9f, 0x5400006a}};
// Harvey: Land flag SpnVisitMainField read through the Land EventFlag reader 0x263f0a0
const XPin XPinHarveyFlag{"HarveyFlag",
                          0x27d63a0,
                          {0xd000bea8, 0x9119ed08, 0x91004269, 0x910003e0, 0xa90023e9, 0x97f9a33b}};

enum : uint32_t { XVisit = 2, XSave = 4 };

const struct {
    const XPin* pin;
    uint32_t parts;
} XPins[] = {{&XPinSaveMgr, XSave | XVisit}, {&XPinSaveSlotData, XSave},
             {&XPinSaveSlotTree, XVisit},    {&XPinCeleste, XVisit},
             {&XPinCelesteDay, XVisit},      {&XPinVisitorHandle, XVisit},
             {&XPinDaisyShop, XVisit},       {&XPinDaisyLevel, XVisit},
             {&XPinDaisyCmp, XVisit},        {&XPinHarveyFlag, XVisit}};

} // namespace

bool ExtraReader::Resolve(const Guest& g) {
    resolved = false;
    dead = 0;
    main_base = g.Base();
    main_size = g.host.main_size;
    houses_ms = 0;
    if (!main_base)
        return false;
    for (const auto& x : XPins) {
        if (!PinMatches(g, *x.pin))
            dead |= x.parts;
    }
    const auto at = [&](const XPin& p, size_t a, size_t o) { return PinData(g, p, a, o); };
    save_mgr = at(XPinSaveMgr, 0, 1);
    session = at(XPinCeleste, 0, 1);
    if (!save_mgr)
        dead |= XSave | XVisit;
    if (!session)
        dead |= XVisit;
    resolved = true;
    return true;
}

void ExtraReader::Sample(const Guest& g, const LiveSnapshot& s, const Need& need,
                         ExtraSnapshot& out) {
    if (!resolved || g.Base() != main_base || g.host.main_size != main_size)
        Resolve(g);
    const auto q = [&](uint64_t a, uint64_t& v) { return a && g.Get(a, v); };

    // ---- caught critters: ItemCollectBit (personal 0x49938) ----
    out.collect_ok = false;
    if (need.book && s.personal_at) {
        out.collect_bits.resize(0x754);
        out.collect_ok =
            g.Read(s.personal_at + 0x49938, out.collect_bits.data(), out.collect_bits.size());
    }

    // ---- Nook Miles+ (personal) ----
    out.nmp_ok = false;
    if (need.today && s.personal_at) {
        uint16_t five = 0xFFFF;
        out.nmp_ok = g.Read(s.personal_at + 0x12728, out.bonus_v.data(), out.bonus_v.size());
        if (flag_five_quest >= 0 && flag_five_quest <= 0x7ff &&
            g.Get(s.personal_at + P::EventFlag + static_cast<uint64_t>(flag_five_quest) * 2, five))
            out.five_quest = five;
        else
            out.five_quest = -1;
    }

    // ---- map: villager houses (main.dat bytes = save object vtable slot 2 "ldr x0,[x0,#0x10]")
    // ----
    out.houses_ok = false;
    if (need.map && !(dead & XSave)) {
        uint64_t mgr = 0, set = 0, obj = 0, vt = 0, slot2 = 0, data = 0;
        uint8_t alt = 0;
        uint32_t ver[2] = {};
        if (q(save_mgr, mgr) && g.Get(mgr + 0x453, alt) && q(mgr + 0x10, set) &&
            q((alt & 1) ? set + 0x130 : set, obj) && q(obj, vt) && q(vt + 0x10, slot2) &&
            slot2 == main_base + XPinSaveSlotData.offset && q(obj + 0x10, data) &&
            g.Read(data, ver, sizeof ver) && ver[0] == 0xA0002u && ver[1] == 0xA0028u) {
            const uint64_t now = NowMs();
            if (houses_source != data) {
                houses_source = data;
                houses_ms = 0;
            }
            bool houses_read_ok = true;
            if (out.npc_houses.size() != 0xBD10 || now - houses_ms > 2000) {
                auto& h = houses_buf;
                h.resize(0xBD10);
                houses_read_ok = g.Read(data + 0x481D20, h.data(), h.size());
                if (houses_read_ok) {
                    if (h != out.npc_houses) {
                        out.npc_houses.swap(h);
                        ++out.houses_rev;
                    }
                    houses_ms = now;
                }
            }
            // Land.EventFlag u16[640]: the value of one flag, -1 when unknown
            const auto land = [&](int uid) {
                uint16_t v = 0;
                return uid >= 0 && uid < 640 &&
                               g.Get(data + 0x22ED00 + static_cast<uint64_t>(uid) * 2, v)
                           ? int{v}
                           : -1;
            };
            const int built = land(land_hotel_built), work = land(land_hotel_work);
            out.hotel_state = built < 0 || work < 0 ? -1 : built ? 2 : work ? 1 : 0;
            out.office_construction1 = land(land_office_c1);
            out.market_construction2 = land(land_market_c2);
            for (int k = 0; k < 3; ++k)
                out.museum_construction[k] = land(land_museum_c[k]);
            out.houses_ok = houses_read_ok && out.npc_houses.size() == 0xBD10;
            // mapzoom lane: players (account table + their personal data) and facility stages
            out.players.clear();
            uint32_t slots[16] = {};
            const bool have_slots = g.Read(mgr + 0x90, slots, sizeof slots);
            for (int k = 0; k < 8; ++k) {
                uint8_t acc[0x48];
                if (!g.Read(data + 0x1e34c0 + 0x48u * k, acc, sizeof acc))
                    continue;
                bool any = false;
                for (int b = 0; b < 16; ++b)
                    any = any || acc[b] != 0;
                if (!any)
                    continue;
                ExtraSnapshot::MapPlayer pl;
                pl.no = k;
                for (int c = 0; c < 10; ++c) {
                    const char16_t ch =
                        static_cast<char16_t>(acc[0x30 + c * 2] | (acc[0x31 + c * 2] << 8));
                    if (!ch)
                        break;
                    pl.name.push_back(ch);
                }
                // GetPersonal: the slot whose {id, valid} = {k, 1} -> [[mgr+0x10]+0x60+i*8]+0x10
                for (int i = 0; have_slots && i < 8; ++i)
                    if (slots[i * 2 + 1] == 1 && slots[i * 2] == static_cast<uint32_t>(k)) {
                        uint64_t pobj = 0, per = 0;
                        uint16_t days = 0;
                        if (q(set + 0x60 + i * 8, pobj) && q(pobj + 0x10, per) &&
                            g.Get(per + 0x1352e, days))
                            pl.days = days;
                        break;
                    }
                out.players.push_back(std::move(pl));
            }
            int32_t lv = 0;
            out.shop_level_map = g.Get(data + 0x48DA30, lv) ? lv : -1;
            out.tailor_level = g.Get(data + 0x48DA5C, lv) ? lv : -1;
            out.museum_level = g.Get(data + 0x491220, lv) ? lv : -1;
            out.camp_level = g.Get(data + 0x575CA8, lv) ? lv : -1;
        }
    }

    // ---- special visitors (main.dat through the code's own save handles) ----
    out.visitors_ok = false;
    out.visitors.clear();
    if (need.today && !(dead & XVisit) && s.clock_ok) {
        uint64_t mgr = 0, set = 0, obj = 0, vt = 0, slot7 = 0, tree = 0;
        uint8_t alt = 0;
        // SaveMgr: obj = mgr+0x453 bit0 ? [set+0x130] : [set]; tree = [obj+0x20] (vtable slot 7,
        // "ldr x0,[x0,#0x20]" pinned by the main reader); handles at tree+0x70+h = {base, off}
        bool ok = q(save_mgr, mgr) && g.Get(mgr + 0x453, alt) && q(mgr + 0x10, set) &&
                  q((alt & 1) ? set + 0x130 : set, obj) && q(obj, vt) && q(vt + 0x38, slot7) &&
                  slot7 == main_base + XPinSaveSlotTree.offset && q(obj + 0x20, tree);
        const auto handle = [&](uint64_t h, uint64_t& at) {
            uint64_t a = 0, b = 0;
            if (!q(tree + 0x70 + h, a) || !q(tree + 0x70 + h + 8, b))
                return false;
            at = a + b;
            return at != 0;
        };
        uint64_t visit = 0, shop = 0, flags = 0;
        ok = ok && handle(0x1bf240, visit) && handle(0x1a0b50, shop) && handle(0xe75f0, flags);
        int32_t cel = -1, level = -1;
        uint64_t sess = 0, s28 = 0;
        int32_t s80 = 0;
        ok = ok && g.Get(visit + 0x74, cel) && g.Get(shop, level);
        out.online = q(session, sess) && sess && q(sess + 0x28, s28) && s28 &&
                     g.Get(sess + 0x80, s80) && s80 == 1;
        uint16_t spn = 0, gev = 0;
        const bool flags_ok = land_spn_visit >= 0 && land_spn_visit < 640 &&
                              land_global_event >= 0 && land_global_event < 640 &&
                              g.Get(flags + static_cast<uint64_t>(land_spn_visit) * 2, spn) &&
                              g.Get(flags + static_cast<uint64_t>(land_global_event) * 2, gev);
        // K.K. inputs (Publisher::KkDay): the Land flag array through the same handle as Harvey's,
        // LandTemp through 0x2643000's node, the players through the account table + GetPersonal
        out.kk_ok = false;
        out.kk_players.clear();
        uint64_t temp = 0;
        out.kk_land.assign(640, 0);
        out.kk_temp.assign(128, 0);
        bool kk = ok && flag_kk_count >= 0 && flag_kk_count < 0x800 && handle(0x205208, temp) &&
                  g.Read(flags, out.kk_land.data(), out.kk_land.size() * 2) &&
                  g.Read(temp, out.kk_temp.data(), out.kk_temp.size() * 2);
        uint64_t vt2 = 0, slot2 = 0, data = 0;
        uint32_t slots[16] = {};
        kk = kk && q(obj, vt2) && q(vt2 + 0x10, slot2) &&
             slot2 == main_base + XPinSaveSlotData.offset && q(obj + 0x10, data) &&
             g.Read(mgr + 0x90, slots, sizeof slots);
        // the handles must land where the file layout says (live check of the offsets, else closed)
        kk = kk && flags == data + 0x22ED00 && temp == data + 0x5785A0;
        for (int k = 0; kk && k < 8; ++k) {
            uint8_t uid[16];
            if (!g.Read(data + 0x1e34c0 + 0x48u * k, uid, sizeof uid)) {
                kk = false;
                break;
            }
            bool used = false;
            for (const uint8_t b : uid)
                used = used || b != 0;
            if (!used)
                continue;
            ExtraSnapshot::KkPlayer pl;
            pl.no = k;
            for (int i = 0; i < 8; ++i)
                if (slots[i * 2 + 1] == 1 && slots[i * 2] == static_cast<uint32_t>(k)) {
                    uint64_t pobj = 0, per = 0;
                    uint16_t cnt = 0, y = 0;
                    uint8_t md[2] = {};
                    if (q(set + 0x60 + i * 8, pobj) && q(pobj + 0x10, per) &&
                        g.Get(per + P::EventFlag + static_cast<uint64_t>(flag_kk_count) * 2, cnt) &&
                        g.Get(per + 0x36A1A, y) && g.Read(per + 0x36A1C, md, 2)) {
                        pl.count = cnt;
                        pl.y = y;
                        pl.m = md[0];
                        pl.d = md[1];
                    }
                    break;
                }
            if (pl.count < 0)
                kk = false; // a player whose personal data is not loaded: K.K. unknown
            out.kk_players.push_back(pl);
        }
        out.kk_ok = kk;
        if (ok) {
            out.visitors_ok = true;
            // Celeste: the game day's weekday (the same day TodayVisitor used: s.visitor_wday)
            if (!out.online && s.visitor_ok && s.visitor_wday >= 0 && cel == s.visitor_wday)
                out.visitors.emplace_back("ows");
            // Daisy Mae: ShopLevel (< 4, else 0) >= 2 and the calendar date (no 5 AM shift) is a
            // Sunday
            const int lv = level >= 0 && level < 4 ? level : 0;
            if (lv >= 2 && Weekday(s.y, s.mo, s.d) == 0)
                out.visitors.emplace_back("boc");
            // Harvey
            if (!out.online && flags_ok && spn != 0 && gev == 0xFFFF)
                out.visitors.emplace_back("spn");
        }
    }
}

} // namespace acnh::live
