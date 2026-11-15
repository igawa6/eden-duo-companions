// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Host-only test of the Monster Sanctuary guest-helper mailbox (ms_helper_mailbox.h), no emulator
// and no game: ms_helper_mailbox::Client against a fake 4096-byte mailbox, with a tiny model of
// the guest side written from native/helper/ms_guest_helper.S (answer = result fields first,
// result_seq last). Also checks that every mailbox offset, op number and result code the guest
// code declares (.equ MB_* / OP_MAX / R_*) is the one this header uses.

#include "ms_helper_mailbox.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

namespace {

namespace hm = ms_helper_mailbox;

class Fake {
public:
    bool fail_stores{}; ///< the host refuses writes (e.g. the mailbox went away)

    bool Load32(std::uint32_t o, std::uint32_t* v) const {
        if (o % 4 || o + 4 > hm::MailboxSize)
            return false;
        std::memcpy(v, bytes.data() + o, 4);
        return true;
    }
    bool Load64(std::uint32_t o, std::uint64_t* v) const {
        if (o % 8 || o + 8 > hm::MailboxSize)
            return false;
        std::memcpy(v, bytes.data() + o, 8);
        return true;
    }
    bool Store32(std::uint32_t o, std::uint32_t v) {
        if (fail_stores || o % 4 || o + 4 > hm::MailboxSize)
            return false;
        std::memcpy(bytes.data() + o, &v, 4);
        ++stores;
        return true;
    }
    bool Store64(std::uint32_t o, std::uint64_t v) {
        if (fail_stores || o % 8 || o + 8 > hm::MailboxSize)
            return false;
        std::memcpy(bytes.data() + o, &v, 8);
        ++stores;
        return true;
    }
    std::uint32_t U32(std::uint32_t o) const {
        std::uint32_t v{};
        assert(Load32(o, &v));
        return v;
    }
    std::uint64_t U64(std::uint32_t o) const {
        std::uint64_t v{};
        assert(Load64(o, &v));
        return v;
    }
    void Poke(std::uint32_t o, std::uint32_t v) {
        std::memcpy(bytes.data() + o, &v, 4);
    }

    /// One guest frame (ms_guest_helper.S): heartbeat, then a pending request (req_seq !=
    /// result_seq) is answered with `code` / `value`, result_seq written last.
    bool GuestFrame(hm::Result code, std::uint64_t value = 0) {
        Poke(hm::OffHeartbeat, U32(hm::OffHeartbeat) + 1);
        if (U32(hm::OffMagic) != hm::MagicValue || U32(hm::OffReqSeq) == U32(hm::OffResSeq))
            return false;
        Poke(hm::OffResCode, static_cast<std::uint32_t>(code));
        std::memcpy(bytes.data() + hm::OffResValue, &value, 8);
        Poke(hm::OffResSeq, U32(hm::OffReqSeq));
        return true;
    }

