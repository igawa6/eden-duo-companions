// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Synthetic tests for the Luminescent Platinum reader: PK8 decryption round trip, the NCE delta
// search and the battle snapshot over a fake guest memory, and the language plumbing (the save's
// language, variant detection, label lookup, CJK reflow and measuring). No game data is embedded.

#include "lp_anim.h"
#include "lp_field_actions.h"
#include "lp_battle_drive.h"
#include "lp_dex.h"
#include "lp_guest.h"
#include "lp_lang.h"
#include "lp_live.h"
#include "lp_map.h"
#include "lp_pk8.h"
#include "lp_strings_data.h"
#include "lp_switch.h"
#include "lp_text.h"
#include "lp_unity.h"
#include "lp_vanilla.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <map>

namespace {

void Put16(std::uint8_t* p, std::uint16_t v) {
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >> 8);
}
void Put32(std::uint8_t* p, std::uint32_t v) {
    for (int i = 0; i < 4; ++i)
        p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}

// Builds an encrypted core/calc pair the way the game stores it.
void Encrypt(const std::uint8_t* plain, std::uint8_t* core, const std::uint8_t* calc_plain, std::uint8_t* calc) {
    using namespace lp_pk8;
    const std::uint32_t ec = Rd32(plain);
    std::memcpy(core, plain, 8);
    const char* order = Orders[((ec >> 13) & 31) % 24];
    // shuffled[src] = plain[i] where order[src] == "ABCD"[i]
    for (int i = 0; i < 4; ++i) {
        const int src = static_cast<int>(std::strchr(order, "ABCD"[i]) - order);
        std::memcpy(core + 8 + src * BlockSize, plain + 8 + i * BlockSize, BlockSize);
    }
    Crypt(core + 8, CoreSize - 8, ec);
    std::memcpy(calc, calc_plain, CalcSize);
    Crypt(calc, CalcSize, ec);
}

void TestPk8RoundTrip() {
    using namespace lp_pk8;
    for (std::uint32_t ec : {0x627EA09Eu, 0x850FFC2Cu, 0x00012345u, 0xDEADBEEFu}) {
        std::uint8_t plain[CoreSize]{}, core[CoreSize]{}, calc_plain[CalcSize]{}, calc[CalcSize]{};
        Put32(plain, ec);
        Put16(plain + 0x08, 445);  // species
        Put16(plain + 0x0A, 326);  // held item
        Put16(plain + 0x0C, 1234); // TID
        Put16(plain + 0x0E, 5678); // SID
        Put32(plain + 0x1C, 0x11112222);
        plain[0x22] = 1 << 2; // female
        const char16_t* nick = u"Test";
        for (int i = 0; nick[i]; ++i)
            Put16(plain + 0x58 + 2 * i, nick[i]);
        const std::uint16_t moves[4]{89, 398, 14, 337};
        for (int i = 0; i < 4; ++i) {
            Put16(plain + 0x72 + 2 * i, moves[i]);
            plain[0x7A + i] = static_cast<std::uint8_t>(10 + i);
            plain[0x7E + i] = 3;
        }
        Put16(plain + 0x8A, 200);
        std::uint16_t sum = 0;
        for (std::size_t i = 8; i < CoreSize; i += 2)
            sum = static_cast<std::uint16_t>(sum + Rd16(plain + i));
        Put16(plain + 6, sum);
        const std::uint16_t c[7]{72, 252, 266, 140, 159, 185, 139};
        for (int i = 0; i < 7; ++i)
            Put16(calc_plain + 2 * i, c[i]);
        Encrypt(plain, core, calc_plain, calc);

        auto m = Decode(core, calc);
        assert(m);
        assert(m->species == 445 && m->held_item == 326 && m->gender == 1);
        assert(Utf8(m->nickname) == "Test");
        assert(m->moves[0] == 89 && m->moves[3] == 337 && m->pp[2] == 12 && m->pp_ups[1] == 3);
        assert(m->hp == 200 && m->level == 72 && m->hp_max == 252 && m->spd == 139);
        // A valid checksum must not make a native-invalid Pokémon editable.
        core[4] = 4;
        assert(!Decode(core, calc));
        core[4] = 0;
        // a flipped byte must fail the checksum instead of yielding garbage
        core[0x40] ^= 0x5A;
        assert(!Decode(core, calc));
    }
    assert(MaxPp(10, 3) == 16 && MaxPp(35, 0) == 35);
}

// An Egg's PK8 already stores its future species, nickname, moves and computed stats.
// Display sanitization is separate from decoding so native item guards retain the real Egg bit.
void TestMapGoalLayout() {
    const auto short_goal = lp_text::FitMapGoal(25, 20);
    assert(short_goal.scale == 5 && short_goal.height == 64 && short_goal.text_height == 25);
    const auto two = lp_text::FitMapGoal(65, 52);
    assert(two.scale == 5 && two.height == 89);
    const auto long_goal = lp_text::FitMapGoal(105, 84);
    assert(long_goal.scale == 5 && long_goal.height == 129);
    const auto fallback = lp_text::FitMapGoal(145, 84);
    assert(fallback.scale == 4 && fallback.height == 108 && fallback.text_height == 84);
    assert(lp_text::FitMapGoal(145, 52).height == 76);
    assert(lp_text::FitMapGoal(145, 20).height == 64);
}

void TestMonIdentity() {
    lp_pk8::Mon mon;
    mon.ec = 12; mon.pid = 34; mon.tid = 56; mon.sid = 78;
    mon.species = 445; mon.form = 1;
    auto mutable_copy = mon;
    mutable_copy.hp = 7; mutable_copy.status = 8; mutable_copy.pp[0] = 2;
    mutable_copy.held_item = 17; mutable_copy.level = 72;
    mutable_copy.nickname = u"Renamed";
    assert(lp_pk8::SameIdentity(mon, mutable_copy));
    for (int field = 0; field < 7; ++field) {
        auto other = mon;
        switch (field) {
        case 0: ++other.ec; break;
        case 1: ++other.pid; break;
        case 2: ++other.tid; break;
        case 3: ++other.sid; break;
        case 4: ++other.species; break;
        case 5: ++other.form; break;
        case 6: other.is_egg = true; break;
        }
        assert(!lp_pk8::SameIdentity(mon, other));
    }
}

void TestEggDisplayPrivacy() {
    lp_pk8::Mon mon;
    mon.species = 175; mon.form = 2; mon.gender = 1; mon.level = 1;
    mon.shiny = true; mon.is_egg = true; mon.nickname = u"Togepi";
    mon.hp = 12; mon.hp_max = 12; mon.atk = 9; mon.ability = 32; mon.held_item = 26;
    mon.moves = {33, 45, 118, 204}; mon.pp = {35, 40, 10, 20}; mon.friendship = 120;
    const auto egg = lp_pk8::DisplayMon(mon);
    assert(egg.is_egg && egg.species == 0 && egg.form == 0 && egg.gender == 2);
    assert(!egg.shiny && egg.nickname.empty() && egg.level == 0 && egg.ability == 0);
    assert(egg.hp == 0 && egg.hp_max == 0 && egg.atk == 0 && egg.held_item == 0 && egg.friendship == 0);
    assert((egg.moves == std::array<std::uint16_t, 4>{}));
    assert((egg.pp == std::array<std::uint8_t, 4>{}));
    assert(mon.species == 175 && mon.shiny && mon.hp == 12); // gameplay copy survives
    mon.is_egg = false;
    const auto hatched = lp_pk8::DisplayMon(mon);
    assert(hatched.species == 175 && hatched.shiny && hatched.hp == 12 && hatched.moves[2] == 118);
}

// A flat fake address space: page-granular byte map.
struct FakeMemory {
    std::map<std::uint64_t, std::uint8_t> bytes;
    void W64(std::uint64_t a, std::uint64_t v) {
        for (int i = 0; i < 8; ++i)
            bytes[a + i] = static_cast<std::uint8_t>(v >> (8 * i));
    }
    void W32(std::uint64_t a, std::uint32_t v) {
        for (int i = 0; i < 4; ++i)
            bytes[a + i] = static_cast<std::uint8_t>(v >> (8 * i));
    }
    void W16(std::uint64_t a, std::uint16_t v) {
        bytes[a] = static_cast<std::uint8_t>(v);
        bytes[a + 1] = static_cast<std::uint8_t>(v >> 8);
    }
    void W8(std::uint64_t a, std::uint8_t v) {
        bytes[a] = v;
    }
    bool Read(std::uint64_t a, void* out, std::size_t n) const {
        auto* o = static_cast<std::uint8_t*>(out);
        for (std::size_t i = 0; i < n; ++i) {
            auto it = bytes.find(a + i);
            if (it == bytes.end())
                return false;
            o[i] = it->second;
        }
        return true;
    }
    bool Write(std::uint64_t a, const void* in, std::size_t n) {
        auto* p = static_cast<const std::uint8_t*>(in);
        for (std::size_t i = 0; i < n; ++i)
            bytes[a + i] = p[i];
        return true;
    }
};

void TestAudioSingletonReadiness() {
    using namespace lp_live;
    constexpr u64 Main = 0x80000000, Klass = 0x210000, Statics = 0x220000, Audio = 0x230000;
    for (const std::int64_t delta : {std::int64_t{0}, std::int64_t{0x3000}, std::int64_t{-0x1C000}}) {
        FakeMemory mem;
        for (const auto& fingerprint : off::Fingerprints)
            mem.W64(Main + fingerprint.slot + delta, Main + fingerprint.target);
        Guest guest;
        guest.main_base = Main;
        guest.read = [&](u64 address, void* output, std::size_t size) { return mem.Read(address,output,size); };
        Reader reader{guest};
        assert(reader.AudioManagerObject() == 0);
        assert(reader.Resolve(0));
        assert(reader.AudioManagerObject() == 0);
        mem.W64(Main + lp_profile::Active.AudioManagerTypeInfo + delta, Klass);
        mem.W64(Klass + off::KlassStatics, Statics);
        mem.W64(Statics, Audio);
        assert(reader.AudioManagerObject() == 0);
        mem.W64(Audio + 0x40, 0x240000); // native instance list
        assert(reader.AudioManagerObject() == 0);
        mem.W64(Audio + 0x48, 0x250000); // native pool
        assert(reader.AudioManagerObject() == Audio);
        mem.W64(Audio + 0x48, 0);
        assert(reader.AudioManagerObject() == 0);
        mem.W64(Statics, 0);
        assert(reader.AudioManagerObject() == 0);
    }
}

void TestDeltaAndBattle() {
    using namespace lp_live;
    constexpr std::uint64_t Main = 0x80000000;
    for (std::int64_t delta : {std::int64_t{0}, std::int64_t{0x3000}, std::int64_t{-0x1C000}}) {
        FakeMemory mem;
        for (const auto& f : off::Fingerprints)
            mem.W64(Main + f.slot + delta, Main + f.target);
        // BattleViewCore singleton chain: slot -> MethodInfo -> klass -> statics -> instance
        const std::uint64_t method = 0x1000000, klass = 0x1001000, statics = 0x1002000, bvc = 0x1003000;
        const std::uint64_t ui = 0x1004000, action = 0x1005000, waza = 0x1006000;
        const std::uint64_t ball = 0x1007000, balls = 0x1008000;
        mem.W64(Main + off::BattleViewCoreGetInstance + delta, method);
        mem.W64(method + off::MethodKlass, klass);
        mem.W64(klass + off::KlassStatics, statics);
        mem.W64(statics, bvc);
        mem.W64(bvc + off::BvcUiSystem, ui);
        mem.W64(bvc + off::BvcViewSystem, 0);
        mem.W64(ui + off::UiActionList, action);
        mem.W64(ui + off::UiWazaList, waza);
        mem.W64(ui + off::UiPokeBallList, ball);
        mem.W64(ball + off::BallListBalls, balls);
        mem.W32(balls + off::ListSize, 2);
        for (std::uint64_t list : {action, waza, ball}) {
            for (std::uint64_t o = 0; o < 0x70; ++o) // the canvas object is mapped as a whole
                mem.W8(list + o, 0);
            mem.W32(list + off::CanvasMaxIndex, 0);
            mem.W32(list + off::CanvasCurrentIndex, 3);
            mem.W8(list + off::CanvasIsFocus, list == action);
            mem.W8(list + off::CanvasIsShow, list == action);
            mem.W8(list + off::CanvasTransitionType, list == action ? 1 : 0);
            mem.W32(list + off::CanvasHideAnchor, 0x44218000); // 646.0f
            mem.W32(list + off::CanvasShowAnchor, 0);
            mem.W64(list + off::CanvasGroup, 0);
        }
        mem.W64(Main + off::PlayerWorkTypeInfo + delta, 0); // no PlayerWork yet

        Guest g;
        g.main_base = Main;
        g.read = [&](std::uint64_t a, void* o, std::size_t n) { return mem.Read(a, o, n); };
        g.write = [&](std::uint64_t a, const void* i, std::size_t n) { return mem.Write(a, i, n); };
        Reader r{g};
        assert(r.Resolve(0));
        assert(r.Delta() == delta);
        mem.W32(action + off::ActionMinIndex, 0);
        mem.W32(action + off::ActionMaxIndex, 0); // first starter fight: only Fight is visible
        assert(r.Sample().action_min == 0 && r.Sample().action_max == 0);
        mem.W32(action + off::ActionMaxIndex, 3);
        assert(r.Sample().action_max == 3);
        mem.W32(action + off::ActionMaxIndex, 99);
        assert(r.Sample().action_max == 3); // corrupt/uninitialised bounds use the normal layout
        mem.W32(action + off::ActionMaxIndex, 3);

        auto s = r.Sample();
        assert(s.in_battle && !s.player_ok);
        assert(s.action.focus && s.action.show && s.action.current == 3);
        assert(!s.waza.focus);
        // IsTransition (+0x63): unread counts as not moving; a slide is reported
        assert(!s.waza.transition);
        mem.W8(waza + off::CanvasIsTransition, 1);
        assert(r.Sample().waza.transition);
        mem.W8(waza + off::CanvasIsTransition, 0);
        assert(!r.Sample().waza.transition);
        // Dialogue Continue is fail-closed and cannot select an intervening native menu.
        assert(!r.CanContinueBattle() && !r.Sample().battle_continue);
        {
            const std::uint64_t msg = 0x1010000, model = 0x1011000, param = 0x1012000;
            mem.W64(ui + off::UiMessageWindow, msg);
            mem.W64(msg + off::MessageModel, model);
            mem.W64(model + off::MessageParam, param);
            mem.W8(msg + off::MessageAuto, 0);
            mem.W8(model + off::MessageInput, 1);
            mem.W32(model + off::MessageState, 5);
            mem.W8(ui + off::UiMessageOpen, 1);
            mem.W8(ui + off::UiMessageSleep, 0);
            mem.W8(ui + off::UiMenuEnd, 1);
            assert(!r.CanContinueBattle()); // Fight still owns native focus
            mem.W8(action + off::CanvasIsFocus, 0);
            assert(r.CanContinueBattle() && r.Sample().battle_continue);
            for (int state : {0, 1, 2, 6, 8, 99}) {
                mem.W32(model + off::MessageState, state);
                assert(!r.CanContinueBattle()); // closed, transition or automatic wait
            }
            for (int state : {3, 4, 5}) {
                mem.W32(model + off::MessageState, state);
                assert(r.CanContinueBattle());
            }
            mem.W32(model + off::MessageState, 7);
            assert(!r.CanContinueBattle());
            mem.W64(ui + off::UiMessageQueue, 0x1015000);
            mem.W8(ui + off::UiMessageKeyWait, 1);
            assert(r.CanContinueBattle()); // trainer defeat uses the battle coroutine's A wait
            mem.W8(model + off::MessageInput, 0);
            mem.W8(msg + off::MessageAuto, 1);
            assert(r.CanContinueBattle()); // coroutine owns A even when MsgWindow does not
            mem.W8(model + off::MessageInput, 1);
            mem.W8(msg + off::MessageAuto, 0);
            mem.W8(ui + off::UiMessageKeyWait, 0);
            assert(!r.CanContinueBattle());
            mem.W64(ui + off::UiMessageQueue, 0);
            mem.W8(param + off::MessageCloseInput, 1);
            assert(r.CanContinueBattle());
            mem.W32(model + off::MessageState, 5);
            for (std::uint64_t list : {action, waza, ball}) {
                mem.W8(list + off::CanvasIsFocus, 1);
                assert(!r.CanContinueBattle());
                mem.W8(list + off::CanvasIsFocus, 0);
                mem.W8(list + off::CanvasIsTransition, 1);
                assert(!r.CanContinueBattle());
                mem.W8(list + off::CanvasIsTransition, 0);
            }
            const std::uint64_t target = 0x1013000;
            mem.W64(ui + off::UiTargetSelect, target);
            for (std::uint64_t o = 0; o < 0x70; ++o)
                mem.W8(target + o, 0);
            mem.W8(target + off::CanvasIsFocus, 1);
            assert(!r.CanContinueBattle()); // never confirm a double-battle target
            mem.W8(target + off::CanvasIsFocus, 0);
            mem.W8(target + off::CanvasIsTransition, 1);
            assert(!r.CanContinueBattle());
            mem.W8(target + off::CanvasIsTransition, 0);
            assert(r.CanContinueBattle());
            mem.W8(ui + off::UiMenuEnd, 0); // no menu opened yet: dialogue must still work
            assert(r.CanContinueBattle());
            mem.W8(ui + off::UiMenuEnd, 1);
            mem.W8(msg + off::MessageAuto, 1);
            assert(!r.CanContinueBattle());
            mem.W8(msg + off::MessageAuto, 0);
            mem.W8(model + off::MessageInput, 0);
            assert(!r.CanContinueBattle());
            mem.W8(model + off::MessageInput, 1);
            mem.W8(ui + off::UiMessageSleep, 1);
            assert(!r.CanContinueBattle());
            mem.W8(ui + off::UiMessageSleep, 0);
            mem.W8(ui + off::UiMessageOpen, 0);
            assert(!r.CanContinueBattle());
            mem.W8(action + off::CanvasIsFocus, 1);
        }
        // Native EXP result owns its own message controller and accepts A while ordinary
        // BattleView dialogue is closed. Learning-move questions must never be auto-confirmed.
        {
            const std::uint64_t win = 0x1020000, inst = 0x1021000, input = 0x1022000;
            const std::uint64_t ctl = 0x1023000, msg = 0x1024000, model = 0x1025000;
            const std::uint64_t prm = 0x1026000, label = 0x1027000, panel = 0x1028000;
            mem.W8(action + off::CanvasIsFocus, 0);
            mem.W8(ui + off::UiMenuEnd, 0);
            mem.W8(ui + off::UiMessageOpen, 0);
            mem.W64(ui + off::UiMenuWindow, win);
            mem.W64(win + off::WindowInstance, inst);
            mem.W64(inst + off::InstanceWindow, win);
            mem.W32(inst + off::InstanceWindowId, off::WindowIdLevelUp);
            mem.W8(win + off::WindowClosing, 0);
            mem.W64(win + off::WindowInput, input);
            mem.W8(input + off::InputEnabled, 1);
            mem.W64(win + off::ExpMessageController, ctl);
            mem.W8(ctl + off::ExpControllerWait, 1);
            mem.W64(ctl + off::ExpControllerMessage, msg);
            mem.W64(msg + off::MessageModel, model);
            mem.W32(model + off::MessageState, 7);
            mem.W64(ctl + off::ExpControllerParam, prm);
            mem.W64(prm + 0x10, label);
            auto set_label = [&](std::u16string_view name) {
                mem.W32(label + 0x10, static_cast<u32>(name.size()));
                for (size_t i = 0; i < name.size(); ++i) mem.W16(label + 0x14 + 2*i, name[i]);
            };
            for (const auto name : {u"SS_level_up_02_01", u"SS_level_up_02_08", u"SS_level_up_02_02",
                                    u"SS_level_up_02_03", u"SS_level_up_02_05", u"SS_level_up_02_06"}) {
                set_label(name);
                assert(r.CanContinueBattle() && r.Sample().battle_continue);
            }
            set_label(u"SS_level_up_02_04"); // A finishes the learning prompt before native choices
            assert(r.CanContinueBattle());
            set_label(u"SS_level_up_02_01");
            mem.W32(model + off::MessageState, 3); // native controller is not waiting for A yet
            assert(!r.CanContinueBattle());
            mem.W32(model + off::MessageState, 7);
            mem.W32(inst + off::InstanceWindowId, 11); // context menu, regardless of stale EXP fields
            assert(!r.CanContinueBattle());
            mem.W32(inst + off::InstanceWindowId, off::WindowIdLevelUp);
            mem.W8(input + off::InputEnabled, 0);
            assert(!r.CanContinueBattle());
            mem.W8(input + off::InputEnabled, 1);
            mem.W8(win + off::WindowClosing, 1);
            assert(!r.CanContinueBattle());
            mem.W8(win + off::WindowClosing, 0);
            mem.W64(inst + off::InstanceWindow, 0); // stale pooled UI pointer
            assert(!r.CanContinueBattle());
            mem.W64(inst + off::InstanceWindow, win);
            mem.W8(ctl + off::ExpControllerWait, 0);
            mem.W8(win + off::ExpGaugeAnimating, 0);
            mem.W64(win + off::ExpStatusPanel, panel);
            mem.W8(panel + off::ExpStatusAnimating, 0);
            mem.W8(panel + off::ExpStatusShown, 0);
            mem.W8(win + off::ExpWaitExit, 1);
            assert(r.CanContinueBattle()); // final EXP exit: exact native A/B wait
            mem.W8(win + off::ExpGaugeAnimating, 1);
            assert(!r.CanContinueBattle());
            mem.W8(win + off::ExpGaugeAnimating, 0);
            mem.W8(win + off::ExpWaitExit, 0); // native clears BEFORE move-learning sequence
            assert(!r.CanContinueBattle());
            mem.W8(panel + off::ExpStatusShown, 1);
            assert(r.CanContinueBattle()); // numeric level-up stats: A advances the card
            mem.W8(panel + off::ExpStatusAnimating, 1);
            assert(!r.CanContinueBattle());
            mem.W8(action + off::CanvasIsFocus, 1);
            mem.W8(ui + off::UiMenuEnd, 1);
        }
        // the game's Bag / Pokémon window over the battle menu (UISystem +0xE8, +0xAC; UIInstance)
        assert(s.battle_window == 0);
        {
            const std::uint64_t win = 0x1009000, inst = 0x100A000;
            mem.W8(ui + off::UiMenuEnd, 0);
            mem.W64(ui + off::UiMenuWindow, win);
            mem.W8(win + off::WindowClosing, 0);
            mem.W64(win + off::WindowInstance, inst);
            mem.W64(inst + off::InstanceWindow, win);
            mem.W32(inst + off::InstanceWindowId, off::WindowIdBag);
            auto w = r.Sample();
            assert(w.battle_window == off::WindowIdBag);
            mem.W32(inst + off::InstanceWindowId, off::WindowIdPokemonBattle);
            w = r.Sample();
            assert(w.battle_window == off::WindowIdPokemonBattle);
            mem.W8(win + off::WindowClosing, 1); // closing: already gone for the companion
            assert(r.Sample().battle_window == 0);
            mem.W8(win + off::WindowClosing, 0);
            mem.W64(inst + off::InstanceWindow, 0); // back in the pool: _uiWindow keeps a stale pointer
            assert(r.Sample().battle_window == 0);
            mem.W64(inst + off::InstanceWindow, win);
            mem.W8(ui + off::UiMenuEnd, 1); // onClosed ran
            assert(r.Sample().battle_window == 0);
            mem.W8(ui + off::UiMenuEnd, 0);
            mem.W32(inst + off::InstanceWindowId, 10); // another window (POKEMON_STATUS) is not ours
            assert(r.Sample().battle_window == 0);
            mem.W8(ui + off::UiMenuEnd, 1);
        }
        assert(!r.SetBallIndex(1)); // closed list cannot be driven
        mem.W8(ball + off::CanvasIsFocus, 1);
        assert(!r.SetBallIndex(-1) && !r.SetBallIndex(2));
        assert(r.SetBallIndex(1));
        assert(r.Sample().ball.current == 1);
        mem.W8(ball + off::CanvasIsFocus, 0);

        assert(r.SetActionIndex(0));
        assert(r.Sample().action.current == 0);
        assert(r.HideBattleMenus(false) == false); // no deco image in the fake memory -> reported incomplete
        r.HideBattleMenus(true);
        float show = 0;
        mem.Read(action + off::CanvasShowAnchor, &show, 4);
        assert(show == 2400.0f);
        mem.Read(ball + off::CanvasShowAnchor, &show, 4);
        assert(show == 2400.0f);
        std::uint8_t type = 0;
        mem.Read(waza + off::CanvasTransitionType, &type, 1);
        assert(type == 1);
        r.HideBattleMenus(false); // restore the originals
        mem.Read(action + off::CanvasShowAnchor, &show, 4);
        mem.Read(waza + off::CanvasTransitionType, &type, 1);
        assert(show == 0.0f && type == 0);
        mem.Read(ball + off::CanvasShowAnchor, &show, 4);
        assert(show == 0.0f);

        // A missing write capability refuses safely instead of invoking an empty callback.
        Guest readonly = g;
        readonly.write = {};
        Reader ro{readonly};
        assert(ro.Resolve(0));
        assert(!ro.SetActionIndex(0) && !ro.SetWazaIndex(0) && !ro.SetBallIndex(0));
        assert(!ro.SetDecoAlpha(0) && !ro.HideBattleMenus(true));

        constexpr u64 Image = 0x1030000;
        mem.W64(ui + off::UiDecoImage, Image);
        mem.W32(Image + off::GraphicColorA, 0x3F800000); // alpha 1
        bool missing_anchor = true;
        Guest fault = g;
        fault.read = [&](u64 a, void* out, size_t n) {
            return !(missing_anchor && a == action + off::CanvasTransitionType) && mem.Read(a, out, n);
        };
        Reader unreadable{fault};
        assert(unreadable.Resolve(0));
        assert(!unreadable.HideBattleMenus(true));
        mem.Read(action + off::CanvasShowAnchor, &show, 4);
        assert(show == 0.0f); // failed snapshot never mutates this canvas
        missing_anchor = false;
        assert(unreadable.HideBattleMenus(false));
        float hide = 0;
        mem.Read(action + off::CanvasHideAnchor, &hide, 4);
        mem.Read(action + off::CanvasTransitionType, &type, 1);
        assert(hide == 646.0f && type == 1); // no placeholder saved/restored

        bool reject_restore = false;
        fault.write = [&](u64 a, const void* in, size_t n) {
            return !(reject_restore && a == action + off::CanvasHideAnchor) && mem.Write(a, in, n);
        };
        Reader retry{fault};
        assert(retry.Resolve(0) && retry.HideBattleMenus(true));
        reject_restore = true;
        assert(!retry.HideBattleMenus(false)); // propagate a failed restore
        mem.Read(action + off::CanvasHideAnchor, &hide, 4);
        assert(hide == 2400.0f);
        reject_restore = false;
        assert(retry.HideBattleMenus(false)); // retain the real anchors for a later retry
        mem.Read(action + off::CanvasHideAnchor, &hide, 4);
        assert(hide == 646.0f);

        // Alpha can fail after all anchors restore; a second call must still retry it.
        constexpr u64 Group = 0x1031000, NativeGroup = 0x1032000;
        mem.W64(action + off::CanvasGroup, Group);
        mem.W64(Group + off::NativePtr, NativeGroup);
        mem.W8(action + off::CanvasIsShow, 1);
        mem.W32(NativeGroup + off::NativeCanvasGroupAlpha, 0x3F800000);
        bool reject_alpha = false;
        fault.write = [&](u64 a, const void* in, size_t n) {
            return !(reject_alpha && a == NativeGroup + off::NativeCanvasGroupAlpha) && mem.Write(a, in, n);
        };
        Reader alpha_retry{fault};
        assert(alpha_retry.Resolve(0) && alpha_retry.HideBattleMenus(true));
        float alpha = 1;
        mem.Read(NativeGroup + off::NativeCanvasGroupAlpha, &alpha, 4);
        assert(alpha == 0);
        reject_alpha = true;
        assert(!alpha_retry.HideBattleMenus(false));
        mem.Read(action + off::CanvasHideAnchor, &hide, 4);
        mem.Read(action + off::CanvasShowAnchor, &show, 4);
        mem.Read(NativeGroup + off::NativeCanvasGroupAlpha, &alpha, 4);
        assert(hide == 646.0f && show == 0 && alpha == 0);
        reject_alpha = false;
        assert(alpha_retry.HideBattleMenus(false));
        mem.Read(NativeGroup + off::NativeCanvasGroupAlpha, &alpha, 4);
        assert(alpha == 1);
        reject_alpha = true;
        assert(!alpha_retry.HideBattleMenus(true));
        mem.Read(action + off::CanvasHideAnchor, &hide, 4);
        mem.Read(action + off::CanvasShowAnchor, &show, 4);
        mem.Read(NativeGroup + off::NativeCanvasGroupAlpha, &alpha, 4);
        assert(hide == 2400.0f && show == 2400.0f && alpha == 1);
        reject_alpha = false;
        assert(alpha_retry.HideBattleMenus(true));
        mem.Read(NativeGroup + off::NativeCanvasGroupAlpha, &alpha, 4);
        assert(alpha == 0); // retry fade even though anchors were already parked
        assert(alpha_retry.HideBattleMenus(false));

        // Every battle sample parks again: the fade is written only when the list is visible.
        int alpha_writes = 0;
        fault.write = [&](u64 a, const void* in, size_t n) {
            if (a == NativeGroup + off::NativeCanvasGroupAlpha) ++alpha_writes;
            return mem.Write(a, in, n);
        };
        Reader steady{fault};
        assert(steady.Resolve(0) && steady.HideBattleMenus(true) && alpha_writes == 1);
        for (int k = 0; k < 5; ++k)
            assert(steady.HideBattleMenus(true));
        assert(alpha_writes == 1);
        mem.W32(NativeGroup + off::NativeCanvasGroupAlpha, 0x3F800000); // the game showed the list
        assert(steady.HideBattleMenus(true) && alpha_writes == 2);

        // A later module instance meets the lists parked: they are not its originals, so it leaves
        // them until it sees the list unparked, then saves and restores the real values.
        Reader later{fault};
        assert(later.Resolve(0) && later.HideBattleMenus(true));
        assert(later.HideBattleMenus(false)); // nothing known to put back
        mem.Read(action + off::CanvasShowAnchor, &show, 4);
        assert(show == 2400.0f);
        assert(steady.HideBattleMenus(false)); // the instance that parked them restores them
        mem.Read(action + off::CanvasHideAnchor, &hide, 4);
        mem.Read(action + off::CanvasShowAnchor, &show, 4);
        mem.Read(action + off::CanvasTransitionType, &type, 1);
        assert(hide == 646.0f && show == 0.0f && type == 1);
        assert(later.HideBattleMenus(true)); // unparked now: the real originals are saved
        mem.Read(action + off::CanvasShowAnchor, &show, 4);
        assert(show == 2400.0f);
        assert(later.HideBattleMenus(false));
        mem.Read(action + off::CanvasHideAnchor, &hide, 4);
        mem.Read(action + off::CanvasShowAnchor, &show, 4);
        mem.Read(waza + off::CanvasTransitionType, &type, 1);
        assert(hide == 646.0f && show == 0.0f && type == 0);
    }
}

