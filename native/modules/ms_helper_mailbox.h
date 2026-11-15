// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Monster Sanctuary guest helper mailbox (native/helper/ms_guest_helper.S): the module side.
// 4096 bytes, epoch word at 4092 (the load plan's mailbox_epoch_offset). The module writes a
// request (fields first, req_seq last, release); the helper answers it on the game's main thread
// at most one per frame (result fields first, result_seq last, release). Offsets here and the
// .equ table at the top of the .S file must match; the static_asserts pin this side.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>

namespace ms_helper_mailbox {

inline constexpr std::uint32_t MailboxSize = 4096;
inline constexpr std::uint32_t EpochOffset = 4092;
inline constexpr std::uint32_t MagicValue = 0x3248534D; // 'MSH2'

inline constexpr std::uint32_t OffMagic = 0x000, OffHeartbeat = 0x004, OffReqSeq = 0x008,
                               OffReqOp = 0x00C, OffReqEpoch = 0x010, OffPtrA = 0x018,
                               OffPtrB = 0x020, OffArgA = 0x030, OffArgB = 0x034, OffArgC = 0x038,
                               OffResSeq = 0x060, OffResCode = 0x064, OffResValue = 0x068,
                               OffHideOn = 0x080, OffHideLease = 0x084, OffHideState = 0x088, OffComboHideState = 0x0C0,
                               OffDiagGs = 0x0B0, OffDiagCs = 0x0B4, OffDiagReqs = 0x0B8,
                               /// guest: render-time hide passes (tk2dCamera.OnPreCull hook)
                               OffDiagPrecull = 0x0BC,
                               /// guest: render-time passes that hid a window the game had
                               /// re-scaled after Update / held windows found re-scaled at all
                               OffDiagSaves = 0x0A8, OffDiagRemarks = 0x0AC,
                               /// guest-only saved state of the rest of the battle-menu group
                               /// (SkillTooltip, MonsterInfo, item menu, ItemTooltip, switch list,
                               /// level popup)
                               OffGroupState = 0x100, GroupStride = 0x20,
                               /// guest: the title's "Press [A] to start" prompt GameObject while
                               /// the game shows it (GameObject.Find, active objects only), else 0
                               OffTitlePrompt = 0x0E0;

enum class Op : std::uint32_t {
    None = 0,
    MenuPick = 1,     ///< arg_a menu (0 command, 1 items, 2 switch list, 3 level popup), arg_b column, arg_c row, ptr_a item
    TargetPick = 2,   ///< ptr_a Monster in TargetSelector.targets
    CancelTarget = 3, ///< CombatController.CancelTargetSelection
    Teleport = 4,     ///< ptr_a TeleportSelectable in MapMenu.teleports (teleporter map open)
    TeamSwap = 5,     ///< ptr_a, ptr_b team monsters
    Equip = 6,        ///< ptr_a Monster, ptr_b InventoryItem, arg_a EquipmentSlot
    Follower = 7,     ///< ptr_a team monster
    MenuCancel = 8,   ///< arg_a menu (1 items, 2 switch list, 3 level popup): the native B (SelectionCancelled)
    InfoPick = 9,     ///< ptr_a Monster: the Monster Info cursor (TargetSelector.SelectTarget)
    /// arg_b column, arg_c row, ptr_a item of a skill in the command window: the native Y (the
    /// cursor moves there, then CombatController.TrySelectMonsterLevel opens its level popup)
    LevelOpen = 10,
};

enum class MenuKind : std::uint32_t { Command = 0, Items = 1, Switch = 2, Level = 3 };

enum class Result : std::uint32_t {
    Ok = 0,
    NotConfigured = 1,
    BadOp = 2,
    StaleEpoch = 3,
    NotReady = 4,       ///< game roots missing
    WrongState = 5,     ///< the game state does not allow it (combat / field / teleporter map)
    NotPlayerTurn = 6,
    MenuClosed = 7,     ///< the native menu / target selector is not taking input
    BadIndex = 8,
    Stale = 9,          ///< the menu row holds another item than the page showed
    Disabled = 10,      ///< the game's own disabled / can-equip rule says no
    NativeDeclined = 11,///< the native call ran and did not accept it
    NotMember = 12,     ///< not in the game's own list
    Same = 13,          ///< already that (teleport start, same follower, same monster)
    SlotInvalid = 14,
    Busy = 15,          ///< follower dashing / using its ability
};

inline std::string_view ResultText(Result r) {
    switch (r) {
    case Result::Ok: return "Done";
    case Result::NotConfigured: return "Helper not ready";
    case Result::BadOp: return "Not supported";
    case Result::StaleEpoch: return "Try again";
    case Result::NotReady: return "Game not ready";
    case Result::WrongState: return "Not possible right now";
    case Result::NotPlayerTurn: return "Not your turn";
    case Result::MenuClosed: return "Menu not open";
    case Result::BadIndex: return "Not in the menu";
    case Result::Stale: return "Menu changed, try again";
    case Result::Disabled: return "Cannot use that";
    case Result::NativeDeclined: return "The game said no";
    case Result::NotMember: return "Not available";
    case Result::Same: return "Already there";
    case Result::SlotInvalid: return "Wrong slot";
    case Result::Busy: return "Busy, try again";
    }
    return "Refused";
}

struct Request {
    Op op{Op::None};
    std::uint32_t epoch{};
    std::uint64_t ptr_a{}, ptr_b{};
    std::uint32_t arg_a{}, arg_b{}, arg_c{};
};

struct Outcome {
    bool answered{};
    Result code{Result::NotConfigured};
    std::uint64_t value{};
};

/// LoadU32/LoadU64: bool(uint32_t offset, T* out). StoreU32/StoreU64: bool(uint32_t offset, T).
template <class LoadU32, class LoadU64, class StoreU32, class StoreU64>
class Client {
public:
    Client(LoadU32 l32, LoadU64 l64, StoreU32 s32, StoreU64 s64)
        : load32_(std::move(l32)), load64_(std::move(l64)), store32_(std::move(s32)),
          store64_(std::move(s64)) {}