    int stores{};

private:
    std::array<std::uint8_t, hm::MailboxSize> bytes{};
};

auto MakeClient(Fake& f) {
    return hm::Client([&f](std::uint32_t o, std::uint32_t* v) { return f.Load32(o, v); },
                      [&f](std::uint32_t o, std::uint64_t* v) { return f.Load64(o, v); },
                      [&f](std::uint32_t o, std::uint32_t v) { return f.Store32(o, v); },
                      [&f](std::uint32_t o, std::uint64_t v) { return f.Store64(o, v); });
}

void TestInitialize() {
    Fake f;
    auto c = MakeClient(f);
    assert(c.Initialize(7));
    assert(f.U32(hm::OffMagic) == hm::MagicValue);
    assert(f.U32(hm::EpochOffset) == 7 && c.Epoch() == 7);
    assert(f.U32(hm::OffReqSeq) == 0 && f.U32(hm::OffHideOn) == 0);
    // epoch 0 means "never initialised" to the guest: a non-zero seed instead
    Fake z;
    auto cz = MakeClient(z);
    assert(cz.Initialize(0) && z.U32(hm::EpochOffset) == 1);
    // a re-configure keeps the sequence the guest already answered: nothing is replayed
    Fake r;
    r.Poke(hm::OffResSeq, 41);
    r.Poke(hm::OffReqSeq, 41);
    auto cr = MakeClient(r);
    assert(cr.Initialize(3));
    assert(r.U32(hm::OffReqSeq) == 41);
    assert(!r.GuestFrame(hm::Result::Ok)); // nothing pending after the re-configure
    assert(cr.TryStage({hm::Op::CancelTarget, 3}));
    assert(r.U32(hm::OffReqSeq) == 42);
    // a refused write reports failure
    Fake bad;
    bad.fail_stores = true;
    auto cb = MakeClient(bad);
    assert(!cb.Initialize(1));
}

void TestRoundTrip() {
    Fake f;
    auto c = MakeClient(f);
    assert(c.Initialize(5));
    assert(!c.Busy() && !c.Poll().answered);
    assert(!c.TryStage({})); // Op::None is never staged
    const hm::Request equip{hm::Op::Equip, 5, 0x1111, 0x2222, 3, 0, 0};
    assert(c.TryStage(equip));
    assert(c.Busy());
    assert(f.U32(hm::OffReqOp) == static_cast<std::uint32_t>(hm::Op::Equip));
    assert(f.U32(hm::OffReqEpoch) == 5);
    assert(f.U64(hm::OffPtrA) == 0x1111 && f.U64(hm::OffPtrB) == 0x2222);
    assert(f.U32(hm::OffArgA) == 3 && f.U32(hm::OffArgB) == 0 && f.U32(hm::OffArgC) == 0);
    assert(f.U32(hm::OffReqSeq) == 1);
    assert(!c.Poll().answered); // the guest has not ticked
    // one request at a time: a second stage is refused and writes nothing
    const int stores = f.stores;
    assert(!c.TryStage({hm::Op::Follower, 5, 0x3333}));
    assert(f.stores == stores && f.U64(hm::OffPtrA) == 0x1111);
    assert(f.GuestFrame(hm::Result::Ok, 0x55));
    const auto out = c.Poll();
    assert(out.answered && out.code == hm::Result::Ok && out.value == 0x55);
    assert(!c.Busy() && !c.Poll().answered); // consumed once
    // the next request gets the next sequence number and its own answer
    assert(c.TryStage({hm::Op::MenuPick, 5, 0x4444, 0, static_cast<std::uint32_t>(hm::MenuKind::Switch), 0, 2}));
    assert(f.U32(hm::OffReqSeq) == 2 && f.U32(hm::OffArgA) == 2 && f.U32(hm::OffArgC) == 2);
    assert(f.GuestFrame(hm::Result::NativeDeclined, 0x0301));
    const auto declined = c.Poll();
    assert(declined.answered && declined.code == hm::Result::NativeDeclined && declined.value == 0x0301);
    // the native B of the item window and the Monster Info cursor carry their own op numbers
    assert(c.TryStage({hm::Op::MenuCancel, 5, 0, 0, static_cast<std::uint32_t>(hm::MenuKind::Items)}));
    assert(f.U32(hm::OffReqOp) == 8 && f.U32(hm::OffArgA) == 1);
    assert(f.GuestFrame(hm::Result::Ok, 0x0301) && c.Poll().code == hm::Result::Ok);
    assert(c.TryStage({hm::Op::InfoPick, 5, 0x7777}));
    assert(f.U32(hm::OffReqOp) == 9 && f.U64(hm::OffPtrA) == 0x7777);
    assert(f.GuestFrame(hm::Result::Ok, 0x7777) && c.Poll().value == 0x7777);
    // owner round 3: the native Y on a skill (column / row / expected row item) and the level
    // popup as menu 3 of MENU_PICK / MENU_CANCEL
    assert(c.TryStage({hm::Op::LevelOpen, 5, 0x8888, 0, 0, 2, 1}));
    assert(f.U32(hm::OffReqOp) == 10 && f.U64(hm::OffPtrA) == 0x8888 && f.U32(hm::OffArgB) == 2 &&
           f.U32(hm::OffArgC) == 1);
    assert(f.GuestFrame(hm::Result::Ok, 1) && c.Poll().value == 1);
    assert(c.TryStage({hm::Op::MenuPick, 5, 0x9999, 0, static_cast<std::uint32_t>(hm::MenuKind::Level), 0, 3}));
    assert(f.U32(hm::OffReqOp) == 1 && f.U32(hm::OffArgA) == 3 && f.U32(hm::OffArgC) == 3);
    assert(f.GuestFrame(hm::Result::Ok, 0x0304) && c.Poll().code == hm::Result::Ok);
    assert(c.TryStage({hm::Op::MenuCancel, 5, 0, 0, static_cast<std::uint32_t>(hm::MenuKind::Level)}));
    assert(f.U32(hm::OffReqOp) == 8 && f.U32(hm::OffArgA) == 3);
    assert(f.GuestFrame(hm::Result::NativeDeclined) && c.Poll().code == hm::Result::NativeDeclined);
    // the title prompt word the guest publishes (0 = no native prompt shown)
    assert(c.Word64(hm::OffTitlePrompt) == 0);
    // a failed write leaves nothing pending
    f.fail_stores = true;
    assert(!c.TryStage({hm::Op::CancelTarget, 5}));
    assert(!c.Busy());
}

void TestAbandon() {
    Fake f;
    auto c = MakeClient(f);
    assert(c.Initialize(2));
    c.Abandon(); // nothing pending: no-op
    assert(!c.Busy() && f.U32(hm::OffReqSeq) == 0);
    // the guest stopped ticking (loading): give up, re-sync req_seq with what it answered
    assert(c.TryStage({hm::Op::Teleport, 2, 0x9000}));
    assert(f.U32(hm::OffReqSeq) == 1);
    c.Abandon();
    assert(!c.Busy());
    assert(f.U32(hm::OffReqSeq) == 0);     // the guest now sees nothing pending
    assert(!f.GuestFrame(hm::Result::Ok)); // and answers nothing
    // the next request reuses the number, so it is answered normally
    assert(c.TryStage({hm::Op::Follower, 2, 0xA000}));
    assert(f.U32(hm::OffReqSeq) == 1);
    assert(f.GuestFrame(hm::Result::Same));
    assert(c.Poll().code == hm::Result::Same);
    // the race fixed in a7b19ad: the answer lands between the caller's empty Poll and Abandon;
    // Abandon must keep it so the next Poll reports it instead of dropping it
    assert(c.TryStage({hm::Op::TeamSwap, 2, 0xB000, 0xC000}));
    assert(!c.Poll().answered);
    assert(f.GuestFrame(hm::Result::Ok, 7));
    c.Abandon();
    assert(c.Busy());
    const auto late = c.Poll();
    assert(late.answered && late.code == hm::Result::Ok && late.value == 7);
    assert(!c.Busy());
}

void TestHide() {
    Fake f;
    auto c = MakeClient(f);
    assert(c.Initialize(1));
    f.Poke(hm::OffHeartbeat, 1000);
    assert(c.SetHide(true, false, 90));
    assert(f.U32(hm::OffHideOn) == 1 && f.U32(hm::OffHideLease) == 1090);
    assert(c.SetHide(false, true, 90) && f.U32(hm::OffHideOn) == 2);
    assert(c.SetHide(true, true, 90) && f.U32(hm::OffHideOn) == 3);
    // Destroy / h_detach: both off, lease = now (the guest restores next frame)
    assert(c.SetHide(false, false, 0));
    assert(f.U32(hm::OffHideOn) == 0 && f.U32(hm::OffHideLease) == 1000);
    assert(c.Heartbeat() == 1000 && c.Word(hm::OffDiagGs) == 0);
    assert(c.Word(hm::MailboxSize) == 0xFFFFFFFFu); // out of range reads as all ones
}

void TestResultText() {
    for (std::uint32_t r = 0; r <= 15; ++r)
        assert(!hm::ResultText(static_cast<hm::Result>(r)).empty());
    assert(hm::ResultText(hm::Result::Ok) == "Done");
    assert(hm::ResultText(static_cast<hm::Result>(99)) == "Refused");
}

/// The .S file's ".equ NAME, value" table.
std::map<std::string, long> GuestEquates(const char* path) {
    std::ifstream in{path};
    assert(in && "ms_guest_helper.S not found");
    std::map<std::string, long> out;
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind(".equ ", 0) != 0)
            continue;
        std::istringstream s{line.substr(5)};
        std::string name, value;
        std::getline(s, name, ',');
        s >> value;
        out[name] = std::stol(value, nullptr, 0);
    }
    return out;
}