void TestMedicineTransactions() {
    using namespace lp_live;
    constexpr u64 Main = 0x80000000, Klass = 0x100000, Statics = 0x110000, Pw = 0x120000;
    constexpr u64 Party = 0x130000, Members = 0x140000, Param = 0x150000;
    constexpr u64 Core = 0x160000, Calc = 0x170000, Bag = 0x180000;
    FakeMemory mem;
    for (const auto& f : off::Fingerprints) mem.W64(Main + f.slot, Main + f.target);
    mem.W64(Main + off::PlayerWorkTypeInfo, Klass);
    mem.W64(Klass + off::KlassStatics, Statics);
    mem.W64(Statics + off::PlayerWorkInstance, Pw);
    mem.W64(Pw + off::PwParty, Party);
    mem.W64(Party + off::PartyMembers, Members);
    mem.W32(Party + off::PartyCount, 1);
    mem.W64(Members + off::ArrayLength, 6);
    mem.W64(Members + off::ArrayData, Param);
    mem.W64(Param + off::ParamCore, Core);
    mem.W64(Param + off::ParamCalc, Calc);
    mem.W64(Core + off::ArrayLength, lp_pk8::CoreSize);
    mem.W64(Calc + off::ArrayLength, lp_pk8::CalcSize);
    mem.W64(Pw + off::PwSaveItem, Bag);
    mem.W64(Bag + off::ArrayLength, 200);
    bool reject_batch = false;
    std::function<void()> before_batch;
    Guest g;
    g.main_base = Main;
    g.read = [&](u64 a, void* o, size_t n) { return mem.Read(a,o,n); };
    g.batch = [&](const std::vector<BatchOp>& ops) {
        if (reject_batch) return false;
        auto hook = std::move(before_batch);
        before_batch = {};
        if (hook) hook();
        assert(ops.size() <= 16);
        for (const auto& op : ops) {
            assert(!op.value.empty() && op.value.size() <= 64);
            std::vector<u8> bytes(op.expect.size());
            if (!mem.Read(op.addr, bytes.data(), bytes.size()) || bytes != op.expect) return false;
        }
        for (const auto& op : ops) mem.Write(op.addr, op.value.data(), op.value.size());
        return true;
    };
    Reader reader(g);
    assert(reader.Resolve(0));
    assert(reader.Sample().game_version == -1);
    mem.W8(Pw + off::PwRomCode, 0);
    assert(reader.Sample().game_version == 0);
    mem.W8(Pw + off::PwRomCode, 1);
    assert(reader.Sample().game_version == 1);
    mem.W8(Pw + off::PwRomCode, 0);
    // An empty party is valid; an absent/corrupt count is unavailable, not empty.
    mem.W32(Party + off::PartyCount, 0);
    assert(reader.Sample().party_ok && reader.Sample().party_count == 0);
    mem.W32(Party + off::PartyCount, 7);
    assert(!reader.Sample().party_ok);
    mem.W64(Pw + off::PwParty, 0);
    assert(!reader.Sample().party_ok);
    mem.W64(Pw + off::PwParty, Party);
    mem.W32(Party + off::PartyCount, 1);
    mem.W64(Members + off::ArrayLength, 0);
    assert(!reader.Sample().party_ok); // count and array must agree
    mem.W64(Members + off::ArrayLength, 7);
    assert(!reader.Sample().party_ok);
    mem.W64(Members + off::ArrayLength, 6);
    assert(reader.Sample().party_ok);

    mem.bytes[Pw+off::PwIsBattling] = 1;
    assert(reader.Sample().is_battling);
    mem.bytes[Pw+off::PwIsBattling] = 0;
    assert(!reader.Sample().is_battling);
    mem.W32(Pw + off::PwPedometer, 98765);
    assert(reader.Sample().steps == 98765 && reader.Sample().steps_ok);
    mem.bytes.erase(Pw + off::PwPedometer);
    assert(reader.Sample().steps == 98765 && !reader.Sample().steps_ok);
    mem.W32(Pw + off::PwPedometer, 100000);
    assert(reader.Sample().steps == 98765 && !reader.Sample().steps_ok);
    assert(!reader.ResetPedometer());
    mem.W32(Pw + off::PwPedometer, 99999);
    assert(reader.ResetPedometer() && reader.Sample().steps == 0);
    mem.W32(Pw + off::PwPedometer, 1);
    assert(reader.Sample().steps == 1);
    mem.W8(Pw + off::PwIsBattling, 1);
    assert(!reader.ResetPedometer() && reader.Sample().steps == 1);
    mem.W8(Pw + off::PwIsBattling, 0);
    auto fixture = [&](int hp, u32 status, int pp, int friendship = 0, bool traded = false) {
        std::array<u8, lp_pk8::CoreSize> plain{}, enc{};
        Put32(plain.data(), 0x627EA09E);
        Put16(plain.data()+8, 445);
        Put16(plain.data()+0x72, 89);
        Put16(plain.data()+0x74, 14);
        plain[0x7A] = plain[0x7B] = static_cast<u8>(pp);
        Put16(plain.data()+0x8A, static_cast<u16>(hp));
        Put32(plain.data()+0x94, status);
        plain[0xC4] = traded ? 1 : 0;
        plain[traded ? 0xC8 : 0x112] = static_cast<u8>(friendship);
        lp_pk8::EncryptRaw(plain, enc.data());
        std::array<u8, lp_pk8::CalcSize> calc{};
        Put16(calc.data(), 72);
        Put16(calc.data()+2, 101);
        lp_pk8::Crypt(calc.data(), calc.size(), lp_pk8::Rd32(plain.data()));
        mem.Write(Core+off::ArrayData, enc.data(), enc.size());
        mem.Write(Calc+off::ArrayData, calc.data(), calc.size());
        mem.W32(Bag+off::ArrayData+17*off::SaveItemSize, 2);
    };
    auto mon = [&] {
        std::array<u8, lp_pk8::CoreSize> core{};
        std::array<u8, lp_pk8::CalcSize> calc{};
        assert(mem.Read(Core+off::ArrayData, core.data(), core.size()));
        assert(mem.Read(Calc+off::ArrayData, calc.data(), calc.size()));
        const auto decoded = lp_pk8::Decode(core.data(), calc.data());
        assert(decoded); // checksum and shuffle survive every edit
        return *decoded;
    };
    auto count = [&] { u32 n = 0; assert(mem.Read(Bag+off::ArrayData+17*off::SaveItemSize, &n,4)); return n; };
    fixture(0,0,0);
    assert(reader.UseMedicine(0,17,20,0,0,{},0) == 0 && count() == 2);
    assert(reader.UseMedicine(0,17,254,1u<<22,0,{},0) == 50);
    assert(mon().hp == 50 && count() == 1);
    assert(reader.UseMedicine(0,17,255,1u<<22,0,{},0) == 0 && count() == 1);
    for (u32 invalid : {6u, 8u, 0x80u, 0xFFFFFFFFu}) {
        fixture(101, invalid, 0);
        assert(reader.UseMedicine(0,17,0,0xF8000u,0,{},0) == 0 && count() == 2);
        assert(mon().status == invalid); // legacy bit masks cannot masquerade as a native status
    }
    fixture(101,5,0);
    assert(reader.UseMedicine(0,17,0,1u<<17,0,{},0) == 0 && count() == 2); // wrong cure
    assert(reader.UseMedicine(0,17,0,1u<<16,0,{},0) == 1);
    assert(mon().hp == 101 && mon().status == 0 && count() == 1);
    for (const auto [status, flag] : std::array<std::pair<u32,u32>,5>{{
        {2,1u<<15}, {5,1u<<16}, {4,1u<<17}, {3,1u<<18}, {1,1u<<19}}}) {
        for (int bit = 15; bit <= 19; ++bit) {
            fixture(101,status,0);
            if (flag != (1u << bit)) {
                assert(reader.UseMedicine(0,17,0,1u<<bit,0,{},0) == 0);
                assert(mon().status == status && count() == 2);
            }
        }
        fixture(101,status,0);
        assert(reader.UseMedicine(0,17,0,flag,0,{},0) == 1);
        assert(mon().status == 0 && count() == 1);
    }
    fixture(75,1,2);
    assert(reader.UseMedicine(0,17,255,0x1f8000,0,{},0) == 26);
    assert(mon().hp == 101 && mon().status == 0 && mon().pp[0] == 2);
    fixture(101,0,2);
    assert(reader.UseMedicine(0,17,0,1u<<10,10,{10,20,0,0},1) == 1);
    assert(mon().pp[0] == 2 && mon().pp[1] == 12 && count() == 1);
    assert(reader.UseMedicine(0,17,0,1u<<11,127,{10,20,0,0},0) == 1);
    assert(mon().pp[0] == 10 && mon().pp[1] == 20 && count() == 0);
    fixture(50,0,0);
    const auto medicine_owner = mon();
    for (int change = 0; change < 5; ++change) {
        auto stale = medicine_owner;
        if (change == 0) ++stale.ec;
        if (change == 1) ++stale.moves[0];
        if (change == 2) ++stale.pp[0];
        if (change == 3) ++stale.pp_ups[0];
        if (change == 4) stale.is_egg = true;
        assert(reader.UseMedicine(0,17,20,0,0,{},0,{},&stale) == 0);
        assert(mon().hp == 50 && count() == 2);
    }
    assert(reader.UseMedicine(0,17,20,0,0,{},0,{},&medicine_owner) == 20);
    assert(mon().hp == 70 && count() == 1);
    fixture(50,0,0);
    reject_batch = true;
    assert(reader.UseMedicine(0,17,20,0,0,{},0) == 0 && mon().hp == 50 && count() == 2);
    reject_batch = false;
    assert(reader.UseMedicine(0,17,20,0,0,{},0,{3,3,0}) == 0 && count() == 2); // bonus modifiers stay native
    assert(reader.UseMedicine(0,17,255,1u<<23,0,{},0) == 0 && count() == 2);
    assert(reader.UseMedicine(1,17,20,0,0,{},0) == 0 && count() == 2);
    mem.bytes[Core+off::ArrayData+4] = 4;
    assert(reader.UseMedicine(0,17,20,0,0,{},0) == 0 && count() == 2);
    for (bool traded : {false,true}) {
        for (int friendship : {5,99,100,199,200,255}) {
            fixture(50,0,0,friendship,traded);
            assert(mon().friendship == friendship); // the current handler's value
            assert(reader.UseMedicine(0,17,20,0,0,{},0,{-5,-10,-15}) == 20);
            std::array<u8,lp_pk8::CoreSize> enc{}, plain{};
            assert(mem.Read(Core+off::ArrayData,enc.data(),enc.size()));
            assert(lp_pk8::DecryptRaw(enc.data(),plain));
            const int penalty = friendship < 100 ? 5 : friendship < 200 ? 10 : 15;
            assert(plain[traded ? 0xC8 : 0x112] == std::max(0,friendship-penalty));
            assert(count() == 1 && mon().hp == 70);
        }
    }
    // EditParty: complete-core preconditions preserve encryption and checksum validity
    fixture(50, 0, 10);
    assert(reader.EditParty({{0, [](std::array<u8, lp_pk8::CoreSize>& p, const lp_pk8::Mon& m) {
        p[0x8A] = static_cast<u8>(m.hp - 20); p[0x7E] = 1; p[0x7A] = static_cast<u8>(m.pp[0] + 2); return true; }}}, 17));
    assert(mon().hp == 30 && mon().pp_ups[0] == 1 && mon().pp[0] == 12 && count() == 1);
    assert(!reader.EditParty({{0, [](auto&, const lp_pk8::Mon&) { return false; }}}, 17) && count() == 1); // refused
    assert(!reader.EditParty({{3, [](auto&, const lp_pk8::Mon&) { return true; }}}, 0));                // no member 3
    assert(!reader.EditParty({{0, [](auto&, const lp_pk8::Mon&) { return true; }}}, 17));
    assert(count() == 1); // no-op callback must not consume an item
    assert(!reader.EditParty({{0, {}}}, 17) && count() == 1); // missing callback
    constexpr u64 FmKlass = 0x190000, FmStatics = 0x191000, Fm = 0x192000;
    constexpr u64 DemoKlass = 0x193000, DemoStatics = 0x194000;
    mem.W64(Main + off::FieldManagerTypeInfo, FmKlass);
    mem.W64(FmKlass + off::KlassStatics, FmStatics);
    mem.W64(FmStatics, Fm);
    mem.W8(Fm + off::FmInit, 1);
    mem.W8(Fm + off::FmMenuOpen, 0);
    mem.W64(Main + lp_profile::Active.DemoSceneManagerTypeInfo, DemoKlass);
    mem.W64(DemoKlass + off::KlassStatics, DemoStatics);
    mem.W8(DemoStatics, 0);
    // Reordering must carry PP/PP Ups and retain valid encryption/checksum.
    fixture(50, 5, 10);
    assert(reader.EditParty({{0, [](auto& p, const auto&) {
        p[0x7A] = 7; p[0x7B] = 12; p[0x7E] = 1; p[0x7F] = 3; return true;
    }}}, 0));
    const auto before_moves = mon();
    assert(!reader.SwapMoves(0, -1, 1, before_moves));
    assert(!reader.SwapMoves(0, 0, 4, before_moves));
    assert(!reader.SwapMoves(0, 0, 0, before_moves));
    assert(!reader.SwapMoves(0, 0, 2, before_moves)); // empty move
    auto stale = before_moves; ++stale.ec;
    assert(!reader.SwapMoves(0, 0, 1, stale));
    stale = before_moves; ++stale.pp[0];
    assert(!reader.SwapMoves(0, 0, 1, stale));
    mem.W8(DemoStatics, 1);
    assert(reader.Sample().demo_active);
    assert(!reader.SwapMoves(0, 0, 1, before_moves));
    mem.W8(DemoStatics, 0);
    assert(!reader.Sample().demo_active);
    mem.W8(Fm + off::FmMenuOpen, 1);
    assert(!reader.SwapMoves(0, 0, 1, before_moves));
    mem.W8(Fm + off::FmMenuOpen, 0);
    mem.W8(Pw + off::PwIsBattling, 1);
    assert(!reader.SwapMoves(0, 0, 1, before_moves));
    mem.W8(Pw + off::PwIsBattling, 0);
    reject_batch = true;
    assert(!reader.SwapMoves(0, 0, 1, before_moves));
    assert(mon().moves == before_moves.moves);
    reject_batch = false;
    assert(reader.SwapMoves(0, 0, 1, before_moves));
    auto after_moves = mon();
    assert(after_moves.moves[0] == 14 && after_moves.moves[1] == 89);
    assert(after_moves.pp[0] == 12 && after_moves.pp[1] == 7);
    assert(after_moves.pp_ups[0] == 3 && after_moves.pp_ups[1] == 1);
    assert(after_moves.hp == before_moves.hp && after_moves.status == before_moves.status);
    assert(lp_pk8::SameIdentity(before_moves, after_moves));
    assert(!reader.SwapMoves(0, 0, 1, before_moves)); // old drag invalidated
    assert(reader.SwapMoves(0, 0, 1, after_moves));
    assert(mon().moves == before_moves.moves && mon().pp == before_moves.pp && mon().pp_ups == before_moves.pp_ups);
    // A native change after decoding can keep the additive checksum unchanged. Comparing
    // only move bytes (or even the checksum) misses it; the complete core must refuse.
    auto native_core_change = [&](bool collide) {
        std::array<u8, lp_pk8::CoreSize> encrypted{}, plain{};
        assert(mem.Read(Core + off::ArrayData, encrypted.data(), encrypted.size()));
        assert(lp_pk8::DecryptRaw(encrypted.data(), plain));
        const u16 checksum = lp_pk8::Rd16(encrypted.data() + 6);
        Put16(plain.data() + 0x58, static_cast<u16>(lp_pk8::Rd16(plain.data() + 0x58) + 1));
        if (collide)
            Put16(plain.data() + 0x5A, static_cast<u16>(lp_pk8::Rd16(plain.data() + 0x5A) - 1));
        lp_pk8::EncryptRaw(plain, encrypted.data());
        assert((lp_pk8::Rd16(encrypted.data() + 6) == checksum) == collide);
        mem.Write(Core + off::ArrayData, encrypted.data(), encrypted.size());
    };
    auto collide_checksum = [&] { native_core_change(true); };
    before_batch = [&] { native_core_change(false); };
    assert(!reader.SwapMoves(0, 0, 1, before_moves));
    assert(mon().moves == before_moves.moves && count() == 2); // checksum remains valid
    before_batch = collide_checksum;
    assert(!reader.SwapMoves(0, 0, 1, before_moves));
    assert(mon().moves == before_moves.moves && count() == 2);
    fixture(50, 0, 0);
    before_batch = collide_checksum;
    assert(!reader.EditParty({{0, [](auto& p, const auto&) {
        Put16(p.data() + 0x8A, 70); return true;
    }}}, 17));
    assert(mon().hp == 50 && count() == 2);

    // Compact party-wide spans keep a full six-member revive inside the 16-op limit.
    // Distinct objects ensure every member is independently edited and checksummed.
    fixture(0, 5, 0);
    std::array<u8, lp_pk8::CoreSize> shared_core{};
    std::array<u8, lp_pk8::CalcSize> shared_calc{};
    assert(mem.Read(Core + off::ArrayData, shared_core.data(), shared_core.size()));
    assert(mem.Read(Calc + off::ArrayData, shared_calc.data(), shared_calc.size()));
    std::vector<std::pair<int, Reader::MonEdit>> revives;
    for (int i = 0; i < 6; ++i) {
        const u64 param = i == 0 ? Param : 0x300000 + static_cast<u64>(i) * 0x10000;
        const u64 core = i == 0 ? Core : param + 0x1000;
        const u64 calc = i == 0 ? Calc : param + 0x2000;
        mem.W64(Members + off::ArrayData + 8 * i, param);
        mem.W64(param + off::ParamCore, core);
        mem.W64(param + off::ParamCalc, calc);
        mem.W64(core + off::ArrayLength, lp_pk8::CoreSize);
        mem.W64(calc + off::ArrayLength, lp_pk8::CalcSize);
        mem.Write(core + off::ArrayData, shared_core.data(), shared_core.size());
        mem.Write(calc + off::ArrayData, shared_calc.data(), shared_calc.size());
        revives.emplace_back(i, [](auto& p, const auto& m) {
            if (m.hp != 0) return false;
            Put16(p.data() + 0x8A, static_cast<u16>(m.hp_max));
            Put32(p.data() + 0x94, 0); return true;
        });
    }
    mem.W32(Party + off::PartyCount, 6);
    before_batch = collide_checksum;
    assert(!reader.EditParty({revives[0], revives[1]}, 17) && count() == 2);
    const auto refused = reader.Sample();
    assert(refused.party[0].mon.hp == 0 && refused.party[1].mon.hp == 0);
    assert(reader.EditParty(revives, 17) && count() == 1);
    const auto revived = reader.Sample();
    assert(revived.party_count == 6);
    for (const auto& member : revived.party)
        assert(member.valid && member.mon.hp == 101 && member.mon.status == 0);
    mem.W32(Party + off::PartyCount, 1);
    fixture(50, 0, 0);
    mem.W64(Members + off::ArrayLength, 0);
    assert(reader.UseMedicine(0,17,20,0,0,{},0) == 0 && count() == 2);
    mem.W64(Members + off::ArrayLength, 6);
    mem.W64(Bag + off::ArrayLength, 4097);
    assert(reader.UseMedicine(0,17,20,0,0,{},0) == 0 && mon().hp == 50 && count() == 2);
    mem.W64(Bag + off::ArrayLength, 200);
    mem.W32(Bag+off::ArrayData+17*off::SaveItemSize, 0x80000000u);
    assert(reader.UseMedicine(0,17,20,0,0,{},0) == 0 && mon().hp == 50);
    fixture(50, 0, 0);
    // Repel: starts only when none is active, takes one
    mem.W32(Bag+off::ArrayData+79*off::SaveItemSize, 3);
    mem.bytes[Pw+off::PwSprayCount] = mem.bytes[Pw+off::PwSprayCount+1] = mem.bytes[Pw+off::PwSprayType] = 0;
    assert(reader.UseRepel(79, 20, 1));
    { std::int16_t units = 0; mem.Read(Pw+off::PwSprayCount, &units, 2); assert(units == 20 && mem.bytes[Pw+off::PwSprayType] == 1); }
    assert(!reader.UseRepel(79, 20, 1)); // still active
    { u32 n = 0; mem.Read(Bag+off::ArrayData+79*off::SaveItemSize, &n, 4); assert(n == 2); }
    // one count reads the same as the whole array; out of range or negative reads 0
    for (u64 i = 0; i < 200 * off::SaveItemSize; ++i)
        if (!mem.bytes.contains(Bag + off::ArrayData + i))
            mem.W8(Bag + off::ArrayData + i, 0);
    mem.W32(Bag+off::ArrayData+5*off::SaveItemSize, 0xFFFFFFFFu);
    {
        const auto counts = reader.BagCounts();
        assert(counts.size() == 200 && counts[79] == 2 && counts[5] == 0);
        for (int i = 0; i < 200; ++i)
            assert(reader.BagCount(i) == counts[static_cast<std::size_t>(i)]);
        assert(reader.BagCount(200) == 0 && reader.BagCount(-1) == 0);
    }
}

