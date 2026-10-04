// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// ACNH 3.0.3 live reader: pins, root decoding and sampling (acnh_live.h). The module-facing seam
// (LiveCreate ... in acnh_types.h) is at the bottom of this file; publishing is acnh_publish.cpp.

#include "acnh_live.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <mutex>
#include <string_view>

#include "acnh_bag.h"
#include "acnh_config.h"
#include "acnh_lang.h"
#include "acnh_pins.h"
#include "acnh_publish.h"
#include "acnh_types.h"
#include "core/mods/modules/dsmod_module_sdk.h"

namespace acnh::live {
namespace {

using Pin = CodePin; // the generated .inc names the type Pin
#include "acnh_live_pins.inc"

const Pin* const AllPins[] = {
    &PinSaveMgr,        &PinSaveSlotData, &PinSaveSlotTree, &PinGetPersonal,   &PinLocalPlayerNo,
    &PinClock,          &PinLang,         &PinLangTable,    &PinPlayerPos,     &PinDeviceSeq,
    &PinSmNames,        &PinSmChange,     &PinActorList,    &PinActorListComp, &PinPlayerActorName,
    &PinPlayerSm,       &PinCameraComp,   &PinCameraSm,     &PinAppTable,      &PinAppOrder,
    &PinAppInstalled,   &PinFlagRead,     &PinAppFrame,     &PinPhoneRoot,     &PinPhoneSeqSlot,
    &PinPhoneSeqVt,     &PinPhoneCursor,  &PinPhoneList,    &PinResOrderCall,  &PinResOrderKey,
    &PinDateToCalendar, &PinBookLoad,     &PinBookCell,     &PinVisitorCall,   &PinTodayVisitor,
    &PinVisitorGst,     &PinVisitorLabel, &PinPocketSlot,   &PinPocketVt,      &PinPocketCursor,
    &PinCaseTable, &PinStageName};

void AddDiag(std::string& diag, std::string_view what) {
    if (diag.size() > 400)
        return;
    if (!diag.empty())
        diag += ';';
    diag += what;
}

bool IsHeapPtr(uint64_t p) {
    return p >= 0x1000 && p < (uint64_t{1} << 39) && (p & 7) == 0;
}

std::u16string Utf16(const uint8_t* p, size_t units) {
    std::u16string s;
    for (size_t i = 0; i < units; ++i) {
        const char16_t c = static_cast<char16_t>(p[i * 2] | p[i * 2 + 1] << 8);
        if (c == 0)
            break;
        s += c;
    }
    return s;
}

// Pin -> domain: a pin mismatch closes only the domains that depend on it.
enum Domain : uint32_t {
    DSave = 1,
    DClock = 2,
    DLang = 4,
    DPos = 8,
    DPhone = 16,
    DPlayer = 32,
    DCamera = 64,
    DApps = 128,
    DOrder = 256,
    DVisitor = 512,
    DPocketUi = 1024,
    DText = 2048
};
uint32_t PinDomains(const Pin* p) {
    if (p == &PinSaveMgr || p == &PinSaveSlotData || p == &PinSaveSlotTree || p == &PinGetPersonal)
        return DSave;
    if (p == &PinLocalPlayerNo)
        return DSave | DPos;
    if (p == &PinClock)
        return DClock;
    if (p == &PinLang || p == &PinLangTable)
        return DLang;
    if (p == &PinPlayerPos || p == &PinStageName)
        return DPos;
    if (p == &PinDeviceSeq)
        return DPhone;
    if (p == &PinSmNames || p == &PinSmChange)
        return DPhone | DPlayer | DCamera;
    if (p == &PinAppTable || p == &PinAppOrder || p == &PinAppInstalled || p == &PinFlagRead ||
        p == &PinAppFrame || p == &PinPhoneRoot || p == &PinPhoneSeqSlot || p == &PinPhoneSeqVt ||
        p == &PinPhoneCursor || p == &PinPhoneList)
        return DApps;
    if (p == &PinCameraComp || p == &PinCameraSm)
        return DCamera;
    if (p == &PinResOrderCall || p == &PinResOrderKey || p == &PinDateToCalendar ||
        p == &PinBookLoad || p == &PinBookCell)
        return DOrder;
    if (p == &PinVisitorCall || p == &PinTodayVisitor || p == &PinVisitorGst ||
        p == &PinVisitorLabel)
        return DVisitor;
    if (p == &PinPocketSlot || p == &PinPocketVt || p == &PinPocketCursor)
        return DPocketUi;
    if (p == &PinCaseTable)
        return DText;
    return DPlayer | DCamera; // actor list, PlayerActor, player state machine
}

} // namespace

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

// ---- guest reads ----------------------------------------------------------------------------
bool Guest::Read(uint64_t at, void* out, size_t n) const {
    // read_memory checks the range itself (is_mapped first, MODULE_GUIDE 1.3): no second walk
    if (!at || !n || at > std::numeric_limits<uint64_t>::max() - n || !host.read_memory)
        return false;
    return host.read_memory(host.userdata, at, out, n) != 0;
}

bool DecodeEncryptedInt(const uint8_t* p, int64_t& value) {
    uint32_t enc;
    uint16_t adjust;
    std::memcpy(&enc, p, 4);
    std::memcpy(&adjust, p + 4, 2);
    const uint8_t shift = p[6], check = p[7];
    const uint8_t sum = static_cast<uint8_t>(p[0] + p[1] + p[2] + p[3] - 0x2D);
    if (sum != check)
        return false;
    const unsigned r = (shift + 3u) & 31u;
    const uint32_t v = r ? (enc >> r) | (enc << (32 - r)) : enc;
    value = static_cast<uint32_t>(v + 0x80E32B11u - adjust);
    return true;
}

int Weekday(int y, int m, int d) {
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 1 || m > 12)
        return 0;
    if (m < 3)
        y -= 1;
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

// ---- resolve --------------------------------------------------------------------------------
bool Reader::CheckPins(const Guest& g, std::string& diag) {
    dead_domains = 0;
    dead_pins.clear();
    for (const Pin* p : AllPins) {
        if (!PinMatches(g, *p)) {
            dead_domains |= PinDomains(p);
            if (dead_pins.size() < 200)
                dead_pins += std::string{dead_pins.empty() ? "" : ","} + p->name;
            AddDiag(diag, std::string{"pin "} + p->name);
        }
    }
    return dead_domains == 0;
}

bool Reader::Resolve(const Guest& g, std::string& diag) {
    resolved = false;
    roots = {};
    main_base = g.host.main_base;
    main_size = g.host.main_size;
    if (!main_base || !main_size) {
        AddDiag(diag, "no main image");
        return false;
    }
    CheckPins(g, diag);
    const uint64_t b = main_base;
    auto at = [&](const Pin& p, size_t a, size_t o) { return PinData(g, p, a, o); };
    roots.save_mgr = at(PinSaveMgr, 0, 1);
    roots.time_mgr = at(PinClock, 0, 1);
    roots.lang = at(PinLang, 0, 1);
    roots.lang_table = at(PinLangTable, 0, 6);
    roots.pno_override = at(PinLocalPlayerNo, 0, 1);
    roots.pno_session = at(PinLocalPlayerNo, 5, 6);
    roots.pno_local = at(PinLocalPlayerNo, 30, 31);
    roots.pos_table = at(PinPlayerPos, 3, 5);
    roots.pos_fallback = at(PinPlayerPos, 22, 23);
    roots.pos_scene = at(PinPlayerPos, 26, 27);
    roots.stage_name = at(PinStageName, 0, 2);
    roots.device_mgr = at(PinDeviceSeq, 0, 1);
    roots.actor_list = at(PinActorList, 0, 1);
    roots.save_vt_slot2 = b + PinSaveSlotData.offset;
    roots.save_vt_slot7 = b + PinSaveSlotTree.offset;
    roots.player_actor_name = b + PinPlayerActorName.offset;
    roots.camera_slot = b + PinCameraComp.offset;
    roots.app_table = at(PinAppTable, 0, 1);
    roots.app_order = at(PinAppOrder, 0, 1);
    roots.app_frames = at(PinAppFrame, 3, 4);
    roots.phone_root = at(PinPhoneRoot, 0, 1);
    roots.phone_seq_vt = at(PinPhoneSeqVt, 0, 1);
    roots.visitor_labels = at(PinVisitorLabel, 5, 6);
    roots.pocket_vt = at(PinPocketVt, 0, 1);
    roots.case_table = at(PinCaseTable, 4, 5);
    case_table.reset();
    roots.visitor_gst = at(PinVisitorGst, 9, 10);
    app_defs.clear();
    app_defs_sent = false;
    resolved = true;
    return true;
}

// ---- save -----------------------------------------------------------------------------------
bool Reader::SaveData(const Guest& g, uint64_t& main_dat, std::string& diag) const {
    // SaveMgr pin: mgr = [g]; set = [mgr+0x10]; obj = (mgr+0x453 & 1) ? [set+0x130] : [set].
    uint64_t mgr = 0, set = 0, obj = 0, vt = 0, slot2 = 0, slot7 = 0, data = 0, tree = 0;
    uint8_t alt = 0;
    if (!roots.save_mgr || !g.Get(roots.save_mgr, mgr) || !IsHeapPtr(mgr) ||
        !g.Get(mgr + 0x453, alt) || !g.Get(mgr + 0x10, set) || !IsHeapPtr(set) ||
        !g.Get(set + ((alt & 1) ? 0x130 : 0), obj) || !IsHeapPtr(obj)) {
        AddDiag(diag, "save mgr");
        return false;
    }
    // The object's vtable must be the class whose slot 2 / slot 7 are the pinned accessors.
    if (!g.Get(obj, vt) || !g.InMain(vt, 0x40) || !g.Get(vt + 0x10, slot2) ||
        !g.Get(vt + 0x38, slot7) || slot2 != roots.save_vt_slot2 || slot7 != roots.save_vt_slot7) {
        AddDiag(diag, "save vtable");
        return false;
    }
    if (!g.Get(obj + 0x10, data) || !IsHeapPtr(data) || !g.Get(obj + 0x20, tree) ||
        !IsHeapPtr(tree)) {
        AddDiag(diag, "save data");
        return false;
    }
    // Cross-check with the game's field handles: tree+0x70 = the Land handle {vt, base, offset}
    // (GSaveMain.Land at 0x110); its base must be the same bytes.
    uint64_t hbase = 0, hoff = 0;
    if (!g.Get(tree + 0x78, hbase) || !g.Get(tree + 0x80, hoff) || hbase != data || hoff != 0x110) {
        AddDiag(diag, "save handle");
        return false;
    }
    main_dat = data;
    save_tree = tree;
    return true;
}

bool Reader::PersonalData(const Guest& g, int player_no, uint64_t& personal, int& slot) const {
    // GetPersonal pin: first slot i with {id == player_no, valid == 1} at mgr+0x90+i*8, then
    // [[mgr+0x10]+0x60+i*8]+0x10.
    uint64_t mgr = 0, set = 0, obj = 0, data = 0;
    if (player_no < 0 || player_no > 7 || !roots.save_mgr || !g.Get(roots.save_mgr, mgr) ||
        !IsHeapPtr(mgr))
        return false;
    uint32_t slots[16];
    if (!g.Read(mgr + 0x90, slots, sizeof slots))
        return false;
    int found = -1;
    for (int i = 0; i < 8; ++i)
        if (slots[i * 2 + 1] == 1 && slots[i * 2] == static_cast<uint32_t>(player_no)) {
            found = i;
            break;
        }
    if (found < 0 || !g.Get(mgr + 0x10, set) || !IsHeapPtr(set) ||
        !g.Get(set + 0x60 + found * 8, obj) || !IsHeapPtr(obj) || !g.Get(obj + 0x10, data) ||
        !IsHeapPtr(data))
        return false;
    personal = data;
    slot = found;
    return true;
}

int Reader::LocalPlayerNo(const Guest& g) const {
    uint8_t v = 0xff;
    if (roots.pno_override && g.Get(roots.pno_override, v) && v < 8)
        return v;
    uint64_t session = 0, x = 0;
    if (roots.pno_session && g.Get(roots.pno_session, session) && IsHeapPtr(session) &&
        g.Get(session, x) && IsHeapPtr(x)) {
        int8_t idx = -1;
        uint64_t from = roots.pno_local; // the GOT slot of the pin holds this same byte's address
        if (g.Get(x + 0x473, idx) && idx != -1) {
            const uint64_t ent =
                x + 0x48 + (static_cast<uint8_t>(idx) < 8 ? uint64_t(uint8_t(idx)) << 7 : 0);
            uint32_t flags = 0;
            if (g.Get(ent, flags) && (flags & 8))
                from = x + 0x471;
        }
        if (from && g.Get(from, v) && v < 8)
            return v;
    }
    if (roots.pno_local && g.Get(roots.pno_local, v) && v < 8)
        return v;
    return -1;
}

// ---- clock / language / position ------------------------------------------------------------
bool Reader::ReadClock(const Guest& g, LiveSnapshot& out) const {
    uint64_t tm = 0, a = 0, b2 = 0;
    if (!roots.time_mgr || !g.Get(roots.time_mgr, tm) || !IsHeapPtr(tm) || !g.Get(tm + 0x4060, a) ||
        !g.Get(tm + 0x4060, b2) || a != b2)
        return false;
    const int y = static_cast<int>(a & 0xffff), mo = (a >> 16) & 0xff, d = (a >> 24) & 0xff,
              h = (a >> 32) & 0xff, mi = (a >> 40) & 0xff, s = (a >> 48) & 0xff;
    if (y < 2000 || y > 2100 || mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || s > 60)
        return false;
    out.y = y, out.mo = mo, out.d = d, out.h = h, out.mi = mi, out.s = s;
    out.wday = Weekday(y, mo, d);
    return true;
}

// Today's visitor, the way TodayVisitor (0x27d6b0c) decides it: the VisitorNpc s32[7] array is
// the save field whose handle {base, off} sits at [save tree]+0x70+0x1bf240 (VisitorCall
// 0x24aa650; must be main.dat+M::VisitorNpc); the day is TimeMgr+0x4068 when its year is set,
// else the clock date where hours 0-4 still belong to the previous day; index = GetDayOfWeek
// (0 = Sunday); during an online session nobody (0); values >= 12 (unsigned) are 0. The label
// is the game's own table (VisitorLabel 0x24aa7cc), "gstA" for 4 (VisitorGst 0x24aa698).
void Reader::ReadVisitor(const Guest& g, LiveSnapshot& out) const {
    out.visitor_ok = false;
    out.visitor_today = -1;
    out.visitor_wday = -1;
    out.visitor_label.clear();
    if ((dead_domains & (DVisitor | DClock | DSave)) || !save_tree || !roots.time_mgr)
        return;
    uint64_t hb = 0, ho = 0, main_dat = 0;
    if (!g.Get(save_tree + 0x70 + 0x1bf240, hb) || !g.Get(save_tree + 0x70 + 0x1bf248, ho))
        return;
    // the handle is {offset, base} or {base, offset} (the code adds both); main.dat is the base
    main_dat = IsHeapPtr(hb) ? hb : ho;
    const uint64_t arr = hb + ho;
    if (arr != main_dat + M::VisitorNpc)
        return;
    std::array<int32_t, 7> a{};
    if (!g.Read(arr, a.data(), sizeof a))
        return;
    uint64_t tm = 0, cal = 0, over = 0;
    if (!g.Get(roots.time_mgr, tm) || !IsHeapPtr(tm) || !g.Get(tm + 0x4060, cal) ||
        !g.Get(tm + 0x4068, over))
        return;
    int y, m, d;
    if (over & 0xffff) {
        y = static_cast<int>(over & 0xffff), m = (over >> 16) & 0xff, d = (over >> 24) & 0xff;
    } else {
        y = static_cast<int>(cal & 0xffff), m = (cal >> 16) & 0xff, d = (cal >> 24) & 0xff;
        const int h = static_cast<int8_t>((cal >> 32) & 0xff);
        if (h <= 4) { // ToPosixTimeFromUtc - 86400 -> ToCalendarTimeInUtc
            static constexpr int Mdays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
            if (--d < 1) {
                if (--m < 1)
                    m = 12, --y;
                const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
                d = m >= 1 && m <= 12 ? Mdays[m - 1] + (m == 2 && leap) : 1;
            }
        }
    }
    if (y < 2000 || y > 2100 || m < 1 || m > 12 || d < 1 || d > 31)
        return;
    const int wday = Weekday(y, m, d);
    uint64_t session = 0, sx = 0;
    uint32_t mode = 0;
    const bool online = roots.pno_session && g.Get(roots.pno_session, session) &&
                        IsHeapPtr(session) && g.Get(session + 0x28, sx) && sx &&
                        g.Get(session + 0x80, mode) && mode == 1;
    const uint32_t raw = static_cast<uint32_t>(a[wday < 7 ? wday : 0]);
    const int v = online ? 0 : (raw < 12 ? static_cast<int>(raw) : 0);
    std::string label;
    if (v == 4) {
        char buf[8] = {};
        if (!roots.visitor_gst || !g.Read(roots.visitor_gst, buf, 5) || buf[4] != 0)
            return;
        label = buf;
    } else if (v > 0) {
        uint64_t table = 0, str = 0;
        char buf[9] = {};
        if (!roots.visitor_labels || !g.Get(roots.visitor_labels, table) ||
            !g.InMain(table, 0x38 + 12 * 8) || !g.Get(table + 0x38 + uint64_t(v) * 8, str) ||
            !g.InMain(str, 8) || !g.Read(str, buf, 8))
            return;
        label.assign(buf, strnlen(buf, 8));
        if (label.empty() || label.size() > 6)
            return;
    }
    out.visitor_today = v;
    out.visitor_wday = wday;
    out.visitor_label = std::move(label);
    out.visitor_ok = true;
}

// The pocket menu's cursor (PocketSlot 0x32874e8: menu = [UI root]+0x88, vtable PocketVt; the
// focus code PocketCursor 0x3087b60 stores the focused slot 0..39 = ItemPocket 0..19 then
// ItemBag 0..19, our bag.{i} order, at menu+0x1c515, 0xff = none). Only while the menu is open
// (camera cItemMenu); the object stays alive after closing with the last slot.
void Reader::ReadPocketCursor(const Guest& g, LiveSnapshot& out) const {
    out.bag_cursor = -1;
    if ((dead_domains & (DPocketUi | DApps)) || !out.bag_open || !roots.phone_root ||
        !roots.pocket_vt)
        return;
    uint64_t root = 0, menu = 0, vt = 0;
    uint8_t cur = 0xff;
    if (g.Get(roots.phone_root, root) && IsHeapPtr(root) && g.Get(root + 0x88, menu) &&
        IsHeapPtr(menu) && g.Get(menu, vt) && vt == roots.pocket_vt && g.Get(menu + 0x1c515, cur) &&
        cur < 40)
        out.bag_cursor = cur;
}

bool Reader::ReadLanguage(const Guest& g, LiveSnapshot& out) const {
    uint64_t obj = 0, table = 0, folder = 0;
    uint32_t idx = 0;
    if (!roots.lang || !g.Get(roots.lang, obj) || !IsHeapPtr(obj) || !g.Get(obj, idx) || idx > 15 ||
        !roots.lang_table || !g.Get(roots.lang_table, table) || !g.InMain(table, 0x60 + 16 * 8) ||
        !g.Get(table + 0x60 + idx * 8, folder) || !g.InMain(folder, 6))
        return false;
    char name[6] = {};
    if (!g.Read(folder, name, 5) || name[4] != 0)
        return false;
    for (int i = 0; i < 4; ++i)
        if (!((name[i] >= 'A' && name[i] <= 'Z') || (name[i] >= 'a' && name[i] <= 'z')))
            return false;
    out.lang_folder.assign(name, 4);
    return true;
}

bool Reader::ReadPosition(const Guest& g, int no, LiveSnapshot& out) const {
    if (no < 0 || no > 7 || !roots.pos_scene)
        return false;
    uint64_t table = 0, rec = 0;
    if (roots.pos_table && g.Get(roots.pos_table, table) && IsHeapPtr(table)) {
        uint32_t flip = 0;
        if (!g.Get(table + 0xd68 + no * 4, flip))
            return false;
        rec = table + no * 0xd0 + (flip & 1) * 0x68 + 0x6e8;
    } else if (roots.pos_fallback) {
        rec = roots.pos_fallback;
    } else {
        return false;
    }
    uint8_t buf[0x68], again[0x68];
    uint16_t scene = 0;
    if (!g.Get(roots.pos_scene, scene))
        return false;
    out.scene_id = scene;
    if (!g.Read(rec, buf, sizeof buf) || !buf[0x64] ||
        (buf[0] | buf[1] << 8) != scene || !g.Read(rec, again, sizeof again) ||
        std::memcmp(buf + 0xc, again + 0xc, 12) != 0)
        return false;
    float p[3];
    std::memcpy(p, buf + 0xc, sizeof p);
    for (const float v : p)
        if (!(v > -1000.f && v < 3000.f))
            return false;
    out.px = p[0], out.py = p[1], out.pz = p[2];
    return true;
}

// ---- scene ----------------------------------------------------------------------------------
bool Reader::StateOf(const Guest& g, uint64_t sm, StateName& out) const {
    // State machine (SmNames / SmChange pins): +8 current id, +0x38 name count, +0x40 names
    // (16-byte SafeString: +8 C string in main's rodata).
    int32_t cur = -1;
    uint32_t count = 0;
    uint64_t names = 0, str = 0;
    if (!g.Get(sm + 8, cur) || !g.Get(sm + 0x38, count) || !g.Get(sm + 0x40, names) || count == 0 ||
        count > 1024 || !IsHeapPtr(names))
        return false;
    out.id = cur;
    out.name.clear();
    if (cur < 0)
        return true; // no state (between machines)
    if (static_cast<uint32_t>(cur) >= count || !g.Get(names + cur * 16 + 8, str) ||
        !g.InMain(str, 1))
        return false;
    char buf[48] = {};
    if (!g.Read(str, buf, sizeof buf - 1))
        return false;
    out.name.assign(buf, strnlen(buf, sizeof buf - 1));
    int32_t again = -2;
    return g.Get(sm + 8, again) && again == cur;
}

uint64_t Reader::FindPlayerActor(const Guest& g, std::string& why) const {
    // ActorList pin: mgr = [g]; head node [mgr+0x10], count [mgr+0x20], node offset [mgr+0x24].
    // Each entry is the list object at comp+0x290 of a component (ActorListComp pin: vtable,
    // [comp+0x278] = comp+0x290); the component's owner actor is at comp+0x48. The player is the
    // actor whose class name accessor (vtable slot 2) is the pinned "PlayerActor" one.
    uint64_t mgr = 0, node = 0;
    int32_t count = 0, nodeoff = 0;
    if (!roots.actor_list || !g.Get(roots.actor_list, mgr) || !IsHeapPtr(mgr) ||
        !g.Get(mgr + 0x10, node) || !g.Get(mgr + 0x20, count) || !g.Get(mgr + 0x24, nodeoff) ||
        count < 0 || count > 512 || nodeoff < 0 || nodeoff > 0x1000) {
        why = "actor list";
        return 0;
    }
    const uint64_t want_vt = PinData(g, PinActorListComp, 0, 1);
    uint64_t found = 0;
    int players = 0, entries = 0;
    for (int k = 0; k <= count && IsHeapPtr(node); ++k) {
        const uint64_t obj = node - nodeoff, comp = obj - 0x290;
        uint64_t vt = 0, back = 0, actor = 0, avt = 0, name_fn = 0, next = 0;
        if (g.Get(comp, vt) && vt == want_vt) {
            ++entries;
            if (g.Get(comp + 0x278, back) && back == obj && g.Get(comp + 0x48, actor) &&
                IsHeapPtr(actor) && g.Get(actor, avt) && g.InMain(avt, 0x18) &&
                g.Get(avt + 0x10, name_fn) && name_fn == roots.player_actor_name) {
                ++players;
                found = actor;
            }
        }
        if (!g.Get(node, next))
            break;
        node = next;
    }
    if (players != 1) {
        // A complete walk (`count` list components, then the end of the list) without a
        // PlayerActor is a real game state, not a broken route: scenes the player is not part of,
        // e.g. the title island and Isabelle's morning announcement on the first boot of a new
        // game day (live 2026-10-02: ObjectOfficeDemoDrink, ObjectSeasonDeco, NpcActor, ...).
        // ActorList pin: push-front of node {next, prev} at [mgr+0x10], the first node's prev =
        // mgr+0x10; the last next is null or that head slot (live: circular through mgr+0x10).
        const uint64_t end = mgr + 0x10;
        uint64_t n = 0;
        bool whole = entries == count && g.Get(end, n);
        for (int k = 0; whole && k < count; ++k)
            whole = g.Get(n, n);
        whole = whole && (n == 0 || n == end);
        why = players ? "several players" : (whole ? "no player in this scene" : "no player actor");
        return 0;
    }
    return found;
}

uint64_t Reader::FindCamera(const Guest& g, uint64_t actor) const {
    // Components ring: node at comp+0x38 ({next, prev}), owner at comp+0x48. Start at the state
    // component [actor+0x70] (PlayerSm pin) and look for the camera component class (vtable slot
    // 11 = the pinned CameraComp accessor); camera = [comp+0x298].
    uint64_t c0 = 0;
    if (!g.Get(actor + 0x70, c0) || !IsHeapPtr(c0))
        return 0;
    const uint64_t start = c0 + 0x38;
    uint64_t n = start;
    for (int k = 0; k < 96; ++k) {
        uint64_t next = 0;
        if (!g.Get(n, next) || !IsHeapPtr(next) || next == start)
            return 0;
        n = next;
        const uint64_t comp = n - 0x38;
        uint64_t owner = 0, vt = 0, fn = 0, cam = 0;
        if (!g.Get(n + 0x10, owner) || owner != actor)
            continue;
        if (g.Get(comp, vt) && g.InMain(vt, 0x60) && g.Get(vt + 0x58, fn) &&
            fn == roots.camera_slot && g.Get(comp + 0x298, cam) && IsHeapPtr(cam))
            return cam;
    }
    return 0;
}

void Reader::ReadScene(const Guest& g, LiveSnapshot& out) {
    // NookPhone: device manager [g], sequence machine at +0x440 (pin reads +0x448 = its +8).
    uint64_t dev = 0;
    out.phone_ok = !(dead_domains & DPhone) && roots.device_mgr && g.Get(roots.device_mgr, dev) &&
                   IsHeapPtr(dev) && StateOf(g, dev + 0x440, out.phone);
    out.player_ok = out.camera_ok = false;
    out.actor_at = 0;
    if (!(dead_domains & DPlayer)) {
        std::string why;
        const uint64_t actor = FindPlayerActor(g, why);
        out.actor_at = actor;
        uint64_t holder = 0;
        if (actor && g.Get(actor + 0x70, holder) && IsHeapPtr(holder))
            out.player_ok = StateOf(g, holder + 0x278, out.player);
        else
            AddDiag(out.diag, why.empty() ? "player sm" : why);
        if (actor && !(dead_domains & DCamera)) {
            const uint64_t cam = FindCamera(g, actor);
            out.camera_ok = cam && StateOf(g, cam + 0x408, out.camera);
        }
    }
    const auto is = [](const StateName& s, std::initializer_list<std::string_view> names) {
        return std::any_of(names.begin(), names.end(),
                           [&](std::string_view n) { return s.name == n; });
    };
    out.phone_open = out.phone_ok && out.phone.id >= 0 && out.phone.name != "cIdle";
    out.bag_open = out.camera_ok && out.camera.name == "cItemMenu";
    // Loading / transition: no player actor (scene being rebuilt), or the player's machine in a
    // hidden / warp / stage (door, stairs) state.
    out.loading = !out.player_ok || out.player.id < 0 || out.player.name.starts_with("cHide") ||
                  out.player.name.starts_with("cStage") ||
                  is(out.player, {"cWaitAfterWarp", "cWaitNoController"});
    out.free = !out.loading && !out.phone_open && out.camera_ok && out.player_ok &&
               // cIdrRoom = the camera inside shops / facilities (LIVE r9 indoor: Nook's Cranny and
               // Able Sisters; X switches it to cItemMenu as in rooms)
               is(out.camera, {"cNormalField", "cNormalRoom", "cIdrRoom"}) &&
               is(out.player, {"cWait", "cMove", "cTurn"});
    out.scene_ok = out.phone_ok || out.player_ok;
}

// ---- NookPhone apps ---------------------------------------------------------------------------
void Reader::ReadPhone(const Guest& g, LiveSnapshot& out) {
    out.phone_ui = {};
    if (dead_domains & DApps)
        return;
    // the app table (static initializer 0x2e59b80, AppTable pin) and the order list (AppOrder pin):
    // read once the initializer has run (18 distinct ids 0..17)
    if (app_defs.empty() && roots.app_table && roots.app_order && roots.app_frames) {
        int32_t order[18];
        float frames[18];
        std::vector<LiveSnapshot::AppDef> defs;
        bool ok = g.Read(roots.app_order, order, sizeof order) &&
                  g.Read(roots.app_frames, frames, sizeof frames);
        uint32_t seen = 0;
        auto cstr = [&](uint64_t at, std::string& s) {
            uint64_t p = 0;
            char buf[64] = {};
            if (!g.Get(at, p) || !g.InMain(p, 1) || !g.Read(p, buf, sizeof buf - 1))
                return false;
            s.assign(buf, strnlen(buf, sizeof buf - 1));
            return true;
        };
        for (int k = 0; ok && k < 18; ++k) {
            if (order[k] < 0 || order[k] > 17 || (seen >> order[k] & 1)) {
                ok = false;
                break;
            }
            seen |= 1u << order[k];
            bool found = false;
            for (int e = 0; e < 18 && ok; ++e) {
                int32_t id = -1;
                if (!g.Get(roots.app_table + e * 0x38, id)) {
                    ok = false;
                    break;
                }
                if (id != order[k])
                    continue;
                LiveSnapshot::AppDef d;
                d.id = id;
                const float frame = frames[id];
                ok = std::isfinite(frame) && frame >= 0.0f && frame < 256.0f; // no UB float->int
                d.frame = ok ? static_cast<int>(frame) : 0;
                ok = ok && cstr(roots.app_table + e * 0x38 + 0x10, d.label) &&
                     cstr(roots.app_table + e * 0x38 + 0x20, d.flag) && d.label.size() == 4;
                found = true;
                defs.push_back(std::move(d));
                break;
            }
            ok = ok && found;
        }
        if (ok && defs.size() == 18)
            app_defs = std::move(defs);
        app_defs_sent = false;
    }
    if (!app_defs_sent) { // read once per resolve: copied into the snapshot once, not every sample
        out.app_defs = app_defs;
        app_defs_sent = !app_defs.empty();
    }
    // the home seq: [[root]+0x90] with the pinned vtable (exists while the home grid is built)
    uint64_t root = 0, seq = 0, vt = 0, arr = 0;
    if (!roots.phone_root || !g.Get(roots.phone_root, root) || !IsHeapPtr(root) ||
        !g.Get(root + 0x90, seq) || !IsHeapPtr(seq) || !g.Get(seq, vt) || vt != roots.phone_seq_vt)
        return;
    int32_t v[4] = {};
    if (!g.Get(seq + 0x228, v[0]) || !g.Get(seq + 0x308, v[1]) || !g.Get(seq + 0x30c, v[2]) ||
        !g.Get(seq + 0x2b4, v[3]) || v[0] < 0 || v[0] > 18 || !g.Get(seq + 0x230, arr) ||
        !IsHeapPtr(arr))
        return;
    auto& p = out.phone_ui;
    for (int i = 0; i < v[0]; ++i) {
        uint64_t e = 0;
        int32_t id = -1;
        if (!g.Get(arr + i * 8, e) || !IsHeapPtr(e) || !g.Get(e, id) || id < 0 || id > 17)
            return;
        p.ids.push_back(id);
    }
    int32_t again = -1;
    if (!g.Get(seq + 0x308, again) || again != v[1])
        return;
    p.count = v[0], p.cursor = v[1], p.page = v[2], p.pages = v[3];
    p.ok = true;
}

// ---- island bytes ---------------------------------------------------------------------------
void Reader::ReadIsland(const Guest& g, uint64_t main_dat, LiveSnapshot& out) {
    const uint64_t now = NowMs();
    if (island_source != main_dat) {
        island_source = main_dat;
        island_last_ms = 0;
    }
    out.island_rev = island_rev;
    if (island_rev && now - island_last_ms < 1000)
        return;
    // into kept buffers (no 150 KB of allocations a second), compared against the kept revision
    island_buf[0].resize(M::LandMakingSize);
    island_buf[1].resize(M::FieldBlockSize);
    island_buf[2].resize(M::StructureSize);
    if (!g.Read(main_dat + M::LandMaking, island_buf[0].data(), island_buf[0].size()) ||
        !g.Read(main_dat + M::FieldBlocks, island_buf[1].data(), island_buf[1].size()) ||
        !g.Read(main_dat + M::Structures, island_buf[2].data(), island_buf[2].size()))
        return;
    island_last_ms = now;
    if (island_rev && island_buf[0] == out.land_making && island_buf[1] == out.field_blocks &&
        island_buf[2] == island_structures)
        return;
    // torn check: the structure list again (the cheap part a relocation touches)
    std::vector<uint8_t>& st2 = island_buf[3];
    st2.resize(M::StructureSize);
    if (!g.Read(main_dat + M::Structures, st2.data(), st2.size()) || st2 != island_buf[2])
        return;
    out.island_rev = ++island_rev;
    out.land_making.swap(island_buf[0]);
    out.field_blocks.swap(island_buf[1]);
    island_structures.swap(island_buf[2]);
}

// ---- sample ---------------------------------------------------------------------------------
void Reader::Sample(const Guest& g, const Need& need, LiveSnapshot& out) {
    ++out.serial;
    out.diag.clear();
    if (!resolved || g.host.main_base != main_base || g.host.main_size != main_size) {
        if (!Resolve(g, out.diag)) {
            out.code_ok = out.save_ok = out.personal_ok = out.clock_ok = out.lang_ok = false;
            out.pos_ok = out.scene_ok = out.phone_ok = out.player_ok = out.camera_ok = false;
            out.order_ok = false;
            out.scene_id = -1;
            out.stage_name.clear();
            // no stale write targets for the Bag page (acnh_bag_live.cpp ReadView)
            out.personal_at = out.actor_at = 0;
            out.player_no = -1;
            return;
        }
    }
    out.code_ok = dead_domains == 0;
    if (dead_domains)
        AddDiag(out.diag, "code");
    out.dead_pins = dead_pins;
    out.dead_domains = dead_domains;
    out.main_base = main_base, out.main_size = main_size;
    out.reloc = g.DataDelta();
    out.heap_lo = g.HeapBegin(), out.heap_hi = g.HeapEnd();
    out.main_dat = 0;

    out.order_ok = !(dead_domains & DOrder);
    if (!case_table && !(dead_domains & DText) && roots.case_table) {
        // the game's {lower, upper} pairs, sorted by lower (binary-searched by the text writer)
        std::vector<uint16_t> pairs(188 * 2);
        bool ok = g.Read(roots.case_table, pairs.data(), pairs.size() * 2) && pairs[0] == 'a' &&
                  pairs[1] == 'A';
        for (size_t i = 2; ok && i < pairs.size(); i += 2)
            ok = pairs[i] > pairs[i - 2];
        if (ok)
            case_table = std::make_shared<const std::vector<uint16_t>>(std::move(pairs));
    }
    out.case_table = case_table;
    out.clock_ok = !(dead_domains & DClock) && ReadClock(g, out);
    out.lang_ok = !(dead_domains & DLang) && ReadLanguage(g, out);
    out.player_no = (dead_domains & DSave) && (dead_domains & DPos) ? -1 : LocalPlayerNo(g);
    out.scene_id = -1;
    out.stage_name.clear();
    if (!(dead_domains & DPos)) {
        // StageName pin: a SafeString object in main; +8 is its text, +0x10 its capacity.
        // Read this independently of position, which is deliberately absent at the title.
        uint64_t object = 0, text = 0;
        uint16_t scene = 0, again = 0;
        char name[32]{};
        if (g.Get(roots.pos_scene, scene) && g.Get(roots.stage_name, object) &&
            g.InMain(object, 0x40) && g.Get(object + 8, text) && g.InMain(text, sizeof name) &&
            g.Read(text, name, sizeof name) && g.Get(roots.pos_scene, again) && scene == again &&
            std::memchr(name, 0, sizeof name)) {
            out.scene_id = scene;
            out.stage_name = name;
        }
    }
    const int named_scene = out.scene_id;
    out.pos_ok = !(dead_domains & DPos) && ReadPosition(g, out.player_no, out);
    if (out.scene_id != named_scene)
        out.stage_name.clear(); // fail closed if the stage changed during this sample

    if (need.scene)
        ReadScene(g, out);
    if (need.pockets && need.scene)
        ReadPocketCursor(g, out);
    if (need.phone)
        ReadPhone(g, out);

    // ---- main.dat ----
    uint64_t main_dat = 0;
    out.save_ok = !(dead_domains & DSave) && SaveData(g, main_dat, out.diag);
    if (out.save_ok) {
        // a cheap sanity read: the header's version words (GSaveMain.Version) start the blob
        uint32_t ver[2] = {};
        if (!g.Read(main_dat, ver, sizeof ver) || ver[0] != 0xA0002u || ver[1] != 0xA0028u) {
            out.save_ok = false;
            AddDiag(out.diag, "save version");
        }
    }
    if (out.save_ok) {
        out.main_dat = main_dat;
        uint8_t name[20], wa[4];
        if (g.Read(main_dat + M::IslandName, name, sizeof name))
            out.island_name = Utf16(name, 10);
        if (g.Read(main_dat + M::WeatherArea, wa, sizeof wa)) {
            int32_t v;
            std::memcpy(&v, wa, 4);
            out.hemi = v == 0 || v == 1 ? v : -1;
        }
        if (need.residents || need.island) {
            for (uint32_t i = 0; i < M::VillagerCount; ++i) {
                uint8_t id[3] = {};
                auto& r = out.residents[i];
                r = {};
                if (g.Read(main_dat + M::Villager + i * M::VillagerStride, id, 3)) {
                    r.species = id[0], r.variant = id[1], r.personality = id[2];
                    r.present = true; // emptiness is decided by the catalog join (publish)
                    uint8_t bd[4];
                    if (g.Read(main_dat + M::Villager + i * M::VillagerStride +
                                   M::VillagerBirthDate,
                               bd, 4)) {
                        r.born_ok = true;
                        r.born_y = static_cast<uint16_t>(bd[0] | bd[1] << 8);
                        r.born_m = bd[2], r.born_d = bd[3];
                    }
                }
            }
            out.structures.resize(M::StructureSize);
            if (!g.Read(main_dat + M::Structures, out.structures.data(), out.structures.size()))
                out.structures.clear();
        }
        if (need.island)
            ReadIsland(g, main_dat, out);
        if (need.today) {
            uint8_t kabu[0x44];
            if (g.Read(main_dat + M::ShopKabu, kabu, sizeof kabu)) {
                std::memcpy(&out.kabu_sun, kabu, 4);
                std::memcpy(out.kabu.data(), kabu + 4, 14 * 4);
            }
            int32_t wt = -1;
            out.weather_today = g.Get(main_dat + M::WeatherToday, wt) ? wt : -1;
            ReadVisitor(g, out);
        }
        if (need.critters) {
            auto& items = museum_buf;
            out.donated.clear();
            if (g.Read(main_dat + M::MuseumItems, items.data(), items.size()))
                for (uint32_t i = 0; i < M::MuseumCount; ++i) {
                    const uint16_t id = static_cast<uint16_t>(items[i * 8] | items[i * 8 + 1] << 8);
                    if (id != 0xFFFE && id != 0)
                        out.donated.push_back(id);
                }
        }
    }

    // ---- personal.dat of the local player ----
    uint64_t per = 0;
    out.personal_ok = out.save_ok && PersonalData(g, out.player_no, per, out.personal_slot);
    out.personal_at = out.personal_ok ? per : 0;
    if (!out.personal_ok) {
        if (out.save_ok)
            AddDiag(out.diag, "personal");
        return;
    }
    {
        uint8_t id[0x38];
        if (g.Read(per + P::PlayerId, id, sizeof id))
            out.player_name = Utf16(id + 0x20, 10);
    }
    if (need.pockets || need.money || need.recipes) {
        // ItemBag (+count), ItemPocket (+count), wallet, expand: one read, torn-checked
        constexpr uint32_t span = P::ExpandBaggage + 1 - P::ItemBag;
        uint8_t a[span], b2[span];
        if (g.Read(per + P::ItemBag, a, span) && g.Read(per + P::ItemBag, b2, span) &&
            std::memcmp(a, b2, span) == 0) {
            auto item = [&](uint32_t off) {
                Item it;
                const uint8_t* p = a + off;
                it.id = static_cast<uint16_t>(p[0] | p[1] << 8);
                it.sys = p[2], it.add = p[3];
                std::memcpy(&it.free, p + 4, 4);
                return it;
            };
            for (int i = 0; i < 20; ++i) {
                out.pockets[i] = item(P::ItemPocket - P::ItemBag + i * 8);
                out.pockets[i].fav = static_cast<int8_t>(a[P::PocketFav - P::ItemBag + i]);
                out.pockets[20 + i] = item(i * 8);
                out.pockets[20 + i].fav = static_cast<int8_t>(a[P::BagFav - P::ItemBag + i]);
            }
            const uint8_t expand = a[P::ExpandBaggage - P::ItemBag];
            out.pocket_slots = expand == 0 ? 20 : expand == 1 ? 30 : expand == 2 ? 40 : 0;
            int64_t v;
            out.wallet = DecodeEncryptedInt(a + (P::Wallet - P::ItemBag), v) ? v : -1;
        } else {
            out.pocket_slots = 0;
            AddDiag(out.diag, "pockets torn");
        }
    }
    if (need.money) {
        uint8_t m[16], s[8];
        int64_t v;
        out.miles = out.bank = -1;
        if (g.Read(per + P::MilesNow, m, sizeof m)) {
            out.miles = DecodeEncryptedInt(m, v) ? v : -1;
        }
        if (g.Read(per + P::Savings, s, sizeof s))
            out.bank = DecodeEncryptedInt(s, v) ? v : -1;
    }
    if (need.critters) {
        // Fish[100] u16 + NewFlag[13] + Count u8 | Insect[100] + [13] + Count | Dive[60] + [8] +
        // Count
        uint8_t c[0x67DCD - P::Fish];
        if (g.Read(per + P::Fish, c, sizeof c)) {
            auto list = [&](uint32_t off, int n, uint32_t count_off, std::vector<uint16_t>& v) {
                v.clear();
                const int count = c[count_off - P::Fish];
                for (int i = 0; i < n && i < count; ++i) {
                    const uint16_t id = static_cast<uint16_t>(c[off - P::Fish + i * 2] |
                                                              c[off - P::Fish + i * 2 + 1] << 8);
                    if (id != 0xFFFE && id != 0)
                        v.push_back(id);
                }
            };
            list(P::Insect, 100, 0x67D4B, out.caught[0]);
            list(P::Fish, 100, 0x67C75, out.caught[1]);
            list(P::Dive, 60, 0x67DCC, out.caught[2]);
        }
    }
    out.flags_ok = (need.phone || need.recipes) &&
                   g.Read(per + P::EventFlag, out.event_flags.data(), sizeof out.event_flags);
    if (need.chest) {
        if (chest_source != per) {
            chest_source = per;
            out.chest.clear();
            out.chest_hash = 0;
            chest_last_ms = 0;
        }
        const uint64_t now = NowMs();
        if (out.chest.empty() || now - chest_last_ms > 2000) {
            auto& c = chest_buf;
            c.resize(P::ChestCount * 8);
            if (g.Read(per + P::Chest, c.data(), c.size())) {
                chest_last_ms = now;
                out.chest.resize(P::ChestCount);
                for (uint32_t i = 0; i < P::ChestCount; ++i) {
                    Item& it = out.chest[i];
                    it.id = static_cast<uint16_t>(c[i * 8] | c[i * 8 + 1] << 8);
                    it.sys = c[i * 8 + 2], it.add = c[i * 8 + 3];
                    std::memcpy(&it.free, &c[i * 8 + 4], 4);
                }
                out.chest_hash = dsmod_sdk::Fnv1a64(c.data(), c.size());
            }
        }
    } else if (!out.chest.empty()) {
        out.chest.clear();
        out.chest_hash = 0;
    }
    if (need.recipes) {
        uint8_t r[0x400];
        if (g.Read(per + P::RecipeCollect, r, sizeof r)) {
            std::memcpy(out.recipe_collect.data(), r, 0x100);
            std::memcpy(out.recipe_made.data(), r + 0x100, 0x100);
            std::memcpy(out.recipe_new.data(), r + 0x200, 0x100);
            std::memcpy(out.recipe_fav.data(), r + 0x300, 0x100);
        }
    }
    if (need.today || need.profile) {
        uint8_t ls[0xE0D4 - P::LifeSupport];
        if (g.Read(per + P::LifeSupport, ls, sizeof ls)) {
            std::memcpy(out.nmp_flags.data(), ls, 0x400);
            const uint8_t* entry = ls + (P::EntryDaily - P::LifeSupport); // EntryDaily u8[512]
            std::memcpy(out.nmp_reward.data(), ls + (P::RewardDaily - P::LifeSupport), 512);
            std::memcpy(out.nmp_bonus.data(), ls + (P::BonusDaily - P::LifeSupport), 512);
            out.nmp_order.clear();
            for (int i = 0; i < 8; ++i) {
                const uint8_t* p = ls + (P::DailyOrder - P::LifeSupport) + i * 2;
                const uint16_t id = static_cast<uint16_t>(p[0] | p[1] << 8);
                if (id < 512 && entry[id])
                    out.nmp_order.push_back(id);
            }
            // entries the order table does not list (should not happen) keep index order
            for (uint16_t id = 0; id < 512; ++id)
                if (entry[id] && std::find(out.nmp_order.begin(), out.nmp_order.end(), id) ==
                                     out.nmp_order.end())
                    out.nmp_order.push_back(id);
        }
        uint8_t pr[0x3660C + 4 - P::ProfileBirthday];
        if (g.Read(per + P::ProfileBirthday, pr, sizeof pr)) {
            out.birth_m = pr[0], out.birth_d = pr[1];
            out.fruit = static_cast<uint16_t>(pr[4] | pr[5] << 8);
            const uint8_t* ts = pr + (P::ProfileStamp - P::ProfileBirthday);
            out.reg_y = static_cast<uint16_t>(ts[0] | ts[1] << 8), out.reg_m = ts[2],
            out.reg_d = ts[3];
        }
    }
}

bool Reader::PassportJpeg(const Guest& g, std::vector<uint8_t>& jpeg) {
    if (!resolved || (dead_domains & DSave))
        return false;
    uint64_t main_dat = 0, per = 0;
    std::string diag;
    int slot = -1;
    if (!SaveData(g, main_dat, diag) || !PersonalData(g, LocalPlayerNo(g), per, slot))
        return false;
    int32_t size = 0;
    if (!g.Get(per + P::JpegSize, size) || size < 4 || size > static_cast<int32_t>(P::JpegMax))
        return false;
    jpeg.resize(static_cast<size_t>(size));
    if (!g.Read(per + P::Jpeg, jpeg.data(), jpeg.size()) || jpeg[0] != 0xFF || jpeg[1] != 0xD8)
        return false;
    return true;
}

} // namespace acnh::live

