// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Published key names of the Dragon Quest III HD-2D Remake companion that the module writes every
// sample, as literals (no formatting per sample). The unit test checks each table against the
// printf format it replaces. Internal header.
#pragma once

namespace dq3::keys {

/// dq3.p<i>.<field> of top card i (MemberKey order).
enum MemberKey : int {
    MOn, MName, MLv, MHpT, MMpT, MFcard, MHp, MHpMax, MMp, MMpMax, MSt0On, MSt0X, MSt0Src, MSt1On, MSt1X, MSt1Src, MemberKeyCount};
inline constexpr const char* Member[4][MemberKeyCount] = {
    {"dq3.p0.on", "dq3.p0.name", "dq3.p0.lv", "dq3.p0.hp_t", "dq3.p0.mp_t", "dq3.p0.fcard", "dq3.p0.hp", "dq3.p0.hp_max", "dq3.p0.mp", "dq3.p0.mp_max", "dq3.p0.st0.on", "dq3.p0.st0.x", "dq3.p0.st0.src", "dq3.p0.st1.on", "dq3.p0.st1.x", "dq3.p0.st1.src"},
    {"dq3.p1.on", "dq3.p1.name", "dq3.p1.lv", "dq3.p1.hp_t", "dq3.p1.mp_t", "dq3.p1.fcard", "dq3.p1.hp", "dq3.p1.hp_max", "dq3.p1.mp", "dq3.p1.mp_max", "dq3.p1.st0.on", "dq3.p1.st0.x", "dq3.p1.st0.src", "dq3.p1.st1.on", "dq3.p1.st1.x", "dq3.p1.st1.src"},
    {"dq3.p2.on", "dq3.p2.name", "dq3.p2.lv", "dq3.p2.hp_t", "dq3.p2.mp_t", "dq3.p2.fcard", "dq3.p2.hp", "dq3.p2.hp_max", "dq3.p2.mp", "dq3.p2.mp_max", "dq3.p2.st0.on", "dq3.p2.st0.x", "dq3.p2.st0.src", "dq3.p2.st1.on", "dq3.p2.st1.x", "dq3.p2.st1.src"},
    {"dq3.p3.on", "dq3.p3.name", "dq3.p3.lv", "dq3.p3.hp_t", "dq3.p3.mp_t", "dq3.p3.fcard", "dq3.p3.hp", "dq3.p3.hp_max", "dq3.p3.mp", "dq3.p3.mp_max", "dq3.p3.st0.on", "dq3.p3.st0.x", "dq3.p3.st0.src", "dq3.p3.st1.on", "dq3.p3.st1.x", "dq3.p3.st1.src"},
};
/// dq3.party.s<k>p<p>: the party reorder press of button k with counter target p.
inline constexpr const char* PartyPress[6][2] = {
    {"dq3.party.s0p0", "dq3.party.s0p1"}, {"dq3.party.s1p0", "dq3.party.s1p1"}, {"dq3.party.s2p0", "dq3.party.s2p1"}, {"dq3.party.s3p0", "dq3.party.s3p1"}, {"dq3.party.s4p0", "dq3.party.s4p1"}, {"dq3.party.s5p0", "dq3.party.s5p1"}};
inline constexpr const char* PartyDrag[4] = {"dq3.party.drag0", "dq3.party.drag1", "dq3.party.drag2", "dq3.party.drag3"};
inline constexpr const char* LvX[4] = {"dq3.p0.lvx", "dq3.p1.lvx", "dq3.p2.lvx", "dq3.p3.lvx"};
inline constexpr const char* NoDrag[4] = {"dq3.p0.nodrag", "dq3.p1.nodrag", "dq3.p2.nodrag", "dq3.p3.nodrag"};
inline constexpr const char* FtabX[4] = {"dq3.ftab0.x", "dq3.ftab1.x", "dq3.ftab2.x", "dq3.ftab3.x"};
inline constexpr const char* FtabT[4] = {"dq3.ftab0.t", "dq3.ftab1.t", "dq3.ftab2.t", "dq3.ftab3.t"};
/// The host's button-action counters of those presses (PACKAGE_FORMAT @<counter>, gen_manifest.py).
inline constexpr const char* PartyCounter[6] = {"@dq3_party_button0", "@dq3_party_button1", "@dq3_party_button2", "@dq3_party_button3", "@dq3_party_button4", "@dq3_party_button5"};

} // namespace dq3::keys
