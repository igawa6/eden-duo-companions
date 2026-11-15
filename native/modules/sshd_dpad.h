// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Skyward Sword HD (1.0.1) native D-pad cluster reader: a display-only mirror of the top-screen
// D-pad (Cross / Dowsing / Help HUD widgets), from the D-pad reader
// analysis. Every layer is the rendered state of one native leaf
// pane: V(p) = the pane resolved by name from a validated widget root, every pane from it up the
// parent chain to the HUD root drawing (EB bit 0), non-zero effective alpha (E9) and, for a
// picture, a material. Anything unsure (bad pointer, wrong vtable or owner name, a changed
// identity between the start and the end of a sample, a cycle, an ambiguous name) clears the
// whole output; a layer must also read visible in two consecutive samples before it is shown.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>

namespace sshd {

/// Reads guest memory at an absolute address; false when unmapped.
using DpadRead = std::function<bool(uint64_t addr, void* out, size_t n)>;

enum DpadLayer : int {
    kDpBaseC,      // Cross P_crossBtn_00 (dim base, tr_crossBtnBasic_00)
    kDpBaseD,      // Dowsing P_crossBtn_00
    kDpBaseH,      // Help P_crossBtn_00
    kDpUp,         // Dowsing P_btn_01 (lit Up, tr_crossBtnBasicT_03)
    kDpDown,       // Cross P_crossBtn_02 (lit Down, tr_crossBtnBasicD_00)
    kDpLeft,       // Cross P_crossBtn_04 (lit Left, tr_crossBtnBasicL_00)
    kDpRight,      // Help P_btn_02 (lit Right, tr_crossBtnBasicR_01)
    kDpEye,        // Dowsing N_lookBtns_00/P_btn_00 (tr_cameraMark_00)
    kDpFi,         // Cross N_swordIcon_00/P_plusIcon_01 (tr_phiIcon_00)
    kDpFiShadow0,  // P_plusIconS_00 (black shadow)
    kDpFiShadow1,  // P_plusIconS_01 (coloured outline)
    kDpFiHalo0,    // P_plusIcon_00 (attention halo), only with the attention latch
    kDpFiHalo1,    // P_plusIcon_02
    kDpBird,       // Cross N_birdIcon_00/P_bairdIcon_00 (th_skyBird_03)
    kDpUpText,     // Dowsing T_dawsing_00 with a recognised message (Look / Dowse / Back)
    kDpHelpText,   // Help T_helpBtn_00 (Help / Close)
    kDpTarget,     // Dowsing N_iconSet_00/../P_comIcon_00, the selected dowsing target
    kDpLayerCount,
};

struct DpadState {
    bool valid = false;                          // pointers / identities validated this sample
    std::array<uint8_t, kDpLayerCount> alpha{};  // rendered effective alpha 0..255, 0 = hidden
    int up_text = 0;      // 0 none, 1 Look, 2 Dowse, 3 Back
    bool help_close = false;
    bool fi_attention = false;
    int target_logical = -1; // logical dowsing icon id 0..21 when kDpTarget is shown
    int target_slot = -1;    // native slot 0..7 (diagnostic)
};

/// The equipped sword as the native model selector computes it (reader analysis
/// readers "Practice Sword", main+0xDDD23C..0xDDD2C8): item flags 14, 13, 9, 12, 11, 10 tested in
/// that order give model stage 5 (True Master Sword), 4 (Master), 3 (Goddess White), 2 (Goddess
/// Longsword), 1 (Goddess Sword), 0 (Practice Sword); none = 6 (no sword). Flags come from the item
/// manager's metadata over the active file (the A/B registry choice), never a fixed slot.
struct SwordState {
    bool valid = false;          // every flag read
    int stage = 6;               // native model stage 0..5, 6 = no sword
    bool practice_owned = false; // item flag 10 (may stay set after upgrades)
    bool Has() const { return valid && stage < 6; }
};
SwordState ReadSword(const DpadRead& rd, uint64_t main);

/// Art (DoButton texture member) for a logical dowsing icon id, nullptr when not drawable.
const char* DowsingTargetArt(int logical);

class DpadReader {
public:
    /// One sample. `main` is the main module base. Never throws.
    DpadState Sample(const DpadRead& rd, uint64_t main);
    /// Drop every cached pane and the previous sample (unload, reconfigure).
    void Reset();

private:
    struct Leaf {
        uint64_t addr = 0;
        bool found = false;
    };
    bool Resolve(const DpadRead& rd, uint64_t main, uint64_t m, const uint64_t roots[3]);
    uint64_t key_[6]{};
    bool cached_ = false;
    std::array<Leaf, kDpLayerCount> leaves_{};
    std::array<uint8_t, kDpLayerCount> prev_{};
};

} // namespace sshd