void TestFieldControls() {
    using namespace lp_live;
    constexpr u64 Main=0x80000000, PwK=0x100000, PwS=0x110000, Pw=0x120000;
    constexpr u64 FmK=0x130000, FmS=0x140000, Fm=0x150000;
    constexpr u64 Wk=0x160000, Ws=0x170000, Watch=0x180000, Items=0x190000;
    FakeMemory mem;
    for (const auto& f : off::Fingerprints) mem.W64(Main+f.slot,Main+f.target);
    mem.W64(Main+off::PlayerWorkTypeInfo,PwK);
    mem.W64(PwK+off::KlassStatics,PwS);mem.W64(PwS+off::PlayerWorkInstance,Pw);
    mem.W8(Pw+off::PwIsBattling,0);
    mem.W8(Pw+off::PwFashion,9);
    mem.W8(Pw+off::PwPlayerSex,1);
    mem.W64(Main+off::FieldManagerTypeInfo,FmK);
    mem.W64(FmK+off::KlassStatics,FmS);mem.W64(FmS,Fm);
    mem.W8(Fm+off::FmInit,1);mem.W8(Fm+off::FmMenuOpen,0);
    mem.W64(Main+off::PoketchWindowTypeInfo,Wk);
    mem.W64(Wk+off::KlassStatics,Ws);mem.W64(Ws,Watch);mem.W64(Watch,Wk);
    mem.W8(Watch+0x1A0,0);mem.W8(Watch+0x138,0);mem.W64(Watch+0x148,0);
    mem.W64(Pw+off::PwTopMenu,Items);mem.W64(Items+off::ArrayLength,8);
    mem.W32(Pw+off::PwTopMenu+8,4);
    constexpr u64 Flags=0x1A0000;
    mem.W64(Pw+off::PwEventFlags,Flags);mem.W64(Flags+off::ArrayLength,512);
    mem.W8(Flags+off::ArrayData+off::FlagTownMap,1);
    mem.W8(Flags+off::ArrayData+off::FlagPokedex,1);
    mem.W8(Flags+off::ArrayData+off::FlagPoketch,1);
    constexpr u64 MapWork=0x1B0000,MapSys=0x1C0000;
    mem.W64(Pw+off::PwWorks,MapWork);mem.W64(MapWork+off::ArrayLength,500);
    mem.W64(Pw+off::PwSysFlags,MapSys);mem.W64(MapSys+off::ArrayLength,1000);
    for (int i=0;i<500;++i) mem.W32(MapWork+off::ArrayData+4*i,0);
    for (int i=0;i<1000;++i) mem.W8(MapSys+off::ArrayData+i,0);
    mem.W32(MapWork+off::ArrayData+278*4,1);mem.W8(MapSys+off::ArrayData+134,1);
    bool reject=false;
    Guest g;
    g.main_base=Main;
    g.read=[&](u64 a,void* o,size_t n){return mem.Read(a,o,n);};
    g.batch=[&](const std::vector<BatchOp>& ops){
        if(reject) return false;
        for(const auto& op:ops) {
            std::vector<u8> bytes(op.expect.size());
            if(!mem.Read(op.addr,bytes.data(),bytes.size()) || bytes!=op.expect) return false;
        }
        for(const auto& op:ops) mem.Write(op.addr,op.value.data(),op.value.size());
        return true;
    };
    Reader r(g);assert(r.Resolve(0));
    assert(r.Sample().fashion==9 && r.Sample().map_acquired && r.Sample().player_sex);
    mem.W8(Pw+off::PwPlayerSex,0);
    assert(!r.Sample().player_sex);
    constexpr u64 Rival = 0x1F0000, Player = 0x1F1000;
    auto name = [&](u64 object, std::u16string_view text) {
        mem.W32(object+0x10,text.size());
        for (size_t i=0;i<text.size();++i) mem.W16(object+0x14+2*i,text[i]);
    };
    mem.W64(Pw+off::PwRivalName,Rival);mem.W64(Pw+off::PwUserName,Player);
    name(Rival,u"Renée");name(Player,u"ヒカリ");
    assert(r.Sample().rival_name=="Renée" && r.Sample().player_name=="ヒカリ");
    name(Rival,u"ABCDEFGHIJKL");assert(r.Sample().rival_name=="ABCDEFGHIJKL");
    mem.W32(Rival+0x10,13);assert(r.Sample().rival_name.empty());
    mem.W32(Rival+0x10,0xFFFFFFFF);assert(r.Sample().rival_name.empty());
    name(Rival,u"Barry");mem.bytes.erase(Rival+0x14+8);assert(r.Sample().rival_name.empty());
    name(Rival,u"B\n");assert(r.Sample().rival_name.empty());
    name(Rival,std::u16string{char16_t(0xD800)});assert(r.Sample().rival_name.empty());
    name(Rival,std::u16string{char16_t(0xDC00)});assert(r.Sample().rival_name.empty());
    name(Rival,u"\U0001F600");assert(r.Sample().rival_name=="😀");
    mem.W64(Pw+off::PwRivalName,0);assert(r.Sample().rival_name.empty());
    assert(lp_strings::MapGuide("Go meet {rival}!", "Renée", "Dawn")=="Go meet Renée!");
    assert(lp_strings::MapGuide("Go check on {rival}.", "Barry", "Dawn")=="Go check on Barry.");
    assert(lp_strings::MapGuide("Meet {supporter}.", "Barry", "ヒカリ")=="Meet ヒカリ.");
    assert(lp_strings::MapGuide("Go meet {rival}!", "", "Dawn").empty());
    assert(lp_strings::MapGuide("Meet {supporter}.", "Barry", "").empty());
    assert(lp_strings::MapGuide("Go to the lake.", "", "")=="Go to the lake.");

    assert((lp_vanilla::MapHead(9, 3, false, false) == std::pair{9, 3}));
    assert((lp_vanilla::MapHead(109, 2, false, true) == std::pair{109, 2}));
    assert((lp_vanilla::MapHead(9, 3, true, true) == std::pair{0, 0}));
    assert((lp_vanilla::MapHead(9, 3, true, false) == std::pair{100, 0}));
    constexpr u64 Dex=0x1D0000;
    mem.W64(Pw+off::PwZukan,Dex);mem.W64(Dex+off::ArrayLength,2);
    // Packed LP records span a word boundary; species numbering starts at one.
    mem.W32(Dex+off::ArrayData,0x32000002);mem.W32(Dex+off::ArrayData+4,3);
    auto dex=r.DexStatuses(true);
    assert(dex.size()==17 && dex[1]==2 && dex[7]==2 && dex[8]==3 && dex[9]==3);
    mem.W32(Dex+off::ArrayData+4,4);assert(r.DexStatuses(true).empty());
    mem.W32(Dex+off::ArrayData,2);mem.W32(Dex+off::ArrayData+4,3);
    dex=r.DexStatuses(false);assert(dex.size()==3 && dex[1]==2 && dex[2]==3);
    mem.W64(Dex+off::ArrayLength,2049);assert(r.DexStatuses(true).empty());
    mem.W64(Dex+off::ArrayLength,0);assert(r.DexStatuses(true).empty());

    assert(r.Sample().map_work[278]==1 && r.Sample().map_sys[134]==1);
    mem.W64(MapWork+off::ArrayLength,9000);mem.W64(MapSys+off::ArrayLength,999);
    assert(r.Sample().map_work[278]==0 && r.Sample().map_sys[134]==0);
    mem.W64(MapWork+off::ArrayLength,500);mem.W64(MapSys+off::ArrayLength,1000);

    mem.W8(Flags+off::ArrayData+off::FlagTownMap,0);
    assert(!r.Sample().map_acquired);
    mem.W8(Flags+off::ArrayData+off::FlagTownMap,1);
    std::array<int32_t,500> work{}; std::array<u8,1000> sys{};
    lp_map::PlayerLocation cursor;
    using Location = lp_map::PlayerLocation::Location;
    const Location city{10, {5, 6}}, room{20, {1, 2}}, next{30, {7, 8}};
    assert(cursor.Update(1, true, false, city, {}).value().cell == city.cell);
    assert(cursor.Update(1, true, true, room, city).value().cell == city.cell);
    assert(cursor.Update(1, true, true, room, {}).value().cell == city.cell);
    assert(cursor.Update(1, true, false, {}, {}).value().cell == city.cell);
    assert(cursor.Update(1, true, false, next, city).value().cell == next.cell);
    assert(!cursor.Update(2, true, true, room, {})); // another save must not inherit a marker
    assert(cursor.Update(2, true, true, room, city).value().zone == city.zone); // cold boot indoors
    assert(!cursor.Update(2, false, false, city, city)); // acquisition gate
    assert(!cursor.Update(0, true, false, city, city)); // missing save owner
    assert(lp_map::Visible(500,work));assert(!lp_map::Visible(-1,work));
    assert(!lp_map::Visible(278,work));work[278]=1;assert(lp_map::Visible(278,work));
    assert(lp_map::Arrived(278,1000,false,work,sys));
    assert(!lp_map::Arrived(500,134,true,work,sys));sys[134]=1;
    assert(lp_map::Arrived(500,134,true,work,sys));work[278]=0;
    assert(!lp_map::Arrived(278,134,true,work,sys));
    // The native guide reads work 249, and unavailable work must not invent a goal.
    mem.W32(MapWork + off::ArrayData + 4 * 249, 1400);
    assert(r.Sample().map_guide == 1400);
    mem.W64(MapWork + off::ArrayLength, 10);
    assert(r.Sample().map_guide == -1);
    mem.W64(MapWork + off::ArrayLength, 500);
    mem.W8(Flags+off::ArrayData+off::FlagPokedex,0);
    mem.W8(Flags+off::ArrayData+off::FlagPoketch,0);
    assert(!r.Sample().pokedex_acquired && !r.Sample().poketch_acquired);
    assert(!r.SelectFieldMenu(0));
    assert(r.SelectFieldMenu(2)); // Bag is native-accessible before receiving the two devices
    mem.W8(Flags+off::ArrayData+off::FlagPokedex,1);
    mem.W8(Flags+off::ArrayData+off::FlagPoketch,1);
    assert(r.SelectFieldMenu(0));
    int type=-1;mem.Read(Pw+off::PwTopMenu+8,&type,4);assert(type==0);
    assert(r.SelectFieldMenu(2));assert(!r.SelectFieldMenu(1));
    reject=true;assert(!r.SelectFieldMenu(0));reject=false;
    mem.Read(Pw+off::PwTopMenu+8,&type,4);assert(type==2);
    mem.W8(Fm+off::FmMenuOpen,1);assert(!r.SelectFieldMenu(0));mem.W8(Fm+off::FmMenuOpen,0);
    mem.W8(Watch+0x1A0,1);assert(!r.SelectFieldMenu(0));
    mem.W8(Watch+0x1A0,0);
    mem.W64(Items+off::ArrayLength,100);assert(!r.SelectFieldMenu(0));
    mem.W64(Items+off::ArrayLength,8);
    mem.W8(Pw+off::PwIsBattling,1);
    assert(!r.SelectFieldMenu(0));
    mem.W8(Pw+off::PwIsBattling,0);mem.W8(Fm+off::FmInit,0);
    assert(!r.SelectFieldMenu(0));
}

// The save's language (PlayerWork + 0xAC msg_lang_id, + 0xB0 is_kanji) through the reader.
void TestLanguageRead() {
    using namespace lp_live;
    constexpr u64 Main = 0x80000000, PwK = 0x100000, PwS = 0x110000, Pw = 0x120000;
    FakeMemory mem;
    for (const auto& f : off::Fingerprints)
        mem.W64(Main + f.slot, Main + f.target);
    mem.W64(Main + off::PlayerWorkTypeInfo, PwK);
    mem.W64(PwK + off::KlassStatics, PwS);
    mem.W64(PwS + off::PlayerWorkInstance, Pw);
    mem.W8(Pw + off::PwIsBattling, 0);
    Guest g;
    g.main_base = Main;
    g.read = [&](u64 a, void* o, size_t n) { return mem.Read(a, o, n); };
    Reader r(g);
    assert(r.Resolve(0));
    static_assert(off::PwMsgLangId == 0xAC && off::PwIsKanji == 0xB0);
    assert(r.Sample().msg_lang_id == 0); // unreadable
    for (const int id : {1, 2, 3, 4, 5, 7, 8, 9, 10}) {
        mem.W32(Pw + off::PwMsgLangId, static_cast<std::uint32_t>(id));
        assert(r.Sample().msg_lang_id == id);
    }
    for (const int bad : {0, 6, 11, -1, 0x101}) { // 6 is unused; anything else is not a language
        mem.W32(Pw + off::PwMsgLangId, static_cast<std::uint32_t>(bad));
        assert(r.Sample().msg_lang_id == 0);
    }
    mem.W32(Pw + off::PwMsgLangId, 1);
    mem.W8(Pw + off::PwIsKanji, 0);
    assert(!r.Sample().is_kanji);
    mem.W8(Pw + off::PwIsKanji, 1);
    assert(r.Sample().is_kanji && r.Sample().msg_lang_id == 1);
    mem.W64(PwS + off::PlayerWorkInstance, 0); // no PlayerWork (boot, title before a save)
    assert(r.Sample().msg_lang_id == 0 && !r.Sample().player_ok);
}

// Slot -> bundles, and the variant from the slot's text (contracts/language-mods.md §7).
void TestLanguageDetect() {
    using lp_lang::Variant;
    assert(lp_lang::Lookup(2).bundle == "english" && lp_lang::Lookup(2).sfx == "en" && !lp_lang::Lookup(2).cjk);
    assert(lp_lang::Lookup(3).prefix == "french" && lp_lang::Lookup(5).sfx == "ge" && lp_lang::Lookup(7).sfx == "sp");
    assert(lp_lang::Lookup(8).face == "nintendo_udsg-r_ko_003" && lp_lang::Lookup(8).font_bundle == "kor_font");
    assert(lp_lang::Lookup(9).face == "nintendo_udsg-r_org_zh-cn_003" && lp_lang::Lookup(9).sfx == "si");
    assert(lp_lang::Lookup(10).face == "nintendo_udjxh-db_zh-tw_003" && lp_lang::Lookup(10).cjk);
    assert(lp_lang::Lookup(1).bundle == "jpn" && lp_lang::Lookup(1, true).bundle == "jpn_kanji");
    assert(lp_lang::Lookup(6).id == 2 && lp_lang::Lookup(0).bundle == "english" && lp_lang::Lookup(42).id == 2);
    assert(lp_lang::Lookup(3).face == lp_lang::Lookup(2).face); // EFIGS share the Latin face
    assert(!lp_lang::Valid(0) && !lp_lang::Valid(6) && !lp_lang::Valid(11) && lp_lang::Valid(7) && lp_lang::Valid(10));

    const std::string_view stock = "Only English is available in the base mod.Translations may appear on Nexus Mods.";
    // slot 2: LP English, or PT-BR (same language-select text, translated types)
    auto d = lp_lang::Detect(2, stock, "Tackle", "Fighting");
    assert(d.variant == Variant::En && !d.modded);
    d = lp_lang::Detect(2, stock, "Tackle", "Lutador");
    assert(d.variant == Variant::PtBR && d.modded);
    // slot 7: Castellano or Latin American Spanish
    const std::string_view es = "Gracias por descargar el parche en español";
    d = lp_lang::Detect(7, es, "Placaje", "Lucha");
    assert(d.variant == Variant::EsES && d.modded);
    d = lp_lang::Detect(7, es, "Tacleada", "Pelea");
    assert(d.variant == Variant::Es419 && d.modded);
    d = lp_lang::Detect(7, stock, "Placaje", "Lucha"); // LP's own Spanish (no mod)
    assert(d.variant == Variant::EsES && !d.modded);
    // the other slots by id; the select text tells a mod from LP's stock bundle
    d = lp_lang::Detect(3, "Le patch Français est disponible pour ce mod.", "Charge", "Combat");
    assert(d.variant == Variant::Fr && d.modded);
    assert(!lp_lang::Detect(3, stock, "Charge", "Combat").modded);
    assert(lp_lang::Detect(5, "If your preferred language is crossed out", "Tackle", "Kampf").modded); // de mod
    assert(lp_lang::Detect(8, "어떤 언어로 플레이하겠습니까", "", "").variant == Variant::Ko);
    assert(lp_lang::Detect(9, "该mod的官方语言仅支持英文", "", "").variant == Variant::ZhHans);
    assert(lp_lang::Detect(10, "基本模組中只有英文版", "", "").variant == Variant::ZhHant);
    assert(lp_lang::Detect(1, "", "", "").variant == Variant::Ja && lp_lang::Detect(4, "", "", "").variant == Variant::It);
    assert(!lp_lang::Detect(8, "", "", "").modded); // missing table: not a mod
    // the published codes
    assert(lp_lang::Code(Variant::En) == "en" && lp_lang::Code(Variant::PtBR) == "pt-BR");
    assert(lp_lang::Code(Variant::EsES) == "es-ES" && lp_lang::Code(Variant::Es419) == "es-419");
    assert(lp_lang::Code(Variant::ZhHans) == "zh-Hans" && lp_lang::Code(Variant::ZhHant) == "zh-Hant");
    assert(lp_lang::Code(Variant::Ko) == "ko" && lp_lang::Code(Variant::Ja) == "ja" && lp_lang::Code(Variant::It) == "it");
}