void TestGuestAgrees() {
    const auto eq = GuestEquates(MS_HELPER_SOURCE);
    const std::map<std::string, std::uint32_t> mine{
        {"MB_MAGIC", hm::OffMagic},       {"MB_HEARTBEAT", hm::OffHeartbeat}, {"MB_REQ_SEQ", hm::OffReqSeq},
        {"MB_REQ_OP", hm::OffReqOp},      {"MB_REQ_EPOCH", hm::OffReqEpoch},  {"MB_PTR_A", hm::OffPtrA},
        {"MB_PTR_B", hm::OffPtrB},        {"MB_ARG_A", hm::OffArgA},          {"MB_ARG_B", hm::OffArgB},
        {"MB_ARG_C", hm::OffArgC},        {"MB_RES_SEQ", hm::OffResSeq},      {"MB_RES_CODE", hm::OffResCode},
        {"MB_RES_VALUE", hm::OffResValue}, {"MB_HIDE_ON", hm::OffHideOn},     {"MB_HIDE_LEASE", hm::OffHideLease},
        {"MB_HIDE_STATE", hm::OffHideState}, {"MB_DIAG_GS", hm::OffDiagGs},   {"MB_DIAG_CS", hm::OffDiagCs},
        {"MB_DIAG_REQS", hm::OffDiagReqs}, {"MB_DIAG_PRECULL", hm::OffDiagPrecull},
        {"MB_DIAG_SAVES", hm::OffDiagSaves}, {"MB_DIAG_REMARKS", hm::OffDiagRemarks}, {"MB_COMBO_STATE", hm::OffComboHideState},
        {"MB_GROUP_STATE", hm::OffGroupState}, {"MB_GROUP_STRIDE", hm::GroupStride},
        {"MB_TITLE_PROMPT", hm::OffTitlePrompt},
        {"MB_EPOCH", hm::EpochOffset},
    };
    for (const auto& [name, off] : mine) {
        const auto it = eq.find(name);
        assert(it != eq.end() && it->second == static_cast<long>(off));
    }
    // every MB_* the guest uses is one the header declares (or the guest's own hide scratch)
    for (const auto& [name, value] : eq)
        if (name.rfind("MB_", 0) == 0)
            assert(mine.count(name) || name == "MB_HIDE_TR" || name == "MB_HIDE_SCALE");
    // the battle-menu group's six saved-state blocks (SkillTooltip, MonsterInfo, item menu,
    // ItemTooltip, switch list, level popup) lie past the combo block, inside the page
    assert(hm::OffGroupState >= hm::OffComboHideState + 0x20 &&
           hm::OffGroupState + 6 * hm::GroupStride <= hm::EpochOffset);
    // the guest's menu numbers are the header's MenuKind (MENU_PICK / MENU_CANCEL arg_a)
    assert(static_cast<int>(hm::MenuKind::Level) == 3 && eq.at("CUI_LEVELSELECT") == 0x78 &&
           eq.at("LSM_ISOPEN") == 0x38 && eq.at("LSM_MENULIST") == 0x28);
    // the ops the guest accepts are exactly the header's (1 .. OP_MAX)
    assert(eq.at("OP_MAX") == static_cast<long>(hm::Op::LevelOpen));
    // the title prompt word sits between the combo block and the group blocks
    assert(hm::OffTitlePrompt >= hm::OffComboHideState + 0x1C && hm::OffTitlePrompt + 8 <= hm::OffGroupState);
    // the two re-hide counters sit between the menu's hide block and the diagnostic words
    assert(hm::OffDiagSaves >= hm::OffHideState + 0x20 && hm::OffDiagRemarks + 4 <= hm::OffDiagGs);
    // the precull counter sits in the diagnostic words, before the combo block
    assert(hm::OffDiagPrecull == hm::OffDiagReqs + 4 && hm::OffDiagPrecull + 4 <= hm::OffComboHideState);
    // the hide mark is 0.125f (the module never writes scales; only the guest compares it)
    assert(eq.at("HIDE_MARK_Z_BITS") == 0x3E000000 && eq.at("HIDE_GRACE") > 0);
    // the result codes
    const std::map<std::string, hm::Result> codes{
        {"R_OK", hm::Result::Ok},                   {"R_NOT_CONFIGURED", hm::Result::NotConfigured},
        {"R_BAD_OP", hm::Result::BadOp},            {"R_STALE_EPOCH", hm::Result::StaleEpoch},
        {"R_NOT_READY", hm::Result::NotReady},      {"R_WRONG_STATE", hm::Result::WrongState},
        {"R_NOT_PLAYER_TURN", hm::Result::NotPlayerTurn}, {"R_MENU_CLOSED", hm::Result::MenuClosed},
        {"R_BAD_INDEX", hm::Result::BadIndex},      {"R_STALE", hm::Result::Stale},
        {"R_DISABLED", hm::Result::Disabled},       {"R_NATIVE_DECLINED", hm::Result::NativeDeclined},
        {"R_NOT_MEMBER", hm::Result::NotMember},    {"R_SAME", hm::Result::Same},
        {"R_SLOT_INVALID", hm::Result::SlotInvalid}, {"R_BUSY", hm::Result::Busy},
    };
    for (const auto& [name, code] : codes)
        assert(eq.at(name) == static_cast<long>(code));
    // the hide-state words (+0x88 / +0xC0) leave room for the transform and saved scale
    assert(hm::OffHideState + 0x18 <= hm::OffDiagGs && hm::OffComboHideState + 0x1C <= hm::EpochOffset);
}

} // namespace

int main() {
    TestInitialize();
    TestRoundTrip();
    TestAbandon();
    TestHide();
    TestResultText();
    TestGuestAgrees();
    std::puts("ms helper mailbox: ok");
    return 0;
}
