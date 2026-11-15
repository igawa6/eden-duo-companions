// SPDX-License-Identifier: GPL-3.0-or-later
// Ready D-pad cluster reader (display only; D-pad reader analysis contract):
// rendered-pane visibility V(p) with the full ancestor test, stale child alpha under a hidden
// parent, map / pause hiding the whole cluster, Help / Close and Look / Dowse / Back selectors,
// the Fi attention latch gating the halos, the selected dowsing target (A/B active file, slot
// availability, logical id + both animation frames, ancestor-disambiguated pane), refusals
// (wrong vtable, owner name, null meter, cycles, ambiguous names, identity change), the two-sample
// debounce and the module's published levels / target key.
#include <cassert>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "core/mods/dsmod_module_abi.h"
#include "core/mods/dsmod_module_extensions.h"
#include "sshd_dpad.h"

namespace {

constexpr uint64_t kMain = 0x80000000ull;

struct Mem {
    std::map<uint64_t, uint8_t> b;
    void Put(uint64_t a, const void* p, size_t n) {
        for (size_t i = 0; i < n; ++i) b[a + i] = static_cast<const uint8_t*>(p)[i];
    }
    void U64(uint64_t a, uint64_t v) { Put(a, &v, 8); }
    void U32(uint64_t a, uint32_t v) { Put(a, &v, 4); }
    void U16(uint64_t a, uint16_t v) { Put(a, &v, 2); }
    void U8(uint64_t a, uint8_t v) { Put(a, &v, 1); }
    void F32(uint64_t a, float v) { Put(a, &v, 4); }
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

struct World {
    Mem mem;
    uint64_t next = 0x90000000ull;
    uint64_t m = 0x86000000ull, c, d, h = 0x87000000ull, fm = 0x88000000ull;
    std::map<std::string, uint64_t> pane; // "C/N_all_00" etc.
    uint64_t Pane(const std::string& key, const char* name, uint64_t parent, uint8_t alpha = 255, bool pic = false) {
        const uint64_t p = next;
        next += 0x200;
        char nm[24]{};
        std::strncpy(nm, name, 23);
        mem.Put(p + 0xEC, nm, 24);
        mem.U64(p + 0x18, parent);
        const uint8_t ab[4] = {alpha, alpha, 0, 1};
        mem.Put(p + 0xE8, ab, 4);
        mem.U64(p + 0x50, pic ? 0x95000000ull : 0);
        mem.U64(p + 0x28, p + 0x28); // empty child list
        if (parent != 0) { // append to the parent's intrusive list (node = child + 8)
            uint64_t prev = parent + 0x28, node = 0;
            mem.Read(prev, &node, 8);
            while (node != parent + 0x28) { prev = node; mem.Read(node, &node, 8); }
            mem.U64(prev, p + 8);
            mem.U64(p + 8, parent + 0x28);
        }
        pane[key] = p;
        return p;
    }
    void Flags(uint64_t a, uint8_t eb) { mem.U8(a + 0xEB, eb); }
    void Alpha(uint64_t a, uint8_t e9) { mem.U8(a + 0xE9, e9); }
    uint64_t story_data() const { return fm + 0x10 + 0x8E4; }
    void Item(int f, bool on) { // synthetic item table: flag f = bit f % 16 of word f / 16
        const uint64_t a = fm + 0x10 + 0x9E4 + 2 * (f / 16);
        uint16_t w = 0;
        mem.Read(a, &w, 2);
        w = on ? (w | (1 << (f % 16))) : (w & ~(1 << (f % 16)));
        mem.U16(a, w);
    }
    void Story(int f, bool on) {
        uint16_t w = 0;
        mem.Read(story_data() + 2 * (f / 16), &w, 2);
        w = on ? (w | (1 << (f % 16))) : (w & ~(1 << (f % 16)));
        mem.U16(story_data() + 2 * (f / 16), w);
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
        mem.U32(d + 0x9100, 0);
        // HUD root and the externally attached owners
        const uint64_t hud = Pane("HUD", "RootPane", 0);
        const uint64_t oc = Pane("C/owner", "N_remoConBtn_03", hud), od = Pane("D/owner", "N_nunBtn_03", hud),
                       oh = Pane("H/owner", "N_remoConBtn_05", hud);
        mem.U64(c + 0x6A0, oc);
        mem.U64(d + 0x1678, od);
        mem.U64(h + 0x2B8, oh);
        // Cross
        const uint64_t rc = Pane("C/root", "RootPane", oc);
        mem.U64(c + 0xC0, rc);
        const uint64_t all = Pane("C/N_all_00", "N_all_00", rc), key = Pane("C/N_key_00", "N_key_00", all);
        Pane("C/base", "P_crossBtn_00", Pane("C/N_bace_00", "N_bace_00", key), 128, true);
        Pane("C/down", "P_crossBtn_02", Pane("C/N_keyD_00", "N_keyD_00", key), 255, true);
        Pane("C/left", "P_crossBtn_04", Pane("C/N_keyL_00", "N_keyL_00", key), 255, true);
        const uint64_t icd = Pane("C/N_iconD_00", "N_iconD_00", all);
        const uint64_t sw = Pane("C/N_swordIcon_00", "N_swordIcon_00", icd);
        Pane("C/fis0", "P_plusIconS_00", sw, 64, true);
        Pane("C/fis1", "P_plusIconS_01", sw, 200, true);
        Pane("C/fi", "P_plusIcon_01", sw, 255, true);
        Pane("C/fih0", "P_plusIcon_00", sw, 90, true);
        Pane("C/fih1", "P_plusIcon_02", sw, 40, true);
        Pane("C/bird", "P_bairdIcon_00", Pane("C/N_birdIcon_00", "N_birdIcon_00", icd), 255, true);
        // Dowsing (embedded layout: root at D+0x700)
        const uint64_t rd = Pane("D/root", "RootPane", od);
        mem.U64(d + 0x700, rd);
        const uint64_t ia = Pane("D/N_itemAll_00", "N_itemAll_00", rd);
        const uint64_t set = Pane("D/N_iconSet_00", "N_iconSet_00",
                                  Pane("D/N_setBtn_00", "N_setBtn_00", Pane("D/N_showSet_00", "N_showSet_00", ia)));
        Pane("D/target", "P_comIcon_00", set, 255, true);
        // the native wheel's own icons share the pane name: never the Ready target
        Pane("D/wheel_icon", "P_comIcon_00", Pane("D/N_A_00", "N_A_00", Pane("D/N_itemSelect_00", "N_itemSelect_00", ia)), 255, true);
        const uint64_t simple = Pane("D/N_showSimple_00", "N_showSimple_00", ia);
        Pane("D/utext", "T_dawsing_00", Pane("D/N_input_00", "N_input_00", simple));
        const uint64_t cb = Pane("D/N_crossBtn_00", "N_crossBtn_00", Pane("D/N_input_01", "N_input_01", simple));
        Pane("D/base", "P_crossBtn_00", cb, 190, true);
        Pane("D/eye", "P_btn_00", Pane("D/N_lookBtns_00", "N_lookBtns_00", cb), 200, true);
        Pane("D/up", "P_btn_01", cb, 255, true);
        // Help
        const uint64_t rh = Pane("H/root", "RootPane", oh);
        mem.U64(h + 0xC0, rh);
        const uint64_t hall = Pane("H/N_all_00", "N_all_00", rh);
        Pane("H/htext", "T_helpBtn_00", hall);
        Pane("H/base", "P_crossBtn_00", hall, 128, true);
        Pane("H/right", "P_btn_02", hall, 255, true);
        // selected dowsing target: file manager A, registry deciding A, flag tables, icon object
        mem.U64(kMain + 0x2BF9CC0, fm);
        mem.U8(fm + 0xCD06, 0);
        const uint64_t node = 0x89000000ull, obj = 0x89100000ull;
        mem.U64(kMain + 0x1688510, node);
        mem.U64(node + 0x20, obj);
        mem.U8(obj + 0x16, 1);
        mem.U16(obj + 0x10, 1); // value != 0 -> file A
        for (const uint64_t mgr_ptr : {kMain + 0x182E1F0, kMain + 0x182E168}) {
            const uint64_t mgr = 0x8A000000ull + (mgr_ptr & 0xFFF) * 0x10000, meta = mgr + 0x1000, table = mgr + 0x2000;
            mem.U64(mgr_ptr, mgr);
            mem.U64(mgr + 0x18, meta);
            mem.U64(meta + 8, table);
            for (int f = 0; f < 0x300; ++f) { // flag f = bit f % 16 of word f / 16
                const uint8_t md[2] = {static_cast<uint8_t>(f / 16), static_cast<uint8_t>((f % 16) << 4)};
                mem.Put(table + 2 * f, md, 2);
            }
        }
        for (int i = 0; i < 0x60; ++i) mem.U8(story_data() + i, 0);
        for (int i = 0; i < 0x40; ++i) mem.U8(fm + 0x10 + 0x9E4 + i, 0);
        mem.U8(fm + 0x10 + 0x53B2, 7); // treasure
        Story(0x66, true);             // treasure dowsing unlocked
        const uint64_t icon = d + 0x1700, ctl = 0x8B000000ull, xf = 0x8B100000ull;
        mem.U8(icon + 0x80, 2);
        mem.U8(icon + 0xD60, 6);
        mem.U64(icon + 0xCA8, ctl);
        mem.F32(ctl + 0x0C, 6.0f);
        mem.U64(icon + 0xCE0, xf);
        mem.F32(xf + 0x20, 6.0f);
    }
};

sshd::DpadState Two(sshd::DpadReader& r, const World& w) {
    const sshd::DpadRead rd = [&w](uint64_t a, void* o, size_t n) { return w.mem.Read(a, o, n); };
    r.Sample(rd, kMain);
    return r.Sample(rd, kMain);
}

void ReaderTests() {
    using namespace sshd;
    // --- normal on foot: base, eye, Fi, Help; first sample is withheld (two-sample debounce)
    {
        World w;
        DpadReader r;
        const DpadRead rd = [&w](uint64_t a, void* o, size_t n) { return w.mem.Read(a, o, n); };
        auto s1 = r.Sample(rd, kMain);
        assert(s1.valid && s1.alpha[kDpFi] == 0 && s1.alpha[kDpBaseC] == 0);
        auto s = r.Sample(rd, kMain);
        assert(s.valid);
        assert(s.alpha[kDpBaseC] == 128 && s.alpha[kDpBaseD] == 190 && s.alpha[kDpBaseH] == 128);
        assert(s.alpha[kDpEye] == 200 && s.alpha[kDpFi] == 255 && s.alpha[kDpFiShadow1] == 200);
        assert(s.alpha[kDpUp] == 255 && s.alpha[kDpDown] == 255 && s.alpha[kDpRight] == 255);
        assert(s.alpha[kDpHelpText] == 255 && !s.help_close);
        assert(s.alpha[kDpUpText] == 0 && s.up_text == 0); // no recognised message
        assert(s.alpha[kDpFiHalo0] == 0 && s.alpha[kDpFiHalo1] == 0 && !s.fi_attention); // no latch
        assert(s.alpha[kDpTarget] == 255 && s.target_logical == 6 && s.target_slot == 7);
        assert(std::strcmp(DowsingTargetArt(s.target_logical), "tr_dauzTarget_06") == 0);
        // a layer that stops rendering hides at once
        w.Alpha(w.pane["C/fi"], 0);
        s = r.Sample(rd, kMain);
        assert(s.alpha[kDpFi] == 0 && s.alpha[kDpBaseC] == 128);
        w.Alpha(w.pane["C/fi"], 255);
        s = r.Sample(rd, kMain);
        assert(s.alpha[kDpFi] == 0); // back after two rendered samples only
        s = r.Sample(rd, kMain);
        assert(s.alpha[kDpFi] == 255);
    }
    // --- a hidden ancestor hides the subtree although the children keep stale alpha
    {
        World w;
        DpadReader r;
        w.Flags(w.pane["C/N_iconD_00"], 0);
        auto s = Two(r, w);
        assert(s.valid && s.alpha[kDpFi] == 0 && s.alpha[kDpFiShadow0] == 0 && s.alpha[kDpBird] == 0);
        assert(s.alpha[kDpBaseC] == 128); // a sibling branch still draws
        // the bird draws only when its own branch renders (selector bytes are never consulted)
        w.Flags(w.pane["C/N_iconD_00"], 1);
        w.Flags(w.pane["C/N_birdIcon_00"], 0);
        s = Two(r, w);
        assert(s.alpha[kDpBird] == 0 && s.alpha[kDpFi] == 255);
    }
    // --- map / pause / Fi dialogue: the HUD root (an external ancestor) stops drawing -> all hidden
    {
        World w;
        DpadReader r;
        w.Flags(w.pane["HUD"], 0);
        auto s = Two(r, w);
        assert(s.valid);
        for (int l = 0; l < kDpLayerCount; ++l) assert(s.alpha[l] == 0);
        assert(s.target_logical == -1);
    }
    // --- Help open -> Close; Up messages Look / Dowse / Back; an unknown message hides the text
    {
        World w;
        DpadReader r;
        w.mem.U8(w.m + 0x218B0, 1);
        w.mem.U32(w.d + 0x9100, 0x52);
        auto s = Two(r, w);
        assert(s.help_close && s.alpha[kDpHelpText] == 255 && s.up_text == 2 && s.alpha[kDpUpText] == 255);
        w.mem.U32(w.d + 0x9100, 0x51);
        s = Two(r, w);
        assert(s.up_text == 1 && s.alpha[kDpUpText] == 255);
        w.mem.U32(w.d + 0x9100, 0x6C);
        s = Two(r, w);
        assert(s.up_text == 3);
        w.mem.U32(w.d + 0x9100, 0x99);
        s = Two(r, w);
        assert(s.up_text == 0 && s.alpha[kDpUpText] == 0);
        // the text pane hidden: no label even with a known message
        w.mem.U32(w.d + 0x9100, 0x52);
        w.Flags(w.pane["D/N_input_00"], 0);
        s = Two(r, w);
        assert(s.alpha[kDpUpText] == 0);
    }
    // --- Fi attention: latch B5F (current) or B60 (prior) with Fi rendered; halos mirror their E9
    {
        World w;
        DpadReader r;
        w.mem.U8(w.c + 0xB60, 1);
        auto s = Two(r, w);
        assert(s.fi_attention && s.alpha[kDpFiHalo0] == 90 && s.alpha[kDpFiHalo1] == 40);
        w.Alpha(w.pane["C/fih0"], 0); // pulse trough
        s = Two(r, w);
        assert(s.fi_attention && s.alpha[kDpFiHalo0] == 0);
        w.Flags(w.pane["C/N_swordIcon_00"], 0); // Fi hidden (active target dowsing): no attention
        s = Two(r, w);
        assert(!s.fi_attention && s.alpha[kDpFiHalo1] == 0);
    }
    // --- selected target refusals
    {
        World w;
        DpadReader r;
        w.Story(0x66, false); // treasure dowsing not unlocked: availability wins
        auto s = Two(r, w);
        assert(s.alpha[kDpTarget] == 0 && s.target_logical == -1);
        w.Story(0x66, true);
        w.mem.U8(w.d + 0x1700 + 0xD60, 3); // logical id disagrees with the slot's resolver
        s = Two(r, w);
        assert(s.alpha[kDpTarget] == 0);
        w.mem.U8(w.d + 0x1700 + 0xD60, 6);
        w.mem.F32(0x8B100000ull + 0x20, 3.0f); // applied frame disagrees (mixed sample)
        s = Two(r, w);
        assert(s.alpha[kDpTarget] == 0);
        w.mem.F32(0x8B100000ull + 0x20, 6.0f);
        w.mem.U8(w.fm + 0x10 + 0x53B2, 8); // no selection
        s = Two(r, w);
        assert(s.alpha[kDpTarget] == 0);
        // crystals (slot 3 -> logical 3, SF68)
        w.mem.U8(w.fm + 0x10 + 0x53B2, 3);
        w.Story(0x68, true);
        w.mem.U8(w.d + 0x1700 + 0xD60, 3);
        w.mem.F32(0x8B000000ull + 0x0C, 3.0f);
        w.mem.F32(0x8B100000ull + 0x20, 3.0f);
        s = Two(r, w);
        assert(s.alpha[kDpTarget] == 255 && s.target_logical == 3);
        assert(std::strcmp(DowsingTargetArt(3), "uk_dawGenki_00") == 0);
        // the registry chooses file B (title / Quest Logs): B's slot 8 -> none
        w.mem.U16(0x89100000ull + 0x10, 0);
        w.mem.U8(w.fm + 0x53D0 + 0x53B2, 8);
        s = Two(r, w);
        assert(s.alpha[kDpTarget] == 0);
        // story slot 0 resolves dynamically; locked story never draws
        w.mem.U16(0x89100000ull + 0x10, 1);
        w.mem.U8(w.fm + 0x10 + 0x53B2, 0);
        s = Two(r, w);
        assert(s.alpha[kDpTarget] == 0);
        w.Story(0x64, true); // SF64 -> logical 19
        w.mem.U8(w.d + 0x1700 + 0xD60, 19);
        w.mem.F32(0x8B000000ull + 0x0C, 19.0f);
        w.mem.F32(0x8B100000ull + 0x20, 19.0f);
        s = Two(r, w);
        assert(s.alpha[kDpTarget] == 255 && s.target_logical == 19);
    }
    // --- two panes of a contract name under the required ancestor: ambiguous, never drawn
    {
        World w;
        w.Pane("D/target2", "P_comIcon_00", w.pane["D/N_iconSet_00"], 255, true);
        DpadReader r;
        auto s = Two(r, w);
        assert(s.valid && s.alpha[kDpTarget] == 0 && s.alpha[kDpEye] == 200);
    }
    // --- refusals clear everything
    {
        World w;
        DpadReader r;
        w.mem.U64(w.c, kMain + 0x1234); // wrong Cross vtable
        auto s = Two(r, w);
        assert(!s.valid && s.alpha[kDpBaseD] == 0);
    }
    {
        World w;
        DpadReader r;
        w.mem.U64(kMain + 0x183C9A0, 0); // no meter (boot)
        assert(!Two(r, w).valid);
    }
    {
        World w;
        DpadReader r;
        char bad[24] = "N_other_00";
        w.mem.Put(w.pane["D/owner"] + 0xEC, bad, 24); // owner name mismatch
        assert(!Two(r, w).valid);
    }
    {
        World w;
        DpadReader r;
        w.mem.U64(w.pane["C/N_all_00"] + 0x18, w.pane["C/fi"]); // parent cycle
        assert(!Two(r, w).valid);
    }
    // --- Help object null (title / menus): Help layers off, the rest validated
    {
        World w;
        DpadReader r;
        w.mem.U64(w.m + 0x20D08, 0);
        auto s = Two(r, w);
        assert(s.valid && s.alpha[kDpHelpText] == 0 && s.alpha[kDpRight] == 0 && s.alpha[kDpBaseC] == 128);
    }
    // --- a new widget root (reload) is looked up again; its first sample is withheld
    {
        World w;
        DpadReader r;
        const DpadRead rd = [&w](uint64_t a, void* o, size_t n) { return w.mem.Read(a, o, n); };
        auto s = Two(r, w);
        assert(s.alpha[kDpRight] == 255);
        const uint64_t rh2 = w.Pane("H/root2", "RootPane", w.pane["H/owner"]);
        w.Pane("H/right2", "P_btn_02", rh2, 200, true);
        w.mem.U64(w.h + 0xC0, rh2);
        s = r.Sample(rd, kMain);
        assert(s.valid && s.alpha[kDpRight] == 0);
        s = r.Sample(rd, kMain);
        assert(s.alpha[kDpRight] == 200 && s.alpha[kDpHelpText] == 0);
        // an unreadable pane read rejects the sample
        w.mem.b.erase(w.pane["H/right2"] + 0xE9);
        assert(!r.Sample(rd, kMain).valid);
    }
}

// --- the module publishes levels and the target key
struct Host {
    World w;
    std::map<std::string, int64_t> ints;
    std::map<std::string, std::string> texts;
};

void ModuleTest() {
    Host hs;
    const auto* api = eden_dsmod_get_module(EDEN_DSMOD_MODULE_ABI_VERSION, EDEN_DSMOD_MODULE_ABI_HASH);
    assert(api);
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
    h.publish_text = [](void* p, const char* k, const char* v) { static_cast<Host*>(p)->texts[k] = v; };
    void* inst = api->create(&h, "");
    assert(inst);
    hs.w.mem.U32(hs.w.d + 0x9100, 0x52);
    api->sample(inst, &h);
    assert(hs.ints.at("sshd.dp.valid") == 1 && hs.ints.at("sshd.dp.fi") == 0); // debounce
    api->sample(inst, &h);
    auto& i = hs.ints;
    assert(i.at("sshd.dp.base_c") == 8 && i.at("sshd.dp.eye") == 12 && i.at("sshd.dp.fi") == 15);
    assert(i.at("sshd.dp.dowse") == 15 && i.at("sshd.dp.look") == 0 && i.at("sshd.dp.back") == 0);
    assert(i.at("sshd.dp.help") == 15 && i.at("sshd.dp.close") == 0);
    assert(i.at("sshd.dp.target") == 15 && hs.texts.at("sshd.dp.tsrc") == "module:sshd:dpad/target/6");
    assert(i.at("sshd.dp.fih0") == 0);
    assert(i.at("sshd.dp.swim") == 0);
    // --- dev.33 swimming (reader analysis): the verified surface states keep the pane mirror
    // (Fi hidden by the native itself), any other water state and Skyloft's lake hide the cluster
    const uint64_t player = 0x8C000000ull, slot = kMain + 0x5AEAD54;
    hs.w.mem.U64(kMain + 0x1826AD0, player);
    hs.w.mem.U32(player + 0x460, 0x40000);
    hs.w.mem.U32(player + 0x468, 0x4F);
    hs.w.mem.Put(slot + 0x531C, "F102_1\0\0", 8);
    hs.w.Alpha(hs.w.pane["C/fi"], 0); // the native hides Fi in the water
    api->sample(inst, &h);
    api->sample(inst, &h);
    assert(i.at("sshd.dp.swim") == 1 && i.at("sshd.dp.valid") == 1 && i.at("sshd.dp.fi") == 0);
    assert(i.at("sshd.dp.eye") == 12 && i.at("sshd.dp.help") == 15);
    hs.w.mem.U32(player + 0x468, 0x56); // swimming Look: still the surface
    api->sample(inst, &h);
    assert(i.at("sshd.dp.swim") == 1 && i.at("sshd.dp.help") == 15);
    hs.w.mem.U32(player + 0x468, 0x4E); // scale-on water state: not verified -> hidden
    api->sample(inst, &h);
    assert(i.at("sshd.dp.swim") == 2 && i.at("sshd.dp.valid") == 0);
    for (const char* k : {"sshd.dp.base_c", "sshd.dp.eye", "sshd.dp.help", "sshd.dp.target"})
        assert(i.at(k) == 0);
    hs.w.mem.U32(player + 0x468, 0x4F);
    hs.w.mem.Put(slot + 0x531C, "F000\0\0\0\0", 8); // Skyloft's lake: not verified -> hidden
    api->sample(inst, &h);
    assert(i.at("sshd.dp.swim") == 2 && i.at("sshd.dp.help") == 0);
    hs.w.mem.U32(player + 0x460, 0); // out of the water: the mirror again (after its debounce)
    hs.w.Alpha(hs.w.pane["C/fi"], 255);
    api->sample(inst, &h);
    api->sample(inst, &h);
    assert(i.at("sshd.dp.swim") == 0 && i.at("sshd.dp.fi") == 15);
    // --- dev.33 stamina gauge (reader analysis): red palette and exhaustion separately
    const uint64_t g = hs.w.m + 0x1BD0;
    assert(i.at("sshd.stam.valid") == 0 && i.at("sshd.stam.red") == 0); // no gauge object: green
    hs.w.mem.U64(g, kMain + 0x165F8F8);
    hs.w.mem.U64(g + 0xD0, kMain + 0x165F858); // Recovery
    hs.w.mem.F32(g + 0x910, 2.0f);
    api->sample(inst, &h);
    assert(i.at("sshd.stam.valid") == 1 && i.at("sshd.stam.red") == 1 && i.at("sshd.stam.exh") == 1);
    hs.w.mem.U64(g + 0xD0, kMain + 0x165F8A0); // ToMax: the red lingers, no longer exhausted
    api->sample(inst, &h);
    assert(i.at("sshd.stam.red") == 1 && i.at("sshd.stam.exh") == 0);
    hs.w.mem.U64(g + 0xD0, kMain + 0x165F6F0); // Full, palette 0
    hs.w.mem.F32(g + 0x910, 0.0f);
    api->sample(inst, &h);
    assert(i.at("sshd.stam.red") == 0 && i.at("sshd.stam.exh") == 0);
    hs.w.mem.U64(g + 0xD0, kMain + 0x165F810); // ToMin: the depletion transition is exhausted
    hs.w.mem.F32(g + 0x910, 1.0f);             // the air palette is not the red land one
    api->sample(inst, &h);
    assert(i.at("sshd.stam.red") == 0 && i.at("sshd.stam.exh") == 1);
    hs.w.mem.U64(g, kMain + 0x165F000); // another object: nothing
    api->sample(inst, &h);
    assert(i.at("sshd.stam.valid") == 0 && i.at("sshd.stam.red") == 0 && i.at("sshd.stam.exh") == 0);
    // --- dev.33 sword (reader analysis): item flag 10 = the Practice Sword (stage 0)
    assert(i.at("sshd.sword.has") == 0 && i.at("sshd.sword.stage") == 6);
    hs.w.Item(10, true);
    api->sample(inst, &h);
    assert(i.at("sshd.sword.has") == 1 && i.at("sshd.sword.stage") == 0);
    assert(hs.texts.at("sshd.sword.src") == "module:sshd:lyt/MenuPause/hm_swordA_00");
    hs.w.Item(11, true);
    api->sample(inst, &h);
    assert(i.at("sshd.sword.stage") == 1 && hs.texts.at("sshd.sword.src") == "module:sshd:lyt/MenuPause/hm_swordB_00");
    hs.w.Flags(hs.w.pane["HUD"], 0); // map: everything zero, no target key
    api->sample(inst, &h);
    for (const char* k : {"sshd.dp.base_c", "sshd.dp.fi", "sshd.dp.help", "sshd.dp.dowse", "sshd.dp.target"})
        assert(i.at(k) == 0);
    assert(hs.texts.at("sshd.dp.tsrc").empty());
    api->destroy(inst);
}

// --- dev.33 sword reader: the native model selector's order over the active file (A / B)
void SwordTests() {
    using namespace sshd;
    World w;
    const DpadRead rd = [&w](uint64_t a, void* o, size_t n) { return w.mem.Read(a, o, n); };
    auto s = ReadSword(rd, kMain);
    assert(s.valid && s.stage == 6 && !s.Has() && !s.practice_owned);
    w.Item(10, true);
    s = ReadSword(rd, kMain);
    assert(s.Has() && s.stage == 0 && s.practice_owned);
    // the native priority 14, 13, 9, 12, 11, 10 -> stages 5, 4, 3, 2, 1, 0
    constexpr int kOrder[5][2] = {{11, 1}, {12, 2}, {9, 3}, {13, 4}, {14, 5}};
    for (const auto& o : kOrder) {
        w.Item(o[0], true);
        s = ReadSword(rd, kMain);
        assert(s.stage == o[1] && s.practice_owned); // flag 10 may stay set after upgrades
    }
    // file B (registry value 0, FM+0xCD06 == 0): its own words decide
    w.mem.U16(0x89100000ull + 0x10, 0);
    for (int i = 0; i < 0x40; ++i) w.mem.U8(w.fm + 0x53D0 + 0x9E4 + i, 0);
    s = ReadSword(rd, kMain);
    assert(s.valid && s.stage == 6);
    w.mem.U8(w.fm + 0xCD06, 1); // the selector byte set: file A again
    s = ReadSword(rd, kMain);
    assert(s.stage == 5);
    // an unreadable item table: not valid, no sword
    w.mem.U64(kMain + 0x182E168, 0);
    s = ReadSword(rd, kMain);
    assert(!s.valid && !s.Has());

    // dev.42: the registry walk is bounded at 64 nodes. A finite registry with no
    // deciding object still means file A; one that is still going at the cap is unknown: refuse
    World w2;
    const DpadRead rd2 = [&w2](uint64_t a, void* o, size_t n) { return w2.mem.Read(a, o, n); };
    w2.Item(10, true);
    const auto chain = [&w2](int count) {
        const uint64_t base = 0x8B000000ull;
        w2.mem.U64(kMain + 0x1688510, base);
        for (int k = 0; k < count; ++k) {
            const uint64_t node = base + 0x100ull * k, obj = node + 0x80;
            w2.mem.U64(node, k == 0 ? 0 : node - 0x100);           // parent
            w2.mem.U64(node + 8, k + 1 < count ? node + 0x100 : 0); // first child
            w2.mem.U64(node + 0x18, 0);                             // no sibling
            w2.mem.U64(node + 0x20, obj);
            w2.mem.U8(obj + 0x16, 2); // not the deciding type
            w2.mem.U16(obj + 0x10, 0);
        }
    };
    chain(10);
    s = ReadSword(rd2, kMain);
    assert(s.valid && s.stage == 0);
    chain(63);
    s = ReadSword(rd2, kMain);
    assert(s.valid && s.stage == 0);
    chain(64); // the 64th node has no child, so the walk ends exactly at the cap: file A
    s = ReadSword(rd2, kMain);
    assert(s.valid && s.stage == 0);
    chain(80); // still going at the cap: no file, no sword
    s = ReadSword(rd2, kMain);
    assert(!s.valid && !s.Has());
}

} // namespace

int main() {
    ReaderTests();
    SwordTests();
    ModuleTest();
    return 0;
}