// Label lookup by name: the language, else English (missing or empty), else "".
void TestLabelLookup() {
    lp_lang::LabelTables t;
    t.lang["ss_btl_app"] = {{"msg_ui_btl_00", "Attaque"}, {"msg_ui_btl_01", ""}};
    t.lang["ss_bag_pocket"] = {{"SS_bag_pocket_001", "Soins"}};
    t.english["ss_btl_app"] = {{"msg_ui_btl_00", "Battle"}, {"msg_ui_btl_01", "Bag"}, {"msg_ui_btl_04", "Run"}};
    t.english["ss_typename"] = {{"TYPENAME_001", "Fighting"}};
    assert(t.Get("ss_btl_app", "msg_ui_btl_00") == "Attaque");
    assert(t.Get("ss_btl_app", "msg_ui_btl_01") == "Bag");      // empty in the language
    assert(t.Get("ss_btl_app", "msg_ui_btl_04") == "Run");      // missing in the language
    assert(t.Get("ss_typename", "TYPENAME_001") == "Fighting"); // a table the language lacks
    assert(t.Get("ss_bag_pocket", "SS_bag_pocket_001") == "Soins");
    assert(t.Get("ss_btl_app", "nope").empty() && t.Get("nope", "msg_ui_btl_00").empty());
    // the catalog keeps these tables; the common_msbt ones are marked
    const auto has = [](std::string_view name, bool common) {
        for (const auto& d : lp_lang::LabelTableList)
            if (d.table == name)
                return d.common == common;
        return false;
    };
    assert(has("ss_btl_app", false) && has("ss_language_select", false) && has("dp_poketch", false));
    assert(has("ss_pokedex", true) && has("ss_monsname", true));
}

// Description line breaks: a space between words, nothing inside Chinese / Japanese text, and a
// kept break after a sentence end of either script.
void TestReflow() {
    using lp_lang::Reflow;
    assert(Reflow("A strong electric blast\ncrashes down.\nThis may paralyze.") ==
           "A strong electric blast crashes down.\nThis may paralyze.");
    assert(Reflow("trailing  \nspace") == "trailing space");
    assert(Reflow("Ends here!\n") == "Ends here!\n");
    // Chinese: no space at a soft break, the break stays after 。
    assert(Reflow("背上的甲壳是由泥土\n形成的。\n喝水之后会变得更硬。") == "背上的甲壳是由泥土形成的。\n喝水之后会变得更硬。");
    assert(Reflow("十万伏特\nPokémon") == "十万伏特Pokémon"); // a Han side is enough
    assert(Reflow("つちで　できた\nこうらは") == "つちで　できたこうらは");
    assert(Reflow("強力的電擊！\n有時會") == "強力的電擊！\n有時會"); // full-width !
    // Korean puts spaces between words
    assert(Reflow("흙으로 만들어진\n등껍질은") == "흙으로 만들어진 등껍질은");
    assert(Reflow("물을 마시면 단단해진다.\n호숫가에") == "물을 마시면 단단해진다.\n호숫가에");
    // the index-table form
    const auto v = Reflow(std::vector<std::string>{"a\nb", "猫\n犬"});
    assert(v[0] == "a b" && v[1] == "猫犬");

    lp_lang::CodepointSet s;
    s.AddText("Aé猫");
    assert(s.Has('A') && s.Has(0xE9) && s.Has(0x732B) && !s.Has('B') && s.Size() == 3);
    assert(!s.Add('A') && !s.Add('\n') && !s.Add(0x1F600)); // known, control, past the BMP
    const auto miss = s.Missing("Aé犬犬B");
    assert(miss.size() == 2 && miss[0] == 0x72AC && miss[1] == 'B');
    assert((s.Sorted() == std::vector<std::uint32_t>{'A', 0xE9, 0x732B}));
}

// lp_text measures Korean / Chinese exactly as the runtime lays it out: advances scaled by
// 5 * scale / line_height, a break only at ASCII spaces, and a space-less run broken by character.
void TestCjkMeasure() {
    lp_assets::Font f;
    f.line_height = 40;
    f.first_codepoint = 0x20;
    f.glyphs.assign(0xAC01 - 0x20, EdenDsmodFontGlyph{});
    f.glyphs[' ' - 0x20].advance = 12;
    f.glyphs['A' - 0x20].advance = 28;
    f.glyphs[0x732B - 0x20].advance = 48; // 猫, a full-width cell
    f.glyphs[0xAC00 - 0x20].advance = 44; // 가
    const lp_text::Measure m{f};
    // scale 4: 20 / 40 per unit
    assert(m.Width("猫", 4) == 24 && m.Width("猫猫猫", 4) == 72 && m.Width("가 A", 4) == 22 + 6 + 14);
    assert(m.Width("猫\n猫", 4) == 48); // a newline has no width
    assert(m.Width("\xF0\x9F\x98\x80", 4) == 10); // past the glyph run: half a line (5 * 4 / 2)
    // ten 猫 (240 px) in 100 px: no spaces, so by character, four per line -> 3 lines
    const std::string ten = "猫猫猫猫猫猫猫猫猫猫";
    assert(m.Lines(ten, 4, 100) == 3);
    assert(m.Height(ten, 4, 100) == 2 * 32 + 20);
    // Korean words wrap at the spaces between them
    assert(m.Lines("가가 가가 가가", 4, 100) == 2 && m.Lines("가가 가가", 4, 100) == 1);
    // colour tags take no room
    assert(m.Width("{c:#FFFF0000}猫{/c}", 4) == 24);
}

// Battle-time battler state (stages, ability, stats, status, types) and the field block over the
// BattleViewCore -> ViewSystem -> BattleEnv chain.
void TestBattleDetails() {
    using namespace lp_live;
    static_assert(StageStat(100, 6) == 100 && StageStat(100, 12) == 400 && StageStat(100, 0) == 25);
    static_assert(StageStat(101, 7) == 151 && StageStat(100, 5) == 66 && StageStat(100, 13) == -1);
    constexpr u64 Main = 0x80000000, Method = 0x100000, Klass = 0x101000, Statics = 0x102000, Bvc = 0x103000;
    constexpr u64 Vs = 0x104000, Env = 0x105000, Pokecon = 0x106000, Parties = 0x107000;
    constexpr u64 Fs = 0x108000, Data = 0x109000, Cont = 0x10A000, Count = 0x10B000, Enable = 0x10C000;
    constexpr u64 Counter = 0x10D000, Values = 0x10E000;
    FakeMemory mem;
    for (const auto& f : off::Fingerprints) mem.W64(Main + f.slot, Main + f.target);
    mem.W64(Main + off::BattleViewCoreGetInstance, Method);
    mem.W64(Method + off::MethodKlass, Klass);
    mem.W64(Klass + off::KlassStatics, Statics);
    mem.W64(Statics, Bvc);
    mem.W64(Bvc + off::BvcUiSystem, 0);
    mem.W64(Bvc + off::BvcViewSystem, Vs);
    mem.W64(Vs + off::ViewSystemEnv, Env);
    mem.W64(Env + off::EnvPokecon, Pokecon);
    mem.W64(Pokecon + off::PokeconParty, Parties);
    // one battler per client: party -> members[0] -> bpp {core, base, vary, contFlag}
    auto battler = [&](int client, u16 species, u16 tokusei, std::array<u8, 7> ranks, std::array<u16, 5> stats,
                       std::array<u8, 3> types) {
        const u64 b = 0x200000 + 0x10000 * static_cast<u64>(client);
        const u64 party = b, members = b + 0x1000, bpp = b + 0x2000, core = b + 0x3000, base = b + 0x4000;
        const u64 vary = b + 0x5000, sick = b + 0x6000, flags = b + 0x7000;
        mem.W64(Parties + off::ArrayData + 8 * static_cast<u64>(client), party);
        mem.W64(party + off::BtlPartyMembers, members);
        mem.W8(party + off::BtlPartyCount, 1);
        mem.W64(members + off::ArrayData, bpp);
        mem.W64(bpp + off::BppCore, core);
        mem.W64(bpp + off::BppWaza, 0);
        mem.W8(bpp + off::BppWazaCount, 0);
        mem.W16(core + off::CoreMonsNo, species);
        mem.W16(core + off::CoreMonsNo + 2, 0);
        mem.W16(core + off::CoreMonsNo + 4, 100);
        mem.W16(core + off::CoreMonsNo + 6, 80);
        for (int i = 8; i < 0x10; ++i) mem.W8(core + off::CoreMonsNo + i, 0);
        mem.W8(core + off::CoreMonsNo + 0xE, 50);
        mem.W16(bpp + off::BppTokusei, tokusei);
        mem.W64(bpp + off::BppVary, vary);
        for (int i = 0; i < 7; ++i) mem.W8(vary + off::VaryRanks + i, ranks[i]);
        mem.W64(bpp + off::BppBase, base);
        for (int i = 0; i < 5; ++i) mem.W16(base + off::BaseStats + 2 * i, stats[i]);
        for (int i = 0; i < 3; ++i) mem.W8(base + off::BaseTypes + i, types[i]);
        mem.W64(core + off::CoreSickCont, sick);
        mem.W64(sick + off::ArrayLength, 36);
        for (int i = 0; i < 36; ++i) mem.W64(sick + off::ArrayData + 8 * i, 0);
        mem.W64(bpp + off::BppContFlag, flags);
        mem.W64(flags + off::ArrayLength, 8);
        for (int i = 0; i < 8; ++i) mem.W8(flags + off::ArrayData + i, 0);
        return std::array<u64, 5>{bpp, sick, flags, vary, base};
    };
    const auto own = battler(0, 398, 22, {8, 6, 6, 4, 12, 6, 0}, {120, 80, 60, 70, 101}, {0, 2, 18});
    const auto foe = battler(1, 445, 24, {6, 6, 6, 6, 6, 6, 6}, {150, 100, 90, 90, 110}, {15, 4, 18});
    Guest g;
    g.main_base = Main;
    g.read = [&](u64 a, void* o, std::size_t n) { return mem.Read(a, o, n); };
    Reader r{g};
    assert(r.Resolve(0));

    // no FieldStatus / counter yet: field stays default, battlers still read
    auto s = r.Sample();
    assert(s.in_battle && !s.battle_field.valid && s.battle_field.weather == -1 && s.battle_field.turn == -1);
    const auto& a = s.front[0];
    assert(a.valid && a.species == 398 && a.ability == 22 && a.stages_ok);
    assert(a.stages[0] == 2 && a.stages[1] == 0 && a.stages[3] == -2 && a.stages[4] == 6 && a.stages[6] == -6);
    assert(a.stats[0] == 120 && a.stats[4] == 101);
    assert(a.staged[0] == 240 && a.staged[1] == 80 && a.staged[3] == 35 && a.staged[4] == 404);
    assert(a.status == 0 && !a.toxic && !a.confused);
    assert(a.types_raw[0] == 0 && a.types_raw[1] == 2 && a.types_raw[2] == 18);
    assert(a.types[0] == 0 && a.types[1] == 2);
    assert(s.front[1].valid && s.front[1].ability == 24 && s.front[1].stages[0] == 0 &&
           s.front[1].types[0] == 15 && s.front[1].types[1] == 4);
    assert(s.own_count == 1 && s.own[0].ability == 22);
    assert(s.foe_count == 1 && s.foe[0].species == 445 && s.foe[0].valid && !s.foe[1].valid);

    // The move set: two moves read through WAZA_SET -> surface; an array shorter than the count
    // (damaged length) reads none instead of the pointers past it.
    {
        constexpr u64 Sets = 0x3B0000, Set0 = 0x3B1000, Set1 = 0x3B2000, Surf0 = 0x3B3000, Surf1 = 0x3B4000;
        mem.W64(own[0] + off::BppWaza, Sets);
        mem.W8(own[0] + off::BppWazaCount, 2);
        mem.W64(Sets + off::ArrayLength, 4);
        mem.W64(Sets + off::ArrayData, Set0);
        mem.W64(Sets + off::ArrayData + 8, Set1);
        mem.W64(Set0 + off::WazaSetSurface, Surf0);
        mem.W64(Set1 + off::WazaSetSurface, Surf1);
        mem.W64(Surf0 + off::WazaNumber, 0);
        mem.W64(Surf1 + off::WazaNumber, 0);
        mem.W32(Surf0 + off::WazaNumber, 33);
        mem.W8(Surf0 + off::WazaNumber + 4, 35);
        mem.W8(Surf0 + off::WazaNumber + 5, 35);
        mem.W32(Surf1 + off::WazaNumber, 45);
        mem.W8(Surf1 + off::WazaNumber + 4, 12);
        mem.W8(Surf1 + off::WazaNumber + 5, 40);
        auto w = r.Sample().front[0];
        assert(w.waza_count == 2 && w.waza[0] == 33 && w.pp[0] == 35 && w.waza[1] == 45 && w.pp[1] == 12 &&
               w.pp_max[1] == 40 && w.waza[2] == 0);
        mem.W64(Sets + off::ArrayLength, 1);
        w = r.Sample().front[0];
        assert(w.waza_count == 2 && w.waza[0] == 0 && w.waza[1] == 0);
        mem.W64(Sets + off::ArrayLength, 4);
        mem.W64(own[0] + off::BppWaza, 0);
        mem.W8(own[0] + off::BppWazaCount, 0);
    }

    // Real shiny state comes from this exact battler's ppSrc, including an opponent.
    // An Egg never carries a shiny indicator; removing ppSrc must clear both bits.
    {
        constexpr u64 Src = 0x3A0000, CoreArray = 0x3A1000, CalcArray = 0x3A2000;
        // Read the fixture's actual BppCore pointer, without assuming its allocation layout.
        u64 core_addr = 0;
        assert(mem.Read(foe[0] + off::BppCore, &core_addr, sizeof core_addr));
        mem.W64(core_addr + 0x10, Src);
        mem.W64(Src + off::ParamCore, CoreArray);
        mem.W64(Src + off::ParamCalc, CalcArray);
        mem.W64(CoreArray + off::ArrayLength, lp_pk8::CoreSize);
        mem.W64(CalcArray + off::ArrayLength, lp_pk8::CalcSize);
        auto source = [&](bool egg, bool shiny) {
            std::array<u8, lp_pk8::CoreSize> plain{}, enc{};
            std::array<u8, lp_pk8::CalcSize> calc{}, calc_enc{};
            Put32(plain.data(), 0x12345678);
            Put16(plain.data() + 8, 445);
            Put16(plain.data() + 0x0C, 1234);
            Put16(plain.data() + 0x0E, 5678);
            Put32(plain.data() + 0x1C, shiny ? (1234 ^ 5678) : 0x11112222);
            Put32(plain.data() + 0x8C, egg ? 1u << 30 : 0);
            u16 sum = 0;
            for (size_t i = 8; i < plain.size(); i += 2) sum += lp_pk8::Rd16(plain.data() + i);
            Put16(plain.data() + 6, sum);
            Put16(calc.data(), 50); Put16(calc.data() + 2, 100);
            Encrypt(plain.data(), enc.data(), calc.data(), calc_enc.data());
            assert(mem.Write(CoreArray + off::ArrayData, enc.data(), enc.size()));
            assert(mem.Write(CalcArray + off::ArrayData, calc_enc.data(), calc_enc.size()));
        };
        source(false, true);
        s = r.Sample();
        assert(s.front[1].shiny && s.foe[0].shiny && !s.front[1].is_egg);
        source(true, true);
        s = r.Sample();
        assert(s.foe[0].is_egg && !s.foe[0].shiny);
        source(false, false);
        s = r.Sample();
        assert(!s.foe[0].is_egg && !s.foe[0].shiny);
        mem.W64(core_addr + 0x10, 0);
    }

    // the opponent's whole party (a trainer's): members 1 and 2 behind the lead, one fainted
    {
        const u64 fparty = 0x210000, fmembers = fparty + 0x1000;
        auto member = [&](int k, u16 species, u16 hp, u8 level) {
            const u64 bpp = 0x2A0000 + 0x1000 * static_cast<u64>(k), core = bpp + 0x800;
            mem.W64(fmembers + off::ArrayData + 8 * static_cast<u64>(k), bpp);
            mem.W64(bpp + off::BppCore, core);
            mem.W64(bpp + off::BppWaza, 0);
            mem.W8(bpp + off::BppWazaCount, 0);
            for (u64 o : {off::BppBase, off::BppVary, off::BppContFlag}) mem.W64(bpp + o, 0);
            mem.W16(bpp + off::BppTokusei, 0);
            for (int i = 0; i < 0x10; ++i) mem.W8(core + off::CoreMonsNo + i, 0);
            mem.W16(core + off::CoreMonsNo, species);
            mem.W16(core + off::CoreMonsNo + 4, 60);
            mem.W16(core + off::CoreMonsNo + 6, hp);
            mem.W8(core + off::CoreMonsNo + 0xE, level);
            mem.W64(core + off::CoreSickCont, 0);
        };
        member(1, 25, 0, 30);   // Pikachu, fainted
        member(2, 460, 41, 33); // Abomasnow
        mem.W8(fparty + off::BtlPartyCount, 3);
        s = r.Sample();
        assert(s.foe_count == 3 && s.foe[0].species == 445 && s.front[1].species == 445);
        assert(s.foe[1].valid && s.foe[1].species == 25 && s.foe[1].hp == 0 && s.foe[1].hp_max == 60 &&
               s.foe[1].level == 30);
        assert(s.foe[2].valid && s.foe[2].species == 460 && s.foe[2].hp == 41 && s.foe[2].level == 33);
        assert(s.own_count == 1 && !s.own[1].valid); // the sides stay apart
        // a bad member reads invalid without hiding the others; a missing pointer too
        mem.W8(0x2A2000 + 0x800 + off::CoreMonsNo + 0xE, 101); // level 101
        s = r.Sample();
        assert(s.foe_count == 3 && !s.foe[2].valid && s.foe[1].valid);
        mem.W64(fmembers + off::ArrayData + 8 * 2, 0);
        s = r.Sample();
        assert(s.foe_count == 3 && !s.foe[2].valid && s.foe[0].valid);
        // a count out of range (garbage) reads no foe party at all, and no foe lead
        mem.W8(fparty + off::BtlPartyCount, 7);
        s = r.Sample();
        assert(s.foe_count == 0 && !s.foe[0].valid && !s.front[1].valid && s.own_count == 1);
        mem.W8(fparty + off::BtlPartyCount, 0);
        s = r.Sample();
        assert(s.foe_count == 0 && !s.front[1].valid);
        mem.W8(fparty + off::BtlPartyCount, 1);
        s = r.Sample();
        assert(s.foe_count == 1 && s.front[1].species == 445);
    }

    // status: toxic (permanent, count max 15) beats nothing; confusion is entry 6
    mem.W64(own[1] + off::ArrayData + 8 * 5, 0x0F01);
    mem.W64(own[1] + off::ArrayData + 8 * 6, 0x0302);
    s = r.Sample();
    assert(s.front[0].status == 5 && s.front[0].toxic && s.front[0].confused);
    // Volatile effects follow native slots, clear when removed and remain unknown in a short array.
    mem.W64(own[1] + off::ArrayData + 8 * 11, 0x0302); // Taunt
    mem.W64(own[1] + off::ArrayData + 8 * 23, 0x0302); // Encore
    mem.W64(own[1] + off::ArrayData + 8 * 33, 0x0302); // Telekinesis, beyond the old Roost read limit
    s = r.Sample();
    assert(s.front[0].volatile_on[11] && s.front[0].volatile_on[23] && s.front[0].volatile_on[33]);
    assert(!s.front[0].volatile_on[44]); // the fixture has only36 slots
    mem.W64(own[1] + off::ArrayData + 8 * 11, 0);
    s = r.Sample();
    assert(!s.front[0].volatile_on[11] && s.front[0].volatile_on[23]);
    mem.W64(own[1] + off::ArrayData + 8 * 4, 0x0001); // burn wins (lower index first)
    mem.W64(own[1] + off::ArrayData + 8 * 5, 0x0501); // plain poison
    s = r.Sample();
    assert(s.front[0].status == 4 && !s.front[0].toxic);
    // Roost (entry 25) drops Flying: Normal / Flying -> Normal / Normal
    mem.W64(own[1] + off::ArrayData + 8 * 25, 0x0102);
    s = r.Sample();
    assert(s.front[0].types[0] == 0 && s.front[0].types[1] == 0 && s.front[0].types_raw[1] == 2);
    // both types NULL: Normal without Burn Up, typeless with it
    mem.W8(own[4] + off::BaseTypes, 18);
    mem.W8(own[4] + off::BaseTypes + 1, 18);
    s = r.Sample();
    assert(s.front[0].types[0] == 0 && s.front[0].types[1] == 0); // no Burn Up: both NULL -> Normal
    mem.W8(own[2] + off::ArrayData + 2, 0x80);
    s = r.Sample();
    assert(s.front[0].types[0] == 18 && s.front[0].types[1] == 18);
    // short sickCont: no Roost entry read, status still read; too short: status unknown
    mem.W64(own[1] + off::ArrayLength, 7);
    s = r.Sample();
    assert(s.front[0].status == 4);
    assert(!s.front[0].volatile_on[23] && !s.front[0].volatile_on[33]);
    mem.W64(own[1] + off::ArrayLength, 3);
    s = r.Sample();
    assert(s.front[0].status == -1 && !s.front[0].confused && s.front[0].valid);
    assert(HiddenMoveBadge(4, false) == 127 && HiddenMoveBadge(5, false) == 128);
    assert(HiddenMoveBadge(4, true) == 128 && HiddenMoveBadge(5, true) == 127);
    assert(HiddenMoveBadge(3, true) == HiddenMoveBadge(3, false)); // Fly is unaffected
    assert(HiddenMoveBadge(-1, false) == -1 && HiddenMoveBadge(8, true) == -1);
    // bad rank byte / null pointers keep the defaults
    mem.W8(foe[3] + off::VaryRanks + 2, 13);
    mem.W64(foe[0] + off::BppBase, 0);
    s = r.Sample();
    assert(s.front[1].valid && !s.front[1].stages_ok && s.front[1].stages[0] == 0);
    assert(s.front[1].stats[0] == -1 && s.front[1].staged[0] == -1 && s.front[1].types[0] == -1);
    mem.W64(foe[0] + off::BppVary, 0);
    assert(!r.Sample().front[1].stages_ok);

    // field: rain 5 turns with 2 passed, psychic terrain 5 (3 passed), Trick Room 5 (1 passed), turn 4
    mem.W64(Env + off::EnvFieldStatus, Fs);
    mem.W64(Fs + off::FieldStatusData, Data);
    for (int i = 0; i < 0x12; ++i) mem.W8(Data + off::DataWeather + i, 0);
    mem.W8(Data + off::DataWeather, 2);
    mem.W32(Data + 0x14, 5);
    mem.W32(Data + 0x1C, 2);
    mem.W8(Data + off::DataGround, 4);
    mem.W64(Data + off::DataCont, Cont);
    mem.W64(Data + off::DataTurnCount, Count);
    mem.W64(Data + off::DataEnable, Enable);
    for (u64 arr : {Cont, Count, Enable}) mem.W64(arr + off::ArrayLength, off::FieldEffects);
    for (int i = 0; i < off::FieldEffects; ++i) {
        mem.W64(Cont + off::ArrayData + 8 * i, 0);
        mem.W32(Count + off::ArrayData + 4 * i, 0);
        mem.W8(Enable + off::ArrayData + i, 0);
    }
    mem.W64(Cont + off::ArrayData + 8 * EffTrickRoom, 0x0502);
    mem.W32(Count + off::ArrayData + 4 * EffTrickRoom, 1);
    mem.W8(Enable + off::ArrayData + EffTrickRoom, 1);
    mem.W64(Cont + off::ArrayData + 8 * EffGround, 0x0504); // poke-turn cont
    mem.W32(Count + off::ArrayData + 4 * EffGround, 3);
    mem.W8(Enable + off::ArrayData + EffGround, 1);
    mem.W64(Cont + off::ArrayData + 8 * EffGravity, 0x0501); // permanent: no limit
    mem.W8(Enable + off::ArrayData + EffGravity, 1);
    mem.W64(Env + off::EnvCounter, Counter);
    mem.W64(Counter + off::CounterValues, Values);
    mem.W64(Values + off::ArrayLength, 8);
    mem.W64(Values + off::ArrayData, 4);
    auto f = r.Sample().battle_field;
    assert(f.valid && f.weather == 2 && f.weather_turns == 3 && f.terrain == 4 && f.terrain_turns == 2);
    assert(f.effect_on[EffTrickRoom] && f.effect_turns[EffTrickRoom] == 4);
    assert(f.effect_on[EffGravity] && f.effect_turns[EffGravity] == 0);
    assert(!f.effect_on[EffWonderRoom] && f.turn == 4);
    // permanent weather, count past the limit, no weather
    mem.W32(Data + 0x14, 0xFF);
    mem.W32(Count + off::ArrayData + 4 * EffTrickRoom, 9);
    f = r.Sample().battle_field;
    assert(f.weather_turns == 0xFF && f.effect_turns[EffTrickRoom] == 0);
    mem.W8(Data + off::DataWeather, 0);
    assert(r.Sample().battle_field.weather_turns == 0);
    // short effect arrays: weather and terrain still read, effects stay off
    mem.W64(Enable + off::ArrayLength, 4);
    f = r.Sample().battle_field;
    assert(f.valid && f.terrain == 4 && f.terrain_turns == -1 && !f.effect_on[EffTrickRoom]);
    // out-of-range weather byte or a null Data: field invalid, turn still read
    mem.W8(Data + off::DataWeather, 9);
    f = r.Sample().battle_field;
    assert(!f.valid && f.weather == -1 && f.turn == 4);
    mem.W64(Fs + off::FieldStatusData, 0);
    mem.W64(Values + off::ArrayLength, 0);
    f = r.Sample().battle_field;
    assert(!f.valid && f.turn == -1);
}

} // namespace

