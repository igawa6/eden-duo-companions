// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Skyward Sword HD top-screen HUD hiding (see sshd_hudhide.h). Addresses are main-relative for
// 1.0.1 (main build 5441AF3C89F97E1D25A8CCF14C742BF82336D8A8) only; the module refuses every other
// build before this runs. Pane paths: DoButton.arc layouts remoConBtn_03 (Cross), nunBtn_03
// (Dowsing), remoConBtn_05 (Help), basicPosition_00 (the meter's own layout) and rupy_01 (rupees).

#include "sshd_hudhide.h"

#include <cstring>
#include <utility>

namespace sshd {
namespace {

constexpr uint64_t kMeterPtr = 0x183C9A0;  // M = *(main+0x183C9A0)
constexpr uint64_t kCrossOff = 0xC928, kDowseOff = 0x15CF8, kHelpPtrOff = 0x20D08;
constexpr uint64_t kCrossVt = 0x165D398, kDowseVt = 0x165DFF0, kHelpVt = 0x165BE50;
constexpr int kMaxChildren = 500, kMaxDepth = 16;

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

// The one direct child of `parent` named `name` (intrusive list: sentinel parent+0x28, each node
// points to child+8), 0 when there is none or more than one. *ok = false on an unreadable list.
uint64_t Child(const DpadRead& rd, uint64_t parent, const char* name, bool* ok) {
    if (parent == 0)
        return 0;
    uint64_t node = 0, found = 0;
    if (!Rd(rd, parent + 0x28, &node)) {
        *ok = false;
        return 0;
    }
    int n = 0;
    for (; node != parent + 0x28 && node != 0; ++n) {
        if (n == kMaxChildren) {
            *ok = false;
            return 0;
        }
        const uint64_t child = node - 8;
        if (NameIs(rd, child, name)) {
            if (found != 0)
                return 0; // ambiguous: never hidden
            found = child;
        }
        if (!Rd(rd, node, &node)) {
            *ok = false;
            return 0;
        }
    }
    return found;
}

HideEntry Entry(const DpadRead& rd, uint64_t pane, HideGroup g) {
    HideEntry e;
    uint32_t nm[2]{};
    if (pane == 0 || !rd(pane + 0xEC, nm, 8))
        return e;
    e.pane = pane;
    e.name_lo = nm[0];
    e.name_hi = nm[1];
    e.group = g;
    return e;
}

} // namespace

void HideResolver::Reset() {
    cached_ = false;
    std::memset(key_, 0, sizeof(key_));
    entries_.clear();
}

bool HideResolver::Resolve(const DpadRead& rd, uint64_t c_root, uint64_t d_root, uint64_t h_root,
                           uint64_t c_owner) {
    entries_.clear();
    bool ok = true;
    // --- D-pad: every pane must resolve, else the whole group stays native (no half cluster)
    std::vector<HideEntry> dp;
    const uint64_t c_all = Child(rd, c_root, "N_all_00", &ok);
    const uint64_t c_key = Child(rd, c_all, "N_key_00", &ok);
    // base + Down / Left / Right arms; the Up arm N_keyT_00 and the N_iconT_00 / N_textT_00 harp /
    // X branch are kept (analysis: preserve the top / X / harp content sharing the cross owner)
    for (const char* n : {"N_bace_00", "N_keyD_00", "N_keyL_00", "N_keyR_00"})
        dp.push_back(Entry(rd, Child(rd, c_key, n, &ok), kHideDpad));
    for (const char* n : {"N_iconD_00", "N_textD_00", "N_textL_00", "N_textR_00"})
        dp.push_back(Entry(rd, Child(rd, c_all, n, &ok), kHideDpad));
    // dowsing prompt (eye / Look / Dowse / Back, its cross) and the selected-target set; the wheel
    // N_itemSelect_00, N_cancel_00, N_resetGyro_00 and N_itemArrow_00 are siblings and stay drawn
    const uint64_t d_all = Child(rd, d_root, "N_itemAll_00", &ok);
    for (const char* n : {"N_showSimple_00", "N_showSet_00"})
        dp.push_back(Entry(rd, Child(rd, d_all, n, &ok), kHideDpad));
    if (!ok)
        return false;
    bool complete = true;
    for (const auto& e : dp)
        complete = complete && e.pane != 0;
    // Help / Close prompt: the Help layout's own N_all_00 (glyph, caption, call layers), not the
    // broad owner pane in the meter layout. dev.37: optional. The Help widget comes and goes (title,
    // around loads); without it the cross and dowsing panes stay hidden (dev.29 let the whole
    // cluster draw then).
    if (h_root != 0) {
        const HideEntry help = Entry(rd, Child(rd, h_root, "N_all_00", &ok), kHideDpad);
        if (!ok)
            return false;
        complete = complete && help.pane != 0;
        dp.push_back(help);
    }
    if (complete)
        entries_.insert(entries_.end(), dp.begin(), dp.end());
    // --- rupees: the meter layout's N_rupyAll_00 (RootPane > N_rupyAll_00 > N_rupy_00) and, when it
    // is attached there, the rupy_01 layout's own N_rupyAll_00 (N_rupy_00 > RootPane > N_rupyAll_00)
    uint64_t q = c_owner, top = 0;
    for (int depth = 0; q != 0 && depth < kMaxDepth; ++depth) {
        uint64_t parent = 0;
        if (!Rd(rd, q + 0x18, &parent))
            return false;
        if (parent == 0)
            top = q;
        q = parent;
    }
    if (top != 0 && NameIs(rd, top, "RootPane")) {
        const uint64_t m_rupy_all = Child(rd, top, "N_rupyAll_00", &ok);
        const uint64_t m_rupy = Child(rd, m_rupy_all, "N_rupy_00", &ok);
        const uint64_t r_root = Child(rd, m_rupy, "RootPane", &ok);
        const uint64_t r_all = Child(rd, r_root, "N_rupyAll_00", &ok);
        if (!ok)
            return false;
        if (m_rupy_all != 0 && m_rupy != 0) {
            entries_.push_back(Entry(rd, m_rupy_all, kHideRupee));
            if (r_all != 0)
                entries_.push_back(Entry(rd, r_all, kHideRupee));
        }
    }
    return true;
}

HideTargets HideResolver::Sample(const DpadRead& rd, uint64_t main) {
    HideTargets out;
    auto fail = [&] {
        Reset();
        return HideTargets{};
    };
    uint64_t m = 0, vc = 0, vd = 0, h = 0, vh = 0, c_root = 0, d_root = 0, h_root = 0, owner = 0, c_owner = 0;
    if (!Rd(rd, main + kMeterPtr, &m) || m == 0)
        return fail();
    const uint64_t c = m + kCrossOff, d = m + kDowseOff;
    if (!Rd(rd, c, &vc) || vc != main + kCrossVt || !Rd(rd, d, &vd) || vd != main + kDowseVt ||
        !Rd(rd, m + kHelpPtrOff, &h))
        return fail();
    if (h != 0 && (!Rd(rd, h, &vh) || vh != main + kHelpVt))
        return fail();
    if (!Rd(rd, c + 0xC0, &c_root) || c_root == 0 || !Rd(rd, d + 0x700, &d_root) || d_root == 0 ||
        (h != 0 && (!Rd(rd, h + 0xC0, &h_root) || h_root == 0)))
        return fail();
    if (!Rd(rd, c + 0x6A0, &c_owner) || !NameIs(rd, c_owner, "N_remoConBtn_03") || !Rd(rd, d + 0x1678, &owner) ||
        !NameIs(rd, owner, "N_nunBtn_03"))
        return fail();
    if (h != 0 && (!Rd(rd, h + 0x2B8, &owner) || !NameIs(rd, owner, "N_remoConBtn_05")))
        return fail();
    const uint64_t key[6] = {main, m, c_root, d_root, h, h_root};
    bool fresh = false;
    if (!cached_ || std::memcmp(key, key_, sizeof(key)) != 0) {
        ++resolves_;
        if (!Resolve(rd, c_root, d_root, h_root, c_owner))
            return fail();
        std::memcpy(key_, key, sizeof(key));
        cached_ = true;
        fresh = true;
    }
    // every cached pane still carries its name (else the allocation was reused: look again once)
    for (int pass = 0;; ++pass) {
        bool same = true;
        for (const auto& e : entries_) {
            uint32_t nm[2]{};
            if (!rd(e.pane + 0xEC, nm, 8) || nm[0] != e.name_lo || nm[1] != e.name_hi) {
                same = false;
                break;
            }
        }
        if (same)
            break;
        ++resolves_;
        if (pass > 0 || fresh || !Resolve(rd, c_root, d_root, h_root, c_owner))
            return fail();
        fresh = true;
    }
    // coherent sample: the identity must not have changed while the panes were read
    uint64_t m2 = 0, r0 = 0, r1 = 0, h2 = 0;
    if (!Rd(rd, main + kMeterPtr, &m2) || m2 != m || !Rd(rd, c + 0xC0, &r0) || r0 != c_root ||
        !Rd(rd, d + 0x700, &r1) || r1 != d_root || !Rd(rd, m + kHelpPtrOff, &h2) || h2 != h)
        return fail();
    out.valid = true;
    out.help = h != 0;
    out.entries = entries_;
    return out;
}

HideStep HudHide::Step(bool show_dpad, bool show_rupee, bool helper_ok, const HideTargets& t, bool legacy, bool menu) {
    const auto wanted = [&](const HideEntry& e) {
        return (e.group == kHideDpad && !show_dpad) || (e.group == kHideRupee && !show_rupee);
    };
    std::vector<HideEntry> want;
    if (!legacy && (!helper_ok || !t.valid)) {
        // hold (see the class comment): the applied list stays without a refresh, minus a group a
        // setting has shown again meanwhile
        if (!active_)
            return {};
        for (const auto& e : applied_)
            if (wanted(e))
                want.push_back(e);
        if (want == applied_)
            return {};
    } else if (helper_ok && t.valid && (!legacy || (t.help && !menu))) {
        for (const auto& e : t.entries) {
            if (wanted(e))
                want.push_back(e);
            if (static_cast<int>(want.size()) == kHideMax)
                break;
        }
    }
    HideStep s;
    if (want.empty()) {
        if (active_)
            s.kind = HideStep::kClear;
        Reset();
        return s;
    }
    if (active_ && want == applied_) {
        s.kind = HideStep::kRefresh;
        return s;
    }
    s.kind = HideStep::kWrite;
    s.list = want;
    applied_ = std::move(want);
    active_ = true;
    return s;
}

} // namespace sshd