// ---- module seam (acnh_types.h) ---------------------------------------------------------------
namespace acnh {

class Live {
public:
    Live(const EdenDsmodHostApi& h, const Services& s) : services{s}, publisher{s}, bag{s.catalog} {
        (void)h;
    }
    Services services;
    live::Reader reader;
    live::LiveSnapshot snap;
    Publisher publisher;
    bag::Writer bag; ///< Bag page direct writes + the empty-hands pocket-menu drive (acnh_bag.h)
    std::mutex jpeg_mutex;
    std::vector<uint8_t> jpeg;
    uint64_t jpeg_rev = 0, jpeg_hash = 0, jpeg_last_ms = 0;
    std::atomic<int> lang{-1};
};

Live* LiveCreate(const EdenDsmodHostApi& host, const Services& services) {
    try {
        return new Live{host, services};
    } catch (...) {
        return nullptr;
    }
}

void LiveDestroy(Live* live) {
    delete live;
}

void LiveSample(Live* live, const EdenDsmodHostApi& host) {
    if (!live)
        return;
#if EDEN_ACNH_DIAGNOSTICS
    const auto t0 = std::chrono::steady_clock::now();
#endif
    live::Guest g{host};
    const live::Need need = live->publisher.WhatToRead();
    live->reader.Sample(g, need, live->snap);
    if (live->snap.lang_ok) {
        const Lang l = LangFromFolder(live->snap.lang_folder, Lang::Count);
        live->lang.store(l == Lang::Count ? -1 : static_cast<int>(l));
    }
    // passport photo: refresh the copy at most every 5 s while the Today page wants it
    if (need.profile && live->snap.personal_ok) {
        const uint64_t now = live::NowMs();
        if (now - live->jpeg_last_ms > 5000) {
            live->jpeg_last_ms = now;
            std::vector<uint8_t> j;
            if (live->reader.PassportJpeg(g, j)) {
                const uint64_t h = dsmod_sdk::Fnv1a64(j.data(), j.size());
                std::lock_guard lock{live->jpeg_mutex};
                if (h != live->jpeg_hash) {
                    live->jpeg_hash = h;
                    live->jpeg = std::move(j);
                    ++live->jpeg_rev;
                }
            }
        }
    }
#if EDEN_ACNH_DIAGNOSTICS
    const auto us =
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0)
            .count();
#else
    constexpr int64_t us = 0;
#endif
    live->publisher.Publish(host, live->snap, us);
    // after the publisher: while the bag drive runs it owns the shared pdrv.* button gates
    live->bag.Sample(host, g, live->snap, live->reader.roots.phone_root,
                     live->publisher.Page() == 2, live->publisher.PhoneDriveBusy(),
                     live->publisher.BagSel());
}

void LiveSetWriteApi(Live* live, const EdenDsmodHostWriteApi& api) {
    if (live)
        live->bag.SetWriteApi(api);
}

void LiveTick(Live*, const EdenDsmodHostApi&) {}

bool LiveAction(Live* live, const char* action, int64_t argument) {
    if (!live || !action)
        return false;
    const std::string_view name{action};
    if (name == "phone_open" && live->bag.DriveBusy())
        return false; // one native-menu drive at a time
    bool handled = false;
    const bool ok = live->bag.Action(name, argument, handled);
    return handled ? ok : live->publisher.Action(action, argument);
}

bool LivePassportJpeg(Live* live, std::vector<uint8_t>& jpeg, uint64_t& revision) {
    if (!live)
        return false;
    std::lock_guard lock{live->jpeg_mutex};
    if (live->jpeg.empty())
        return false;
    jpeg = live->jpeg;
    revision = live->jpeg_rev;
    return true;
}

int LiveLanguage(Live* live) {
    return live ? live->lang.load() : -1;
}

} // namespace acnh