// The mailbox job runner against a fake stub: one call per "frame", results gate the next step.
// Double battles: the position table (multiMode), view positions, the Pokémon choosing its command
// and the target select, over BattleViewCore -> ViewSystem {MainModule, BTL_CLIENT, BattleEnv}.
void TestDoubleBattle() {
    using namespace lp_live;
    static_assert(PosOwner(0, 0) == 0 && PosOwner(0, 1) == 1 && PosOwner(0, 2) == 0 && PosOwner(0, 3) == 1);
    static_assert(PosOwner(2, 2) == 2 && PosOwner(2, 3) == 3 && PosOwner(5, 3) == 5 && PosOwner(6, 3) == 1);
    static_assert(PosOwner(7, 0) == -1 && PosOwner(0, 4) == -1 && PosOwner(-1, 0) == -1);
    static_assert(PosMemberIndex(0, 2) == 1 && PosMemberIndex(0, 3) == 1 && PosMemberIndex(1, 2) == 0);
    static_assert(PosMemberIndex(1, 3) == 0 && PosMemberIndex(4, 2) == 1 && PosMemberIndex(4, 3) == 0);
    static_assert(PosMemberIndex(6, 3) == 1 && PosMemberIndex(5, 3) == -1);
    static_assert(BtlPosToView(0, 0, 0) == 0 && BtlPosToView(0, 0, 1) == 1 && BtlPosToView(0, 0, 2) == 2 &&
                  BtlPosToView(0, 0, 3) == 3);
    static_assert(BtlPosToView(0, 1, 1) == 0 && BtlPosToView(0, 1, 0) == 1 && BtlPosToView(0, 1, 3) == 2);
    static_assert(BtlPosToView(5, 0, 3) == 255 && BtlPosToView(5, 0, 2) == 2 && BtlPosToView(0, 0, 4) == 255);

    constexpr u64 Main = 0x80000000, Method = 0x100000, Klass = 0x101000, Statics = 0x102000, Bvc = 0x103000;
    constexpr u64 Vs = 0x104000, Env = 0x105000, Pokecon = 0x106000, Parties = 0x107000;
    constexpr u64 MainMod = 0x108000, Setup = 0x109000, Client = 0x10A000, Ui = 0x10B000, Ts = 0x10C000;
    FakeMemory mem;
    for (const auto& f : off::Fingerprints) mem.W64(Main + f.slot, Main + f.target);
    mem.W64(Main + off::BattleViewCoreGetInstance, Method);
    mem.W64(Method + off::MethodKlass, Klass);
    mem.W64(Klass + off::KlassStatics, Statics);
    mem.W64(Statics, Bvc);
    mem.W64(Bvc + off::BvcUiSystem, Ui);
    mem.W64(Bvc + off::BvcViewSystem, Vs);
    mem.W64(Ui + off::UiActionList, 0);
    mem.W64(Ui + off::UiWazaList, 0);
    mem.W64(Ui + off::UiPokeBallList, 0);
    mem.W64(Ui + off::UiTargetSelect, Ts);
    mem.W64(Vs + off::ViewSystemEnv, Env);
    mem.W64(Vs + off::VsMainModule, MainMod);
    mem.W64(Vs + off::VsClient, Client);
    mem.W64(Env + off::EnvPokecon, Pokecon);
    mem.W64(Pokecon + off::PokeconParty, Parties);
    mem.W64(Parties + off::ArrayLength, 4);
    mem.W32(MainMod + off::MainRule, 1);
    mem.W64(MainMod + off::MainSetup, Setup);
    mem.W8(Setup + off::SetupMultiMode, 0);
    mem.W8(MainMod + off::MainMyClient, 0);
    mem.W8(MainMod + off::MainMyOrgPos, 0);
    mem.W8(Client + off::ClientProcPokeIdx, 0);
    // target select: closed (canvas 0x58..0x63, single-target flag)
    mem.W8(Ts + off::CanvasIsTransition, 0);
    mem.W32(Ts + off::CanvasMaxIndex, 3);
    mem.W32(Ts + off::CanvasCurrentIndex, 3);
    mem.W8(Ts + off::CanvasIsFocus, 0);
    mem.W8(Ts + off::CanvasIsShow, 0);
    mem.W8(Ts + off::CanvasIsValid, 0);
    mem.W8(Ts + off::TargetSingle, 1);
    // client parties: members -> bpp -> core (species, hp, level)
    auto party = [&](int client, std::vector<std::pair<u16, u16>> mons) {
        const u64 b = 0x200000 + 0x10000 * static_cast<u64>(client), members = b + 0x100;
        mem.W64(Parties + off::ArrayData + 8 * static_cast<u64>(client), b);
        mem.W64(b + off::BtlPartyMembers, members);
        mem.W8(b + off::BtlPartyCount, static_cast<u8>(mons.size()));
        for (std::size_t k = 0; k < mons.size(); ++k) {
            const u64 bpp = b + 0x1000 * (k + 1), core = bpp + 0x800;
            mem.W64(members + off::ArrayData + 8 * k, bpp);
            mem.W64(bpp + off::BppCore, core);
            mem.W64(bpp + off::BppWaza, 0);
            mem.W8(bpp + off::BppWazaCount, 0);
            for (int i = 0; i < 0x10; ++i) mem.W8(core + off::CoreMonsNo + i, 0);
            mem.W16(core + off::CoreMonsNo, mons[k].first);
            mem.W16(core + off::CoreMonsNo + 4, 200);
            mem.W16(core + off::CoreMonsNo + 6, mons[k].second);
            mem.W8(core + off::CoreMonsNo + 0xE, 50);
            mem.W8(core + off::CoreMyId, static_cast<u8>(client*6+k));
        }
    };
    party(0, {{445, 150}, {491, 120}, {384, 200}});
    party(1, {{81, 40}, {82, 0}});
    party(2, {{395, 99}});
    party(3, {{376, 77}});
    Guest g;
    g.main_base = Main;
    g.read = [&](u64 a, void* o, std::size_t n) { return mem.Read(a, o, n); };
    g.write = [&](u64 a, const void* data, size_t size) { return mem.Write(a, data, size); };
    Reader r{g};
    assert(r.Resolve(0));

    // Replacement suggestions must use the announced native ID, not a fainted front
    // or guessed team order. The announcement is only valid in this exact subprocedure.
    constexpr u64 Menu = 0x330000, Instance = 0x331000, Proc = 0x332000;
    constexpr u64 Message = 0x333000, Args = 0x334000;
    mem.W8(Ui + off::UiMenuEnd, 0);
    mem.W64(Ui + off::UiMenuWindow, Menu);
    mem.W8(Menu + off::WindowClosing, 0);
    mem.W64(Menu + off::WindowInstance, Instance);
    mem.W64(Instance + off::InstanceWindow, Menu);
    mem.W32(Instance + off::InstanceWindowId, off::WindowIdPokemonBattle);
    mem.W64(Client + 0x200, Proc);
    mem.W64(Proc + 0x10, Main + lp_profile::Active.ConfirmIrekae);
    mem.W32(Client + 0x208, 3);
    mem.W64(Client + 0x220, Message);
    mem.W16(Message + 0x10, 22);
    mem.W8(Message + 0x13, 1);
    mem.W8(Message + 0x14, 2);
    mem.W64(Message + 0x18, Args);
    mem.W64(Args + off::ArrayLength, 2);
    mem.W32(Args + off::ArrayData, 1);
    mem.W32(Args + off::ArrayData + 4, 6);
    assert(r.Sample().switch_foe.valid && r.Sample().switch_foe.species == 81);
    mem.W32(Args + off::ArrayData + 4, 7); // fainted reserve
    assert(!r.Sample().switch_foe.valid);
    mem.W32(Args + off::ArrayData + 4, 31); // no native Pokémon
    assert(!r.Sample().switch_foe.valid);
    mem.W32(Args + off::ArrayData + 4, 6);
    mem.W32(Client + 0x208, 2); // announcement not yet waiting for the player
    assert(!r.Sample().switch_foe.valid);
    mem.W32(Client + 0x208, 3);
    mem.W64(Proc + 0x10, Main + lp_profile::Active.ConfirmIrekae + 4);
    assert(!r.Sample().switch_foe.valid);
    mem.W64(Proc + 0x10, Main + lp_profile::Active.ConfirmIrekae);
    mem.W32(Args + off::ArrayData, 0); // own trainer cannot be the announced foe
    assert(!r.Sample().switch_foe.valid);
    mem.W32(Args + off::ArrayData, 1);
    mem.W8(Ui + off::UiMenuEnd, 1);
    assert(!r.Sample().switch_foe.valid); // stale pooled window / finished menu

    // Gender comes from each battler's actual encrypted PokémonParam, including foes.
    constexpr u64 Source = 0x340000, SourceCore = 0x341000, SourceCalc = 0x342000;
    mem.W64(0x210000 + 0x1000 + 0x800 + 0x10, Source);
    mem.W64(Source + off::ParamCore, SourceCore);
    mem.W64(Source + off::ParamCalc, SourceCalc);
    mem.W64(SourceCore + off::ArrayLength, lp_pk8::CoreSize);
    mem.W64(SourceCalc + off::ArrayLength, lp_pk8::CalcSize);
    auto gender_fixture = [&](int gender, bool egg) {
        std::array<u8, lp_pk8::CoreSize> plain{}, enc{};
        std::array<u8, lp_pk8::CalcSize> calc{};
        Put32(plain.data(), 0x627EA09E);
        Put16(plain.data()+8, 81);
        plain[0x22] = static_cast<u8>(gender << 2);
        if (egg) Put32(plain.data()+0x8C, 1u<<30);
        lp_pk8::EncryptRaw(plain, enc.data());
        Put16(calc.data(), 50);
        Put16(calc.data()+2, 200);
        lp_pk8::Crypt(calc.data(), calc.size(), lp_pk8::Rd32(plain.data()));
        mem.Write(SourceCore + off::ArrayData, enc.data(), enc.size());
        mem.Write(SourceCalc + off::ArrayData, calc.data(), calc.size());
    };
    for (int gender : {0, 1, 2}) {
        gender_fixture(gender, false);
        assert(r.Sample().opponents[0].gender == gender);
    }
    gender_fixture(1, true);
    assert(r.Sample().opponents[0].is_egg && r.Sample().opponents[0].gender == 2);
    gender_fixture(1, false);

    // a normal double (multiMode 0): both your front members and both of the foe's
    auto s = r.Sample();
    auto d = s.dbl;
    assert(d.valid && d.is_double && d.rule == 1 && d.multi == 0 && d.cover == 2);
    assert(d.view[0].exists && d.view[0].client == 0 && d.view[0].index == 0 && d.view[0].mon.species == 445);
    assert(d.view[2].client == 0 && d.view[2].index == 1 && d.view[2].mon.species == 491 && d.view[2].btl_pos == 2);
    assert(d.view[1].client == 1 && d.view[1].mon.species == 81 && d.view[1].Present());
    assert(d.view[3].client == 1 && d.view[3].index == 1 && d.view[3].mon.valid && !d.view[3].Present()); // fainted
    assert(d.proc_index == 0 && d.proc_view == 0 && s.front[0].species == 445);
    assert(!d.target_open && d.target_view == -1);

    // the second Pokémon chooses: front[0] follows it; past the last slot nobody chooses
    mem.W8(Client + off::ClientProcPokeIdx, 1);
    s = r.Sample();
    assert(s.dbl.proc_view == 2 && s.dbl.proc_index == 1 && s.front[0].species == 491);
    mem.W8(Client + off::ClientProcPokeIdx, 2);
    s = r.Sample();
    assert(s.dbl.proc_view == -1 && s.dbl.proc_index == -1 && s.front[0].species == 445);

    // the target select: open (show, focus, not submitted) on far 2; submitted; a spread move
    mem.W8(Ts + off::CanvasIsFocus, 1);
    mem.W8(Ts + off::CanvasIsShow, 1);
    d = r.Sample().dbl;
    assert(d.target_open && d.target_single && d.target_view == 3);
    constexpr u64 Buttons = 0x310000;
    mem.W64(Ts + 0x68, Buttons);
    mem.W64(Buttons + off::ArrayLength, 4);
    for (int k = 0; k < 4; ++k) {
        const u64 button = Buttons + 0x100 * (k + 1);
        mem.W64(Buttons + off::ArrayData + 8 * k, button);
        mem.W8(button + 0x98, k != 1); // only far1 is a valid native target
    }
    assert(r.CanTargetInput() && r.CanTargetInput(1));
    assert(!r.CanTargetInput(0) && !r.CanTargetInput(3) && !r.CanTargetInput(-1));
    assert(!r.SetTargetIndex(3) && r.SetTargetIndex(1));
    assert(r.Sample().dbl.target_view == 1 && r.Sample().dbl.target_enabled[1]);
    mem.W8(Ts + off::CanvasIsTransition, 1);
    assert(!r.CanTargetInput() && !r.SetTargetIndex(1));
    mem.W8(Ts + off::CanvasIsTransition, 0);
    mem.W32(Ts + off::CanvasCurrentIndex, 7); // out of range: no target
    assert(r.Sample().dbl.target_view == -1);
    mem.W32(Ts + off::CanvasCurrentIndex, 1);
    mem.W8(Ts + off::TargetSingle, 0);
    d = r.Sample().dbl;
    assert(d.target_open && !d.target_single && d.target_view == -1);
    assert(r.CanTargetInput(-1) && !r.CanTargetInput(1) && !r.SetTargetIndex(1));
    mem.W8(Ts + off::CanvasIsValid, 1);
    assert(!r.Sample().dbl.target_open);
    assert(!r.CanTargetInput() && !r.CanTargetInput(-1));
    // sliding in after a move was picked (not shown, not focused yet): moving, not open
    assert(!r.Sample().dbl.target_moving);
    mem.W8(Ts + off::CanvasIsTransition, 1);
    d = r.Sample().dbl;
    assert(d.target_moving && !d.target_open);
    mem.W8(Ts + off::CanvasIsTransition, 0);

    // a multi battle (1): partner on near 2, enemy 2 on far 2, one position each
    mem.W8(Setup + off::SetupMultiMode, 1);
    mem.W8(Client + off::ClientProcPokeIdx, 0);
    d = r.Sample().dbl;
    assert(d.valid && d.cover == 1 && d.view[2].client == 2 && d.view[2].mon.species == 395);
    assert(d.view[3].client == 3 && d.view[3].mon.species == 376 && d.view[0].mon.species == 445);
    // Terrain lists reserves from BOTH opposing trainers, without duplicating a
    // normal double's shared party or including our partner's Pokemon.
    party(1, {{81, 40}, {82, 0}, {83, 100}});
    party(3, {{376, 77}, {25, 50}});
    s = r.Sample();
    assert(s.opponents.size() == 5);
    assert(s.opponents[0].species == 81 && s.opponents[1].species == 82 && s.opponents[1].hp == 0);
    assert(s.opponents[2].species == 83 && s.opponents[3].species == 376 && s.opponents[4].species == 25);
    mem.W8(Setup + off::SetupMultiMode, 0);
    assert(r.Sample().opponents.size() == 3); // both far positions belong to client 1
    mem.W8(Setup + off::SetupMultiMode, 1);
    party(1, {{81, 40}, {82, 0}, {83, 100}, {84, 100}, {85, 100}, {86, 100}});
    party(3, {{376, 77}, {25, 50}, {26, 100}, {27, 100}, {28, 100}, {29, 100}});
    assert(r.Sample().opponents.size() == 12);
    mem.W8(0x230000 + off::BtlPartyCount, 7);
    assert(r.Sample().opponents.size() == 6); // malformed second trainer is bounded
    party(1, {{81, 40}, {82, 0}});
    party(3, {{376, 77}});
    // the party array too short for clients 2 / 3: their spots exist but stay unread
    mem.W64(Parties + off::ArrayLength, 2);
    d = r.Sample().dbl;
    assert(d.view[2].exists && !d.view[2].mon.valid && !d.view[2].Present() && !d.view[3].mon.valid);
    mem.W64(Parties + off::ArrayLength, 4);
    // PA_A (5): no far 2
    mem.W8(Setup + off::SetupMultiMode, 5);
    d = r.Sample().dbl;
    assert(d.valid && !d.view[3].exists && d.view[1].exists && d.view[2].client == 2);
    // P_AA (4): your two against two trainers
    mem.W8(Setup + off::SetupMultiMode, 4);
    d = r.Sample().dbl;
    assert(d.cover == 2 && d.view[2].mon.species == 491 && d.view[3].mon.species == 376);
    // an unknown multiMode and a single battle: no double state
    mem.W8(Setup + off::SetupMultiMode, 9);
    d = r.Sample().dbl;
    assert(!d.valid && d.is_double);
    mem.W8(Setup + off::SetupMultiMode, 0);
    mem.W32(MainMod + off::MainRule, 0);
    s = r.Sample();
    assert(!s.dbl.valid && !s.dbl.is_double && s.dbl.rule == 0 && s.front[0].species == 445);
    mem.W64(Vs + off::VsMainModule, 0);
    assert(r.Sample().dbl.rule == -1);
}

