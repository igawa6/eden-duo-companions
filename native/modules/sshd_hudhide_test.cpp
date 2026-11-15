// SPDX-License-Identifier: GPL-3.0-or-later
// Top-screen HUD hiding (owner 2026-11-11): which panes go in the draw hook's list (Cross base +
// Down / Left / Right, dowsing prompt + selected-target set, Help prompt, rupee counter; never the
// wheel, the first-person controls or the harp / X branch), the hide / refresh / restore state
// machine (settings default Off = hidden, re-show, helper gate, reused allocations; dev.37: hidden in
// menus and without Help too, a refused sample or a stalled helper holds the list instead of
// clearing it, and the dev.36 behaviour replayed for diagnostics), and the module end to end:
// settings flags, the double-buffered mailbox list the helper reads (dev.37: a change is written
// into the other bank and published with one select-word store, never count 0 in between; 120-frame
// ttl), clean restore on re-show / unload, and the bottom D-pad mirror still publishing while the
// top copy is hidden (nothing in the panes is written).
#include <cassert>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "core/mods/dsmod_module_abi.h"
#include "core/mods/dsmod_module_extensions.h"
#include "sshd_hudhide.h"

namespace {

constexpr uint64_t kMain = 0x80000000ull;

struct Mem {
    std::map<uint64_t, uint8_t> b;
    void Put(uint64_t a, const void* p, size_t n) {
        for (size_t i = 0; i < n; ++i) b[a + i] = static_cast<const uint8_t*>(p)[i];
    }
    void U64(uint64_t a, uint64_t v) { Put(a, &v, 8); }
    void U32(uint64_t a, uint32_t v) { Put(a, &v, 4); }
    void U8(uint64_t a, uint8_t v) { Put(a, &v, 1); }
    bool Read(uint64_t a, void* out, size_t n) const {
        auto* o = static_cast<uint8_t*>(out);
        for (size_t i = 0; i < n; ++i) {
            auto it = b.find(a + i);
            if (it == b.end()) return false;
            o[i] = it->second;
        }
        return true;
    }
};

// The meter, its Cross / Dowsing / Help widgets and the rupee layout as the live pane trees have
// them (D-pad reader analysis, on foot, DoButton basicPosition_00 / rupy_01).
struct World {
    Mem mem;
    uint64_t next = 0x90000000ull;
    uint64_t m = 0x86000000ull, c, d, h = 0x87000000ull;
    std::map<std::string, uint64_t> pane;
    uint64_t Pane(const std::string& key, const char* name, uint64_t parent, uint8_t alpha = 255) {
        const uint64_t p = next;
        next += 0x200;
        char nm[24]{};
        std::strncpy(nm, name, 23);
        mem.Put(p + 0xEC, nm, 24);
        mem.U64(p + 0x18, parent);
        const uint8_t ab[4] = {alpha, alpha, 0, 1};
        mem.Put(p + 0xE8, ab, 4);
        mem.U64(p + 0x50, name[0] == 'P' ? 0x95000000ull : 0);
        mem.U64(p + 0x28, p + 0x28);
        if (parent != 0) {
            uint64_t prev = parent + 0x28, node = 0;
            mem.Read(prev, &node, 8);
            while (node != parent + 0x28) { prev = node; mem.Read(node, &node, 8); }
            mem.U64(prev, p + 8);
            mem.U64(p + 8, parent + 0x28);
        }
        pane[key] = p;
        return p;
    }
    void Rename(const std::string& key, const char* name) {
        char nm[24]{};
        std::strncpy(nm, name, 23);
        mem.Put(pane[key] + 0xEC, nm, 24);
    }
    World() {
        c = m + 0xC928;
        d = m + 0x15CF8;
        mem.U64(kMain + 0x183C9A0, m);
        mem.U64(kMain + 0x1826AD0, 0); // dev.33: no player object (not in water)
        mem.U64(c, kMain + 0x165D398);
        mem.U64(d, kMain + 0x165DFF0);
        mem.U64(m + 0x20D08, h);
        mem.U64(h, kMain + 0x165BE50);
        mem.U8(m + 0x218B0, 0);
        mem.U8(c + 0xB5F, 0);
        mem.U8(c + 0xB60, 0);
        mem.U32(d + 0x9100, 0x52);
        // the meter's own layout (basicPosition_00)
        const uint64_t top = Pane("M/root", "RootPane", 0);
        const uint64_t rupy_all = Pane("M/N_rupyAll_00", "N_rupyAll_00", top);
        const uint64_t rupy = Pane("M/N_rupy_00", "N_rupy_00", rupy_all);
        const uint64_t remo = Pane("M/N_remoAll_00", "N_remoAll_00", top);
        const uint64_t oc = Pane("C/owner", "N_remoConBtn_03", remo), oh = Pane("H/owner", "N_remoConBtn_05", remo);
        const uint64_t od = Pane("D/owner", "N_nunBtn_03", Pane("M/N_nunAll_00", "N_nunAll_00", top));
        Pane("M/N_heartAll_00", "N_heartAll_00", top);
        mem.U64(c + 0x6A0, oc);
        mem.U64(d + 0x1678, od);
        mem.U64(h + 0x2B8, oh);
        // rupy_01, attached under N_rupy_00
        const uint64_t rr = Pane("R/root", "RootPane", rupy);
        const uint64_t ra = Pane("R/N_rupyAll_00", "N_rupyAll_00", rr);
        Pane("R/P_number_00", "P_number_00", Pane("R/N_text_00", "N_text_00", ra));
        // Cross (remoConBtn_03)
        const uint64_t rc = Pane("C/root", "RootPane", oc);
        mem.U64(c + 0xC0, rc);
        const uint64_t all = Pane("C/N_all_00", "N_all_00", rc), key = Pane("C/N_key_00", "N_key_00", all);
        Pane("C/base", "P_crossBtn_00", Pane("C/N_bace_00", "N_bace_00", key), 128);
        Pane("C/N_keyT_00", "N_keyT_00", key);
        Pane("C/down", "P_crossBtn_02", Pane("C/N_keyD_00", "N_keyD_00", key));
        Pane("C/N_keyR_00", "N_keyR_00", key);
        Pane("C/left", "P_crossBtn_04", Pane("C/N_keyL_00", "N_keyL_00", key));
        Pane("C/harp", "P_harp_00", Pane("C/N_iconT_00", "N_iconT_00", all));
        const uint64_t icd = Pane("C/N_iconD_00", "N_iconD_00", all);
        const uint64_t sw = Pane("C/N_swordIcon_00", "N_swordIcon_00", icd);
        Pane("C/fi", "P_plusIcon_01", sw);
        Pane("C/fih0", "P_plusIcon_00", sw, 90);
        Pane("C/fih1", "P_plusIcon_02", sw, 40);
        for (const char* n : {"N_textT_00", "N_textD_00", "N_textL_00", "N_textR_00"})
            Pane(std::string("C/") + n, n, all);
        // Dowsing (nunBtn_03, root at D+0x700)
        const uint64_t rd = Pane("D/root", "RootPane", od);
        mem.U64(d + 0x700, rd);
        const uint64_t ia = Pane("D/N_itemAll_00", "N_itemAll_00", rd);
        const uint64_t set = Pane("D/N_iconSet_00", "N_iconSet_00",
                                  Pane("D/N_setBtn_00", "N_setBtn_00", Pane("D/N_showSet_00", "N_showSet_00", ia)));
        Pane("D/target", "P_comIcon_00", set);
        Pane("D/N_cancel_00", "N_cancel_00", ia);
        Pane("D/N_resetGyro_00", "N_resetGyro_00", ia);
        const uint64_t simple = Pane("D/N_showSimple_00", "N_showSimple_00", ia);
        Pane("D/wheel_icon", "P_comIcon_00", Pane("D/N_A_00", "N_A_00", Pane("D/N_itemSelect_00", "N_itemSelect_00", ia)));
        Pane("D/N_itemArrow_00", "N_itemArrow_00", ia);
        Pane("D/utext", "T_dawsing_00", Pane("D/N_input_00", "N_input_00", simple));
        const uint64_t cb = Pane("D/N_crossBtn_00", "N_crossBtn_00", Pane("D/N_input_01", "N_input_01", simple));
        Pane("D/base", "P_crossBtn_00", cb, 190);
        Pane("D/eye", "P_btn_00", Pane("D/N_lookBtns_00", "N_lookBtns_00", cb), 200);
        Pane("D/up", "P_btn_01", cb);
        // Help (remoConBtn_05)
        const uint64_t rh = Pane("H/root", "RootPane", oh);
        mem.U64(h + 0xC0, rh);
        const uint64_t hall = Pane("H/N_all_00", "N_all_00", rh);
        Pane("H/htext", "T_helpBtn_00", Pane("H/N_text_00", "N_text_00", hall));
        Pane("H/base", "P_crossBtn_00", hall, 128);
        Pane("H/right", "P_btn_02", hall);
    }
    sshd::DpadRead Reader() const {
        return [this](uint64_t a, void* o, size_t n) { return mem.Read(a, o, n); };
    }
};

uint32_t Lo(const char* name) {
    char n[8]{};
    std::strncpy(n, name, 8);
    uint32_t v;
    std::memcpy(&v, n, 4);
    return v;
}
uint32_t Hi(const char* name) {
    char n[8]{};
    std::strncpy(n, name, 8);
    uint32_t v;
    std::memcpy(&v, n + 4, 4);
    return v;
}

const char* const kDpadKeys[] = {"C/N_bace_00", "C/N_keyD_00", "C/N_keyL_00",     "C/N_keyR_00",
                                 "C/N_iconD_00", "C/N_textD_00", "C/N_textL_00",   "C/N_textR_00",
                                 "D/N_showSimple_00", "D/N_showSet_00", "H/N_all_00"};
const char* const kKept[] = {"C/N_all_00",     "C/N_key_00",     "C/N_keyT_00",       "C/N_iconT_00",
                             "C/N_textT_00",   "D/N_itemAll_00", "D/N_itemSelect_00", "D/N_cancel_00",
                             "D/N_resetGyro_00", "D/N_itemArrow_00", "H/root", "C/owner", "D/owner", "H/owner",
                             "M/N_heartAll_00", "M/N_remoAll_00"};

bool Has(const std::vector<sshd::HideEntry>& v, uint64_t p) {
    for (const auto& e : v)
        if (e.pane == p) return true;
    return false;
}

void ResolverTests() {
    using namespace sshd;
    // --- on foot: the D-pad cluster panes and both rupee groups, nothing else
    {
        World w;
        HideResolver r;
        const auto t = r.Sample(w.Reader(), kMain);
        assert(t.valid && t.help);
        assert(t.entries.size() == 13);
        assert(r.Resolves() == 1);
        assert(r.Sample(w.Reader(), kMain).entries == t.entries && r.Resolves() == 1); // cached
        for (const char* k : kDpadKeys) {
            assert(Has(t.entries, w.pane[k]));
        }
        for (const char* k : kKept)
            assert(!Has(t.entries, w.pane[k]));
        assert(Has(t.entries, w.pane["M/N_rupyAll_00"]) && Has(t.entries, w.pane["R/N_rupyAll_00"]));
        int dpad = 0, rupee = 0;
        for (const auto& e : t.entries) {
            dpad += e.group == kHideDpad;
            rupee += e.group == kHideRupee;
            char nm[25]{};
            w.mem.Read(e.pane + 0xEC, nm, 24);
            assert(e.name_lo == Lo(nm) && e.name_hi == Hi(nm)); // the hook's name check
        }
        assert(dpad == 11 && rupee == 2);
        assert(static_cast<int>(t.entries.size()) <= kHideMax);
    }
    // --- title / Quest Logs (Help object null): still valid targets; dev.37: without Help the cross
    //     and dowsing panes stay listed. dev.48: the resolver reads no menu word at all (the stack slot
    //     main+0x192D0E4 dev.29..47 read here); whatever it holds changes nothing
    {
        World w;
        HideResolver r;
        w.mem.U8(kMain + 0x192D0E4, 0x11);
        auto t = r.Sample(w.Reader(), kMain);
        assert(t.valid && t.help && t.entries.size() == 13);
        w.mem.U64(w.m + 0x20D08, 0);
        t = r.Sample(w.Reader(), kMain);
        assert(t.valid && !t.help && t.entries.size() == 12);
        assert(!Has(t.entries, w.pane["H/N_all_00"]) && Has(t.entries, w.pane["C/N_bace_00"]));
        assert(r.Resolves() == 2); // looked up again only because the Help widget went away
    }
    // --- refusals: no meter, wrong vtable, wrong owner name -> invalid, no entries
    {
        World w;
        HideResolver r;
        w.mem.U64(kMain + 0x183C9A0, 0);
        auto t = r.Sample(w.Reader(), kMain);
        assert(!t.valid && t.entries.empty());
    }
    {
        World w;
        HideResolver r;
        w.mem.U64(w.d, kMain + 0x1234);
        assert(!r.Sample(w.Reader(), kMain).valid);
    }
    {
        World w;
        HideResolver r;
        w.Rename("H/owner", "N_other_00");
        assert(!r.Sample(w.Reader(), kMain).valid);
    }
    // --- a missing or ambiguous cluster pane: the D-pad group stays native as a whole
    {
        World w;
        HideResolver r;
        w.Pane("D/N_showSet_00b", "N_showSet_00", w.pane["D/N_itemAll_00"]);
        auto t = r.Sample(w.Reader(), kMain);
        assert(t.valid);
        for (const auto& e : t.entries) assert(e.group == kHideRupee);
    }
    {
        World w;
        HideResolver r;
        w.Rename("C/N_textR_00", "N_textX_00");
        auto t = r.Sample(w.Reader(), kMain);
        assert(t.valid && t.entries.size() == 2);
    }
    // --- the rupy layout not attached under N_rupy_00: the meter group alone
    {
        World w;
        HideResolver r;
        w.Rename("R/root", "Other");
        auto t = r.Sample(w.Reader(), kMain);
        assert(t.valid && t.entries.size() == 12 && Has(t.entries, w.pane["M/N_rupyAll_00"]));
    }
    // --- a cached pane renamed (allocation reused): looked up again; a new widget root re-resolves
    {
        World w;
        HideResolver r;
        auto t = r.Sample(w.Reader(), kMain);
        assert(t.entries.size() == 13);
        const uint64_t old_h = w.pane["H/N_all_00"];
        const uint64_t rh2 = w.Pane("H/root2", "RootPane", w.pane["H/owner"]);
        const uint64_t hall2 = w.Pane("H/N_all_00b", "N_all_00", rh2);
        w.mem.U64(w.h + 0xC0, rh2);
        t = r.Sample(w.Reader(), kMain);
        assert(t.valid && Has(t.entries, hall2) && !Has(t.entries, old_h));
        w.Rename("C/N_bace_00", "Picture_99"); // reused: the D-pad group drops out, rupees stay
        t = r.Sample(w.Reader(), kMain);
        assert(t.valid && t.entries.size() == 2);
    }
}

void StateMachineTests() {
    using namespace sshd;
    World w;
    HideResolver r;
    const auto t = r.Sample(w.Reader(), kMain);
    HudHide hh;
    // defaults: both settings Off -> everything hidden, written once, then refreshed
    auto s = hh.Step(false, false, true, t);
    assert(s.kind == HideStep::kWrite && s.list.size() == 13 && hh.Active());
    s = hh.Step(false, false, true, t);
    assert(s.kind == HideStep::kRefresh);
    // re-show the rupees: a new list without them
    s = hh.Step(false, true, true, t);
    assert(s.kind == HideStep::kWrite && s.list.size() == 11);
    for (const auto& e : s.list) assert(e.group == kHideDpad);
    // re-show the D-pad too: nothing left to hide -> cleared once, then idle
    s = hh.Step(true, true, true, t);
    assert(s.kind == HideStep::kClear && !hh.Active());
    s = hh.Step(true, true, true, t);
    assert(s.kind == HideStep::kNone);
    // hide again
    s = hh.Step(false, false, true, t);
    assert(s.kind == HideStep::kWrite && s.list.size() == 13);
    // only the rupees hidden
    s = hh.Step(true, false, true, t);
    assert(s.kind == HideStep::kWrite && s.list.size() == 2);
    s = hh.Step(false, false, true, t);
    assert(s.kind == HideStep::kWrite && s.list.size() == 13);
    // dev.37: a menu opens: the same list stays hidden (refreshed, never cleared)
    s = hh.Step(false, false, true, t, false, true);
    assert(s.kind == HideStep::kRefresh && hh.Active());
    // the Help widget goes (title, around a load): the list without it, written once, no clear
    HideTargets nohelp = t;
    nohelp.help = false;
    nohelp.entries.pop_back();
    s = hh.Step(false, false, true, nohelp);
    assert(s.kind == HideStep::kWrite && s.list.size() == 12);
    s = hh.Step(false, false, true, t);
    assert(s.kind == HideStep::kWrite && s.list.size() == 13);
    // the helper stalls (the game loop stopped: nothing draws) or a sample is refused (an
    // off-thread read mid-update, the meter rebuilt): hold, no refresh, never a clear
    for (int k = 0; k < 200; ++k) {
        s = hh.Step(false, false, false, t);
        assert(s.kind == HideStep::kNone && hh.Active());
        s = hh.Step(false, false, true, HideTargets{});
        assert(s.kind == HideStep::kNone && hh.Active());
    }
    s = hh.Step(false, false, true, t); // valid again: the same list, refreshed (not rewritten)
    assert(s.kind == HideStep::kRefresh);
    // a setting shown while the targets are unreadable: the held list loses that group at once
    s = hh.Step(false, true, true, HideTargets{});
    assert(s.kind == HideStep::kWrite && s.list.size() == 11);
    s = hh.Step(true, true, true, HideTargets{});
    assert(s.kind == HideStep::kClear && !hh.Active());
    // nothing applied: invalid samples stay idle
    s = hh.Step(false, false, true, HideTargets{});
    assert(s.kind == HideStep::kNone && !hh.Active());
    // a changed pane address rewrites the list
    s = hh.Step(false, false, true, t);
    assert(s.kind == HideStep::kWrite);
    HideTargets moved = t;
    moved.entries[0].pane += 0x1000;
    s = hh.Step(false, false, true, moved);
    assert(s.kind == HideStep::kWrite && s.list[0].pane == t.entries[0].pane + 0x1000);
    // capacity: never more than the mailbox holds
    HideTargets many = t;
    while (many.entries.size() < 20) many.entries.push_back(t.entries[0]);
    s = hh.Step(false, false, true, many);
    assert(static_cast<int>(s.list.size()) == kHideMax);
    // the dev.36 behaviour (diagnostics replay): cleared in menus, without Help, on a refused sample
    // and on a stalled helper
    HudHide old;
    assert(old.Step(false, false, true, t, true).kind == HideStep::kWrite);
    assert(old.Step(false, false, true, t, true, true).kind == HideStep::kClear); // the module's menu state
    assert(old.Step(false, false, true, t, true).kind == HideStep::kWrite);
    assert(old.Step(false, false, true, nohelp, true).kind == HideStep::kClear);
    assert(old.Step(false, false, true, t, true).kind == HideStep::kWrite);
    assert(old.Step(false, false, true, HideTargets{}, true).kind == HideStep::kClear);
    assert(old.Step(false, false, true, t, true).kind == HideStep::kWrite);
    assert(old.Step(false, false, false, t, true).kind == HideStep::kClear);
}

// --- the module: flags, mailbox list layout, restore paths, D-pad mirror unaffected
struct Host {
    World w;
    std::map<std::string, int64_t> ints, flags;
    uint32_t mailbox[1024]{};
    std::vector<uint32_t> selects; // every value stored to the select word (+0x204)
};

void ModuleTest() {
    Host hs;
    const auto* api = eden_dsmod_get_module(EDEN_DSMOD_MODULE_ABI_VERSION, EDEN_DSMOD_MODULE_ABI_HASH);
    const auto* ext = eden_dsmod_get_extensions(EDEN_DSMOD_EXT_VERSION, EDEN_DSMOD_EXT_HASH);
    assert(api && ext);
    EdenDsmodHostApi h{};
    h.abi_version = EDEN_DSMOD_MODULE_ABI_VERSION;
    h.abi_hash = EDEN_DSMOD_MODULE_ABI_HASH;
    h.struct_size = sizeof(h);
    h.title_id = 0x01002DA013484000ull;
    const uint8_t build[] = {0x54, 0x41, 0xAF, 0x3C, 0x89, 0xF9, 0x7E, 0x1D, 0x25, 0xA8,
                             0xCC, 0xF1, 0x4C, 0x74, 0x2B, 0xF8, 0x23, 0x36, 0xD8, 0xA8};
    std::memcpy(h.build_id, build, sizeof(build));
    h.main_base = kMain;
    h.userdata = &hs;
    h.read_memory = [](void* p, uint64_t a, void* out, size_t n) -> EdenDsmodBool {
        return static_cast<Host*>(p)->w.mem.Read(a, out, n) ? EDEN_DSMOD_TRUE : EDEN_DSMOD_FALSE;
    };
    h.publish_i64 = [](void* p, const char* k, int64_t v) { static_cast<Host*>(p)->ints[k] = v; };
    h.publish_f64 = [](void*, const char*, double) {};
    h.publish_text = [](void*, const char*, const char*) {};
    h.get_i64 = [](void* p, const char* k, int64_t def) -> int64_t {
        const auto& f = static_cast<Host*>(p)->flags;
        const auto it = f.find(k);
        return it == f.end() ? def : it->second;
    };
    void* inst = api->create(&h, "");
    assert(inst);
    auto& mb = hs.mailbox;
    auto& i = hs.ints;
    auto tick = [&] { ++mb[0x68 / 4]; api->sample(inst, &h); };
    // no load plan (no mailbox): nothing can be hidden, the D-pad mirror publishes as before
    tick();
    tick();
    assert(i.at("sshd.hud.hidden") == 0 && i.at("sshd.dp.fi") == 15 && i.at("sshd.dp.help") == 15);
    EdenDsmodHostExtensions e{};
    e.version = EDEN_DSMOD_EXT_VERSION;
    e.abi_hash = EDEN_DSMOD_EXT_HASH;
    e.struct_size = sizeof(e);
    e.userdata = &hs;
    e.mailbox_address = 0x9000000;
    e.mailbox_size = 4096;
    e.load_u32 = [](void* p, uint32_t o, uint32_t* v) -> EdenDsmodBool { *v = static_cast<Host*>(p)->mailbox[o / 4]; return EDEN_DSMOD_TRUE; };
    e.store_u32 = [](void* p, uint32_t o, uint32_t v) -> EdenDsmodBool {
        auto& hs = *static_cast<Host*>(p);
        hs.mailbox[o / 4] = v;
        if (o == 0x204)
            hs.selects.push_back(v);
        return EDEN_DSMOD_TRUE;
    };
    mb[0x204 / 4] = 0x107; // a stale list from an earlier session is dropped on configure
    mb[0x200 / 4] = 9;
    mb[0x41C / 4] = 5;
    ext->configure(inst, &e);
    assert(mb[0x204 / 4] == 0 && mb[0x200 / 4] == 0 && mb[0x41C / 4] == 0 && mb[0x410 / 4] == 0);
    auto count = [&] { return mb[0x204 / 4] & 0xFF; };
    auto bank = [&] { return (mb[0x204 / 4] >> 8) & 1; };
    // defaults (flags absent = Off): D-pad + rupees hidden; the list as the helper reads it, in the
    // bank the select word names (the first write goes to bank 1)
    hs.selects.clear();
    tick();
    assert(count() == 13 && bank() == 1 && mb[0x200 / 4] == sshd::kHideTtl && sshd::kHideTtl == 120);
    assert(i.at("sshd.hud.hidden") == 3);
    assert(hs.selects.size() == 1); // one store publishes bank + count
    auto entry = [&](int k) {
        const uint32_t at = 0x210 + 0x100 * bank() + 16 * k;
        const uint64_t p = mb[at / 4] | (uint64_t{mb[at / 4 + 1]} << 32);
        return std::make_tuple(p, mb[at / 4 + 2], mb[at / 4 + 3]);
    };
    std::vector<uint64_t> listed;
    for (int k = 0; k < 13; ++k) {
        const auto [p, lo, hi] = entry(k);
        char nm[25]{};
        assert(hs.w.mem.Read(p + 0xEC, nm, 24));
        assert(lo == Lo(nm) && hi == Hi(nm));
        listed.push_back(p);
    }
    for (const char* k : kDpadKeys) {
        bool found = false;
        for (const uint64_t p : listed) found = found || p == hs.w.pane[k];
        assert(found);
    }
    // the helper counts the ttl down; the module refreshes it every sample
    mb[0x200 / 4] = 3;
    mb[0x208 / 4] = 1234; // draws the hook skipped
    tick();
    assert(mb[0x200 / 4] == sshd::kHideTtl && i.at("sshd.hud.hits") == 1234);
    // top hidden, bottom mirror unchanged: Help -> Close, Fi attention, dowse text and target art
    hs.w.mem.U8(hs.w.m + 0x218B0, 1);
    hs.w.mem.U8(hs.w.c + 0xB60, 1);
    tick();
    tick();
    assert(i.at("sshd.dp.close") == 15 && i.at("sshd.dp.help") == 0 && i.at("sshd.dp.fiatt") == 1);
    assert(i.at("sshd.dp.fih0") > 0 && i.at("sshd.dp.dowse") == 15 && i.at("sshd.dp.eye") == 12);
    assert(count() == 13);
    // Options: Top rupees On -> the rupee panes leave the list: the other bank, one store
    hs.selects.clear();
    const uint32_t b0 = bank();
    hs.flags["@flag:hud.rupee"] = 1;
    tick();
    assert(count() == 11 && bank() != b0 && i.at("sshd.hud.hidden") == 1 && hs.selects.size() == 1);
    // Top D-pad On as well -> list cleared at once
    hs.flags["@flag:hud.dpad"] = 1;
    tick();
    assert(count() == 0 && mb[0x200 / 4] == 0 && i.at("sshd.hud.hidden") == 0);
    assert(i.at("sshd.dp.close") == 15); // the mirror never depended on the hiding
    // both Off again -> hidden again
    hs.flags.clear();
    tick();
    assert(count() == 13);
    // from here on the count never drops to 0 while the list changes (dev.37: the flash)
    hs.selects.clear();
    // pause menu (dev.48: the helper's game-thread menu sample the module's gates read, code 1, for
    // long enough to open the debounced gate): stays hidden (dev.37), nothing rewritten
    const auto menu_tick = [&](uint32_t code) {
        mb[0x6C / 4] = code;
        mb[0x74 / 4] = code != 0 ? 0x148 : 0;
        mb[0x70 / 4] = mb[0x68 / 4] + 1; // this frame's stamp (tick() beats the loop first)
        tick();
    };
    for (int k = 0; k < 6; ++k)
        menu_tick(1);
    assert(i.at("sshd.menu.open") == 1);
    assert(count() == 13 && mb[0x200 / 4] == sshd::kHideTtl && hs.selects.empty());
    for (int k = 0; k < 6; ++k)
        menu_tick(0);
    assert(i.at("sshd.menu.open") == 0);
    assert(count() == 13 && hs.selects.empty());
    // title / around a load (Help object gone): the cross stays hidden, the list without Help
    hs.w.mem.U64(hs.w.m + 0x20D08, 0);
    tick();
    assert(count() == 12);
    hs.w.mem.U64(hs.w.m + 0x20D08, hs.w.h);
    tick();
    assert(count() == 13);
    // the meter unreadable for a while (rebuilt around a load): held, the ttl not refreshed
    hs.w.mem.U64(kMain + 0x183C9A0, 0);
    mb[0x200 / 4] = 50;
    for (int k = 0; k < 20; ++k) tick();
    assert(count() == 13 && mb[0x200 / 4] == 50);
    hs.w.mem.U64(kMain + 0x183C9A0, hs.w.m);
    tick();
    assert(count() == 13 && mb[0x200 / 4] == sshd::kHideTtl);
    // the loop hook stops beating (the game loop stalled): held as well, refreshed once it beats
    for (int k = 0; k < 40; ++k) api->sample(inst, &h);
    assert(count() == 13);
    tick();
    assert(count() == 13 && mb[0x200 / 4] == sshd::kHideTtl);
    for (const uint32_t v : hs.selects) assert((v & 0xFF) != 0);
    // unload: list cleared synchronously
    api->destroy(inst);
    assert(count() == 0 && mb[0x200 / 4] == 0);
}

} // namespace

int main() {
    ResolverTests();
    StateMachineTests();
    ModuleTest();
    return 0;
}