    /// Once after configure: seeds the sequence words, the epoch and (last) the magic. Keeps the
    /// sequence numbers the guest already answered so a re-configure does not replay anything.
    bool Initialize(std::uint32_t initial_epoch) {
        if (initial_epoch == 0)
            initial_epoch = 1;
        std::uint32_t seq{};
        load32_(OffResSeq, &seq);
        next_seq_ = seq;
        bool ok = store32_(OffReqSeq, seq);
        ok = store32_(OffHideOn, 0) && ok;
        ok = store32_(EpochOffset, initial_epoch) && ok;
        ok = store32_(OffMagic, MagicValue) && ok;
        return ok;
    }

    std::uint32_t Epoch() {
        std::uint32_t v{};
        return load32_(EpochOffset, &v) ? v : 0;
    }
    std::uint32_t Heartbeat() {
        std::uint32_t v{};
        return load32_(OffHeartbeat, &v) ? v : 0;
    }
    std::uint32_t Word(std::uint32_t off) {
        std::uint32_t v{};
        return load32_(off, &v) ? v : 0xFFFFFFFFu;
    }
    std::uint64_t Word64(std::uint32_t off) {
        std::uint64_t v{};
        return load64_(off, &v) ? v : 0;
    }

    /// Independent menu/combo hide bits plus a shared lease, renewed every sample.
    bool SetHide(bool menu, bool combo, std::uint32_t lease_frames) {
        const std::uint32_t hb = Heartbeat();
        bool ok = store32_(OffHideLease, hb + lease_frames);
        return store32_(OffHideOn, (menu ? 1u : 0u) | (combo ? 2u : 0u)) && ok;
    }

    bool Busy() const { return pending_; }

    bool TryStage(const Request& r) {
        if (pending_ || r.op == Op::None)
            return false;
        if (++next_seq_ == 0)
            next_seq_ = 1;
        bool ok = store64_(OffPtrA, r.ptr_a);
        ok = store64_(OffPtrB, r.ptr_b) && ok;
        ok = store32_(OffArgA, r.arg_a) && ok;
        ok = store32_(OffArgB, r.arg_b) && ok;
        ok = store32_(OffArgC, r.arg_c) && ok;
        ok = store32_(OffReqEpoch, r.epoch) && ok;
        ok = store32_(OffReqOp, static_cast<std::uint32_t>(r.op)) && ok;
        ok = store32_(OffReqSeq, next_seq_) && ok; // release: last
        if (!ok)
            return false;
        pending_ = true;
        return true;
    }

    Outcome Poll() {
        if (!pending_)
            return {};
        std::uint32_t seq{};
        if (!load32_(OffResSeq, &seq) || seq != next_seq_)
            return {};
        std::uint32_t code{};
        std::uint64_t value{};
        load32_(OffResCode, &code);
        load64_(OffResValue, &value);
        pending_ = false;
        return {true, static_cast<Result>(code), value};
    }

    /// Give up on an unanswered request (the guest stopped ticking: loading, paused).
    void Abandon() {
        if (!pending_)
            return;
        // the guest may answer in the instant between the caller's Poll() (which just found no
        // match) and this call: re-check first, and if it now matches, leave `pending_` set so
        // the next Poll() consumes the real answer instead of this call silently discarding it.
        std::uint32_t seq{};
        if (load32_(OffResSeq, &seq) && seq == next_seq_)
            return;
        pending_ = false;
        // re-sync with what the guest has answered; a late answer to the abandoned request is
        // then ignored because its seq no longer matches the next one staged
        store32_(OffReqSeq, seq);
        next_seq_ = seq;
    }

private:
    LoadU32 load32_;
    LoadU64 load64_;
    StoreU32 store32_;
    StoreU64 store64_;
    std::uint32_t next_seq_{};
    bool pending_{};
};

} // namespace ms_helper_mailbox