void TestGuestMailbox() {
    std::map<lp_guest::u32, lp_guest::u64> mb;
    lp_guest::Io io;
    io.address = 0x1000;
    io.load32 = [&](lp_guest::u32 o, lp_guest::u32* v) { *v = static_cast<lp_guest::u32>(mb[o]); return true; };
    io.store32 = [&](lp_guest::u32 o, lp_guest::u32 v) { mb[o] = v; return true; };
    io.load64 = [&](lp_guest::u32 o, lp_guest::u64* v) { *v = mb[o]; return true; };
    io.store64 = [&](lp_guest::u32 o, lp_guest::u64 v) { mb[o] = v; return true; };
    std::vector<std::array<lp_guest::u64, 5>> calls;
    lp_guest::u64 next_result = 0;
    const auto frame = [&] { // what the guest stub does once per field frame
        ++mb[lp_guest::MbHeartbeat];
        if (mb[lp_guest::MbSeq] == mb[lp_guest::MbTaken]) return;
        mb[lp_guest::MbTaken] = mb[lp_guest::MbSeq];
        if (mb[lp_guest::MbFn]) {
            calls.push_back({mb[lp_guest::MbFn], mb[lp_guest::MbArgs], mb[lp_guest::MbArgs + 8],
                             mb[lp_guest::MbArgs + 16], mb[lp_guest::MbArgs + 24]});
            mb[lp_guest::MbResult] = next_result;
        }
        mb[lp_guest::MbDone] = mb[lp_guest::MbSeq];
    };
    lp_guest::Mailbox m;
    m.Configure(io);
    assert(m.Present() && !m.Alive());
    frame();
    m.Poll();
    assert(m.Alive());
    const auto step = [](lp_guest::u64 fn, lp_guest::u64 a0, std::function<bool(lp_guest::u64)> ok) {
        return lp_guest::Step{[fn] { return fn; }, [a0] { return std::array<lp_guest::u64, 4>{a0, 7, 0, 0}; },
                              std::move(ok), "refused"};
    };
    int finished = 0;
    bool result = false;
    std::string why;
    const auto done = [&](bool ok, const std::string& r) { ++finished; result = ok; why = r; };
    // all steps pass: two calls in order, one per frame
    assert(m.Run({step(0x111, 1, lp_guest::False), step(0x222, 2, lp_guest::Any)}, done));
    assert(!m.Run({step(0x333, 3, lp_guest::Any)}, done)); // busy
    next_result = 0x100; // low byte 0 = false
    frame();
    m.Poll();
    assert(calls.size() == 1 && calls[0][0] == 0x111 && calls[0][1] == 1 && calls[0][2] == 7 && finished == 0);
    frame();
    m.Poll();
    assert(calls.size() == 2 && calls[1][0] == 0x222 && finished == 1 && result && !m.Busy());
    // a refused first step stops the job
    next_result = 1;
    assert(m.Run({step(0x444, 4, lp_guest::Zero), step(0x555, 5, lp_guest::Any)}, done));
    frame();
    m.Poll();
    assert(calls.size() == 3 && finished == 2 && !result && why == "refused" && !m.Busy());
    // a step without an address (no instance) is never issued
    assert(!m.Run({step(0, 6, lp_guest::Any)}, done));
    assert(!m.Busy());
    // A timed-out (cancelled, fn = 0) request the stub has not served yet still owns its seq:
    // no new request is written over it until the stub has served it.
    mb[lp_guest::MbFn] = 0;
    mb[lp_guest::MbSeq] = mb[lp_guest::MbDone] + 1;
    assert(!m.Run({step(0x666, 6, lp_guest::Any)}, done) && !m.Busy() && mb[lp_guest::MbFn] == 0);
    frame(); // serves the cancelled seq: nothing runs
    m.Poll();
    assert(calls.size() == 3 && finished == 2);
    assert(m.Run({step(0x666, 6, lp_guest::Any)}, done));
    frame();
    m.Poll();
    assert(calls.size() == 4 && calls[3][0] == 0x666 && calls[3][1] == 6 && finished == 3 && result);
    // A consumable field item (lp_field_actions::ConsumedOnUse) is paid for only after the use ran:
    // the check refusing it, or the use never being served, costs nothing.
    assert(lp_field_actions::ConsumedOnUse(78) && lp_field_actions::ConsumedOnUse(94) &&
           !lp_field_actions::ConsumedOnUse(1) && !lp_field_actions::ConsumedOnUse(443));
    int taken = 0;
    const auto field_use = [&](lp_guest::u64 check) {
        return m.Run({step(0x900, 78, lp_guest::Zero), step(0x901, 78, lp_guest::Any)},
                     [&](bool ok, const std::string& r) {
                         if (ok) ++taken; // what UseFieldItem's job does on success
                         done(ok, r);
                     }) && (next_result = check, true);
    };
    assert(field_use(1)); // UI_onUseFieldItem: not available here
    frame();
    m.Poll();
    assert(!m.Busy() && !result && taken == 0 && calls.back()[0] == 0x900);
    assert(field_use(0)); // available: the use runs on the next frame, then the item is taken
    frame();
    m.Poll();
    assert(m.Busy() && taken == 0 && calls.back()[0] == 0x900);
    frame();
    m.Poll();
    assert(!m.Busy() && result && taken == 1 && calls.back()[0] == 0x901 && calls.back()[1] == 78);
    // a failed result read refuses instead of judging a default 0 as "Available"
    io.load64 = [](lp_guest::u32, lp_guest::u64*) { return false; };
    m.Configure(io);
    assert(m.Run({step(0x777, 7, lp_guest::Zero), step(0x888, 8, lp_guest::Any)}, done));
    frame();
    m.Poll();
    assert(calls.size() == 8 && finished == 6 && !result && !m.Busy());
}

// A small type chart for the switch tests (sixteenths): the pairs the scenarios use, else neutral.
int TestEff(int attack, const std::array<int, 3>& t) {
    enum { Normal = 0, Flying = 2, Ground = 4, Rock = 5, Fire = 9, Water = 10, Grass = 11, Electric = 12 };
    const auto one = [](int a, int d) {
        if (d < 0 || d >= 18)
            return 4;
        if ((a == Electric && d == Ground) || (a == Ground && d == Flying))
            return 0;
        if ((a == Fire && (d == Water || d == Fire || d == Rock)) || (a == Water && (d == Water || d == Grass)) ||
            (a == Grass && (d == Fire || d == Grass || d == Flying)) || (a == Electric && (d == Grass || d == Electric)) ||
            (a == Normal && d == Rock))
            return 2;
        if ((a == Water && (d == Fire || d == Ground || d == Rock)) || (a == Fire && d == Grass) ||
            (a == Grass && (d == Water || d == Ground || d == Rock)) || (a == Electric && (d == Water || d == Flying)) ||
            (a == Ground && (d == Fire || d == Electric || d == Rock)) || (a == Rock && (d == Fire || d == Flying)))
            return 8;
        return 4;
    };
    if (t[0] < 0 || t[1] < 0)
        return -1;
    const int a = one(attack, t[0]), b = t[1] == t[0] ? 4 : one(attack, t[1]);
    const int c = t[2] >= 0 && t[2] < 18 && t[2] != t[0] && t[2] != t[1] ? one(attack, t[2]) : 4;
    return a * b * c / 4;
}

// The reasons in English, as the module words them with the English table.
std::string EnglishReason(const lp_switch::Reason& r) {
    static constexpr const char* Types[18] = {"Normal", "Fighting", "Flying", "Poison", "Ground", "Rock",
                                              "Bug",    "Ghost",    "Steel",  "Fire",   "Water",  "Grass",
                                              "Electric", "Psychic", "Ice",   "Dragon", "Dark",   "Fairy"};
    static constexpr const char* Status[6] = {"", "Paralyzed", "Asleep", "Frozen", "Burned", "Poisoned"};
    using R = lp_switch::Reason;
    const std::string type = r.type >= 0 && r.type < 18 ? Types[r.type] : "";
    switch (r.kind) {
    case R::Immune: return "Immune to " + type;
    case R::Resists: return "Resists " + type;
    case R::Super: return "Super effective " + (r.move.empty() ? type : r.move);
    case R::Faster: return "Faster";
    case R::Weak: return "!Weak to " + type;
    case R::Status: return std::string{"!"} + Status[r.status];
    case R::LowHp: return "!Low HP";
    default: return "Even matchup";
    }
}
std::vector<std::string> Reasons(const lp_switch::Suggestion& s) {
    std::vector<std::string> out;
    for (const auto& r : s.reasons)
        out.push_back(EnglishReason(r));
    return out;
}

void TestSwitchSuggestions() {
    using lp_switch::Battler;
    using lp_switch::Move;
    using lp_switch::Suggest;
    const auto mon = [](int slot, int t1, int t2, std::vector<Move> moves, std::array<int, 5> stats, int hp = 100,
                        int status = 0) {
        Battler b;
        b.slot = slot;
        b.types = {t1, t2, -1};
        b.stats = stats;
        b.hp = hp;
        b.hp_max = 100;
        b.status = status;
        b.moves = std::move(moves);
        return b;
    };
    const Move ember{9, 2, 40, "Ember"}, scratch{0, 1, 40, "Scratch"}, surf{10, 2, 90, "Surf"};
    const Move razor{11, 1, 55, "Razor Leaf"}, tackle{0, 1, 40, "Tackle"}, quake{4, 1, 100, "Earthquake"};
    const Move bolt{12, 2, 90, "Thunderbolt"}, growl{0, 0, 0, "Growl"}, rock{5, 1, 75, "Rock Slide"};
    const std::array<int, 5> even{50, 50, 50, 50, 50}, fast{50, 50, 50, 50, 90}, slow{50, 50, 50, 50, 20};
    assert(TestEff(10, {9, 9, -1}) == 32 && TestEff(12, {4, 4, -1}) == 0 && TestEff(9, {10, 10, -1}) == 8);

    // a Fire foe: the Water mon resists its Ember, hits it super effectively and outspeeds it
    const Battler fire = mon(-1, 9, 9, {ember, scratch}, even);
    auto s = Suggest({mon(1, 11, 11, {razor}, slow), mon(2, 10, 10, {surf, tackle}, fast),
                      mon(3, 0, 0, {tackle}, even, 0), mon(4, 0, 0, {tackle}, even)},
                     {fire}, TestEff);
    assert(s.size() == 3); // the fainted one is left out
    assert(s[0].slot == 2 && s[0].reasons.size() == 3);
    assert(Reasons(s[0])[0] == "Resists Fire" && Reasons(s[0])[1] == "Super effective Surf" && Reasons(s[0])[2] == "Faster");
    assert(s[1].slot == 4 && s[2].slot == 1);
    assert(Reasons(s[2]).back() == "!Weak to Fire" && s[2].score < s[1].score);
    assert(Suggest({mon(2, 10, 10, {surf}, fast)}, {}, TestEff).empty()); // no foe known: nothing to rate
    assert(Suggest({mon(1, 11, 11, {razor}, slow), mon(2, 10, 10, {surf}, fast)}, {fire}, TestEff, 1).size() == 1);

    // an Electric foe: Ground is immune and Earthquake is super effective; Flying is weak to it
    const Battler elec = mon(-1, 12, 12, {bolt, growl}, fast);
    s = Suggest({mon(1, 2, 2, {tackle}, even), mon(2, 4, 4, {quake}, slow)}, {elec}, TestEff);
    assert(s.size() == 2 && s[0].slot == 2);
    assert(Reasons(s[0])[0] == "Immune to Electric" && Reasons(s[0])[1] == "Super effective Earthquake");
    assert(Reasons(s[1])[0] == "!Weak to Electric");

    // the foe's moves unknown: its types stand in for its attacks
    s = Suggest({mon(1, 11, 11, {razor}, even), mon(2, 5, 5, {rock}, even)}, {mon(-1, 9, 9, {}, even)}, TestEff);
    assert(s[0].slot == 2 && Reasons(s[0])[0] == "Resists Fire" && Reasons(s[0])[1] == "Super effective Rock Slide");

    // HP and status: the same mon asleep or nearly fainted ranks lower; warnings only when there is room
    s = Suggest({mon(1, 10, 10, {surf}, fast, 100, 2), mon(2, 10, 10, {surf}, fast, 10), mon(3, 10, 10, {surf}, fast)},
                {fire}, TestEff);
    assert(s[0].slot == 3 && s[1].slot == 1 && s[2].slot == 2);
    assert(s[1].reasons.size() == 3 && Reasons(s[1])[1] == "Super effective Surf" && Reasons(s[1])[2] == "!Asleep");
    const auto warn = Suggest({mon(1, 0, 0, {tackle}, slow, 10, 2)}, {fire}, TestEff);
    assert(warn[0].reasons.size() == 2 && Reasons(warn[0])[0] == "!Asleep" && Reasons(warn[0])[1] == "!Low HP");
    // paralysis halves Speed: 90 -> 45 is not "Faster" than 50 any more ...
    s = Suggest({mon(1, 10, 10, {surf}, fast, 100, 1)}, {fire}, TestEff);
    {
        const auto words = Reasons(s[0]);
        assert(std::find(words.begin(), words.end(), "Faster") == words.end());
    }
    // ... but still is than 40 (a quarter, 22, would not be)
    s = Suggest({mon(1, 0, 0, {tackle}, fast, 100, 1)}, {mon(-1, 0, 0, {tackle}, {50, 50, 50, 50, 40})}, TestEff);
    assert(Reasons(s[0]).size() == 2 && Reasons(s[0])[0] == "Faster" && Reasons(s[0])[1] == "!Paralyzed");

    // a double: rated against both foes (Ground/Rock is immune to one, resists the other, hits both hard)
    s = Suggest({mon(1, 10, 10, {surf}, even), mon(2, 4, 5, {quake}, even)}, {fire, elec}, TestEff);
    assert(s[0].slot == 2 && Reasons(s[0])[0] == "Immune to Electric");
    // only status moves: no offence at all
    s = Suggest({mon(1, 10, 10, {growl}, even), mon(2, 10, 10, {surf}, even)}, {fire}, TestEff);
    assert(s[0].slot == 2 && s[1].score < s[0].score - 40);
    // its strongest attack (STAB Scratch) is neutral on Water: the resisted weaker one is the reason
    s = Suggest({mon(1, 10, 10, {tackle}, even)}, {mon(-1, 0, 0, {ember, scratch}, even)}, TestEff);
    assert(Reasons(s[0])[0] == "Resists Fire");
    // a weak foe caps every matchup: the stronger of two equal matchups first
    const std::array<int, 5> tiny{5, 5, 5, 5, 5}, big{200, 200, 200, 200, 200};
    s = Suggest({mon(1, 0, 0, {tackle}, even), mon(2, 0, 0, {tackle}, big)}, {mon(-1, 0, 0, {tackle}, tiny)}, TestEff);
    assert(s[0].score == s[1].score && s[0].slot == 2 && s[0].strength > s[1].strength);
    // ... but a super-effective move still ranks first against it
    s = Suggest({mon(1, 0, 0, {tackle}, big), mon(2, 10, 10, {surf}, even)}, {mon(-1, 9, 9, {scratch}, tiny)}, TestEff);
    assert(s[0].slot == 2 && Reasons(s[0])[0] == "Super effective Surf" && s[0].score > s[1].score);
}

// The string table: label normalisation, lookup by variant with the label / our row / English
// fallback, and the generated table's own consistency.
void TestStringTable() {
    using lp_lang::Variant;
    using lp_strings::NormalizeLabel;
    assert(NormalizeLabel(" Sort") == "Sort" && NormalizeLabel("Vus :") == "Vus");
    assert(NormalizeLabel("Capturés\u00a0:") == "Capturés" && NormalizeLabel("已捉到：") == "已捉到");
    assert(NormalizeLabel("Which move’s PP\ndo you want to boost?") == "Which move’s PP do you want to boost?");
    assert(NormalizeLabel("要增加哪个招式\n的ＰＰ？") == "要增加哪个招式的ＰＰ？"); // no space inside Chinese
    assert(NormalizeLabel("Lv. ") == "Lv." && NormalizeLabel("a  b") == "a b");
    assert(NormalizeLabel("reloj digital", lp_strings::UpperFirst) == "Reloj digital");
    assert(NormalizeLabel("équipe Pokémon", lp_strings::UpperFirst) == "Équipe Pokémon");

    // a C row: the label where the row lists the variant, else our text, else English
    lp_strings::Row row{"dex_sort", "ss_pokedex", "SS_pokedex_166", 0,
                        1u << static_cast<int>(Variant::Fr) | 1u << static_cast<int>(Variant::Ko),
                        {"Sort", "Ordenar", "Trier", "Sortieren"}};
    std::map<std::string, std::string> labels{{"ss_pokedex/SS_pokedex_166", " Trier"}};
    const lp_strings::LabelFn fn = [&labels](std::string_view t, std::string_view l) {
        const auto it = labels.find(std::string{t} + "/" + std::string{l});
        return it == labels.end() ? std::string{} : it->second;
    };
    assert(lp_strings::Resolve(row, Variant::Fr, fn) == "Trier");     // the game's own word, trimmed
    assert(lp_strings::Resolve(row, Variant::PtBR, fn) == "Ordenar"); // not listed: our row
    assert(lp_strings::Resolve(row, Variant::Ko, fn) == "Trier");     // listed: whatever the catalog has
    assert(lp_strings::Resolve(row, Variant::ZhHant, fn) == "Sort");  // nothing for it: English
    labels.clear();
    assert(lp_strings::Resolve(row, Variant::Fr, fn) == "Trier"); // the label empty: our fr row
    assert(lp_strings::Resolve(row, Variant::Ko, fn) == "Sort");  // the label empty, no ko row: English

    // the generated table
    std::map<std::string_view, int> keys;
    for (const auto& r : lp_strings::Rows) {
        assert(!r.text[0].empty());       // an English text for every row
        assert(keys.emplace(r.key, 0).second); // keys are unique
        if (!r.label.empty()) {
            // a label table the catalog keeps by name
            const bool kept = std::any_of(lp_lang::LabelTableList.begin(), lp_lang::LabelTableList.end(),
                                          [&r](const auto& d) { return d.table == r.table; });
            assert(kept);
        }
        // every variant of a template has the English slots; particle tokens only in Korean
        const auto slots = [](std::string_view t) {
            std::vector<std::string_view> out;
            for (std::size_t i = t.find('{'); i != std::string_view::npos; i = t.find('{', i + 1)) {
                const auto close = t.find('}', i);
                const auto token = t.substr(i + 1, close - i - 1);
                if (token.find('/') == std::string_view::npos)
                    out.push_back(token);
            }
            std::sort(out.begin(), out.end());
            return out;
        };
        const auto has_particle = [](std::string_view t) {
            for (std::size_t i = t.find('{'); i != std::string_view::npos; i = t.find('{', i + 1))
                if (t.substr(i, t.find('}', i) - i).find('/') != std::string_view::npos)
                    return true;
            return false;
        };
        assert(!has_particle(r.text[0]));
        for (std::size_t v = 1; v < r.text.size(); ++v) {
            if (r.text[v].empty())
                continue;
            assert(slots(r.text[v]) == slots(r.text[0]));
            if (v != static_cast<std::size_t>(Variant::Ko))
                assert(!has_particle(r.text[v]));
        }
    }
    assert(lp_strings::Rows.size() == static_cast<std::size_t>(lp_strings::Key::Count));
    // the Pokétch apps are consecutive rows (the module indexes them by app)
    assert(static_cast<int>(lp_strings::Key::poketch_app_19) - static_cast<int>(lp_strings::Key::poketch_app_0) == 19);
    for (const auto& w : lp_strings::Widths)
        assert(w.key < lp_strings::Rows.size() && w.scale > 0);
    for (const auto& f : lp_strings::Fits)
        assert(f.key < lp_strings::Rows.size() && f.min_scale <= f.scale);

    // a whole table resolved: English without a catalog, the label in a modded language
    lp_strings::Table t;
    t.Build(lp_strings::Rows, Variant::En, nullptr);
    assert(t.Get(static_cast<std::size_t>(lp_strings::Key::weak_to)) == "Weak to");
    assert(t.Get(static_cast<std::size_t>(lp_strings::Key::cancel)) == "Cancel"); // B row: English default
    t.Build(lp_strings::Rows, Variant::Fr, [](std::string_view table, std::string_view label) {
        return table == "ss_bag" && label == "SS_bag_096" ? std::string{"Retour"} : std::string{};
    });
    assert(t.Variant() == Variant::Fr);
    assert(t.Get(static_cast<std::size_t>(lp_strings::Key::cancel)) == "Retour");             // the game's label
    assert(t.Get(static_cast<std::size_t>(lp_strings::Key::weak_to)) == "Faible face à");      // our row
    assert(t.Get(static_cast<std::size_t>(lp_strings::Key::power)) == "Power");                // no label: English
    assert(t.Get(static_cast<std::size_t>(lp_strings::Key::Count)).empty());
}

// Templates: named slots, the plural rows and the Korean particle choice.
void TestTemplates() {
    using lp_lang::Variant;
    using lp_strings::Fill;
    assert(Fill("Used {item} on {who}.", {{"item", "Potion"}, {"who", "Pika"}}) == "Used Potion on Pika.");
    assert(Fill("{who}의 체력이 {n} 회복되었다!", {{"who", "피카"}, {"n", "20"}}) == "피카의 체력이 20 회복되었다!");
    assert(Fill("vs {foe}", {}) == "vs ");               // a slot without a value is empty
    assert(Fill("{n} {nope}", {{"n", "3"}}) == "3 ");
    assert(Fill("no slots", {{"n", "3"}}) == "no slots");
    // plural: n == 1 takes _one (en, de, es); fr and pt-BR also 0; ko / zh one form
    assert(lp_strings::PluralOne(Variant::En, 1) && !lp_strings::PluralOne(Variant::En, 0) &&
           !lp_strings::PluralOne(Variant::En, 2));
    assert(lp_strings::PluralOne(Variant::De, 1) && !lp_strings::PluralOne(Variant::Es419, 0));
    assert(lp_strings::PluralOne(Variant::Fr, 0) && lp_strings::PluralOne(Variant::PtBR, 1) &&
           !lp_strings::PluralOne(Variant::Fr, 2));
    assert(lp_strings::PluralOne(Variant::Ko, 5) && lp_strings::PluralOne(Variant::ZhHant, 3));
    {
        lp_strings::Table t;
        t.Build(lp_strings::Rows, Variant::Fr, nullptr);
        const auto& one = t.Get(static_cast<std::size_t>(lp_strings::Key::turns_left_one));
        const auto& other = t.Get(static_cast<std::size_t>(lp_strings::Key::turns_left_other));
        assert(Fill(lp_strings::PluralOne(Variant::Fr, 1) ? one : other, {{"n", "1"}}) == "1 tour restant");
        assert(Fill(lp_strings::PluralOne(Variant::Fr, 4) ? one : other, {{"n", "4"}}) == "4 tours restants");
    }

    // Korean particles: final consonant (jong) -> first form, vowel -> second, ㄹ before 으로/로 -> 로,
    // digits by their Sino-Korean reading, anything else the combined form
    using lp_strings::KoParticle;
    assert(KoParticle("을/를", "몬스터볼") == "을");   // 볼: final ㄹ
    assert(KoParticle("을/를", "상처약") == "을");     // 약: final ㄱ
    assert(KoParticle("을/를", "포션") == "을");       // 션: final ㄴ
    assert(KoParticle("을/를", "사이다") == "를");     // 다: no final
    assert(KoParticle("이/가", "피카츄") == "가" && KoParticle("이/가", "꼬부기") == "가");
    assert(KoParticle("은/는", "리자몽") == "은");
    assert(KoParticle("과/와", "물") == "과" && KoParticle("과/와", "나무") == "와");
    assert(KoParticle("으로/로", "물") == "로" && KoParticle("으로/로", "불꽃") == "으로" &&
           KoParticle("으로/로", "바다") == "로");
    assert(KoParticle("을/를", "Potion") == "을(를)" && KoParticle("으로/로", "TM24") == "로"); // 4 사: a vowel
    assert(KoParticle("을/를", "기술머신2") == "를" && KoParticle("을/를", "기술머신3") == "을" &&
           KoParticle("으로/로", "기술머신7") == "로" && KoParticle("이/가", "") == "이(가)");
    assert(KoParticle("으로/로", "Potion") == "(으)로");
    assert(Fill("{item}{을/를} 썼다!", {{"item", "상처약"}}) == "상처약을 썼다!");
    assert(Fill("{who}에게 {item}{을/를} 썼다!", {{"who", "피카"}, {"item", "사이다"}}) == "피카에게 사이다를 썼다!");
    assert(Fill("{item}{을/를} 썼다!", {{"item", "Max Elixir"}}) == "Max Elixir을(를) 썼다!");
    assert(Fill("이 {을/를}", {{"item", "x"}}) == "이 을(를)"); // not right after a slot: combined
    // the ko rows that carry particles fill without leftover braces
    {
        lp_strings::Table t;
        t.Build(lp_strings::Rows, Variant::Ko, nullptr);
        const auto used = Fill(t.Get(static_cast<std::size_t>(lp_strings::Key::toast_used_on)),
                               {{"item", "상처약"}, {"who", "피카츄"}});
        assert(used == "피카츄에게 상처약을 썼다!");
        assert(Fill(t.Get(static_cast<std::size_t>(lp_strings::Key::popup_use_on)), {{"item", "사이다"}}).find('{') ==
               std::string::npos);
    }

    // the Pokédex A–Z key: accents with their letter, Œ as oe, case folded
    using lp_strings::FoldKey;
    assert(FoldKey("Écrapince") < FoldKey("Ectoplasma") && FoldKey("Évoli") > FoldKey("Ectoplasma"));
    assert(FoldKey("Œuf") == "oeuf" && FoldKey("Straße") == "strasse" && FoldKey("Ñ") == "n");
    assert(FoldKey("가나") == "가나"); // Hangul as it is (code-point order is 가나다 order)
}

