// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Skyward Sword HD native D-pad cluster reader (see sshd_dpad.h). Addresses are main-relative
// for 1.0.1 (main build 5441AF3C89F97E1D25A8CCF14C742BF82336D8A8) only; the module refuses every
// other build before this runs. Contract: D-pad reader analysis
// (pointer contract, V(p), selectors, A/B active file, slot availability and target resolvers).

#include "sshd_dpad.h"
#include "sshd_story.h"

#include <cmath>
#include <cstring>
#include <string_view>
#include <vector>

namespace sshd {
namespace {

constexpr uint64_t kMeterPtr = 0x183C9A0;   // M = *(main+0x183C9A0)
constexpr uint64_t kCrossOff = 0xC928;      // C = M+0xC928
constexpr uint64_t kDowseOff = 0x15CF8;     // D = M+0x15CF8
constexpr uint64_t kHelpPtrOff = 0x20D08;   // H = *(M+0x20D08), null on title / menus
constexpr uint64_t kCrossVt = 0x165D398, kDowseVt = 0x165DFF0, kHelpVt = 0x165BE50;
constexpr uint64_t kHelpOpenOff = 0x218B0;  // u8(M+0x218B0) != 0: Help is open -> "Close"
constexpr uint64_t kFileMgr = 0x2BF9CC0, kRegistry = 0x1688510;
constexpr size_t kActiveFileWalkMax = 64; // registry nodes ActiveFile visits before it gives up
constexpr uint64_t kStoryMgr = 0x182E1F0, kItemMgr = 0x182E168;

constexpr int kMaxPanes = 600, kMaxChildren = 500, kMaxDepth = 64;

enum Widget { kC, kD, kH };
struct Spec {
    Widget w;
    const char* name;
    const char* ancestor; // a pane of this name must be on the parent chain (nullptr = any)
};
constexpr Spec kSpecs[kDpLayerCount] = {
    {kC, "P_crossBtn_00", nullptr},     {kD, "P_crossBtn_00", nullptr},
    {kH, "P_crossBtn_00", nullptr},     {kD, "P_btn_01", nullptr},
    {kC, "P_crossBtn_02", "N_keyD_00"}, {kC, "P_crossBtn_04", "N_keyL_00"},
    {kH, "P_btn_02", nullptr},          {kD, "P_btn_00", "N_lookBtns_00"},
    {kC, "P_plusIcon_01", "N_swordIcon_00"}, {kC, "P_plusIconS_00", "N_swordIcon_00"},
    {kC, "P_plusIconS_01", "N_swordIcon_00"}, {kC, "P_plusIcon_00", "N_swordIcon_00"},
    {kC, "P_plusIcon_02", "N_swordIcon_00"}, {kC, "P_bairdIcon_00", "N_birdIcon_00"},
    {kD, "T_dawsing_00", nullptr},      {kH, "T_helpBtn_00", nullptr},
    {kD, "P_comIcon_00", "N_iconSet_00"},
};

// Logical dowsing icon id -> art (contract "Art indexed by logical ID 0..21").
constexpr const char* kTargetArt[22] = {
    "tr_dauzTarget_00", "tr_dauzTarget_01", "tr_dauzTarget_02", "uk_dawGenki_00",   "tr_dauzTarget_04",
    "uc_bottleHoly_00", "tr_dauzTarget_06", "tr_dauzTarget_07", "tr_dauzTarget_08", "tr_cameraMark_00",
    "tr_dauzTarget_10", "ts_keyKakera_00",  "ts_dauzPump_00",   "tr_dauzTarget_13", "ts_waterPot_00",
    "tr_dauzTarget_15", "ts_GdsCube_00",    "tr_dauzTarget_17", "tr_dauzTarget_18", "tr_dauzTarget_19",
    "tr_dauzTarget_20", "tr_dauzTarget_21"};

template <typename T> bool Rd(const DpadRead& rd, uint64_t a, T* v) {
    return a != 0 && rd(a, v, sizeof(T));
}
bool Name(const DpadRead& rd, uint64_t pane, char (&out)[25]) {
    std::memset(out, 0, sizeof(out));
    return pane != 0 && rd(pane + 0xEC, out, 24);
}
bool NameIs(const DpadRead& rd, uint64_t pane, const char* want) {
    char n[25];
    return Name(rd, pane, n) && std::strcmp(n, want) == 0;
}

// V(p): every pane from p up the parent chain to the HUD root draws (EB bit 0), p's effective
// alpha E9 is non-zero and a picture has its material. Returns E9, 0 = not rendered, -1 = the
// chain is unreadable or cyclic (the whole sample is then rejected).
int Rendered(const DpadRead& rd, uint64_t p, bool picture) {
    uint64_t seen[kMaxDepth];
    int n = 0;
    uint8_t leaf_alpha = 0;
    for (uint64_t q = p; q != 0;) {
        for (int i = 0; i < n; ++i)
            if (seen[i] == q)
                return -1;
        if (n == kMaxDepth)
            return -1;
        seen[n++] = q;
        uint8_t ab[4];
        uint64_t parent = 0;
        if (!rd(q + 0xE8, ab, 4) || !Rd(rd, q + 0x18, &parent))
            return -1;
        if (q == p)
            leaf_alpha = ab[1];
        if ((ab[3] & 1) == 0)
            return 0;
        q = parent;
    }
    if (leaf_alpha == 0)
        return 0;
    if (picture) {
        uint64_t mat = 0;
        if (!Rd(rd, p + 0x50, &mat))
            return -1;
        if (mat == 0)
            return 0;
    }
    return leaf_alpha;
}

// Active-file flag value (contract "Availability"): metadata table of the manager gives the word
// index and packed shift/width for the flag; the value comes from the active file's flag words.
struct Flags {
    const DpadRead* rd;
    uint64_t story_table = 0, item_table = 0, story_data = 0, item_data = 0;
    bool ok = true;
    int Get(uint64_t table, uint64_t data, int f) {
        uint8_t meta[2];
        uint16_t word = 0;
        if (!(*rd)(table + 2u * static_cast<uint64_t>(f), meta, 2) || !Rd(*rd, data + 2u * meta[0], &word)) {
            ok = false;
            return 0;
        }
        const int shift = meta[1] >> 4, width = meta[1] & 15;
        return (word >> shift) & ((2 << width) - 1);
    }
    bool S(int f) { return Get(story_table, story_data, f) != 0; }
    bool I(int f) { return Get(item_table, item_data, f) != 0; }
};
bool Table(const DpadRead& rd, uint64_t main, uint64_t mgr_off, uint64_t* table) {
    uint64_t mgr = 0, meta = 0;
    return Rd(rd, main + mgr_off, &mgr) && mgr != 0 && Rd(rd, mgr + 0x18, &meta) && meta != 0 &&
           Rd(rd, meta + 8, table) && *table != 0;
}

bool TrialPending(Flags& f) {
    constexpr int kTrial[4][2] = {{187, 0x5D}, {188, 0x61}, {189, 0x62}, {193, 0x63}};
    for (const auto& t : kTrial)
        if (f.I(t[0]) && !f.S(t[1]))
            return true;
    return false;
}
bool Available(Flags& f, int slot) {
    switch (slot) {
    case 0:
        return f.S(0x64) || TrialPending(f) || (f.S(0x40) && !f.S(0x41)) || (f.S(0x10F) && !f.S(0x0E)) ||
               (f.S(0x51) && !f.S(0x12)) || f.S(0x29C) || f.S(0x29D);
    case 1: return f.S(0x69);
    case 2:
        return f.S(0x29E) || f.S(0x2A8) || f.S(0x2A9) || f.S(0x2AA) || (f.S(0x38) && !f.S(0x39)) ||
               (f.S(0x6A) && !f.S(0x78)) || (f.S(0x6B) && !f.S(0x08));
    case 3: return f.S(0x68);
    case 4: return f.S(0x6C);
    case 5: return f.S(0x6E);
    case 6: return true;
    case 7: return f.S(0x66);
    default: return false;
    }
}
// Ordered native resolvers (story 0xB69DF4, quest 0xB69F48); static slots map directly.
int SlotLogical(Flags& f, int slot) {
    if (slot == 0) {
        if (f.S(0x64)) return 19;
        if (TrialPending(f)) return 20;
        if (f.S(0x40) && !f.S(0x41)) return 5;
        if (f.S(0x10F) && !f.S(0x0E)) return 18;
        if (f.S(0x51) && !f.S(0x12)) return 17;
        return f.S(0x29C) ? 10 : 14;
    }
    if (slot == 2) {
        constexpr int kOne[4][2] = {{0x29E, 15}, {0x2A8, 12}, {0x2A9, 21}, {0x2AA, 13}};
        for (const auto& k : kOne)
            if (f.S(k[0])) return k[1];
        constexpr int kTwo[3][3] = {{0x38, 0x39, 8}, {0x6A, 0x78, 11}, {0x6B, 0x08, 7}};
        for (const auto& k : kTwo)
            if (f.S(k[0]) && !f.S(k[1])) return k[2];
        return 15;
    }
    constexpr int kStatic[8] = {-1, 4, -1, 3, 1, 16, 9, 6};
    return slot >= 0 && slot < 8 ? kStatic[slot] : -1;
}

// The file the game reads (the A/B choice): walk the bounded registry (kActiveFileWalkMax nodes,
// ancestor climbs capped by sshd::NextTreeNode); the first object with type byte 1 decides; B only
// if it exists with value 0 and FM+0xCD06 is 0. No decision within the cap = unknown (false).
bool ActiveFile(const DpadRead& rd, uint64_t main, uint64_t* active) {
    uint64_t fm = 0, node = 0;
    if (!Rd(rd, main + kFileMgr, &fm) || fm == 0 || !Rd(rd, main + kRegistry, &node))
        return false;
    bool use_b = false, decided = false;
    std::vector<uint64_t> seen;
    while (node != 0 && seen.size() < kActiveFileWalkMax) {
        for (const uint64_t s : seen)
            if (s == node)
                return false;
        seen.push_back(node);
        uint64_t obj = 0, next = 0;
        uint8_t type = 0;
        uint16_t value = 0;
        if (!Rd(rd, node + 0x20, &obj) || !Rd(rd, obj + 0x16, &type) || !Rd(rd, obj + 0x10, &value))
            return false;
        if (type == 1) {
            uint8_t sel = 1;
            if (!Rd(rd, fm + 0xCD06, &sel))
                return false;
            use_b = value == 0 && sel == 0;
            decided = true;
            break;
        }
        uint64_t child = 0;
        if (!Rd(rd, node + 8, &child))
            return false;
        // dev.41: the shared depth-first step, its ancestor climb capped (a parent
        // chain that loops while the game changes the tree ends the walk: unknown, fail closed)
        if (!NextTreeNode(rd, node, child, &next))
            return false;
        node = next;
    }
    // dev.42: the walk hit its node cap before any object decided: the file is
    // unknown, so refuse instead of assuming file A (the end of the tree still means file A)
    if (!decided && node != 0)
        return false;
    *active = fm + (use_b ? 0x53D0 : 0x10);
    return true;
}

// The selected dowsing target, or -1: an available slot whose resolved logical id agrees with the
// icon object's logical byte and with both animation frames.
int SelectedTarget(const DpadRead& rd, uint64_t main, uint64_t d, int* slot_out) {
    uint64_t active = 0;
    uint8_t slot = 8, logical = 0xFF, type = 0;
    if (!ActiveFile(rd, main, &active) || !Rd(rd, active + 0x53B2, &slot) || slot > 7)
        return -1;
    *slot_out = slot;
    Flags f{&rd};
    if (!Table(rd, main, kStoryMgr, &f.story_table) || !Table(rd, main, kItemMgr, &f.item_table))
        return -1;
    f.story_data = active + 0x8E4;
    f.item_data = active + 0x9E4;
    const bool avail = Available(f, slot);
    const int want = SlotLogical(f, slot);
    if (!f.ok || !avail || want < 0)
        return -1;
    const uint64_t icon = d + 0x1700;
    uint64_t ctl = 0, xf = 0;
    float frame = -1, applied = -1;
    if (!Rd(rd, icon + 0x80, &type) || type != 2 || !Rd(rd, icon + 0xD60, &logical) ||
        !Rd(rd, icon + 0xCA8, &ctl) || !Rd(rd, ctl + 0x0C, &frame) || !Rd(rd, icon + 0xCE0, &xf) ||
        !Rd(rd, xf + 0x20, &applied))
        return -1;
    if (logical != want || logical >= 22 || std::fabs(frame - logical) > 0.01f ||
        std::fabs(applied - logical) > 0.01f)
        return -1;
    return logical;
}

} // namespace

SwordState ReadSword(const DpadRead& rd, uint64_t main) {
    SwordState out;
    uint64_t active = 0;
    Flags f{&rd};
    if (!ActiveFile(rd, main, &active) || !Table(rd, main, kItemMgr, &f.item_table))
        return out;
    f.item_data = active + 0x9E4;
    // native priority: True Master 14, Master 13, Goddess White 9, Goddess Longsword 12, Goddess 11,
    // Practice 10 -> model stages 5, 4, 3, 2, 1, 0
    constexpr int kOrder[6][2] = {{14, 5}, {13, 4}, {9, 3}, {12, 2}, {11, 1}, {10, 0}};
    int stage = 6;
    for (const auto& o : kOrder)
        if (f.I(o[0]) && stage == 6)
            stage = o[1];
    out.practice_owned = f.I(10);
    if (!f.ok)
        return out;
    out.valid = true;
    out.stage = stage;
    return out;
}

const char* DowsingTargetArt(int logical) {
    return logical >= 0 && logical < 22 ? kTargetArt[logical] : nullptr;
}

void DpadReader::Reset() {
    cached_ = false;
    std::memset(key_, 0, sizeof(key_));
    leaves_ = {};
    prev_ = {};
}

bool DpadReader::Resolve(const DpadRead& rd, uint64_t main, uint64_t m, const uint64_t roots[3]) {
    leaves_ = {};
    for (int w = 0; w < 3; ++w) {
        if (roots[w] == 0)
            continue;
        int count[kDpLayerCount]{};
        std::vector<uint64_t> queue{roots[w]}, seen{roots[w]};
        for (size_t qi = 0; qi < queue.size(); ++qi) {
            const uint64_t p = queue[qi];
            char name[25];
            if (!Name(rd, p, name))
                return false;
            for (int l = 0; l < kDpLayerCount; ++l) {
                if (kSpecs[l].w != w || std::strcmp(name, kSpecs[l].name) != 0)
                    continue;
                bool anc = kSpecs[l].ancestor == nullptr;
                uint64_t q = 0;
                if (!Rd(rd, p + 0x18, &q))
                    return false;
                for (int depth = 0; !anc && q != 0 && depth < kMaxDepth; ++depth) {
                    anc = NameIs(rd, q, kSpecs[l].ancestor);
                    if (!Rd(rd, q + 0x18, &q))
                        return false;
                }
                if (anc) {
                    ++count[l];
                    leaves_[l].addr = p;
                }
            }
            // children: intrusive list, sentinel p+0x28, each node points to child+8
            uint64_t node = 0;
            if (!Rd(rd, p + 0x28, &node))
                return false;
            for (int c = 0; node != p + 0x28 && node != 0 && c < kMaxChildren; ++c) {
                const uint64_t child = node - 8;
                bool dup = false;
                for (const uint64_t s : seen)
                    if (s == child) { dup = true; break; }
                if (!dup) {
                    if (static_cast<int>(seen.size()) >= kMaxPanes)
                        return false;
                    seen.push_back(child);
                    queue.push_back(child);
                }
                if (!Rd(rd, node, &node))
                    return false;
            }
        }
        for (int l = 0; l < kDpLayerCount; ++l)
            if (kSpecs[l].w == w)
                leaves_[l].found = count[l] == 1; // an ambiguous name is never drawn
    }
    key_[0] = main;
    key_[1] = m;
    key_[2] = roots[0];
    key_[3] = roots[1];
    key_[4] = roots[2];
    cached_ = true;
    return true;
}

DpadState DpadReader::Sample(const DpadRead& rd, uint64_t main) {
    DpadState out;
    auto fail = [&] {
        Reset();
        return DpadState{};
    };
    uint64_t m = 0, vc = 0, vd = 0, h = 0, vh = 0, roots[3]{}, owner = 0;
    if (!Rd(rd, main + kMeterPtr, &m) || m == 0)
        return fail();
    const uint64_t c = m + kCrossOff, d = m + kDowseOff;
    if (!Rd(rd, c, &vc) || vc != main + kCrossVt || !Rd(rd, d, &vd) || vd != main + kDowseVt ||
        !Rd(rd, m + kHelpPtrOff, &h))
        return fail();
    if (h != 0 && (!Rd(rd, h, &vh) || vh != main + kHelpVt))
        return fail();
    if (!Rd(rd, c + 0xC0, &roots[0]) || roots[0] == 0 || !Rd(rd, d + 0x700, &roots[1]) || roots[1] == 0 ||
        (h != 0 && (!Rd(rd, h + 0xC0, &roots[2]) || roots[2] == 0)))
        return fail();
    // owner panes as the contract names them
    if (!Rd(rd, c + 0x6A0, &owner) || !NameIs(rd, owner, "N_remoConBtn_03") || !Rd(rd, d + 0x1678, &owner) ||
        !NameIs(rd, owner, "N_nunBtn_03"))
        return fail();
    if (h != 0 && (!Rd(rd, h + 0x2B8, &owner) || !NameIs(rd, owner, "N_remoConBtn_05")))
        return fail();
    if (!cached_ || key_[0] != main || key_[1] != m || key_[2] != roots[0] || key_[3] != roots[1] ||
        key_[4] != roots[2]) {
        prev_ = {};
        if (!Resolve(rd, main, m, roots))
            return fail();
    }
    std::array<uint8_t, kDpLayerCount> raw{};
    for (int l = 0; l < kDpLayerCount; ++l) {
        const auto& lf = leaves_[l];
        if (!lf.found || (kSpecs[l].w == kH && h == 0))
            continue;
        if (!NameIs(rd, lf.addr, kSpecs[l].name)) // a reused allocation: look the panes up again
            return fail();
        const int a = Rendered(rd, lf.addr, kSpecs[l].name[0] == 'P');
        if (a < 0)
            return fail();
        raw[l] = static_cast<uint8_t>(a);
    }
    // selectors read with the panes, consumed only where the pane itself renders
    uint32_t msg = 0;
    uint8_t help_open = 0, fi_cur = 0, fi_prev = 0;
    if (!Rd(rd, d + 0x9100, &msg) || !Rd(rd, m + kHelpOpenOff, &help_open) || !Rd(rd, c + 0xB5F, &fi_cur) ||
        !Rd(rd, c + 0xB60, &fi_prev))
        return fail();
    out.up_text = msg == 0x51 ? 1 : msg == 0x52 ? 2 : msg == 0x6C ? 3 : 0;
    if (out.up_text == 0)
        raw[kDpUpText] = 0;
    out.help_close = help_open != 0;
    out.fi_attention = raw[kDpFi] != 0 && (fi_cur != 0 || fi_prev != 0);
    if (!out.fi_attention)
        raw[kDpFiHalo0] = raw[kDpFiHalo1] = 0;
    if (raw[kDpTarget] != 0) {
        out.target_logical = SelectedTarget(rd, main, d, &out.target_slot);
        if (out.target_logical < 0)
            raw[kDpTarget] = 0;
    }
    // coherent sample: the identity must not have changed while the layers were read
    uint64_t m2 = 0, r0 = 0, r1 = 0, h2 = 0;
    if (!Rd(rd, main + kMeterPtr, &m2) || m2 != m || !Rd(rd, c + 0xC0, &r0) || r0 != roots[0] ||
        !Rd(rd, d + 0x700, &r1) || r1 != roots[1] || !Rd(rd, m + kHelpPtrOff, &h2) || h2 != h)
        return fail();
    out.valid = true;
    for (int l = 0; l < kDpLayerCount; ++l)
        out.alpha[l] = prev_[l] != 0 ? raw[l] : 0; // shown only after two rendered samples
    prev_ = raw;
    if (out.alpha[kDpTarget] == 0)
        out.target_logical = -1;
    return out;
}

} // namespace sshd