// CJK faces: glyphs moved down so their em box is centred on the cap band; nothing horizontal or
// in the line pitch changes, so lp_text's widths and heights stay exact.
void TestCjkCentring() {
    // a face whose 'H' is 700 units (cap 70 px at 0.1 px / unit) and 国 from -120 to 880: the
    // ideograph is drawn 1.05 cap tall (73.5 px: 0.0735 px / unit), its centre 380 units * 0.0735 =
    // 27.9 px moved to 0.45 cap (31.5 px): 4 px up
    auto m = lp_unity::CjkMetricsFor(-120, 880, 70, 0.1f);
    assert(std::abs(m.scale - 0.0735f) < 1e-4f && m.shift == -4);
    // a small ideograph is never drawn larger than the face's own scale
    m = lp_unity::CjkMetricsFor(0, 600, 70, 0.1f);
    assert(m.scale == 0.1f && m.shift == -2); // centre 30 px -> 31.5
    m = lp_unity::CjkMetricsFor(-200, 1000, 48, 0.08f); // tall: smaller, centre 16.8 -> 21.6
    assert(std::abs(m.scale - 0.042f) < 1e-4f && m.shift == -5);
    assert(lp_unity::CjkMetricsFor(5, 5, 48, 0.1f).scale == 0.1f); // no box: unchanged
    assert(lp_unity::CjkCentred(0x56FD) && lp_unity::CjkCentred(0xD55C) && lp_unity::CjkCentred(0x3042) &&
           lp_unity::CjkCentred(0x3002) && lp_unity::CjkCentred(0xFF01));
    assert(!lp_unity::CjkCentred('A') && !lp_unity::CjkCentred(0xE9) && !lp_unity::CjkCentred(0x2605));
    // bearing_y is not part of any measure: the same advances measure the same with or without it
    lp_assets::Font f;
    f.line_height = 40;
    f.first_codepoint = 0x20;
    f.glyphs.assign(0xAC01 - 0x20, EdenDsmodFontGlyph{});
    f.glyphs[0xAC00 - 0x20] = EdenDsmodFontGlyph{0, 0, 40, 44, 2, 46, 44};
    const lp_text::Measure before{f};
    const int w = before.Width("가가 가", 5), h = before.Height("가가 가가", 5, 60);
    f.glyphs[0xAC00 - 0x20].bearing_y = 42;
    const lp_text::Measure after{f};
    assert(after.Width("가가 가", 5) == w && after.Height("가가 가가", 5, 60) == h);
    // 가 at scale 5: 44 * 25 / 40 = 27.5 px each; the space here has no advance
    assert(w == 82);
}

// The top screen's HP gauges (BUIStatusWindow -> HpBar -> Slider) give each battler the HP shown now
// (hp_view), matched by pokeID (CORE_PARAM.myID) and max HP; the foe's rank bytes stage its stats.
void TestHpGaugeAndFoeStages() {
    using namespace lp_live;
    constexpr u64 Main = 0x80000000, Method = 0x100000, Klass = 0x101000, Statics = 0x102000, Bvc = 0x103000;
    constexpr u64 Vs = 0x104000, Env = 0x105000, Pokecon = 0x106000, Parties = 0x107000, Ui = 0x108000;
    constexpr u64 Windows = 0x109000;
    FakeMemory mem;
    for (const auto& f : off::Fingerprints) mem.W64(Main + f.slot, Main + f.target);
    mem.W64(Main + off::BattleViewCoreGetInstance, Method);
    mem.W64(Method + off::MethodKlass, Klass);
    mem.W64(Klass + off::KlassStatics, Statics);
    mem.W64(Statics, Bvc);
    mem.W64(Bvc + off::BvcUiSystem, Ui);
    mem.W64(Bvc + off::BvcViewSystem, Vs);
    mem.W64(Vs + off::ViewSystemEnv, Env);
    mem.W64(Env + off::EnvPokecon, Pokecon);
    mem.W64(Pokecon + off::PokeconParty, Parties);
    struct Mon { u64 bpp, core, vary; };
    // party[client] with one member: hp / hp_max, pokeID, all ranks neutral, five stats of 100
    auto battler = [&](int client, u16 species, u16 hp, u16 hp_max, u8 id) {
        const u64 b = 0x200000 + 0x10000 * static_cast<u64>(client);
        const u64 party = b, members = b + 0x1000, bpp = b + 0x2000, core = b + 0x3000, base = b + 0x4000,
                  vary = b + 0x5000;
        mem.W64(Parties + off::ArrayData + 8 * static_cast<u64>(client), party);
        mem.W64(party + off::BtlPartyMembers, members);
        mem.W8(party + off::BtlPartyCount, 1);
        mem.W64(members + off::ArrayData, bpp);
        mem.W64(bpp + off::BppCore, core);
        mem.W64(bpp + off::BppWaza, 0);
        mem.W8(bpp + off::BppWazaCount, 0);
        for (int i = 0; i < 0x10; ++i) mem.W8(core + off::CoreMonsNo + i, 0);
        mem.W16(core + off::CoreMonsNo, species);
        mem.W16(core + off::CoreMonsNo + 4, hp_max);
        mem.W16(core + off::CoreMonsNo + 6, hp);
        mem.W8(core + off::CoreMonsNo + 0xE, 100);
        mem.W8(core + off::CoreMyId, id);
        mem.W64(bpp + off::BppVary, vary);
        for (int i = 0; i < 7; ++i) mem.W8(vary + off::VaryRanks + i, 6);
        mem.W64(bpp + off::BppBase, base);
        for (int i = 0; i < 5; ++i) mem.W16(base + off::BaseStats + 2 * i, 100);
        for (int i = 0; i < 3; ++i) mem.W8(base + off::BaseTypes + i, 18);
        return Mon{bpp, core, vary};
    };
    const auto own = battler(0, 445, 197, 252, 0);
    const auto foe = battler(1, 269, 207, 261, 14);
    (void)own;
    // status windows: [0] near (ours), [1] far (the foe), [2] never set up
    auto window = [&](int i, u8 id, u32 max, float value, bool display, bool init) {
        const u64 w = 0x300000 + 0x1000 * static_cast<u64>(i), bar = w + 0x400, slider = w + 0x800;
        mem.W64(Windows + off::ArrayData + 8 * static_cast<u64>(i), w);
        mem.W8(w + off::StatusInitialized, init ? 1 : 0);
        mem.W8(w + off::StatusDisplay, display ? 1 : 0);
        mem.W8(w + off::StatusPokeId, id);
        mem.W32(w + off::StatusMaxHp, max);
        mem.W64(w + off::StatusHpBar, bar);
        mem.W32(bar + off::HpBarMax, max);
        mem.W64(bar + off::HpBarSlider, slider);
        std::uint32_t raw = 0;
        std::memcpy(&raw, &value, 4);
        mem.W32(slider + off::SliderValue, raw);
        return slider;
    };
    Guest g;
    g.main_base = Main;
    g.read = [&](u64 a, void* o, std::size_t n) { return mem.Read(a, o, n); };
    Reader r{g};
    assert(r.Resolve(0));
    // no window array yet: no gauge, the real HP stands
    auto s = r.Sample();
    assert(s.front[0].valid && s.front[0].id == 0 && s.front[1].id == 14);
    assert(s.front[0].hp_view == -1 && s.front[1].hp_view == -1);
    mem.W64(Ui + off::UiStatusWindows, Windows);
    mem.W64(Windows + off::ArrayLength, 3);
    // ours finished draining (197 / 252 = 0.78174), the foe's still full (the hit not shown yet)
    window(0, 0, 252, 197.0f / 252.0f, true, true);
    const u64 foe_slider = window(1, 14, 261, 1.0f, true, true);
    window(2, 0, 0, 0.0f, false, false);
    s = r.Sample();
    assert(s.front[0].hp_view == 197 && s.front[1].hp_view == 261 && s.foe[0].hp_view == 261);
    assert(s.front[1].hp == 207); // the real HP is unchanged
    // mid-drain: (int)(0.866 * 261 + 0.5) = 226, as HpBar.UpdateHp prints it
    float mid = 0.866f;
    std::uint32_t raw = 0;
    std::memcpy(&raw, &mid, 4);
    mem.W32(foe_slider + off::SliderValue, raw);
    assert(r.Sample().front[1].hp_view == 226);
    // a window for another battler (pokeID) or another max HP is not this battler's gauge
    mem.W8(0x301000 + off::StatusPokeId, 15);
    assert(r.Sample().front[1].hp_view == -1);
    mem.W8(0x301000 + off::StatusPokeId, 14);
    mem.W32(0x301400 + off::HpBarMax, 300);
    assert(r.Sample().front[1].hp_view == -1);
    mem.W32(0x301400 + off::HpBarMax, 261);
    // a value outside 0..1 (a torn read) is ignored; an uninitialised window too
    float bad = 1.5f;
    std::memcpy(&raw, &bad, 4);
    mem.W32(foe_slider + off::SliderValue, raw);
    assert(r.Sample().front[1].hp_view == -1);
    mem.W32(foe_slider + off::SliderValue, 0x3F800000); // 1.0f
    mem.W8(0x301000 + off::StatusInitialized, 0);
    assert(r.Sample().front[1].hp_view == -1);
    mem.W8(0x301000 + off::StatusInitialized, 1);
    assert(r.Sample().front[1].hp_view == 261);
    // the foe's own stat stages (Quiver Dance: Sp. Atk, Sp. Def, Speed +1) stage its stats
    for (int i : {2, 3, 4}) mem.W8(foe.vary + off::VaryRanks + static_cast<u64>(i), 7);
    s = r.Sample();
    assert(s.front[1].stages_ok && s.front[1].stages[2] == 1 && s.front[1].stages[4] == 1);
    assert(s.front[1].staged[0] == 100 && s.front[1].staged[2] == 150 && s.front[1].staged[4] == 150);
    assert(s.foe[0].staged[3] == 150);
}

// The two games' Bag tabs (lp_vanilla): the game's ItemPocket per tab (Treasures = 6, Key Items = 8,
// no tab for Foods 7), nine tabs with Luminescent Platinum's Extra Items, vanilla BD's own eight;
// stepping wraps within the game's tabs and a tab of the other layout falls back to the first.
void TestVanillaBag() {
    using namespace lp_vanilla;
    assert(BagFieldPockets[6] == 6 && BagFieldPockets[7] == 8 && BagFieldPockets[8] == 9);
    for (int k = 0; k < 6; ++k)
        assert(BagFieldPockets[k] == k);
    assert(std::find(BagFieldPockets.begin(), BagFieldPockets.end(), 7) == BagFieldPockets.end());
    assert(BagTabs(true) == 9 && BagTabs(false) == 8);
    // vanilla: Key Items (7) -> Medicine (0) and back; LP: Key Items -> Extra Items (8) -> Medicine
    assert(StepTab(7, true, false) == 0 && StepTab(0, false, false) == 7);
    assert(StepTab(7, true, true) == 8 && StepTab(8, true, true) == 0 && StepTab(0, false, true) == 8);
    assert(StepTab(3, true, false) == 4 && StepTab(3, false, true) == 2);
    // Extra Items carried over into vanilla (or a bad index) starts again at the first tab
    assert(ClampTab(8, false) == 0 && ClampTab(8, true) == 8 && ClampTab(-1, true) == 0 && ClampTab(7, false) == 7);
    assert(StepTab(8, true, false) == 1); // an out-of-range tab steps from the first
    // title logos: LP's 61.9 KB placeholder is not a logo; BD's own French / Chinese ones are
    assert(!OwnLogo(61903) && !OwnLogo(0) && OwnLogo(96163) && OwnLogo(96764) && OwnLogo(142087) && OwnLogo(4158873));
}

// The Pokédex readers (lp_dex) over a synthetic PersonalTable + EvolveTable: branching chains in
// depth-first table order, merged ways into one member, forms with their own rows and evolutions,
// the implicit Shedinja, invalid rows left out; the gender ratio and the EV yield.
void TestDexChains() {
    using namespace lp_dex;
    Table t(20);
    for (int i = 1; i < 20; ++i)
        t[i].species = i;
    // 1 -> 2 (Lv. 16) -> 3 (Lv. 32): a straight line
    t[1].evolve = {{Level, 0, 2, 0, 16}};
    t[2].evolve = {{Level, 0, 3, 0, 32}};
    // 4 (Eevee-like) -> 5 (two ways), 6, and 7 (invalid: left out)
    t[4].evolve = {{UseItem, 85, 5, 0, 0}, {UseItem, 83, 6, 0, 0}, {LevelMossRock, 0, 5, 0, 0}, {UseItem, 84, 7, 0, 0}};
    t[7].valid = false;
    // 8 (Wurmple-like) -> 9 -> 10 and 8 -> 11 -> 12
    t[8].evolve = {{LevelRandomLow, 0, 9, 0, 7}, {LevelRandomHigh, 0, 11, 0, 7}};
    t[9].evolve = {{Level, 0, 10, 0, 10}};
    t[11].evolve = {{Level, 0, 12, 0, 10}};
    // 13 (Burmy-like) has forms 1, 2 in rows 17, 18; each form evolves into 14's same form or 15
    t[13].form_index = 17;
    t[13].form_max = 3;
    t[14].form_index = 19; // 14 has form 1 in row 19 (form 2 has no row: the species row)
    t[14].form_max = 2;
    t[17].species = 13;
    t[18].species = 13;
    t[19].species = 14;
    t[13].evolve = {{LevelFemale, 0, 14, 0, 20}, {LevelMale, 0, 15, 0, 20}};
    t[17].evolve = {{LevelFemale, 0, 14, 1, 20}, {LevelMale, 0, 15, 0, 20}};
    // 16 (Nincada-like) -> 15 (Ninjask) and the implicit Shedinja (292, out of this table: ignored)
    t[16].evolve = {{LevelNinjask, 0, 15, 0, 20}};
    AddImplicit(t);
    assert(t[16].evolve.size() == 2 && t[16].evolve[1].method == LevelShedinja && t[16].evolve[1].species == Shedinja);
    AddImplicit(t); // idempotent
    assert(t[16].evolve.size() == 2);

    const auto ids = [](const std::vector<Member>& c) {
        std::vector<std::pair<int, int>> out;
        for (const auto& m : c)
            out.push_back({m.species, m.depth});
        return out;
    };
    // a straight line, from any member
    for (int s : {1, 2, 3})
        assert((ids(Chain(t, s, 0)) == std::vector<std::pair<int, int>>{{1, 0}, {2, 1}, {3, 2}}));
    // a branch: two ways into 5 merged into one member, the invalid 7 left out
    const auto e = Chain(t, 6, 0);
    assert((ids(e) == std::vector<std::pair<int, int>>{{4, 0}, {5, 1}, {6, 1}}));
    assert(e[1].via.size() == 2 && e[1].via[0].param == 85 && e[1].via[1].method == LevelMossRock);
    assert(e[1].parent == 0 && e[2].parent == 0);
    assert(Chain(t, 7, 0).size() == 1); // an invalid species shows only itself
    // two levels of branches, depth first
    assert((ids(Chain(t, 12, 0)) == std::vector<std::pair<int, int>>{{8, 0}, {9, 1}, {10, 2}, {11, 1}, {12, 2}}));
    // forms: form 1 of 13 evolves into form 1 of 14; 14's form 1 leads back to 13's form 1
    const auto b1 = Chain(t, 13, 1);
    assert(b1.size() == 3 && b1[0].form == 1 && b1[1].species == 14 && b1[1].form == 1 && b1[2].species == 15);
    const auto w1 = Chain(t, 14, 1);
    assert(w1[0].species == 13 && w1[0].form == 1);
    // the form without a row of its own is the species row (IndexOf), and IdentOf maps rows back
    assert(IndexOf(t, 14, 2) == 14 && IndexOf(t, 13, 2) == 18 && IdentOf(t, 18) == std::make_pair(13, 2));
    // 15 is reached from 13 first (table order): its chain is 13's (form 0)
    assert(Chain(t, 15, 0)[0].species == 13 && Chain(t, 15, 0)[0].form == 0);
    // a species with nothing: itself
    assert(Chain(t, 3, 0).size() == 3 && Chain(t, 5, 0)[0].species == 4);

    // gender: the standard eighths, single genders, none
    assert(GenderOf(31).kind == Gender::Mixed && GenderOf(31).male_permille == 875);
    assert(GenderOf(63).male_permille == 750 && GenderOf(127).male_permille == 500);
    assert(GenderOf(191).male_permille == 250 && GenderOf(225).male_permille == 125);
    assert(GenderOf(0).kind == Gender::MaleOnly && GenderOf(0).male_permille == 1000);
    assert(GenderOf(254).kind == Gender::FemaleOnly && GenderOf(254).male_permille == 0);
    assert(GenderOf(255).kind == Gender::Genderless);
    // EV yield: 2 bits each from HP, Atk, Def, Spe, SpA, SpD; returned as HP, Atk, Def, SpA, SpD, Spe
    assert((EvYield(256) == std::array<int, 6>{0, 0, 0, 1, 0, 0}));      // Bulbasaur: 1 Sp. Atk
    assert((EvYield(64) == std::array<int, 6>{0, 0, 0, 0, 0, 1}));       // 1 Speed
    assert((EvYield(2 | (1 << 10)) == std::array<int, 6>{2, 0, 0, 0, 1, 0})); // 2 HP, 1 Sp. Def
}

void TestDelayedFieldInputs() {
    using namespace lp_field_actions;
    assert(MoveSlot(INT64_MIN, 2) == -1 && MoveSlot(INT64_MAX, 2) == -1);
    assert(MoveSlot(99, 2) == -1 && MoveSlot(112, 2) == -1);
    for (int slot = 0; slot < 4; ++slot) assert(MoveSlot(108 + slot, 2) == slot);
    assert(NextMoveToken(0) == 1);
    assert(NextMoveToken((INT64_MAX - 103) / 4) == 1);
    assert(BagStep(1, 101, true, true, false) == Input::OpenMenu);
    assert(BagStep(2, 901, true, false, true) == Input::Accept);
    assert(BagStep(1, 5000, true, true, false) == Input::Cancel);
    assert(BagStep(2, 5000, true, false, true) == Input::Cancel);
    assert(BagStep(1, 200, false, true, false) == Input::Cancel);
    assert(BagStep(1, 200, true, false, true) == Input::Cancel);
    assert(ShortcutStep(1, 151, true, true) == Input::Wheel);
    assert(ShortcutStep(2, 701, true, false) == Input::Direction);
    assert(ShortcutStep(3, 1701, true, true) == Input::Restore);
    assert(ShortcutStep(1, 5000, true, true) == Input::Restore);
    assert(ShortcutStep(2, 5000, true, true) == Input::Restore);
    assert(ShortcutStep(2, 701, false, true) == Input::Restore);
    assert(ShortcutStep(1, 151, true, false) == Input::Restore);
    assert(ShortcutStep(0, 5000, false, false) == Input::None);
    assert(ShortcutStep(4, 10, true, true) == Input::Restore);
}

// Sprite motion (lp_anim): every curve is a pure function of the time since its event.
void TestAnimation() {
    using namespace lp_anim;
    assert(PartySwap(-1, 0, 0, 1).Rest());
    assert(PartySwap(0, 1, 0, 1).y == -150);
    assert(PartySwap(120, 1, 0, 1).x == 28 && PartySwap(120, 1, 0, 1).y == -150);
    assert(PartySwap(280, 1, 0, 1).y == -75 && PartySwap(280, 0, 0, 1).y == 75);
    assert(PartySwap(440, 1, 0, 1).y == 0 && PartySwap(440, 1, 0, 1).x == 28);
    assert(PartySwap(560, 1, 0, 1).Rest() && PartySwap(280, 2, 0, 1).Rest());
    // hop: up and back in HopMs, HopPx at the top, eased (faster near the ground than near the top)
    assert(HopY(-1) == 0 && HopY(0) == 0 && HopY(HopMs) == 0 && HopY(HopMs + 50) == 0);
    assert(HopY(HopMs / 2) == -HopPx);
    for (double t = 0; t < HopMs; t += 5)
        assert(HopY(t) <= 0 && HopY(t) >= -HopPx && HopY(t) == HopY(HopMs - t)); // a symmetric arc
    assert(-HopY(30) > -HopY(150) - -HopY(120));                                   // ease-out at the start
    // hit: shakes for HitMs only, both directions, never past its amplitude, decaying
    assert(HitShake(-1).Rest() && HitShake(HitMs).Rest() && HitShake(HitMs + 1000).Rest());
    bool left = false, right = false;
    for (double t = 0; t < HitMs; t += 10) {
        const Pose p = HitShake(t);
        assert(std::abs(p.x) <= HitPxX && std::abs(p.y) <= HitPxY && p.rot == 0 && p.scale == 1000 && p.fade == 0);
        left = left || p.x < 0;
        right = right || p.x > 0;
    }
    assert(left && right && HitShake(0).x == HitPxX && std::abs(HitShake(HitMs - 1).x) <= 1);
    // faint: sinks and fades over FaintMs, monotonic, then holds the end state
    assert(Faint(0, FaintPx).Rest() && Faint(-50, FaintPx).Rest());
    for (double t = 0; t + 20 <= FaintMs; t += 20) {
        assert(Faint(t + 20, FaintPx).y >= Faint(t, FaintPx).y);
        assert(Faint(t + 20, FaintPx).fade >= Faint(t, FaintPx).fade);
    }
    assert(Faint(FaintMs, FaintPx).y == FaintPx && Faint(FaintMs, FaintPx).fade == FadeSteps);
    assert(Faint(FaintMs * 10, FaintPxCard) == Faint(FaintMs, FaintPxCard) && Faint(FaintMs, FaintPxCard).y == FaintPxCard);
    assert(Faint(FaintMs / 2, FaintPx).y < FaintPx / 2); // ease-in: slow start
    // breathing: 1.000 at the start and every period, 1.025 at the half, in 0.5 % steps
    assert(Breath(-1) == 1000 && Breath(0) == 1000 && Breath(BreathMs) == 1000 && Breath(BreathMs * 3) == 1000);
    assert(Breath(BreathMs / 2) == 1000 + BreathMilli && Breath(BreathMs * 2.5) == 1000 + BreathMilli);
    for (double t = 0; t < BreathMs * 2; t += 7)
        assert(Breath(t) >= 1000 && Breath(t) <= 1000 + BreathMilli && (Breath(t) - 1000) % BreathStep == 0);
    // status loops: sleep bobs up SleepPx every SleepMs; paralysis twitches TwitchMs every TwitchEveryMs;
    // red HP sways +-WobbleDeg every WobbleMs; each starts at rest
    assert(SleepY(0) == 0 && SleepY(SleepMs / 2) == -SleepPx && SleepY(SleepMs) == 0 && SleepY(-5) == 0);
    int twitching = 0;
    for (double t = 0; t < TwitchEveryMs; t += 1)
        twitching += TwitchX(t) != 0;
    assert(twitching >= TwitchMs - 2 && twitching <= TwitchMs + 2 && TwitchX(0) == 0 && TwitchX(TwitchEveryMs - 1) != 0);
    assert(TwitchX(TwitchEveryMs * 2 - TwitchMs + 1) == TwitchPx && TwitchX(TwitchEveryMs * 2 + 10) == 0);
    assert(WobbleDegAt(0) == 0 && WobbleDegAt(WobbleMs / 4) == WobbleDeg && WobbleDegAt(WobbleMs * 3 / 4) == -WobbleDeg);
    assert(LoopOf(2, 10, 100) == Loop::Sleep && LoopOf(1, 10, 100) == Loop::Paralysis && LoopOf(0, 20, 100) == Loop::LowHp);
    assert(LoopOf(0, 21, 100) == Loop::None && LoopOf(2, 0, 100) == Loop::None && LoopOf(4, 50, 100) == Loop::None);
    assert(LoopPose(Loop::None, 1234).Rest() && LoopPose(Loop::LowHp, WobbleMs / 4).rot == WobbleDeg);

    // a battler: the shown HP draining starts one hit; a pause then another drain starts the next
    Battler b;
    assert(b.At(0, FaintPx).Rest());
    b.Observe(0, 7, true, 100, 100, 0);
    assert(b.At(0, FaintPx).Rest() && b.At(5000, FaintPx).Rest()); // healthy, idle: rest
    b.Observe(1000, 7, true, 98, 100, 0);                            // the gauge starts to drain
    assert(b.At(1000, FaintPx) == HitShake(0));
    for (int k = 1; k <= 30; ++k)                                    // drains for 0.5 s: one hit
        b.Observe(1000 + k * 16.7, 7, true, 98 - k, 100, 0);
    assert(b.At(1000 + HitMs / 2, FaintPx) == HitShake(HitMs / 2));
    assert(b.At(1000 + HitMs, FaintPx).Rest() && b.At(1600, FaintPx).Rest()); // settled, HP 68
    b.Observe(3000, 7, true, 60, 100, 0);                             // the next hit
    assert(b.At(3000 + 80, FaintPx) == HitShake(80));
    // the same HP or healing: no hit
    b.Observe(4000, 7, true, 60, 100, 0);
    b.Observe(4100, 7, true, 80, 100, 0);
    assert(b.At(4100, FaintPx).Rest());
    // status loops (asleep, then paralysed) start at rest when they begin
    b.Observe(5000, 7, true, 80, 100, 2);
    assert(b.At(5000, FaintPx).Rest() && b.At(5000 + SleepMs / 2, FaintPx).y == -SleepPx);
    b.Observe(6000, 7, true, 80, 100, 1);
    assert(b.At(6000, FaintPx).Rest() && b.At(6000 + TwitchEveryMs - 10, FaintPx).x != 0);
    // priority: a hit over the status loop, then the loop again
    b.Observe(7000, 7, true, 15, 100, 1);
    assert(b.At(7000, FaintPx) == HitShake(0) && b.At(6000 + 2 * TwitchEveryMs - 10, FaintPx).x != 0);
    // a long drain into the red: still after the shake until the gauge stops, then the sway
    {
        Battler d;
        d.Observe(0, 1, true, 100, 100, 0);
        for (int k = 1; k <= 60; ++k) // 1 s of drain, from 100 to 10
            d.Observe(k * 16.7, 1, true, 100 - k * 3 / 2, 100, 0);
        assert(d.At(700, FaintPx).Rest() && d.At(1002 + HitQuietMs + WobbleMs / 4, FaintPx).rot != 0);
    }
    // red HP without a status: the sway
    b.Observe(9000, 7, true, 15, 100, 0);
    assert(b.At(9000 + WobbleMs / 4, FaintPx).rot == WobbleDeg);
    // faint over everything (also over the hit of the drain that emptied the gauge), then it stays
    b.Observe(10000, 7, true, 5, 100, 0);
    b.Observe(10100, 7, true, 0, 100, 0);
    assert(b.At(10100, FaintPx).Rest() && b.At(10100 + FaintMs / 2, FaintPx) == Faint(FaintMs / 2, FaintPx));
    assert(b.At(10100 + 60000, FaintPx) == Faint(FaintMs, FaintPx));
    // replaced: the next Pokémon stands at rest (no hit for its different HP)
    b.Observe(80000, 8, true, 40, 100, 0);
    assert(b.At(80000, FaintPx).Rest() && b.At(80300, FaintPx).Rest());
    // a spot that already shows a fainted Pokémon (the page opened later): sunk at once
    b.Observe(90000, 9, true, 0, 100, 0);
    assert(b.At(90000, FaintPx) == Faint(FaintMs, FaintPx));
    // gone (out of view, not in battle): rest
    b.Observe(91000, 9, false, 0, 100, 0);
    assert(b.At(91000, FaintPx).Rest());

    // the focused sprite: no hop when the page opens, a hop on a new selection or a tap,
    // breathing only while visible (from rest), nothing at all out of view
    Focus f;
    f.Observe(0, 3, false);
    assert(f.At(0).Rest() && f.At(1200).Rest());
    f.Observe(100, 3, true);
    assert(f.At(100).Rest() && f.At(100 + BreathMs / 2).scale == 1000 + BreathMilli && f.At(100 + BreathMs / 2).y == 0);
    f.Observe(5000, 4, true); // a new selection
    assert(f.At(5000 + HopMs / 2).y == -HopPx && f.At(5000 + HopMs / 2).scale == 1000);
    assert(f.At(5000 + HopMs).y == 0 && f.At(5000 + HopMs).scale == 1000);
    f.Hop(8000); // the selected plate tapped again
    assert(f.At(8000 + HopMs / 2).y == -HopPx);
    f.Observe(9000, 5, false); // another page: still, and no hop for a change made there
    assert(f.At(9000).Rest() && f.At(9000 + HopMs / 2).Rest());
    f.Observe(9500, 5, true);
    assert(f.At(9500 + HopMs / 2).y == 0 && f.At(9500).scale == 1000);

    // the setting off: the manifest publishes the rest values (lp.an.* = @flag:lp_anim ? pose : rest),
    // and those are exactly Pose{}
    assert(Pose{}.Rest() && Pose{}.x == 0 && Pose{}.y == 0 && Pose{}.rot == 0 && Pose{}.scale == 1000 && Pose{}.fade == 0);
}

// The reader's small writes and the hidden-item scan over a fake guest (batch = compare, then write).
void TestReaderWrites() {
    using namespace lp_live;
    constexpr u64 Main = 0x80000000, PwK = 0x100000, PwS = 0x110000, Pw = 0x120000;
    FakeMemory mem;
    for (const auto& f : off::Fingerprints) mem.W64(Main + f.slot, Main + f.target);
    mem.W64(Main + off::PlayerWorkTypeInfo, PwK);
    mem.W64(PwK + off::KlassStatics, PwS);
    mem.W64(PwS + off::PlayerWorkInstance, Pw);
    Guest g;
    g.main_base = Main;
    g.read = [&](u64 a, void* o, size_t n) { return mem.Read(a, o, n); };
    g.batch = [&](const std::vector<BatchOp>& ops) {
        for (const auto& op : ops) {
            std::vector<u8> bytes(op.expect.size());
            if (!mem.Read(op.addr, bytes.data(), bytes.size()) || bytes != op.expect) return false;
        }
        for (const auto& op : ops) mem.Write(op.addr, op.value.data(), op.value.size());
        return true;
    };
    Reader r(g);
    assert(r.Resolve(0));
    const auto rd64 = [&](u64 a) { u64 v = 0; assert(mem.Read(a, &v, 8)); return v; };
    const auto rd32 = [&](u64 a) { std::int32_t v = 0; assert(mem.Read(a, &v, 4)); return v; };

    // SwapParty: swaps two member pointers; the same slot or one past the party is refused
    constexpr u64 Party = 0x130000, Members = 0x140000;
    mem.W64(Pw + off::PwParty, Party);
    mem.W64(Party + off::PartyMembers, Members);
    mem.W32(Party + off::PartyCount, 3);
    mem.W64(Members + off::ArrayLength, 6);
    for (u64 i = 0; i < 6; ++i) mem.W64(Members + off::ArrayData + 8 * i, i < 3 ? 0xA00000 + i : 0);
    assert(r.SwapParty(0, 2));
    assert(rd64(Members + off::ArrayData) == 0xA00002 && rd64(Members + off::ArrayData + 16) == 0xA00000);
    assert(rd64(Members + off::ArrayData + 8) == 0xA00001);
    assert(!r.SwapParty(1, 1) && !r.SwapParty(0, 3) && !r.SwapParty(-1, 0) && !r.SwapParty(0, 6));
    assert(rd64(Members + off::ArrayData) == 0xA00002 && rd64(Members + off::ArrayData + 16) == 0xA00000);

    for (u32 count : {7u, 0xFFFFFFFFu}) {
        mem.W32(Party + off::PartyCount, count);
        assert(!r.SwapParty(0, 2));
    }
    mem.W32(Party + off::PartyCount, 3);
    for (u64 length : {2u, 7u}) {
        mem.W64(Members + off::ArrayLength, length);
        assert(!r.SwapParty(0, 2));
    }
    mem.W64(Members + off::ArrayLength, 6);
    assert(rd64(Members + off::ArrayData) == 0xA00002 && rd64(Members + off::ArrayData + 16) == 0xA00000);

    // SetShortcut: only over the value the companion saw
    constexpr u64 Short = 0x150000;
    mem.W64(Pw + off::PwShortcut, Short);
    mem.W64(Short + off::ArrayLength, 4);
    for (u64 i = 0; i < 4; ++i) mem.W16(Short + off::ArrayData + 2 * i, 0);
    mem.W16(Short + off::ArrayData + 2, 10);
    assert(!r.SetShortcut(1, 11, 20) && r.Shortcuts()[1] == 10);
    assert(r.SetShortcut(1, 10, 20) && r.Shortcuts()[1] == 20);
    assert(!r.SetShortcut(4, 0, 20));
    assert(!r.SetShortcut(1, 20 + 0x10000, 30));
    assert(!r.SetShortcut(1, 20, -1) && !r.SetShortcut(1, 20, 0x10000));
    mem.W64(Short + off::ArrayLength, 17);
    assert(!r.SetShortcut(1, 20, 30));
    mem.W64(Short + off::ArrayLength, 4);
    assert(r.Shortcuts()[1] == 20);

    // SetBagCursor: pocket 8 is button 7; the row counts only the items held (count > 0)
    constexpr u64 IwK = 0x160000, IwS = 0x170000, Iw = 0x180000, Lists = 0x190000, List = 0x1A0000;
    constexpr u64 ListArr = 0x1B0000, Mems = 0x1C0000, Mem = 0x1D0000, Idx = 0x1E0000, Scroll = 0x1F0000;
    constexpr u64 Bag = 0x200000, Info = 0x210000;
    mem.W64(Main + off::ItemWorkTypeInfo, IwK);
    mem.W64(IwK + off::KlassStatics, IwS);
    mem.W64(IwS, Iw);
    mem.W64(Iw + off::IwCategorized, Lists);
    mem.W64(Lists + off::ArrayLength, 9);
    mem.W64(Lists + off::ArrayData + 8 * 8, List);
    mem.W64(List + off::ListItems, ListArr);
    mem.W64(ListArr + off::ArrayLength, 4);
    mem.W32(List + off::ListSize, 3);
    for (u64 i = 0; i < 3; ++i) {
        mem.W64(ListArr + off::ArrayData + 8 * i, Info + 0x100 * i);
        mem.W16(Info + 0x100 * i + off::ItemInfoWorkNo, static_cast<std::uint16_t>(5 + i));
    }
    mem.W64(Pw + off::PwSaveItem, Bag);
    mem.W64(Bag + off::ArrayLength, 200);
    for (u64 i = 0; i < 200 * off::SaveItemSize; ++i) mem.W8(Bag + off::ArrayData + i, 0);
    mem.W32(Bag + off::ArrayData + 6 * off::SaveItemSize, 2);
    mem.W32(Bag + off::ArrayData + 7 * off::SaveItemSize, 1); // item 5 is not held
    mem.W64(Iw + off::IwListMemories, Mems);
    mem.W64(Mems + off::ArrayLength, 8);
    mem.W64(Mems + off::ArrayData, Mem);
    mem.W64(Mem + off::MemIndexes, Idx);
    mem.W64(Mem + off::MemScroll, Scroll);
    mem.W64(Idx + off::ArrayLength, 9);
    mem.W64(Scroll + off::ArrayLength, 9);
    mem.W32(Mem + off::MemCategory, 0);
    for (u64 i = 0; i < 9; ++i) {
        mem.W32(Idx + off::ArrayData + 4 * i, 0);
        mem.W32(Scroll + off::ArrayData + 4 * i, 5);
    }
    assert(!r.SetBagCursor(7, 7) && !r.SetBagCursor(8, 5)); // no pocket 7; item 5 is not listed
    assert(r.SetBagCursor(8, 7));
    assert(rd32(Mem + off::MemCategory) == 7 && rd32(Idx + off::ArrayData + 4 * 7) == 1 &&
           rd32(Scroll + off::ArrayData + 4 * 7) == 0 && rd32(Scroll + off::ArrayData + 4 * 8) == 5);
    mem.W64(ListArr + off::ArrayLength, 2); // a _size past its items array is refused
    mem.W32(Mem + off::MemCategory, 0);
    assert(!r.SetBagCursor(8, 7) && rd32(Mem + off::MemCategory) == 0);
    mem.W64(ListArr + off::ArrayLength, 4);
    // ...and every write is conditional on the value read just before (expected bytes)
    std::size_t cursor_ops = 0;
    bool expects_all = true;
    Guest watch = g;
    watch.batch = [&](const std::vector<BatchOp>& ops) {
        cursor_ops = ops.size();
        for (const auto& op : ops) expects_all = expects_all && op.expect.size() == op.value.size();
        return g.batch(ops);
    };
    Reader wr(watch);
    assert(wr.Resolve(0));
    mem.W32(Mem + off::MemCategory, 3);
    mem.W32(Idx + off::ArrayData + 4 * 7, 9);
    assert(wr.SetBagCursor(8, 7) && cursor_ops == 3 && expects_all);
    assert(rd32(Mem + off::MemCategory) == 7 && rd32(Idx + off::ArrayData + 4 * 7) == 1);
    mem.W64(Idx + off::ArrayLength, 8); // a resized memory array: refused before any write
    mem.W32(Mem + off::MemCategory, 0);
    assert(!wr.SetBagCursor(8, 7) && rd32(Mem + off::MemCategory) == 0);
    mem.W64(Idx + off::ArrayLength, 9);

    // ConsumeItem (a field use the game has run): one item of the same save, over the count read now
    const u64 rope = Bag + off::ArrayData + 78 * off::SaveItemSize;
    mem.W32(rope, 2);
    assert(!r.ConsumeItem(Pw + 8, 78) && rd32(rope) == 2); // another save
    assert(!r.ConsumeItem(0, 78) && rd32(rope) == 2);
    assert(r.ConsumeItem(Pw, 78) && rd32(rope) == 1);
    assert(r.ConsumeItem(Pw, 78) && rd32(rope) == 0);
    assert(!r.ConsumeItem(Pw, 78) && rd32(rope) == 0); // none left: never negative
    assert(!r.ConsumeItem(Pw, 200) && !r.ConsumeItem(Pw, -1));

    // HiddenItems: vanished, no dowsing level, non-finite and far (|x| >= 32768) ones are left out
    constexpr u64 EvK = 0x220000, EvS = 0x230000, Ev = 0x240000, EvList = 0x250000, EvArr = 0x260000;
    constexpr u64 Ent = 0x300000, Flags = 0x270000;
    mem.W64(Main + off::EvDataManagerTypeInfo, EvK);
    mem.W64(EvK + off::KlassStatics, EvS);
    mem.W64(EvS, Ev);
    mem.W64(Ev + off::EvFieldObjects, EvList);
    mem.W64(EvList + off::ListItems, EvArr);
    mem.W64(Pw + off::PwEventFlags, Flags);
    mem.W64(Flags + off::ArrayLength, 512);
    for (u64 i = 0; i < 512; ++i) mem.W8(Flags + off::ArrayData + i, 0);
    mem.W8(Flags + off::ArrayData + 100, 1);
    struct E { int dowsing, vanish; float x, z; };
    const std::vector<E> ents = {{1, -1, 10.2f, 5.0f},     {2, 100, 1, 1},      {0, -1, 1, 1},
                                 {1, -1, NAN, 1},          {1, -1, 40000, 1},   {1, -1, 1, -32768},
                                 {3, 101, -3.0f, -2.0f}};
    mem.W64(EvArr + off::ArrayLength, ents.size());
    mem.W32(EvList + off::ListSize, static_cast<u32>(ents.size()));
    for (u64 i = 0; i < ents.size(); ++i) {
        const u64 e = Ent + 0x1000 * i, prm = e + 0x800;
        mem.W64(EvArr + off::ArrayData + 8 * i, e);
        mem.W64(e + off::EntityParams, prm);
        mem.W32(prm + off::ParamDowsing, static_cast<u32>(ents[i].dowsing));
        mem.W32(prm + off::ParamVanish, static_cast<u32>(ents[i].vanish));
        u32 bits = 0;
        std::memcpy(&bits, &ents[i].x, 4);
        mem.W32(e + off::EntityWorldX, bits);
        std::memcpy(&bits, &ents[i].z, 4);
        mem.W32(e + off::EntityWorldZ, bits);
    }
    auto hidden = r.HiddenItems();
    assert(hidden.size() == 2);
    assert(hidden[0].grid_x == -9 && hidden[0].grid_y == 5 && hidden[0].dowsing == 1); // x -> 0.5 - x
    assert(hidden[1].grid_x == 3 && hidden[1].grid_y == -1 && hidden[1].dowsing == 3);
    mem.W32(EvList + off::ListSize, static_cast<u32>(ents.size() + 1)); // _size past the items array
    assert(r.HiddenItems().empty());
}

void TestMenuCursorResetRace() {
    using namespace lp_battle_drive;
    // Focus arriving before the show transition ends must not submit or set the cursor yet.
    assert(MenuCursor(1, 1, true, true, 0, 1, false) == CursorStep::Wait);
    assert(MenuCursor(2, 2, false, false, 0, 1, false) == CursorStep::Wait);
    assert(MenuCursor(5, 5, true, false, 0, 1, false) == CursorStep::Write);
    // Native setup reset the first store: write again, then settle before the one A.
    assert(MenuCursor(8, 3, true, false, 0, 1, false) == CursorStep::Write);
    assert(MenuCursor(10, 2, true, false, 1, 1, false) == CursorStep::Wait);
    assert(MenuCursor(12, 4, true, false, 1, 1, false) == CursorStep::Press);
    assert(MenuCursor(13, 5, true, false, 1, 1, true) == CursorStep::Wait);
    assert(MenuCursor(14, 6, false, false, 1, 1, true) == CursorStep::Done);
    // Lost focus without an A cannot be mistaken for a successful submitted move.
    assert(MenuCursor(14, 6, false, false, 1, 1, false) == CursorStep::Wait);
    // Repeated cursor resets cannot extend the original deadline, even with a recent write.
    assert(MenuCursor(151, 0, true, false, 0, 1, false) == CursorStep::Timeout);
    assert(MenuCursor(151, 5, false, false, 0, 1, false) == CursorStep::Timeout);
    // Bag / Pokémon / Run use the same policy. A command cannot report success
    // when focus disappeared before its A, and retries cannot submit the default Battle.
    assert(MenuCursor(5, 5, false, false, 0, 2, false) == CursorStep::Wait);
    assert(MenuCursor(6, 6, true, true, 0, 2, false) == CursorStep::Wait);
    assert(MenuCursor(7, 7, true, false, 0, 2, false) == CursorStep::Write);
    assert(MenuCursor(9, 2, true, false, 2, 2, false) == CursorStep::Wait);
    assert(MenuCursor(11, 4, true, false, 2, 2, false) == CursorStep::Press);
    assert(MenuCursor(12, 5, false, false, 2, 2, true) == CursorStep::Done);
}

int main() {
    TestMenuCursorResetRace();
    TestAnimation();
    TestDelayedFieldInputs();
    TestPk8RoundTrip();
    TestMonIdentity();
    TestEggDisplayPrivacy();
    TestMapGoalLayout();
    TestHpGaugeAndFoeStages();
    TestDexChains();
    TestDeltaAndBattle();
    TestAudioSingletonReadiness();
    TestMedicineTransactions();
    TestFieldControls();
    TestReaderWrites();
    TestBattleDetails();
    TestDoubleBattle();
    TestSwitchSuggestions();
    TestGuestMailbox();
    TestLanguageRead();
    TestLanguageDetect();
    TestLabelLookup();
    TestReflow();
    TestCjkMeasure();
    TestStringTable();
    TestTemplates();
    TestCjkCentring();
    TestVanillaBag();
    std::puts("lp tests passed");
    return 0;
}
